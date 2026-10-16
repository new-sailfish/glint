#include "win_app.hpp"

#include "glint/core.hpp"
#include "win_common.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace glint {
namespace {

std::wstring module_path() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (count == 0) return {};
        if (count < path.size() - 1) { path.resize(count); return path; }
        path.resize(path.size() * 2);
    }
}

bool write_manifest(const std::filesystem::path& path, const std::wstring& exe, const std::wstring& extension_id) {
    if (extension_id.empty()) return false;
    const auto json = Json::Object{
        {"name", "com.glint.host"},
        {"description", "Glint browser tab search native host"},
        {"path", wide_to_utf8(exe)},
        {"type", "stdio"},
        {"allowed_origins", Json::Array{"chrome-extension://" + wide_to_utf8(extension_id) + "/"}}
    };
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    const auto text = Json(json).dump() + "\n";
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return output.good();
}

bool set_registry(const wchar_t* subkey, const std::filesystem::path& manifest) {
    HKEY key = nullptr;
    const LONG status = RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) return false;
    const std::wstring value = manifest.wstring();
    const LONG write_status = RegSetValueExW(key, nullptr, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return write_status == ERROR_SUCCESS;
}

bool remove_registry(const wchar_t* subkey) {
    return RegDeleteTreeW(HKEY_CURRENT_USER, subkey) == ERROR_SUCCESS || GetLastError() == ERROR_FILE_NOT_FOUND;
}

} // namespace

int run_register(bool unregister_host, const wchar_t* chrome_id, const wchar_t* edge_id) {
    const std::filesystem::path directory = std::filesystem::path(win::local_app_data()) / L"Glint";
    const auto chrome_manifest = directory / L"com.glint.host.chrome.json";
    const auto edge_manifest = directory / L"com.glint.host.edge.json";
    constexpr wchar_t chrome_key[] = L"Software\\Google\\Chrome\\NativeMessagingHosts\\com.glint.host";
    constexpr wchar_t edge_key[] = L"Software\\Microsoft\\Edge\\NativeMessagingHosts\\com.glint.host";

    if (unregister_host) {
        const bool chrome_ok = remove_registry(chrome_key);
        const bool edge_ok = remove_registry(edge_key);
        std::error_code ignored;
        std::filesystem::remove(chrome_manifest, ignored);
        std::filesystem::remove(edge_manifest, ignored);
        return chrome_ok && edge_ok ? 0 : 1;
    }

    const std::wstring exe = module_path();
    if (exe.empty() || (!chrome_id || !*chrome_id) && (!edge_id || !*edge_id)) {
        std::wcerr << L"Usage: Glint.exe --register [--chrome-id ID] [--edge-id ID]\n";
        return 2;
    }
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) { std::wcerr << L"Cannot create " << directory << L"\n"; return 1; }
    bool success = true;
    if (chrome_id && *chrome_id) success = write_manifest(chrome_manifest, exe, chrome_id) && set_registry(chrome_key, chrome_manifest) && success;
    if (edge_id && *edge_id) success = write_manifest(edge_manifest, exe, edge_id) && set_registry(edge_key, edge_manifest) && success;
    if (!success) std::wcerr << L"Native Messaging registration failed.\n";
    return success ? 0 : 1;
}

} // namespace glint
