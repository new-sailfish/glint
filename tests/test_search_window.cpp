// Exercise the real search window without starting its bridge, desktop
// discovery, updater, global shortcut, or tray integration.
#include "../src/win_app.cpp"
#include "browser_launch.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <condition_variable>

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

// Give real native custom drawing a recognizable icon without depending on
// installed applications or the machine's Shell image-list contents.
class SearchIconLoader {
public:
    static constexpr COLORREF color = RGB(217, 33, 197);
    static constexpr COLORREF edge_color = RGB(25, 183, 211);

    SearchIconLoader() {
        images_ = ImageList_Create(16, 16, ILC_COLOR32, 2, 1);
        require_search(images_ != nullptr, "create shared search icon fixture");
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = 16; info.bmiHeader.biHeight = -16;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        require_search(bitmap && pixels, "create search icon fixture pixels");
        const DWORD colors[] = {0xffd921c5u, 0xff19b7d3u};
        for (int i = 0; i < 2; ++i) {
            std::fill_n(static_cast<DWORD*>(pixels), 16 * 16, colors[i]);
            require_search(ImageList_Add(images_, bitmap, nullptr) == i, "populate distinct shared search icon fixtures");
        }
        DeleteObject(bitmap);
    }
    ~SearchIconLoader() { release(); ImageList_Destroy(images_); }

    icons::Cache::Loader loader() {
        return [this](const std::wstring& path, UINT) {
            std::unique_lock lock(mutex_);
            requested_ = true;
            changed_.notify_all();
            // Bound even fixture failures, so a failed test cannot hang cache
            // shutdown while a worker waits for its deliberately slow lookup.
            changed_.wait_for(lock, std::chrono::seconds(5), [this] { return released_; });
            return icons::Icon{images_, path.ends_with(L"msedge.exe") ? 1 : 0};
        };
    }
    void wait_requested() {
        std::unique_lock lock(mutex_);
        require_search(changed_.wait_for(lock, std::chrono::seconds(3), [this] { return requested_; }),
            "native result drawing requests its application icon asynchronously");
    }
    void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }
    HIMAGELIST images() const { return images_; }

private:
    HIMAGELIST images_ = nullptr;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool requested_ = false;
    bool released_ = false;
};

struct ReleaseSearchIconLoader {
    SearchIconLoader& loader;
    ~ReleaseSearchIconLoader() { loader.release(); }
};

struct SearchWindowTestAccess {
    DesktopApp app;

    explicit SearchWindowTestAccess(bool populate = true, icons::Cache::Loader loader = {})
        : app({}, std::move(loader)) {
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

    static void check_activated_targets_return_as_recent_cards() {
        SearchWindowTestAccess test;
        auto& app = test.app;
        const auto verify_cards = [&app](const std::vector<std::int64_t>& expected, const char* message) {
            require_search(ListView_GetView(app.list_) == LV_VIEW_TILE &&
                ListView_GetItemCount(app.list_) == static_cast<int>(expected.size()) &&
                app.rows_.size() == expected.size(), message);
            for (std::size_t i = 0; i < expected.size(); ++i)
                require_search(app.rows_[i].key.tab_id == expected[i], message);
            wchar_t title[256]{};
            ListView_GetItemText(app.list_, 0, 0, title, static_cast<int>(std::size(title)));
            require_search(std::wstring_view(title) == app.rows_.front().title,
                "the first native card displays the reordered target title");
            require_search(ListView_GetNextItem(app.list_, -1, LVNI_SELECTED) == 0 && GetFocus() == app.edit_,
                "reopening after activation selects the newest card and retains search input focus");
        };
        const auto activate_and_reopen = [&app](const std::string& request, std::int64_t accessed_at) {
            const int selected = ListView_GetNextItem(app.list_, -1, LVNI_SELECTED);
            require_search(selected >= 0 && static_cast<std::size_t>(selected) < app.rows_.size(),
                "prepare a selected target for activation confirmation");
            const auto key = app.rows_[static_cast<std::size_t>(selected)].key;
            require_search(app.index_.track_activation(key, request, accessed_at),
                "track the selected browser target while awaiting activation confirmation");
            app.hide_search();
            app.receive(key.connection_id, Json(Json::Object{{"type", "activate_result"}, {"version", 1},
                {"requestId", request}, {"ok", true}}).dump());
            app.drain_events();
            app.show_search();
        };

        verify_cards({1, 2, 3, 4, 5, 6}, "prepare six recent cards in descending visit order");
        ListView_SetItemState(app.list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(app.list_, 4, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        activate_and_reopen("recent-card", 1730000002000);
        verify_cards({5, 1, 2, 3, 4, 6},
            "activating the third card in the first column moves it to the first recent position");

        app.receive(11, Json(Json::Object{{"type", "upsert"}, {"version", 1}, {"tab", Json::Object{
            {"tabId", 5}, {"windowId", 7}, {"index", 4}, {"title", "Browser tab guide"},
            {"url", "https://example.test/work/5"}, {"active", true}, {"pinned", false},
            {"lastAccessed", std::int64_t{1730000000600}}}}}).dump());
        app.drain_events();
        verify_cards({5, 1, 2, 3, 4, 6},
            "an older browser event cannot undo a confirmed card activation");

        SetWindowTextW(app.edit_, L"Issue tracker");
        require_search(ListView_GetView(app.list_) == LV_VIEW_DETAILS && ListView_GetItemCount(app.list_) == 1 &&
            app.rows_.front().key.tab_id == 7, "search reaches an open tab outside the six recent cards");
        activate_and_reopen("searched-tab", 1730000003000);
        verify_cards({7, 5, 1, 2, 3, 4},
            "activating a search result outside the recent six inserts it first and evicts the oldest card");
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

    static void check_application_icons(const std::filesystem::path& screenshots) {
        SearchIconLoader loader;
        {
            SearchWindowTestAccess test(true, loader.loader());
            ReleaseSearchIconLoader release_on_exit{loader};
            auto& app = test.app;
            require_search(app.index_.apply(12, Json::Object{{"type", "hello"}, {"version", 1},
                {"sessionId", "edge-test"}, {"profileId", "edge-test"}, {"browser", "Edge"},
                {"profileLabel", "Personal"}}), "create second browser icon fixture");
            const Json::Array edge_tabs{Json::Object{{"tabId", 20}, {"windowId", 9}, {"index", 0},
                {"title", "Edge project"}, {"url", "https://example.test/edge"},
                {"active", true}, {"pinned", false}, {"lastAccessed", std::int64_t{1730000000000}}}};
            require_search(app.index_.apply(12, Json::Object{{"type", "snapshot"}, {"version", 1}, {"tabs", edge_tabs}}),
                "seed an Edge target outside the recent cards");
            const std::wstring chrome_path = L"C:\\Users\\Éric\\Chrome\\chrome.exe";
            const std::wstring edge_path = L"C:\\GlintTests\\Edge\\msedge.exe";
            auto connections = app.index_.connections();
            for (auto& connection : connections)
                connection.executable_path = connection.connection_id == 11 ? chrome_path : edge_path;
            app.connection_overview_.update(connections);
            app.refresh_results(true);
            test.size(96);

            const auto keys = [&app] {
                std::vector<TabKey> result;
                for (const auto& row : app.rows_) result.push_back(row.key);
                return result;
            };
            const auto select_fifth = [&app] {
                ListView_SetItemState(app.list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_SetItemState(app.list_, 4, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                SetFocus(app.edit_);
            };
            const auto wait_ready = [&app, &loader](const std::wstring& path) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                do {
                    const auto icon = app.icon_cache_.get(path, app.dpi_);
                    if (icon.images == loader.images() && icon.index == (path.ends_with(L"msedge.exe") ? 1 : 0)) return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                } while (std::chrono::steady_clock::now() < deadline);
                require_search(false, "background search icon lookup completes within its deadline");
            };
            const auto assert_preserved = [&app, &keys](const std::vector<TabKey>& before,
                                                       std::wstring_view query, const char* message) {
                wchar_t actual[64]{}; GetWindowTextW(app.edit_, actual, static_cast<int>(std::size(actual)));
                require_search(keys() == before && std::wstring_view(actual) == query &&
                    GetFocus() == app.edit_ && ListView_GetNextItem(app.list_, -1, LVNI_SELECTED) == 4, message);
            };

            require_search((GetWindowLongPtrW(app.list_, GWL_STYLE) & LVS_SHAREIMAGELISTS) != 0 &&
                (GetWindowLongPtrW(app.connection_list_, GWL_STYLE) & LVS_SHAREIMAGELISTS) != 0,
                "native result and connection controls do not take ownership of shared image lists");
            SearchRender pending(app.list_);
            loader.wait_requested();
            require_search(!pending.contains(SearchIconLoader::color),
                "a pending application icon paints its fallback without waiting for Shell lookup");

            // A lookup started in card mode completes after the user has typed
            // and selected a different row in the full search results.
            SetWindowTextW(app.edit_, L"example.test");
            require_search(app.rows_.size() == 9 && ListView_GetView(app.list_) == LV_VIEW_DETAILS,
                "typing while icon lookup is pending remains responsive");
            select_fifth();
            const auto report_keys = keys();
            SearchRender pending_report(app.list_);
            loader.release();
            wait_ready(chrome_path); wait_ready(edge_path);
            ValidateRect(app.list_, nullptr);
            app.icons_changed();
            require_search(GetUpdateRect(app.list_, nullptr, FALSE) != FALSE,
                "completed application icons invalidate the current source rows for repaint");
            assert_preserved(report_keys, L"example.test",
                "icon completion preserves search text, result order, selected row, and edit focus");
            SearchRender report_icons(app.list_);
            require_search(report_icons.contains(SearchIconLoader::color) && report_icons.contains(SearchIconLoader::edge_color),
                "actual report custom drawing displays distinct application icons");
            for (std::size_t i = 0; i < app.rows_.size(); ++i) {
                wchar_t source[128]{};
                ListView_GetItemText(app.list_, static_cast<int>(i), 2, source, static_cast<int>(std::size(source)));
                require_search(std::wstring_view(source) == (app.rows_[i].key.connection_id == 11 ? L"Chrome" : L"Edge"),
                    "result source labels show browser names without profile suffixes");
                RECT cell{};
                require_search(ListView_GetSubItemRect(app.list_, static_cast<int>(i), 2, LVIR_BOUNDS, &cell) != FALSE,
                    "report source cells expose their native bounds");
                const COLORREF expected = app.rows_[i].key.connection_id == 11
                    ? SearchIconLoader::color : SearchIconLoader::edge_color;
                require_search(report_icons.pixel(cell.left + app.px(17), (cell.top + cell.bottom) / 2) == expected,
                    "each report source cell draws the icon belonging to that row's executable");
            }
            Tab native; native.key.source = TabSource::Window; native.application = L"Claude";
            require_search(app.source_name(native) == L"Claude", "desktop window source labels retain the application name");
            native.key.source = TabSource::UiTab; native.application = L"Notepad";
            require_search(app.source_name(native) == L"Notepad", "native tab source labels retain the application name");
            test.screenshot(screenshots, L"search-icons-results-96");

            SetWindowTextW(app.edit_, L"");
            for (const UINT dpi : {96u, 144u, 192u}) {
                test.size(dpi);
                select_fifth();
                const auto card_keys = keys();
                SearchRender request(app.list_);
                wait_ready(chrome_path);
                ValidateRect(app.list_, nullptr);
                app.icons_changed();
                if (dpi != 96) require_search(GetUpdateRect(app.list_, nullptr, FALSE) != FALSE,
                    "completion matches the uppercase Unicode executable path and repaints its recent cards");
                assert_preserved(card_keys, L"",
                    "application icon refresh preserves recent card order and keyboard selection at every DPI");
                require_search(SearchRender(app.list_).contains(SearchIconLoader::color),
                    "actual recent card drawing includes the application icon at every DPI");
                test.verify_controls_fit();
                test.screenshot(screenshots, L"search-icons-recent-" + std::to_wstring(dpi));
            }

            SendMessageW(app.connection_summary_, BM_CLICK, 0, 0);
            SearchRender connection_request(app.connection_list_);
            wait_ready(edge_path);
            app.icons_changed();
            SearchRender connection_icons(app.connection_list_);
            require_search(connection_icons.contains(SearchIconLoader::color) && connection_icons.contains(SearchIconLoader::edge_color),
                "expanded connection details display distinct application icons");
            bool found_work = false;
            for (int i = 0; i < ListView_GetItemCount(app.connection_list_); ++i) {
                wchar_t profile[128]{};
                ListView_GetItemText(app.connection_list_, i, 0, profile, static_cast<int>(std::size(profile)));
                found_work = found_work || std::wstring_view(profile).find(L"Work") != std::wstring_view::npos;
                RECT cell{};
                ListView_GetSubItemRect(app.connection_list_, i, 0, LVIR_BOUNDS, &cell);
                const COLORREF expected = app.connection_overview_.profiles()[static_cast<std::size_t>(i)].browser == L"chrome"
                    ? SearchIconLoader::color : SearchIconLoader::edge_color;
                require_search(connection_icons.pixel(cell.left + app.px(17), (cell.top + cell.bottom) / 2) == expected,
                    "each connection profile draws its own browser executable icon");
            }
            require_search(found_work, "expanded connection details retain profile names for disambiguation");
            test.verify_controls_fit();
            test.screenshot(screenshots, L"search-icons-connections-192");
        }
        require_search(ImageList_GetImageCount(loader.images()) == 2,
            "closing the search window preserves the shared icon image list");
    }

    static void preview_system_application_icons(const std::filesystem::path& screenshots) {
        if (screenshots.empty()) return;
        struct BrowserPreview {
            const char* name;
            std::filesystem::path executable;
            ConnectionId connection;
        };
        std::vector<BrowserPreview> browsers;
        for (const auto browser : {browser_launch::Browser::Chrome, browser_launch::Browser::Edge}) {
            auto executable = browser_launch::find_executable(browser);
            if (!executable.empty()) browsers.push_back({
                browser == browser_launch::Browser::Chrome ? "Chrome" : "Edge", std::move(executable),
                browser == browser_launch::Browser::Chrome ? 111u : 112u});
        }
        if (browsers.empty()) {
            std::cout << "System icon preview skipped: Chrome and Edge executable paths are unavailable\n";
            return;
        }

        // Read installed executable icons only. Every title, URL and profile
        // below is synthetic; this preview never starts or connects a browser.
        SearchWindowTestAccess test(false);
        auto& app = test.app;
        for (const auto& browser : browsers) {
            const std::string id = "system-icon-preview-" + std::to_string(browser.connection);
            require_search(app.index_.apply(browser.connection, Json::Object{{"type", "hello"}, {"version", 1},
                {"sessionId", id}, {"profileId", id}, {"browser", browser.name}, {"profileLabel", "Work"}}),
                "create synthetic browser connection for system icon preview");
            Json::Array tabs;
            const char* titles[] = {"Glint documentation", "Release checklist", "Project notes"};
            for (int i = 0; i < 3; ++i) {
                tabs.emplace_back(Json::Object{{"tabId", i + 1}, {"windowId", 7}, {"index", i},
                    {"title", titles[i]}, {"url", "https://example.test/preview/" + std::to_string(i + 1)},
                    {"active", i == 0}, {"pinned", false},
                    {"lastAccessed", std::int64_t{1730000001000} - i * 100 -
                        static_cast<std::int64_t>(browser.connection - 111) * 50}});
            }
            require_search(app.index_.apply(browser.connection,
                Json::Object{{"type", "snapshot"}, {"version", 1}, {"tabs", tabs}}),
                "seed synthetic tabs for system icon preview");
        }
        auto connections = app.index_.connections();
        for (auto& connection : connections) {
            const auto browser = std::find_if(browsers.begin(), browsers.end(), [&](const auto& candidate) {
                return candidate.connection == connection.connection_id;
            });
            if (browser != browsers.end()) connection.executable_path = browser->executable.wstring();
        }
        app.connection_overview_.update(connections);
        app.refresh_results(true);
        test.size(96);
        SearchRender request(app.list_);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        bool ready = false;
        do {
            ready = std::all_of(browsers.begin(), browsers.end(), [&app](const auto& browser) {
                const auto icon = app.icon_cache_.get(browser.executable.wstring(), app.dpi_);
                return icon.images && icon.index >= 0 && icon.index < ImageList_GetImageCount(icon.images);
            });
            if (ready) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        if (!ready) {
            std::cout << "System icon preview skipped: Shell icon lookup did not complete within its deadline\n";
            return;
        }
        app.icons_changed();
        test.verify_controls_fit();
        test.screenshot(screenshots, L"search-system-icons-96");
        SetWindowTextW(app.edit_, L"example.test");
        test.screenshot(screenshots, L"search-system-icons-results-96");
        SendMessageW(app.connection_summary_, BM_CLICK, 0, 0);
        test.verify_controls_fit();
        test.screenshot(screenshots, L"search-system-icons-connections-96");
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
        glint::SearchWindowTestAccess::check_activated_targets_return_as_recent_cards();
        glint::SearchWindowTestAccess::check_dpi_and_rendering(screenshots);
        glint::SearchWindowTestAccess::check_empty_connections();
        glint::SearchWindowTestAccess::check_application_icons(screenshots);
        glint::SearchWindowTestAccess::preview_system_application_icons(screenshots);
        std::cout << "Search window integration checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
