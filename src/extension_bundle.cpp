#include "extension_bundle.hpp"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace glint::extension_bundle {
namespace {

enum : int {
    ResourceManifest = 101,
    ResourceBackground = 102,
    ResourceBackgroundCore = 103,
    ResourceOptionsHtml = 104,
    ResourceOptionsJs = 105,
};

struct ResourceSpec {
    int id;
    const wchar_t* filename;
};

constexpr std::array<ResourceSpec, 5> kResources = {{
    {ResourceManifest, L"manifest.json"},
    {ResourceBackground, L"background.js"},
    {ResourceBackgroundCore, L"background-core.js"},
    {ResourceOptionsHtml, L"options.html"},
    {ResourceOptionsJs, L"options.js"},
}};

std::wstring windows_error(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(flags, nullptr, code, 0,
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text = length ? std::wstring(buffer, length)
                               : L"Windows error " + std::to_wstring(code);
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
        text.pop_back();
    }
    return text;
}

void set_error(std::wstring& error, std::wstring_view operation, DWORD code) {
    error.assign(operation);
    error.append(L": ");
    error.append(windows_error(code));
}

bool local_app_data(std::filesystem::path& root, std::wstring& error) {
    const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required != 0) {
        std::wstring value(required, L'\0');
        const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), required);
        if (copied != 0 && copied < required) {
            value.resize(copied);
            root = std::filesystem::path(value);
            return true;
        }
    }

    PWSTR known_folder = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &known_folder);
    if (SUCCEEDED(result) && known_folder) {
        root = std::filesystem::path(known_folder);
        CoTaskMemFree(known_folder);
        return true;
    }
    if (known_folder) CoTaskMemFree(known_folder);
    set_error(error, L"find LocalAppData", HRESULT_CODE(result));
    return false;
}

bool load_resource(HMODULE module, const ResourceSpec& spec, std::vector<std::byte>& bytes,
                   std::wstring& error) {
    // RT_RCDATA is declared as an ANSI resource-name macro in some Windows
    // SDK headers.  Pass the numeric resource type explicitly so the W
    // overload is selected consistently with UNICODE builds.
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(spec.id),
                                         MAKEINTRESOURCEW(10));
    if (!resource) {
        set_error(error, std::wstring(L"find embedded ") + spec.filename, GetLastError());
        return false;
    }
    const HGLOBAL loaded = LoadResource(module, resource);
    if (!loaded) {
        set_error(error, std::wstring(L"load embedded ") + spec.filename, GetLastError());
        return false;
    }
    const DWORD size = SizeofResource(module, resource);
    const auto* data = static_cast<const std::byte*>(LockResource(loaded));
    if (size != 0 && !data) {
        set_error(error, std::wstring(L"read embedded ") + spec.filename, ERROR_INVALID_DATA);
        return false;
    }
    if (size == 0) {
        bytes.clear();
    } else {
        bytes.assign(data, data + size);
    }
    return true;
}

bool matches_file(const std::filesystem::path& path, const std::vector<std::byte>& expected,
                  bool& matches, std::wstring& error) {
    matches = false;
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return true;
        set_error(error, L"read existing extension file", code);
        return false;
    }

    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(handle, &size) != FALSE;
    if (!ok) {
        set_error(error, L"read existing extension file size", GetLastError());
    } else if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) != expected.size()) {
        ok = true;
    } else {
        std::vector<std::byte> actual(expected.size());
        std::size_t offset = 0;
        while (offset < actual.size()) {
            const DWORD chunk = static_cast<DWORD>((std::min)(actual.size() - offset,
                static_cast<std::size_t>(1u << 20)));
            DWORD read = 0;
            if (!ReadFile(handle, actual.data() + offset, chunk, &read, nullptr) || read == 0) {
                set_error(error, L"read existing extension file", GetLastError());
                ok = false;
                break;
            }
            offset += read;
        }
        if (ok) matches = actual == expected;
    }
    CloseHandle(handle);
    return ok;
}

bool write_file_atomically(const std::filesystem::path& target, const std::vector<std::byte>& bytes,
                           int resource_id, std::wstring& error) {
    const std::wstring base = target.wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(resource_id);
    std::filesystem::path temporary;
    HANDLE handle = INVALID_HANDLE_VALUE;
    for (unsigned int attempt = 0; attempt != 100; ++attempt) {
        temporary = base + L"-" + std::to_wstring(attempt);
        handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS) {
            set_error(error, L"create temporary extension file", GetLastError());
            return false;
        }
    }
    if (handle == INVALID_HANDLE_VALUE) {
        set_error(error, L"create temporary extension file", ERROR_FILE_EXISTS);
        return false;
    }

    bool ok = true;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>((std::min)(bytes.size() - offset,
            static_cast<std::size_t>(1u << 20)));
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) || written == 0) {
            set_error(error, L"write temporary extension file", GetLastError());
            ok = false;
            break;
        }
        offset += written;
    }
    if (ok && !FlushFileBuffers(handle)) {
        set_error(error, L"flush temporary extension file", GetLastError());
        ok = false;
    }
    if (!CloseHandle(handle)) {
        if (ok) set_error(error, L"close temporary extension file", GetLastError());
        ok = false;
    }
    if (!ok) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        set_error(error, L"replace extension file", GetLastError());
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

} // namespace

bool ensure_extracted(std::filesystem::path& directory, std::wstring& error) {
    directory.clear();
    error.clear();

    std::filesystem::path local_data;
    if (!local_app_data(local_data, error)) return false;
    directory = local_data / L"Glint" / L"extension";

    std::error_code filesystem_error;
    std::filesystem::create_directories(directory, filesystem_error);
    if (filesystem_error) {
        const auto message = filesystem_error.message();
        error = L"create extension directory: " +
            std::wstring(message.begin(), message.end());
        return false;
    }
    if (!std::filesystem::is_directory(directory, filesystem_error) || filesystem_error) {
        error = L"extension path is not a directory";
        return false;
    }

    const HMODULE module = GetModuleHandleW(nullptr);
    std::array<std::vector<std::byte>, kResources.size()> contents;
    for (std::size_t i = 0; i != kResources.size(); ++i) {
        if (!load_resource(module, kResources[i], contents[i], error)) return false;
    }
    for (std::size_t i = 0; i != kResources.size(); ++i) {
        const auto target = directory / kResources[i].filename;
        bool matches = false;
        if (!matches_file(target, contents[i], matches, error)) return false;
        if (!matches && !write_file_atomically(target, contents[i], kResources[i].id, error)) return false;
    }
    return true;
}

} // namespace glint::extension_bundle
