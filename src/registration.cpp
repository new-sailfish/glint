#include "win_app.hpp"
#include "registration.hpp"
#include "extension_identity.hpp"
#include "bridge_installation.hpp"

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
    return bridge_installation::is_current_bridge(candidate);
}

struct ManifestInfo {
    std::wstring id;
    bool current = false;
};

std::vector<std::wstring> bundled_ids(std::wstring& error) {
    // This remains the legacy directory's identity. New bundles are extracted
    // into browser-extension with a fixed key; no new path-ID origin is needed.
    const auto directory = std::filesystem::path(win::local_app_data()) / L"Glint" / L"extension";
    const auto path_id = setup::legacy_unpacked_id(directory, error);
    if (path_id.empty()) return {};
    std::vector<std::wstring> ids{utf8_to_wide(setup::kBundledExtensionId)};
    if (path_id != ids.front()) ids.push_back(path_id);
    return ids;
}

ManifestInfo inspect_manifest_text(const std::filesystem::path& manifest_path, const std::string& text) {
    ManifestInfo result;
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
            !origins || !origins->is_array() || origins->as_array().empty() || origins->as_array().size() > 2) return result;
        std::vector<std::wstring> ids;
        for (const Json& origin : origins->as_array()) {
            if (!origin.is_string()) return result;
            std::wstring id;
            if (!parse_origin(origin.as_string(), id)) return result;
            ids.push_back(std::move(id));
        }
        if (ids.size() == 2) {
            std::wstring error;
            auto expected = bundled_ids(error);
            if (expected.size() != 2) return result;
            std::sort(expected.begin(), expected.end());
            auto sorted = ids;
            std::sort(sorted.begin(), sorted.end());
            if (sorted != expected) return result;
            result.id = utf8_to_wide(setup::kBundledExtensionId);
        } else result.id = ids.front();
        result.current = same_file_as_current(manifest_path, utf8_to_wide(path->as_string()));
    } catch (const ProtocolError&) {
        return {};
    }
    return result;
}

ManifestInfo inspect_manifest(const std::filesystem::path& manifest_path) {
    std::string text;
    return read_file_limited(manifest_path, text) ? inspect_manifest_text(manifest_path, text) : ManifestInfo{};
}

void read_browser_registration(const wchar_t* subkey, std::wstring& id, bool& current) {
    std::wstring registry_path;
    if (!read_registry_string(subkey, registry_path)) return;
    const ManifestInfo info = inspect_manifest(std::filesystem::path(registry_path));
    id = info.id;
    current = info.current;
}

bool write_manifest(const std::filesystem::path& path, std::wstring_view executable, const std::vector<std::wstring>& ids) {
    Json::Array origins;
    for (const auto& id : ids) origins.emplace_back("chrome-extension://" + wide_to_utf8(id) + "/");
    const Json manifest = Json::Object{
        {"name", "com.glint.host"},
        {"description", "Glint browser tab search native host"},
        {"path", wide_to_utf8(executable)},
        {"type", "stdio"},
        {"allowed_origins", std::move(origins)}
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

int register_origin_sets(const std::vector<std::wstring>& chrome_ids, const std::vector<std::wstring>& edge_ids, std::wstring& error) {
    std::filesystem::path bridge;
    if (!bridge_installation::ensure_current(bridge, error)) return 1;
    const std::wstring executable = bridge.wstring();
    const std::filesystem::path directory = std::filesystem::path(win::local_app_data()) / L"Glint";
    std::error_code create_error;
    std::filesystem::create_directories(directory, create_error);
    if (create_error) { error = L"无法创建注册目录：" + directory.wstring(); return 1; }
    const auto chrome_manifest = directory / L"com.glint.host.chrome.json";
    const auto edge_manifest = directory / L"com.glint.host.edge.json";
    bool success = true;
    if (!chrome_ids.empty()) success = write_manifest(chrome_manifest, executable, chrome_ids) && set_registry(kChromeKey, chrome_manifest) && success;
    if (!edge_ids.empty()) success = write_manifest(edge_manifest, executable, edge_ids) && set_registry(kEdgeKey, edge_manifest) && success;
    if (!success) { error = L"Native Messaging 注册失败，请检查当前用户的注册表和注册目录是否可写。"; return 1; }
    return 0;
}

} // namespace

namespace setup {

bool migrate_existing_hosts(std::wstring& error) {
    error.clear();
    std::filesystem::path bridge;
    for (const auto key : {kChromeKey, kEdgeKey}) {
        std::wstring registry_path;
        if (!read_registry_string(key, registry_path)) continue;
        const std::filesystem::path manifest_path(registry_path);
        std::string text;
        if (!read_file_limited(manifest_path, text)) continue;
        const auto existing = inspect_manifest_text(manifest_path, text);
        // Never infer extra origins or configure a browser that was absent.
        // Invalid manifests require explicit repair through the normal setup UI.
        if (existing.id.empty() || existing.current) continue;
        if (bridge.empty() && !bridge_installation::ensure_current(bridge, error)) return false;
        Json manifest;
        try { manifest = Json::parse(text); }
        catch (const ProtocolError&) { error = L"现有浏览器通信桥配置已变化，请重新连接浏览器。"; return false; }
        manifest["path"] = wide_to_utf8(bridge.wstring());
        const auto temporary = manifest_path.parent_path() / (manifest_path.filename().wstring() +
            L".migrate-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            const auto updated = manifest.dump() + "\n";
            output.write(updated.data(), static_cast<std::streamsize>(updated.size()));
            output.close();
            if (!output) {
                DeleteFileW(temporary.c_str());
                error = L"无法保存浏览器通信桥配置：" + manifest_path.wstring();
                return false;
            }
        }
        if (!MoveFileExW(temporary.c_str(), manifest_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD failure = GetLastError();
            DeleteFileW(temporary.c_str());
            error = L"无法更新浏览器通信桥配置：" + win::last_error_text(failure);
            return false;
        }
    }
    return true;
}

bool register_bundled_host(bool edge, std::wstring& error) {
    error.clear();
    const auto ids = bundled_ids(error);
    if (ids.empty()) return false;
    return edge ? register_origin_sets({}, ids, error) == 0 : register_origin_sets(ids, {}, error) == 0;
}

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
    return register_origin_sets(has_chrome ? std::vector<std::wstring>{normalized_chrome} : std::vector<std::wstring>{},
                                has_edge ? std::vector<std::wstring>{normalized_edge} : std::vector<std::wstring>{}, error);
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
