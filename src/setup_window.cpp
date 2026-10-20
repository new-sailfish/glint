#include "setup_window.hpp"
#include "extension_bundle.hpp"
#include "registration.hpp"
#include "setup_progress.hpp"
#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cstring>
#include <utility>

namespace glint {
namespace {
constexpr wchar_t kSetupClass[] = L"Glint.SetupWindow";
constexpr int kContentHeight = 586, kContentWidth = 680;
enum : int {
    ChooseChrome = 2101, ChooseEdge, Primary, Back, Later, OpenPage,
    CopyAddress, CopyPath, OpenFolder, Manual, ManualId, RegisterManual, AddBrowser, FolderPath, ReopenPage
};
std::wstring edit_text(HWND edit) {
    const int count = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<std::size_t>(count) + 1, L'\0');
    GetWindowTextW(edit, text.data(), count + 1);
    text.resize(count);
    const auto first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    return text.substr(first, text.find_last_not_of(L" \t\r\n") - first + 1);
}
} // namespace
SetupWindow::SetupWindow(HWND owner, std::function<void()> show_search)
    : owner_(owner), show_search_(std::move(show_search)) {}
SetupWindow::~SetupWindow() {
    if (window_) DestroyWindow(window_);
    for (auto font : fonts_) if (font) DeleteObject(font);
}
int SetupWindow::px(int value) const { return MulDiv(value, dpi_, 96); }
void SetupWindow::show() {
    if (!window_ && !create()) {
        MessageBoxW(owner_, L"无法打开连接向导。请参照 README.md 安装扩展。", L"Glint", MB_OK | MB_ICONERROR);
        return;
    }
    update_browser_status(); update_status();
    ShowWindow(window_, SW_SHOWNORMAL);
    SetForegroundWindow(window_);
    HWND focus = page_ == 0 ? GetDlgItem(window_, browser_ == browser_launch::Browser::Edge ? ChooseEdge : ChooseChrome)
        : (page_ == 1 ? GetDlgItem(window_, OpenPage) : (page_ == 3 ? manual_id_ : primary_));
    SetFocus(IsWindowEnabled(focus) ? focus : back_);
}
bool SetupWindow::translate(MSG& message) {
    return window_ && IsWindowVisible(window_) && IsDialogMessageW(window_, &message);
}
HWND SetupWindow::add(const wchar_t* type, const wchar_t* text, DWORD style, int id,
    int x, int y, int width, int height, int page, int font, bool stretch) {
    const DWORD ex = wcscmp(type, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0;
    if (wcscmp(type, L"BUTTON") == 0) style |= BS_NOTIFY;
    HWND child = CreateWindowExW(ex, type, text, WS_CHILD | style, px(x), px(y), px(width), px(height),
        window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    controls_.push_back({child, x, y, width, height, font, page, stretch});
    return child;
}
bool SetupWindow::create() {
    WNDCLASSEXW klass{sizeof(klass)};
    klass.lpfnWndProc = window_proc;
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    klass.lpszClassName = kSetupClass;
    if (!RegisterClassExW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    dpi_ = GetDpiForSystem();
    RECT frame{0, 0, px(kContentWidth), px(kContentHeight)};
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VSCROLL | WS_CLIPCHILDREN;
    AdjustWindowRectExForDpi(&frame, style, FALSE, WS_EX_CONTROLPARENT, dpi_);
    POINT cursor{}; GetCursorPos(&cursor);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const auto& work = monitor.rcWork;
    const int width = (std::min)(frame.right - frame.left, work.right - work.left);
    const int height = (std::min)(frame.bottom - frame.top, work.bottom - work.top);
    window_ = CreateWindowExW(WS_EX_CONTROLPARENT, kSetupClass, L"Glint — 连接浏览器", style,
        work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2,
        width, height, nullptr, nullptr, klass.hInstance, this);
    if (!window_) return false;
    dpi_ = GetDpiForWindow(window_);
    heading_ = add(L"STATIC", L"", 0, 0, 24, 22, 632, 36, -1, 2, true);
    add(L"STATIC", L"让打开的标签页出现在 Glint 搜索中。", 0, 0, 24, 66, 632, 24, -1, 0, true);
    progress_ = add(L"STATIC", L"", 0, 0, 24, 102, 632, 24, -1, 1, true);
    add(L"STATIC", L"", SS_ETCHEDHORZ, 0, 24, 136, 632, 2, -1, 0, true);
    add(L"STATIC", L"你想先连接哪个浏览器？", 0, 0, 24, 160, 632, 26, 0, 1, true);
    add(L"BUTTON", L"Google Chrome", WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, ChooseChrome, 24, 200, 290, 44, 0, 1);
    add(L"BUTTON", L"Microsoft Edge", WS_TABSTOP | BS_AUTORADIOBUTTON, ChooseEdge, 338, 200, 290, 44, 0, 1);
    browser_status_ = add(L"STATIC", L"", 0, 0, 24, 260, 632, 44, 0, 0, true);
    add(L"STATIC", L"Glint 会自动准备扩展和本机连接，你无需填写扩展 ID。\r\n下一步只需要在浏览器里加载一次扩展。", 0, 0, 24, 324, 632, 48, 0, 0, true);
    add(L"STATIC", L"两个浏览器都要用？先完成一个，最后可以继续连接另一个。", 0, 0, 24, 390, 632, 24, 0, 0, true);
    add(L"BUTTON", L"其他版本：手动填写扩展 ID", WS_TABSTOP, Manual, 24, 430, 270, 28, 0);
    add(L"STATIC", L"1. 打开浏览器并进入扩展管理页", 0, 0, 24, 154, 632, 24, 1, 1, true);
    add(L"BUTTON", L"打开浏览器并复制页面地址", WS_TABSTOP, OpenPage, 24, 184, 294, 34, 1);
    add(L"STATIC", L"然后在浏览器中按 Ctrl+L → Ctrl+V → 回车，打开下面的地址。", 0, 0, 24, 222, 632, 24, 1, 0, true);
    address_ = add(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL | WS_TABSTOP, 0, 24, 250, 440, 28, 1, 0, true);
    add(L"BUTTON", L"复制页面地址", WS_TABSTOP, CopyAddress, -164, 250, 140, 28, 1);
    developer_hint_ = add(L"STATIC", L"", 0, 0, 24, 290, 632, 26, 1, 1, true);
    load_hint_ = add(L"STATIC", L"", 0, 0, 24, 328, 632, 66, 1, 0, true);
    add(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL | WS_TABSTOP, FolderPath, 24, 400, 632, 28, 1, 0, true);
    add(L"BUTTON", L"复制扩展文件夹路径", WS_TABSTOP, CopyPath, 24, 438, 190, 30, 1);
    add(L"BUTTON", L"查看文件夹", WS_TABSTOP, OpenFolder, 226, 438, 132, 30, 1);
    connection_ = add(L"STATIC", L"", 0, 0, 24, 166, 632, 38, 2, 2, true);
    connection_detail_ = add(L"STATIC", L"", 0, 0, 24, 222, 632, 80, 2, 0, true);
    add(L"STATIC", L"还没连上？\r\n• 确认浏览器扩展列表中已出现 Glint，且开关已打开。\r\n• 点击 Glint 卡片上的重新加载按钮，或在扩展页点“重试连接”。\r\n• 已安装旧版本时，请移除旧版后重新加载本次的扩展文件夹。", 0, 0, 24, 318, 632, 100, 2, 0, true);
    add(L"BUTTON", L"打开浏览器并复制页面地址", WS_TABSTOP, ReopenPage, 24, 430, 270, 30, 2);
    add(L"BUTTON", L"连接另一个浏览器", WS_TABSTOP, AddBrowser, 310, 430, 194, 30, 2);
    add(L"STATIC", L"仅适用于商店版或自行修改过身份的扩展。\r\n使用 Glint 内置扩展时，点击“上一步”走自动连接即可。", 0, 0, 24, 162, 632, 54, 3, 0, true);
    add(L"STATIC", L"当前所选浏览器的扩展 ID", 0, 0, 24, 244, 632, 24, 3, 1, true);
    manual_id_ = add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, ManualId, 24, 278, 632, 30, 3, 0, true);
    SendMessageW(manual_id_, EM_SETLIMITTEXT, 128, 0);
    SendMessageW(manual_id_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"从扩展详情页复制 32 位 ID"));
    add(L"BUTTON", L"保存此 ID 并检查连接", WS_TABSTOP, RegisterManual, 24, 328, 236, 34, 3);
    notice_ = add(L"STATIC", L"", 0, 0, 24, 474, 632, 42, -1, 0, true);
    add(L"STATIC", L"", SS_ETCHEDHORZ, 0, 24, 524, 632, 2, -1, 0, true);
    back_ = add(L"BUTTON", L"上一步", WS_TABSTOP, Back, 24, 540, 110, 32, -1);
    add(L"BUTTON", L"稍后设置", WS_TABSTOP, Later, -362, 540, 116, 32, -1);
    primary_ = add(L"BUTTON", L"", WS_TABSTOP | BS_DEFPUSHBUTTON, Primary, -234, 540, 210, 32, -1);
    if (browser_launch::find_executable(browser_).empty() && !browser_launch::find_executable(browser_launch::Browser::Edge).empty()) browser_ = browser_launch::Browser::Edge;
    CheckRadioButton(window_, ChooseChrome, ChooseEdge, browser_ == browser_launch::Browser::Edge ? ChooseEdge : ChooseChrome);
    set_fonts(); set_page(0); return true;
}
void SetupWindow::set_fonts() {
    HFONT old[3] = {fonts_[0], fonts_[1], fonts_[2]};
    for (int i = 0; i < 3; ++i) fonts_[i] = CreateFontW(-MulDiv(i == 2 ? 17 : 10, dpi_, 72), 0, 0, 0, i == 0 ? FW_NORMAL : FW_SEMIBOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    for (const auto& c : controls_) SendMessageW(c.window, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_[c.font]), TRUE);
    for (auto font : old) if (font) DeleteObject(font);
}
void SetupWindow::layout() {
    if (!window_) return;
    RECT client{}; GetClientRect(window_, &client);
    const int height = static_cast<int>(client.bottom);
    scroll_ = (std::clamp)(scroll_, 0, (std::max)(0, px(kContentHeight) - height));
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS, 0, px(kContentHeight) - 1, static_cast<UINT>(height), scroll_};
    SetScrollInfo(window_, SB_VERT, &info, TRUE);
    for (const auto& c : controls_) {
        const int x = c.x < 0 ? client.right + px(c.x) : px(c.x);
        int width = c.stretch ? client.right - x - px(24) : px(c.width);
        if (c.window == address_) width -= px(152);
        MoveWindow(c.window, x, px(c.y) - scroll_, (std::max)(1, width), px(c.height), TRUE);
    }
}
void SetupWindow::scroll_to(int position) { scroll_ = position; layout(); }
void SetupWindow::ensure_visible(HWND child) {
    if (!child || !IsWindowVisible(child)) return;
    RECT rect{}, client{}; GetWindowRect(child, &rect);
    MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&rect), 2); GetClientRect(window_, &client);
    if (rect.top < 0) scroll_to(scroll_ + rect.top - px(8));
    else if (rect.bottom > client.bottom) scroll_to(scroll_ + rect.bottom - client.bottom + px(8));
}
void SetupWindow::update_connection(std::vector<ConnectionInfo> connections, const std::wstring& hotkey) {
    connections_ = std::move(connections); hotkey_ = hotkey; update_status();
}
void SetupWindow::set_page(int page) {
    page_ = page; scroll_ = 0;
    for (const auto& c : controls_) ShowWindow(c.window, c.page == -1 || c.page == page ? SW_SHOW : SW_HIDE);
    ShowWindow(back_, page == 0 ? SW_HIDE : SW_SHOW);
    SetWindowTextW(notice_, L"");
    SetWindowTextW(heading_, page == 0 ? L"先连接一个浏览器" : (page == 1 ? L"在浏览器中加载 Glint 扩展" : (page == 2 ? L"确认连接，开始搜索" : L"手动连接其他版本")));
    const bool edge = browser_ == browser_launch::Browser::Edge;
    const std::wstring progress = page == 3 ? std::wstring(L"高级设置 · ") + (edge ? L"Microsoft Edge" : L"Google Chrome")
        : L"第 " + std::to_wstring(page + 1) + L" 步 / 共 3 步   ·   " + (page == 0 ? L"选择浏览器" : (edge ? L"Microsoft Edge" : L"Google Chrome"));
    SetWindowTextW(progress_, progress.c_str());
    SetWindowTextW(address_, browser_launch::extension_page(browser_));
    SetWindowTextW(developer_hint_, edge ? L"2. 开启页面左侧的“开发人员模式”" : L"2. 开启页面右上角的“开发者模式”");
    SetWindowTextW(load_hint_, edge
        ? L"3. 点击下方“复制扩展文件夹路径”，再在浏览器点击\r\n“加载解压缩的扩展”。在弹窗中按 Alt+D、Ctrl+V、回车，\r\n最后点击“选择文件夹”。"
        : L"3. 点击下方“复制扩展文件夹路径”，再在浏览器点击\r\n“加载已解压的扩展程序”。在弹窗中按 Alt+D、Ctrl+V、回车，\r\n最后点击“选择文件夹”。");
    SetWindowTextW(GetDlgItem(window_, FolderPath), extension_directory_.c_str());
    update_browser_status(); update_status(); layout();
    HWND focus = page == 0 ? GetDlgItem(window_, edge ? ChooseEdge : ChooseChrome)
        : (page == 1 ? GetDlgItem(window_, OpenPage) : (page == 3 ? manual_id_ : primary_));
    SetFocus(IsWindowEnabled(focus) ? focus : back_);
}
void SetupWindow::update_status() {
    if (!window_ || !primary_) return;
    const bool edge = browser_ == browser_launch::Browser::Edge;
    const auto summary = setup::summarize_connections(connections_, edge);
    const auto name = edge ? L"Edge" : L"Chrome";
    const std::wstring status = summary.profiles ? std::wstring(name) + L" 已连接" : std::wstring(L"正在等待 ") + name + L" 连接…";
    SetWindowTextW(connection_, status.c_str());
    const std::wstring detail = summary.profiles
        ? L"已收到 " + std::to_wstring(summary.profiles) + L" 个配置文件、" + std::to_wstring(summary.tabs) + L" 个标签页。\r\n点击“开始搜索”，或按 " + hotkey_ + L"，输入标题或网址即可搜索。"
        : L"Glint 尚未收到这个浏览器的标签页。\r\n加载扩展后会自动连接，此处会显示实际收到的标签页数量。";
    SetWindowTextW(connection_detail_, detail.c_str());
    SetWindowTextW(primary_, page_ == 0 ? L"下一步：安装扩展" : (page_ == 1 ? L"我已加载扩展，检查连接" : (page_ == 2 ? L"开始搜索" : L"返回自动连接")));
    const bool can_continue = page_ != 2 || summary.profiles != 0;
    if (!can_continue && GetFocus() == primary_) SetFocus(back_);
    EnableWindow(primary_, can_continue);
}
void SetupWindow::update_browser_status() {
    if (!browser_status_) return;
    const auto name = browser_ == browser_launch::Browser::Edge ? L"Edge" : L"Chrome";
    const bool installed = !browser_launch::find_executable(browser_).empty();
    const std::wstring detected = installed ? std::wstring(L"已找到 ") + name + L"。下一步会准备好扩展文件和连接。"
        : std::wstring(L"没有找到 ") + name + L" 的安装位置。仍可继续，并手动打开扩展管理页。";
    SetWindowTextW(browser_status_, detected.c_str());
}
bool SetupWindow::prepare() {
    std::wstring error;
    if (!extension_bundle::ensure_extracted(extension_directory_, error)) {
        SetWindowTextW(notice_, (L"无法准备扩展文件：" + error).c_str()); return false;
    }
    if (!setup::register_bundled_host(browser_ == browser_launch::Browser::Edge, error)) {
        SetWindowTextW(notice_, (L"无法保存本机连接：" + error).c_str()); return false;
    }
    return true;
}
void SetupWindow::dismiss() {
    if (!setup::dismiss_onboarding()) MessageBoxW(window_, L"无法保存引导偏好，下次启动时可能再次显示。", L"Glint", MB_OK | MB_ICONWARNING);
    ShowWindow(window_, SW_HIDE);
}
void SetupWindow::register_manual() {
    const auto id = edit_text(manual_id_);
    if (id.empty()) { SetWindowTextW(notice_, L"请从扩展详情页复制 ID，再粘贴到上方。"); return; }
    std::wstring error;
    const bool edge = browser_ == browser_launch::Browser::Edge;
    if (setup::register_hosts(edge ? L"" : id, edge ? id : L"", error) != 0) { SetWindowTextW(notice_, error.c_str()); return; }
    manual_flow_ = true; set_page(2);
}
bool SetupWindow::copy_text(const std::wstring& text, const wchar_t* success) {
    if (!OpenClipboard(window_)) { SetWindowTextW(notice_, L"剪贴板暂时不可用。可选中上方路径或地址，按 Ctrl+C 复制。"); return false; }
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    bool copied = false;
    if (memory) {
        if (void* data = GlobalLock(memory)) {
            memcpy(data, text.c_str(), (text.size() + 1) * sizeof(wchar_t)); GlobalUnlock(memory);
            if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory)) copied = true;
        }
        if (!copied) GlobalFree(memory);
    }
    CloseClipboard();
    SetWindowTextW(notice_, copied ? success : L"复制失败。请选中上方路径或地址，按 Ctrl+C 复制。"); return copied;
}
void SetupWindow::open_extensions() {
    const bool copied = copy_text(browser_launch::extension_page(browser_), L"页面地址已复制。请在浏览器中按 Ctrl+L、Ctrl+V、回车。");
    std::wstring error;
    if (!browser_launch::open_extensions(browser_, error)) {
        SetWindowTextW(notice_, L"未能打开浏览器。请复制页面地址，在所选浏览器的地址栏中粘贴并回车。");
        MessageBoxW(window_, error.c_str(), L"Glint — 请手动打开扩展管理页", MB_OK | MB_ICONWARNING); return;
    }
    SetWindowTextW(notice_, copied ? L"页面地址已复制。请在浏览器中按 Ctrl+L、Ctrl+V、回车；进入扩展页后再复制文件夹路径。"
        : L"已请求打开浏览器，但复制失败。请在地址栏手动输入 chrome://extensions/ 或 edge://extensions/。");
}
void SetupWindow::command(int id) {
    switch (id) {
    case ChooseChrome: case ChooseEdge:
        browser_ = id == ChooseEdge ? browser_launch::Browser::Edge : browser_launch::Browser::Chrome;
        update_browser_status(); update_status(); break;
    case Primary:
        if (page_ == 0) { if (prepare()) { manual_flow_ = false; set_page(1); } }
        else if (page_ == 1) set_page(2);
        else if (page_ == 2) {
            if (setup::summarize_connections(connections_, browser_ == browser_launch::Browser::Edge).profiles) { dismiss(); show_search_(); }
        } else set_page(0);
        break;
    case Back: set_page(page_ == 2 ? (manual_flow_ ? 3 : 1) : 0); break;
    case AddBrowser:
        browser_ = browser_ == browser_launch::Browser::Edge ? browser_launch::Browser::Chrome : browser_launch::Browser::Edge;
        CheckRadioButton(window_, ChooseChrome, ChooseEdge, browser_ == browser_launch::Browser::Edge ? ChooseEdge : ChooseChrome);
        manual_flow_ = false; set_page(0); break;
    case Manual: {
        if (manual_browser_ != browser_) {
            const auto registration = setup::read_registration();
            const auto& extension_id = browser_ == browser_launch::Browser::Edge ? registration.edge_id : registration.chrome_id;
            SetWindowTextW(manual_id_, extension_id.c_str());
            manual_browser_ = browser_;
        }
        set_page(3); break;
    }
    case RegisterManual: register_manual(); break;
    case OpenPage: case ReopenPage: open_extensions(); break;
    case CopyAddress: copy_text(browser_launch::extension_page(browser_), L"页面地址已复制。请粘贴到对应浏览器的地址栏，按回车。"); break;
    case CopyPath: copy_text(extension_directory_.wstring(), L"文件夹路径已复制。请在选择文件夹窗口中按 Alt+D，再按 Ctrl+V 粘贴。"); break;
    case OpenFolder: {
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", extension_directory_.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) SetWindowTextW(notice_, L"无法打开文件夹。请复制上方路径，在文件资源管理器地址栏中粘贴。");
        break;
    }
    case IDCANCEL: case Later: dismiss(); break;
    }
}
LRESULT CALLBACK SetupWindow::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<SetupWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<SetupWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        self->window_ = hwnd; SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
    switch (message) {
    case DM_GETDEFID: return MAKELONG(Primary, DC_HASDEFID);
    case WM_COMMAND:
        if (HIWORD(wparam) == EN_SETFOCUS || HIWORD(wparam) == BN_SETFOCUS) self->ensure_visible(reinterpret_cast<HWND>(lparam));
        if (HIWORD(wparam) == BN_CLICKED) self->command(LOWORD(wparam)); return 0;
    case WM_SIZE: self->layout(); return 0;
    case WM_DPICHANGED: {
        self->dpi_ = HIWORD(wparam); self->scroll_ = 0; self->set_fonts();
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
    case WM_VSCROLL: {
        SCROLLINFO info{sizeof(info), SIF_ALL}; GetScrollInfo(hwnd, SB_VERT, &info);
        switch (LOWORD(wparam)) {
        case SB_LINEUP: info.nPos -= self->px(24); break;
        case SB_LINEDOWN: info.nPos += self->px(24); break;
        case SB_PAGEUP: info.nPos -= info.nPage; break;
        case SB_PAGEDOWN: info.nPos += info.nPage; break;
        case SB_THUMBTRACK: info.nPos = info.nTrackPos; break;
        }
        self->scroll_to(info.nPos); return 0;
    }
    case WM_MOUSEWHEEL: self->scroll_to(self->scroll_ - GET_WHEEL_DELTA_WPARAM(wparam) * self->px(40) / WHEEL_DELTA); return 0;
    case WM_CLOSE: self->dismiss(); return 0;
    case WM_NCDESTROY: self->window_ = nullptr; break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
} // namespace glint
