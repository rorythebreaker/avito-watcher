#include "core/secrets.h"

#include <windows.h>
#include <dpapi.h>

#include "util/strings.h"

namespace core {
namespace {

constexpr const char* kPrefix = "dpapi:";

}  // namespace

std::string encrypt_secret(const std::string& value) {
    if (value.empty() || util::starts_with(value, kPrefix)) return value;

    DATA_BLOB input;
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(value.data()));
    input.cbData = static_cast<DWORD>(value.size());

    DATA_BLOB output = {};
    if (!CryptProtectData(&input, L"AvitoWatcher", nullptr, nullptr, nullptr, 0, &output)) {
        return value;  // no encryption available, keep the plain value usable
    }
    std::string encoded = kPrefix + util::base64_encode(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return encoded;
}

std::string decrypt_secret(const std::string& value) {
    if (!util::starts_with(value, kPrefix)) return value;

    std::vector<unsigned char> raw = util::base64_decode(value.substr(std::strlen(kPrefix)));
    if (raw.empty()) return {};

    DATA_BLOB input;
    input.pbData = raw.data();
    input.cbData = static_cast<DWORD>(raw.size());

    DATA_BLOB output = {};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output)) {
        return {};
    }
    std::string result(reinterpret_cast<char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return result;
}

}  // namespace core
