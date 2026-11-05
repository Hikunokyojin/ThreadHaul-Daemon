#include "LogChain.h"

#include <fstream>

#include <nlohmann/json.hpp>

#include "SecureChannel.h"

namespace logging {

std::string LineHash(const std::string& line) {
    return securechannel::ToHex(securechannel::Sha256(line));
}

std::string ChainLine(const std::string& line, const std::string& prevHash) {
    if (line.size() < 2 || line.back() != '}') return line;
    std::string out = line.substr(0, line.size() - 1);
    out += ",\"prev_hash\":\"" + prevHash + "\"}";
    return out;
}

std::string LastLineHash(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    // Read the tail; log lines are far shorter than 64 KiB.
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size <= 0) return "";
    const std::streamoff start = size > 65536 ? size - 65536 : 0;
    f.seekg(start);
    std::string tail(static_cast<size_t>(size - start), '\0');
    f.read(tail.data(), static_cast<std::streamsize>(tail.size()));
    while (!tail.empty() && (tail.back() == '\n' || tail.back() == '\r')) tail.pop_back();
    if (tail.empty()) return "";
    const size_t nl = tail.find_last_of('\n');
    return LineHash(nl == std::string::npos ? tail : tail.substr(nl + 1));
}

ChainReport VerifyChain(std::istream& in, const std::string& expectedFirstPrev) {
    ChainReport r;
    std::string line, previous;
    bool havePrevious = false, seenChained = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        ++r.lines;
        const auto j = nlohmann::json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            r.brokenLine = r.lines;
            r.message = "line is not a JSON object";
            return r;
        }
        const auto it = j.find("prev_hash");
        if (it == j.end() || !it->is_string()) {
            if (seenChained) {
                r.brokenLine = r.lines;
                r.message = "prev_hash missing after the chain started";
                return r;
            }
            ++r.unchained;
        } else {
            const std::string prev = it->get<std::string>();
            const std::string want = havePrevious ? LineHash(previous) : expectedFirstPrev;
            if (!want.empty() && prev != want) {
                r.brokenLine = r.lines;
                r.message = havePrevious
                    ? "prev_hash does not match the line before it (edited, inserted or removed line)"
                    : "first line does not link to the end of the older rotated file";
                return r;
            }
            seenChained = true;
            ++r.chained;
        }
        previous = line;
        havePrevious = true;
    }
    r.message = r.chained == 0 ? "no chained lines" : "chain intact";
    return r;
}

}
