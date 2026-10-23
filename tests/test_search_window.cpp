// Exercise the real search window without starting its bridge, desktop
// discovery, updater, global shortcut, or tray integration.
#include "../src/win_app.cpp"

#include <fstream>
#include <iostream>
#include <stdexcept>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace glint {
namespace {

void require_search(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class SearchTestDesktop {
public:
    SearchTestDesktop() {
        previous_ = GetThreadDesktop(GetCurrentThreadId());
        const auto name = L"Glint.Search.Tests." + std::to_wstring(GetCurrentProcessId());
        desktop_ = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        require_search(desktop_ && SetThreadDesktop(desktop_), "create private search test desktop");
    }
    ~SearchTestDesktop() { SetThreadDesktop(previous_); CloseDesktop(desktop_); }
private:
    HDESK previous_ = nullptr;
    HDESK desktop_ = nullptr;
};

RECT search_bounds(HWND window, HWND parent) {
    RECT rectangle{};
    GetWindowRect(window, &rectangle);
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rectangle), 2);
    return rectangle;
}

class SearchRender {
public:
    explicit SearchRender(HWND window) {
        GetClientRect(window, &bounds_);
        info_.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info_.bmiHeader.biWidth = bounds_.right;
        info_.bmiHeader.biHeight = -bounds_.bottom;
        info_.bmiHeader.biPlanes = 1;
        info_.bmiHeader.biBitCount = 32;
        info_.bmiHeader.biCompression = BI_RGB;
        dc_ = CreateCompatibleDC(nullptr);
        bitmap_ = CreateDIBSection(dc_, &info_, DIB_RGB_COLORS, &pixels_, nullptr, 0);
        require_search(dc_ && bitmap_ && pixels_, "create offscreen search render target");
        previous_ = SelectObject(dc_, bitmap_);
        SendMessageW(window, WM_PRINT, reinterpret_cast<WPARAM>(dc_), PRF_CLIENT | PRF_ERASEBKGND | PRF_CHILDREN);
        GdiFlush();
    }
    ~SearchRender() { SelectObject(dc_, previous_); DeleteObject(bitmap_); DeleteDC(dc_); }
    COLORREF pixel(int x, int y) const { return GetPixel(dc_, x, y); }
    bool contains(COLORREF color) const {
        const auto dib_color = static_cast<DWORD>((GetRValue(color) << 16) | (GetGValue(color) << 8) | GetBValue(color));
        const auto* data = static_cast<const DWORD*>(pixels_);
        return std::any_of(data, data + bounds_.right * bounds_.bottom,
            [dib_color](DWORD value) { return (value & 0x00ffffff) == dib_color; });
    }
    void save(const std::filesystem::path& path) const {
        BITMAPFILEHEADER file{};
        file.bfType = 0x4d42;
        file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
        file.bfSize = file.bfOffBits + static_cast<DWORD>(bounds_.right * bounds_.bottom * 4);
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(&file), sizeof(file));
        output.write(reinterpret_cast<const char*>(&info_.bmiHeader), sizeof(BITMAPINFOHEADER));
        output.write(static_cast<const char*>(pixels_), static_cast<std::streamsize>(file.bfSize - file.bfOffBits));
        require_search(static_cast<bool>(output), "save search window diagnostic image");
    }
private:
    RECT bounds_{};
    BITMAPINFO info_{};
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    void* pixels_ = nullptr;
};

struct SearchWindowTestAccess {
    DesktopApp app;

    explicit SearchWindowTestAccess(bool populate = true) {
        require_search(app.create_window(false), "create actual search window without shell integration");
        require_search(!app.hotkey_.active() && !app.tray_added_, "test does not register a global shortcut or tray icon");
        if (populate) {
            require_search(app.index_.apply(11, Json::Object{{"type", "hello"}, {"version", 1},
                {"sessionId", "search-test"}, {"profileId", "search-test"},
                {"browser", "Chrome"}, {"profileLabel", "Work"}}), "create synthetic browser connection");
            Json::Array tabs;
            const char* titles[] = {"Glint documentation", "设计评审 — 搜索与键盘操作", "Release checklist",
                "Project notes", "Browser tab guide", "Keyboard shortcuts", "Issue tracker", "Build output"};
            for (int i = 0; i < 8; ++i) {
                tabs.emplace_back(Json::Object{{"tabId", i + 1}, {"windowId", 7}, {"index", i},
                    {"title", titles[i]}, {"url", "https://example.test/work/" + std::to_string(i + 1)},
                    {"active", i == 0}, {"pinned", false}, {"lastAccessed", std::int64_t{1730000001000} - i * 100}});
            }
            require_search(app.index_.apply(11, Json::Object{{"type", "snapshot"}, {"version", 1}, {"tabs", tabs}}),
                "seed eight open tabs with distinct visit times");
        }
        ShowWindow(app.window_, SW_SHOWNOACTIVATE);
        SetFocus(app.edit_);
        app.refresh_results(true);
    }
    ~SearchWindowTestAccess() { if (app.window_) DestroyWindow(app.window_); }

    void size(UINT dpi, int width = 700, int height = 520) {
        app.dpi_ = dpi;
        app.update_font();
        RECT client{}, outer{};
        GetClientRect(app.window_, &client); GetWindowRect(app.window_, &outer);
        const int frame_width = outer.right - outer.left - client.right;
        const int frame_height = outer.bottom - outer.top - client.bottom;
        SetWindowPos(app.window_, nullptr, 0, 0, app.px(width) + frame_width,
            app.px(height) + SearchCaption::height(dpi) + frame_height,
            SWP_NOACTIVATE | SWP_NOZORDER);
        app.layout();
    }

    void verify_controls_fit() const {
        RECT client{}; GetClientRect(app.window_, &client);
        for (HWND child = GetWindow(app.window_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            if (!IsWindowVisible(child)) continue;
            const auto bounds = search_bounds(child, app.window_);
            require_search(bounds.left >= 0 && bounds.top >= 0 && bounds.right <= client.right && bounds.bottom <= client.bottom,
                "visible search controls fit inside the window at every DPI and compact size");
            require_search(bounds.right > bounds.left && bounds.bottom > bounds.top,
                "visible search controls retain a usable area");
        }
        const auto results = search_bounds(app.list_, app.window_);
        const auto status = search_bounds(app.status_, app.window_);
        const auto connection = search_bounds(app.connection_summary_, app.window_);
        const auto setup = search_bounds(app.setup_button_, app.window_);
        require_search(results.bottom <= status.top && status.bottom <= connection.top,
            "results, result count, and connection status do not overlap");
        require_search(connection.right <= setup.left && connection.top == setup.top && connection.bottom == setup.bottom,
            "connection summary and connect action share one compact status row");
        require_search(connection.bottom - connection.top <= app.px(30), "connection status occupies at most thirty DIP");
        if (app.query_empty_ && !app.rows_.empty()) {
            RECT viewport{}; GetClientRect(app.list_, &viewport);
            for (int i = 0; i < ListView_GetItemCount(app.list_); ++i) {
                RECT item{}; ListView_GetItemRect(app.list_, i, &item, LVIR_BOUNDS);
                if (item.left < 0 || item.right > viewport.right) {
                    std::cerr << "Card width overflow: dpi=" << app.dpi_ << " expanded=" << app.connections_expanded_
                        << " columns=" << app.recent_columns_ << " client=" << viewport.right << 'x' << viewport.bottom
                        << " item=" << i << " bounds=[" << item.left << ',' << item.top << ',' << item.right << ',' << item.bottom << "]\n";
                }
                require_search(item.left >= 0 && item.right <= viewport.right,
                    "recent cards fit horizontally even when vertical scrolling is needed");
            }
            require_search((GetWindowLongPtrW(app.list_, GWL_STYLE) & WS_HSCROLL) == 0,
                "recent cards never require a horizontal scrollbar");
        }
    }

    void screenshot(const std::filesystem::path& directory, const std::wstring& name) const {
        if (!directory.empty()) SearchRender(app.window_).save(directory / (name + L".bmp"));
    }

    static void check_connections(const std::filesystem::path& screenshots) {
        SearchWindowTestAccess test;
        auto& app = test.app;
        test.size(96);
        require_search(!IsWindowVisible(app.connection_list_) && !IsWindowVisible(app.connection_hint_),
            "browser connection details start collapsed");
        require_search(IsWindowVisible(app.setup_button_), "connect browser remains directly available");
        wchar_t type[32]{}; GetClassNameW(app.connection_summary_, type, 32);
        require_search(wcscmp(type, L"Button") == 0, "status disclosure retains native button accessibility");
        require_search((GetWindowLongPtrW(app.connection_summary_, GWL_STYLE) & WS_TABSTOP) != 0,
            "keyboard traversal can reach the connection disclosure");
        const auto collapsed_results = search_bounds(app.list_, app.window_);
        test.verify_controls_fit();
        test.screenshot(screenshots, L"search-recent-96");
        SendMessageW(app.connection_summary_, BM_CLICK, 0, 0);
        require_search(IsWindowVisible(app.connection_list_) && ListView_GetItemCount(app.connection_list_) == 1,
            "clicking the status row expands connected profile details");
        require_search(search_bounds(app.list_, app.window_).bottom < collapsed_results.bottom,
            "expanded details take space only while visible");
        test.verify_controls_fit();
        test.screenshot(screenshots, L"search-connections-96");
        SendMessageW(app.connection_summary_, BM_CLICK, 0, 0);
        require_search(!IsWindowVisible(app.connection_list_) && search_bounds(app.list_, app.window_).bottom == collapsed_results.bottom,
            "collapsing details returns their space to results");
        for (const bool visible : {true, false}) {
            ShowWindow(app.window_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
            app.refresh_connections();
            require_search((GetWindowLongPtrW(app.connection_list_, GWL_STYLE) & WS_VISIBLE) == 0,
                "connection refresh leaves collapsed details hidden even when reenabling redraw");
            ShowWindow(app.window_, SW_SHOWNOACTIVATE);
            require_search(!IsWindowVisible(app.connection_list_) && !IsWindowVisible(app.connection_hint_),
                "a refresh while hidden cannot reveal collapsed details on the next display");
        }
    }

    static void check_query_modes(const std::filesystem::path& screenshots) {
        SearchWindowTestAccess test;
        auto& app = test.app;
        test.size(96);
        require_search(ListView_GetView(app.list_) == LV_VIEW_TILE && ListView_GetItemCount(app.list_) == 6,
            "empty search shows no more than six native recent cards");
        require_search(app.rows_.front().key.tab_id == 1 && app.rows_.back().key.tab_id == 6,
            "recent cards use visit order and omit older still-open tabs");
        require_search(ListView_GetNextItem(app.list_, -1, LVNI_SELECTED) == 0, "initial recent card is selected");
        require_search(GetFocus() == app.edit_, "opening recent cards keeps typing focus in search");
        SetFocus(app.list_);
        ListView_SetItemState(app.list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(app.list_, 4, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        SetFocus(app.edit_);
        SetWindowTextW(app.edit_, L"example.test");
        require_search(ListView_GetView(app.list_) == LV_VIEW_DETAILS && ListView_GetItemCount(app.list_) == 8,
            "typing switches to the existing full report list");
        require_search(ListView_GetNextItem(app.list_, -1, LVNI_SELECTED) == 0 && GetFocus() == app.edit_,
            "a changed query selects the first result and keeps typing focus in search");
        test.verify_controls_fit();
        test.screenshot(screenshots, L"search-results-96");
        SetWindowTextW(app.edit_, L"no-such-tab");
        require_search(ListView_GetItemCount(app.list_) == 0 && GetFocus() == app.edit_,
            "an unmatched query keeps search usable without stale cards");
        SetWindowTextW(app.edit_, L"");
        require_search(ListView_GetView(app.list_) == LV_VIEW_TILE && ListView_GetItemCount(app.list_) == 6 &&
            ListView_GetNextItem(app.list_, -1, LVNI_SELECTED) == 0, "clearing search restores recent cards and first selection");
    }

    static void check_dpi_and_rendering(const std::filesystem::path& screenshots) {
        SearchWindowTestAccess test;
        auto& app = test.app;
        for (const UINT dpi : {96u, 144u, 192u}) {
            test.size(dpi);
            test.verify_controls_fit();
            RECT first{}, second{}, third{}, sixth{};
            require_search(ListView_GetItemRect(app.list_, 0, &first, LVIR_BOUNDS) &&
                ListView_GetItemRect(app.list_, 1, &second, LVIR_BOUNDS) &&
                ListView_GetItemRect(app.list_, 2, &third, LVIR_BOUNDS) &&
                ListView_GetItemRect(app.list_, 5, &sixth, LVIR_BOUNDS), "native cards expose their item bounds");
            RECT client{}; GetClientRect(app.list_, &client);
            require_search(first.top == second.top && first.right <= second.left && third.top >= first.bottom,
                "six recent cards form two columns without overlap");
            require_search(sixth.right <= client.right && sixth.bottom <= client.bottom,
                "all six recent cards fit at the normal size for every DPI");
            SearchRender render(app.list_);
            if (!ui::high_contrast()) {
                require_search(render.contains(ui::accent()) && render.contains(ui::ink()),
                    "actual native tile paint includes selection and readable text");
                require_search(render.pixel(first.left + app.px(10), first.top + app.px(10)) == ui::wash(),
                    "selected card paints a distinct background");
                require_search(render.pixel(second.left + app.px(10), second.top + app.px(10)) == ui::surface(),
                    "unselected card paints the ordinary background");
            }
            test.screenshot(screenshots, L"search-recent-" + std::to_wstring(dpi));
            test.size(dpi, 500, 340);
            test.verify_controls_fit();
            RECT compact_first{}, compact_second{};
            ListView_GetItemRect(app.list_, 0, &compact_first, LVIR_BOUNDS);
            ListView_GetItemRect(app.list_, 1, &compact_second, LVIR_BOUNDS);
            if (compact_second.top < compact_first.bottom || compact_second.left != compact_first.left) {
                RECT compact_client{}; GetClientRect(app.list_, &compact_client);
                LVTILEVIEWINFO tiles{sizeof(tiles)}; tiles.dwMask = LVTVIM_TILESIZE | LVTVIM_COLUMNS;
                ListView_GetTileViewInfo(app.list_, &tiles);
                std::cerr << "Compact card layout: dpi=" << dpi << " columns=" << app.recent_columns_
                    << " client=" << compact_client.right << 'x' << compact_client.bottom
                    << " tile=" << tiles.sizeTile.cx << 'x' << tiles.sizeTile.cy
                    << " first=[" << compact_first.left << ',' << compact_first.top << ',' << compact_first.right << ',' << compact_first.bottom
                    << "] second=[" << compact_second.left << ',' << compact_second.top << ',' << compact_second.right << ',' << compact_second.bottom << "]\n";
            }
            require_search(compact_second.top >= compact_first.bottom && compact_second.left == compact_first.left,
                "compact windows place recent cards in one scrollable column");
            SendMessageW(app.connection_summary_, BM_CLICK, 0, 0);
            test.verify_controls_fit();
            test.screenshot(screenshots, L"search-compact-expanded-" + std::to_wstring(dpi));
            SendMessageW(app.connection_summary_, BM_CLICK, 0, 0);
        }
    }

    static void check_reopen_and_failure_recovery() {
        SearchWindowTestAccess test;
        auto& app = test.app;
        SetWindowTextW(app.edit_, L"Keyboard");
        require_search(ListView_GetItemCount(app.list_) == 1 && app.rows_.front().key.tab_id == 6,
            "prepare a filtered result before activation failure recovery");
        app.hide_search();
        app.show_search(false);
        wchar_t query[32]{};
        GetWindowTextW(app.edit_, query, 32);
        require_search(std::wstring_view(query) == L"Keyboard" && ListView_GetView(app.list_) == LV_VIEW_DETAILS &&
            ListView_GetItemCount(app.list_) == 1 && app.rows_.front().key.tab_id == 6,
            "activation failure recovery preserves the query and its report results");
        require_search(GetFocus() == app.edit_ && ListView_GetNextItem(app.list_, -1, LVNI_SELECTED) == 0,
            "failure recovery leaves search ready to retry the selected result");
        app.hide_search();
        app.show_search();
        require_search(GetWindowTextLengthW(app.edit_) == 0 && ListView_GetView(app.list_) == LV_VIEW_TILE &&
            ListView_GetItemCount(app.list_) == 6 && ListView_GetNextItem(app.list_, -1, LVNI_SELECTED) == 0,
            "a new search invocation clears the previous query and opens recent cards");
        require_search(GetFocus() == app.edit_, "a new search invocation is ready for immediate typing");
    }

    static void check_empty_connections() {
        SearchWindowTestAccess test(false);
        auto& app = test.app;
        require_search(ListView_GetItemCount(app.list_) == 0 && !IsWindowVisible(app.connection_hint_),
            "an empty connection set keeps the initial view compact");
        SendMessageW(app.connection_summary_, BM_CLICK, 0, 0);
        require_search(IsWindowVisible(app.connection_hint_) && IsWindowVisible(app.setup_button_),
            "expanded empty connection details explain how to connect a browser");
        test.verify_controls_fit();
    }
};

} // namespace
} // namespace glint

int wmain(int argc, wchar_t** argv) {
    try {
        std::filesystem::path screenshots;
        if (argc == 3 && std::wstring_view(argv[1]) == L"--screenshots") {
            screenshots = argv[2];
            std::filesystem::create_directories(screenshots);
        } else if (argc != 1) {
            throw std::runtime_error("usage: glint_search_window_tests [--screenshots directory]");
        }
        glint::SearchTestDesktop desktop;
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS};
        glint::require_search(InitCommonControlsEx(&controls) != FALSE, "initialize native search controls");
        glint::i18n::current_language = glint::i18n::Language::Chinese;
        glint::SearchWindowTestAccess::check_connections(screenshots);
        glint::SearchWindowTestAccess::check_query_modes(screenshots);
        glint::SearchWindowTestAccess::check_reopen_and_failure_recovery();
        glint::SearchWindowTestAccess::check_dpi_and_rendering(screenshots);
        glint::SearchWindowTestAccess::check_empty_connections();
        std::cout << "Search window integration checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
