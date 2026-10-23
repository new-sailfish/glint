#include "update_controller.hpp"
#include <windows.h>
#include <condition_variable>
#include <iostream>
#include <stdexcept>

using namespace glint::update;
void require(bool v, const char* message) { if (!v) throw std::runtime_error(message); }
int main() {
    const auto key = L"Software\\GlintUpdateTest-" + std::to_wstring(GetCurrentProcessId());
    std::mutex mutex;
    std::condition_variable wake;
    std::atomic<int> found{0}, downloaded{0}, installed{0};
    std::atomic<std::int64_t> now{1000};
    const auto cache = std::filesystem::temp_directory_path() / L"Glint-controller-test";
    const ExecutableIdentity original_identity{cache.parent_path() / L"Glint-controller-original.exe", std::string(64, 'b')};
    auto current_identity = original_identity;
    Services services;
    services.identity = [&] { return current_identity; };
    services.now = [&] { return now.load(); };
    services.find = [&](std::string_view current, const Cancel&) -> std::optional<Release> {
        require(current == "0.0.0", "compare actual running version");
        ++found;
        Release release; release.version = "0.1.5"; release.sha256 = std::string(64, 'a'); return release;
    };
    services.download = [&](Release release, const std::filesystem::path& root, const Cancel&, const Progress& progress) {
        ++downloaded; progress(10, 10); return Package{release, root / L"candidate" / L"Glint.exe", root / L"candidate"};
    };
    services.verify = [](const auto&, const auto&) {};
    services.install = [&](const Package&, std::wstring&) { ++installed; return true; };
    auto notify = [&] { wake.notify_all(); };
    auto wait = [&](auto predicate) {
        std::unique_lock lock(mutex);
        require(wake.wait_for(lock, std::chrono::seconds(5), predicate), "controller operation completed");
    };
    try {
        {
            Controller controller("0.0.0", cache, notify, services, key);
            controller.tick();
            require(found == 0, "startup waits thirty seconds");
            now = 1030; controller.tick();
            wait([&] { return controller.snapshot().state == State::Ready; });
            require(found == 1 && downloaded == 1 && installed == 0, "automatic download never authorizes installation");
            require(controller.snapshot().notify_ready, "prepared version notifies once");
            controller.notified();
            require(!controller.snapshot().notify_ready, "notification acknowledged");
            require(controller.set_automatic(false), "persist disabled preference");
            controller.stop();
            require(installed == 0, "ordinary exit never installs");
        }
        {
            Controller controller("0.0.0", cache, notify, services, key);
            require(!controller.snapshot().automatic, "automatic setting survives restart");
            controller.check();
            wait([&] { return controller.snapshot().state == State::Ready; });
            require(found == 1 && downloaded == 1, "cached package restores without network");
            require(!controller.snapshot().notify_ready, "same version does not notify again");
            controller.install();
            wait([&] { return controller.snapshot().exit_to_install; });
            require(installed == 1, "explicit action alone installs");
        }
        const auto offer_release = services.find;
        for (const bool changed_path : {true, false}) {
            RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
            current_identity = original_identity;
            services.find = offer_release;
            {
                Controller controller("0.0.0", cache, notify, services, key);
                controller.check();
                wait([&] { return controller.snapshot().state == State::Ready; });
                controller.stop();
            }
            if (changed_path) current_identity.path = cache.parent_path() / L"Glint-controller-moved.exe";
            else current_identity.sha256 = std::string(64, 'c');
            const int before = found;
            services.find = [&](std::string_view, const Cancel&) -> std::optional<Release> { ++found; return {}; };
            {
                Controller controller("0.0.0", cache, notify, services, key);
                controller.check();
                wait([&] { const auto state = controller.snapshot().state; return state == State::Idle || state == State::Ready; });
                require(controller.snapshot().state == State::Idle && found == before + 1,
                    changed_path ? "moved executable rejects cached pending update" : "same-version replacement rejects cached pending update");
            }
        }
        RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
        current_identity = original_identity;
        services.find = offer_release;
        {
            Controller controller("0.0.0", cache, notify, services, key);
            controller.check();
            wait([&] { return controller.snapshot().state == State::Ready; });
            controller.stop();
        }
        services.identity = []() -> ExecutableIdentity { throw Error("Cannot inspect running executable", FailureKind::Filesystem); };
        const int before_identity_failure = found;
        {
            Controller controller("0.0.0", cache, notify, services, key);
            controller.check();
            wait([&] { const auto state = controller.snapshot().state; return state == State::Error || state == State::Ready; });
            require(controller.snapshot().state == State::Error && !controller.snapshot().error.empty(),
                "identity acquisition failure does not restore Ready");
            require(found == before_identity_failure, "identity acquisition failure does not prepare an unbound update");
        }
        services.identity = [&] { return current_identity; };
        RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
        services.find = [&](std::string_view, const Cancel&) -> std::optional<Release> {
            ++found; throw Error("limited", FailureKind::RateLimit, 120);
        };
        {
            Controller controller("0.0.0", cache, notify, services, key);
            controller.check();
            wait([&] { return controller.snapshot().state == State::Error; });
            const int before = found;
            controller.check();
            require(found == before, "manual check cannot bypass server rate limit");
            now += 121; controller.check();
            wait([&] { return found > before && controller.snapshot().state == State::Error; });
        }
        RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
        std::cout << "Update controller checks passed\n";
    } catch (const std::exception& e) {
        RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str()); std::cerr << e.what() << '\n'; return 1;
    }
}
