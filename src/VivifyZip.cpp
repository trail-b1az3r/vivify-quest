#include "VivifyZip.hpp"

#include <zlib.h>

#include <fstream>
#include <vector>

namespace Vivify {

namespace {

uint16_t Read16(std::span<uint8_t const> data, size_t at) {
  return static_cast<uint16_t>(data[at] | (data[at + 1] << 8));
}

uint32_t Read32(std::span<uint8_t const> data, size_t at) {
  return static_cast<uint32_t>(data[at]) | (static_cast<uint32_t>(data[at + 1]) << 8) |
         (static_cast<uint32_t>(data[at + 2]) << 16) | (static_cast<uint32_t>(data[at + 3]) << 24);
}

bool SafeEntryName(std::string const& name) {
  if (name.empty() || name[0] == '/' || name[0] == '\\') return false;
  if (name.find(':') != std::string::npos) return false;
  size_t start = 0;
  while (start <= name.size()) {
    size_t end = name.find_first_of("/\\", start);
    if (end == std::string::npos) end = name.size();
    if (name.compare(start, end - start, "..") == 0) return false;
    start = end + 1;
  }
  return true;
}

bool Inflate(std::span<uint8_t const> in, std::vector<uint8_t>& out) {
  z_stream stream{};
  // Negative window bits: raw deflate, as zip stores it (no zlib header).
  if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return false;
  stream.next_in = const_cast<Bytef*>(in.data());
  stream.avail_in = static_cast<uInt>(in.size());
  // zlib refuses a null output buffer, which an empty vector's data() can be.
  uint8_t empty = 0;
  stream.next_out = out.empty() ? &empty : out.data();
  stream.avail_out = static_cast<uInt>(out.size());
  int const result = inflate(&stream, Z_FINISH);
  bool const ok = result == Z_STREAM_END && stream.total_out == out.size();
  inflateEnd(&stream);
  return ok;
}

}

bool ExtractZip(std::span<uint8_t const> data, std::filesystem::path const& dest, std::string& error) {
  // End of central directory: 22 bytes plus a comment of up to 64 KiB.
  if (data.size() < 22) {
    error = "not a zip (too small)";
    return false;
  }
  size_t eocd = std::string::npos;
  size_t const lowest = data.size() > 22 + 0xFFFF ? data.size() - 22 - 0xFFFF : 0;
  for (size_t at = data.size() - 22 + 1; at-- > lowest;) {
    if (Read32(data, at) == 0x06054b50) {
      eocd = at;
      break;
    }
  }
  if (eocd == std::string::npos) {
    error = "not a zip (no end record)";
    return false;
  }
  uint16_t const entries = Read16(data, eocd + 10);
  uint32_t const directorySize = Read32(data, eocd + 12);
  uint32_t const directoryOffset = Read32(data, eocd + 16);
  if (directoryOffset == 0xFFFFFFFFu || static_cast<uint64_t>(directoryOffset) + directorySize > eocd) {
    error = "unsupported zip (zip64 or bad directory)";
    return false;
  }

  std::error_code ec;
  std::filesystem::create_directories(dest, ec);
  if (ec) {
    error = "cannot create '" + dest.string() + "': " + ec.message();
    return false;
  }

  size_t at = directoryOffset;
  for (uint16_t i = 0; i < entries; i++) {
    if (at + 46 > eocd || Read32(data, at) != 0x02014b50) {
      error = "bad central directory entry";
      return false;
    }
    uint16_t const flags = Read16(data, at + 8);
    uint16_t const method = Read16(data, at + 10);
    uint32_t const crc = Read32(data, at + 16);
    uint32_t const compressedSize = Read32(data, at + 20);
    uint32_t const size = Read32(data, at + 24);
    uint16_t const nameLength = Read16(data, at + 28);
    uint16_t const extraLength = Read16(data, at + 30);
    uint16_t const commentLength = Read16(data, at + 32);
    uint32_t const localOffset = Read32(data, at + 42);
    if (at + 46 + nameLength > eocd) {
      error = "bad entry name";
      return false;
    }
    std::string const name(reinterpret_cast<char const*>(data.data() + at + 46), nameLength);
    at += 46 + nameLength + extraLength + commentLength;

    if (!SafeEntryName(name)) {
      error = "unsafe entry name '" + name + "'";
      return false;
    }
    std::filesystem::path const target = dest / std::filesystem::path(name).relative_path();
    if (name.back() == '/' || name.back() == '\\') {
      std::filesystem::create_directories(target, ec);
      if (ec) {
        error = "cannot create '" + target.string() + "': " + ec.message();
        return false;
      }
      continue;
    }
    if (flags & 1) {
      error = "encrypted entry '" + name + "'";
      return false;
    }
    if (compressedSize == 0xFFFFFFFFu || size == 0xFFFFFFFFu || localOffset == 0xFFFFFFFFu) {
      error = "unsupported zip64 entry '" + name + "'";
      return false;
    }
    if (static_cast<uint64_t>(localOffset) + 30 > data.size() || Read32(data, localOffset) != 0x04034b50) {
      error = "bad local header for '" + name + "'";
      return false;
    }
    // Sizes come from the central directory: a local header written with
    // flag bit 3 carries zeros and puts the real sizes after the data.
    uint64_t const payload = static_cast<uint64_t>(localOffset) + 30 + Read16(data, localOffset + 26) +
                             Read16(data, localOffset + 28);
    if (payload + compressedSize > data.size()) {
      error = "truncated entry '" + name + "'";
      return false;
    }
    std::span<uint8_t const> const in = data.subspan(static_cast<size_t>(payload), compressedSize);

    std::vector<uint8_t> out;
    if (method == 0) {
      if (compressedSize != size) {
        error = "bad stored entry '" + name + "'";
        return false;
      }
      out.assign(in.begin(), in.end());
    } else if (method == 8) {
      // Deflate expands at most ~1032:1; a larger claimed size is a corrupt
      // header, and would otherwise be allocated up front.
      if (size > static_cast<uint64_t>(compressedSize) * 1032 + 1024) {
        error = "bad size for '" + name + "'";
        return false;
      }
      out.resize(size);
      if (!Inflate(in, out)) {
        error = "corrupt deflate data in '" + name + "'";
        return false;
      }
    } else {
      error = "unsupported compression " + std::to_string(method) + " in '" + name + "'";
      return false;
    }
    if (crc32(0L, out.data(), static_cast<uInt>(out.size())) != crc) {
      error = "checksum mismatch in '" + name + "'";
      return false;
    }

    std::filesystem::create_directories(target.parent_path(), ec);
    std::ofstream os(target, std::ios::binary | std::ios::trunc);
    if (!os.is_open()) {
      error = "cannot write '" + target.string() + "'";
      return false;
    }
    os.write(reinterpret_cast<char const*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!os.good()) {
      error = "write failed for '" + target.string() + "'";
      return false;
    }
  }
  return true;
}

}
