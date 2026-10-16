#include "win_app.hpp"

#include "glint/core.hpp"
#include "registration.hpp"
#include "setup_window.hpp"
#include "win_common.hpp"

#include <windows.h>
#include <commctrl.h>
#include <sddl.h>
#include <shellapi.h>

#include <atomic>
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
constexpr UINT ID_HOTKEY = 1;
constexpr UINT ID_TRAY_SHOW = 1001;
constexpr UINT ID_TRAY_HOTKEY = 1002;
constexpr UINT ID_TRAY_EXIT = 1003;
constexpr UINT ID_TRAY_SETUP = 1004;
constexpr wchar_t kClassName[] = L"Glint.SearchWindow";

struct PipeClient {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ConnectionId id = 0;
    std::mutex write_mutex;
};

class PipeServer {
public:
    using MessageCallback = std::function<void(ConnectionId, std::string)>;
    using DisconnectCallback = std::function<void(ConnectionId)>;

    PipeServer(MessageCallback message, DisconnectCallback disconnect)
        : on_message_(std::move(message)), on_disconnect_(std::move(disconnect)) {}
    ~PipeServer() { stop(); }

    void start() { thread_ = std::thread([this] { accept_loop(); }); }

    bool send(ConnectionId id, std::string_view message) {
        std::shared_ptr<PipeClient> client;
        {
            std::lock_guard lock(mutex_);
            const auto found = clients_.find(id);
            if (found == clients_.end()) return false;
            client = found->second;
        }
        const auto frame = encode_frame(message);
        std::lock_guard write_lock(client->write_mutex);
        return win::write_all(client->handle, frame.data(), frame.size());
    }

    void stop() {
        if (stopping_.exchange(true)) return;
        // Wake ConnectNamedPipe so the accept thread can leave promptly.
        HANDLE wake = CreateFileW(win::pipe_name().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
        if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake);
        if (thread_.joinable()) thread_.join();
        std::unordered_map<ConnectionId, std::shared_ptr<PipeClient>> clients;
        {
            std::lock_guard lock(mutex_);
            clients.swap(clients_);
        }
        for (auto& [id, client] : clients) {
            (void)id;
            DisconnectNamedPipe(client->handle);
            CloseHandle(client->handle);
        }
        std::vector<std::thread> client_threads;
        {
            std::lock_guard lock(thread_mutex_);
            client_threads.swap(client_threads_);
        }
        for (auto& client_thread : client_threads) if (client_thread.joinable()) client_thread.join();
    }

private:
    MessageCallback on_message_;
    DisconnectCallback on_disconnect_;
    std::atomic_bool stopping_ = false;
    std::thread thread_;
    std::mutex mutex_;
    std::unordered_map<ConnectionId, std::shared_ptr<PipeClient>> clients_;
    std::mutex thread_mutex_;
    std::vector<std::thread> client_threads_;
    std::atomic<ConnectionId> next_id_ = 1;

    HANDLE make_server_pipe() {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        const std::wstring sddl = L"D:(A;;GA;;;" + win::current_user_sid() + L")";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return INVALID_HANDLE_VALUE;
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
        HANDLE pipe = CreateNamedPipeW(win::pipe_name().c_str(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES,
            1024 * 1024, 1024 * 1024, 0, &attributes);
        LocalFree(descriptor);
        return pipe;
    }

    void accept_loop() {
        while (!stopping_.load()) {
            HANDLE pipe = make_server_pipe();
            if (pipe == INVALID_HANDLE_VALUE) return;
            const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
            if (!connected) { CloseHandle(pipe); continue; }
            if (stopping_.load()) { DisconnectNamedPipe(pipe); CloseHandle(pipe); break; }
            auto client = std::make_shared<PipeClient>();
            client->handle = pipe;
            client->id = next_id_++;
            {
                std::lock_guard lock(mutex_);
                clients_.emplace(client->id, client);
            }
            std::lock_guard lock(thread_mutex_);
            client_threads_.emplace_back([this, client] { client_loop(client); });
        }
    }

    void client_loop(const std::shared_ptr<PipeClient>& client) {
        FrameReader reader;
        std::vector<std::uint8_t> buffer(16 * 1024);
        bool valid = true;
        while (!stopping_.load()) {
            DWORD count = 0;
            if (!win::read_some(client->handle, buffer.data(), buffer.size(), count) || count == 0) { valid = false; break; }
            try {
                const auto messages = reader.feed(std::span<const std::uint8_t>(buffer.data(), count));
                for (const auto& message : messages) on_message_(client->id, message);
            } catch (const ProtocolError&) { valid = false; break; }
        }
        (void)valid;
        bool close_handle = false;
        {
            std::lock_guard lock(mutex_);
            close_handle = clients_.erase(client->id) != 0;
        }
        if (close_handle) { DisconnectNamedPipe(client->handle); CloseHandle(client->handle); }
        on_disconnect_(client->id);
    }
};

struct Hotkey { UINT modifiers; UINT key; };
constexpr Hotkey kHotkeys[] = {
    {MOD_CONTROL | MOD_SHIFT, VK_SPACE},
    {MOD_CONTROL | MOD_ALT, VK_SPACE},
    {MOD_ALT, VK_SPACE},
};

std::wstring hotkey_name(Hotkey hotkey) {
    if (hotkey.modifiers == (MOD_CONTROL | MOD_SHIFT)) return L"Ctrl+Shift+Space";
    if (hotkey.modifiers == (MOD_CONTROL | MOD_ALT)) return L"Ctrl+Alt+Space";
    return L"Alt+Space";
}

DWORD load_hotkey_index() {
    HKEY key = nullptr; DWORD value = 0, size = sizeof(value), type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Glint", 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        RegQueryValueExW(key, L"Hotkey", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
        RegCloseKey(key);
    }
    return value < ARRAYSIZE(kHotkeys) ? value : 0;
}

void save_hotkey_index(DWORD index) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Glint", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(key, L"Hotkey", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&index), sizeof(index));
        RegCloseKey(key);
    }
}

class DesktopApp {
public:
    DesktopApp() : pipe_([this](ConnectionId id, std::string message) { receive(id, std::move(message)); },
                         [this](ConnectionId id) { disconnected(id); }) {}
    ~DesktopApp() { shutdown(); }

    int run(bool open_setup) {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
        InitCommonControlsEx(&controls);
        if (!create_window()) return 1;
        pipe_.start();
        setup_ = std::make_unique<SetupWindow>(window_, [this] { show_search(); });
        if (open_setup || setup::should_show_onboarding(setup::onboarding_dismissed(), setup::read_registration())) show_setup();
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (setup_->translate(message)) continue;
            if (message.message == WM_KEYDOWN && (GetFocus() == edit_ || GetFocus() == list_)) {
                if (message.wParam == VK_ESCAPE) { hide_search(); continue; }
                if (message.wParam == VK_RETURN) { activate_selected(); continue; }
                if (GetFocus() == edit_ && (message.wParam == VK_DOWN || message.wParam == VK_UP)) {
                    SetFocus(list_);
                    const int count = ListView_GetItemCount(list_);
                    if (count > 0) {
                        int selected = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
                        selected = message.wParam == VK_DOWN ? (selected < 0 ? 0 : min(selected + 1, count - 1))
                                                              : (selected < 0 ? count - 1 : max(selected - 1, 0));
                        ListView_SetItemState(list_, selected, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                        ListView_EnsureVisible(list_, selected, FALSE);
                    }
                    continue;
                }
            }
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
    HWND setup_button_ = nullptr;
    std::unique_ptr<SetupWindow> setup_;
    NOTIFYICONDATAW tray_{};
    UINT taskbar_created_ = 0;
    bool tray_added_ = false;
    DWORD hotkey_index_ = 0;
    TabIndex index_;
    PipeServer pipe_;
    std::mutex event_mutex_;
    std::vector<std::pair<ConnectionId, std::string>> events_;
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
        if (message == self->taskbar_created_) { self->add_tray(); return 0; }
        switch (message) {
        case WM_SHOW_SETUP: self->show_setup(); return 0;
        case WM_HOTKEY: self->show_search(); return 0;
        case WM_COMMAND:
            if (HIWORD(wparam) == EN_CHANGE && reinterpret_cast<HWND>(lparam) == self->edit_) { self->refresh_results(); return 0; }
            if (LOWORD(wparam) == ID_TRAY_SHOW) { self->show_search(); return 0; }
            if (LOWORD(wparam) == ID_TRAY_HOTKEY) { self->configure_hotkey(); return 0; }
            if (LOWORD(wparam) == ID_TRAY_SETUP) { self->show_setup(); return 0; }
            if (LOWORD(wparam) == ID_TRAY_EXIT) { DestroyWindow(hwnd); return 0; }
            break;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) { self->hide_search(); return 0; }
            if (wparam == VK_RETURN && GetFocus() == self->list_) { self->activate_selected(); return 0; }
            break;
        case WM_NOTIFY:
            if (reinterpret_cast<NMHDR*>(lparam)->hwndFrom == self->list_ && reinterpret_cast<NMHDR*>(lparam)->code == NM_DBLCLK) { self->activate_selected(); return 0; }
            break;
        case WM_SIZE:
            self->layout(); return 0;
        case WM_CLOSE: self->hide_search(); return 0;
        case WM_DESTROY:
            self->shutdown(); PostQuitMessage(0); return 0;
        case WM_PIPE_EVENT: self->drain_events(); return 0;
        case WM_TRAY_EVENT:
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
        klass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        klass.lpszClassName = kClassName;
        if (!RegisterClassExW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"Glint", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 900, 520, nullptr, nullptr, klass.hInstance, this);
        if (!window_) return false;
        edit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                12, 12, 860, 30, window_, nullptr, klass.hInstance, nullptr);
        list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                12, 52, 860, 420, window_, nullptr, klass.hInstance, nullptr);
        status_ = CreateWindowExW(0, L"STATIC", L"浏览器尚未连接", WS_CHILD | WS_VISIBLE,
                                   12, 478, 860, 24, window_, nullptr, klass.hInstance, nullptr);
        setup_button_ = CreateWindowExW(0, L"BUTTON", L"连接浏览器", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            752, 478, 120, 26, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_TRAY_SETUP)), klass.hInstance, nullptr);
        SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        SendMessageW(status_, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        SendMessageW(setup_button_, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        const wchar_t* headers[] = {L"标题", L"网址", L"浏览器 / 配置"};
        const int widths[] = {280, 420, 170};
        for (int i = 0; i != 3; ++i) { LVCOLUMNW column{LVCF_TEXT | LVCF_WIDTH}; column.pszText = const_cast<LPWSTR>(headers[i]); column.cx = widths[i]; ListView_InsertColumn(list_, i, &column); }
        hotkey_index_ = load_hotkey_index();
        if (!RegisterHotKey(window_, ID_HOTKEY, kHotkeys[hotkey_index_].modifiers, kHotkeys[hotkey_index_].key)) {
            MessageBoxW(window_, L"默认全局热键注册失败，可能已被其他程序占用。可从托盘配置热键。", L"Glint", MB_OK | MB_ICONWARNING);
        }
        tray_.cbSize = sizeof(tray_); tray_.hWnd = window_; tray_.uID = 1; tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; tray_.uCallbackMessage = WM_TRAY_EVENT;
        tray_.hIcon = LoadIconW(nullptr, IDI_APPLICATION); wcscpy_s(tray_.szTip, L"Glint 标签页搜索"); add_tray();
        layout();
        return true;
    }

    void add_tray() { if (!tray_added_ && window_ && Shell_NotifyIconW(NIM_ADD, &tray_)) tray_added_ = true; }
    void remove_tray() { if (tray_added_) { Shell_NotifyIconW(NIM_DELETE, &tray_); tray_added_ = false; } }

    void layout() {
        if (!window_ || !edit_) return;
        RECT rect{}; GetClientRect(window_, &rect);
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        MoveWindow(edit_, 12, 12, width - 24, 30, TRUE);
        MoveWindow(list_, 12, 52, width - 24, height - 92, TRUE);
        MoveWindow(status_, 12, height - 32, width - 160, 24, TRUE);
        MoveWindow(setup_button_, width - 140, height - 34, 128, 26, TRUE);
    }

    void show_search() {
        ShowWindow(window_, SW_SHOW); SetForegroundWindow(window_); SetFocus(edit_); SendMessageW(edit_, EM_SETSEL, 0, -1); refresh_results();
    }
    void hide_search() { ShowWindow(window_, SW_HIDE); }

    void show_setup() {
        if (!setup_) return;
        setup_->update_connection(index_.connection_count(), index_.tab_count(), hotkey_name(kHotkeys[hotkey_index_]));
        setup_->show();
    }

    void show_tray_menu() {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, ID_TRAY_SHOW, L"显示搜索");
        AppendMenuW(menu, MF_STRING, ID_TRAY_SETUP, L"连接浏览器（安装引导）");
        AppendMenuW(menu, MF_STRING, ID_TRAY_HOTKEY, L"配置全局热键");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"退出 Glint");
        POINT point{}; GetCursorPos(&point); SetForegroundWindow(window_); TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr); DestroyMenu(menu);
    }

    void configure_hotkey() {
        const int choice = MessageBoxW(window_, L"选择全局热键：\n\n“是” Ctrl+Shift+Space\n“否” Ctrl+Alt+Space\n“取消”保持当前设置", L"配置 Glint 热键", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (choice == IDCANCEL) return;
        const DWORD next = choice == IDYES ? 0 : 1;
        if (next == hotkey_index_) return;
        UnregisterHotKey(window_, ID_HOTKEY);
        if (!RegisterHotKey(window_, ID_HOTKEY, kHotkeys[next].modifiers, kHotkeys[next].key)) {
            MessageBoxW(window_, L"这个热键已被其他程序占用，原热键保持不变。", L"Glint", MB_OK | MB_ICONWARNING);
            RegisterHotKey(window_, ID_HOTKEY, kHotkeys[hotkey_index_].modifiers, kHotkeys[hotkey_index_].key);
            return;
        }
        hotkey_index_ = next; save_hotkey_index(next);
        if (setup_) setup_->update_connection(index_.connection_count(), index_.tab_count(), hotkey_name(kHotkeys[hotkey_index_]));
    }

    void refresh_results() {
        if (!edit_ || !list_) return;
        const int length = GetWindowTextLengthW(edit_);
        std::wstring query(static_cast<std::size_t>(length) + 1, L'\0');
        if (length) { GetWindowTextW(edit_, query.data(), length + 1); query.resize(static_cast<std::size_t>(length)); }
        rows_ = index_.search(query);
        SendMessageW(list_, WM_SETREDRAW, FALSE, 0); ListView_DeleteAllItems(list_);
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            const auto& row = rows_[i];
            LVITEMW item{LVIF_TEXT, static_cast<int>(i), 0, 0, 0, const_cast<LPWSTR>(row.title.c_str()), 0, 0, 0};
            ListView_InsertItem(list_, &item);
            ListView_SetItemText(list_, static_cast<int>(i), 1, const_cast<LPWSTR>(row.url.c_str()));
            std::wstring source = row.browser;
            if (!row.profile_label.empty()) source += L" / " + row.profile_label;
            ListView_SetItemText(list_, static_cast<int>(i), 2, const_cast<LPWSTR>(source.c_str()));
        }
        SendMessageW(list_, WM_SETREDRAW, TRUE, 0); InvalidateRect(list_, nullptr, FALSE);
        std::wstring text;
        if (index_.connection_count() == 0) text = L"浏览器尚未连接，点击“连接浏览器”安装扩展。";
        else if (rows_.empty() && !query.empty()) text = L"没有匹配结果";
        else text = std::to_wstring(rows_.size()) + L" 个标签页";
        SetWindowTextW(status_, text.c_str());
    }

    void activate_selected() {
        const int selected = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
        if (selected < 0 || static_cast<std::size_t>(selected) >= rows_.size()) return;
        const auto& tab = rows_[static_cast<std::size_t>(selected)];
        const std::string request = std::to_string(request_counter_++);
        const Json message = Json::Object{{"type", "activate"}, {"version", static_cast<int>(kProtocolVersion)},
            {"requestId", request}, {"tabId", tab.key.tab_id}, {"windowId", tab.window_id}};
        if (!pipe_.send(tab.key.connection_id, message.dump())) {
            MessageBoxW(window_, L"浏览器连接已断开，无法切换标签页。", L"Glint", MB_OK | MB_ICONWARNING);
            drain_events();
            return;
        }
        hide_search();
    }

    void receive(ConnectionId id, std::string message) {
        try {
            const Json parsed = Json::parse(message);
            if (!index_.apply(id, parsed) && parsed.find("type") && parsed.at("type").is_string() && parsed.at("type").as_string() != "activate_result") return;
        } catch (const ProtocolError&) { return; }
        {
            std::lock_guard lock(event_mutex_); events_.emplace_back(id, std::move(message));
        }
        if (window_) PostMessageW(window_, WM_PIPE_EVENT, 0, 0);
    }
    void disconnected(ConnectionId id) { index_.disconnect(id); if (window_) PostMessageW(window_, WM_PIPE_EVENT, 0, 0); }

    void drain_events() {
        std::vector<std::pair<ConnectionId, std::string>> events;
        { std::lock_guard lock(event_mutex_); events.swap(events_); }
        for (auto& [id, text] : events) {
            (void)id;
            try {
                const Json message = Json::parse(text);
                const auto* type = message.find("type");
                if (type && type->is_string() && type->as_string() == "activate_result") {
                    const auto* ok = message.find("ok");
                    if (ok && ok->is_bool() && !ok->as_bool()) {
                        const auto* error = message.find("error");
                        const std::wstring detail = error && error->is_string() ? utf8_to_wide(error->as_string()) : L"标签页已关闭";
                        MessageBoxW(window_, (L"激活失败：" + detail).c_str(), L"Glint", MB_OK | MB_ICONWARNING);
                    }
                }
            } catch (const ProtocolError&) {}
        }
        if (IsWindowVisible(window_)) refresh_results();
        if (setup_) setup_->update_connection(index_.connection_count(), index_.tab_count(), hotkey_name(kHotkeys[hotkey_index_]));
    }

    void shutdown() {
        if (!window_) return;
        setup_.reset();
        UnregisterHotKey(window_, ID_HOTKEY); remove_tray(); pipe_.stop(); window_ = nullptr;
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
            PostMessageW(existing, open_setup ? WM_SHOW_SETUP : WM_HOTKEY, 0, 0);
        }
        CloseHandle(mutex); return 0;
    }
    DesktopApp app;
    const int result = app.run(open_setup);
    CloseHandle(mutex);
    return result;
}

} // namespace glint
