#include "update_controller.hpp"
#include "update_feed.hpp"
#include "update_install.hpp"
#include "glint/core.hpp"
#include "language.hpp"

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <limits>

namespace glint::update {
namespace {
constexpr int kSourcePolicy = 2;

std::wstring wide(std::string_view value) {
    if (value.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!count) return L"Invalid UTF-8";
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}
std::string narrow(std::wstring_view value) {
    if (value.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw Error("Invalid update path");
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}
std::int64_t number(const Json& value, const char* field) {
    const auto* item = value.find(field);
    return item && item->is_integer() && item->as_integer() >= 0 ? item->as_integer() : 0;
}
std::string string(const Json& value, const char* field) {
    const auto* item = value.find(field);
    return item && item->is_string() ? item->as_string() : std::string{};
}
bool no_reparse(std::filesystem::path path) {
    for (; !path.empty(); ) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        const auto parent = path.parent_path();
        if (parent == path) break;
        path = parent;
    }
    return true;
}
ExecutableIdentity validate_identity(ExecutableIdentity identity) {
    if (!identity.path.is_absolute() || identity.path.native().find(L'\0') != std::wstring::npos ||
        identity.path.filename().empty() || identity.sha256.size() != 64 ||
        !std::all_of(identity.sha256.begin(), identity.sha256.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) throw Error("Could not identify the running executable", FailureKind::Filesystem);
    identity.path = identity.path.lexically_normal();
    return identity;
}
ExecutableIdentity running_identity() {
    std::wstring image(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
    if (!length || length >= image.size())
        throw Error("Could not locate the running executable", FailureKind::Filesystem);
    image.resize(length);
    std::filesystem::path path(image);
    return {path, file_sha256(path)};
}
} // namespace

Controller::Controller(std::string version, std::filesystem::path cache, Notify notify,
    Services services, std::wstring settings_key)
    : cache_(std::filesystem::absolute(cache).lexically_normal()), settings_key_(std::move(settings_key)),
      services_(std::move(services)), notify_(std::move(notify)) {
    state_.current_version = std::move(version);
    if (!services_.find) services_.find = find_release;
    if (!services_.download) services_.download = download_release;
    if (!services_.verify) services_.verify = verify_binary;
    if (!services_.install) services_.install = start_install;
    if (!services_.now) services_.now = [] {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    };
    if (!services_.identity) services_.identity = running_identity;
    try { identity_ = validate_identity(services_.identity()); }
    catch (const std::exception& error) { state_.state = State::Error; state_.error = wide(error.what()); }
    load();
    const auto failed = failed_install_version();
    if (!failed.empty()) {
        blocked_version_ = failed;
        if (package_ && package_->release.version == failed) package_.reset();
    }
    first_check_ = services_.now() + 30;
}
Controller::~Controller() { stop(); }
void Controller::load() {
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, settings_key_.c_str(), L"State", RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t) || bytes > 32768 || bytes % sizeof(wchar_t)) return;
    std::wstring text(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, settings_key_.c_str(), L"State", RRF_RT_REG_SZ, nullptr, text.data(), &bytes) != ERROR_SUCCESS) return;
    try {
        text.resize(wcsnlen(text.c_str(), text.size()));
        const auto value = Json::parse(narrow(text));
        const auto* automatic = value.find("automatic");
        if (automatic && automatic->is_bool()) state_.automatic = automatic->as_bool();
        schedule_.last_success = number(value, "lastCheck");
        schedule_.retry_at = number(value, "retryAt");
        schedule_.server_limit = number(value, "serverLimit");
        schedule_.failures = static_cast<unsigned>((std::min)(number(value, "failures"), std::int64_t{100}));
        // Older releases treated one GitHub limit as a cooldown for every source.
        if (number(value, "sourcePolicy") < kSourcePolicy && schedule_.server_limit) {
            schedule_.server_limit = 0;
            schedule_.retry_at = 0;
        }
        // Bound future persisted times so a clock correction cannot disable updates forever.
        const auto now = services_.now();
        if (schedule_.retry_at > now + 604800) schedule_.retry_at = 0;
        if (schedule_.server_limit > now + 604800) schedule_.server_limit = 0;
        blocked_version_ = string(value, "blocked");
        notified_version_ = string(value, "notified");
        const auto* pending = value.find("pending");
        if (pending && pending->is_object() && identity_ &&
            string(*pending, "fromVersion") == state_.current_version &&
            string(*pending, "fromPath") == narrow(identity_->path.wstring()) &&
            string(*pending, "fromSHA256") == identity_->sha256) {
            Package candidate;
            candidate.release.version = string(*pending, "version");
            candidate.release.sha256 = string(*pending, "sha256");
            candidate.release.size = static_cast<std::uint64_t>(number(*pending, "size"));
            candidate.directory = std::filesystem::path(wide(string(*pending, "directory")));
            candidate.executable = candidate.directory / L"Glint.exe";
            if (safe_package(candidate) && candidate.release.version != blocked_version_) package_ = std::move(candidate);
        }
    } catch (const std::exception&) { /* An invalid local record is never executable authority. */ }
}
void Controller::save_locked() {
    Json value = Json::Object{{"sourcePolicy", kSourcePolicy}, {"automatic", state_.automatic}, {"lastCheck", schedule_.last_success},
        {"retryAt", schedule_.retry_at}, {"serverLimit", schedule_.server_limit}, {"failures", static_cast<int>(schedule_.failures)},
        {"blocked", blocked_version_}, {"notified", notified_version_}};
    if (package_) {
        if (!identity_) throw Error("Cannot save an update without the running executable identity", FailureKind::Filesystem);
        const auto& p = *package_;
        value["pending"] = Json::Object{{"fromVersion", state_.current_version},
            {"fromPath", narrow(identity_->path.wstring())}, {"fromSHA256", identity_->sha256}, {"version", p.release.version},
            {"sha256", p.release.sha256}, {"size", p.release.size}, {"directory", narrow(p.directory.wstring())}};
    }
    const auto text = wide(value.dump());
    HKEY key = nullptr;
    LSTATUS result = RegCreateKeyExW(HKEY_CURRENT_USER, settings_key_.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result == ERROR_SUCCESS) {
        result = RegSetValueExW(key, L"State", 0, REG_SZ, reinterpret_cast<const BYTE*>(text.c_str()),
            static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
    if (result != ERROR_SUCCESS) throw Error("Could not save update settings (Windows " + std::to_string(result) + ")", FailureKind::Filesystem);
}
bool Controller::safe_package(const Package& p) const {
    return p.directory.is_absolute() && p.directory.lexically_normal().parent_path() == cache_ &&
        p.executable.lexically_normal() == p.directory.lexically_normal() / L"Glint.exe" &&
        !p.directory.filename().empty() && no_reparse(p.executable) && p.release.sha256.size() == 64 && !p.release.version.empty();
}
Snapshot Controller::snapshot() const { std::lock_guard lock(mutex_); return state_; }
void Controller::changed() { if (notify_) notify_(); }
void Controller::tick() {
    bool run = false;
    {
        std::lock_guard lock(mutex_);
        const auto now = services_.now();
        run = !stopped_ && !busy_ && now >= first_check_ && state_.state != State::Ready &&
            state_.state != State::Installing && (package_.has_value() || (state_.automatic && schedule_.due(now)));
    }
    if (run) check(false);
}
void Controller::check(bool manual) {
    if (busy_) return;
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard lock(mutex_);
        if (stopped_ || state_.state == State::Ready || state_.state == State::Installing) return;
        if (!package_ && !schedule_.manual_allowed(services_.now())) {
            state_.state = State::Error;
            state_.error = i18n::text(L"All update sources are temporarily rate-limited. Please try again after the retry interval.",
                L"所有更新源暂时受到请求限制，请等待重试间隔后再试。");
        } else {
            cancel_ = false; busy_ = true;
            state_.state = State::Checking; state_.error.clear(); state_.checked = false;
            if (manual) blocked_version_.clear();
        }
    }
    changed();
    if (busy_) worker_ = std::thread([this, manual] { run_check(manual); });
}
void Controller::run_check(bool manual) {
    std::string candidate_version;
    try {
        auto identity = validate_identity(services_.identity());
        {
            std::lock_guard lock(mutex_);
            if (identity_ && (identity_->path != identity.path || identity_->sha256 != identity.sha256)) package_.reset();
            identity_ = std::move(identity);
        }
        std::optional<Package> cached;
        { std::lock_guard lock(mutex_); cached = package_; }
        if (cached) {
            try {
                if (!safe_package(*cached)) throw Error("Unsafe update cache");
                services_.verify(cached->executable, cached->release);
            } catch (const std::exception&) {
                cached.reset(); std::lock_guard lock(mutex_); package_.reset(); save_locked();
            }
        }
        if (!cached) {
            if (cancel_) throw Error("Cancelled", FailureKind::Cancelled);
            // A disabled setting still permits restoring an already downloaded package.
            if (!manual && !snapshot().automatic) throw Error("Cancelled", FailureKind::Cancelled);
            auto release = services_.find(state_.current_version, [this] { return cancel_.load(); });
            if (release) {
                candidate_version = release->version;
                {
                    std::lock_guard lock(mutex_);
                    if (!manual && blocked_version_ == candidate_version) throw Error("This update failed validation or installation. Use Check for updates to retry.");
                    state_.state = State::Downloading; state_.target_version = release->version; state_.received = 0; state_.total = release->size;
                }
                changed();
                cached = services_.download(*release, cache_, [this] { return cancel_.load(); },
                    [this](std::uint64_t received, std::uint64_t total) {
                        { std::lock_guard lock(mutex_); state_.received = received; state_.total = total; }
                        changed();
                    });
                if (!safe_package(*cached)) throw Error("Downloaded update escaped its cache directory");
            }
        }
        if (cancel_) throw Error("Cancelled", FailureKind::Cancelled);
        {
            std::lock_guard lock(mutex_);
            package_ = std::move(cached); schedule_.success(services_.now()); state_.checked = true;
            state_.state = package_ ? State::Ready : State::Idle;
            state_.target_version = package_ ? package_->release.version : std::string{};
            state_.notify_ready = package_ && notified_version_ != package_->release.version;
            save_locked();
        }
    } catch (const Error& error) {
        std::lock_guard lock(mutex_);
        if (error.kind == FailureKind::Cancelled) { state_.state = State::Idle; state_.error.clear(); }
        else {
            state_.state = State::Error; state_.error = wide(error.what());
            schedule_.failure(services_.now(), error.retry_after_seconds);
            if (error.kind == FailureKind::Invalid && !candidate_version.empty()) blocked_version_ = candidate_version;
            try { save_locked(); } catch (const std::exception&) {}
        }
    } catch (const std::exception& error) {
        std::lock_guard lock(mutex_);
        state_.state = State::Error; state_.error = wide(error.what()); schedule_.failure(services_.now());
        try { save_locked(); } catch (const std::exception&) {}
    }
    busy_ = false; changed();
}
void Controller::install() {
    if (busy_) return;
    if (worker_.joinable()) worker_.join();
    Package candidate;
    {
        std::lock_guard lock(mutex_);
        if (stopped_ || state_.state != State::Ready || !package_) return;
        candidate = *package_; state_.state = State::Installing; state_.error.clear(); busy_ = true;
    }
    changed();
    worker_ = std::thread([this, candidate] {
        std::wstring error;
        bool started = false;
        try {
            if (!safe_package(candidate)) throw Error("Unsafe update cache");
            services_.verify(candidate.executable, candidate.release);
            started = services_.install(candidate, error);
        } catch (const std::exception& e) { error = wide(e.what()); }
        {
            std::lock_guard lock(mutex_);
            state_.exit_to_install = started;
            if (!started) {
                state_.state = State::Error; state_.error = std::move(error); blocked_version_ = candidate.release.version;
                package_.reset();
                try { save_locked(); } catch (const std::exception&) {}
            }
        }
        busy_ = false; changed();
    });
}
bool Controller::set_automatic(bool enabled) {
    bool success = true;
    {
        std::lock_guard lock(mutex_);
        const bool before = state_.automatic;
        state_.automatic = enabled;
        try { save_locked(); }
        catch (const std::exception& error) { state_.automatic = before; state_.error = wide(error.what()); success = false; }
        if (success && !enabled && state_.state != State::Installing) cancel_ = true;
    }
    changed(); return success;
}
void Controller::notified() {
    std::lock_guard lock(mutex_);
    if (!state_.notify_ready) return;
    notified_version_ = state_.target_version; state_.notify_ready = false;
    try { save_locked(); } catch (const std::exception&) {}
}
void Controller::stop() {
    { std::lock_guard lock(mutex_); stopped_ = true; cancel_ = true; }
    if (worker_.joinable()) worker_.join();
}
} // namespace glint::update
