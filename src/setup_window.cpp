#include "setup_window.hpp"

#include "registration.hpp"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace glint {
namespace {
constexpr wchar_t kSetupClass[] = L"Glint.SetupWindow";
constexpr int kContentHeight = 634;
constexpr int kContentWidth = 740;
enum : int {
    OpenFolder = 2101, CopyPath, OpenChrome, OpenEdge, ChromeId, EdgeId,
    RegisterIds, ShowSearch, Later
};

std::filesystem::path extension_directory() {
    std::wstring exe(32768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    exe.resize(count);
    const auto home = std::filesystem::path(exe).parent_path();
    auto directory = home;
    for (int level = 0; level < 5; ++level) {
        const auto candidate = directory / L"extension";
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate / L"manifest.json", error)) return candidate;
        const auto parent = directory.parent_path();
        if (parent == directory) break;
        directory = parent;
    }
    return home / L"extension";
}

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
    : owner_(owner), show_search_(std::move(show_search)), extension_directory_(extension_directory()) {}

SetupWindow::~SetupWindow() {
    if (window_) DestroyWindow(window_);
    for (auto font : fonts_) if (font) DeleteObject(font);
}

int SetupWindow::px(int value) const { return MulDiv(value, dpi_, 96); }

void SetupWindow::show() {
    if (!window_ && !create()) {
        MessageBoxW(owner_, L"无法打开安装引导。请参照随附的 README.md 安装扩展。", L"Glint", MB_OK | MB_ICONERROR);
        return;
    }
    update_status();
    ShowWindow(window_, SW_SHOWNORMAL);
    SetForegroundWindow(window_);
    SetFocus(GetDlgItem(window_, OpenFolder));
}

bool SetupWindow::translate(MSG& message) {
    return window_ && IsWindowVisible(window_) && IsDialogMessageW(window_, &message);
}

HWND SetupWindow::add(const wchar_t* type, const wchar_t* text, DWORD style, int id,
                     int x, int y, int width, int height, int font, bool stretch) {
    const DWORD ex = wcscmp(type, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0;
    if (wcscmp(type, L"BUTTON") == 0) style |= BS_NOTIFY;
    HWND child = CreateWindowExW(ex, type, text, WS_CHILD | WS_VISIBLE | style,
        px(x), px(y), px(width), px(height), window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    controls_.push_back({child, x, y, width, height, font, stretch});
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
    constexpr DWORD style = WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_CLIPCHILDREN;
    AdjustWindowRectExForDpi(&frame, style, FALSE, WS_EX_CONTROLPARENT, dpi_);
    POINT cursor{};
    GetCursorPos(&cursor);
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

    add(L"STATIC", L"连接你的浏览器", 0, 0, 20, 16, 680, 32, 2, true);
    add(L"STATIC", L"完成以下 3 步，即可搜索 Chrome 和 Edge 的标签页。", 0, 0, 20, 55, 680, 24, 0, true);

    add(L"STATIC", L"1. 安装 Glint 扩展", 0, 0, 20, 94, 680, 24, 1, true);
    add(L"STATIC", L"在扩展管理页开启“开发者模式”，点击“加载已解压的扩展程序”（Chrome）\r\n或“加载解压缩的扩展”（Edge），选择下面的 extension 文件夹。", 0, 0, 20, 124, 680, 44, 0, true);
    add(L"EDIT", extension_directory_.c_str(), ES_READONLY | ES_AUTOHSCROLL | WS_TABSTOP, 0, 20, 174, 680, 28, 0, true);
    add(L"BUTTON", L"打开扩展文件夹", WS_TABSTOP, OpenFolder, 20, 210, 164, 30);
    add(L"BUTTON", L"复制路径", WS_TABSTOP, CopyPath, 194, 210, 116, 30);
    add(L"BUTTON", L"Chrome 安装页", WS_TABSTOP, OpenChrome, 320, 210, 182, 30);
    add(L"BUTTON", L"Edge 安装页", WS_TABSTOP, OpenEdge, 512, 210, 188, 30);

    add(L"STATIC", L"2. 粘贴扩展 ID，注册本机连接", 0, 0, 20, 262, 680, 24, 1, true);
    add(L"STATIC", L"从扩展“详情”或 Glint 扩展设置页复制 ID。只用一种浏览器时，另一项留空。\r\n注册仅作用于当前 Windows 用户，无需管理员权限。", 0, 0, 20, 292, 680, 44, 0, true);
    add(L"STATIC", L"Chrome 扩展 ID", 0, 0, 20, 342, 330, 20);
    add(L"STATIC", L"Edge 扩展 ID", 0, 0, 370, 342, 330, 20);
    chrome_id_ = add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, ChromeId, 20, 368, 330, 28);
    edge_id_ = add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, EdgeId, 370, 368, 330, 28);
    SendMessageW(chrome_id_, EM_SETLIMITTEXT, 128, 0);
    SendMessageW(edge_id_, EM_SETLIMITTEXT, 128, 0);
    SendMessageW(chrome_id_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"32 位 ID（字母 a–p）"));
    SendMessageW(edge_id_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"32 位 ID（字母 a–p）"));
    add(L"BUTTON", L"保存并注册", WS_TABSTOP | BS_DEFPUSHBUTTON, RegisterIds, 20, 408, 146, 32);
    result_ = add(L"STATIC", L"", 0, 0, 180, 406, 520, 48, 0, true);

    add(L"STATIC", L"3. 确认连接，开始搜索", 0, 0, 20, 472, 680, 24, 1, true);
    connection_ = add(L"STATIC", L"", 0, 0, 20, 504, 680, 24, 0, true);
    add(L"STATIC", L"每个需要搜索的浏览器配置文件都要安装扩展。请在相应配置文件内操作。\r\n关闭引导后 Glint 会驻留托盘；右键托盘 →“连接浏览器”可随时重新打开。", 0, 0, 20, 536, 680, 42, 0, true);
    search_ = add(L"BUTTON", L"开始搜索", WS_TABSTOP, ShowSearch, 20, 590, 150, 32);
    add(L"BUTTON", L"稍后设置", WS_TABSTOP, Later, 184, 590, 120, 32);

    const auto state = setup::read_registration();
    SetWindowTextW(chrome_id_, state.chrome_id.c_str());
    SetWindowTextW(edge_id_, state.edge_id.c_str());
    if ((!state.chrome_id.empty() && !state.chrome_current) || (!state.edge_id.empty() && !state.edge_current)) {
        SetWindowTextW(result_, L"注册路径或配置已失效。请再次点击“保存并注册”更新。 ");
    } else if (state.chrome_current || state.edge_current) {
        SetWindowTextW(result_, L"已有本机注册。若更换扩展 ID 或程序路径，请重新注册。 ");
    }
    set_fonts();
    layout();
    update_status();
    return true;
}

void SetupWindow::set_fonts() {
    HFONT old[3] = {fonts_[0], fonts_[1], fonts_[2]};
    for (int i = 0; i < 3; ++i) {
        fonts_[i] = CreateFontW(-MulDiv(i == 2 ? 16 : 10, dpi_, 72), 0, 0, 0, i == 0 ? FW_NORMAL : FW_SEMIBOLD,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Segoe UI");
    }
    for (const auto& c : controls_) SendMessageW(c.window, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_[c.font]), TRUE);
    for (auto font : old) if (font) DeleteObject(font);
}

void SetupWindow::layout() {
    if (!window_) return;
    RECT client{};
    GetClientRect(window_, &client);
    const int client_height = static_cast<int>(client.bottom);
    const int content_height = px(kContentHeight);
    scroll_ = (std::clamp)(scroll_, 0, (std::max)(0, content_height - client_height));
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS, 0, px(kContentHeight) - 1,
                    static_cast<UINT>(client_height), scroll_};
    SetScrollInfo(window_, SB_VERT, &info, TRUE);
    for (const auto& c : controls_) {
        const int width = c.stretch ? client.right - px(c.x + 20) : px(c.width);
        MoveWindow(c.window, px(c.x), px(c.y) - scroll_, width, px(c.height), TRUE);
    }
}

void SetupWindow::scroll_to(int position) { scroll_ = position; layout(); }

void SetupWindow::ensure_visible(HWND child) {
    RECT rect{}, client{};
    GetWindowRect(child, &rect);
    MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&rect), 2);
    GetClientRect(window_, &client);
    if (rect.top < 0) scroll_to(scroll_ + rect.top - px(8));
    else if (rect.bottom > client.bottom) scroll_to(scroll_ + rect.bottom - client.bottom + px(8));
}

void SetupWindow::update_connection(std::size_t profiles, std::size_t tabs, const std::wstring& hotkey) {
    profiles_ = profiles;
    tabs_ = tabs;
    hotkey_ = hotkey;
    update_status();
}

void SetupWindow::update_status() {
    if (!connection_) return;
    const std::wstring text = profiles_ == 0
        ? L"等待浏览器连接。注册后可在扩展管理页点击“重新加载”，或等待自动重连。"
        : L"已连接 " + std::to_wstring(profiles_) + L" 个配置文件 · " + std::to_wstring(tabs_) + L" 个标签页 · 热键 " + hotkey_;
    SetWindowTextW(connection_, text.c_str());
    EnableWindow(search_, profiles_ != 0);
}

void SetupWindow::dismiss() {
    if (!setup::dismiss_onboarding()) {
        MessageBoxW(window_, L"无法保存引导偏好。下次启动时可能再次显示安装引导。", L"Glint", MB_OK | MB_ICONWARNING);
    }
    ShowWindow(window_, SW_HIDE);
}

void SetupWindow::register_ids() {
    const auto chrome = edit_text(chrome_id_);
    const auto edge = edit_text(edge_id_);
    std::wstring error;
    if (setup::register_hosts(chrome, edge, error) != 0) {
        SetWindowTextW(result_, error.c_str());
        return;
    }
    SetWindowTextW(result_, L"注册已保存。请重新加载浏览器扩展，或等待自动重连。连接结果见下方。 ");
    update_status();
}

void SetupWindow::copy_text(const std::wstring& text) {
    if (!OpenClipboard(window_)) {
        SetWindowTextW(result_, L"剪贴板暂时不可用，请稍后重试。");
        return;
    }
    const auto bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool copied = false;
    if (memory) {
        if (void* data = GlobalLock(memory)) {
            memcpy(data, text.c_str(), bytes);
            GlobalUnlock(memory);
            if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory)) copied = true;
        }
        if (!copied) GlobalFree(memory);
    }
    CloseClipboard();
    SetWindowTextW(result_, copied ? L"扩展文件夹路径已复制。" : L"无法复制，请直接选择上方路径复制。");
}

void SetupWindow::open(const wchar_t* path, const wchar_t* parameters) {
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", path, parameters, nullptr, SW_SHOWNORMAL));
    if (result <= 32) {
        const std::wstring text = parameters
            ? std::wstring(L"无法自动打开浏览器。请在对应浏览器地址栏输入：\r\n") + parameters
            : L"无法打开文件夹。请确认已完整解压发行包，且 Glint.exe 旁保留 extension 文件夹。";
        MessageBoxW(window_, text.c_str(), L"Glint", MB_OK | MB_ICONWARNING);
    }
}

void SetupWindow::command(int id) {
    switch (id) {
    case OpenFolder: {
        std::error_code error;
        if (!std::filesystem::is_regular_file(extension_directory_ / L"manifest.json", error)) {
            MessageBoxW(window_, L"没有找到扩展。请完整解压 Glint 发行包，并将 extension 文件夹保留在 Glint.exe 旁。\r\n本地构建时请保留仓库内的 extension 文件夹。", L"Glint", MB_OK | MB_ICONWARNING);
        } else open(extension_directory_.c_str());
        break;
    }
    case CopyPath: copy_text(extension_directory_.wstring()); break;
    case OpenChrome: open(L"chrome.exe", L"chrome://extensions/"); break;
    case OpenEdge: open(L"msedge.exe", L"edge://extensions/"); break;
    case RegisterIds: register_ids(); break;
    case ShowSearch: if (profiles_ != 0) { dismiss(); show_search_(); } break;
    case IDCANCEL:
    case Later: dismiss(); break;
    }
}

LRESULT CALLBACK SetupWindow::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<SetupWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<SetupWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        self->window_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
    switch (message) {
    case DM_GETDEFID: return MAKELONG(RegisterIds, DC_HASDEFID);
    case WM_COMMAND:
        if (HIWORD(wparam) == EN_SETFOCUS || HIWORD(wparam) == BN_SETFOCUS) self->ensure_visible(reinterpret_cast<HWND>(lparam));
        if (HIWORD(wparam) == BN_CLICKED) self->command(LOWORD(wparam));
        return 0;
    case WM_SIZE: self->layout(); return 0;
    case WM_DPICHANGED: {
        self->dpi_ = HIWORD(wparam);
        self->scroll_ = 0;
        self->set_fonts();
        const auto* rect = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        self->layout();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize = {self->px(kContentWidth + 20), self->px(400)};
        return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO info{sizeof(info), SIF_ALL};
        GetScrollInfo(hwnd, SB_VERT, &info);
        switch (LOWORD(wparam)) {
        case SB_LINEUP: info.nPos -= self->px(24); break;
        case SB_LINEDOWN: info.nPos += self->px(24); break;
        case SB_PAGEUP: info.nPos -= info.nPage; break;
        case SB_PAGEDOWN: info.nPos += info.nPage; break;
        case SB_THUMBTRACK: info.nPos = info.nTrackPos; break;
        }
        self->scroll_to(info.nPos);
        return 0;
    }
    case WM_MOUSEWHEEL: self->scroll_to(self->scroll_ - GET_WHEEL_DELTA_WPARAM(wparam) * self->px(40) / WHEEL_DELTA); return 0;
    case WM_CLOSE: self->dismiss(); return 0;
    case WM_NCDESTROY: self->window_ = nullptr; break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

} // namespace glint
