#pragma once

// The few Win32 type names that ThreadHaul's shared engine headers use.
// Windows gets the real definitions; Linux gets same-sized stand-ins, so the
// headers, the state machine and the actuator manager compile unchanged.

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdint>

using DWORD = std::uint32_t;
using ULONG = unsigned long;
using BOOL = int;
using HANDLE = void*;
using DWORD_PTR = std::uintptr_t;

// Linux code fills this with 100 ns units, like Windows, so CpuMath is shared.
struct FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
};
#endif
