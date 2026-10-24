#include "process_icons.hpp"

#include <shellapi.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <utility>

namespace glint::icons {

std::wstring normalize_path(std::wstring_view path) {
    std::wstring normalized(path);
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
    // Case folding and slash normalization are purely lexical: resolving the
    // path through the filesystem here would put storage I/O on the UI thread.
    if (!normalized.empty()) {
        std::wstring lower(normalized.size(), L'\0');
        const int length = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
            normalized.data(), static_cast<int>(normalized.size()), lower.data(),
            static_cast<int>(lower.size()), nullptr, nullptr, 0);
        if (length > 0) normalized = std::move(lower);
    }
    return normalized;
}

namespace {

constexpr std::size_t capacity = 256;
constexpr auto failure_backoff = std::chrono::seconds(30);
constexpr auto refresh_interval = std::chrono::seconds(60);

bool valid_icon(Icon icon) {
    if (!icon.images || icon.index < 0) return false;
    int width = 0;
    int height = 0;
    return ImageList_GetIconSize(icon.images, &width, &height) && width > 0 && height > 0 &&
        icon.index < ImageList_GetImageCount(icon.images);
}

Icon load_shell_icon(const std::wstring& path, UINT dpi) {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) ||
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return {};
    SHFILEINFOW info{};
    const UINT size_flag = MulDiv(16, static_cast<int>(dpi), 96) <= 16
        ? SHGFI_SMALLICON : SHGFI_LARGEICON;
    // SHGFI_ICON is deliberately absent: no HICON is allocated or owned here.
    const auto result = SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info),
        SHGFI_SYSICONINDEX | size_flag);
    Icon icon{reinterpret_cast<HIMAGELIST>(result), info.iIcon};
    return valid_icon(icon) ? icon : Icon{};
}

} // namespace

struct Cache::Impl {
    struct Key {
        std::wstring path;
        UINT dpi = 96;
        bool operator<(const Key& other) const {
            if (path != other.path) return path < other.path;
            return dpi < other.dpi;
        }
    };
    struct Entry {
        Icon icon;
        std::chrono::steady_clock::time_point refresh_after{};
        std::uint64_t touched = 0;
        bool pending = false;
    };
    struct Job {
        Key key;
        std::wstring query_path;
        std::uint64_t generation = 0;
    };

    Loader loader;
    Clock clock;
    std::mutex mutex;
    std::condition_variable changed;
    std::condition_variable finished;
    std::map<Key, Entry> entries;
    std::deque<Job> queue;
    std::set<std::wstring> updated;
    std::uint64_t generation = 0;
    std::uint64_t touched = 0;
    HWND notify = nullptr;
    UINT message = 0;
    bool started = false;
    bool stopped = false;
    bool exited = false;
    bool notified = false;
    bool capacity_waiter = false;
    bool retry_visible = false;

    void run() {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [this] { return stopped || !queue.empty(); });
                if (stopped) break;
                job = std::move(queue.front());
                queue.pop_front();
            }
            Icon loaded;
            try {
                if (SUCCEEDED(initialized)) loaded = loader(job.query_path, job.key.dpi);
                if (!valid_icon(loaded)) loaded = {};
            } catch (...) {
                // Shell providers or an injected loader must not terminate the
                // worker; failed paths receive the same bounded retry interval.
                loaded = {};
            }
            {
                std::lock_guard lock(mutex);
                if (stopped) break;
                if (job.generation != generation) continue;
                const auto found = entries.find(job.key);
                if (found == entries.end()) continue;
                Entry& entry = found->second;
                entry.icon = loaded;
                entry.pending = false;
                entry.refresh_after = clock() + (loaded.images ? refresh_interval : failure_backoff);
                if (capacity_waiter) {
                    retry_visible = true;
                    capacity_waiter = false;
                }
                // Keep notification metadata bounded too, even while the window
                // is hidden and no caller consumes updates.
                if (updated.size() == capacity) updated.erase(updated.begin());
                updated.insert(job.key.path);
                // Posting under the lock serializes against stop(): no worker
                // can post after shutdown clears the destination HWND.
                if (!notified && notify && message)
                    notified = PostMessageW(notify, message, 0, 0) != FALSE;
            }
        }
        if (SUCCEEDED(initialized)) CoUninitialize();
        {
            std::lock_guard lock(mutex);
            exited = true;
        }
        finished.notify_all();
    }
};

Cache::Cache(Loader loader, Clock clock) : impl_(std::make_shared<Impl>()) {
    impl_->loader = loader ? std::move(loader) : load_shell_icon;
    impl_->clock = clock ? std::move(clock) : [] { return std::chrono::steady_clock::now(); };
}

Cache::~Cache() { stop(); }

void Cache::start(HWND notify, UINT message) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->started || impl_->stopped) return;
    impl_->notify = notify;
    impl_->message = message;
    impl_->started = true;
    // Capturing only the shared state makes bounded shutdown safe even if a
    // third-party Shell handler never returns from its current query.
    worker_ = std::thread([state = impl_] { state->run(); });
}

Icon Cache::get(std::wstring_view path, UINT dpi) {
    if (path.empty() || path.size() > 32767) return {};
    Impl::Key key{normalize_path(path), dpi ? dpi : 96};
    std::lock_guard lock(impl_->mutex);
    if (!impl_->started || impl_->stopped) return {};
    auto found = impl_->entries.find(key);
    if (found == impl_->entries.end()) {
        if (impl_->entries.size() == capacity) {
            auto oldest = impl_->entries.end();
            for (auto candidate = impl_->entries.begin(); candidate != impl_->entries.end(); ++candidate) {
                if (!candidate->second.pending && (oldest == impl_->entries.end() ||
                    candidate->second.touched < oldest->second.touched)) oldest = candidate;
            }
            // All slots are queued or in flight; retain placeholders and ask
            // the UI to retry visible rows when a completion frees capacity.
            // A single bit avoids retaining an unbounded set of rejected paths.
            if (oldest == impl_->entries.end()) {
                impl_->capacity_waiter = true;
                return {};
            }
            impl_->entries.erase(oldest);
        }
        found = impl_->entries.emplace(key, Impl::Entry{}).first;
    }
    Impl::Entry& entry = found->second;
    entry.touched = ++impl_->touched;
    // These image-list metadata queries do not invoke the Shell or filesystem.
    // Reacquire an invalidated list/index immediately, without bypassing the
    // backoff on entries that already represent a failed lookup.
    if (entry.icon.images && !valid_icon(entry.icon)) {
        entry.icon = {};
        entry.refresh_after = {};
    }
    if (!entry.pending && impl_->clock() >= entry.refresh_after) {
        entry.pending = true;
        // Normalize only the identity key: preserving the lookup spelling also
        // permits executable paths inside case-sensitive Windows directories.
        impl_->queue.push_back({std::move(key), std::wstring(path), impl_->generation});
        impl_->changed.notify_one();
    }
    return entry.icon;
}

std::vector<std::wstring> Cache::take_updates() {
    std::lock_guard lock(impl_->mutex);
    std::vector<std::wstring> result(impl_->updated.begin(), impl_->updated.end());
    if (impl_->retry_visible) result.emplace_back();
    impl_->updated.clear();
    impl_->notified = false;
    impl_->retry_visible = false;
    return result;
}

void Cache::invalidate() {
    std::lock_guard lock(impl_->mutex);
    ++impl_->generation;
    impl_->entries.clear();
    impl_->queue.clear();
    impl_->updated.clear();
    impl_->notified = false;
    impl_->capacity_waiter = false;
    impl_->retry_visible = false;
}

void Cache::stop() {
    {
        std::unique_lock lock(impl_->mutex);
        impl_->stopped = true;
        impl_->notify = nullptr;
        impl_->message = 0;
        impl_->queue.clear();
        impl_->entries.clear();
        impl_->updated.clear();
        impl_->capacity_waiter = false;
        impl_->retry_visible = false;
        impl_->changed.notify_all();
        if (worker_.joinable()) {
            impl_->finished.wait_for(lock, std::chrono::milliseconds(250),
                [this] { return impl_->exited; });
        }
    }
    if (worker_.joinable()) {
        bool exited = false;
        {
            std::lock_guard lock(impl_->mutex);
            exited = impl_->exited;
        }
        if (exited) worker_.join();
        else worker_.detach();
    }
}

bool draw_icon(Icon icon, HDC dc, int x, int y, int size) {
    if (!dc || size <= 0 || !valid_icon(icon)) return false;
    IMAGELISTDRAWPARAMS params{sizeof(params)};
    params.himl = icon.images;
    params.i = icon.index;
    params.hdcDst = dc;
    params.x = x;
    params.y = y;
    params.cx = size;
    params.cy = size;
    params.rgbBk = CLR_NONE;
    params.rgbFg = CLR_DEFAULT;
    params.fStyle = ILD_TRANSPARENT | ILD_SCALE;
    return ImageList_DrawIndirect(&params) != FALSE;
}

} // namespace glint::icons
