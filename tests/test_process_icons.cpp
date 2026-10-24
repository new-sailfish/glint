#include "process_icons.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using glint::icons::Cache;
using glint::icons::Icon;

namespace {

template <typename Predicate>
bool wait_for(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}

struct Images {
    HIMAGELIST value = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, 1, 1);
    Images() {
        assert(value);
        assert(ImageList_AddIcon(value, LoadIconW(nullptr, IDI_APPLICATION)) == 0);
    }
    ~Images() { ImageList_Destroy(value); }
    Icon icon() const { return {value, 0}; }
};

struct Gate {
    std::mutex mutex;
    std::condition_variable condition;
    bool released = false;
    std::atomic_int entered{0};
    std::atomic_int exited{0};
    void wait() {
        ++entered;
        std::unique_lock lock(mutex);
        condition.wait(lock, [this] { return released; });
        ++exited;
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        condition.notify_all();
    }
};

struct TestClock {
    std::atomic_int seconds{0};
    auto now() const {
        return std::chrono::steady_clock::time_point{} + std::chrono::seconds(seconds.load());
    }
};

void wait_update(Cache& cache) {
    assert(wait_for([&] { return !cache.take_updates().empty(); }));
}

void requests_are_asynchronous_deduplicated_and_case_insensitive() {
    Images images;
    Gate gate;
    Cache cache([&](const std::wstring& path, UINT dpi) {
        assert(path == L"C:/Apps/Browser.exe");
        assert(dpi == 96);
        APTTYPE apartment{};
        APTTYPEQUALIFIER qualifier{};
        assert(SUCCEEDED(CoGetApartmentType(&apartment, &qualifier)));
        assert(apartment == APTTYPE_STA || apartment == APTTYPE_MAINSTA);
        gate.wait();
        return images.icon();
    });
    cache.start(nullptr, 0);
    assert(!cache.get(L"C:/Apps/Browser.exe", 96).images);
    assert(wait_for([&] { return gate.entered == 1; }));
    const auto before = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) assert(!cache.get(L"c:\\apps\\browser.exe", 96).images);
    assert(std::chrono::steady_clock::now() - before < 500ms);
    assert(gate.entered == 1);
    gate.release();
    wait_update(cache);
    const Icon hit = cache.get(L"C:\\APPS\\BROWSER.EXE", 96);
    assert(hit.images == images.value && hit.index == 0);
    assert(gate.entered == 1);
}

void failures_retry_after_backoff_and_successes_refresh_in_background() {
    Images images;
    TestClock clock;
    std::atomic_int attempts{0};
    Cache cache([&](const std::wstring&, UINT) {
        return ++attempts == 1 ? Icon{} : images.icon();
    }, [&] { return clock.now(); });
    cache.start(nullptr, 0);
    assert(!cache.get(L"C:\\retry.exe", 96).images);
    wait_update(cache);
    assert(attempts == 1);
    clock.seconds = 29;
    for (int i = 0; i < 20; ++i) assert(!cache.get(L"C:\\retry.exe", 96).images);
    assert(attempts == 1);
    clock.seconds = 30;
    cache.get(L"C:\\retry.exe", 96);
    wait_update(cache);
    assert(attempts == 2);
    assert(cache.get(L"C:\\retry.exe", 96).images == images.value);
    clock.seconds = 89;
    assert(cache.get(L"C:\\retry.exe", 96).images == images.value);
    assert(attempts == 2);
    clock.seconds = 90;
    // A refresh must retain the old icon until its replacement arrives.
    assert(cache.get(L"C:\\retry.exe", 96).images == images.value);
    wait_update(cache);
    assert(attempts == 3);
}

void invalid_cached_indexes_are_reacquired_without_waiting_for_refresh() {
    Images images;
    Gate gate;
    TestClock clock;
    std::atomic_int attempts{0};
    Cache cache([&](const std::wstring&, UINT) {
        if (++attempts == 2) {
            gate.wait();
            assert(ImageList_AddIcon(images.value, LoadIconW(nullptr, IDI_APPLICATION)) == 0);
        }
        return images.icon();
    }, [&] { return clock.now(); });
    cache.start(nullptr, 0);
    cache.get(L"C:\\changed-list.exe", 96);
    wait_update(cache);
    assert(cache.get(L"C:\\changed-list.exe", 96).images == images.value);
    // Mutate only the test-owned fixture; the production cache never modifies
    // a Shell image list. The handle survives but its cached index is invalid.
    assert(ImageList_Remove(images.value, 0));
    assert(!cache.get(L"C:\\changed-list.exe", 96).images);
    assert(wait_for([&] { return gate.entered == 1; }));
    assert(!cache.get(L"C:\\changed-list.exe", 96).images);
    assert(attempts == 2);
    gate.release();
    wait_update(cache);
    assert(cache.get(L"C:\\changed-list.exe", 96).images == images.value);
}

void shell_lookup_preserves_the_original_path_spelling() {
    Images images;
    std::wstring queried;
    const std::wstring original = L"C:\\CaseSensitive\\MyApp.exe";
    Cache cache([&](const std::wstring& path, UINT) {
        queried = path;
        return images.icon();
    });
    cache.start(nullptr, 0);
    cache.get(original, 96);
    wait_update(cache);
    assert(queried == original);
    assert(cache.get(L"c:/casesensitive/myapp.exe", 96).images == images.value);
}

void unicode_paths_use_the_same_normalization_for_updates_and_lookups() {
    Images images;
    const std::wstring original = L"C:/Users/Éric/Glint.EXE";
    const std::wstring normalized = L"c:\\users\\éric\\glint.exe";
    assert(glint::icons::normalize_path(original) == normalized);
    Cache cache([&](const std::wstring& path, UINT) {
        assert(path == original);
        return images.icon();
    });
    cache.start(nullptr, 0);
    cache.get(original, 96);
    std::vector<std::wstring> updates;
    assert(wait_for([&] {
        updates = cache.take_updates();
        return !updates.empty();
    }));
    assert(updates.size() == 1 && updates.front() == normalized);
    assert(cache.get(normalized, 96).images == images.value);
}

void stale_inflight_results_are_discarded_after_invalidation() {
    Images images;
    Gate gate;
    std::atomic_int attempts{0};
    Cache cache([&](const std::wstring&, UINT dpi) {
        if (++attempts == 1) {
            assert(dpi == 96);
            gate.wait();
            return Icon{};
        }
        assert(dpi == 144);
        return images.icon();
    });
    cache.start(nullptr, 0);
    cache.get(L"C:\\dpi.exe", 96);
    assert(wait_for([&] { return gate.entered == 1; }));
    cache.invalidate();
    assert(!cache.get(L"C:\\dpi.exe", 144).images);
    gate.release();
    wait_update(cache);
    assert(attempts == 2);
    assert(cache.get(L"C:\\dpi.exe", 144).images == images.value);
    assert(cache.take_updates().empty());
}

void cache_and_pending_queue_are_bounded() {
    Images images;
    Gate gate;
    std::atomic_int attempts{0};
    Cache cache([&](const std::wstring&, UINT) {
        if (++attempts == 1) gate.wait();
        return images.icon();
    });
    cache.start(nullptr, 0);
    cache.get(L"C:\\0.exe", 96);
    assert(wait_for([&] { return gate.entered == 1; }));
    for (int i = 1; i < 1024; ++i) cache.get(L"C:\\" + std::to_wstring(i) + L".exe", 96);
    gate.release();
    assert(wait_for([&] { return attempts == 256; }));
    // All 256 admitted keys have completed. A later key can evict completed metadata.
    wait_update(cache);
    assert(!cache.get(L"C:\\1024.exe", 96).images);
    assert(wait_for([&] { return attempts == 257; }));
    assert(wait_for([&] { return cache.get(L"C:\\1024.exe", 96).images == images.value; }));
    cache.get(L"C:\\0.exe", 96);
    assert(wait_for([&] { return attempts == 258; }));
}

void capacity_rejections_request_a_coalesced_retry_when_a_slot_completes() {
    Images images;
    Gate gate;
    std::atomic_int attempts{0};
    Cache cache([&](const std::wstring&, UINT) {
        if (++attempts == 1) gate.wait();
        return images.icon();
    });
    cache.start(nullptr, 0);
    cache.get(L"C:\\capacity-0.exe", 96);
    assert(wait_for([&] { return gate.entered == 1; }));
    for (int i = 1; i < 256; ++i)
        cache.get(L"C:\\capacity-" + std::to_wstring(i) + L".exe", 96);
    for (int i = 0; i < 20; ++i)
        assert(!cache.get(L"C:\\rejected.exe", 96).images);
    gate.release();
    std::vector<std::wstring> updates;
    assert(wait_for([&] {
        updates = cache.take_updates();
        return !updates.empty();
    }));
    assert(std::count(updates.begin(), updates.end(), L"") == 1);
    assert(wait_for([&] { return attempts == 256; }));
    const auto later = cache.take_updates();
    assert(std::count(later.begin(), later.end(), L"") == 0);
    assert(wait_for([&] { return cache.get(L"C:\\rejected.exe", 96).images == images.value; }));
    assert(attempts == 257);
}

void stop_does_not_wait_indefinitely_for_shell_and_drops_late_updates() {
    const auto gate = std::make_shared<Gate>();
    Cache cache([gate](const std::wstring&, UINT) {
        gate->wait();
        return Icon{};
    });
    cache.start(nullptr, 0);
    cache.get(L"C:\\blocked.exe", 96);
    assert(wait_for([&] { return gate->entered == 1; }));
    const auto before = std::chrono::steady_clock::now();
    cache.stop();
    assert(std::chrono::steady_clock::now() - before < 1s);
    assert(gate->exited == 0);
    gate->release();
    assert(wait_for([&] { return gate->exited == 1; }));
    assert(cache.take_updates().empty());
    assert(!cache.get(L"C:\\blocked.exe", 96).images);
}

void invalid_icons_are_rejected_and_index_zero_is_drawable() {
    Images images;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, 64, 64);
    const HGDIOBJ previous = SelectObject(dc, bitmap);
    assert(!glint::icons::draw_icon({}, dc, 0, 0, 16));
    assert(!glint::icons::draw_icon({images.value, -1}, dc, 0, 0, 16));
    assert(!glint::icons::draw_icon({images.value, 1}, dc, 0, 0, 16));
    assert(!glint::icons::draw_icon(images.icon(), dc, 0, 0, 0));
    assert(glint::icons::draw_icon(images.icon(), dc, 0, 0, 24));
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}

void default_loader_returns_shell_icons_for_an_existing_executable() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    assert(length > 0 && length < path.size());
    path.resize(length);
    Cache cache;
    cache.start(nullptr, 0);
    assert(!cache.get(path, 96).images);
    wait_update(cache);
    const Icon icon = cache.get(path, 96);
    assert(icon.images && icon.index >= 0 && icon.index < ImageList_GetImageCount(icon.images));
    assert(!cache.get(L"C:\\this-glint-icon-test-file-does-not-exist.exe", 96).images);
    wait_update(cache);
    assert(!cache.get(L"C:\\this-glint-icon-test-file-does-not-exist.exe", 96).images);
}

} // namespace

int main() {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    assert(InitCommonControlsEx(&controls));
    invalid_cached_indexes_are_reacquired_without_waiting_for_refresh();
    shell_lookup_preserves_the_original_path_spelling();
    unicode_paths_use_the_same_normalization_for_updates_and_lookups();
    requests_are_asynchronous_deduplicated_and_case_insensitive();
    failures_retry_after_backoff_and_successes_refresh_in_background();
    stale_inflight_results_are_discarded_after_invalidation();
    cache_and_pending_queue_are_bounded();
    capacity_rejections_request_a_coalesced_retry_when_a_slot_completes();
    stop_does_not_wait_indefinitely_for_shell_and_drops_late_updates();
    invalid_icons_are_rejected_and_index_zero_is_drawable();
    default_loader_returns_shell_icons_for_an_existing_executable();
    std::cout << "Process icon cache tests passed\n";
}
