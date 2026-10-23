#include "language.hpp"
#include "bridge_installation.hpp"
#include "win_common.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <atomic>
#include <system_error>

#pragma comment(lib, "bcrypt.lib")

namespace glint::bridge_installation {
namespace {
using Digest = std::array<UCHAR, 32>;

struct ReadFileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~ReadFileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

struct HashState {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    ~HashState() {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    }
};

std::filesystem::path module_path(std::wstring& error) {
    std::wstring path(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count >= path.size()) {
        error = i18n::text(L"Could not determine the full path to Glint.exe.", L"无法确定 Glint.exe 的完整路径。");
        return {};
    }
    path.resize(count);
    return path;
}

bool file_digest(const std::filesystem::path& path, Digest& digest, std::wstring& error) {
    ReadFileHandle input{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    if (input.value == INVALID_HANDLE_VALUE) {
        error = i18n::text(L"Could not read the bridge file: ", L"无法读取通信桥文件：") + path.wstring() + i18n::text(L" (", L"（") + win::last_error_text() + i18n::text(L").", L"）。");
        return false;
    }
    HashState state;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&state.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0) status = BCryptCreateHash(state.algorithm, &state.hash, nullptr, 0, nullptr, 0, 0);
    std::array<UCHAR, 64 * 1024> buffer{};
    bool nonempty = false;
    while (status >= 0) {
        DWORD bytes = 0;
        if (!ReadFile(input.value, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr)) {
            error = i18n::text(L"Could not read the bridge file: ", L"读取通信桥文件失败：") + win::last_error_text();
            return false;
        }
        if (!bytes) break;
        nonempty = true;
        status = BCryptHashData(state.hash, buffer.data(), bytes, 0);
    }
    if (status >= 0) status = BCryptFinishHash(state.hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    if (status < 0 || !nonempty) {
        error = i18n::text(L"Could not verify the SHA-256 hash of the bridge file.", L"无法验证通信桥文件的 SHA-256 内容。");
        return false;
    }
    return true;
}

std::wstring digest_text(const Digest& digest) {
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(64);
    for (const UCHAR byte : digest) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

bool current_identity(std::filesystem::path& source, std::filesystem::path& destination,
    Digest& digest, std::wstring& error) {
    error.clear();
    source = module_path(error);
    if (source.empty() || !file_digest(source, digest, error)) return false;
    const std::filesystem::path local_data(win::local_app_data());
    if (!local_data.is_absolute() || local_data.native().find(L'\0') != std::wstring::npos) {
        error = i18n::text(L"Could not locate the local app data folder. The bridge was not installed.", L"无法确定当前用户的本地应用数据目录，未安装通信桥。");
        return false;
    }
    destination = local_data / L"Glint" / L"bridge" / digest_text(digest) / L"GlintBridge.exe";
    return true;
}

bool same_absolute_path(std::filesystem::path left, std::filesystem::path right) {
    if (!left.is_absolute() || left.native().find(L'\0') != std::wstring::npos) return false;
    left = left.lexically_normal(); left.make_preferred();
    right = right.lexically_normal(); right.make_preferred();
    const auto& a = left.native();
    const auto& b = right.native();
    return a.size() == b.size() && CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
        b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool matches_digest(const std::filesystem::path& path, const Digest& expected) {
    Digest found{};
    std::wstring ignored;
    return file_digest(path, found, ignored) && found == expected;
}
} // namespace

std::filesystem::path path_for_current_executable(std::wstring& error) {
    std::filesystem::path source, destination;
    Digest digest{};
    return current_identity(source, destination, digest, error) ? destination : std::filesystem::path{};
}

bool ensure_current(std::filesystem::path& path, std::wstring& error) {
    path.clear();
    std::filesystem::path source, destination;
    Digest digest{};
    if (!current_identity(source, destination, digest, error)) return false;
    if (matches_digest(destination, digest)) { path = destination; return true; }
    std::error_code directory_error;
    std::filesystem::create_directories(destination.parent_path(), directory_error);
    if (directory_error) { error = i18n::text(L"Could not create the bridge installation folder: ", L"无法创建通信桥安装目录：") + destination.parent_path().wstring(); return false; }

    static std::atomic<unsigned long long> sequence = 0;
    const auto temporary = destination.parent_path() / (L"GlintBridge-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence++) + L".tmp");
    if (!CopyFileW(source.c_str(), temporary.c_str(), TRUE)) {
        error = i18n::text(L"Could not install the bridge: ", L"无法安装通信桥：") + win::last_error_text();
        return false;
    }
    const auto remove_temporary = [&] { DeleteFileW(temporary.c_str()); };
    if (!matches_digest(temporary, digest)) {
        remove_temporary();
        error = i18n::text(L"The bridge copy failed verification. Installed files were not changed.", L"通信桥副本内容验证失败，未更新安装文件。");
        return false;
    }
    // Another startup may have installed this identical version concurrently.
    if (matches_digest(destination, digest)) {
        remove_temporary();
        path = destination;
        return true;
    }
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD failure = GetLastError();
        remove_temporary();
        // An identical concurrent installation is usable even if held open.
        if (matches_digest(destination, digest)) { path = destination; return true; }
        error = i18n::text(L"Could not update the bridge file: ", L"无法更新通信桥文件：") + win::last_error_text(failure);
        return false;
    }
    if (!matches_digest(destination, digest)) {
        error = i18n::text(L"The installed bridge failed verification.", L"已安装的通信桥内容验证失败。");
        return false;
    }
    path = destination;
    return true;
}

bool is_current_bridge(const std::filesystem::path& path) {
    std::filesystem::path source, destination;
    Digest digest{};
    std::wstring ignored;
    return current_identity(source, destination, digest, ignored) &&
        same_absolute_path(path, destination) && matches_digest(path, digest);
}

bool is_installed_bridge(const std::filesystem::path& path) {
    const std::filesystem::path local_data(win::local_app_data());
    if (!local_data.is_absolute()) return false;
    const auto version = path.parent_path().filename().wstring();
    if (version.size() != 64) return false;
    Digest expected{};
    const auto nibble = [](wchar_t character) -> int {
        if (character >= L'0' && character <= L'9') return character - L'0';
        if (character >= L'a' && character <= L'f') return character - L'a' + 10;
        if (character >= L'A' && character <= L'F') return character - L'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const int high = nibble(version[2 * i]), low = nibble(version[2 * i + 1]);
        if (high < 0 || low < 0) return false;
        expected[i] = static_cast<UCHAR>((high << 4) | low);
    }
    const auto canonical = local_data / L"Glint" / L"bridge" / version / L"GlintBridge.exe";
    return same_absolute_path(path, canonical) && matches_digest(path, expected);
}
} // namespace glint::bridge_installation
