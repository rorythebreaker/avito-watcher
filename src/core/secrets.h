// Encryption of the Telegram token and the mail password.
//
// The ciphertext is bound to the current Windows account through DPAPI, so the
// settings file cannot be read on another machine or by another user. When
// DPAPI is unavailable the value is stored as-is and the app keeps working.
#pragma once

#include <string>

namespace core {

std::string encrypt_secret(const std::string& value);
std::string decrypt_secret(const std::string& value);

}  // namespace core
