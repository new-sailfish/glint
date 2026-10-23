#include "hotkey.hpp"
#include "language.hpp"

#include <windows.h>

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace glint::hotkey;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Every registry operation is redirected to this unique test-owned key. These
// tests run serially because RegOverridePredefKey affects the whole process.
class RegistrySandbox {
public:
    RegistrySandbox() {
        static std::atomic<unsigned long> sequence = 0;
        path_ = L"Software\\Glint.HotkeyTests." + std::to_wstring(GetCurrentProcessId()) + L"." +
            std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(sequence.fetch_add(1));
        require(RegCreateKeyExW(HKEY_CURRENT_USER, path_.c_str(), 0, nullptr, 0,
                               KEY_ALL_ACCESS, nullptr, &root_, nullptr) == ERROR_SUCCESS,
                "create isolated hotkey registry root");
        require(RegOverridePredefKey(HKEY_CURRENT_USER, root_) == ERROR_SUCCESS, "redirect hotkey test registry");
    }

    ~RegistrySandbox() {
        RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        RegCloseKey(root_);
        RegDeleteTreeW(HKEY_CURRENT_USER, path_.c_str());
    }

    void write(const wchar_t* name, DWORD type, const void* value, DWORD size) {
        HKEY key = nullptr;
        require(RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Glint", 0, nullptr, 0,
                               KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS, "create isolated Glint key");
        const auto status = RegSetValueExW(key, name, 0, type, static_cast<const BYTE*>(value), size);
        RegCloseKey(key);
        require(status == ERROR_SUCCESS, "write isolated hotkey registry value");
    }

    void dword(const wchar_t* name, DWORD value) { write(name, REG_DWORD, &value, sizeof(value)); }

    DWORD dword(const wchar_t* name) const {
        DWORD value = 0, size = sizeof(value), type = 0;
        require(RegGetValueW(HKEY_CURRENT_USER, L"Software\\Glint", name, RRF_RT_REG_DWORD,
                             &type, &value, &size) == ERROR_SUCCESS && size == sizeof(value), "read isolated DWORD");
        return value;
    }

    void remove(const wchar_t* name) {
        const auto status = RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\Glint", name);
        require(status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND, "remove isolated hotkey value");
    }

    void make_unwritable() {
        HKEY deleted = nullptr;
        require(RegCreateKeyExW(HKEY_CURRENT_USER, L"DeletedRoot", 0, nullptr, 0,
                               KEY_ALL_ACCESS, nullptr, &deleted, nullptr) == ERROR_SUCCESS, "create failure hive");
        require(RegDeleteKeyW(HKEY_CURRENT_USER, L"DeletedRoot") == ERROR_SUCCESS, "delete failure hive");
        const auto status = RegOverridePredefKey(HKEY_CURRENT_USER, deleted);
        RegCloseKey(deleted);
        require(status == ERROR_SUCCESS, "redirect to deleted test key for persistence failure");
    }

    void restore() {
        require(RegOverridePredefKey(HKEY_CURRENT_USER, root_) == ERROR_SUCCESS, "restore writable test registry");
    }

private:
    HKEY root_ = nullptr;
    std::wstring path_;
};

DWORD packed(Binding binding) { return static_cast<DWORD>((binding.modifiers << 16) | binding.key); }

class TestWindow {
public:
    TestWindow() {
        window = CreateWindowExW(0, L"STATIC", L"Glint hotkey tests", 0, 0, 0, 0, 0,
                                 HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(window != nullptr, "create test-owned message window");
    }
    ~TestWindow() { DestroyWindow(window); }
    HWND window = nullptr;
};

class Reservation {
public:
    Reservation(HWND window, Binding binding) : window_(window) {
        require(RegisterHotKey(window_, 90, binding.modifiers | MOD_NOREPEAT, binding.key) != FALSE,
                "reserve a test-owned hotkey conflict");
    }
    ~Reservation() { UnregisterHotKey(window_, 90); }
private:
    HWND window_;
};

std::vector<Binding> unused_bindings(HWND window) {
    std::vector<Binding> result;
    for (UINT key = VK_F13; key <= VK_F24; ++key) {
        const Binding binding{MOD_CONTROL | MOD_ALT | MOD_SHIFT, key};
        if (!RegisterHotKey(window, 91, binding.modifiers | MOD_NOREPEAT, binding.key)) continue;
        UnregisterHotKey(window, 91);
        result.push_back(binding);
        if (result.size() == 3) break;
    }
    require(result.size() == 3, "find three unused hotkeys without replacing existing registrations");
    return result;
}

void accepts_supported_bindings() {
    require(valid(kDefault), "default Ctrl+Shift+Space remains valid");
    for (const UINT modifier : {MOD_CONTROL, MOD_ALT, MOD_WIN, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN}) {
        for (const UINT key : {UINT('A'), UINT('0'), UINT(VK_SPACE), UINT(VK_TAB), UINT(VK_RETURN),
                               UINT(VK_LEFT), UINT(VK_DELETE), UINT(VK_OEM_1), UINT(VK_OEM_102), UINT(VK_NUMPAD0)}) {
            require(valid({modifier, key}), "accept letters, navigation, punctuation, and keypad with strong modifiers");
        }
    }
    for (UINT key = VK_F1; key <= VK_F24; ++key) {
        require(valid({0, key}) == (key != VK_F12), "function keys may be bare except debugger-reserved F12");
        require(valid({MOD_SHIFT, key}) == (key != VK_F12), "shifted function keys remain supported except F12");
    }
}

void rejects_unsafe_or_unrepresentable_bindings() {
    for (const UINT key : {UINT('A'), UINT('0'), UINT(VK_SPACE), UINT(VK_TAB), UINT(VK_OEM_1)}) {
        require(!valid({0, key}) && !valid({MOD_SHIFT, key}), "ordinary keys require Ctrl, Alt, or Win");
    }
    for (const UINT key : {UINT(0), UINT(0xff), UINT(0x100), UINT(VK_SHIFT), UINT(VK_CONTROL), UINT(VK_MENU),
                           UINT(VK_LSHIFT), UINT(VK_RCONTROL), UINT(VK_LMENU), UINT(VK_LWIN), UINT(VK_RWIN),
                           UINT(VK_LBUTTON), UINT(VK_RBUTTON), UINT(VK_XBUTTON1), UINT(VK_PACKET), UINT(VK_PROCESSKEY), UINT(VK_F12)}) {
        require(!valid({MOD_CONTROL, key}), "reject modifier-only, mouse, undefined, injected, and reserved keys");
    }
    require(!valid({MOD_CONTROL | MOD_NOREPEAT, 'G'}), "MOD_NOREPEAT is registration behavior, not a saved modifier");
    require(!valid({MOD_CONTROL | 0x8000, 'G'}), "reject unknown modifier flags");
}

void names_are_readable_and_modifiers_are_ordered() {
    require(name(kDefault) == L"Ctrl+Shift+Space", "retain familiar default name");
    require(name({MOD_WIN | MOD_SHIFT | MOD_ALT | MOD_CONTROL, 'G'}) == L"Ctrl+Alt+Shift+Win+G", "canonical modifier order");
    require(name({MOD_ALT, '7'}) == L"Alt+7", "stable digit label");
    require(name({0, VK_F24}) == L"F24", "stable function-key label");
    require(name({MOD_CONTROL, VK_LEFT}).size() > 5, "navigation keys have localized readable names");
    require(name({MOD_CONTROL, VK_OEM_1}).size() > 5, "punctuation keys have localized readable names");
    require(name({0, 0}).empty(), "invalid binding has no misleading display name");
}
void validation_uses_selected_language() {
    std::wstring error;
    glint::i18n::current_language = glint::i18n::Language::English;
    require(name({MOD_CONTROL, VK_LEFT}) == L"Ctrl+Left", "English navigation key name ignores OS language");
    require(name({MOD_CONTROL, VK_BROWSER_FAVORITES}) == L"Ctrl+Browser Favorites", "English media key name ignores OS language");
    require(!save({0, 'G'}, error) && error.find(L"Ctrl, Alt, or Win") != std::wstring::npos,
        "English invalid-binding guidance");
    Registration registration;
    require(!registration.start(nullptr, kDefault, error) && error.find(L"window") != std::wstring::npos,
        "English registration failure guidance");
    glint::i18n::current_language = glint::i18n::Language::Chinese;
    require(name({MOD_CONTROL, VK_LEFT}) == L"Ctrl+左方向键", "Chinese navigation key name follows preference");
    require(name({MOD_CONTROL, VK_BROWSER_FAVORITES}) == L"Ctrl+浏览器收藏夹", "Chinese media key name follows preference");
    require(!save({0, 'G'}, error) && error.find(L"普通键") != std::wstring::npos,
        "Chinese invalid-binding guidance");
}

void defaults_and_legacy_values_are_preserved() {
    RegistrySandbox registry;
    require(load() == kDefault, "absent settings use the historical default");
    const Binding legacy[] = {kDefault, {MOD_CONTROL | MOD_ALT, VK_SPACE}, {MOD_ALT, VK_SPACE}};
    for (DWORD index = 0; index < 3; ++index) {
        registry.dword(L"Hotkey", index);
        require(load() == legacy[index], "migrate each historical preset without changing behavior");
    }
    registry.dword(L"Hotkey", 999);
    require(load() == kDefault, "unknown historical preset uses default");
    const wchar_t bad[] = L"1";
    registry.write(L"Hotkey", REG_SZ, bad, sizeof(bad));
    require(load() == kDefault, "legacy value requires exact DWORD type");
    const BYTE short_value = 1;
    registry.write(L"Hotkey", REG_DWORD, &short_value, sizeof(short_value));
    require(load() == kDefault, "legacy value requires full DWORD size");
}

void new_binding_round_trips_without_changing_legacy_setting() {
    RegistrySandbox registry;
    registry.dword(L"Hotkey", 2);
    const Binding binding{MOD_CONTROL | MOD_ALT | MOD_WIN, 'G'};
    std::wstring error = L"stale";
    require(save(binding, error) && error.empty(), "save a valid custom binding and clear stale error");
    require(load() == binding, "custom binding has priority over legacy preset");
    require(registry.dword(L"HotkeyBinding") == packed(binding), "store key in low word and modifiers in high word");
    require(registry.dword(L"Hotkey") == 2, "custom save leaves legacy setting intact");
    require(save({0, VK_F8}, error) && load() == Binding{0, VK_F8}, "zero-modifier function key round trips");
}

void malformed_new_setting_falls_back_to_legacy() {
    RegistrySandbox registry;
    registry.dword(L"Hotkey", 1);
    const Binding legacy{MOD_CONTROL | MOD_ALT, VK_SPACE};
    for (const DWORD value : {DWORD(0), packed({MOD_CONTROL, VK_F12}), packed({0x8000, 'G'}), packed({MOD_CONTROL, 0xff})}) {
        registry.dword(L"HotkeyBinding", value);
        require(load() == legacy, "invalid custom binding falls back to valid legacy selection");
    }
    const DWORD value = packed({MOD_CONTROL, 'G'});
    registry.write(L"HotkeyBinding", REG_BINARY, &value, sizeof(value));
    require(load() == legacy, "custom binding requires exact DWORD registry type");
    registry.write(L"HotkeyBinding", REG_DWORD, &value, sizeof(value) - 1);
    require(load() == legacy, "custom binding rejects truncated DWORDs");
    const unsigned long long oversized = value;
    registry.write(L"HotkeyBinding", REG_DWORD, &oversized, sizeof(oversized));
    require(load() == legacy, "custom binding rejects oversized DWORDs");
}

void invalid_or_failed_saves_preserve_existing_setting() {
    RegistrySandbox registry;
    const Binding original{MOD_ALT, 'H'};
    std::wstring error;
    require(save(original, error), "save initial custom binding");
    require(!save({MOD_SHIFT, 'G'}, error) && !error.empty(), "invalid binding returns useful error");
    require(load() == original, "invalid save leaves existing setting intact");
    registry.make_unwritable();
    require(!save({MOD_CONTROL, 'G'}, error) && !error.empty(), "registry write failure returns useful error");
    registry.restore();
    require(load() == original, "failed save leaves existing setting intact");
}

void registration_start_and_changes_are_transactional() {
    RegistrySandbox registry;
    TestWindow window;
    TestWindow other;
    const auto bindings = unused_bindings(window.window);
    const auto first = bindings[0], second = bindings[1], third = bindings[2];
    std::wstring error;
    Registration registration;
    require(!registration.active(), "registration begins inactive");
    require(registration.start(window.window, first, error), "start registers an available custom binding");
    require(registration.active() && registration.current() == first && error.empty(), "start records successful binding");
    require(load() == kDefault, "start does not alter saved settings");
    const auto first_id = registration.id();
    require(first_id == 1 || first_id == 2, "registration uses the private id pair");

    require(registration.change(first, error), "applying current binding succeeds without duplicate registration");
    require(registration.id() == first_id && load() == first, "unchanged binding can be persisted without changing registration id");
    {
        Reservation conflict(other.window, second);
        require(!registration.change(second, error) && !error.empty(), "report occupied candidate binding");
        require(registration.active() && registration.current() == first && registration.id() == first_id,
                "registration conflict preserves previous active shortcut");
        require(load() == first, "registration conflict never persists rejected candidate");
        require(!RegisterHotKey(other.window, 92, first.modifiers, first.key), "previous hotkey remains reserved after conflict");
    }
    require(!registration.change({0, 'G'}, error) && !error.empty(), "reject invalid replacement");
    require(registration.current() == first && load() == first, "invalid replacement retains current and saved shortcut");

    registry.make_unwritable();
    require(!registration.change(second, error) && !error.empty(), "report persistence failure after candidate registration");
    require(registration.active() && registration.current() == first && registration.id() == first_id,
            "persistence failure retains previous active shortcut");
    {
        Reservation released_candidate(other.window, second);
        require(!RegisterHotKey(other.window, 92, first.modifiers, first.key), "persistence rollback keeps previous hotkey reserved");
    }
    registry.restore();
    require(load() == first, "persistence rollback keeps previous saved shortcut");

    require(registration.change(second, error) && error.empty(), "switch to available custom shortcut");
    require(registration.current() == second && registration.id() != first_id && load() == second,
            "successful change switches ids and persists new binding");
    { Reservation released_previous(other.window, first); }
    require(registration.change(third, error) && registration.id() == first_id, "subsequent change alternates back to original id");
    registration.stop();
    registration.stop();
    require(!registration.active(), "stop is idempotent");
    { Reservation released_on_stop(other.window, third); }
}

void failed_start_can_retry_and_destruction_releases_binding() {
    RegistrySandbox registry;
    TestWindow window;
    TestWindow other;
    const auto bindings = unused_bindings(window.window);
    const auto binding = bindings[0];
    std::wstring error;
    {
        Registration registration;
        require(registration.current() == kDefault, "registration starts with the historical default binding");
        require(!registration.start(nullptr, binding, error) && !error.empty(), "start requires a valid owner window");
        require(!registration.start(window.window, {0, 'G'}, error) && !error.empty(), "start rejects invalid shortcut");
        {
            Reservation conflict(other.window, binding);
            require(!registration.start(window.window, binding, error) && !error.empty(), "occupied startup shortcut reports failure");
            require(!registration.active(), "failed start remains inactive");
            require(registration.current() == binding, "failed startup retains the attempted binding for settings");
            require(registration.change(bindings[1], error) && registration.active(),
                    "a failed initial registration can recover by choosing another binding");
            registration.stop();
        }
        require(registration.start(window.window, binding, error), "retry startup after conflict resolves");
        require(registration.start(window.window, binding, error), "repeat startup with current binding is harmless");
    }
    { Reservation released_on_destruction(other.window, binding); }
    {
        Registration registration(window.window);
        require(registration.change(binding, error) && registration.active() && load() == binding,
                "window constructor permits initial persistent registration through change");
    }
}

} // namespace

int main() {
    const std::pair<const char*, void (*)()> tests[] = {
        {"accepts supported bindings", accepts_supported_bindings},
        {"rejects unsafe bindings", rejects_unsafe_or_unrepresentable_bindings},
        {"readable canonical names", names_are_readable_and_modifiers_are_ordered},
        {"selected-language validation", validation_uses_selected_language},
        {"default and legacy settings", defaults_and_legacy_values_are_preserved},
        {"custom binding round trip", new_binding_round_trips_without_changing_legacy_setting},
        {"malformed settings fallback", malformed_new_setting_falls_back_to_legacy},
        {"failed saves preserve state", invalid_or_failed_saves_preserve_existing_setting},
        {"transactional registration", registration_start_and_changes_are_transactional},
        {"registration retry and cleanup", failed_start_can_retry_and_destruction_releases_binding},
    };
    unsigned failures = 0;
    for (const auto& [label, test] : tests) {
        try {
            test();
            std::cout << "PASS: " << label << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << label << ": " << error.what() << '\n';
            ++failures;
        }
    }
    std::cout << (std::size(tests) - failures) << '/' << std::size(tests) << " hotkey test groups passed\n";
    return failures ? 1 : 0;
}
