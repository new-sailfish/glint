#include "update_controller.hpp"
#include "glint/core.hpp"
#include <windows.h>
#include <condition_variable>
#include <iostream>
#include <stdexcept>

using namespace glint::update;
void require(bool v, const char* message) { if (!v) throw std::runtime_error(message); }
void save_state(const std::wstring& key, const std::wstring& state) {
    HKEY handle = nullptr;
    require(RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr) == ERROR_SUCCESS,
        "create controller state fixture");
    const auto result = RegSetValueExW(handle, L"State", 0, REG_SZ, reinterpret_cast<const BYTE*>(state.c_str()),
        static_cast<DWORD>((state.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(handle);
    require(result == ERROR_SUCCESS, "write controller state fixture");
}
glint::Json load_state(const std::wstring& key) {
    DWORD bytes = 0;
    require(RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"State", RRF_RT_REG_SZ, nullptr, nullptr, &bytes) == ERROR_SUCCESS,
        "measure controller state");
    std::wstring state(bytes / sizeof(wchar_t), L'\0');
    require(RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"State", RRF_RT_REG_SZ, nullptr, state.data(), &bytes) == ERROR_SUCCESS,
        "read controller state");
    state.resize(wcsnlen(state.c_str(), state.size()));
    std::string json;
    json.reserve(state.size());
    for (const auto ch : state) {
        require(ch <= 0x7f, "controller state fixture is ASCII");
        json.push_back(static_cast<char>(ch));
    }
    return glint::Json::parse(json);
}
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
        services.find = [&](std::string_view, const Cancel&) -> std::optional<Release> { ++found; return {}; };
        for (const auto* policy : {L"", L"\"sourcePolicy\":1,"}) {
            RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
            save_state(key, std::wstring(L"{") + policy +
                L"\"automatic\":true,\"retryAt\":10000,\"serverLimit\":8000,\"failures\":2}");
            now = 3000;
            const int before = found;
            Controller controller("0.0.0", cache, notify, services, key);
            now = 3030; controller.tick();
            wait([&] { return controller.snapshot().checked; });
            require(found == before + 1, "legacy GitHub cooldown allows immediate automatic fallback check");
        }
        RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
        save_state(key, L"{\"automatic\":true,\"retryAt\":10000,\"serverLimit\":0,\"failures\":2}");
        {
            now = 3000;
            const int before = found;
            Controller controller("0.0.0", cache, notify, services, key);
            now = 3030; controller.tick();
            require(controller.snapshot().state == State::Idle && found == before,
                "legacy ordinary failure keeps automatic backoff");
        }
        RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
        services.find = [&](std::string_view, const Cancel&) -> std::optional<Release> {
            ++found; throw Error("limited", FailureKind::RateLimit, 120);
        };
        {
            Controller controller("0.0.0", cache, notify, services, key);
            controller.check();
            wait([&] { return controller.snapshot().state == State::Error; });
            controller.stop();
            const auto state = load_state(key);
            const auto* policy = state.find("sourcePolicy");
            require(policy && policy->is_integer() && policy->as_integer() == 2,
                "saved all-source cooldown records current source policy");
        }
        {
            Controller controller("0.0.0", cache, notify, services, key);
            const int before = found;
            controller.check();
            require(controller.snapshot().state == State::Error && found == before,
                "restarted manual check cannot bypass all-source rate limit");
            now += 121; controller.check();
            wait([&] { return found > before && controller.snapshot().state == State::Error; });
        }
        RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
        std::cout << "Update controller checks passed\n";
    } catch (const std::exception& e) {
        RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str()); std::cerr << e.what() << '\n'; return 1;
    }
}
