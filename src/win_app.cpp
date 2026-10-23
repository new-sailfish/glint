#include "language.hpp"
#include "language_sync.hpp"
#include "win_app.hpp"

#include "glint/core.hpp"
#include "glint_version.h"
#include "connection_overview.hpp"
#include "registration.hpp"
#include "pipe_server.hpp"
#include "extension_bundle.hpp"
#include "setup_window.hpp"
#include "hotkey_window.hpp"
#include "update_controller.hpp"
#include "update_window.hpp"
#include "update_install.hpp"
#include "search_navigation.hpp"
#include "win_common.hpp"
#include "ui_theme.hpp"

#include <windows.h>
#include <commctrl.h>
#include <sddl.h>
#include <shellapi.h>

#include <atomic>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

namespace glint {
namespace {

constexpr UINT WM_PIPE_EVENT = WM_APP + 1;
constexpr UINT WM_TRAY_EVENT = WM_APP + 2;
constexpr UINT WM_SHOW_SETUP = WM_APP + 3;
constexpr UINT WM_SHOW_SEARCH = WM_APP + 4;
constexpr UINT WM_UPDATE_EVENT = WM_APP + 5;
constexpr UINT_PTR UPDATE_TIMER = 20;
constexpr UINT_PTR STARTUP_TIMER = 21;
constexpr UINT ID_TRAY_SHOW = 1001;
constexpr UINT ID_TRAY_HOTKEY = 1002;
constexpr UINT ID_TRAY_EXIT = 1003;
constexpr UINT ID_TRAY_SETUP = 1004;
constexpr UINT ID_LANGUAGE = 1005;
constexpr UINT ID_TRAY_UPDATE = 1006;
constexpr UINT ID_TRAY_INSTALL = 1007;
constexpr UINT ID_LANGUAGE_SYSTEM = 1010;
constexpr UINT ID_LANGUAGE_ENGLISH = 1011;
constexpr UINT ID_LANGUAGE_CHINESE = 1012;
constexpr UINT_PTR ID_LANGUAGE_RETRY = 2005;
constexpr wchar_t kClassName[] = L"Glint.SearchWindow";

class DesktopApp {
public:
    explicit DesktopApp(std::vector<std::wstring> warnings = {}) : pipe_([this](ConnectionId id, std::string message) { receive(id, std::move(message)); },
                         [this](ConnectionId id) { disconnected(id); },
                         [this](ConnectionId id, std::string request) { send_failed(id, std::move(request)); }), startup_warnings_(std::move(warnings)) {}
    ~DesktopApp() { shutdown(); }

    int run(bool open_setup) {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS};
        InitCommonControlsEx(&controls);
        if (!create_window()) return 1;
        pipe_.start();
        const auto update_window = window_;
        updater_ = std::make_unique<update::Controller>(GLINT_VERSION_STRING,
            std::filesystem::path(win::local_app_data()) / L"Glint" / L"updates" / L"downloads",
            [update_window] { PostMessageW(update_window, WM_UPDATE_EVENT, 0, 0); });
        SetTimer(window_, UPDATE_TIMER, 1000, nullptr);
        SetTimer(window_, STARTUP_TIMER, 50, nullptr);
        setup_ = std::make_unique<SetupWindow>(window_, [this] { show_search(); });
        if (open_setup) show_setup();
        else show_search();
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (update_window_ && update_window_->translate(message)) continue;
            if (hotkey_window_ && hotkey_window_->translate(message)) continue;
            // WM_DESTROY clears the owned windows before WM_QUIT is read;
            // previously posted messages can still be drained in between.
            if (setup_ && setup_->translate(message)) continue;
            if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE &&
                (message.hwnd == window_ || IsChild(window_, message.hwnd))) {
                hide_search(); continue;
            }
            if (message.message == WM_KEYDOWN && (GetFocus() == edit_ || GetFocus() == list_)) {
                if (message.wParam == VK_ESCAPE) { hide_search(); continue; }
                if (message.wParam == VK_RETURN) { activate_selected(); continue; }
                if (navigate_search_results(edit_, list_, static_cast<UINT>(message.wParam))) continue;
            }
            if (IsDialogMessageW(window_, &message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

private:
    HWND window_ = nullptr;
    HWND edit_ = nullptr;
    HWND list_ = nullptr;
    HWND status_ = nullptr;
    HWND connection_summary_ = nullptr;
    HWND connection_list_ = nullptr;
    HWND connection_hint_ = nullptr;
    HWND setup_button_ = nullptr;
    HWND heading_ = nullptr;
    HWND subtitle_ = nullptr;
    HWND settings_button_ = nullptr;
    HWND language_button_ = nullptr;
    HWND keyboard_hint_ = nullptr;
    HFONT font_ = nullptr;
    HFONT heading_font_ = nullptr;
    HFONT small_font_ = nullptr;
    HIMAGELIST row_spacing_ = nullptr;
    bool query_empty_ = true;
    UINT dpi_ = 96;
    std::unique_ptr<SetupWindow> setup_;
    std::unique_ptr<HotkeyWindow> hotkey_window_;
    std::unique_ptr<update::Controller> updater_;
    std::unique_ptr<UpdateWindow> update_window_;
    hotkey::Registration hotkey_;
    NOTIFYICONDATAW tray_{};
    UINT taskbar_created_ = 0;
    bool tray_added_ = false;
    TabIndex index_;
    ConnectionOverview connection_overview_;
    PipeServer pipe_;
    std::vector<std::wstring> startup_warnings_;
    std::mutex event_mutex_;
    std::vector<std::pair<ConnectionId, std::string>> events_;
    std::vector<std::vector<ConnectionInfo>> connection_events_;
    std::vector<Tab> rows_;
    std::atomic<std::uint64_t> request_counter_ = 1;

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<DesktopApp*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->window_ = hwnd;
        }
        if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
        if (message == self->taskbar_created_) { self->tray_added_ = false; self->add_tray(); return 0; }
        switch (message) {
        case WM_SETTINGCHANGE: {
            const auto previous = i18n::current_language.load();
            i18n::refresh();
            if (previous != i18n::current_language) {
                self->refresh_language();
                self->sync_extension_languages();
            }
            break;
        }
        case WM_SHOW_SETUP: self->show_setup(); return 0;
        case WM_SHOW_SEARCH: self->show_search(); return 0;
        case WM_UPDATE_EVENT: self->update_changed(); return 0;
        case WM_TIMER:
            if (wparam == ID_LANGUAGE_RETRY) { self->sync_extension_languages(); return 0; }
            if (wparam == UPDATE_TIMER && self->updater_) self->updater_->tick();
            if (wparam == STARTUP_TIMER && self->pipe_.ready()) {
                KillTimer(hwnd, STARTUP_TIMER);
                update::acknowledge_update_startup();
                if (!self->startup_warnings_.empty()) {
                    std::wstring details;
                    for (const auto& warning : self->startup_warnings_) { if (!details.empty()) details += L"\r\n\r\n"; details += warning; }
                    self->startup_warnings_.clear();
                    MessageBoxW(hwnd, details.c_str(), L"Glint", MB_OK | MB_ICONINFORMATION);
                }
            }
            return 0;
        case WM_HOTKEY:
            if (self->hotkey_.active() && wparam == static_cast<WPARAM>(self->hotkey_.id()) &&
                LOWORD(lparam) == self->hotkey_.current().modifiers && HIWORD(lparam) == self->hotkey_.current().key) {
                if (self->hotkey_window_ && self->hotkey_window_->visible())
                    self->hotkey_window_->record_registered(self->hotkey_.current());
                else self->show_search();
            }
            return 0;
        case WM_COMMAND:
            if (HIWORD(wparam) == EN_CHANGE && reinterpret_cast<HWND>(lparam) == self->edit_) { self->refresh_results(); return 0; }
            if ((HIWORD(wparam) == EN_SETFOCUS || HIWORD(wparam) == EN_KILLFOCUS) &&
                reinterpret_cast<HWND>(lparam) == self->edit_) {
                InvalidateRect(hwnd, nullptr, FALSE); return 0;
            }
            if (LOWORD(wparam) == ID_TRAY_SHOW) { self->show_search(); return 0; }
            if (LOWORD(wparam) == ID_TRAY_HOTKEY) { self->configure_hotkey(); return 0; }
            if (LOWORD(wparam) == ID_TRAY_SETUP) { self->show_setup(); return 0; }
            if (LOWORD(wparam) == ID_TRAY_UPDATE) { self->show_updates(true); return 0; }
            if (LOWORD(wparam) == ID_TRAY_INSTALL) { self->show_updates(false); if (self->updater_) self->updater_->install(); return 0; }
            if (LOWORD(wparam) == ID_TRAY_EXIT) { DestroyWindow(hwnd); return 0; }
            if (LOWORD(wparam) == ID_LANGUAGE) { self->show_language_menu(); return 0; }
            if (LOWORD(wparam) >= ID_LANGUAGE_SYSTEM && LOWORD(wparam) <= ID_LANGUAGE_CHINESE) {
                self->change_language(static_cast<i18n::Preference>(LOWORD(wparam) - ID_LANGUAGE_SYSTEM)); return 0;
            }
            break;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) { self->hide_search(); return 0; }
            if (wparam == VK_RETURN && GetFocus() == self->list_) { self->activate_selected(); return 0; }
            break;
        case WM_NOTIFY:
            if (reinterpret_cast<NMHDR*>(lparam)->code == NM_DBLCLK &&
                reinterpret_cast<NMHDR*>(lparam)->hwndFrom == self->list_) {
                self->activate_selected(); return 0;
            }
            if (reinterpret_cast<NMHDR*>(lparam)->code == NM_CUSTOMDRAW) {
                const auto* header = reinterpret_cast<NMHDR*>(lparam);
                if (header->hwndFrom == self->list_ || header->hwndFrom == self->connection_list_) {
                    auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lparam);
                    return self->draw_list(*draw);
                }
            }
            break;
        case WM_ERASEBKGND: {
            self->paint(reinterpret_cast<HDC>(wparam));
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{}; HDC dc = BeginPaint(hwnd, &paint);
            self->paint(dc); EndPaint(hwnd, &paint); return 0;
        }
        case WM_PRINTCLIENT: self->paint(reinterpret_cast<HDC>(wparam)); return 0;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            const HWND control = reinterpret_cast<HWND>(lparam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, control == self->status_ || control == self->connection_hint_ ||
                control == self->subtitle_ || control == self->keyboard_hint_ ? ui::muted() : ui::ink());
            return reinterpret_cast<LRESULT>(ui::background_brush());
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, ui::ink()); SetBkColor(dc, ui::surface());
            return reinterpret_cast<LRESULT>(ui::surface_brush());
        }
        case WM_SIZE:
            self->layout(); return 0;
        case WM_DPICHANGED: {
            self->dpi_ = HIWORD(wparam);
            self->update_font();
            const auto* rect = reinterpret_cast<RECT*>(lparam);
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromRect(rect, MONITOR_DEFAULTTONEAREST), &monitor);
            const auto& work = monitor.rcWork;
            const int width = (std::min)(rect->right - rect->left, work.right - work.left);
            const int height = (std::min)(rect->bottom - rect->top, work.bottom - work.top);
            const int x = (std::clamp)(rect->left, work.left, work.right - width);
            const int y = (std::clamp)(rect->top, work.top, work.bottom - height);
            SetWindowPos(hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
            self->layout(); return 0;
        }
        case WM_CLOSE: self->hide_search(); return 0;
        case WM_DESTROY:
            self->shutdown(); PostQuitMessage(0); return 0;
        case WM_PIPE_EVENT: self->drain_events(); return 0;
        case WM_TRAY_EVENT:
            if (lparam == NIN_BALLOONUSERCLICK) { self->show_updates(false); return 0; }
            if (lparam == WM_LBUTTONUP || lparam == WM_LBUTTONDBLCLK) self->show_search();
            else if (lparam == WM_RBUTTONUP) self->show_tray_menu();
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    bool create_window() {
        taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
        WNDCLASSEXW klass{sizeof(klass)};
        klass.lpfnWndProc = window_proc;
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        klass.hIcon = ui::icon(klass.hInstance);
        klass.hIconSm = ui::icon(klass.hInstance);
        klass.hbrBackground = ui::background_brush();
        klass.lpszClassName = kClassName;
        if (!RegisterClassExW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        dpi_ = GetDpiForSystem();
        RECT frame{0, 0, px(700), px(520)};
        constexpr DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
        AdjustWindowRectExForDpi(&frame, style, FALSE, WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT, dpi_);
        POINT point{}; GetCursorPos(&point);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTOPRIMARY), &monitor);
        const auto& work = monitor.rcWork;
        const int width = (std::min)(frame.right - frame.left, work.right - work.left);
        const int height = (std::min)(frame.bottom - frame.top, work.bottom - work.top);
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT, kClassName, L"Glint " GLINT_VERSION_WSTRING, style,
            work.left + (work.right - work.left - width) / 2,
            work.top + (work.bottom - work.top - height) / 2,
            width, height, nullptr, nullptr, klass.hInstance, this);
        if (!window_) return false;
        // The cursor's monitor can use a different scale from the system's
        // primary monitor. Resolve the hidden window's actual DPI before
        // creating controls, then size its client area using that same scale.
        dpi_ = GetDpiForWindow(window_);
        if (!dpi_) dpi_ = 96;
        frame = {0, 0, px(700), px(520)};
        AdjustWindowRectExForDpi(&frame, style, FALSE, WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT, dpi_);
        const int actual_width = (std::min)(frame.right - frame.left, work.right - work.left);
        const int actual_height = (std::min)(frame.bottom - frame.top, work.bottom - work.top);
        SetWindowPos(window_, nullptr,
            work.left + (work.right - work.left - actual_width) / 2,
            work.top + (work.bottom - work.top - actual_height) / 2,
            actual_width, actual_height, SWP_NOZORDER | SWP_NOACTIVATE);
        heading_ = CreateWindowExW(0, L"STATIC", L"Glint", WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, window_, nullptr, klass.hInstance, nullptr);
        subtitle_ = CreateWindowExW(0, L"STATIC", i18n::text(L"Find your open tabs", L"标签页，随手找到"), WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS,
            0, 0, 0, 0, window_, nullptr, klass.hInstance, nullptr);
        settings_button_ = CreateWindowExW(0, L"BUTTON", i18n::text(L"Shortcut", L"快捷键设置"), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_TRAY_HOTKEY)), klass.hInstance, nullptr);
        language_button_ = CreateWindowExW(0, L"BUTTON", L"English / 中文", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LANGUAGE)), klass.hInstance, nullptr);
        keyboard_hint_ = CreateWindowExW(0, L"STATIC", i18n::text(L"↑ ↓ Select     Enter Switch     Esc Hide", L"↑ ↓ 选择     Enter 切换     Esc 收起"), WS_CHILD | WS_VISIBLE | SS_RIGHT,
            0, 0, 0, 0, window_, nullptr, klass.hInstance, nullptr);
        edit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                12, 12, 860, 30, window_, nullptr, klass.hInstance, nullptr);
        list_ = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS,
                                12, 52, 860, 420, window_, nullptr, klass.hInstance, nullptr);
        status_ = CreateWindowExW(0, L"STATIC", i18n::text(L"0 tabs", L"0 个标签页"), WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS,
                                   12, 478, 860, 24, window_, nullptr, klass.hInstance, nullptr);
        connection_summary_ = CreateWindowExW(0, L"STATIC", i18n::text(L"0 profiles connected", L"已连接 0 个配置文件"), WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS,
            12, 478, 680, 24, window_, nullptr, klass.hInstance, nullptr);
        setup_button_ = CreateWindowExW(0, L"BUTTON", i18n::text(L"Connect browser", L"连接新的浏览器"), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            752, 478, 120, 26, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_TRAY_SETUP)), klass.hInstance, nullptr);
        connection_list_ = CreateWindowExW(0, WC_LISTVIEWW, i18n::text(L"Browser connections", L"浏览器连接状态"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS,
            12, 478, 860, 100, window_, nullptr, klass.hInstance, nullptr);
        connection_hint_ = CreateWindowExW(0, L"STATIC", i18n::text(L"Click “Connect browser” to add Chrome or Edge.", L"点击“连接新的浏览器”，按引导添加 Chrome 或 Edge。"),
            WS_CHILD | WS_VISIBLE | SS_CENTER, 24, 510, 830, 24, window_, nullptr, klass.hInstance, nullptr);
        update_font();
        ui::style_button(setup_button_, true);
        ui::style_button(settings_button_);
        ui::style_button(language_button_);
        SendMessageW(edit_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(i18n::text(L"Search tab titles or URLs", L"搜索标签页标题或网址")));
        ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        ListView_SetExtendedListViewStyle(connection_list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        ListView_SetBkColor(list_, ui::surface());
        ListView_SetTextBkColor(list_, ui::surface());
        ListView_SetTextColor(list_, ui::ink());
        ListView_SetBkColor(connection_list_, ui::surface());
        ListView_SetTextBkColor(connection_list_, ui::surface());
        ListView_SetTextColor(connection_list_, ui::ink());
        const wchar_t* headers[] = {i18n::text(L"Title", L"标题"), i18n::text(L"URL", L"网址"), i18n::text(L"Browser / Profile", L"浏览器 / 配置")};
        const int widths[] = {280, 420, 170};
        for (int i = 0; i != 3; ++i) { LVCOLUMNW column{LVCF_TEXT | LVCF_WIDTH}; column.pszText = const_cast<LPWSTR>(headers[i]); column.cx = widths[i]; ListView_InsertColumn(list_, i, &column); }
        const wchar_t* connection_headers[] = {i18n::text(L"Browser / Profile", L"浏览器 / 配置"), i18n::text(L"Status", L"连接状态"), i18n::text(L"Tabs", L"标签页")};
        for (int i = 0; i != 3; ++i) {
            LVCOLUMNW column{LVCF_TEXT | LVCF_WIDTH};
            column.pszText = const_cast<LPWSTR>(connection_headers[i]);
            column.cx = px(i == 0 ? 560 : 140);
            ListView_InsertColumn(connection_list_, i, &column);
        }
        std::wstring hotkey_error;
        if (!hotkey_.start(window_, hotkey::load(), hotkey_error)) {
            startup_warnings_.push_back(hotkey_error + i18n::text(L"\nChoose “Configure shortcut” in the tray menu to change it.", L"\n可从托盘打开“配置全局热键”修改。"));
        }
        tray_.cbSize = sizeof(tray_); tray_.hWnd = window_; tray_.uID = 1; tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; tray_.uCallbackMessage = WM_TRAY_EVENT;
        tray_.hIcon = ui::icon(klass.hInstance); wcscpy_s(tray_.szTip, (std::wstring(L"Glint " GLINT_VERSION_WSTRING) + i18n::text(L" · Tab search", L" · 标签页搜索")).c_str()); add_tray();
        layout();
        return true;
    }

    void add_tray() { if (!tray_added_ && window_ && Shell_NotifyIconW(NIM_ADD, &tray_)) tray_added_ = true; }
    void remove_tray() { if (tray_added_) { Shell_NotifyIconW(NIM_DELETE, &tray_); tray_added_ = false; } }

    int px(int value) const { return MulDiv(value, dpi_, 96); }

    void update_font() {
        LOGFONTW description{};
        description.lfHeight = -MulDiv(10, dpi_, 72);
        description.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(description.lfFaceName, L"Microsoft YaHei UI");
        HFONT next = CreateFontIndirectW(&description);
        if (!next) return;
        for (const auto control : {edit_, list_, status_, connection_summary_, connection_list_, connection_hint_, setup_button_})
            if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
        if (font_) DeleteObject(font_);
        font_ = next;
        description.lfHeight = -MulDiv(17, dpi_, 72);
        description.lfWeight = FW_SEMIBOLD;
        HFONT next_heading = CreateFontIndirectW(&description);
        if (next_heading) {
            SendMessageW(heading_, WM_SETFONT, reinterpret_cast<WPARAM>(next_heading), TRUE);
            if (heading_font_) DeleteObject(heading_font_);
            heading_font_ = next_heading;
        }
        description.lfHeight = -MulDiv(9, dpi_, 72);
        description.lfWeight = FW_NORMAL;
        HFONT next_small = CreateFontIndirectW(&description);
        if (next_small) {
            for (const auto control : {subtitle_, keyboard_hint_, status_, settings_button_, language_button_})
                if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(next_small), TRUE);
            if (small_font_) DeleteObject(small_font_);
            small_font_ = next_small;
        }
        HIMAGELIST spacing = ImageList_Create(1, px(32), ILC_COLOR32, 1, 1);
        if (spacing) {
            ListView_SetImageList(list_, spacing, LVSIL_SMALL);
            ListView_SetImageList(connection_list_, spacing, LVSIL_SMALL);
            if (row_spacing_) ImageList_Destroy(row_spacing_);
            row_spacing_ = spacing;
        }
    }

    void layout() {
        if (!window_ || !edit_) return;
        RECT rect{}; GetClientRect(window_, &rect);
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        const int content_width = (std::max)(px(200), width - px(40));
        const bool compact = height < px(420);
        const bool short_viewport = height < px(350);
        const int results_top = px(compact ? 112 : 132);
        const int overview_top = height - px(short_viewport ? 114 : 146);
        const int results_height = (std::max)(px(42), overview_top - results_top - px(32));
        MoveWindow(heading_, px(64), px(14), (std::min)(px(140), width - px(190)), px(29), TRUE);
        MoveWindow(subtitle_, px(64), px(44), (std::min)(px(240), width - px(200)), px(20), TRUE);
        ShowWindow(subtitle_, compact ? SW_HIDE : SW_SHOW);
        MoveWindow(settings_button_, width - px(126), px(22), px(106), px(30), TRUE);
        MoveWindow(language_button_, width - px(276), px(22), px(140), px(30), TRUE);
        MoveWindow(edit_, px(54), px(compact ? 69 : 84), content_width - px(46), px(24), TRUE);
        MoveWindow(list_, px(20), results_top, content_width, results_height, TRUE);
        MoveWindow(status_, px(20), results_top + results_height + px(6), px(210), px(22), TRUE);
        MoveWindow(keyboard_hint_, px(230), results_top + results_height + px(6), (std::max)(px(1), content_width - px(210)), px(22), TRUE);
        ShowWindow(keyboard_hint_, width >= px(540) ? SW_SHOW : SW_HIDE);
        MoveWindow(connection_summary_, px(20), overview_top + px(7), content_width - px(180), px(24), TRUE);
        MoveWindow(setup_button_, width - px(170), overview_top, px(150), px(32), TRUE);
        MoveWindow(connection_list_, px(20), overview_top + px(42), content_width, px(short_viewport ? 60 : 90), TRUE);
        MoveWindow(connection_hint_, px(30), overview_top + px(short_viewport ? 64 : 82), content_width - px(20), px(22), TRUE);
        const int available = (std::max)(px(120), content_width - GetSystemMetricsForDpi(SM_CXVSCROLL, dpi_));
        ListView_SetColumnWidth(list_, 0, available * 44 / 100);
        ListView_SetColumnWidth(list_, 1, available * 34 / 100);
        ListView_SetColumnWidth(list_, 2, available - available * 44 / 100 - available * 34 / 100);
        const int state_width = (std::min)(px(140), available * 28 / 100);
        const int count_width = (std::min)(px(80), available * 17 / 100);
        ListView_SetColumnWidth(connection_list_, 0, available - state_width - count_width);
        ListView_SetColumnWidth(connection_list_, 1, state_width);
        ListView_SetColumnWidth(connection_list_, 2, count_width);
        InvalidateRect(window_, nullptr, FALSE);
    }

    void paint(HDC dc) const {
        ui::paint_background(window_, dc);
        RECT client{}; GetClientRect(window_, &client);
        const bool compact = client.bottom < px(420);
        const int search_top = px(compact ? 62 : 74);
        RECT logo{px(20), px(22), px(52), px(54)}; ui::paint_logo(dc, logo);
        // The edit remains a native, borderless EDIT inside a painted search
        // surface, preserving IME, caret, selection and keyboard behavior.
        const int saved = SaveDC(dc);
        HPEN pen = CreatePen(PS_SOLID, px(1), GetFocus() == edit_ ? ui::accent() : ui::line());
        SelectObject(dc, pen); SelectObject(dc, ui::surface_brush());
        RoundRect(dc, px(20), search_top, client.right - px(20), search_top + px(compact ? 38 : 44), px(12), px(12));
        HPEN search_pen = CreatePen(PS_SOLID, px(1), ui::muted());
        SelectObject(dc, search_pen); SelectObject(dc, GetStockObject(NULL_BRUSH));
        const int search_center = search_top + px(compact ? 12 : 15);
        Ellipse(dc, px(34), search_center, px(44), search_center + px(10));
        MoveToEx(dc, px(43), search_center + px(9), nullptr); LineTo(dc, px(47), search_center + px(13));
        RestoreDC(dc, saved); DeleteObject(search_pen); DeleteObject(pen);
    }

    LRESULT draw_list(NMLVCUSTOMDRAW& draw) const {
        if (ui::high_contrast()) return CDRF_DODEFAULT;
        const bool results = draw.nmcd.hdr.hwndFrom == list_;
        if (draw.nmcd.dwDrawStage == CDDS_PREPAINT)
            return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
        if (draw.nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW | CDRF_NOTIFYPOSTPAINT;
        if (draw.nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
            const int item = static_cast<int>(draw.nmcd.dwItemSpec);
            const bool selected = (ListView_GetItemState(draw.nmcd.hdr.hwndFrom, item, LVIS_SELECTED) & LVIS_SELECTED) != 0;
            draw.clrTextBk = selected ? ui::wash() : item % 2 ? RGB(249, 251, 255) : ui::surface();
            draw.clrText = !results || draw.iSubItem == 0 ? ui::ink() : ui::muted();
            if (!results && draw.iSubItem == 1) {
                const auto& profiles = connection_overview_.profiles();
                if (item >= 0 && static_cast<std::size_t>(item) < profiles.size()) {
                    const auto state = profiles[static_cast<std::size_t>(item)].state;
                    draw.clrText = state == ProfileConnectionState::connected ? ui::success() :
                        state == ProfileConnectionState::synchronizing ? ui::warning() : ui::muted();
                }
            }
            const HWND list = draw.nmcd.hdr.hwndFrom;
            RECT cell{}; ListView_GetSubItemRect(list, item, draw.iSubItem, LVIR_BOUNDS, &cell);
            if (draw.iSubItem == 0) cell.right = cell.left + ListView_GetColumnWidth(list, 0);
            const int saved = SaveDC(draw.nmcd.hdc);
            IntersectClipRect(draw.nmcd.hdc, cell.left, cell.top, cell.right, cell.bottom);
            HBRUSH brush = CreateSolidBrush(draw.clrTextBk);
            FillRect(draw.nmcd.hdc, &cell, brush); DeleteObject(brush);
            wchar_t text[2048]{}; ListView_GetItemText(list, item, draw.iSubItem, text, 2048);
            SetBkMode(draw.nmcd.hdc, TRANSPARENT); SetTextColor(draw.nmcd.hdc, draw.clrText);
            SelectObject(draw.nmcd.hdc, font_);
            cell.left += px(9); cell.right -= px(7);
            DrawTextW(draw.nmcd.hdc, text, -1, &cell, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            RestoreDC(draw.nmcd.hdc, saved);
            return CDRF_SKIPDEFAULT;
        }
        if (draw.nmcd.dwDrawStage == CDDS_ITEMPOSTPAINT && GetFocus() == draw.nmcd.hdr.hwndFrom &&
            (ListView_GetItemState(draw.nmcd.hdr.hwndFrom, static_cast<int>(draw.nmcd.dwItemSpec), LVIS_FOCUSED) & LVIS_FOCUSED)) {
            RECT focus = draw.nmcd.rc; InflateRect(&focus, -px(1), -px(1)); DrawFocusRect(draw.nmcd.hdc, &focus);
            return CDRF_DODEFAULT;
        }
        if (draw.nmcd.dwDrawStage == CDDS_POSTPAINT && results && rows_.empty()) {
            RECT bounds{}; GetClientRect(list_, &bounds);
            bounds.top += px(42); bounds.left += px(24); bounds.right -= px(24);
            SetTextColor(draw.nmcd.hdc, ui::muted()); SetBkMode(draw.nmcd.hdc, TRANSPARENT);
            const auto previous = SelectObject(draw.nmcd.hdc, font_);
            const wchar_t* message = !query_empty_ ? i18n::text(L"No matching tabs.\nTry another title keyword or URL.", L"没有找到匹配的标签页。\n换个标题关键词，或试试网址。")
                : connection_overview_.connected_count() ? i18n::text(L"No open tabs in the connected browsers.", L"当前浏览器还没有打开标签页。")
                : i18n::text(L"Connect Chrome or Edge to see your tabs here.", L"连接 Chrome 或 Edge，让标签页出现在这里。");
            DrawTextW(draw.nmcd.hdc, message, -1, &bounds, DT_CENTER | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(draw.nmcd.hdc, previous);
        }
        return CDRF_DODEFAULT;
    }

    void show_search() {
        ShowWindow(window_, SW_SHOW); SetForegroundWindow(window_); SetFocus(edit_); SendMessageW(edit_, EM_SETSEL, 0, -1); refresh_results();
    }
    void hide_search() { ShowWindow(window_, SW_HIDE); }

    std::wstring active_hotkey_name() const {
        return hotkey_.active() ? hotkey::name(hotkey_.current()) : std::wstring{};
    }

    void show_setup() {
        if (!setup_) return;
        setup_->update_connection(index_.connections(), active_hotkey_name());
        setup_->show();
    }

    void show_tray_menu() {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, ID_TRAY_SHOW, i18n::text(L"Show search", L"显示搜索"));
        AppendMenuW(menu, MF_STRING, ID_TRAY_SETUP, i18n::text(L"Connect browser (setup)", L"连接新的浏览器（安装引导）"));
        AppendMenuW(menu, MF_STRING, ID_TRAY_HOTKEY, i18n::text(L"Configure shortcut", L"配置全局热键"));
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(language_menu()), L"Language / 语言");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_TRAY_UPDATE, i18n::text(L"Software updates…", L"软件更新…"));
        if (updater_ && updater_->snapshot().state == update::State::Ready) {
            const auto version = updater_->snapshot().target_version;
            const auto label = std::wstring(i18n::text(L"Restart and update to ", L"重启并升级到 ")) + std::wstring(version.begin(), version.end());
            AppendMenuW(menu, MF_STRING, ID_TRAY_INSTALL, label.c_str());
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, i18n::text(L"Exit Glint", L"退出 Glint"));
        POINT point{}; GetCursorPos(&point); SetForegroundWindow(window_); TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr); DestroyMenu(menu);
    }

    void show_updates(bool check) {
        if (!updater_) return;
        if (!update_window_) update_window_ = std::make_unique<UpdateWindow>(window_,
            [this] { updater_->check(); }, [this] { updater_->install(); },
            [this](bool enabled) { updater_->set_automatic(enabled); });
        update_window_->show(updater_->snapshot());
        if (check) updater_->check();
    }

    void update_changed() {
        if (!updater_) return;
        const auto state = updater_->snapshot();
        if (update_window_) update_window_->refresh(state);
        if (state.notify_ready && tray_added_) {
            tray_.uFlags |= NIF_INFO;
            wcscpy_s(tray_.szInfoTitle, i18n::text(L"Glint update ready", L"Glint 更新已准备好"));
            wcscpy_s(tray_.szInfo, i18n::text(L"Click to review the update and choose when to restart.", L"点击查看更新，由你决定何时重启安装。"));
            tray_.dwInfoFlags = NIIF_INFO;
            if (Shell_NotifyIconW(NIM_MODIFY, &tray_)) updater_->notified();
            tray_.uFlags &= ~NIF_INFO;
        }
        if (state.exit_to_install) DestroyWindow(window_);
    }

    HMENU language_menu() const {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, ID_LANGUAGE_SYSTEM, i18n::text(L"Follow system", L"跟随系统"));
        AppendMenuW(menu, MF_STRING, ID_LANGUAGE_ENGLISH, L"English");
        AppendMenuW(menu, MF_STRING, ID_LANGUAGE_CHINESE, L"中文");
        CheckMenuRadioItem(menu, ID_LANGUAGE_SYSTEM, ID_LANGUAGE_CHINESE,
            ID_LANGUAGE_SYSTEM + static_cast<UINT>(i18n::current_preference), MF_BYCOMMAND);
        return menu;
    }

    void show_language_menu() {
        HMENU menu = language_menu();
        RECT bounds{}; GetWindowRect(language_button_, &bounds);
        TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, bounds.left, bounds.bottom, 0, window_, nullptr);
        DestroyMenu(menu);
    }

    void change_language(i18n::Preference preference) {
        std::wstring error;
        if (!i18n::save_preference(preference, error)) {
            MessageBoxW(window_, error.c_str(), L"Glint", MB_OK | MB_ICONWARNING); return;
        }
        refresh_language();
        sync_extension_languages();
    }

    void sync_extension_languages() {
        const auto connections = index_.connections();
        const bool queued = send_language_settings(connections, i18n::current_preference, i18n::current_language.load(),
            [this](ConnectionId id, const std::string& message) { return pipe_.send(id, message); });
        if (queued) KillTimer(window_, ID_LANGUAGE_RETRY);
        else SetTimer(window_, ID_LANGUAGE_RETRY, 1000, nullptr);
    }

    void refresh_language() {
        SetWindowTextW(subtitle_, i18n::text(L"Find your open tabs", L"标签页，随手找到"));
        SetWindowTextW(settings_button_, i18n::text(L"Shortcut", L"快捷键设置"));
        SetWindowTextW(keyboard_hint_, i18n::text(L"↑ ↓ Select     Enter Switch     Esc Hide", L"↑ ↓ 选择     Enter 切换     Esc 收起"));
        SetWindowTextW(setup_button_, i18n::text(L"Connect browser", L"连接新的浏览器"));
        SetWindowTextW(connection_list_, i18n::text(L"Browser connections", L"浏览器连接状态"));
        SetWindowTextW(connection_hint_, i18n::text(L"Click “Connect browser” to add Chrome or Edge.", L"点击“连接新的浏览器”，按引导添加 Chrome 或 Edge。"));
        SendMessageW(edit_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(i18n::text(L"Search tab titles or URLs", L"搜索标签页标题或网址")));
        const wchar_t* headers[] = {i18n::text(L"Title", L"标题"), i18n::text(L"URL", L"网址"), i18n::text(L"Browser / Profile", L"浏览器 / 配置")};
        const wchar_t* connections[] = {headers[2], i18n::text(L"Status", L"连接状态"), i18n::text(L"Tabs", L"标签页")};
        for (int i = 0; i < 3; ++i) {
            LVCOLUMNW column{LVCF_TEXT};
            column.pszText = const_cast<LPWSTR>(headers[i]); ListView_SetColumn(list_, i, &column);
            column.pszText = const_cast<LPWSTR>(connections[i]); ListView_SetColumn(connection_list_, i, &column);
        }
        wcscpy_s(tray_.szTip, (std::wstring(L"Glint " GLINT_VERSION_WSTRING) + i18n::text(L" · Tab search", L" · 标签页搜索")).c_str());
        if (tray_added_) Shell_NotifyIconW(NIM_MODIFY, &tray_);
        if (setup_) {
            setup_->refresh_language();
            setup_->update_connection(index_.connections(), active_hotkey_name());
        }
        if (hotkey_window_) hotkey_window_->refresh_language();
        if (update_window_ && updater_) update_window_->refresh(updater_->snapshot());
        // Translation does not change the query or connection data. Updating
        // cells in place preserves selection, keyboard focus and scroll.
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            auto name = connection_overview_.name_for(rows_[i].key.connection_id);
            if (name.empty()) name = rows_[i].browser;
            ListView_SetItemText(list_, static_cast<int>(i), 2, name.data());
        }
        const auto& profiles = connection_overview_.profiles();
        for (std::size_t i = 0; i < profiles.size(); ++i) {
            auto name = profiles[i].name();
            ListView_SetItemText(connection_list_, static_cast<int>(i), 0, name.data());
            const auto* state = profiles[i].state == ProfileConnectionState::connected ? i18n::text(L"Connected", L"已连接")
                : profiles[i].state == ProfileConnectionState::synchronizing ? i18n::text(L"Syncing", L"正在同步") : i18n::text(L"Disconnected", L"未连接");
            ListView_SetItemText(connection_list_, static_cast<int>(i), 1, const_cast<LPWSTR>(state));
        }
        update_result_status();
        update_connection_summary();
        InvalidateRect(list_, nullptr, FALSE);
    }

    void configure_hotkey() {
        if (!hotkey_window_) {
            hotkey_window_ = std::make_unique<HotkeyWindow>(window_, [this](hotkey::Binding binding, std::wstring& error) {
                if (!hotkey_.change(binding, error)) return false;
                if (setup_) setup_->update_connection(index_.connections(), active_hotkey_name());
                return true;
            });
        }
        hotkey_window_->show(hotkey_.current());
    }

    void refresh_results() {
        if (!edit_ || !list_) return;
        refresh_connections();
        const int length = GetWindowTextLengthW(edit_);
        std::wstring query(static_cast<std::size_t>(length) + 1, L'\0');
        if (length) GetWindowTextW(edit_, query.data(), length + 1);
        query.resize(static_cast<std::size_t>(length));
        query_empty_ = query.empty();
        rows_ = index_.search(query);
        SendMessageW(list_, WM_SETREDRAW, FALSE, 0); ListView_DeleteAllItems(list_);
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            const auto& row = rows_[i];
            LVITEMW item{LVIF_TEXT, static_cast<int>(i), 0, 0, 0, const_cast<LPWSTR>(row.title.c_str()), 0, 0, 0};
            ListView_InsertItem(list_, &item);
            ListView_SetItemText(list_, static_cast<int>(i), 1, const_cast<LPWSTR>(row.url.c_str()));
            std::wstring source = connection_overview_.name_for(row.key.connection_id);
            if (source.empty()) source = row.browser;
            ListView_SetItemText(list_, static_cast<int>(i), 2, const_cast<LPWSTR>(source.c_str()));
        }
        SendMessageW(list_, WM_SETREDRAW, TRUE, 0); InvalidateRect(list_, nullptr, FALSE);
        update_result_status();
    }

    void update_result_status() {
        std::wstring text;
        if (rows_.empty() && !query_empty_) text = i18n::text(L"No matching tabs", L"没有匹配结果");
        else text = std::to_wstring(rows_.size()) + i18n::text(L" tabs", L" 个标签页");
        SetWindowTextW(status_, text.c_str());
    }

    void update_connection_summary() {
        const auto& profiles = connection_overview_.profiles();
        const auto connected = connection_overview_.connected_count();
        const auto synchronizing = static_cast<std::size_t>(std::count_if(profiles.begin(), profiles.end(), [](const auto& profile) {
            return profile.state == ProfileConnectionState::synchronizing;
        }));
        std::wstring summary = i18n::text(L"Connected: ", L"已连接 ") + std::to_wstring(connected) + i18n::text(L" profiles", L" 个配置文件");
        if (synchronizing) summary += L" · " + std::to_wstring(synchronizing) + i18n::text(L" syncing", L" 个同步中");
        if (profiles.size() > connected + synchronizing)
            summary += L" · " + std::to_wstring(profiles.size() - connected - synchronizing) + i18n::text(L" disconnected", L" 个未连接");
        SetWindowTextW(connection_summary_, summary.c_str());
    }

    void refresh_connections() {
        const auto connections = index_.connections();
        connection_overview_.update(connections);
        const auto& profiles = connection_overview_.profiles();
        update_connection_summary();
        ShowWindow(connection_hint_, profiles.empty() ? SW_SHOW : SW_HIDE);
        SendMessageW(connection_list_, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(connection_list_);
        for (std::size_t i = 0; i < profiles.size(); ++i) {
            const auto& profile = profiles[i];
            std::wstring name = profile.name();
            LVITEMW item{LVIF_TEXT, static_cast<int>(i), 0, 0, 0, name.data(), 0, 0, 0};
            ListView_InsertItem(connection_list_, &item);
            const wchar_t* state = profile.state == ProfileConnectionState::connected ? i18n::text(L"Connected", L"已连接")
                : profile.state == ProfileConnectionState::synchronizing ? i18n::text(L"Syncing", L"正在同步") : i18n::text(L"Disconnected", L"未连接");
            ListView_SetItemText(connection_list_, static_cast<int>(i), 1, const_cast<LPWSTR>(state));
            std::wstring tabs = profile.state == ProfileConnectionState::connected ? std::to_wstring(profile.tab_count) : L"—";
            ListView_SetItemText(connection_list_, static_cast<int>(i), 2, tabs.data());
        }
        SendMessageW(connection_list_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(connection_list_, nullptr, FALSE);
    }

    void activate_selected() {
        const int selected = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
        if (selected < 0 || static_cast<std::size_t>(selected) >= rows_.size()) return;
        const auto& tab = rows_[static_cast<std::size_t>(selected)];
        const std::string request = std::to_string(request_counter_++);
        const Json message = Json::Object{{"type", "activate"}, {"version", static_cast<int>(kProtocolVersion)},
            {"requestId", request}, {"tabId", tab.key.tab_id}, {"windowId", tab.window_id}};
        // Only the foreground input recipient can delegate this permission.
        // Do it before hiding Glint or handing the request to the pipe worker.
        // Failure is not conclusive: the browser may already own permission;
        // the extension verifies actual window focus before reporting success.
        pipe_.allow_foreground(tab.key.connection_id);
        // Hiding the foreground search window lets Windows activate the window
        // beneath it. Finish that handoff before the pipe worker can ask the
        // browser to raise the target, or the fallback can cover it again.
        hide_search();
        if (!pipe_.send(tab.key.connection_id, message.dump())) {
            show_search();
            MessageBoxW(window_, i18n::text(L"Could not send the switch request. Please try again.", L"暂时无法发送切换请求，请稍后重试。"), L"Glint", MB_OK | MB_ICONWARNING);
            drain_events();
            return;
        }
    }

    void receive(ConnectionId id, std::string message) {
        try {
            const Json parsed = Json::parse(message);
            std::lock_guard lock(event_mutex_);
            if (!index_.apply(id, parsed) && parsed.find("type") && parsed.at("type").is_string() && parsed.at("type").as_string() != "activate_result") return;
            const auto* type = parsed.find("type");
            if (type && type->is_string() && (type->as_string() == "hello" || type->as_string() == "snapshot"))
                connection_events_.push_back(index_.connections());
            events_.emplace_back(id, std::move(message));
        } catch (const ProtocolError&) { return; }
        if (window_) PostMessageW(window_, WM_PIPE_EVENT, 0, 0);
    }
    void send_failed(ConnectionId id, std::string request) {
        try {
            const Json original = Json::parse(request);
            const Json* request_id = original.find("requestId");
            if (!request_id || !request_id->is_string()) return;
            const Json failure = Json::Object{{"type", "activate_result"}, {"version", static_cast<int>(kProtocolVersion)},
                {"requestId", request_id->as_string()}, {"ok", false}, {"errorCode", "send_failed"}, {"error", "Could not send the switch request to the browser."}};
            receive(id, failure.dump());
        } catch (const ProtocolError&) {}
    }
    void disconnected(ConnectionId id) {
        {
            std::lock_guard lock(event_mutex_);
            index_.disconnect(id);
            connection_events_.push_back(index_.connections());
        }
        if (window_) PostMessageW(window_, WM_PIPE_EVENT, 0, 0);
    }

    void drain_events() {
        std::vector<std::pair<ConnectionId, std::string>> events;
        std::vector<std::vector<ConnectionInfo>> connection_events;
        {
            std::lock_guard lock(event_mutex_);
            events.swap(events_);
            connection_events.swap(connection_events_);
        }
        // Keep short-lived profiles in the overview even if hello, snapshot,
        // and disconnect all arrive before the UI processes its first event.
        for (const auto& connections : connection_events) connection_overview_.update(connections);
        for (auto& [id, text] : events) {
            try {
                const Json message = Json::parse(text);
                const auto* type = message.find("type");
                if (type && type->is_string() && type->as_string() == "hello") {
                    // Resolve on the UI thread so a delayed hello always gets
                    // the newest saved preference, including after reconnect.
                    if (!pipe_.send(id, language_settings_message(i18n::current_preference, i18n::current_language.load())))
                        SetTimer(window_, ID_LANGUAGE_RETRY, 1000, nullptr);
                }
                if (type && type->is_string() && type->as_string() == "activate_result") {
                    const auto* ok = message.find("ok");
                    if (ok && ok->is_bool() && !ok->as_bool()) {
                        const auto* error = message.find("error");
                        const auto* code = message.find("errorCode");
                        const auto* translated = code && code->is_string() ? i18n::activation_error(code->as_string()) : nullptr;
                        const std::wstring detail = translated ? translated : error && error->is_string() ? utf8_to_wide(error->as_string()) : i18n::text(L"The tab has been closed.", L"标签页已关闭");
                        MessageBoxW(window_, (i18n::text(L"Could not activate tab: ", L"激活失败：") + detail).c_str(), L"Glint", MB_OK | MB_ICONWARNING);
                    }
                }
            } catch (const ProtocolError&) {}
        }
        if (IsWindowVisible(window_)) refresh_results();
        else refresh_connections();
        if (setup_) setup_->update_connection(index_.connections(), active_hotkey_name());
    }

    void shutdown() {
        if (!window_) return;
        KillTimer(window_, UPDATE_TIMER); KillTimer(window_, STARTUP_TIMER);
        update_window_.reset();
        if (updater_) { updater_->stop(); updater_.reset(); }
        hotkey_window_.reset();
        setup_.reset();
        hotkey_.stop(); remove_tray(); pipe_.stop(); window_ = nullptr;
        if (font_) { DeleteObject(font_); font_ = nullptr; }
        if (heading_font_) { DeleteObject(heading_font_); heading_font_ = nullptr; }
        if (small_font_) { DeleteObject(small_font_); small_font_ = nullptr; }
        if (row_spacing_) { ImageList_Destroy(row_spacing_); row_spacing_ = nullptr; }
    }
};

} // namespace

int run_desktop(bool open_setup) {
    const std::wstring mutex_name = L"Local\\Glint.Desktop." + win::current_user_sid();
    HANDLE mutex = CreateMutexW(nullptr, TRUE, mutex_name.c_str());
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kClassName, nullptr)) {
            DWORD process = 0;
            GetWindowThreadProcessId(existing, &process);
            AllowSetForegroundWindow(process);
            PostMessageW(existing, open_setup ? WM_SHOW_SETUP : WM_SHOW_SEARCH, 0, 0);
        }
        CloseHandle(mutex); return 0;
    }
    // Keep the browser-loadable bundle in sync with this desktop executable
    // even when the user starts Glint normally instead of opening the setup
    // wizard. Files are replaced atomically by ensure_extracted; a browser
    // holding an older extension copy can finish its current session safely.
    std::vector<std::wstring> startup_warnings;
    const auto update_result = update::installation_startup_result();
    if (!update_result.empty()) startup_warnings.push_back(update_result);
    std::filesystem::path extension_directory;
    std::wstring extension_error;
    bool extension_changed = false;
    if (!extension_bundle::ensure_extracted(extension_directory, extension_error, &extension_changed) && !extension_error.empty()) {
        startup_warnings.push_back(i18n::text(L"Could not update browser extension files: ", L"无法更新浏览器扩展文件：") + extension_error +
            i18n::text(L"\r\nGlint will continue running. Choose “Connect browser” in the tray menu to retry, then reload the extension.", L"\r\nGlint 将继续运行。请从托盘打开“连接浏览器”重试更新，再重新加载扩展。"));
    }
    std::wstring image(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
    bool adjacent_changed = false;
    if (length && length < image.size()) {
        image.resize(length);
        if (!extension_bundle::sync_adjacent(std::filesystem::path(image).parent_path(), adjacent_changed, extension_error))
            startup_warnings.push_back(i18n::text(L"Could not update adjacent browser extension files: ", L"无法更新程序旁的浏览器扩展文件：") + extension_error);
    }
    if (extension_changed || adjacent_changed) startup_warnings.push_back(i18n::text(
        L"Extension files were updated. Reload the Glint extension in each connected browser profile to use the new extension code.",
        L"扩展文件已更新。请在每个已连接浏览器配置文件的扩展管理页重新加载 Glint 扩展，使新版扩展生效。"));
    // Preserve each already registered browser's explicit origins while moving
    // its native host to the versioned bridge copy. No unconfigured browser is
    // registered by ordinary desktop startup.
    std::wstring bridge_error;
    if (!setup::migrate_existing_hosts(bridge_error) && !bridge_error.empty()) {
        startup_warnings.push_back(bridge_error);
    }
    DesktopApp app(std::move(startup_warnings));
    const int result = app.run(open_setup);
    CloseHandle(mutex);
    return result;
}

} // namespace glint
