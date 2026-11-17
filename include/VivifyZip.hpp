#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace Vivify {

// Extracts a zip archive held in memory into `dest` (created if missing).
//
// Written for BeatSaver map zips, which the game has no managed API to open:
// stored and deflated entries only, no encryption, no zip64. Entries whose
// names would land outside `dest` (absolute, or containing "..") are refused
// rather than skipped, since an archive carrying them is not a map.
// Returns false with `error` set on the first problem; files already written
// are left for the caller to discard.
bool ExtractZip(std::span<uint8_t const> data, std::filesystem::path const& dest, std::string& error);

}
