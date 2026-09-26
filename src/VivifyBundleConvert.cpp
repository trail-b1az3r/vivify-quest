#include "VivifyBundleConvert.hpp"
#include "VivifySerializedFile.hpp"
#include "VivifyDxbc.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Vivify::BundleConvert {

namespace {

// ---------------------------------------------------------------------------
// Small endian-aware readers over an in-memory buffer.
// ---------------------------------------------------------------------------

class ByteReader {
 public:
  ByteReader(uint8_t const* data, size_t size) : _data(data), _size(size) {}

  bool ok() const { return _ok; }
  size_t position() const { return _pos; }
  size_t remaining() const { return _pos <= _size ? _size - _pos : 0; }

  void seek(size_t pos) {
    if (pos > _size) {
      _ok = false;
      return;
    }
    _pos = pos;
  }

  void align(size_t alignment) {
    if (alignment == 0) return;
    size_t const rem = _pos % alignment;
    if (rem != 0) skip(alignment - rem);
  }

  void skip(size_t count) {
    if (count > remaining()) {
      _ok = false;
      _pos = _size;
      return;
    }
    _pos += count;
  }

  uint8_t u8() {
    if (remaining() < 1) {
      _ok = false;
      return 0;
    }
    return _data[_pos++];
  }

  uint16_t u16be() {
    uint8_t b[2];
    if (!raw(b, 2)) return 0;
    return static_cast<uint16_t>((static_cast<uint16_t>(b[0]) << 8) | b[1]);
  }

  uint32_t u32be() {
    uint8_t b[4];
    if (!raw(b, 4)) return 0;
    return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
           (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
  }

  uint64_t u64be() {
    uint8_t b[8];
    if (!raw(b, 8)) return 0;
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) value = (value << 8) | b[i];
    return value;
  }

  // Reads a NUL-terminated string. A missing terminator is a parse failure
  // rather than a silent truncation.
  std::string cstring(size_t maxLength = 4096) {
    std::string out;
    while (out.size() <= maxLength) {
      if (remaining() < 1) {
        _ok = false;
        return out;
      }
      uint8_t const c = _data[_pos++];
      if (c == 0) return out;
      out.push_back(static_cast<char>(c));
    }
    _ok = false;
    return out;
  }

  bool raw(void* dest, size_t count) {
    if (count > remaining()) {
      _ok = false;
      _pos = _size;
      return false;
    }
    std::memcpy(dest, _data + _pos, count);
    _pos += count;
    return true;
  }

 private:
  uint8_t const* _data;
  size_t _size;
  size_t _pos = 0;
  bool _ok = true;
};

uint32_t ReadU32(uint8_t const* p, bool bigEndian) {
  if (bigEndian) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
  }
  return (static_cast<uint32_t>(p[3]) << 24) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[1]) << 8) | static_cast<uint32_t>(p[0]);
}

void WriteU32(uint8_t* p, uint32_t value, bool bigEndian) {
  if (bigEndian) {
    p[0] = static_cast<uint8_t>(value >> 24);
    p[1] = static_cast<uint8_t>(value >> 16);
    p[2] = static_cast<uint8_t>(value >> 8);
    p[3] = static_cast<uint8_t>(value);
  } else {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
    p[2] = static_cast<uint8_t>(value >> 16);
    p[3] = static_cast<uint8_t>(value >> 24);
  }
}

void AppendU16BE(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}

void AppendU32BE(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value >> 24));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}

void AppendU64BE(std::vector<uint8_t>& out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>(value >> shift));
  }
}

void AppendCString(std::vector<uint8_t>& out, std::string const& value) {
  out.insert(out.end(), value.begin(), value.end());
  out.push_back(0);
}

// ---------------------------------------------------------------------------
// LZ4 block decompression (covers both LZ4 and LZ4HC -- same block format).
// ---------------------------------------------------------------------------

bool Lz4DecompressBlock(uint8_t const* src, size_t srcSize, uint8_t* dst, size_t dstSize) {
  size_t sp = 0;
  size_t dp = 0;
  while (sp < srcSize) {
    uint8_t const token = src[sp++];

    size_t literalLength = token >> 4;
    if (literalLength == 15) {
      for (;;) {
        if (sp >= srcSize) return false;
        uint8_t const add = src[sp++];
        literalLength += add;
        if (add != 255) break;
        if (literalLength > dstSize) return false;
      }
    }
    if (literalLength > srcSize - sp) return false;
    if (literalLength > dstSize - dp) return false;
    std::memcpy(dst + dp, src + sp, literalLength);
    sp += literalLength;
    dp += literalLength;

    // The last sequence in a block is literals only and stops here.
    if (sp == srcSize) break;
    if (srcSize - sp < 2) return false;

    size_t const offset = static_cast<size_t>(src[sp]) | (static_cast<size_t>(src[sp + 1]) << 8);
    sp += 2;
    if (offset == 0 || offset > dp) return false;

    size_t matchLength = token & 0x0F;
    if (matchLength == 15) {
      for (;;) {
        if (sp >= srcSize) return false;
        uint8_t const add = src[sp++];
        matchLength += add;
        if (add != 255) break;
        if (matchLength > dstSize) return false;
      }
    }
    matchLength += 4;
    if (matchLength > dstSize - dp) return false;

    // Matches may overlap the bytes they produce, so this must stay a byte
    // copy rather than a memcpy/memmove.
    size_t matchPos = dp - offset;
    for (size_t i = 0; i < matchLength; i++) {
      dst[dp++] = dst[matchPos++];
    }
  }
  return dp == dstSize;
}

// ---------------------------------------------------------------------------
// LZMA1 decompression, following the reference LZMA specification decoder.
// Unity stores 5 property bytes followed by the raw stream, with the
// uncompressed size known from the archive tables rather than from the stream.
// ---------------------------------------------------------------------------

class LzmaDecoder {
 public:
  // props: the 5-byte LZMA property header (lclppb byte + 4-byte dictionary size).
  bool Decode(uint8_t const* props, uint8_t const* src, size_t srcSize, uint8_t* dst, size_t dstSize) {
    uint32_t d = props[0];
    if (d >= 9 * 5 * 5) return false;
    _lc = static_cast<unsigned>(d % 9);
    d /= 9;
    _lp = static_cast<unsigned>(d % 5);
    _pb = static_cast<unsigned>(d / 5);

    _src = src;
    _srcSize = srcSize;
    _srcPos = 0;
    _dst = dst;
    _dstSize = dstSize;
    _dstPos = 0;

    if (!RangeInit()) return false;

    _litProbs.assign(static_cast<size_t>(0x300) << (_lc + _lp), kProbInit);
    _isMatch.assign(kNumStates << kNumPosBitsMax, kProbInit);
    _isRep.assign(kNumStates, kProbInit);
    _isRepG0.assign(kNumStates, kProbInit);
    _isRepG1.assign(kNumStates, kProbInit);
    _isRepG2.assign(kNumStates, kProbInit);
    _isRep0Long.assign(kNumStates << kNumPosBitsMax, kProbInit);
    _posSlot.assign(static_cast<size_t>(kNumLenToPosStates) * (1u << 6), kProbInit);
    _specPos.assign(kNumFullDistances, kProbInit);
    _align.assign(1u << kNumAlignBits, kProbInit);
    _lenDecoder.Reset();
    _repLenDecoder.Reset();

    unsigned state = 0;
    uint32_t rep0 = 0;
    uint32_t rep1 = 0;
    uint32_t rep2 = 0;
    uint32_t rep3 = 0;

    while (_dstPos < _dstSize) {
      unsigned const posState = static_cast<unsigned>(_dstPos) & ((1u << _pb) - 1);
      if (DecodeBit(_isMatch[(state << kNumPosBitsMax) + posState]) == 0) {
        if (!DecodeLiteral(state, rep0)) return false;
        state = state < 4 ? 0 : (state < 10 ? state - 3 : state - 6);
        continue;
      }

      uint32_t len;
      if (DecodeBit(_isRep[state]) != 0) {
        if (_dstPos == 0) return false;
        if (static_cast<uint64_t>(rep0) >= _dstPos) return false;
        if (DecodeBit(_isRepG0[state]) == 0) {
          if (DecodeBit(_isRep0Long[(state << kNumPosBitsMax) + posState]) == 0) {
            state = state < 7 ? 9 : 11;
            _dst[_dstPos] = _dst[_dstPos - rep0 - 1];
            _dstPos++;
            continue;
          }
        } else {
          uint32_t dist;
          if (DecodeBit(_isRepG1[state]) == 0) {
            dist = rep1;
          } else {
            if (DecodeBit(_isRepG2[state]) == 0) {
              dist = rep2;
            } else {
              dist = rep3;
              rep3 = rep2;
            }
            rep2 = rep1;
          }
          rep1 = rep0;
          rep0 = dist;
        }
        if (static_cast<uint64_t>(rep0) >= _dstPos) return false;
        len = _repLenDecoder.Decode(*this, posState);
        state = state < 7 ? 8 : 11;
      } else {
        rep3 = rep2;
        rep2 = rep1;
        rep1 = rep0;
        len = _lenDecoder.Decode(*this, posState);
        state = state < 7 ? 7 : 10;
        rep0 = DecodeDistance(len);
        if (rep0 == 0xFFFFFFFFu) {
          // End-of-stream marker. Only valid if we already produced everything.
          return _dstPos == _dstSize;
        }
        if (static_cast<uint64_t>(rep0) >= _dstPos) return false;
      }

      len += kMatchMinLen;
      if (len > _dstSize - _dstPos) return false;
      size_t matchPos = _dstPos - rep0 - 1;
      for (uint32_t i = 0; i < len; i++) {
        _dst[_dstPos++] = _dst[matchPos++];
      }
      if (_corrupted) return false;
    }
    return !_corrupted && _dstPos == _dstSize;
  }

 private:
  static constexpr unsigned kNumBitModelTotalBits = 11;
  static constexpr unsigned kNumMoveBits = 5;
  static constexpr uint16_t kProbInit = (1u << kNumBitModelTotalBits) / 2;
  static constexpr unsigned kNumPosBitsMax = 4;
  static constexpr unsigned kNumStates = 12;
  static constexpr unsigned kNumLenToPosStates = 4;
  static constexpr unsigned kNumAlignBits = 4;
  static constexpr unsigned kEndPosModelIndex = 14;
  static constexpr unsigned kNumFullDistances = 1u << (kEndPosModelIndex >> 1);
  static constexpr uint32_t kMatchMinLen = 2;
  static constexpr uint32_t kTopValue = 1u << 24;

  uint8_t NextByte() {
    if (_srcPos >= _srcSize) {
      _corrupted = true;
      return 0;
    }
    return _src[_srcPos++];
  }

  bool RangeInit() {
    if (_srcSize < 5) return false;
    if (NextByte() != 0) return false;
    _code = 0;
    _range = 0xFFFFFFFFu;
    for (int i = 0; i < 4; i++) _code = (_code << 8) | NextByte();
    return _code != _range;
  }

  void Normalize() {
    if (_range < kTopValue) {
      _range <<= 8;
      _code = (_code << 8) | NextByte();
    }
  }

  unsigned DecodeBit(uint16_t& prob) {
    uint32_t const bound = (_range >> kNumBitModelTotalBits) * prob;
    unsigned symbol;
    if (_code < bound) {
      prob = static_cast<uint16_t>(prob + (((1u << kNumBitModelTotalBits) - prob) >> kNumMoveBits));
      _range = bound;
      symbol = 0;
    } else {
      prob = static_cast<uint16_t>(prob - (prob >> kNumMoveBits));
      _code -= bound;
      _range -= bound;
      symbol = 1;
    }
    Normalize();
    return symbol;
  }

  uint32_t DecodeDirectBits(unsigned count) {
    uint32_t result = 0;
    do {
      _range >>= 1;
      _code -= _range;
      uint32_t const t = 0u - (_code >> 31);
      _code += _range & t;
      if (_code == _range) _corrupted = true;
      Normalize();
      result <<= 1;
      result += t + 1;
    } while (--count);
    return result;
  }

  uint32_t BitTreeDecode(uint16_t* probs, unsigned numBits) {
    uint32_t m = 1;
    for (unsigned i = 0; i < numBits; i++) m = (m << 1) + DecodeBit(probs[m]);
    return m - (1u << numBits);
  }

  uint32_t BitTreeReverseDecode(uint16_t* probs, unsigned numBits) {
    uint32_t m = 1;
    uint32_t symbol = 0;
    for (unsigned i = 0; i < numBits; i++) {
      unsigned const bit = DecodeBit(probs[m]);
      m = (m << 1) + bit;
      symbol |= static_cast<uint32_t>(bit) << i;
    }
    return symbol;
  }

  struct LenDecoder {
    uint16_t choice = kProbInit;
    uint16_t choice2 = kProbInit;
    uint16_t low[1u << kNumPosBitsMax][8];
    uint16_t mid[1u << kNumPosBitsMax][8];
    uint16_t high[256];

    void Reset() {
      choice = kProbInit;
      choice2 = kProbInit;
      for (auto& row : low) std::fill(std::begin(row), std::end(row), kProbInit);
      for (auto& row : mid) std::fill(std::begin(row), std::end(row), kProbInit);
      std::fill(std::begin(high), std::end(high), kProbInit);
    }

    uint32_t Decode(LzmaDecoder& rc, unsigned posState) {
      if (rc.DecodeBit(choice) == 0) return rc.BitTreeDecode(low[posState], 3);
      if (rc.DecodeBit(choice2) == 0) return 8 + rc.BitTreeDecode(mid[posState], 3);
      return 16 + rc.BitTreeDecode(high, 8);
    }
  };

  bool DecodeLiteral(unsigned state, uint32_t rep0) {
    if (_dstPos >= _dstSize) return false;
    unsigned prevByte = _dstPos > 0 ? _dst[_dstPos - 1] : 0;
    size_t const litState =
        ((_dstPos & ((1u << _lp) - 1)) << _lc) + (prevByte >> (8 - _lc));
    uint16_t* probs = _litProbs.data() + (static_cast<size_t>(0x300) * litState);

    unsigned symbol = 1;
    if (state >= 7) {
      if (static_cast<uint64_t>(rep0) + 1 > _dstPos) return false;
      unsigned matchByte = _dst[_dstPos - rep0 - 1];
      do {
        unsigned const matchBit = (matchByte >> 7) & 1;
        matchByte = static_cast<unsigned>(matchByte << 1) & 0xFF;
        unsigned const bit = DecodeBit(probs[((1 + matchBit) << 8) + symbol]);
        symbol = (symbol << 1) | bit;
        if (matchBit != bit) break;
      } while (symbol < 0x100);
    }
    while (symbol < 0x100) symbol = (symbol << 1) | DecodeBit(probs[symbol]);
    _dst[_dstPos++] = static_cast<uint8_t>(symbol);
    return !_corrupted;
  }

  uint32_t DecodeDistance(uint32_t len) {
    unsigned lenState = len < kNumLenToPosStates ? static_cast<unsigned>(len) : kNumLenToPosStates - 1;
    uint32_t const posSlot = BitTreeDecode(_posSlot.data() + (static_cast<size_t>(lenState) << 6), 6);
    if (posSlot < 4) return posSlot;

    unsigned const numDirectBits = static_cast<unsigned>((posSlot >> 1) - 1);
    uint32_t dist = (2 | (posSlot & 1)) << numDirectBits;
    if (posSlot < kEndPosModelIndex) {
      dist += BitTreeReverseDecode(_specPos.data() + dist - posSlot, numDirectBits);
    } else {
      dist += DecodeDirectBits(numDirectBits - kNumAlignBits) << kNumAlignBits;
      dist += BitTreeReverseDecode(_align.data(), kNumAlignBits);
    }
    return dist;
  }

  uint8_t const* _src = nullptr;
  size_t _srcSize = 0;
  size_t _srcPos = 0;
  uint8_t* _dst = nullptr;
  size_t _dstSize = 0;
  size_t _dstPos = 0;

  uint32_t _range = 0;
  uint32_t _code = 0;
  bool _corrupted = false;

  unsigned _lc = 0;
  unsigned _lp = 0;
  unsigned _pb = 0;

  std::vector<uint16_t> _litProbs;
  std::vector<uint16_t> _isMatch;
  std::vector<uint16_t> _isRep;
  std::vector<uint16_t> _isRepG0;
  std::vector<uint16_t> _isRepG1;
  std::vector<uint16_t> _isRepG2;
  std::vector<uint16_t> _isRep0Long;
  std::vector<uint16_t> _posSlot;
  std::vector<uint16_t> _specPos;
  std::vector<uint16_t> _align;
  LenDecoder _lenDecoder;
  LenDecoder _repLenDecoder;
};

// ---------------------------------------------------------------------------
// UnityFS archive structures.
// ---------------------------------------------------------------------------

// Archive-level flag bits (UnityFS header "flags" field).
constexpr uint32_t kArchiveCompressionMask = 0x3F;
constexpr uint32_t kArchiveBlocksAndDirectoryInfoCombined = 0x40;
constexpr uint32_t kArchiveBlocksInfoAtTheEnd = 0x80;
constexpr uint32_t kArchiveBlockInfoNeedPaddingAtStart = 0x200;

// Compression ids shared by the archive header and per-block flags.
constexpr uint32_t kCompressionNone = 0;
constexpr uint32_t kCompressionLzma = 1;
constexpr uint32_t kCompressionLz4 = 2;
constexpr uint32_t kCompressionLz4Hc = 3;
constexpr uint32_t kCompressionLzham = 4;

// Blocks we emit when repacking. Matches Unity's own chunk size.
constexpr uint32_t kOutputBlockSize = 128 * 1024;

// A bundle bigger than this is refused rather than risking an allocation that
// takes the game down. Vivify bundles are a few hundred MB at the very most.
constexpr uint64_t kMaxUncompressedBytes = 2ull * 1024 * 1024 * 1024;

struct StorageBlock {
  uint32_t uncompressedSize = 0;
  uint32_t compressedSize = 0;
  uint16_t flags = 0;
};

struct DirectoryNode {
  uint64_t offset = 0;
  uint64_t size = 0;
  uint32_t flags = 0;
  std::string path;
};

struct ArchiveHeader {
  std::string signature;
  uint32_t version = 0;
  std::string unityVersion;
  std::string unityRevision;
  uint64_t size = 0;
  uint32_t compressedBlocksInfoSize = 0;
  uint32_t uncompressedBlocksInfoSize = 0;
  uint32_t flags = 0;
  // Offset of the first storage block within the file.
  uint64_t blocksStart = 0;
};

bool Decompress(uint32_t compression, uint8_t const* src, size_t srcSize, uint8_t* dst, size_t dstSize,
                Status& outStatus) {
  switch (compression) {
    case kCompressionNone:
      if (srcSize != dstSize) {
        outStatus = Status::Corrupt;
        return false;
      }
      std::memcpy(dst, src, dstSize);
      return true;
    case kCompressionLz4:
    case kCompressionLz4Hc:
      if (!Lz4DecompressBlock(src, srcSize, dst, dstSize)) {
        outStatus = Status::Corrupt;
        return false;
      }
      return true;
    case kCompressionLzma: {
      if (srcSize < 5) {
        outStatus = Status::Corrupt;
        return false;
      }
      LzmaDecoder decoder;
      if (!decoder.Decode(src, src + 5, srcSize - 5, dst, dstSize)) {
        outStatus = Status::Corrupt;
        return false;
      }
      return true;
    }
    case kCompressionLzham:
    default:
      // LZHAM is legal in the format but Unity has not produced it for years,
      // and shipping a decoder for it would be dead weight.
      outStatus = Status::UnsupportedCompression;
      return false;
  }
}

// Seeking reader over the source bundle. Blocks are pulled in one at a time so
// the compressed archive is never resident alongside the expanded one -- on a
// large bundle that halves peak memory, which matters when this runs on device
// next to the game.
class FileSource {
 public:
  bool Open(std::string const& path, Result& result) {
    std::error_code ec;
    auto const fileSize = std::filesystem::file_size(path, ec);
    if (ec) {
      result.status = Status::SourceUnreadable;
      result.message = "could not stat '" + path + "'";
      return false;
    }
    _stream.open(path, std::ios::binary);
    if (!_stream.is_open()) {
      result.status = Status::SourceUnreadable;
      result.message = "could not open '" + path + "'";
      return false;
    }
    _size = static_cast<uint64_t>(fileSize);
    return true;
  }

  uint64_t size() const { return _size; }

  bool ReadAt(uint64_t offset, size_t count, std::vector<uint8_t>& out) {
    if (offset > _size || _size - offset < count) return false;
    out.resize(count);
    if (count == 0) return true;
    _stream.clear();
    _stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!_stream) return false;
    return static_cast<bool>(_stream.read(reinterpret_cast<char*>(out.data()),
                                          static_cast<std::streamsize>(count)));
  }

 private:
  std::ifstream _stream;
  uint64_t _size = 0;
};

// Largest fixed archive header we will ever need to inspect: signature, version,
// two version strings, three sizes and the flags, plus the version-7 alignment.
constexpr size_t kMaxHeaderScan = 512;

// Parses the archive header and the (possibly compressed) blocks/directory
// table. Leaves header.blocksStart pointing at the first storage block.
bool ParseArchive(FileSource& source, ArchiveHeader& header, std::vector<StorageBlock>& blocks,
                  std::vector<DirectoryNode>& nodes, Result& result) {
  std::vector<uint8_t> head;
  if (!source.ReadAt(0, static_cast<size_t>(std::min<uint64_t>(source.size(), kMaxHeaderScan)), head)) {
    result.status = Status::SourceUnreadable;
    result.message = "could not read the archive header";
    return false;
  }
  ByteReader reader(head.data(), head.size());
  header.signature = reader.cstring(32);
  if (!reader.ok()) {
    result.status = Status::NotAUnityBundle;
    result.message = "file does not start with a Unity archive signature";
    return false;
  }
  if (header.signature != "UnityFS") {
    // The signature came from an arbitrary file, so scrub it before it reaches
    // a log line.
    std::string printable;
    for (char c : header.signature) {
      printable.push_back(c >= 0x20 && c < 0x7F ? c : '?');
    }
    result.status = Status::NotAUnityBundle;
    result.message = "unsupported archive signature '" + printable + "' (only UnityFS is handled)";
    return false;
  }
  header.version = reader.u32be();
  header.unityVersion = reader.cstring(64);
  header.unityRevision = reader.cstring(64);
  header.size = reader.u64be();
  header.compressedBlocksInfoSize = reader.u32be();
  header.uncompressedBlocksInfoSize = reader.u32be();
  header.flags = reader.u32be();
  if (!reader.ok()) {
    result.status = Status::Corrupt;
    result.message = "truncated UnityFS header";
    return false;
  }
  if (header.version >= 7) reader.align(16);

  if (header.uncompressedBlocksInfoSize == 0 || header.uncompressedBlocksInfoSize > kMaxUncompressedBytes) {
    result.status = Status::Corrupt;
    result.message = "implausible blocks-info size";
    return false;
  }

  // Position just past the fixed header (and its version-7 padding) -- where
  // the blocks-info sits in the common layout, and where the storage blocks
  // start when the table is stored at the end instead.
  uint64_t const afterHeader = reader.position();

  uint64_t blocksInfoOffset;
  if ((header.flags & kArchiveBlocksInfoAtTheEnd) != 0) {
    if (source.size() < header.compressedBlocksInfoSize) {
      result.status = Status::Corrupt;
      result.message = "blocks-info claims to sit past the end of the file";
      return false;
    }
    blocksInfoOffset = source.size() - header.compressedBlocksInfoSize;
    header.blocksStart = afterHeader;
  } else {
    blocksInfoOffset = afterHeader;
    header.blocksStart = afterHeader + header.compressedBlocksInfoSize;
  }
  if ((header.flags & kArchiveBlockInfoNeedPaddingAtStart) != 0) {
    header.blocksStart += (16 - (header.blocksStart % 16)) % 16;
  }

  std::vector<uint8_t> blocksInfoRaw;
  if (!source.ReadAt(blocksInfoOffset, header.compressedBlocksInfoSize, blocksInfoRaw)) {
    result.status = Status::Corrupt;
    result.message = "blocks-info is out of bounds";
    return false;
  }

  std::vector<uint8_t> blocksInfo(header.uncompressedBlocksInfoSize);
  Status failure = Status::Corrupt;
  if (!Decompress(header.flags & kArchiveCompressionMask, blocksInfoRaw.data(), blocksInfoRaw.size(),
                  blocksInfo.data(), blocksInfo.size(), failure)) {
    result.status = failure;
    result.message = failure == Status::UnsupportedCompression
                         ? "blocks-info uses compression id " +
                               std::to_string(header.flags & kArchiveCompressionMask) + ", which is not supported"
                         : "blocks-info failed to decompress";
    return false;
  }

  ByteReader info(blocksInfo.data(), blocksInfo.size());
  info.skip(16);  // uncompressed data hash
  uint32_t const blockCount = info.u32be();
  if (!info.ok() || blockCount > (1u << 22)) {
    result.status = Status::Corrupt;
    result.message = "implausible storage block count";
    return false;
  }
  blocks.resize(blockCount);
  for (uint32_t i = 0; i < blockCount; i++) {
    blocks[i].uncompressedSize = info.u32be();
    blocks[i].compressedSize = info.u32be();
    blocks[i].flags = info.u16be();
  }
  uint32_t const nodeCount = info.u32be();
  if (!info.ok() || nodeCount > (1u << 20)) {
    result.status = Status::Corrupt;
    result.message = "implausible directory node count";
    return false;
  }
  nodes.resize(nodeCount);
  for (uint32_t i = 0; i < nodeCount; i++) {
    nodes[i].offset = info.u64be();
    nodes[i].size = info.u64be();
    nodes[i].flags = info.u32be();
    nodes[i].path = info.cstring();
  }
  if (!info.ok()) {
    result.status = Status::Corrupt;
    result.message = "truncated blocks/directory table";
    return false;
  }
  return true;
}

bool ReadBlocks(FileSource& source, ArchiveHeader const& header,
                std::vector<StorageBlock> const& blocks, std::vector<uint8_t>& out, Result& result) {
  uint64_t total = 0;
  for (auto const& block : blocks) total += block.uncompressedSize;
  if (total == 0 || total > kMaxUncompressedBytes) {
    result.status = total == 0 ? Status::Corrupt : Status::OutOfMemory;
    result.message = total == 0 ? "archive contains no data" : "uncompressed archive exceeds the 2 GiB limit";
    return false;
  }
  out.resize(static_cast<size_t>(total));

  uint64_t srcOffset = header.blocksStart;
  size_t dstOffset = 0;
  std::vector<uint8_t> compressed;
  for (auto const& block : blocks) {
    if (!source.ReadAt(srcOffset, block.compressedSize, compressed)) {
      result.status = Status::Corrupt;
      result.message = "storage block runs past the end of the file";
      return false;
    }
    Status failure = Status::Corrupt;
    if (!Decompress(block.flags & kArchiveCompressionMask, compressed.data(), compressed.size(),
                    out.data() + dstOffset, block.uncompressedSize, failure)) {
      result.status = failure;
      result.message = failure == Status::UnsupportedCompression
                           ? "a storage block uses compression id " +
                                 std::to_string(block.flags & kArchiveCompressionMask) + ", which is not supported"
                           : "a storage block failed to decompress";
      return false;
    }
    srcOffset += block.compressedSize;
    dstOffset += block.uncompressedSize;
  }
  return true;
}

// ---------------------------------------------------------------------------
// SerializedFile target-platform patching.
// ---------------------------------------------------------------------------

struct TargetPlatformField {
  bool found = false;
  size_t offset = 0;   // absolute offset of the int32 within `data`
  bool bigEndian = false;
  int32_t value = 0;
};

// Locates m_TargetPlatform in the SerializedFile that starts at `base`.
// Layout per the SerializedFile header:
//   u32be metadataSize, u32be fileSize, u32be version, u32be dataOffset
//   [version >= 9]  u8 endianness + 3 reserved bytes
//   [version >= 22] u32be metadataSize, u64be fileSize, u64be dataOffset, u64be unknown
//   [version >= 7]  cstring unityVersion
//   [version >= 8]  int32 m_TargetPlatform      <- in the file's own endianness
TargetPlatformField FindTargetPlatform(uint8_t const* data, size_t dataSize, size_t base, uint64_t nodeSize) {
  TargetPlatformField field;
  if (base >= dataSize) return field;
  size_t const available = std::min<uint64_t>(nodeSize, dataSize - base);
  if (available < 32) return field;

  ByteReader reader(data + base, static_cast<size_t>(available));
  uint64_t metadataSize = reader.u32be();
  uint64_t fileSize = reader.u32be();
  uint32_t const version = reader.u32be();
  uint64_t dataOffset = reader.u32be();
  if (!reader.ok() || version < 8 || version > 100) return field;

  bool bigEndian = true;
  if (version >= 9) {
    bigEndian = reader.u8() != 0;
    reader.skip(3);
  }
  if (version >= 22) {
    metadataSize = reader.u32be();
    fileSize = reader.u64be();
    dataOffset = reader.u64be();
    reader.skip(8);  // reserved
  }
  if (!reader.ok()) return field;

  // Bundles also carry raw payload nodes (.resS/.resource streams). Those are
  // not serialized files and must not be "patched"; a serialized file always
  // describes its own extent consistently, which raw data effectively never
  // does by chance.
  if (fileSize == 0 || fileSize > nodeSize) return field;
  if (dataOffset == 0 || dataOffset > fileSize) return field;
  if (metadataSize == 0 || metadataSize > fileSize) return field;
  if (version >= 7) {
    std::string const unityVersion = reader.cstring(64);
    if (!reader.ok() || unityVersion.empty()) return field;
  }
  size_t const fieldOffset = reader.position();
  if (!reader.ok() || available - fieldOffset < 4) return field;

  field.found = true;
  field.offset = base + fieldOffset;
  field.bigEndian = bigEndian;
  field.value = static_cast<int32_t>(ReadU32(data + field.offset, bigEndian));
  return field;
}

// ---------------------------------------------------------------------------
// Repacking.
// ---------------------------------------------------------------------------

// Replaces one directory node's contents inside an unpacked archive.
//
// A node whose length changes moves every node stored after it, and the
// directory table records absolute offsets into the unpacked data, so both the
// bytes and the table have to be rebuilt together. This is the outer half of
// conversion step 4: RewriteSerializedFile fixes the object offsets inside one
// file, and this fixes the node offsets around it.
//
// The gap that preceded each node in the original is preserved, so replacing a
// node with exactly the bytes it already had reproduces the buffer it started
// as.
bool ReplaceNodeData(std::vector<DirectoryNode>& nodes, std::vector<uint8_t>& data,
                     size_t nodeIndex, std::vector<uint8_t> const& replacement) {
  if (nodeIndex >= nodes.size()) return false;

  std::vector<size_t> order(nodes.size());
  for (size_t i = 0; i < order.size(); i++) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&nodes](size_t a, size_t b) {
    return nodes[a].offset < nodes[b].offset;
  });

  std::vector<uint8_t> rebuilt;
  uint64_t previousEnd = 0;
  for (size_t index : order) {
    DirectoryNode& node = nodes[index];
    if (node.offset < previousEnd) return false;  // overlapping nodes: refuse rather than guess
    if (node.offset > data.size() || node.size > data.size() - node.offset) return false;

    uint64_t const gap = node.offset - previousEnd;
    previousEnd = node.offset + node.size;
    rebuilt.insert(rebuilt.end(), static_cast<size_t>(gap), 0);

    uint8_t const* source = data.data() + node.offset;
    size_t const sourceSize = static_cast<size_t>(node.size);
    node.offset = rebuilt.size();
    if (index == nodeIndex) {
      node.size = replacement.size();
      rebuilt.insert(rebuilt.end(), replacement.begin(), replacement.end());
    } else {
      rebuilt.insert(rebuilt.end(), source, source + sourceSize);
    }
  }

  // Anything past the last node -- Unity does not write it, but a bundle is
  // untrusted input -- is carried over rather than dropped.
  if (previousEnd < data.size()) {
    rebuilt.insert(rebuilt.end(), data.begin() + static_cast<long>(previousEnd), data.end());
  }

  data = std::move(rebuilt);
  return true;
}

bool WriteConverted(std::string const& destPath, ArchiveHeader const& header,
                    std::vector<DirectoryNode> const& nodes, std::vector<uint8_t> const& data, Result& result) {
  // Blocks/directory table for the uncompressed output.
  std::vector<uint8_t> blocksInfo;
  blocksInfo.insert(blocksInfo.end(), 16, 0);  // hash: Unity does not verify this for uncompressed archives

  uint64_t remaining = data.size();
  uint32_t blockCount = static_cast<uint32_t>((remaining + kOutputBlockSize - 1) / kOutputBlockSize);
  AppendU32BE(blocksInfo, blockCount);
  while (remaining > 0) {
    uint32_t const chunk = static_cast<uint32_t>(std::min<uint64_t>(remaining, kOutputBlockSize));
    AppendU32BE(blocksInfo, chunk);  // uncompressed size
    AppendU32BE(blocksInfo, chunk);  // compressed size (identical: stored)
    AppendU16BE(blocksInfo, static_cast<uint16_t>(kCompressionNone));
    remaining -= chunk;
  }

  AppendU32BE(blocksInfo, static_cast<uint32_t>(nodes.size()));
  for (auto const& node : nodes) {
    AppendU64BE(blocksInfo, node.offset);
    AppendU64BE(blocksInfo, node.size);
    AppendU32BE(blocksInfo, node.flags);
    AppendCString(blocksInfo, node.path);
  }

  // Header. Compression is cleared, the table is stored up front, and no
  // padding flag is set, so the reader path stays the simplest one Unity has.
  uint32_t const outFlags = kArchiveBlocksAndDirectoryInfoCombined;

  std::vector<uint8_t> out;
  AppendCString(out, "UnityFS");
  AppendU32BE(out, header.version);
  AppendCString(out, header.unityVersion);
  AppendCString(out, header.unityRevision);
  size_t const sizeFieldOffset = out.size();
  AppendU64BE(out, 0);  // patched below once the total is known
  AppendU32BE(out, static_cast<uint32_t>(blocksInfo.size()));
  AppendU32BE(out, static_cast<uint32_t>(blocksInfo.size()));
  AppendU32BE(out, outFlags);
  if (header.version >= 7) {
    while (out.size() % 16 != 0) out.push_back(0);
  }
  out.insert(out.end(), blocksInfo.begin(), blocksInfo.end());

  uint64_t const totalSize = static_cast<uint64_t>(out.size()) + data.size();
  for (int i = 0; i < 8; i++) {
    out[sizeFieldOffset + i] = static_cast<uint8_t>(totalSize >> (56 - 8 * i));
  }

  std::error_code ec;
  auto const parent = std::filesystem::path(destPath).parent_path();
  if (!parent.empty()) std::filesystem::create_directories(parent, ec);

  // Write to a scratch name and rename into place, so a conversion that is cut
  // short (the game is closed, the device runs out of space) can never leave a
  // truncated file that a later run would treat as a finished, cached bundle.
  std::string const tempPath = destPath + ".part";
  ec.clear();
  std::filesystem::remove(tempPath, ec);

  {
    std::ofstream os(tempPath, std::ios::binary | std::ios::trunc);
    if (!os.is_open()) {
      result.status = Status::DestUnwritable;
      result.message = "could not open '" + tempPath + "' for writing";
      return false;
    }
    os.write(reinterpret_cast<char const*>(out.data()), static_cast<std::streamsize>(out.size()));
    os.write(reinterpret_cast<char const*>(data.data()), static_cast<std::streamsize>(data.size()));
    os.flush();
    if (!os.good()) {
      os.close();
      ec.clear();
      std::filesystem::remove(tempPath, ec);
      result.status = Status::DestUnwritable;
      result.message = "write to '" + tempPath + "' failed (out of space?)";
      return false;
    }
  }

  ec.clear();
  std::filesystem::rename(tempPath, destPath, ec);
  if (ec) {
    ec.clear();
    std::filesystem::remove(tempPath, ec);
    result.status = Status::DestUnwritable;
    result.message = "could not move the converted bundle into '" + destPath + "'";
    return false;
  }
  result.outputBytes = totalSize;
  return true;
}

// Shared front half of ConvertToAndroid/NeedsAndroidConversion: read, unpack,
// and locate every SerializedFile's target-platform field.
bool LoadAndScan(std::string const& sourcePath, ArchiveHeader& header, std::vector<DirectoryNode>& nodes,
                 std::vector<uint8_t>& data, std::vector<TargetPlatformField>& fields, Result& result) {
  FileSource source;
  if (!source.Open(sourcePath, result)) return false;

  std::vector<StorageBlock> blocks;
  if (!ParseArchive(source, header, blocks, nodes, result)) return false;
  if (!ReadBlocks(source, header, blocks, data, result)) return false;

  for (auto const& node : nodes) {
    if (node.offset > data.size() || data.size() - node.offset < node.size) continue;
    auto field = FindTargetPlatform(data.data(), data.size(), static_cast<size_t>(node.offset), node.size);
    if (field.found) fields.push_back(field);
  }
  result.serializedFilesSeen = static_cast<int>(fields.size());
  if (fields.empty()) {
    result.status = Status::Corrupt;
    result.message = "no SerializedFile headers found inside the archive";
    return false;
  }
  for (auto const& field : fields) {
    if (field.value != kBuildTargetAndroid) {
      result.sourcePlatform = BuildTargetName(field.value);
      break;
    }
  }
  if (result.sourcePlatform.empty()) result.sourcePlatform = BuildTargetName(kBuildTargetAndroid);
  return true;
}

}  // namespace

std::string_view StatusText(Status status) {
  switch (status) {
    case Status::Success: return "converted";
    case Status::AlreadyAndroid: return "already an Android bundle";
    case Status::SourceUnreadable: return "source bundle unreadable";
    case Status::NotAUnityBundle: return "not a UnityFS asset bundle";
    case Status::UnsupportedCompression: return "unsupported bundle compression";
    case Status::Corrupt: return "bundle structure could not be parsed";
    case Status::DestUnwritable: return "could not write the converted bundle";
    case Status::OutOfMemory: return "bundle too large to convert on device";
  }
  return "unknown";
}

std::string BuildTargetName(int32_t target) {
  switch (target) {
    case kBuildTargetStandaloneOSX: return "StandaloneOSX";
    case kBuildTargetStandaloneWindows: return "StandaloneWindows";
    case kBuildTargetiOS: return "iOS";
    case kBuildTargetAndroid: return "Android";
    case kBuildTargetStandaloneWindows64: return "StandaloneWindows64";
    case kBuildTargetWebGL: return "WebGL";
    case kBuildTargetStandaloneLinux64: return "StandaloneLinux64";
    default: return "BuildTarget(" + std::to_string(target) + ")";
  }
}

bool IsUnityBundleFile(std::string const& path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return false;
  char signature[8] = {};
  if (!in.read(signature, sizeof(signature))) return false;
  return std::memcmp(signature, "UnityFS\0", sizeof(signature)) == 0;
}

namespace {

// True for a ShaderGpuProgramType that carries DirectX bytecode.
//
// Every DirectX stage is offered to the translator rather than filtered by
// stage here. Which stages it can actually handle is its own business and it
// says so by name -- a hull program comes back with "tessellation programs are
// not translated" -- and keeping the decision in one place stops this list
// drifting out of step with what the translator grew to support.
bool IsTranslatableDirectXProgram(int32_t programType) {
  switch (programType) {
    case SerializedFileParse::kGpuProgramDX11VertexSM40:
    case SerializedFileParse::kGpuProgramDX11VertexSM50:
    case SerializedFileParse::kGpuProgramDX11PixelSM40:
    case SerializedFileParse::kGpuProgramDX11PixelSM50:
    case SerializedFileParse::kGpuProgramDX11GeometrySM40:
    case SerializedFileParse::kGpuProgramDX11GeometrySM50:
    case SerializedFileParse::kGpuProgramDX11HullSM50:
    case SerializedFileParse::kGpuProgramDX11DomainSM50:
      return true;
    default:
      return false;
  }
}

// Translates one shader's programs in place. Returns false, with a reason, if
// any program in it could not be translated: a shader is converted whole or
// not at all, because a program store holding half GLSL and half DirectX would
// leave Unity picking whichever it found first.
bool TranslateShaderPrograms(std::vector<SerializedFileParse::ShaderSubProgram>& programs,
                             int& programsTranslated, std::string& reason) {
  int translated = 0;
  for (auto& program : programs) {
    if (SerializedFileParse::GpuProgramIsGlslSource(program.programType)) continue;
    if (!IsTranslatableDirectXProgram(program.programType)) {
      reason = "carries a " + std::string(SerializedFileParse::GpuProgramTypeName(program.programType)) +
               " program, which is not DirectX bytecode";
      return false;
    }
    auto const result = Vivify::Dxbc::TranslateDxbcToGlsl(
        program.code.empty() ? nullptr : program.code.data(), program.code.size());
    if (!result.ok) {
      reason = result.error;
      return false;
    }
    program.code.assign(result.source.begin(), result.source.end());
    // Unity's GLES program types do not distinguish vertex from fragment; which
    // stage a program is comes from the sub-program list that points at its
    // blob index, and those are left exactly where they were.
    program.programType = SerializedFileParse::kGpuProgramGLES3;
    // The statistics block describes register and instruction counts for a
    // program that no longer exists. Unity does not need it to link a GLES
    // shader, and leaving DirectX numbers in it would be a lie in the one place
    // a reader would go looking.
    program.stats.assign(program.stats.size(), 0u);
    translated++;
  }
  if (translated == 0) {
    reason = "has no DirectX programs to translate";
    return false;
  }
  programsTranslated += translated;
  return true;
}


// ---------------------------------------------------------------------------
// Conversion through m_ParsedForm
//
// Translating each program's bytecode is only half of converting a shader.
// Unity decides what runs from m_ParsedForm, not from the store: every pass
// lists its keyword variants per stage, each naming a store entry and the
// ShaderGpuProgramType of the program there. Unity keeps only the entries whose
// type its renderer can run, so a store full of GLSL behind a program list that
// still says "Direct3D 11" is a shader with no programs at all on a Quest --
// isSupported = false, which is what every converted shader reported.
//
// GLES also does not take stages one at a time the way D3D does. Unity stores a
// GLES variant as one GLSL source holding every stage between #ifdef VERTEX /
// #ifdef FRAGMENT / #ifdef GEOMETRY sections and compiles it into a linked
// program; the vertex entry carries that source. So each vertex variant is
// linked here with the fragment (and geometry) variant Unity would pair it
// with, and the same linked source is written to every stage's entry, so
// whichever entry Unity reads a GLES program from holds a complete one.
//
// Everything is emitted for multiview (see Dxbc::GlslOptions::multiview): the
// Quest renders both eyes in one pass, and a program that does not declare two
// views cannot draw into that framebuffer at all.
// ---------------------------------------------------------------------------

struct VariantKey {
  int32_t subShader, pass;
  bool player;
  int32_t list;
  bool operator<(VariantKey const& o) const {
    return std::tie(subShader, pass, player, list) < std::tie(o.subShader, o.pass, o.player, o.list);
  }
};

int32_t GlesProgramTypeForVersion(int version) {
  if (version >= 320) return SerializedFileParse::kGpuProgramGLES31AEP;
  if (version >= 310) return SerializedFileParse::kGpuProgramGLES31;
  return SerializedFileParse::kGpuProgramGLES3;
}

bool IsDirectXProgramType(int32_t type) {
  return type >= SerializedFileParse::kGpuProgramDX11VertexSM40 &&
         type <= SerializedFileParse::kGpuProgramDX11DomainSM50;
}

// How well a candidate's keyword set matches the one wanted: every shared
// keyword counts for it, every keyword only one side has counts against it.
int KeywordMatchScore(std::vector<uint16_t> const& want, std::vector<uint16_t> const& have) {
  int score = 0;
  for (uint16_t k : want) {
    score += std::find(have.begin(), have.end(), k) != have.end() ? 4 : -1;
  }
  for (uint16_t k : have) {
    if (std::find(want.begin(), want.end(), k) == want.end()) score -= 2;
  }
  return score;
}

SerializedFileParse::ParsedProgramRef const* BestMatch(
    std::vector<SerializedFileParse::ParsedProgramRef const*> const& candidates,
    SerializedFileParse::ParsedProgramRef const& to) {
  SerializedFileParse::ParsedProgramRef const* best = nullptr;
  int bestScore = 0;
  for (auto const* candidate : candidates) {
    int score = KeywordMatchScore(to.keywordIndices, candidate->keywordIndices);
    if (candidate->hardwareTier != to.hardwareTier) score -= 1;
    if (best == nullptr || score > bestScore) {
      best = candidate;
      bestScore = score;
    }
  }
  return best;
}

// Replaces a translated stage's "#version N es" line with the version the
// whole program links at. GLSL ES will not link stages of different versions.
std::string WithVersion(std::string const& source, int version) {
  size_t const end = source.find('\n');
  if (source.rfind("#version ", 0) != 0 || end == std::string::npos) return source;
  return "#version " + std::to_string(version) + " es" + source.substr(end);
}

// Inserts declarations after a stage's preamble (#version, #extension,
// layout(num_views), precision), where GLSL ES allows them.
std::string InsertDeclarations(std::string const& source, std::string const& declarations) {
  if (declarations.empty()) return source;
  size_t at = source.find("precision highp int;\n");
  if (at != std::string::npos) {
    at += std::string("precision highp int;\n").size();
  } else {
    at = source.find('\n');
    at = at == std::string::npos ? source.size() : at + 1;
  }
  return source.substr(0, at) + declarations + source.substr(at);
}

// Every "<qualifiers>in vec4 vs_X;" a stage declares, with its qualifiers.
std::map<std::string, std::string> DeclaredVaryings(std::string const& source, std::string const& storage) {
  std::map<std::string, std::string> out;
  size_t pos = 0;
  std::string const needle = storage + " vec4 vs_";
  while ((pos = source.find(needle, pos)) != std::string::npos) {
    size_t const lineStart = source.rfind('\n', pos);
    size_t const begin = lineStart == std::string::npos ? 0 : lineStart + 1;
    size_t const nameStart = pos + needle.size() - 3;  // at "vs_"
    size_t const nameEnd = source.find(';', nameStart);
    if (nameEnd == std::string::npos) break;
    // Only a whole declaration line counts, not a use of the name.
    if (source.find('\n', begin) > nameEnd) {
      out[source.substr(nameStart, nameEnd - nameStart)] = source.substr(begin, pos - begin);
    }
    pos = nameEnd;
  }
  return out;
}

// Links a vertex, a fragment and (optionally) a geometry stage into the single
// source Unity stores for a GLES variant.
//
// Two things GLSL ES checks at link time that stages translated one at a time
// cannot know about each other: an interpolation qualifier on a varying must
// match on both sides, and every varying the next stage reads must be written
// by the one before it. A fragment's `flat`/`centroid` is copied onto the
// vertex (or geometry) output, and a varying the fragment reads but nothing
// writes is declared on the writing side, where it is simply undefined -- which
// is also what D3D gives a pixel shader for an input the vertex shader skipped.
std::string LinkStages(Vivify::Dxbc::GlslResult const& vertex, Vivify::Dxbc::GlslResult const* fragment,
                       Vivify::Dxbc::GlslResult const* geometry, int& version) {
  version = vertex.version;
  if (fragment != nullptr) version = std::max(version, fragment->version);
  if (geometry != nullptr) version = std::max(version, geometry->version);

  std::string vs = WithVersion(vertex.source, version);
  std::string fs = fragment != nullptr ? WithVersion(fragment->source, version) : std::string();
  std::string gs = geometry != nullptr ? WithVersion(geometry->source, version) : std::string();

  if (fragment != nullptr) {
    std::string& feeder = geometry != nullptr ? gs : vs;
    auto const reads = DeclaredVaryings(fs, "in");
    auto const writes = DeclaredVaryings(feeder, "out");
    std::string missing;
    for (auto const& [name, qualifiers] : reads) {
      auto it = writes.find(name);
      if (it == writes.end()) {
        missing += qualifiers + "out vec4 " + name + ";\n";
        continue;
      }
      if (it->second == qualifiers) continue;
      std::string const from = it->second + "out vec4 " + name + ";";
      std::string const to = qualifiers + "out vec4 " + name + ";";
      size_t const at = feeder.find(from);
      if (at != std::string::npos) feeder.replace(at, from.size(), to);
    }
    feeder = InsertDeclarations(feeder, missing);
  }

  std::string linked = "#ifdef VERTEX\n" + vs + "#endif\n";
  if (fragment != nullptr) linked += "#ifdef FRAGMENT\n" + fs + "#endif\n";
  if (geometry != nullptr) linked += "#ifdef GEOMETRY\n" + gs + "#endif\n";
  return linked;
}

struct LinkedShader {
  bool converted = false;
  std::string reason;
  std::vector<uint8_t> body;
  int variantsLinked = 0;
  int variantsRefused = 0;
  int stereoRemapped = 0;
  int programsTranslated = 0;
};

LinkedShader ConvertThroughParsedForm(uint8_t const* nodeData, size_t nodeSize,
                                      SerializedFileParse::ShaderObject const& shader) {
  using SerializedFileParse::ParsedProgramRef;
  LinkedShader out;

  auto decoded = SerializedFileParse::DecodeShaderPrograms(nodeData, nodeSize, shader);
  if (!decoded.ok) {
    out.reason = decoded.message.empty() ? "its program store could not be read" : decoded.message;
    return out;
  }

  // Only a Direct3D 11 store is converted, and only its one group.
  int32_t group = -1;
  for (size_t i = 0; i < shader.platforms.size(); i++) {
    if (shader.platforms[i] == SerializedFileParse::kShaderPlatformD3D11) group = static_cast<int32_t>(i);
  }
  if (group < 0) {
    out.reason = "it has no Direct3D 11 programs";
    return out;
  }

  std::map<uint32_t, size_t> entryAt;  // blob index -> position in decoded.programs
  int32_t entryCount = 0;
  for (size_t i = 0; i < decoded.programs.size(); i++) {
    if (decoded.programs[i].groupIndex != group) continue;
    entryAt[static_cast<uint32_t>(decoded.programs[i].blobIndex)] = i;
    entryCount++;
  }

  // Working copies: blob indices may be repointed below.
  std::vector<ParsedProgramRef> refs;
  for (auto const& ref : shader.programRefs) {
    if (!IsDirectXProgramType(ref.gpuProgramType)) continue;
    if (ref.stage == SerializedFileParse::kProgramStageRayTracing) continue;
    refs.push_back(ref);
  }
  if (refs.empty()) {
    out.reason = "m_ParsedForm lists no Direct3D 11 programs";
    return out;
  }

  std::vector<SerializedFileParse::BytePatch> patches;
  auto patchU32 = [&patches](size_t at, uint32_t value) {
    patches.push_back({at, {static_cast<uint8_t>(value & 0xff), static_cast<uint8_t>((value >> 8) & 0xff),
                            static_cast<uint8_t>((value >> 16) & 0xff), static_cast<uint8_t>((value >> 24) & 0xff)}});
  };

  // Point each plain variant at its single-pass instanced twin. PC Vivify
  // bundles are built for SPI, so a stereo-aware shader carries both: the
  // plain one reads unity_MatrixVP -- one camera -- and the SPI one indexes
  // unity_StereoMatrixVP by an eye taken from the instance ID. On the Quest the
  // STEREO_INSTANCING_ON keyword is never enabled, so Unity picks the plain
  // variant; handing it the SPI code, translated for multiview, is what gives
  // each eye its own projection.
  int32_t spiKeyword = -1;
  for (size_t i = 0; i < shader.keywordNames.size(); i++) {
    if (shader.keywordNames[i] == "STEREO_INSTANCING_ON") spiKeyword = static_cast<int32_t>(i);
  }
  if (spiKeyword >= 0) {
    for (auto& plain : refs) {
      if (std::find(plain.keywordIndices.begin(), plain.keywordIndices.end(), spiKeyword) !=
          plain.keywordIndices.end()) {
        continue;
      }
      for (auto const& spi : refs) {
        if (spi.subShader != plain.subShader || spi.pass != plain.pass || spi.stage != plain.stage ||
            spi.player != plain.player || spi.list != plain.list || spi.hardwareTier != plain.hardwareTier) {
          continue;
        }
        if (std::find(spi.keywordIndices.begin(), spi.keywordIndices.end(), spiKeyword) ==
            spi.keywordIndices.end()) {
          continue;
        }
        std::vector<uint16_t> without;
        for (uint16_t k : spi.keywordIndices) {
          if (static_cast<int32_t>(k) != spiKeyword) without.push_back(k);
        }
        std::vector<uint16_t> mine = plain.keywordIndices;
        std::sort(without.begin(), without.end());
        std::sort(mine.begin(), mine.end());
        if (without != mine) continue;
        if (plain.blobIndex != spi.blobIndex) {
          plain.blobIndex = spi.blobIndex;
          patchU32(plain.blobIndexFileOffset, spi.blobIndex);
          out.stereoRemapped++;
        }
        break;
      }
    }
  }

  // Translate each program the variants use, once.
  Vivify::Dxbc::GlslOptions options;
  options.multiview = true;
  std::map<uint32_t, Vivify::Dxbc::GlslResult> translated;
  auto translate = [&](uint32_t blob) -> Vivify::Dxbc::GlslResult const* {
    auto cached = translated.find(blob);
    if (cached == translated.end()) {
      Vivify::Dxbc::GlslResult result;
      auto at = entryAt.find(blob);
      if (at == entryAt.end()) {
        result.error = "entry " + std::to_string(blob) + " is not in the store";
      } else {
        auto const& program = decoded.programs[at->second];
        if (program.raw || !IsTranslatableDirectXProgram(program.programType)) {
          result.error = "entry " + std::to_string(blob) + " is not DirectX bytecode";
        } else {
          result = Vivify::Dxbc::TranslateDxbcToGlsl(program.code.empty() ? nullptr : program.code.data(),
                                                     program.code.size(), options);
          if (result.ok) out.programsTranslated++;
        }
      }
      cached = translated.emplace(blob, std::move(result)).first;
    }
    return cached->second.ok ? &cached->second : nullptr;
  };

  std::map<VariantKey, std::vector<ParsedProgramRef const*>> byStage[3];
  bool tessellation = false;
  for (auto const& ref : refs) {
    VariantKey const key{ref.subShader, ref.pass, ref.player, ref.list};
    if (ref.stage == SerializedFileParse::kProgramStageHull || ref.stage == SerializedFileParse::kProgramStageDomain) {
      tessellation = true;
      continue;
    }
    if (ref.stage >= 0 && ref.stage <= 2) byStage[ref.stage][key].push_back(&ref);
  }

  // What each store entry has been assigned, so an entry shared by variants
  // that link to different programs is split rather than overwritten.
  std::map<uint32_t, std::string> assigned;
  std::vector<SerializedFileParse::ShaderSubProgram> added;
  auto place = [&](ParsedProgramRef const& ref, std::string const& source, int version) {
    uint32_t blob = ref.blobIndex;
    auto existing = assigned.find(blob);
    if (existing != assigned.end() && existing->second != source) {
      // Another variant already wrote a different program here: give this one
      // its own entry, cloned from the original so it keeps its keywords.
      auto base = entryAt.find(ref.blobIndex);
      if (base == entryAt.end()) return false;
      SerializedFileParse::ShaderSubProgram copy = decoded.programs[base->second];
      copy.blobIndex = copy.programIndex = entryCount + static_cast<int32_t>(added.size());
      copy.segment = 0;
      blob = static_cast<uint32_t>(copy.blobIndex);
      added.push_back(std::move(copy));
      patchU32(ref.blobIndexFileOffset, blob);
    }
    assigned[blob] = source;
    int32_t const type = GlesProgramTypeForVersion(version);
    patches.push_back({ref.gpuProgramTypeFileOffset, {static_cast<uint8_t>(static_cast<int8_t>(type))}});
    return true;
  };

  std::set<VariantKey> keys;
  for (auto const& stage : byStage) {
    for (auto const& [key, _] : stage) keys.insert(key);
  }
  for (auto const& key : keys) {
    auto const& vertices = byStage[0][key];
    auto const& fragments = byStage[1][key];
    auto const& geometries = byStage[2][key];
    if (vertices.empty()) continue;

    auto link = [&](ParsedProgramRef const& vertex, ParsedProgramRef const* fragment,
                    ParsedProgramRef const* geometry, std::string& source, int& version) {
      auto const* vs = translate(vertex.blobIndex);
      auto const* fs = fragment != nullptr ? translate(fragment->blobIndex) : nullptr;
      auto const* gs = geometry != nullptr ? translate(geometry->blobIndex) : nullptr;
      if (vs == nullptr || (fragment != nullptr && fs == nullptr) || (geometry != nullptr && gs == nullptr)) {
        return false;
      }
      source = LinkStages(*vs, fs, gs, version);
      return true;
    };

    for (auto const* vertex : vertices) {
      std::string source;
      int version = 300;
      auto const* fragment = BestMatch(fragments, *vertex);
      auto const* geometry = BestMatch(geometries, *vertex);
      if (!link(*vertex, fragment, geometry, source, version) || !place(*vertex, source, version)) {
        out.variantsRefused++;
        continue;
      }
      out.variantsLinked++;
    }
    for (auto const* stageList : {&fragments, &geometries}) {
      for (auto const* ref : *stageList) {
        std::string source;
        int version = 300;
        auto const* vertex = BestMatch(vertices, *ref);
        ParsedProgramRef const* fragment = stageList == &fragments ? ref : BestMatch(fragments, *ref);
        ParsedProgramRef const* geometry = stageList == &geometries ? ref : BestMatch(geometries, *ref);
        if (vertex == nullptr || !link(*vertex, fragment, geometry, source, version) ||
            !place(*ref, source, version)) {
          out.variantsRefused++;
        }
      }
    }
  }

  if (assigned.empty()) {
    // Nothing linked. Report the first translation failure, which is the
    // useful thing to know about a shader that stays on its stand-in.
    for (auto const& [blob, result] : translated) {
      if (!result.ok) {
        out.reason = result.error;
        break;
      }
    }
    if (out.reason.empty()) out.reason = tessellation ? "it uses tessellation" : "no variant could be linked";
    return out;
  }

  // Write the linked sources into the store.
  std::vector<SerializedFileParse::ShaderSubProgram> programs = std::move(decoded.programs);
  for (auto& program : added) programs.push_back(std::move(program));
  for (auto& program : programs) {
    if (program.groupIndex != group) continue;
    auto it = assigned.find(static_cast<uint32_t>(program.blobIndex));
    if (it == assigned.end()) continue;
    program.code.assign(it->second.begin(), it->second.end());
    // The program's own header type follows the linked version, the same way
    // m_ParsedForm's does; the statistics described the DirectX program.
    int version = 300;
    if (it->second.find("#version 320 es") != std::string::npos) version = 320;
    else if (it->second.find("#version 310 es") != std::string::npos) version = 310;
    program.programType = GlesProgramTypeForVersion(version);
    program.stats.assign(program.stats.size(), 0u);
  }

  std::vector<int32_t> platforms = shader.platforms;
  platforms[static_cast<size_t>(group)] = SerializedFileParse::kShaderPlatformGLES3Plus;
  auto store = SerializedFileParse::EncodeShaderPrograms(platforms, programs, decoded.layouts);
  if (!store.ok) {
    out.reason = store.message;
    return out;
  }
  auto rebuilt = SerializedFileParse::BuildShaderObjectBody(nodeData, nodeSize, shader, platforms, store, patches);
  if (!rebuilt.ok) {
    out.reason = rebuilt.message;
    return out;
  }
  out.converted = true;
  out.body = std::move(rebuilt.body);
  return out;
}

}  // namespace

ShaderConversion ConvertShadersToGles(std::string const& sourcePath,
                                      std::string const& destPath) {
  ShaderConversion conversion;
  ArchiveHeader header;
  std::vector<DirectoryNode> nodes;
  std::vector<uint8_t> data;
  std::vector<TargetPlatformField> fields;
  Result result;
  if (!LoadAndScan(sourcePath, header, nodes, data, fields, result)) {
    conversion.status = result.status;
    conversion.message = result.message;
    return conversion;
  }

  // Retarget first: the platform field is a fixed-width int inside each file's
  // header, so it can be written before anything moves, and doing it here means
  // a bundle whose shaders all refuse still comes out loadable.
  int retargeted = 0;
  for (auto const& field : fields) {
    if (field.value == kBuildTargetAndroid) continue;
    WriteU32(data.data() + field.offset, static_cast<uint32_t>(kBuildTargetAndroid),
             field.bigEndian);
    retargeted++;
  }

  constexpr size_t kMaxLoggedRefusals = 8;
  int filesRewritten = 0;
  for (size_t index = 0; index < nodes.size(); index++) {
    if (nodes[index].offset > data.size() ||
        nodes[index].size > data.size() - nodes[index].offset) {
      conversion.status = Status::Corrupt;
      conversion.message = "directory node '" + nodes[index].path + "' points outside the data";
      return conversion;
    }
    uint8_t const* const nodeData = data.data() + nodes[index].offset;
    size_t const nodeSize = static_cast<size_t>(nodes[index].size);

    auto file = SerializedFileParse::InspectSerializedFile(nodeData, nodeSize);
    if (!file.isSerializedFile) continue;

    // Make the block-compressed textures readable before anything moves.
    //
    // m_IsReadable is a single serialized byte in a fixed-width field, so it is
    // written where it sits: the object does not change size, nothing after it
    // moves, and a bundle whose shaders all refuse still comes out with usable
    // textures. Without the flag Unity drops each texture's CPU copy after
    // upload, and the mod's on-device decoder is left asking for bytes that are
    // no longer there -- which is how converted levels came to render black.
    for (auto const& texture : file.textures) {
      if (!SerializedFileParse::TextureFormatNeedsDecodingOnQuest(texture.textureFormat)) continue;
      conversion.texturesSeen++;
      if (texture.streamed) conversion.texturesStreamed++;
      if (!texture.isReadablePresent || texture.isReadable) continue;
      size_t const at = nodes[index].offset + texture.isReadableFileOffset;
      if (at >= data.size()) continue;
      // Only a byte that currently reads as a bool is touched. Anything else
      // means the field was not where the type tree said it was, and writing
      // over it would corrupt the texture.
      if (data[at] > 1) continue;
      data[at] = 1;
      conversion.texturesMarkedReadable++;
    }

    if (file.shaders.empty()) continue;

    std::vector<SerializedFileParse::ObjectEdit> edits;
    for (auto const& shader : file.shaders) {
      conversion.shadersSeen++;
      bool alreadyRuns = false;
      for (int32_t platform : shader.platforms) {
        if (SerializedFileParse::ShaderPlatformRunsOnQuest(platform)) alreadyRuns = true;
      }
      if (alreadyRuns) {
        conversion.shadersLeftAlone++;
        continue;
      }

      if (shader.parsedFormRead) {
        auto linked = ConvertThroughParsedForm(nodeData, nodeSize, shader);
        conversion.variantsLinked += linked.variantsLinked;
        conversion.variantsRefused += linked.variantsRefused;
        conversion.stereoVariantsRemapped += linked.stereoRemapped;
        if (!linked.converted) {
          conversion.shadersRefused++;
          if (conversion.refusals.size() < kMaxLoggedRefusals) {
            conversion.refusals.push_back(
                (shader.name.empty() ? ("shader@" + std::to_string(shader.pathID)) : shader.name) + ": " +
                linked.reason);
          }
          continue;
        }
        edits.push_back({shader.pathID, std::move(linked.body)});
        conversion.shadersTranslated++;
        conversion.shadersLinked++;
        conversion.programsTranslated += linked.programsTranslated;
        continue;
      }

      auto decoded = SerializedFileParse::DecodeShaderPrograms(nodeData, nodeSize, shader);
      if (!decoded.ok || decoded.programs.empty()) {
        conversion.shadersRefused++;
        if (conversion.refusals.size() < kMaxLoggedRefusals) {
          conversion.refusals.push_back(
              (shader.name.empty() ? ("shader@" + std::to_string(shader.pathID)) : shader.name) + ": " +
              (decoded.message.empty() ? "no programs decoded" : decoded.message));
        }
        continue;
      }

      std::string reason;
      int translatedHere = 0;
      if (!TranslateShaderPrograms(decoded.programs, translatedHere, reason)) {
        conversion.shadersRefused++;
        if (conversion.refusals.size() < kMaxLoggedRefusals) {
          conversion.refusals.push_back(
              (shader.name.empty() ? ("shader@" + std::to_string(shader.pathID)) : shader.name) + ": " + reason);
        }
        continue;
      }

      std::vector<int32_t> platforms(shader.platforms.size(),
                                     SerializedFileParse::kShaderPlatformGLES3Plus);
      auto store = SerializedFileParse::EncodeShaderPrograms(platforms, decoded.programs, decoded.layouts);
      auto rebuilt = SerializedFileParse::BuildShaderObjectBody(nodeData, nodeSize, shader,
                                                                platforms, store);
      if (!rebuilt.ok) {
        conversion.shadersRefused++;
        if (conversion.refusals.size() < kMaxLoggedRefusals) {
          conversion.refusals.push_back(
              (shader.name.empty() ? ("shader@" + std::to_string(shader.pathID)) : shader.name) + ": " +
              rebuilt.message);
        }
        continue;
      }
      edits.push_back({shader.pathID, std::move(rebuilt.body)});
      conversion.shadersTranslated++;
      conversion.programsTranslated += translatedHere;
    }

    if (edits.empty()) continue;
    auto rewritten = SerializedFileParse::RewriteSerializedFile(nodeData, nodeSize, edits);
    if (!rewritten.ok) {
      conversion.status = Status::Corrupt;
      conversion.message = "could not rebuild '" + nodes[index].path + "': " + rewritten.message;
      return conversion;
    }
    if (!ReplaceNodeData(nodes, data, index, rewritten.data)) {
      conversion.status = Status::Corrupt;
      conversion.message = "could not relay the archive around a rebuilt '" + nodes[index].path + "'";
      return conversion;
    }
    filesRewritten++;
  }

  // Nothing to retarget and nothing translated means the bundle already runs
  // here; writing a byte-for-byte copy of it would only cost storage on the
  // headset and hide that fact from the caller.
  if (retargeted == 0 && conversion.shadersTranslated == 0 && conversion.texturesMarkedReadable == 0) {
    conversion.status = Status::AlreadyAndroid;
    conversion.message =
        conversion.shadersRefused > 0
            ? "already targets Android; " + std::to_string(conversion.shadersRefused) +
                  " shader(s) could not be translated and were left as they were"
            : "already targets Android and has no DirectX shaders to translate";
    return conversion;
  }

  if (!WriteConverted(destPath, header, nodes, data, result)) {
    conversion.status = result.status;
    conversion.message = result.message;
    return conversion;
  }

  conversion.status = Status::Success;
  conversion.outputBytes = result.outputBytes;
  conversion.message = "translated " + std::to_string(conversion.shadersTranslated) + " of " +
                       std::to_string(conversion.shadersSeen) + " shader(s) (" +
                       std::to_string(conversion.programsTranslated) + " program(s)) across " +
                       std::to_string(filesRewritten) + " serialized file(s); " +
                       std::to_string(conversion.shadersLeftAlone) + " already ran here, " +
                       std::to_string(conversion.shadersRefused) + " left as they were; " +
                       std::to_string(conversion.texturesMarkedReadable) + " of " +
                       std::to_string(conversion.texturesSeen) +
                       " block-compressed texture(s) marked readable (" +
                       std::to_string(conversion.texturesStreamed) + " streamed)";
  return conversion;
}

ShaderScan ScanShaders(std::string const& bundlePath) {
  ShaderScan scan;
  ArchiveHeader header;
  std::vector<DirectoryNode> nodes;
  std::vector<uint8_t> data;
  Result result;

  FileSource source;
  if (!source.Open(bundlePath, result)) {
    scan.message = result.message.empty() ? "bundle unreadable" : result.message;
    return scan;
  }
  std::vector<StorageBlock> blocks;
  if (!ParseArchive(source, header, blocks, nodes, result) ||
      !ReadBlocks(source, header, blocks, data, result)) {
    scan.message = result.message.empty() ? "bundle could not be unpacked" : result.message;
    return scan;
  }

  std::set<int32_t> platforms;
  std::set<int32_t> programTypes;
  for (auto const& node : nodes) {
    if (node.offset >= data.size()) continue;
    size_t const available = static_cast<size_t>(
        std::min<uint64_t>(node.size, data.size() - node.offset));
    auto file = SerializedFileParse::InspectSerializedFile(data.data() + node.offset, available);
    if (!file.isSerializedFile) continue;  // raw .resS payload node, not an error
    scan.serializedFiles++;
    if (scan.unityVersion.empty()) scan.unityVersion = file.unityVersion;
    if (!file.typeTreePresent) scan.typeTreeStripped = true;
    scan.shaderObjects += file.shaderObjectCount;
    for (auto const& shader : file.shaders) {
      if (!shader.name.empty()) scan.shaderNames.push_back(shader.name);
      bool runsHere = false;
      for (int32_t platform : shader.platforms) {
        platforms.insert(platform);
        if (SerializedFileParse::ShaderPlatformRunsOnQuest(platform)) runsHere = true;
      }
      if (runsHere) scan.shadersRunnableOnQuest++;

      // Decompress and split the shader's program store. This is what turns
      // "the platform field says Direct3D" into "here are the N programs, and
      // here is what each one is" -- the difference between knowing conversion
      // is needed and being able to do it.
      auto decoded = SerializedFileParse::DecodeShaderPrograms(
          data.data() + node.offset, available, shader);
      if (!decoded.ok) scan.undecodableShaders++;
      for (auto const& program : decoded.programs) {
        scan.programs++;
        programTypes.insert(program.programType);
        if (SerializedFileParse::GpuProgramIsGlslSource(program.programType)) {
          scan.glslSourcePrograms++;
        } else {
          scan.binaryPrograms++;
        }
      }
    }
    if (scan.message.empty() && !file.message.empty()) scan.message = file.message;
  }

  scan.parsed = scan.serializedFiles > 0;
  scan.platforms.assign(platforms.begin(), platforms.end());
  scan.programTypes.assign(programTypes.begin(), programTypes.end());
  if (!scan.parsed && scan.message.empty()) {
    scan.message = "no SerializedFile found inside the archive";
  }
  return scan;
}

std::string DescribeShaderScan(ShaderScan const& scan) {
  if (!scan.parsed) return "shader scan failed: " + scan.message;
  std::string text = "unity=" + (scan.unityVersion.empty() ? std::string("?") : scan.unityVersion) +
                     " serializedFiles=" + std::to_string(scan.serializedFiles) +
                     " shaders=" + std::to_string(scan.shaderObjects) +
                     " runnableOnQuest=" + std::to_string(scan.shadersRunnableOnQuest) +
                     " platforms=[";
  for (size_t i = 0; i < scan.platforms.size(); i++) {
    if (i != 0) text += ", ";
    text += std::string(SerializedFileParse::ShaderPlatformName(scan.platforms[i])) + "(" +
            std::to_string(scan.platforms[i]) + ")";
  }
  text += "]";
  text += " programs=" + std::to_string(scan.programs) +
          " glslSource=" + std::to_string(scan.glslSourcePrograms) +
          " binary=" + std::to_string(scan.binaryPrograms);
  if (!scan.programTypes.empty()) {
    text += " programTypes=[";
    for (size_t i = 0; i < scan.programTypes.size(); i++) {
      if (i != 0) text += ", ";
      text += std::string(SerializedFileParse::GpuProgramTypeName(scan.programTypes[i]));
    }
    text += "]";
  }
  if (scan.undecodableShaders > 0) {
    text += " undecodableShaders=" + std::to_string(scan.undecodableShaders);
  }
  if (scan.typeTreeStripped) text += " typeTree=stripped";
  if (!scan.message.empty()) text += " note='" + scan.message + "'";
  return text;
}

Result RepackBundle(std::string const& sourcePath, std::string const& destPath) {
  Result result;
  ArchiveHeader header;
  std::vector<DirectoryNode> nodes;
  std::vector<uint8_t> data;
  std::vector<TargetPlatformField> fields;
  if (!LoadAndScan(sourcePath, header, nodes, data, fields, result)) return result;

  int rebuilt = 0;
  for (size_t i = 0; i < nodes.size(); i++) {
    if (nodes[i].offset > data.size() || nodes[i].size > data.size() - nodes[i].offset) {
      result.status = Status::Corrupt;
      result.message = "directory node '" + nodes[i].path + "' points outside the unpacked data";
      return result;
    }
    size_t const available = static_cast<size_t>(nodes[i].size);
    auto rewritten = SerializedFileParse::RewriteSerializedFile(
        data.data() + nodes[i].offset, available, {});
    // A node that is not a serialized file is a raw .resS payload; leave it be.
    if (!rewritten.ok) continue;

    if (!ReplaceNodeData(nodes, data, i, rewritten.data)) {
      result.status = Status::Corrupt;
      result.message = "could not relay the archive around a rebuilt '" + nodes[i].path + "'";
      return result;
    }
    rebuilt++;
  }

  if (rebuilt == 0) {
    result.status = Status::Corrupt;
    result.message = "no SerializedFile inside the archive could be rebuilt";
    return result;
  }

  if (!WriteConverted(destPath, header, nodes, data, result)) return result;
  result.status = Status::Success;
  result.serializedFilesRetargeted = rebuilt;
  result.message = "rebuilt " + std::to_string(rebuilt) + " serialized file(s) through the rewriter";
  return result;
}

Result ConvertToAndroid(std::string const& sourcePath, std::string const& destPath) {
  Result result;
  ArchiveHeader header;
  std::vector<DirectoryNode> nodes;
  std::vector<uint8_t> data;
  std::vector<TargetPlatformField> fields;
  if (!LoadAndScan(sourcePath, header, nodes, data, fields, result)) return result;

  for (auto const& field : fields) {
    if (field.value == kBuildTargetAndroid) continue;
    WriteU32(data.data() + field.offset, static_cast<uint32_t>(kBuildTargetAndroid), field.bigEndian);
    result.serializedFilesRetargeted++;
  }
  if (result.serializedFilesRetargeted == 0) {
    result.status = Status::AlreadyAndroid;
    result.message = "every SerializedFile already targets Android";
    return result;
  }

  if (!WriteConverted(destPath, header, nodes, data, result)) return result;

  result.status = Status::Success;
  result.message = "retargeted " + std::to_string(result.serializedFilesRetargeted) + " of " +
                   std::to_string(result.serializedFilesSeen) + " serialized file(s) from " +
                   result.sourcePlatform + " to Android";
  return result;
}

}
