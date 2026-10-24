#pragma once

#include <windows.h>
#include <commctrl.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace glint::icons {

// Shared lexical key for cache lookups and UI completion matching; never accesses disk.
std::wstring normalize_path(std::wstring_view path);

// These handles belong to the Shell. Callers must never destroy or modify them.
struct Icon {
    HIMAGELIST images = nullptr;
    int index = -1;
};

class Cache {
public:
    using Loader = std::function<Icon(const std::wstring&, UINT)>;
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    explicit Cache(Loader loader = {}, Clock clock = {});
    ~Cache();
    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;

    void start(HWND notify, UINT message);
    // Memory-only lookup. An empty result means the caller should draw a placeholder.
    Icon get(std::wstring_view path, UINT dpi);
    // Normalized paths, never row positions or process IDs. An empty path asks
    // the UI to retry visible rows after cache capacity becomes available.
    // Wake messages and capacity retry requests are coalesced.
    std::vector<std::wstring> take_updates();
    void invalidate();
    // Terminal, idempotent shutdown; a stalled Shell query cannot delay this indefinitely.
    void stop();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    std::thread worker_;
};

bool draw_icon(Icon icon, HDC dc, int x, int y, int size);

} // namespace glint::icons
