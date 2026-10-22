#include "setup_window.hpp"
#include "extension_identity.hpp"
#include "registration.hpp"

#include <commctrl.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr int ChooseChrome = 2101, ChooseEdge = 2102, Primary = 2103, Back = 2104;
constexpr int Later = 2105;
constexpr int OpenPage = 2106, CopyAddress = 2107, CopyPath = 2108;
constexpr int OpenFolder = 2109;
constexpr int Manual = 2110, ManualId = 2111, RegisterManual = 2112, AddBrowser = 2113;
constexpr int FolderPath = 2114, ReopenPage = 2115;
constexpr int RepairId = 2116, RegisterRepair = 2117, RepairLabel = 2118, RepairHelp = 2119, RepairResult = 2120;
constexpr int Illustration = 2201, Troubleshoot = 2202, BackToStatus = 2203;
constexpr int ConnectionStatus = 2204, ConnectionDetail = 2205, StepProgress = 2206;
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

std::wstring visible_text(HWND window) {
    std::wstring value;
    for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (IsWindowVisible(child)) value += text(child) + L'\n';
    }
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
    const auto instructions = visible_text(window);
    require(instructions.find(L"底部“文件夹”输入框") != std::wstring::npos,
            "loading instructions must identify the folder chooser's bottom input field");
    require(instructions.find(L"直接点击“选择文件夹”") != std::wstring::npos,
            "a full folder path must be selected directly without navigating into it first");
    require(instructions.find(L"Alt+D") == std::wstring::npos,
            "loading instructions must not send the copied extension path to the chooser's address bar");
}

void check_guide_page(HWND window, int page) {
    require(IsWindowVisible(control(window, Illustration)), "every installation step must have its own illustration");
    require(!text(control(window, StepProgress)).empty(), "the guide must show step progress");
    require((IsWindowVisible(control(window, OpenPage)) != FALSE) == (page == 1), "opening the browser belongs to its own step");
    require((IsWindowVisible(control(window, CopyAddress)) != FALSE) == (page == 1), "copying the page address belongs to its own step");
    require((IsWindowVisible(control(window, FolderPath)) != FALSE) == (page == 3), "the extension path belongs only to the loading step");
    require((IsWindowVisible(control(window, CopyPath)) != FALSE) == (page == 3), "copying the extension path belongs only to the loading step");
    require((IsWindowVisible(control(window, OpenFolder)) != FALSE) == (page == 3), "opening the extension folder belongs only to the loading step");
    require(!IsWindowVisible(control(window, ManualId)), "automatic setup must not ask for an extension ID");
    require(!IsWindowVisible(control(window, RepairId)), "installation steps must not mix in repair controls");
    if (page == 2) {
        require(visible_text(window).find(L"开发者模式") != std::wstring::npos,
                "the developer-mode step must name the setting to enable");
    }
}

void check_confirmation(HWND window, const wchar_t* browser, bool connected) {
    const auto expected = std::wstring(browser) + (connected ? L" 已连接" : L" 未连接");
    require(IsWindowVisible(control(window, ConnectionStatus)), "confirmation must show an explicit connection status");
    require(text(control(window, ConnectionStatus)) == expected, "confirmation status must match the selected browser's synchronization state");
    require(IsWindowVisible(control(window, ConnectionDetail)), "confirmation must show connection details");
    for (const int id : {RepairId, RegisterRepair, RepairLabel, RepairHelp, RepairResult, BackToStatus}) {
        require(!IsWindowVisible(control(window, id)), "confirmation must keep repair fields on their separate page");
    }
    require((IsWindowVisible(control(window, AddBrowser)) != FALSE) == connected, "adding another browser belongs to the connected state");
    require((IsWindowVisible(control(window, ReopenPage)) != FALSE) == !connected, "reopening the extension page belongs to the disconnected state");
    require((IsWindowVisible(control(window, Troubleshoot)) != FALSE) == !connected, "troubleshooting belongs to the disconnected state");
    require(text(control(window, Primary)) == (connected ? L"开始搜索" : L"检查连接"),
            "the primary action must reflect whether the selected browser is connected");
    if (connected) {
        const auto content = visible_text(window);
        require(content.find(L"未连接") == std::wstring::npos && content.find(L"等待") == std::wstring::npos,
                "connected confirmation must not also suggest connection is incomplete");
    }
}

void advance_to_confirmation(HWND window) {
    for (int page = 0; page < 4; ++page) click(window, Primary);
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

void resize_client(HWND window, int width, int height) {
    // A native window can extend past the test monitor. Size its client canvas
    // explicitly rather than changing the user's monitor or DPI settings.
    // A scrollbar appearing/disappearing during WM_SIZE can change the frame
    // width, so measure again before correcting the final client dimensions.
    for (int attempt = 0; attempt < 3; ++attempt) {
        RECT client{}, frame{};
        require(GetClientRect(window, &client) && GetWindowRect(window, &frame), "read layout fixture dimensions");
        if (client.right == width && client.bottom == height) return;
        require(SetWindowPos(window, nullptr, 0, 0,
            width + frame.right - frame.left - client.right,
            height + frame.bottom - frame.top - client.bottom,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE), "size the layout fixture client canvas");
    }
    RECT client{};
    require(GetClientRect(window, &client) && client.right == width && client.bottom == height,
            "layout fixture must have its requested client dimensions");
}

void check_layout(HWND window, UINT dpi) {
    RECT frame{0, 0, MulDiv(680, dpi, 96), MulDiv(586, dpi, 96)};
    // Sending WM_DPICHANGED exercises the content scaling; the window manager
    // still uses the real monitor DPI for its non-client frame in this test.
    require(AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)),
        FALSE, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)), GetDpiForWindow(window)) != FALSE,
        "calculate DPI test window bounds");
    frame.right -= frame.left; frame.bottom -= frame.top; frame.left = 0; frame.top = 0;
    frame.right = (std::min)(frame.right, 1024L);
    frame.bottom = (std::min)(frame.bottom, 720L);
    // Stay on the current monitor: moving to desktop (0,0) could generate a
    // real DPI transition that replaces the synthetic DPI under test.
    MONITORINFO monitor{sizeof(monitor)};
    require(GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor), "read the fixture monitor");
    OffsetRect(&frame, monitor.rcWork.left, monitor.rcWork.top);
    SendMessageW(window, WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&frame));
    // Production correctly clips the suggested frame to the monitor and uses
    // scrolling. Check the complete scaled layout on its full logical canvas;
    // constrained-viewport reachability is verified separately below.
    resize_client(window, MulDiv(680, dpi, 96), MulDiv(586, dpi, 96));
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

void resize_to_short_viewport(HWND window, UINT dpi = 0) {
    if (!dpi) dpi = GetDpiForWindow(window);
    const int width = MulDiv(680, dpi, 96);
    const int height = (std::min)(MulDiv(380, dpi, 96), 689);
    RECT frame{0, 0, width, height}, current{};
    require(GetWindowRect(window, &current), "read short-monitor window position");
    require(AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)),
        FALSE, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)), GetDpiForWindow(window)),
        "calculate short-monitor window bounds");
    frame.right = current.left + frame.right - frame.left;
    frame.bottom = current.top + frame.bottom - frame.top;
    frame.left = current.left; frame.top = current.top;
    SendMessageW(window, WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&frame));
    // Keep this test about vertical scrolling: use the full logical width and
    // a deterministic clipped height even on a smaller hosted monitor.
    resize_client(window, width, height);
}

void check_focused_control_in_viewport(HWND window, const char* message) {
    const auto focused = GetFocus();
    require(focused && IsChild(window, focused) && IsWindowVisible(focused) && IsWindowEnabled(focused),
            "setup must retain an interactive visible focused control");
    RECT client{}; require(GetClientRect(window, &client), "read short-monitor viewport");
    const auto rect = bounds(window, focused);
    if (rect.top < 0 || rect.bottom > client.bottom) {
        std::cerr << "focus=" << GetDlgCtrlID(focused) << " bounds=" << rect.top << ',' << rect.bottom
            << " viewport=" << client.bottom << '\n';
        throw std::runtime_error(message);
    }
}

void short_monitor_focus_and_scrolling() {
    Sandbox sandbox;
    glint::SetupWindow setup(nullptr, [] {});
    setup.show();
    const auto window = test_window();
    for (const UINT dpi : {96u, 144u, 192u}) {
        resize_to_short_viewport(window, dpi);
        SCROLLINFO scroll{sizeof(scroll), SIF_RANGE | SIF_PAGE};
        require(GetScrollInfo(window, SB_VERT, &scroll) && scroll.nPage < static_cast<UINT>(scroll.nMax + 1),
                "short-viewport fixture must require vertical scrolling");
        for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            if (!IsWindowVisible(child) || !IsWindowEnabled(child) || !(GetWindowLongPtrW(child, GWL_STYLE) & WS_TABSTOP)) continue;
            SetFocus(child);
            require(GetFocus() == child, "a visible tab stop must accept keyboard focus");
            check_focused_control_in_viewport(window, "every setup tab stop must scroll into the clipped viewport at each DPI");
        }
    }
    click(window, ChooseChrome);
    click(window, Primary);
    resize_to_short_viewport(window);
    SetFocus(control(window, Primary));
    require(GetFocus() == control(window, Primary), "the Next button must own focus before advancing");
    check_focused_control_in_viewport(window, "focusing Next must scroll it into a short viewport");

    // On the developer step, Next remains the same HWND. SetFocus alone does
    // not emit another focus notification after navigation resets scrolling.
    click(window, Primary);
    check_guide_page(window, 2);
    require(GetFocus() == control(window, Primary), "advancing to developer mode must keep focus on Next");
    check_focused_control_in_viewport(window, "advancing with the same focused Next button must keep it in view");

    resize_to_short_viewport(window);
    check_focused_control_in_viewport(window, "a DPI layout update must bring the current focused control back into view");

    // Reading from the top must remain possible even though keyboard focus
    // stays on an off-screen footer action until the guide is shown again.
    for (int line = 0; line < 40; ++line) SendMessageW(window, WM_VSCROLL, SB_LINEUP, 0);
    RECT client{}; GetClientRect(window, &client);
    require(bounds(window, control(window, StepProgress)).top >= 0 &&
            bounds(window, control(window, Primary)).bottom > client.bottom,
            "manual scrolling must let the user read the heading above the footer");
    setup.show();
    check_guide_page(window, 2);
    check_focused_control_in_viewport(window, "showing an existing guide must reveal its already focused control");
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
    check_guide_page(window, 1);
    require(GetFocus() != nullptr && IsWindowVisible(GetFocus()), "installation page must focus a visible control");
    const auto state = glint::setup::read_registration();
    require(state.edge_current && state.edge_id == glint::utf8_to_wide(glint::setup::kBundledExtensionId),
            "Next must register the bundled identity for Edge");
    require(state.chrome_id.empty(), "Edge setup must leave Chrome unregistered");
    const auto expected = sandbox.directory() / L"Glint" / L"browser-extension";
    require(std::filesystem::is_regular_file(expected / L"manifest.json"), "Next must extract the embedded extension");
    require(text(control(window, OpenPage)) == L"打开浏览器并复制页面地址",
            "browser action must explain that it copies the page address");
    require(text(control(window, CopyAddress)) == L"复制页面地址",
            "the address action must remain distinct from copying the extension folder");
    check_all_dpis(window);
    click(window, Primary);
    check_guide_page(window, 2);
    check_all_dpis(window);
    click(window, Primary);
    check_guide_page(window, 3);
    require(text(control(window, FolderPath)) == expected.wstring(), "loading page must display the extracted folder");
    check_installation_actions(window);
    check_all_dpis(window);
    click(window, Primary);
    check_confirmation(window, L"Edge", false);
    require(GetFocus() != nullptr && IsWindowVisible(GetFocus()) && IsWindowEnabled(GetFocus()),
            "disconnected confirmation must focus a visible enabled control");
    click(window, Primary);
    require(searches == 0, "checking a disconnected browser must never open search");
    check_confirmation(window, L"Edge", false);
    check_all_dpis(window);
    glint::TabIndex index;
    require(index.apply(1, hello("Chrome")) && index.apply(1, snapshot()), "synchronize real Chrome connection");
    setup.update_connection(index.connections(), L"Ctrl+Shift+Space");
    check_confirmation(window, L"Edge", false);
    click(window, Primary);
    require(searches == 0, "Chrome synchronization must not unlock Edge search");
    require(index.apply(2, hello("Edge")), "accept real Edge hello");
    setup.update_connection(index.connections(), L"Ctrl+Shift+Space");
    check_confirmation(window, L"Edge", false);
    click(window, Primary);
    require(searches == 0, "Edge hello without a snapshot must not unlock search");
    SetFocus(control(window, Troubleshoot));
    require(index.apply(2, snapshot()), "synchronize real empty Edge snapshot");
    setup.update_connection(index.connections(), L"Ctrl+Shift+Space");
    check_confirmation(window, L"Edge", true);
    require(GetFocus() == control(window, Primary), "connection success must move focus off a hidden troubleshooting action");
    check_all_dpis(window);
    setup.update_connection(index.connections(), L"");
    require(text(control(window, ConnectionDetail)).find(L"快捷键未启用") != std::wstring::npos,
        "failed hotkey registration must not be advertised as usable");
    setup.update_connection(index.connections(), L"Ctrl+Alt+K");
    require(text(control(window, ConnectionDetail)).find(L"Ctrl+Alt+K") != std::wstring::npos,
        "connected guidance must display the active custom shortcut");
    setup.update_connection(index.connections(), L"Ctrl+Shift+Space");
    SetFocus(control(window, AddBrowser));
    require(index.disconnect(2), "disconnect real Edge connection");
    setup.update_connection(index.connections(), L"Ctrl+Shift+Space");
    check_confirmation(window, L"Edge", false);
    require(GetFocus() == control(window, Primary), "disconnect must move focus off the hidden add-browser action");
    click(window, Primary);
    require(searches == 0, "Edge disconnect must prevent search again");
    require(GetFocus() != nullptr && IsWindowEnabled(GetFocus()), "disconnect must leave keyboard focus on an enabled control");
    click(window, Back);
    check_guide_page(window, 3);
    click(window, Back);
    check_guide_page(window, 2);
    click(window, Back);
    check_guide_page(window, 1);
    click(window, Back);
    require(IsWindowVisible(control(window, ChooseChrome)) != FALSE, "Back from the first guide step must return to browser selection");
    click(window, ChooseChrome);
    click(window, Primary);
    check_guide_page(window, 1);
    require(glint::setup::read_registration().chrome_current, "selecting another browser must prepare its connection");
    require(glint::setup::read_registration().edge_current, "preparing Chrome must preserve Edge registration");
    click(window, Primary);
    click(window, Primary);
    click(window, Primary);
    check_confirmation(window, L"Chrome", true);
    click(window, AddBrowser);
    require(IsWindowVisible(control(window, ChooseEdge)) != FALSE, "Add another browser must return to selection");
    require(SendMessageW(control(window, ChooseEdge), BM_GETCHECK, 0, 0) == BST_CHECKED,
            "Add another browser must select Edge after completing Chrome");
    require(searches == 0, "navigation must not open search prematurely");

    click(window, Primary);
    click(window, Primary);
    check_guide_page(window, 2);
    setup.show();
    check_guide_page(window, 2);
    require(GetFocus() != nullptr && IsWindowVisible(GetFocus()), "showing an existing guide must focus a visible control");
    click(window, Later);
    require(!IsWindowVisible(window), "postponing setup must hide its window");
    setup.show();
    require(IsWindowVisible(control(window, ChooseChrome)), "a hidden guide must reopen at browser selection for a new connection");
    require(!IsWindowVisible(control(window, OpenPage)) && !IsWindowVisible(control(window, FolderPath)) &&
            text(control(window, StepProgress)).find(L"第 1 步") != std::wstring::npos,
            "reopening a hidden guide must reset its actions and progress to browser selection");
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
    check_confirmation(window, L"Chrome", false);
    click(window, Primary);
    require(searches == 0, "manual registration must still wait for a real snapshot before searching");
    check_all_dpis(window);
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
    setup.show();
    require(IsWindowVisible(control(window, ChooseChrome)), "opening the guide after completion must start a new browser connection");
    require(!IsWindowVisible(control(window, ConnectionStatus)), "a new connection must not reopen the prior success page");
}

void confirmation_page_id_repair() {
    Sandbox sandbox;
    std::wstring error;
    require(glint::setup::register_hosts(kManualId, L"", error) == 0, "seed an independent Chrome registration");
    int searches = 0;
    glint::SetupWindow setup(nullptr, [&] { ++searches; });
    setup.show();
    const auto window = test_window();
    click(window, ChooseEdge);
    advance_to_confirmation(window);
    check_confirmation(window, L"Edge", false);
    click(window, Troubleshoot);
    require(GetDlgItem(window, RepairId) != nullptr, "the separate troubleshooting page must offer an extension ID repair field");
    require(IsWindowVisible(control(window, RepairId)) && IsWindowVisible(control(window, RegisterRepair)),
            "troubleshooting must expose ID repair without visiting advanced settings");
    require(text(control(window, RepairLabel)).find(L"ID") != std::wstring::npos &&
            text(control(window, RepairLabel)).find(L"页面地址") != std::wstring::npos,
            "repair label must explain the supported extension ID or page address");
    require(text(control(window, RegisterRepair)) == L"保存 ID 并重试", "repair must have an explicit save and retry action");
    const auto help = text(control(window, RepairHelp));
    require(help.find(L"回到此处粘贴") != std::wstring::npos && help.find(L"扩展管理") != std::wstring::npos &&
            help.find(L"32") != std::wstring::npos && help.find(L"Ctrl+L") != std::wstring::npos,
            "repair must explain how to copy a complete Glint page address or its 32-character ID");
    check_unique_control_ids(window);
    check_all_dpis(window);
    require(GetFocus() == control(window, RepairId), "troubleshooting must focus the repair field");
    click(window, BackToStatus);
    check_confirmation(window, L"Edge", false);
    click(window, Troubleshoot);
    click(window, Back);
    check_confirmation(window, L"Edge", false);
    click(window, Troubleshoot);
    const auto original_edge = glint::setup::read_registration().edge_id;
    for (const auto invalid : {L"", L"not-an-extension-id", L"Abcdefghijklmnopabcdefghijklmnop",
         L"https://example.test/chrome-extension://ponmlkjihgfedcbaponmlkjihgfedcba/options.html",
         L"chrome-extension://ponmlkjihgfedcbaponmlkjihgfedcba.evil/options.html",
         L"chrome-extension://ponmlkjihgfedcbaponmlkjihgfedcba:80/options.html",
         L"chrome-extension://ponmlkjihgfedcbaponmlkjihgfedcba",
         L"chrome-extension://ponmlkjihgfedcbaponmlkjihgfedcba/options.html extra"}) {
        SetWindowTextW(control(window, RepairId), invalid);
        click(window, RegisterRepair);
        const auto registration = glint::setup::read_registration();
        require(registration.edge_id == original_edge && registration.chrome_id == kManualId,
                "invalid repair ID must preserve both browser registrations");
        require(text(control(window, RepairResult)).find(L"32") != std::wstring::npos,
                "invalid repair ID must show the required format");
        require(searches == 0 && IsWindowVisible(control(window, RepairId)), "invalid ID must not unlock search or leave troubleshooting");
    }
    check_all_dpis(window);
    SetWindowTextW(control(window, RepairId), (std::wstring(L"  ") + kEdgeManualId + L"  ").c_str());
    click(window, RegisterRepair);
    auto registration = glint::setup::read_registration();
    require(registration.edge_current && registration.edge_id == kEdgeManualId && registration.chrome_id == kManualId,
            "repair must trim and register only the currently selected browser's ID");
    require(text(control(window, RepairId)) == kEdgeManualId, "successful repair must display the normalized extension ID");
    SetWindowTextW(control(window, RepairId), (std::wstring(L"  chrome-extension://") + kEdgeManualId + L"/options.html  ").c_str());
    click(window, RegisterRepair);
    require(text(control(window, RepairId)) == kEdgeManualId && glint::setup::read_registration().edge_id == kEdgeManualId,
            "repair must accept the trimmed full Glint options-page address and display its ID");
    std::ifstream manifest_input(sandbox.directory() / L"Glint" / L"com.glint.host.edge.json", std::ios::binary);
    require(manifest_input.good(), "open repaired Edge host manifest");
    const auto manifest = glint::Json::parse(std::string(std::istreambuf_iterator<char>(manifest_input), {}));
    const auto& origins = manifest.at("allowed_origins").as_array();
    require(origins.size() == 1 && origins.front().as_string() ==
            "chrome-extension://" + glint::wide_to_utf8(kEdgeManualId) + "/", "repair must authorize only the supplied ID");
    require(text(control(window, RepairResult)).find(L"已保存") != std::wstring::npos &&
            text(control(window, RepairResult)).find(L"等待") != std::wstring::npos,
            "registration success must explain that synchronization is still pending");
    require(IsWindowVisible(control(window, RepairId)) && searches == 0,
            "saving an ID must not report a connection or open search");
    check_all_dpis(window);
    glint::TabIndex index;
    require(index.apply(1, hello("Chrome")) && index.apply(1, snapshot()), "synchronize the other browser after repair");
    setup.update_connection(index.connections(), L"Ctrl+Shift+Space");
    require(IsWindowVisible(control(window, RepairId)) && searches == 0, "the other browser must not complete repaired setup");
    require(index.apply(2, hello("Edge")), "receive repaired Edge hello");
    setup.update_connection(index.connections(), L"Ctrl+Shift+Space");
    require(IsWindowVisible(control(window, RepairId)) && searches == 0, "a hello after repair must still wait for a snapshot");
    require(index.apply(2, snapshot()), "receive repaired Edge snapshot");
    setup.update_connection(index.connections(), L"Ctrl+Shift+Space");
    check_confirmation(window, L"Edge", true);
    require(!IsWindowVisible(control(window, RepairId)), "selected-browser synchronization must return troubleshooting to clean confirmation");
    check_all_dpis(window);
    require(index.disconnect(2), "disconnect repaired Edge");
    setup.update_connection(index.connections(), L"Ctrl+Shift+Space");
    check_confirmation(window, L"Edge", false);
    click(window, Primary);
    require(searches == 0, "disconnect must prevent repaired setup from opening search again");
    setup.update_connection({}, L"Ctrl+Shift+Space");
    for (int page = 4; page > 0; --page) click(window, Back);
    click(window, ChooseChrome);
    advance_to_confirmation(window);
    check_confirmation(window, L"Chrome", false);
    click(window, Troubleshoot);
    require(text(control(window, RepairId)).empty(), "switching browsers must clear the other browser's repair ID");
    SetWindowTextW(control(window, RepairId), (std::wstring(L"chrome-extension://") + kManualId + L"/options.html").c_str());
    click(window, RegisterRepair);
    registration = glint::setup::read_registration();
    require(registration.chrome_id == kManualId && registration.edge_id == kEdgeManualId,
            "repairing Chrome must preserve Edge's repaired ID");
    setup.update_connection(index.connections(), L"Ctrl+Shift+Space");
    check_confirmation(window, L"Chrome", true);
    click(window, Primary);
    require(searches == 1 && !IsWindowVisible(window), "a synchronized repaired browser must open search normally");
}
} // namespace

int main() {
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
        require(InitCommonControlsEx(&controls) != FALSE, "initialize native setup controls");
        automatic_edge_flow();
        manual_validation_and_completion();
        confirmation_page_id_repair();
        short_monitor_focus_and_scrolling();
        std::cout << "setup window tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "setup window test failed: " << error.what() << '\n';
        return 1;
    }
}
