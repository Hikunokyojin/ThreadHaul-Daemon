#pragma once

#include <string>

namespace daemon {

// Config tamper protection. The installer locks config.json down to
// SYSTEM + Administrators (write) and the service account (read). At every
// load the daemon checks that this is still true and records the file's
// SHA-256 so any change is visible in the log.

// True if Everyone, Users, Authenticated Users or Interactive can write,
// delete or re-permission the file (or it has a NULL DACL). `who` names the
// first offending group. Returns false if the ACL cannot be read.
bool IsWritableByNonAdmins(const std::wstring& path, std::wstring& who);

// Lowercase hex SHA-256 of the file, or empty on error.
std::string Sha256File(const std::wstring& path);

}
