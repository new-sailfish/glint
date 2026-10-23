#include "update_window.hpp"
#include "language.hpp"
#include "ui_theme.hpp"
#include <commctrl.h>
#include <shellapi.h>

namespace glint {
namespace {
constexpr int Check = 4101, Install = 4102, Automatic = 4103, Download = 4104;
std::wstring version_text(const std::string& value) { return {value.begin(), value.end()}; }
}
UpdateWindow::UpdateWindow(HWND owner, std::function<void()> check, std::function<void()> install,
    std::function<void(bool)> automatic)
    : owner_(owner), check_action_(std::move(check)), install_action_(std::move(install)), automatic_action_(std::move(automatic)) {}
UpdateWindow::~UpdateWindow() {
    if (window_) DestroyWindow(window_);
    if (font_) DeleteObject(font_);
    if (heading_font_) DeleteObject(heading_font_);
}
bool UpdateWindow::create() {
    WNDCLASSEXW klass{sizeof(klass)};
    klass.hInstance = GetModuleHandleW(nullptr); klass.lpfnWndProc = window_proc;
    klass.lpszClassName = L"Glint.UpdateWindow"; klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hbrBackground = ui::background_brush(); klass.hIcon = ui::icon(klass.hInstance);
    if (!RegisterClassExW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    dpi_ = owner_ ? GetDpiForWindow(owner_) : GetDpiForSystem();
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    RECT bounds{0, 0, MulDiv(560, dpi_, 96), MulDiv(420, dpi_, 96)};
    AdjustWindowRectExForDpi(&bounds, style, FALSE, WS_EX_CONTROLPARENT, dpi_);
    window_ = CreateWindowExW(WS_EX_CONTROLPARENT, klass.lpszClassName, L"Glint", style,
        CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
        owner_, nullptr, klass.hInstance, this);
    if (!window_) return false;
    auto add = [&](const wchar_t* type, DWORD extra, int id, DWORD ex = 0) {
        return CreateWindowExW(ex, type, L"", WS_CHILD | WS_VISIBLE | extra, 0, 0, 0, 0,
            window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), klass.hInstance, nullptr);
    };
    heading_ = add(L"STATIC", 0, 0);
    versions_ = add(L"STATIC", 0, 0);
    status_ = add(L"STATIC", 0, 0);
    progress_ = add(PROGRESS_CLASSW, PBS_SMOOTH, 0);
    detail_ = add(L"EDIT", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, 0, WS_EX_CLIENTEDGE);
    automatic_ = add(L"BUTTON", BS_AUTOCHECKBOX | WS_TABSTOP, Automatic);
    download_ = add(L"BUTTON", BS_PUSHBUTTON | WS_TABSTOP, Download);
    check_ = add(L"BUTTON", BS_PUSHBUTTON | WS_TABSTOP, Check);
    install_ = add(L"BUTTON", BS_DEFPUSHBUTTON | WS_TABSTOP, Install);
    if (!heading_ || !versions_ || !status_ || !progress_ || !detail_ || !automatic_ || !download_ || !check_ || !install_) {
        DestroyWindow(window_); return false;
    }
    ui::style_button(check_); ui::style_button(install_, true); ui::style_button(download_);
    fonts(); layout(); return true;
}
void UpdateWindow::fonts() {
    LOGFONTW spec{}; spec.lfHeight = -MulDiv(10, dpi_, 72); spec.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(spec.lfFaceName, L"Microsoft YaHei UI");
    HFONT font = CreateFontIndirectW(&spec);
    spec.lfHeight = -MulDiv(16, dpi_, 72); spec.lfWeight = FW_SEMIBOLD;
    HFONT heading = CreateFontIndirectW(&spec);
    if (font) {
        for (auto control : {versions_, status_, detail_, automatic_, download_, check_, install_})
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        if (font_) DeleteObject(font_); font_ = font;
    }
    if (heading) {
        SendMessageW(heading_, WM_SETFONT, reinterpret_cast<WPARAM>(heading), TRUE);
        if (heading_font_) DeleteObject(heading_font_); heading_font_ = heading;
    }
}
void UpdateWindow::layout() {
    if (!window_ || !install_) return;
    const auto px = [&](int n) { return MulDiv(n, dpi_, 96); };
    RECT r{}; GetClientRect(window_, &r);
    const int width = r.right - px(40);
    MoveWindow(heading_, px(20), px(16), width, px(36), TRUE);
    MoveWindow(versions_, px(20), px(62), width, px(28), TRUE);
    MoveWindow(status_, px(20), px(102), width, px(44), TRUE);
    MoveWindow(progress_, px(20), px(152), width, px(12), TRUE);
    MoveWindow(detail_, px(20), px(178), width, px(114), TRUE);
    MoveWindow(automatic_, px(20), px(308), width, px(26), TRUE);
    MoveWindow(download_, px(20), px(366), px(154), px(32), TRUE);
    MoveWindow(check_, r.right - px(354), px(366), px(154), px(32), TRUE);
    MoveWindow(install_, r.right - px(188), px(366), px(168), px(32), TRUE);
}
void UpdateWindow::refresh(const update::Snapshot& state) {
    state_ = state;
    if (!window_) return;
    SetWindowTextW(window_, i18n::text(L"Glint · Updates", L"Glint · 软件更新"));
    SetWindowTextW(heading_, i18n::text(L"Keep Glint up to date", L"保持 Glint 为最新版本"));
    auto versions = std::wstring(i18n::text(L"Current version: ", L"当前版本：")) + version_text(state.current_version);
    if (!state.target_version.empty()) versions += std::wstring(i18n::text(L"    New version: ", L"    新版本：")) + version_text(state.target_version);
    SetWindowTextW(versions_, versions.c_str());
    using update::State;
    const wchar_t* status = i18n::text(L"Updates are downloaded automatically. You choose when to restart.", L"更新会自动下载，由你决定何时重启安装。");
    if (state.state == State::Idle && state.checked) status = i18n::text(L"No newer version is available.", L"当前没有更高版本可用。");
    if (state.state == State::Checking) status = i18n::text(L"Checking for updates…", L"正在检查更新…");
    if (state.state == State::Downloading) status = i18n::text(L"Downloading and verifying the new version…", L"正在下载并校验新版本…");
    if (state.state == State::Ready) status = i18n::text(L"The update is ready. Restart Glint whenever you are ready.", L"更新已准备好，可以随时重启 Glint 完成升级。");
    if (state.state == State::Installing) status = i18n::text(L"Preparing to restart Glint…", L"正在准备重启 Glint…");
    if (state.state == State::Error) status = i18n::text(L"The update could not be completed. Your current version is still available.", L"更新未完成，当前版本仍可继续使用。");
    SetWindowTextW(status_, status);
    const auto percent = state.total ? (std::min)(std::uint64_t{100}, state.received * 100 / state.total) : 0;
    SendMessageW(progress_, PBM_SETPOS, state.state == State::Ready ? 100 : static_cast<WPARAM>(percent), 0);
    std::wstring details = state.error;
    if (details.empty()) details = i18n::text(
        L"Downloads come from Glint’s GitHub Releases and are verified before installation.\r\nOnly Glint restarts. If extension files change, reload the Glint extension in each browser profile when prompted.",
        L"更新来自 Glint 的 GitHub Releases，安装前会校验文件。\r\n升级仅重启 Glint。扩展文件有变化时，请按提示在各浏览器配置文件中重新加载 Glint 扩展。");
    SetWindowTextW(detail_, details.c_str());
    SetWindowTextW(automatic_, i18n::text(L"Automatically check and download updates", L"自动检查并下载更新"));
    SendMessageW(automatic_, BM_SETCHECK, state.automatic ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(check_, i18n::text(L"Check for updates", L"检查更新"));
    SetWindowTextW(install_, i18n::text(L"Restart and update", L"重启并升级"));
    SetWindowTextW(download_, i18n::text(L"Download page", L"官方下载页面"));
    const bool busy = state.state == State::Checking || state.state == State::Downloading || state.state == State::Installing;
    EnableWindow(check_, !busy && state.state != State::Ready);
    EnableWindow(install_, state.state == State::Ready);
    EnableWindow(automatic_, state.state != State::Installing);
}
void UpdateWindow::show(const update::Snapshot& state) {
    if (!window_ && !create()) return;
    refresh(state); ShowWindow(window_, SW_SHOWNORMAL); SetForegroundWindow(window_);
}
bool UpdateWindow::visible() const { return window_ && IsWindowVisible(window_); }
bool UpdateWindow::translate(MSG& message) {
    if (!visible() || (message.hwnd != window_ && !IsChild(window_, message.hwnd))) return false;
    if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) { ShowWindow(window_, SW_HIDE); return true; }
    return IsDialogMessageW(window_, &message) != FALSE;
}
LRESULT CALLBACK UpdateWindow::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<UpdateWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<UpdateWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); self->window_ = hwnd;
    }
    if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
    switch (message) {
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case Check: if (IsWindowEnabled(self->check_)) self->check_action_(); return 0;
        case Install: if (IsWindowEnabled(self->install_)) self->install_action_(); return 0;
        case Automatic: self->automatic_action_(SendMessageW(self->automatic_, BM_GETCHECK, 0, 0) == BST_CHECKED); return 0;
        case Download: ShellExecuteW(hwnd, L"open", L"https://github.com/new-sailfish/glint/releases/latest", nullptr, nullptr, SW_SHOWNORMAL); return 0;
        case IDCANCEL: ShowWindow(hwnd, SW_HIDE); return 0;
        }
        break;
    case WM_CLOSE: ShowWindow(hwnd, SW_HIDE); return 0;
    case WM_DPICHANGED:
        self->dpi_ = HIWORD(wparam); self->fonts();
        { const auto* bounds = reinterpret_cast<RECT*>(lparam);
          SetWindowPos(hwnd, nullptr, bounds->left, bounds->top, bounds->right - bounds->left, bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE); }
        self->layout(); return 0;
    case WM_SIZE: self->layout(); return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
        SetTextColor(reinterpret_cast<HDC>(wparam), ui::ink());
        return reinterpret_cast<LRESULT>(ui::background_brush());
    case WM_DESTROY: self->window_ = nullptr; return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
} // namespace glint
