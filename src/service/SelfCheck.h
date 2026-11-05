#pragma once

#include <string>

namespace daemon {

// "ThreadHaulDaemon.exe selfcheck": inspects an installation and prints one
// PASS / WARN / FAIL line per check, with what to do about each problem.
// Read-only. Returns 0 if nothing failed (warnings allowed), 1 otherwise.
int RunSelfCheck(const std::wstring& configPath);

}
