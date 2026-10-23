#include "language.hpp"
#include "extension_identity.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <bit>
#include <limits>

#pragma comment(lib, "bcrypt.lib")

namespace glint::setup {

std::wstring legacy_unpacked_id(const std::filesystem::path& directory, std::wstring& error) {
    error.clear();
    auto text = directory.native();
    if (text.empty() || text.find(L'\0') != std::wstring::npos || !directory.is_absolute()) {
        error = i18n::text(L"The extension folder must be an absolute path without null characters.", L"扩展目录必须是完整的绝对路径，且不能包含空字符。");
        return {};
    }

    // Match Chromium's Windows FilePath hashing: normalize only an ASCII drive
    // letter, leaving all other characters and their UTF-16 code units intact.
    // The directory selector supplies a directory without trailing separators.
    const auto root_length = directory.root_name().native().size() + 1;
    while (text.size() > root_length && (text.back() == L'\\' || text.back() == L'/')) text.pop_back();
    if (text.size() >= 2 && text[0] >= L'a' && text[0] <= L'z' && text[1] == L':') text[0] -= L'a' - L'A';

    static_assert(sizeof(wchar_t) == 2 && std::endian::native == std::endian::little,
                  "Chromium's Windows path identity hashes UTF-16LE code units");
    if (text.size() > (std::numeric_limits<ULONG>::max)() / sizeof(wchar_t)) {
        error = i18n::text(L"The extension path is too long to calculate its compatibility identity.", L"扩展目录路径过长，无法计算兼容身份。");
        return {};
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    std::array<UCHAR, 32> digest{};
    if (status >= 0) {
        status = BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(text.data()),
                            static_cast<ULONG>(text.size() * sizeof(wchar_t)),
                            digest.data(), static_cast<ULONG>(digest.size()));
    }
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) {
        error = i18n::text(L"Could not calculate the extension path SHA-256 identity (Windows NTSTATUS ", L"无法计算扩展路径的 SHA-256 身份（Windows NTSTATUS ") +
            std::to_wstring(static_cast<unsigned long>(status)) + i18n::text(L").", L"）。");
        return {};
    }

    std::wstring result;
    result.reserve(32);
    for (std::size_t index = 0; index < 16; ++index) {
        result += static_cast<wchar_t>(L'a' + (digest[index] >> 4));
        result += static_cast<wchar_t>(L'a' + (digest[index] & 15));
    }
    return result;
}

} // namespace glint::setup
