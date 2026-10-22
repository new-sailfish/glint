#include "registration.hpp"
#include "extension_identity.hpp"
#include "bridge_installation.hpp"
#include "win_app.hpp"

#include "glint/core.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <atomic>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kChromeId[] = L"abcdefghijklmnopabcdefghijklmnop";
constexpr wchar_t kEdgeId[] = L"ponmlkjihgfedcbaponmlkjihgfedcba";
constexpr wchar_t kChromeKey[] = L"Software\\Google\\Chrome\\NativeMessagingHosts\\com.glint.host";
constexpr wchar_t kEdgeKey[] = L"Software\\Microsoft\\Edge\\NativeMessagingHosts\\com.glint.host";

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::wstring environment_value(const wchar_t* name) {
    const DWORD count = GetEnvironmentVariableW(name, nullptr, 0);
    if (!count) return {};
    std::wstring value(count, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), count);
    require(copied != 0 && copied < count, "read environment variable");
    value.resize(copied);
    return value;
}

// Registry redirection is process-wide, so these tests deliberately run serially
// and call the registration helpers directly instead of spawning Glint.exe.
class Sandbox {
public:
    Sandbox() {
        static std::atomic<unsigned long long> sequence = 0;
        const auto suffix = std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence.fetch_add(1));
        registry_path_ = L"Software\\Glint.RegistrationTests." + suffix;
        directory_ = std::filesystem::temp_directory_path() / (L"Glint-registration-tests-" + suffix);
        { std::error_code create_error; const bool made = std::filesystem::create_directory(directory_, create_error); if (!made) { throw std::runtime_error(("create unique test directory: " + directory_.string() + " error=" + create_error.message()).c_str()); } }
        original_local_app_data_ = environment_value(L"LOCALAPPDATA");
        local_app_data_existed_ = !original_local_app_data_.empty() || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
        require(RegCreateKeyExW(HKEY_CURRENT_USER, registry_path_.c_str(), 0, nullptr, 0,
                               KEY_ALL_ACCESS, nullptr, &root_, nullptr) == ERROR_SUCCESS, "create test registry root");
        require(RegOverridePredefKey(HKEY_CURRENT_USER, root_) == ERROR_SUCCESS, "redirect HKCU for test");
        redirected_ = true;
        require(SetEnvironmentVariableW(L"LOCALAPPDATA", directory_.c_str()) != FALSE, "redirect LOCALAPPDATA for test");
    }

    ~Sandbox() {
        if (redirected_) RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        if (root_) RegCloseKey(root_);
        if (!registry_path_.empty()) RegDeleteTreeW(HKEY_CURRENT_USER, registry_path_.c_str());
        SetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data_existed_ ? original_local_app_data_.c_str() : nullptr);
        // Only remove the unique directory successfully created by this fixture.
        const auto name = directory_.filename().wstring();
        if (name.starts_with(L"Glint-registration-tests-") && directory_.parent_path() == std::filesystem::temp_directory_path()) {
            std::error_code ignored;
            std::filesystem::remove_all(directory_, ignored);
        }
    }

    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;

    const std::filesystem::path& directory() const { return directory_; }

private:
    HKEY root_ = nullptr;
    bool redirected_ = false;
    bool local_app_data_existed_ = false;
    std::wstring registry_path_;
    std::filesystem::path directory_;
    std::wstring original_local_app_data_;
};

std::filesystem::path registered_manifest(const wchar_t* subkey) {
    DWORD size = 0;
    require(RegGetValueW(HKEY_CURRENT_USER, subkey, nullptr, RRF_RT_REG_SZ, nullptr, nullptr, &size) == ERROR_SUCCESS,
            "read native host manifest registry size");
    std::wstring path(size / sizeof(wchar_t), L'\0');
    require(RegGetValueW(HKEY_CURRENT_USER, subkey, nullptr, RRF_RT_REG_SZ, nullptr, path.data(), &size) == ERROR_SUCCESS,
            "read native host manifest registry path");
    if (!path.empty() && path.back() == L'\0') path.pop_back();
    return path;
}

glint::Json load_manifest(const wchar_t* subkey) {
    std::ifstream input(registered_manifest(subkey), std::ios::binary);
    require(input.good(), "open registered manifest");
    const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return glint::Json::parse(text);
}

void replace_manifest(const wchar_t* subkey, const glint::Json& manifest) {
    std::ofstream output(registered_manifest(subkey), std::ios::binary | std::ios::trunc);
    output << manifest.dump();
    require(output.good(), "rewrite test manifest");
}

std::wstring current_executable() {
    std::vector<wchar_t> path(32768);
    const DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    require(count != 0 && count < path.size(), "read current executable path");
    return std::wstring(path.data(), count);
}

void validation_and_no_partial_update() {
    Sandbox sandbox;
    require(glint::setup::valid_extension_id(kChromeId), "accept 32 lowercase a-p characters");
    require(!glint::setup::valid_extension_id(L""), "reject empty ID");
    require(!glint::setup::valid_extension_id(L"abcdefghijklmnopabcdefghijklmno"), "reject short ID");
    require(!glint::setup::valid_extension_id(L"abcdefghijklmnopabcdefghijklmnopq"), "reject long ID");
    require(!glint::setup::valid_extension_id(L"Abcdefghijklmnopabcdefghijklmnop"), "reject uppercase ID");
    require(!glint::setup::valid_extension_id(L"qbcdefghijklmnopabcdefghijklmnop"), "reject character outside a-p");
    require(!glint::setup::valid_extension_id(L" abcdefghijklmnopabcdefghijklmnop"), "reject whitespace in ID");

    const auto initial = glint::setup::read_registration();
    require(initial.chrome_id.empty() && initial.edge_id.empty() && !initial.chrome_current && !initial.edge_current,
            "fresh registry has no registered browser");
    std::wstring error;
    require(glint::setup::register_hosts(kChromeId, L"invalid", error) != 0 && !error.empty(), "report invalid Edge ID");
    require(!std::filesystem::exists(sandbox.directory() / L"Glint"), "validate both IDs before creating files");
    require(glint::setup::read_registration().chrome_id.empty(), "invalid Edge ID must not partially register Chrome");
    require(glint::setup::register_hosts(L"", L"", error) != 0 && !error.empty(), "require at least one browser");

    require(glint::setup::register_hosts(kChromeId, L"", error) == 0 && error.empty(), "register valid Chrome ID");
    require(glint::setup::register_hosts(kEdgeId, L"invalid", error) != 0, "reject invalid replacement before writes");
    require(glint::setup::read_registration().chrome_id == kChromeId, "invalid replacement preserves existing manifest");
    require(glint::setup::register_hosts(L" \t", kEdgeId, error) == 0 && error.empty(), "treat whitespace-only Chrome input as empty");
    const auto whitespace_state = glint::setup::read_registration();
    require(whitespace_state.chrome_id == kChromeId && whitespace_state.edge_id == kEdgeId,
            "whitespace-only browser input preserves the other registration");
}

void independent_browser_registration() {
    Sandbox sandbox;
    std::wstring error;
    require(glint::setup::register_hosts(kChromeId, L"", error) == 0, "register Chrome independently");
    auto state = glint::setup::read_registration();
    require(state.chrome_id == kChromeId && state.chrome_current && state.edge_id.empty() && !state.edge_current,
            "Chrome is current without Edge");
    const auto chrome_manifest = load_manifest(kChromeKey).dump();
    require(glint::setup::register_hosts(L"", kEdgeId, error) == 0, "register Edge independently");
    state = glint::setup::read_registration();
    require(state.chrome_id == kChromeId && state.edge_id == kEdgeId && state.chrome_current && state.edge_current,
            "separate registrations preserve the other browser");
    require(load_manifest(kChromeKey).dump() == chrome_manifest, "empty Chrome input leaves existing Chrome manifest unchanged");

    for (const auto& [key, id] : {std::pair{kChromeKey, kChromeId}, std::pair{kEdgeKey, kEdgeId}}) {
        const auto manifest = load_manifest(key);
        require(manifest.at("name").as_string() == "com.glint.host", "manifest native host name");
        require(manifest.at("type").as_string() == "stdio", "manifest host transport");
        const std::filesystem::path bridge{glint::utf8_to_wide(manifest.at("path").as_string())};
        require(bridge.filename() == L"GlintBridge.exe" && bridge.parent_path().filename().wstring().size() == 64 &&
            bridge.parent_path().parent_path() == sandbox.directory() / L"Glint" / L"bridge",
            "manifest targets content-addressed GlintBridge executable instead of desktop");
        const auto& origins = manifest.at("allowed_origins").as_array();
        require(origins.size() == 1 && origins[0].as_string() == "chrome-extension://" + glint::wide_to_utf8(id) + "/",
                "manifest allows only requested extension origin");
        require(registered_manifest(key).parent_path() == sandbox.directory() / L"Glint", "manifest uses isolated LOCALAPPDATA");
    }
}

void bundled_registration_only_changes_selected_browser() {
    const auto bundled_id = glint::utf8_to_wide(glint::setup::kBundledExtensionId);
    require(glint::setup::valid_extension_id(bundled_id), "bundled extension ID is valid");
    for (const bool edge : {false, true}) {
        Sandbox sandbox;
        std::wstring error;
        require(glint::setup::register_hosts(kChromeId, kEdgeId, error) == 0, "prepare custom registrations");
        const wchar_t* selected_key = edge ? kEdgeKey : kChromeKey;
        const wchar_t* other_key = edge ? kChromeKey : kEdgeKey;
        const auto other_path = registered_manifest(other_key);
        const auto other_manifest = load_manifest(other_key).dump();
        const auto other_timestamp = std::filesystem::last_write_time(other_path);

        error = L"old error";
        require(glint::setup::register_bundled_host(edge, error) && error.empty(), "register bundled host for selected browser");
        const auto state = glint::setup::read_registration();
        require((edge ? state.edge_id : state.chrome_id) == bundled_id, "selected browser uses fixed bundled ID");
        require((edge ? state.edge_current : state.chrome_current), "bundled host targets current executable");
        require((edge ? state.chrome_id : state.edge_id) == (edge ? kChromeId : kEdgeId), "other browser retains custom ID");
        require(registered_manifest(other_key) == other_path && load_manifest(other_key).dump() == other_manifest,
                "other browser retains registry path and manifest contents");
        require(std::filesystem::last_write_time(other_path) == other_timestamp, "other browser manifest is not rewritten");
        const auto manifest = load_manifest(selected_key);
        const auto& origins = manifest.at("allowed_origins").as_array();
        std::wstring identity_error;
        const auto path_id = glint::setup::legacy_unpacked_id(sandbox.directory() / L"Glint" / L"extension", identity_error);
        require(!path_id.empty() && identity_error.empty(), "calculate known bundle directory identity");
        require(origins.size() == 2 && origins[0].as_string() ==
                std::string("chrome-extension://") + glint::setup::kBundledExtensionId + "/" &&
                origins[1].as_string() == "chrome-extension://" + glint::wide_to_utf8(path_id) + "/",
                "bundled manifest allows only public-key and stable bundle path identities");
        require(registered_manifest(selected_key).parent_path() == sandbox.directory() / L"Glint",
                "bundled manifest uses isolated LOCALAPPDATA");
    }
}

void bundled_manifest_reader_restricts_multiple_origins() {
    Sandbox sandbox;
    std::wstring error;
    require(glint::setup::register_bundled_host(false, error), "prepare bundled registration");
    auto manifest = load_manifest(kChromeKey);
    const auto original = manifest;
    auto origins = manifest.at("allowed_origins").as_array();
    require(origins.size() == 2, "bundled registration has exactly two origins");
    std::swap(origins[0], origins[1]);
    manifest["allowed_origins"] = origins;
    replace_manifest(kChromeKey, manifest);
    auto state = glint::setup::read_registration();
    require(state.chrome_current && state.chrome_id == glint::utf8_to_wide(glint::setup::kBundledExtensionId),
            "known pair is accepted in either order and reports public-key identity");

    for (const auto& invalid_origins : {
             glint::Json::Array{origins[0], origins[0]},
             glint::Json::Array{origins[0], "chrome-extension://abcdefghijklmnopabcdefghijklmnop/"},
             glint::Json::Array{origins[0], origins[1], "chrome-extension://abcdefghijklmnopabcdefghijklmnop/"},
             glint::Json::Array{"chrome-extension://*/"},
             glint::Json::Array{}}) {
        manifest = original;
        manifest["allowed_origins"] = invalid_origins;
        replace_manifest(kChromeKey, manifest);
        state = glint::setup::read_registration();
        require(!state.chrome_current && state.chrome_id.empty(), "reject duplicates, arbitrary pairs, extra origins, wildcards and empty lists");
    }

    require(glint::setup::register_hosts(kChromeId, L"", error) == 0, "manual repair replaces bundled origins");
    const auto manual_origins = load_manifest(kChromeKey).at("allowed_origins").as_array();
    require(manual_origins.size() == 1 && manual_origins[0].as_string() ==
            "chrome-extension://" + glint::wide_to_utf8(kChromeId) + "/", "manual registration restricts to supplied ID");
}

void bundled_registration_reports_write_failure() {
    Sandbox sandbox;
    { std::ofstream blocked(sandbox.directory() / L"Glint"); blocked << "blocks registration directory"; }
    std::wstring error;
    require(!glint::setup::register_bundled_host(false, error) && !error.empty(), "bundled registration reports failure");
    const auto state = glint::setup::read_registration();
    require(state.chrome_id.empty() && state.edge_id.empty(), "failed bundled registration leaves registry empty");
}

void stale_and_invalid_manifests() {
    Sandbox sandbox;
    std::wstring error;
    require(glint::setup::register_hosts(kChromeId, L"", error) == 0, "prepare Chrome registration");
    auto manifest = load_manifest(kChromeKey);
    const auto original = manifest;
    manifest["path"] = glint::wide_to_utf8((sandbox.directory() / L"old-location" / L"Glint.exe").wstring());
    replace_manifest(kChromeKey, manifest);
    auto state = glint::setup::read_registration();
    require(state.chrome_id == kChromeId && !state.chrome_current, "old executable path retains ID but requires re-registration");

    manifest = original;
    const auto executable = std::filesystem::path(glint::utf8_to_wide(original.at("path").as_string()));
    manifest["path"] = glint::wide_to_utf8((executable.parent_path() / L"." / executable.filename()).wstring());
    replace_manifest(kChromeKey, manifest);
    require(glint::setup::read_registration().chrome_current, "equivalent executable path remains current");

    manifest = original;
    manifest["path"] = glint::wide_to_utf8(current_executable());
    replace_manifest(kChromeKey, manifest);
    state = glint::setup::read_registration();
    require(state.chrome_id == kChromeId && !state.chrome_current, "legacy desktop host retains ID and requires migration");

    manifest = original;
    const auto bridge = std::filesystem::path(glint::utf8_to_wide(original.at("path").as_string()));
    { std::ofstream modified(bridge, std::ios::binary | std::ios::trunc); modified << "modified binary"; }
    require(!glint::setup::read_registration().chrome_current, "modified bridge bytes require repair");
    std::filesystem::path repaired;
    require(glint::bridge_installation::ensure_current(repaired, error), "restore bridge for manifest validation");

    for (const auto* field : {"name", "type"}) {
        manifest = original;
        manifest[field] = "incorrect";
        replace_manifest(kChromeKey, manifest);
        require(!glint::setup::read_registration().chrome_current, "reject wrong native host name or transport");
    }
    manifest = original;
    manifest["allowed_origins"] = glint::Json::Array{"chrome-extension://invalid/"};
    replace_manifest(kChromeKey, manifest);
    require(!glint::setup::read_registration().chrome_current, "reject malformed extension origin");

    { std::ofstream output(registered_manifest(kChromeKey), std::ios::binary | std::ios::trunc); output << "not json"; }
    require(!glint::setup::read_registration().chrome_current, "malformed manifest is not registered");
    std::filesystem::remove(registered_manifest(kChromeKey));
    require(!glint::setup::read_registration().chrome_current, "missing manifest is not registered");
}

void migration_preserves_origins_without_configuring_other_browsers() {
    Sandbox sandbox;
    std::wstring error;
    require(glint::setup::migrate_existing_hosts(error) && error.empty(), "empty registration migration succeeds");
    require(!std::filesystem::exists(sandbox.directory() / L"Glint"), "empty migration writes no files or browser registration");
    require(glint::setup::register_hosts(kChromeId, L"", error) == 0, "prepare custom Chrome registration");
    auto manifest = load_manifest(kChromeKey);
    manifest["path"] = glint::wide_to_utf8(current_executable());
    manifest["description"] = "custom retained description";
    replace_manifest(kChromeKey, manifest);
    const auto origins = manifest.at("allowed_origins").dump();
    require(!glint::setup::read_registration().chrome_current, "legacy desktop target is stale");
    require(glint::setup::migrate_existing_hosts(error) && error.empty(), "migrate configured browser automatically");
    const auto migrated = load_manifest(kChromeKey);
    require(migrated.at("allowed_origins").dump() == origins &&
        migrated.at("description").as_string() == "custom retained description", "migration only changes binary path");
    auto state = glint::setup::read_registration();
    require(state.chrome_current && state.chrome_id == kChromeId && state.edge_id.empty() && !state.edge_current,
        "migration updates existing browser without adding Edge");
    require(migrated.at("path").as_string().find("GlintBridge.exe") != std::string::npos, "migration targets installed bridge");
    const auto timestamp = std::filesystem::last_write_time(registered_manifest(kChromeKey));
    require(glint::setup::migrate_existing_hosts(error) && error.empty() &&
        std::filesystem::last_write_time(registered_manifest(kChromeKey)) == timestamp, "current registration migration does not rewrite manifest");

    require(glint::setup::register_bundled_host(true, error), "prepare bundled Edge registration");
    manifest = load_manifest(kEdgeKey);
    manifest["path"] = glint::wide_to_utf8(current_executable());
    replace_manifest(kEdgeKey, manifest);
    require(glint::setup::migrate_existing_hosts(error), "migrate bundled registration");
    require(load_manifest(kEdgeKey).at("allowed_origins").dump() == manifest.at("allowed_origins").dump(),
        "migration retains both known bundled origins exactly");

    manifest = load_manifest(kChromeKey);
    manifest["path"] = glint::wide_to_utf8(current_executable());
    manifest["allowed_origins"] = glint::Json::Array{"chrome-extension://*/"};
    replace_manifest(kChromeKey, manifest);
    const auto invalid = load_manifest(kChromeKey).dump();
    require(glint::setup::migrate_existing_hosts(error) && error.empty(), "invalid registrations are left for explicit setup repair");
    require(load_manifest(kChromeKey).dump() == invalid, "migration never broadens or repairs malformed origins implicitly");
}

void unregister_is_idempotent() {
    Sandbox sandbox;
    std::wstring error;
    require(glint::setup::register_hosts(kChromeId, kEdgeId, error) == 0, "prepare both registrations for removal");
    const auto chrome_path = registered_manifest(kChromeKey);
    const auto edge_path = registered_manifest(kEdgeKey);
    const std::filesystem::path bridge(glint::utf8_to_wide(load_manifest(kChromeKey).at("path").as_string()));
    require(glint::run_register(true, L"", L"") == 0, "unregister both browsers");
    require(glint::run_register(true, L"", L"") == 0, "repeated unregister succeeds");
    const auto state = glint::setup::read_registration();
    require(state.chrome_id.empty() && state.edge_id.empty() && !state.chrome_current && !state.edge_current,
            "unregister clears both registrations");
    require(!std::filesystem::exists(chrome_path) && !std::filesystem::exists(edge_path), "unregister removes generated manifests");
    require(std::filesystem::is_regular_file(bridge), "unregister leaves a potentially running browser bridge executable intact");
}

void onboarding_state() {
    Sandbox sandbox;
    glint::setup::RegistrationState state;
    require(!glint::setup::onboarding_dismissed(), "onboarding is initially not dismissed");
    require(glint::setup::should_show_onboarding(false, state), "show first-launch guide without registration");
    require(!glint::setup::should_show_onboarding(true, state), "respect explicit dismissal");
    state.chrome_current = true;
    require(!glint::setup::should_show_onboarding(false, state), "registered Chrome suppresses automatic guide");
    state.chrome_current = false;
    state.edge_current = true;
    require(!glint::setup::should_show_onboarding(false, state), "registered Edge suppresses automatic guide");
    require(glint::setup::dismiss_onboarding(), "persist guide dismissal");
    require(glint::setup::onboarding_dismissed(), "read persisted guide dismissal");
    require(glint::setup::dismiss_onboarding(), "guide dismissal is idempotent");
}

} // namespace

int main() {
    try {
        validation_and_no_partial_update();
        independent_browser_registration();
        bundled_registration_only_changes_selected_browser();
        bundled_manifest_reader_restricts_multiple_origins();
        bundled_registration_reports_write_failure();
        stale_and_invalid_manifests();
        migration_preserves_origins_without_configuring_other_browsers();
        unregister_is_idempotent();
        onboarding_state();
        std::cout << "registration tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "registration test failed: " << error.what() << '\n';
        return 1;
    }
}
