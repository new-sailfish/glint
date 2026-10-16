#include "win_app.hpp"
#include "registration.hpp"

#include "glint/core.hpp"
#include "win_common.hpp"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace glint {
namespace {

constexpr wchar_t kChromeKey[] = L"Software\\Google\\Chrome\\NativeMessagingHosts\\com.glint.host";
constexpr wchar_t kEdgeKey[] = L"Software\\Microsoft\\Edge\\NativeMessagingHosts\\com.glint.host";
constexpr wchar_t kOnboardingKey[] = L"Software\\Glint";
constexpr wchar_t kOnboardingValue[] = L"OnboardingDismissed";

std::wstring module_path() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (count == 0) return {};
        if (count < path.size() - 1) { path.resize(count); return path; }
        path.resize(path.size() * 2);
    }
}

bool read_registry_string(const wchar_t* subkey, std::wstring& value) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    DWORD type = 0;
    DWORD bytes = 0;
    LONG status = RegQueryValueExW(key, nullptr, nullptr, &type, nullptr, &bytes);
    if (status != ERROR_SUCCESS || type != REG_SZ || bytes == 0 || bytes > 65536 || bytes % sizeof(wchar_t) != 0) {
        RegCloseKey(key);
        return false;
    }
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t));
    status = RegQueryValueExW(key, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ || bytes == 0 || bytes % sizeof(wchar_t) != 0) return false;
    const std::size_t count = bytes / sizeof(wchar_t);
    buffer.resize(count);
    if (buffer.back() != L'\0' || std::find(buffer.begin(), buffer.end() - 1, L'\0') != buffer.end() - 1) return false;
    value.assign(buffer.data(), count - 1);
    return !value.empty();
}

bool read_file_limited(const std::filesystem::path& path, std::string& text) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > kMaxMessageBytes) return false;
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    text.resize(static_cast<std::size_t>(size));
    input.read(text.data(), static_cast<std::streamsize>(size));
    return !input.bad() && input.gcount() == static_cast<std::streamsize>(size);
}

bool valid_extension_id_impl(std::wstring_view id) {
    if (id.size() != 32) return false;
    for (const wchar_t character : id) if (character < L'a' || character > L'p') return false;
    return true;
}

std::wstring trim_input(std::wstring_view input) {
    std::size_t first = 0;
    while (first < input.size() && std::iswspace(input[first])) ++first;
    std::size_t last = input.size();
    while (last > first && std::iswspace(input[last - 1])) --last;
    return std::wstring(input.substr(first, last - first));
}

bool parse_origin(std::string_view origin, std::wstring& id) {
    constexpr std::string_view prefix = "chrome-extension://";
    if (origin.size() != prefix.size() + 32 + 1 || origin.compare(0, prefix.size(), prefix) != 0 || origin.back() != '/') return false;
    const std::string_view id_text = origin.substr(prefix.size(), 32);
    for (const char character : id_text) if (character < 'a' || character > 'p') return false;
    id = utf8_to_wide(id_text);
    return valid_extension_id_impl(id);
}

bool same_file_as_current(const std::filesystem::path& manifest_path, std::wstring_view path_text) {
    if (path_text.empty() || path_text.find(L'\0') != std::wstring_view::npos) return false;
    std::filesystem::path candidate{std::wstring(path_text)};
    if (candidate.is_relative()) candidate = manifest_path.parent_path() / candidate;
    const auto current = std::filesystem::path(module_path());
    std::error_code candidate_error;
    std::error_code current_error;
    if (current.empty() || !std::filesystem::is_regular_file(candidate, candidate_error) ||
        !std::filesystem::is_regular_file(current, current_error) || candidate_error || current_error) return false;
    std::error_code equivalent_error;
    return std::filesystem::equivalent(candidate, current, equivalent_error) && !equivalent_error;
}

struct ManifestInfo {
    std::wstring id;
    bool current = false;
};

ManifestInfo inspect_manifest(const std::filesystem::path& manifest_path) {
    ManifestInfo result;
    std::string text;
    if (!read_file_limited(manifest_path, text)) return result;
    try {
        const Json manifest = Json::parse(text);
        if (!manifest.is_object()) return result;
        const Json* name = manifest.find("name");
        const Json* description = manifest.find("description");
        const Json* type = manifest.find("type");
        const Json* path = manifest.find("path");
        const Json* origins = manifest.find("allowed_origins");
        if (!name || !name->is_string() || name->as_string() != "com.glint.host" ||
            !description || !description->is_string() || !type || !type->is_string() || type->as_string() != "stdio" ||
            !path || !path->is_string() || path->as_string().empty() ||
            !origins || !origins->is_array() || origins->as_array().size() != 1) return result;
        std::wstring first_id;
        for (const Json& origin : origins->as_array()) {
            if (!origin.is_string()) return result;
            std::wstring id;
            if (!parse_origin(origin.as_string(), id)) return result;
            if (first_id.empty()) first_id = std::move(id);
        }
        result.id = std::move(first_id);
        result.current = same_file_as_current(manifest_path, utf8_to_wide(path->as_string()));
    } catch (const ProtocolError&) {
        return {};
    }
    return result;
}

void read_browser_registration(const wchar_t* subkey, std::wstring& id, bool& current) {
    std::wstring registry_path;
    if (!read_registry_string(subkey, registry_path)) return;
    const ManifestInfo info = inspect_manifest(std::filesystem::path(registry_path));
    id = info.id;
    current = info.current;
}

bool write_manifest(const std::filesystem::path& path, std::wstring_view executable, std::wstring_view extension_id) {
    const Json manifest = Json::Object{
        {"name", "com.glint.host"},
        {"description", "Glint browser tab search native host"},
        {"path", wide_to_utf8(executable)},
        {"type", "stdio"},
        {"allowed_origins", Json::Array{"chrome-extension://" + wide_to_utf8(extension_id) + "/"}}
    };
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    const auto text = manifest.dump() + "\n";
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
    const LONG status = RegDeleteTreeW(HKEY_CURRENT_USER, subkey);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}

bool remove_manifest(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
    return !error;
}

} // namespace

namespace setup {

bool valid_extension_id(std::wstring_view id) { return valid_extension_id_impl(id); }

RegistrationState read_registration() {
    RegistrationState state;
    read_browser_registration(kChromeKey, state.chrome_id, state.chrome_current);
    read_browser_registration(kEdgeKey, state.edge_id, state.edge_current);
    return state;
}

int register_hosts(std::wstring_view chrome_id, std::wstring_view edge_id, std::wstring& error) {
    error.clear();
    const std::wstring normalized_chrome = trim_input(chrome_id);
    const std::wstring normalized_edge = trim_input(edge_id);
    const bool has_chrome = !normalized_chrome.empty();
    const bool has_edge = !normalized_edge.empty();
    if (!has_chrome && !has_edge) { error = L"请至少填写一个 Chrome 或 Edge 扩展 ID。"; return 2; }
    if ((has_chrome && !valid_extension_id_impl(normalized_chrome)) || (has_edge && !valid_extension_id_impl(normalized_edge))) {
        error = L"扩展 ID 必须是 32 位小写 a-p 字符。未写入任何注册信息。";
        return 2;
    }
    const std::wstring executable = module_path();
    if (executable.empty()) { error = L"无法确定 Glint.exe 的路径。"; return 1; }
    const std::filesystem::path directory = std::filesystem::path(win::local_app_data()) / L"Glint";
    std::error_code create_error;
    std::filesystem::create_directories(directory, create_error);
    if (create_error) { error = L"无法创建注册目录：" + directory.wstring(); return 1; }
    const auto chrome_manifest = directory / L"com.glint.host.chrome.json";
    const auto edge_manifest = directory / L"com.glint.host.edge.json";
    bool success = true;
    if (has_chrome) success = write_manifest(chrome_manifest, executable, normalized_chrome) && set_registry(kChromeKey, chrome_manifest) && success;
    if (has_edge) success = write_manifest(edge_manifest, executable, normalized_edge) && set_registry(kEdgeKey, edge_manifest) && success;
    if (!success) { error = L"Native Messaging 注册失败，请检查当前用户的注册表和注册目录是否可写。"; return 1; }
    return 0;
}

bool onboarding_dismissed() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kOnboardingKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    const LONG status = RegQueryValueExW(key, kOnboardingValue, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && type == REG_DWORD && size == sizeof(value) && value != 0;
}

bool dismiss_onboarding() {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kOnboardingKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    const DWORD value = 1;
    const LONG status = RegSetValueExW(key, kOnboardingValue, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

bool should_show_onboarding(bool dismissed, const RegistrationState& state) {
    return !dismissed && !state.chrome_current && !state.edge_current;
}

} // namespace setup

int run_register(bool unregister_host, const wchar_t* chrome_id, const wchar_t* edge_id) {
    if (unregister_host) {
        const std::filesystem::path directory = std::filesystem::path(win::local_app_data()) / L"Glint";
        const bool chrome_ok = remove_registry(kChromeKey);
        const bool edge_ok = remove_registry(kEdgeKey);
        const bool chrome_file_ok = remove_manifest(directory / L"com.glint.host.chrome.json");
        const bool edge_file_ok = remove_manifest(directory / L"com.glint.host.edge.json");
        if (!chrome_ok || !edge_ok || !chrome_file_ok || !edge_file_ok) {
            std::wcerr << L"Native Messaging 注销失败，请检查当前用户的注册表和注册目录是否可写。\n";
            return 1;
        }
        return 0;
    }
    std::wstring error;
    const int result = setup::register_hosts(chrome_id ? chrome_id : L"", edge_id ? edge_id : L"", error);
    if (result != 0 && !error.empty()) std::wcerr << error << L"\n";
    return result;
}

} // namespace glint
