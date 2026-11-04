#pragma once

#include <cstddef>
#include <istream>
#include <string>

namespace logging {

// Tamper-evident log chain. Every line carries "prev_hash": the hex SHA-256
// of the previous line's exact bytes (without the newline). Editing,
// inserting or deleting a line breaks the link on the line after it. The
// chain carries across rotation, so <log>.1's last line links to <log>'s
// first. It is not a signature: someone with write access can rebuild the
// whole chain, and truncating the newest lines is not detectable from the
// file alone. Lines written before chaining existed are reported as
// unchained, not as tampering.
constexpr const char* kGenesisHash =
    "0000000000000000000000000000000000000000000000000000000000000000";

std::string LineHash(const std::string& line);

// Appends ,"prev_hash":"<prev>" to a serialized JSON object.
std::string ChainLine(const std::string& jsonObjectLine, const std::string& prevHash);

// Returns the hash of the last non-empty line of a file, or "" if the file
// is missing or empty.
std::string LastLineHash(const std::wstring& path);

struct ChainReport {
    size_t lines = 0;
    size_t chained = 0;
    size_t unchained = 0;   // lines with no prev_hash (before chaining started)
    size_t brokenLine = 0;  // 1-based; 0 = intact
    std::string message;
    bool Ok() const { return brokenLine == 0; }
};

// Verifies one file. expectedFirstPrev, if non-empty, is the hash the first
// chained line must link to (the last line of the next-older file).
ChainReport VerifyChain(std::istream& in, const std::string& expectedFirstPrev = "");

}
