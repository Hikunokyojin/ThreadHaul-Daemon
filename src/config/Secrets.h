#pragma once

#include <string>

namespace config {

// Secrets in the config file may be stored encrypted with Windows DPAPI as
// "dpapi:<base64>". Encryption uses machine scope, so the service can read
// them under any account, but the value is useless on another computer or
// in a copied config. Produce one with "ThreadHaulDaemon.exe protect-secret".
// The config file's ACL still controls who on this machine can read it.
constexpr const char* kSecretPrefix = "dpapi:";

bool IsProtectedSecret(const std::string& value);

// Returns "dpapi:<base64>", or "" on failure.
std::string ProtectSecret(const std::string& plaintext);

// Plain values pass through unchanged. A "dpapi:" value is decrypted;
// returns false (and sets error) if it is malformed or from another machine.
bool UnprotectSecret(const std::string& value, std::string& plaintext, std::string& error);

}
