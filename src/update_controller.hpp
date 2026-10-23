#pragma once

#include "update_schedule.hpp"
#include "update_types.hpp"

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>

namespace glint::update {
enum class State { Idle, Checking, Downloading, Ready, Installing, Error };
struct Snapshot {
    State state = State::Idle;
    bool automatic = true;
    bool checked = false;
    bool notify_ready = false;
    bool exit_to_install = false;
    std::string current_version;
    std::string target_version;
    std::wstring error;
    std::uint64_t received = 0, total = 0;
};
struct ExecutableIdentity {
    std::filesystem::path path;
    std::string sha256;
};
struct Services {
    std::function<std::optional<Release>(std::string_view, const Cancel&)> find;
    std::function<Package(Release, const std::filesystem::path&, const Cancel&, const Progress&)> download;
    std::function<void(const std::filesystem::path&, const Release&)> verify;
    std::function<bool(const Package&, std::wstring&)> install;
    std::function<std::int64_t()> now;
    std::function<ExecutableIdentity()> identity;
};

class Controller {
public:
    using Notify = std::function<void()>;
    Controller(std::string version, std::filesystem::path cache, Notify notify,
        Services services = {}, std::wstring settings_key = L"Software\\Glint\\Updates");
    ~Controller();
    void tick();
    void check(bool manual = true);
    void install();
    bool set_automatic(bool enabled);
    void notified();
    Snapshot snapshot() const;
    void stop();

private:
    mutable std::mutex mutex_;
    Snapshot state_;
    Schedule schedule_;
    std::string blocked_version_, notified_version_;
    std::optional<Package> package_;
    std::optional<ExecutableIdentity> identity_;
    std::filesystem::path cache_;
    std::wstring settings_key_;
    Services services_;
    Notify notify_;
    std::thread worker_;
    std::atomic<bool> cancel_{false}, busy_{false};
    bool stopped_ = false;
    std::int64_t first_check_ = 0;
    void load();
    void save_locked();
    void changed();
    void run_check(bool manual);
    bool safe_package(const Package&) const;
};
} // namespace glint::update
