#include "setup_window.hpp"
#include "extension_identity.hpp"
#include "registration.hpp"

#include <commctrl.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr int ChooseChrome = 2101, ChooseEdge = 2102, Primary = 2103, Back = 2104;
constexpr int OpenPage = 2106, CopyAddress = 2107, CopyPath = 2108;
constexpr int Manual = 2110, ManualId = 2111, RegisterManual = 2112, AddBrowser = 2113;
constexpr int FolderPath = 2114;
constexpr wchar_t kManualId[] = L"abcdefghijklmnopabcdefghijklmnop";
constexpr wchar_t kEdgeManualId[] = L"ponmlkjihgfedcbaponmlkjihgfedcba";

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Both redirections are process-local. No registration helper is called until
// the isolated HKCU hive and LOCALAPPDATA directory are ready.
class Sandbox {
public:
    Sandbox() {
        const auto suffix = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        registry_path_ = L"Software\\Glint.SetupWindowTests." + suffix;
        directory_ = std::filesystem::temp_directory_path() / (L"Glint-setup-window-tests-" + suffix);
        try {
            require(std::filesystem::create_directory(directory_), "create unique setup test directory");
            directory_created_ = true;
            const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
            environment_existed_ = count != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
            if (count) {
                original_environment_.resize(count);
                const auto copied = GetEnvironmentVariableW(L"LOCALAPPDATA", original_environment_.data(), count);
                require(copied != 0 && copied < count, "read original LOCALAPPDATA");
                original_environment_.resize(copied);
            }
            require(RegCreateKeyExW(HKEY_CURRENT_USER, registry_path_.c_str(), 0, nullptr, 0,
                KEY_ALL_ACCESS, nullptr, &root_, nullptr) == ERROR_SUCCESS, "create isolated registry root");
            require(RegOverridePredefKey(HKEY_CURRENT_USER, root_) == ERROR_SUCCESS, "redirect setup test HKCU");
            redirected_ = true;
            require(SetEnvironmentVariableW(L"LOCALAPPDATA", directory_.c_str()) != FALSE,
                    "redirect setup test LOCALAPPDATA");
            environment_changed_ = true;
        } catch (...) { cleanup(); throw; }
    }
    ~Sandbox() { cleanup(); }
    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;
    const std::filesystem::path& directory() const { return directory_; }
private:
    void cleanup() noexcept {
        if (environment_changed_) SetEnvironmentVariableW(L"LOCALAPPDATA", environment_existed_ ? original_environment_.c_str() : nullptr);
        if (redirected_) RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        if (root_) { RegCloseKey(root_); RegDeleteTreeW(HKEY_CURRENT_USER, registry_path_.c_str()); }
        if (directory_created_ && directory_.is_absolute() &&
            directory_.parent_path() == std::filesystem::temp_directory_path() &&
            directory_.filename().wstring().starts_with(L"Glint-setup-window-tests-")) {
            std::error_code ignored;
            std::filesystem::remove_all(directory_, ignored);
        }
    }
    HKEY root_ = nullptr;
    bool redirected_ = false, directory_created_ = false;
    bool environment_existed_ = false, environment_changed_ = false;
    std::filesystem::path directory_;
    std::wstring registry_path_, original_environment_;
};

HWND test_window() {
    HWND window = FindWindowW(L"Glint.SetupWindow", nullptr);
    while (window) {
        DWORD process = 0;
        const auto thread = GetWindowThreadProcessId(window, &process);
        if (process == GetCurrentProcessId() && thread == GetCurrentThreadId()) return window;
        window = FindWindowExW(nullptr, window, L"Glint.SetupWindow", nullptr);
    }
    throw std::runtime_error("find the setup window owned by this test process");
}

HWND control(HWND window, int id) {
    const auto child = GetDlgItem(window, id);
    require(child != nullptr, "find setup control");
    return child;
}

void check_unique_control_ids(HWND window) {
    std::vector<int> ids;
    for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        const auto id = GetDlgCtrlID(child);
        if (id <= 0) continue;
        require(std::find(ids.begin(), ids.end(), id) == ids.end(), "interactive controls must have unique IDs across all pages");
        ids.push_back(id);
    }
}

void click(HWND window, int id) {
    const auto child = control(window, id);
    require(IsWindowVisible(child) != FALSE, "click only a visible setup control");
    require(IsWindowEnabled(child) != FALSE, "click only an enabled setup control");
    SendMessageW(child, BM_CLICK, 0, 0);
}

std::wstring text(HWND window) {
    std::wstring value(static_cast<std::size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
    const auto copied = GetWindowTextW(window, value.data(), static_cast<int>(value.size()));
    value.resize(copied);
    return value;
}

void check_installation_actions(HWND window) {
    // Keep the user's clipboard untouched: these assertions guard the distinct
    // page-address and folder-path actions without invoking either copy button.
    require(text(control(window, OpenPage)) == L"打开浏览器并复制页面地址",
            "browser action must explain that it copies the page address");
    require(text(control(window, CopyAddress)) == L"复制页面地址",
            "the address action must remain distinct from copying the extension folder");
    require(text(control(window, CopyPath)) == L"复制扩展文件夹路径",
            "the path action must explicitly identify the extension folder");
}

glint::Json hello(const char* browser) {
    return glint::Json::Object{{"type", "hello"}, {"version", 1}, {"sessionId", "session"},
        {"profileId", "Default"}, {"browser", browser}, {"profileLabel", "Default"}};
}

glint::Json snapshot() {
    return glint::Json::Object{{"type", "snapshot"}, {"version", 1}, {"tabs", glint::Json::Array{}}};
}

RECT bounds(HWND parent, HWND child) {
    RECT rect{};
    require(GetWindowRect(child, &rect) != FALSE, "read control bounds");
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2);
    return rect;
}

void check_layout(HWND window, UINT dpi) {
    RECT frame{0, 0, MulDiv(680, dpi, 96), MulDiv(586, dpi, 96)};
    // Sending WM_DPICHANGED exercises the content scaling; the window manager
    // still uses the real monitor DPI for its non-client frame in this test.
    require(AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)),
        FALSE, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)), GetDpiForWindow(window)) != FALSE,
        "calculate DPI test window bounds");
    frame.right -= frame.left; frame.bottom -= frame.top; frame.left = 0; frame.top = 0;
    SendMessageW(window, WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&frame));
    RECT client{}; GetClientRect(window, &client);
    std::vector<RECT> occupied;
    for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (!IsWindowVisible(child)) continue;
        const auto rect = bounds(window, child);
        if (rect.left < 0 || rect.top < 0 || rect.right > client.right || rect.bottom > client.bottom) {
            std::cerr << "DPI=" << dpi << " control=" << GetDlgCtrlID(child)
                << " bounds=" << rect.left << ',' << rect.top << ',' << rect.right << ',' << rect.bottom
                << " viewport=" << client.right << ',' << client.bottom << '\n';
            throw std::runtime_error("visible setup control must fit its viewport at 100/150/200 percent DPI");
        }
        require(rect.right > rect.left && rect.bottom > rect.top, "visible controls must have positive size");
        for (const auto& other : occupied) {
            RECT overlap{};
            require(!IntersectRect(&overlap, &rect, &other), "visible setup controls must not overlap");
        }
        occupied.push_back(rect);
        wchar_t klass[32]{}; GetClassNameW(child, klass, 32);
        const auto value = text(child);
        if (wcscmp(klass, L"Static") == 0 && !value.empty()) {
            HDC dc = GetDC(child);
            const auto previous = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(child, WM_GETFONT, 0, 0)));
            RECT needed{0, 0, rect.right - rect.left, 0};
            DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &needed, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(dc, previous); ReleaseDC(child, dc);
            require(needed.bottom <= rect.bottom - rect.top, "setup instructions must fit without text clipping");
        }
    }
}

void check_all_dpis(HWND window) {
    for (const UINT dpi : {96u, 144u, 192u}) check_layout(window, dpi);
    check_layout(window, 96);
}

void automatic_edge_flow() {
    Sandbox sandbox;
    int searches = 0;
    glint::SetupWindow setup(nullptr, [&] { ++searches; });
    setup.show();
    const auto window = test_window();
    check_unique_control_ids(window);
    require(!IsWindowVisible(control(window, ManualId)), "manual extension ID must be hidden by default");
    require(!IsWindowVisible(control(window, Back)), "first page must not show Back");
    check_all_dpis(window);
    click(window, ChooseEdge);
    click(window, Primary);
    require(IsWindowVisible(control(window, FolderPath)) != FALSE, "Next must advance to browser installation");
    require(!IsWindowVisible(control(window, ManualId)), "automatic setup must not ask for an extension ID");
    require(GetFocus() != nullptr && IsWindowVisible(GetFocus()), "installation page must focus a visible control");
    const auto state = glint::setup::read_registration();
    require(state.edge_current && state.edge_id == glint::utf8_to_wide(glint::setup::kBundledExtensionId),
            "Next must register the bundled identity for Edge");
    require(state.chrome_id.empty(), "Edge setup must leave Chrome unregistered");
    const auto expected = sandbox.directory() / L"Glint" / L"extension";
    require(std::filesystem::is_regular_file(expected / L"manifest.json"), "Next must extract the embedded extension");
    require(text(control(window, FolderPath)) == expected.wstring(), "installation page must display the extracted folder");
    check_installation_actions(window);
    check_all_dpis(window);
    click(window, Primary);
    require(IsWindowVisible(control(window, AddBrowser)) != FALSE, "second Next must show connection confirmation");
    require(!IsWindowEnabled(control(window, Primary)), "confirmation must wait for the selected browser");
    require(GetFocus() == control(window, Back), "waiting confirmation must focus the enabled Back button");
    check_all_dpis(window);
    glint::TabIndex index;
    require(index.apply(1, hello("Chrome")) && index.apply(1, snapshot()), "synchronize real Chrome connection");
    setup.update_connection(index.connections(), L"Ctrl+Shift+Space");
    require(!IsWindowEnabled(control(window, Primary)), "Chrome synchronization must not unlock Edge setup");
    require(index.apply(2, hello("Edge")), "accept real Edge hello");
    setup.update_connection(index.connections(), L"Ctrl+Shift+Space");
    require(!IsWindowEnabled(control(window, Primary)), "Edge hello without a snapshot must not unlock setup");
    require(index.apply(2, snapshot()), "synchronize real empty Edge snapshot");
    setup.update_connection(index.connections(), L"Ctrl+Shift+Space");
    require(IsWindowEnabled(control(window, Primary)) != FALSE, "empty Edge snapshot must unlock setup");
    SetFocus(control(window, Primary));
    require(index.disconnect(2), "disconnect real Edge connection");
    setup.update_connection(index.connections(), L"Ctrl+Shift+Space");
    require(!IsWindowEnabled(control(window, Primary)), "Edge disconnect must lock setup again");
    require(GetFocus() != nullptr && IsWindowEnabled(GetFocus()), "disconnect must leave keyboard focus on an enabled control");
    click(window, Back);
    require(IsWindowVisible(control(window, FolderPath)) != FALSE, "Back from confirmation must return to installation");
    click(window, Back);
    require(IsWindowVisible(control(window, ChooseChrome)) != FALSE, "Back from installation must return to browser selection");
    click(window, ChooseChrome);
    click(window, Primary);
    check_installation_actions(window);
    require(glint::setup::read_registration().chrome_current, "selecting another browser must prepare its connection");
    require(glint::setup::read_registration().edge_current, "preparing Chrome must preserve Edge registration");
    click(window, Primary);
    require(IsWindowEnabled(control(window, Primary)) != FALSE, "existing synchronized Chrome must unlock Chrome confirmation");
    click(window, AddBrowser);
    require(IsWindowVisible(control(window, ChooseEdge)) != FALSE, "Add another browser must return to selection");
    require(SendMessageW(control(window, ChooseEdge), BM_GETCHECK, 0, 0) == BST_CHECKED,
            "Add another browser must select Edge after completing Chrome");
    require(searches == 0, "navigation must not open search prematurely");
}

void manual_validation_and_completion() {
    Sandbox sandbox;
    int searches = 0;
    glint::SetupWindow setup(nullptr, [&] { ++searches; });
    setup.show();
    const auto window = test_window();
    click(window, ChooseChrome);
    click(window, Manual);
    require(IsWindowVisible(control(window, ManualId)) != FALSE, "advanced settings must reveal the ID field");
    require(GetFocus() == control(window, ManualId), "advanced settings must focus the ID field");
    setup.show();
    require(GetFocus() == control(window, ManualId), "reopening advanced settings must focus the ID field");
    check_all_dpis(window);
    SetWindowTextW(control(window, ManualId), L" \t\r\n ");
    click(window, RegisterManual);
    require(IsWindowVisible(control(window, ManualId)) != FALSE, "blank manual ID must stay on advanced settings");
    require(glint::setup::read_registration().chrome_id.empty(), "blank manual ID must not register anything");
    SetWindowTextW(control(window, ManualId), L"invalid");
    click(window, RegisterManual);
    require(IsWindowVisible(control(window, ManualId)) != FALSE, "invalid manual ID must stay on advanced settings");
    require(glint::setup::read_registration().chrome_id.empty(), "invalid manual ID must not register anything");
    SetWindowTextW(control(window, ManualId), (std::wstring(L"  ") + kManualId + L"  ").c_str());
    click(window, RegisterManual);
    require(!IsWindowVisible(control(window, ManualId)), "valid manual ID must advance to confirmation");
    require(glint::setup::read_registration().chrome_id == kManualId, "valid manual ID must be trimmed before registration");
    require(!IsWindowEnabled(control(window, Primary)), "manual registration must still wait for a real snapshot");
    click(window, Back);
    require(IsWindowVisible(control(window, ManualId)) != FALSE, "Back from manual confirmation must return to advanced settings");
    require(GetFocus() == control(window, ManualId), "returning to advanced settings must focus the ID field");
    click(window, Primary);
    require(IsWindowVisible(control(window, ChooseChrome)) != FALSE, "advanced settings must offer a return to automatic connection");
    click(window, Manual);
    require(text(control(window, ManualId)) == std::wstring(L"  ") + kManualId + L"  ", "advanced navigation must preserve the entered ID");
    click(window, Back);
    click(window, ChooseEdge);
    click(window, Manual);
    require(text(control(window, ManualId)).empty(), "switching to unregistered Edge must not reuse Chrome's manual ID");
    click(window, RegisterManual);
    require(IsWindowVisible(control(window, ManualId)) != FALSE, "Edge must still require its own nonempty ID after switching browsers");
    require(glint::setup::read_registration().edge_id.empty(), "switching browsers must not register the previous browser's ID");
    SetWindowTextW(control(window, ManualId), kEdgeManualId);
    click(window, RegisterManual);
    require(glint::setup::read_registration().edge_id == kEdgeManualId, "manual Edge registration must use Edge's supplied ID");
    click(window, Back);
    click(window, Primary);
    click(window, ChooseChrome);
    click(window, Manual);
    require(text(control(window, ManualId)) == kManualId, "switching back to Chrome must restore Chrome's own registered ID");
    require(glint::setup::read_registration().edge_id == kEdgeManualId, "returning to Chrome must preserve Edge's registration");
    click(window, RegisterManual);
    glint::TabIndex index;
    require(index.apply(1, hello("Chrome")) && index.apply(1, snapshot()), "synchronize manual Chrome connection");
    setup.update_connection(index.connections(), L"Ctrl+Shift+Space");
    click(window, Primary);
    require(searches == 1 && !IsWindowVisible(window), "successful completion must open search and hide setup");
    require(glint::setup::onboarding_dismissed(), "successful completion must persist dismissal in the isolated registry");
}
} // namespace

int main() {
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
        require(InitCommonControlsEx(&controls) != FALSE, "initialize native setup controls");
        automatic_edge_flow();
        manual_validation_and_completion();
        std::cout << "setup window tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "setup window test failed: " << error.what() << '\n';
        return 1;
    }
}
