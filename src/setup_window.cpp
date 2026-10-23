#include "setup_window.hpp"
#include "extension_bundle.hpp"
#include "registration.hpp"
#include "setup_progress.hpp"
#include "setup_illustration.hpp"
#include "ui_theme.hpp"
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
    CopyAddress, CopyPath, OpenFolder, Manual, ManualId, RegisterManual, AddBrowser, FolderPath, ReopenPage,
    RepairId, RegisterRepair, RepairLabel, RepairHelp, RepairResult,
    Illustration = 2201, Troubleshoot, BackToStatus, ConnectionStatus, ConnectionDetail, StepProgress
};
enum Page : int { Choose, Extensions, Developer, Folder, Confirm, Advanced, Repair };
std::wstring edit_text(HWND edit) {
    const int count = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<std::size_t>(count) + 1, L'\0');
    GetWindowTextW(edit, text.data(), count + 1);
    text.resize(count);
    const auto first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    return text.substr(first, text.find_last_not_of(L" \t\r\n") - first + 1);
}
std::wstring repair_extension_id(std::wstring_view value) {
    constexpr std::wstring_view prefix = L"chrome-extension://";
    if (value.starts_with(prefix)) {
        const auto slash = value.find(L'/', prefix.size());
        if (slash != prefix.size() + 32 || value.find_first_of(L" \t\r\n\\") != std::wstring_view::npos) return {};
        value = value.substr(prefix.size(), 32);
    }
    return setup::valid_extension_id(value) ? std::wstring(value) : std::wstring{};
}
} // namespace
SetupWindow::SetupWindow(HWND owner, std::function<void()> show_search)
    : owner_(owner), show_search_(std::move(show_search)) {}
SetupWindow::~SetupWindow() {
    if (window_) DestroyWindow(window_);
    for (auto font : fonts_) if (font) DeleteObject(font);
}
int SetupWindow::px(int value) const { return MulDiv(value, dpi_, 96); }
void SetupWindow::paint(HDC dc) const {
    ui::paint_background(window_, dc);
    RECT logo{px(24), px(24) - scroll_, px(62), px(62) - scroll_};
    ui::paint_logo(dc, logo);
    if (page_ == Choose) {
        for (const auto edge : {false, true}) {
            const int x = edge ? 346 : 24;
            const bool selected = (browser_ == browser_launch::Browser::Edge) == edge;
            HBRUSH brush = CreateSolidBrush(ui::surface());
            HPEN pen = CreatePen(PS_SOLID, px(1), selected ? ui::accent() : ui::line());
            const auto old_brush = SelectObject(dc, brush);
            const auto old_pen = SelectObject(dc, pen);
            RoundRect(dc, px(x), px(334) - scroll_, px(x + 310), px(384) - scroll_, px(12), px(12));
            SelectObject(dc, old_pen); SelectObject(dc, old_brush);
            DeleteObject(pen); DeleteObject(brush);
        }
    }
}
void SetupWindow::show() {
    const bool reopening = window_ && !IsWindowVisible(window_);
    if (!window_ && !create()) {
        MessageBoxW(owner_, L"无法打开连接向导。请参照 README.md 安装扩展。", L"Glint", MB_OK | MB_ICONERROR);
        return;
    }
    if (reopening) set_page(Choose);
    update_browser_status(); update_status();
    ShowWindow(window_, SW_SHOWNORMAL);
    SetForegroundWindow(window_);
    SetFocus(page_focus());
    ensure_visible(page_focus());
}
HWND SetupWindow::page_focus() const {
    if (page_ == Choose) return GetDlgItem(window_, browser_ == browser_launch::Browser::Edge ? ChooseEdge : ChooseChrome);
    if (page_ == Extensions) return GetDlgItem(window_, OpenPage);
    if (page_ == Folder) return GetDlgItem(window_, CopyPath);
    if (page_ == Advanced) return manual_id_;
    if (page_ == Repair) return repair_id_;
    return primary_;
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
    if (wcscmp(type, L"BUTTON") == 0) {
        const DWORD button_type = style & BS_TYPEMASK;
        if (button_type == BS_PUSHBUTTON || button_type == BS_DEFPUSHBUTTON)
            ui::style_button(child, id == Primary || id == RegisterRepair || id == RegisterManual);
    } else if (wcscmp(type, L"EDIT") == 0) ui::style_edit(child);
    return child;
}
bool SetupWindow::create() {
    WNDCLASSEXW klass{sizeof(klass)};
    klass.lpfnWndProc = window_proc;
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hIcon = ui::icon();
    klass.hIconSm = ui::icon();
    klass.hbrBackground = ui::background_brush();
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
    // Initial placement can resolve to a monitor with a different scale from
    // the system DPI. Size the hidden frame and controls at that same scale.
    dpi_ = GetDpiForWindow(window_);
    if (!dpi_) dpi_ = 96;
    frame = {0, 0, px(kContentWidth), px(kContentHeight)};
    AdjustWindowRectExForDpi(&frame, style, FALSE, WS_EX_CONTROLPARENT, dpi_);
    const int actual_width = (std::min)(frame.right - frame.left, work.right - work.left);
    const int actual_height = (std::min)(frame.bottom - frame.top, work.bottom - work.top);
    SetWindowPos(window_, nullptr,
        work.left + (work.right - work.left - actual_width) / 2,
        work.top + (work.bottom - work.top - actual_height) / 2,
        actual_width, actual_height, SWP_NOZORDER | SWP_NOACTIVATE);
    heading_ = add(L"STATIC", L"", 0, 0, 76, 22, 580, 36, -1, 2, true);
    add(L"STATIC", L"让打开的标签页出现在 Glint 搜索中。", 0, 0, 76, 66, 580, 24, -1, 0, true);
    progress_ = add(L"STATIC", L"", 0, StepProgress, 24, 102, 632, 24, -1, 1, true);
    add(L"STATIC", L"", SS_OWNERDRAW, 0, 24, 136, 632, 1, -1, 0, true);
    illustration_ = add(L"STATIC", L"", SS_OWNERDRAW, Illustration, 24, 154, 632, 170, -1, 0, true);
    add(L"BUTTON", L"Google Chrome", WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, ChooseChrome, 36, 340, 286, 38, Choose, 1);
    add(L"BUTTON", L"Microsoft Edge", WS_TABSTOP | BS_AUTORADIOBUTTON, ChooseEdge, 358, 340, 286, 38, Choose, 1);
    browser_status_ = add(L"STATIC", L"", 0, 0, 24, 390, 632, 42, Choose, 0, true);
    add(L"BUTTON", L"其他版本：手动填写扩展 ID", WS_TABSTOP, Manual, 24, 438, 270, 28, Choose);
    add(L"STATIC", L"先在浏览器中选好要连接的配置文件。\r\n再按 Ctrl+L → Ctrl+V → 回车，打开扩展管理页。", 0, 0, 24, 338, 632, 44, Extensions, 0, true);
    add(L"BUTTON", L"打开浏览器并复制页面地址", WS_TABSTOP, OpenPage, 24, 390, 294, 34, Extensions);
    address_ = add(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL | WS_TABSTOP, 0, 24, 436, 440, 28, Extensions, 0, true);
    add(L"BUTTON", L"复制页面地址", WS_TABSTOP, CopyAddress, -164, 436, 140, 28, Extensions);
    developer_hint_ = add(L"STATIC", L"", 0, 0, 24, 342, 632, 28, Developer, 1, true);
    add(L"STATIC", L"打开开关后，页面会出现加载扩展的按钮。\r\n完成后回到这里，继续下一步。", 0, 0, 24, 384, 632, 46, Developer, 0, true);
    load_hint_ = add(L"STATIC", L"", 0, 0, 24, 332, 632, 46, Folder, 0, true);
    add(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL | WS_TABSTOP, FolderPath, 24, 388, 632, 28, Folder, 0, true);
    add(L"BUTTON", L"复制扩展文件夹路径", WS_TABSTOP, CopyPath, 24, 430, 190, 32, Folder);
    add(L"BUTTON", L"查看文件夹", WS_TABSTOP, OpenFolder, 226, 430, 132, 32, Folder);
    connection_ = add(L"STATIC", L"", 0, ConnectionStatus, 24, 338, 632, 32, Confirm, 2, true);
    connection_detail_ = add(L"STATIC", L"", 0, ConnectionDetail, 24, 380, 632, 44, Confirm, 0, true);
    add(L"BUTTON", L"打开扩展页并复制地址", WS_TABSTOP, ReopenPage, 24, 434, 270, 30, Confirm);
    add(L"BUTTON", L"连接遇到问题", WS_TABSTOP, Troubleshoot, 310, 434, 194, 30, Confirm);
    add(L"BUTTON", L"连接新的浏览器", WS_TABSTOP, AddBrowser, 24, 434, 194, 30, Confirm);
    add(L"STATIC", L"粘贴扩展 ID 或页面地址修复连接", 0, RepairLabel, 24, 162, 632, 26, Repair, 1, true);
    add(L"STATIC", L"在浏览器的 Glint 页面按 Ctrl+L、Ctrl+C 复制完整地址；\r\n回到此处粘贴即可，也支持扩展管理卡片上的 32 位 ID。", 0, RepairHelp, 24, 204, 632, 44, Repair, 0, true);
    repair_id_ = add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, RepairId, 24, 266, 398, 30, Repair);
    SendMessageW(repair_id_, EM_SETLIMITTEXT, 4096, 0);
    SendMessageW(repair_id_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"粘贴 32 位 ID 或 chrome-extension:// 页面地址"));
    add(L"BUTTON", L"保存 ID 并重试", WS_TABSTOP, RegisterRepair, 438, 264, 218, 34, Repair);
    repair_result_ = add(L"STATIC", L"", 0, RepairResult, 24, 316, 632, 58, Repair, 0, true);
    add(L"BUTTON", L"返回连接状态", WS_TABSTOP, BackToStatus, 24, 430, 194, 32, Repair);
    add(L"STATIC", L"仅适用于商店版或自行修改过身份的扩展。\r\n使用 Glint 内置扩展时，返回选择浏览器即可自动配置。", 0, 0, 24, 338, 632, 44, Advanced, 0, true);
    manual_id_ = add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, ManualId, 24, 388, 632, 30, Advanced, 0, true);
    SendMessageW(manual_id_, EM_SETLIMITTEXT, 128, 0);
    SendMessageW(manual_id_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"从扩展详情页复制 32 位 ID"));
    add(L"BUTTON", L"保存此 ID 并检查连接", WS_TABSTOP, RegisterManual, 24, 430, 236, 34, Advanced);
    notice_ = add(L"STATIC", L"", 0, 0, 24, 474, 632, 42, -1, 0, true);
    add(L"STATIC", L"", SS_OWNERDRAW, 0, 24, 524, 632, 1, -1, 0, true);
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
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    for (const auto& c : controls_) SendMessageW(c.window, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_[c.font]), TRUE);
    for (auto font : old) if (font) DeleteObject(font);
}
void SetupWindow::layout() {
    if (!window_ || dismissing_) return;
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
    InvalidateRect(window_, nullptr, FALSE);
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
    connections_ = std::move(connections); hotkey_ = hotkey;
    if (window_ && page_ == Repair && setup::summarize_connections(connections_, browser_ == browser_launch::Browser::Edge).profiles)
        set_page(Confirm);
    else update_status();
}
void SetupWindow::set_page(int page) {
    page_ = page; scroll_ = 0;
    for (const auto& c : controls_) ShowWindow(c.window, c.page == -1 || c.page == page ? SW_SHOW : SW_HIDE);
    ShowWindow(illustration_, page == Repair ? SW_HIDE : SW_SHOW);
    ShowWindow(back_, page == Choose ? SW_HIDE : SW_SHOW);
    SetWindowTextW(notice_, L"");
    if (page == Repair && repair_browser_ != browser_) {
        SetWindowTextW(repair_id_, L"");
        SetWindowTextW(repair_result_, L"");
        repair_browser_ = browser_;
    }
    constexpr const wchar_t* headings[] = {L"选择要连接的浏览器", L"打开扩展管理页", L"开启开发者模式", L"加载 Glint 扩展", L"确认连接状态", L"手动连接其他版本", L"修复浏览器连接"};
    SetWindowTextW(heading_, headings[page]);
    const bool edge = browser_ == browser_launch::Browser::Edge;
    const std::wstring progress = page >= Advanced ? std::wstring(L"高级设置 · ") + (edge ? L"Microsoft Edge" : L"Google Chrome")
        : L"第 " + std::to_wstring(page + 1) + L" 步 / 共 5 步   ·   " + (edge ? L"Microsoft Edge" : L"Google Chrome");
    SetWindowTextW(progress_, progress.c_str());
    SetWindowTextW(address_, browser_launch::extension_page(browser_));
    SetWindowTextW(developer_hint_, edge ? L"在页面左侧，打开“开发人员模式”开关。" : L"在页面右上角，打开“开发者模式”开关。");
    SetWindowTextW(load_hint_, edge
        ? L"点击浏览器的“加载解压缩的扩展”。在弹窗底部“文件夹”输入框\r\n粘贴下方路径（Ctrl+V），直接点击“选择文件夹”。"
        : L"点击浏览器的“加载已解压的扩展程序”。在弹窗底部“文件夹”输入框\r\n粘贴下方路径（Ctrl+V），直接点击“选择文件夹”。");
    SetWindowTextW(GetDlgItem(window_, FolderPath), extension_directory_.c_str());
    update_browser_status(); update_status(); layout();
    SetFocus(page_focus());
    ensure_visible(page_focus());
}
void SetupWindow::update_status() {
    if (!window_ || !primary_) return;
    const bool edge = browser_ == browser_launch::Browser::Edge;
    const auto summary = setup::summarize_connections(connections_, edge);
    const auto name = edge ? L"Edge" : L"Chrome";
    const bool connected = summary.profiles != 0;
    const std::wstring status = std::wstring(name) + (connected ? L" 已连接" : L" 未连接");
    SetWindowTextW(connection_, status.c_str());
    const std::wstring search_instruction = hotkey_.empty()
        ? L"点击“开始搜索”。快捷键未启用，可从托盘打开快捷键设置。"
        : L"点击“开始搜索”，或按 " + hotkey_ + L" 搜索标题和网址。";
    const std::wstring detail = summary.profiles
        ? L"已同步 " + std::to_wstring(summary.profiles) + L" 个配置文件、" + std::to_wstring(summary.tabs) + L" 个标签页。\r\n" + search_instruction
        : L"尚未收到这个浏览器的标签页。确认 Glint 扩展已启用，\r\n在扩展面板点击“重试连接”，此处会自动更新。";
    SetWindowTextW(connection_detail_, detail.c_str());
    constexpr const wchar_t* actions[] = {L"下一步：打开扩展页", L"已打开扩展页，下一步", L"已开启，下一步", L"已加载，检查连接", L"检查连接", L"返回选择浏览器", L"返回连接状态"};
    SetWindowTextW(primary_, page_ == Confirm && connected ? L"开始搜索" : actions[page_]);
    EnableWindow(primary_, TRUE);
    SetWindowTextW(GetDlgItem(window_, Later), page_ == Confirm && connected ? L"关闭" : L"稍后设置");
    if (page_ == Confirm) {
        const auto focus = GetFocus();
        if ((connected && (focus == GetDlgItem(window_, Troubleshoot) || focus == GetDlgItem(window_, ReopenPage))) ||
            (!connected && focus == GetDlgItem(window_, AddBrowser))) SetFocus(primary_);
        ShowWindow(GetDlgItem(window_, Troubleshoot), connected ? SW_HIDE : SW_SHOW);
        ShowWindow(GetDlgItem(window_, ReopenPage), connected ? SW_HIDE : SW_SHOW);
        ShowWindow(GetDlgItem(window_, AddBrowser), connected ? SW_SHOW : SW_HIDE);
        if (connected) SetWindowTextW(notice_, L"");
    }
    InvalidateRect(illustration_, nullptr, FALSE);
}
void SetupWindow::update_browser_status() {
    if (!browser_status_) return;
    const auto name = browser_ == browser_launch::Browser::Edge ? L"Edge" : L"Chrome";
    const bool installed = !browser_launch::find_executable(browser_).empty();
    const std::wstring detected = installed ? std::wstring(L"已找到 ") + name + L"。每个要连接的配置文件都需要加载一次扩展。"
        : std::wstring(L"没有找到 ") + name + L" 的安装位置。你仍可继续，并手动打开浏览器。";
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
    // The themed non-client frame can send WM_SIZE synchronously while hiding.
    // Rebuilding scrollbars/layout in that transition can restore WS_VISIBLE.
    dismissing_ = true;
    ShowWindow(window_, SW_HIDE);
    dismissing_ = false;
}
void SetupWindow::register_manual() {
    const auto id = edit_text(manual_id_);
    if (id.empty()) { SetWindowTextW(notice_, L"请从扩展详情页复制 ID，再粘贴到上方。"); return; }
    std::wstring error;
    const bool edge = browser_ == browser_launch::Browser::Edge;
    if (setup::register_hosts(edge ? L"" : id, edge ? id : L"", error) != 0) { SetWindowTextW(notice_, error.c_str()); return; }
    manual_flow_ = true; set_page(Confirm);
}
void SetupWindow::register_repair() {
    const auto id = repair_extension_id(edit_text(repair_id_));
    if (id.empty()) {
        SetWindowTextW(repair_result_, L"请输入 32 位小写字母 a–p 的 ID，\r\n或粘贴 Glint 完整的 chrome-extension:// 页面地址。");
        SetFocus(repair_id_);
        return;
    }
    std::wstring error;
    const bool edge = browser_ == browser_launch::Browser::Edge;
    if (setup::register_hosts(edge ? L"" : id, edge ? id : L"", error) != 0) {
        SetWindowTextW(repair_result_, error.c_str());
        return;
    }
    SetWindowTextW(repair_id_, id.c_str());
    SetWindowTextW(manual_id_, id.c_str());
    manual_browser_ = browser_;
    SetWindowTextW(repair_result_, L"ID 已保存，正在等待扩展自动重试。\r\n也可在浏览器 Glint 页点击“重试连接”，或重新加载扩展。");
    update_status();
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
        set_page(Choose); break;
    case Primary:
        if (page_ == Choose) { if (prepare()) { manual_flow_ = false; set_page(Extensions); } }
        else if (page_ >= Extensions && page_ <= Folder) set_page(page_ + 1);
        else if (page_ == Confirm) {
            if (setup::summarize_connections(connections_, browser_ == browser_launch::Browser::Edge).profiles) { dismiss(); show_search_(); }
            else SetWindowTextW(notice_, L"尚未连接。请在浏览器的 Glint 面板点击“重试连接”，或打开“连接遇到问题”。");
        } else set_page(page_ == Repair ? Confirm : Choose);
        break;
    case Back: set_page(page_ == Confirm ? (manual_flow_ ? Advanced : Folder) : (page_ >= Advanced ? (page_ == Repair ? Confirm : Choose) : (std::max)(0, page_ - 1))); break;
    case Troubleshoot: set_page(Repair); break;
    case BackToStatus: set_page(Confirm); break;
    case AddBrowser:
        browser_ = browser_ == browser_launch::Browser::Edge ? browser_launch::Browser::Chrome : browser_launch::Browser::Edge;
        CheckRadioButton(window_, ChooseChrome, ChooseEdge, browser_ == browser_launch::Browser::Edge ? ChooseEdge : ChooseChrome);
        manual_flow_ = false; set_page(Choose); break;
    case Manual: {
        if (manual_browser_ != browser_) {
            const auto registration = setup::read_registration();
            const auto& extension_id = browser_ == browser_launch::Browser::Edge ? registration.edge_id : registration.chrome_id;
            SetWindowTextW(manual_id_, extension_id.c_str());
            manual_browser_ = browser_;
        }
        set_page(Advanced); break;
    }
    case RegisterManual: register_manual(); break;
    case RegisterRepair: register_repair(); break;
    case OpenPage: case ReopenPage: open_extensions(); break;
    case CopyAddress: copy_text(browser_launch::extension_page(browser_), L"页面地址已复制。请粘贴到对应浏览器的地址栏，按回车。"); break;
    case CopyPath: copy_text(extension_directory_.wstring(), L"路径已复制。请粘贴到弹窗底部“文件夹”输入框，再点击“选择文件夹”。"); break;
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
    case WM_ERASEBKGND:
        ui::paint_background(hwnd, reinterpret_cast<HDC>(wparam)); return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        self->paint(dc);
        EndPaint(hwnd, &paint); return 0;
    }
    case WM_PRINTCLIENT: self->paint(reinterpret_cast<HDC>(wparam)); return 0;
    case WM_CTLCOLORSTATIC: {
        const auto child = reinterpret_cast<HWND>(lparam);
        COLORREF color = ui::muted();
        if (child == self->heading_ || child == GetDlgItem(hwnd, RepairLabel)) color = ui::ink();
        else if (child == self->progress_ || child == self->developer_hint_) color = ui::accent();
        else if (child == self->connection_) {
            const bool connected = setup::summarize_connections(self->connections_, self->browser_ == browser_launch::Browser::Edge).profiles != 0;
            color = connected ? ui::success() : ui::ink();
        } else if (child == self->notice_ || child == self->repair_result_) color = ui::accent();
        const auto style = GetWindowLongPtrW(child, GWL_STYLE);
        wchar_t type[16]{}; GetClassNameW(child, type, 16);
        if (wcscmp(type, L"Edit") == 0 && (style & ES_READONLY))
            return ui::control_color(reinterpret_cast<HDC>(wparam), ui::ink(), true);
        return ui::control_color(reinterpret_cast<HDC>(wparam), color);
    }
    case WM_CTLCOLOREDIT:
        return ui::control_color(reinterpret_cast<HDC>(wparam), ui::ink(), true);
    case WM_CTLCOLORBTN: {
        const auto child = reinterpret_cast<HWND>(lparam);
        const bool browser_choice = child == GetDlgItem(hwnd, ChooseChrome) || child == GetDlgItem(hwnd, ChooseEdge);
        return ui::control_color(reinterpret_cast<HDC>(wparam), ui::ink(), browser_choice);
    }
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        if (item->CtlID == Illustration) {
            const bool edge = self->browser_ == browser_launch::Browser::Edge;
            setup::paint_illustration(item->hDC, item->rcItem, self->page_, edge,
                setup::summarize_connections(self->connections_, edge).profiles != 0, self->fonts_[0]);
            return TRUE;
        }
        if (item->CtlID == 0 && (GetWindowLongPtrW(item->hwndItem, GWL_STYLE) & SS_TYPEMASK) == SS_OWNERDRAW) {
            HBRUSH brush = CreateSolidBrush(ui::line());
            FillRect(item->hDC, &item->rcItem, brush); DeleteObject(brush); return TRUE;
        }
        break;
    }
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
        self->layout();
        if (IsChild(hwnd, GetFocus())) self->ensure_visible(GetFocus());
        return 0;
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
