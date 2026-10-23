#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <utility>

namespace glint {

inline constexpr std::uint32_t kProtocolVersion = 1;
inline constexpr std::size_t kMaxMessageBytes = 1024 * 1024;

class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A small, strict JSON implementation. Strings are always well-formed UTF-8.
class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json, std::less<>>;
    using Value = std::variant<std::nullptr_t, bool, std::int64_t, double,
                               std::string, Array, Object>;

    Json() : value_(nullptr) {}
    Json(std::nullptr_t) : value_(nullptr) {}
    Json(bool value) : value_(value) {}
    Json(int value) : value_(static_cast<std::int64_t>(value)) {}
    Json(std::int64_t value) : value_(value) {}
    Json(std::uint64_t value);
    Json(double value) : value_(value) {}
    Json(const char* value) : value_(std::string(value)) {}
    Json(std::string value) : value_(std::move(value)) {}
    Json(std::string_view value) : value_(std::string(value)) {}
    Json(Array value) : value_(std::move(value)) {}
    Json(Object value) : value_(std::move(value)) {}

    static Json parse(std::string_view input);
    std::string dump() const;

    bool is_null() const noexcept;
    bool is_object() const noexcept;
    bool is_array() const noexcept;
    bool is_string() const noexcept;
    bool is_integer() const noexcept;
    bool is_bool() const noexcept;
    const std::string& as_string() const;
    std::int64_t as_integer() const;
    bool as_bool() const;
    const Array& as_array() const;
    const Object& as_object() const;
    Array& as_array();
    Object& as_object();
    const Json* find(std::string_view key) const noexcept;
    Json* find(std::string_view key) noexcept;
    const Json& at(std::string_view key) const;
    Json& operator[](std::string key);
    const Value& value() const noexcept { return value_; }

private:
    Value value_;
};

// Browser native messaging and local IPC use the same uint32 little-endian
// byte length followed by UTF-8 JSON. Zero and over-limit lengths are invalid.
class FrameReader {
public:
    explicit FrameReader(std::size_t maximum_bytes = kMaxMessageBytes);
    std::vector<std::string> feed(std::span<const std::uint8_t> bytes);
    bool has_partial_frame() const noexcept;
    void reset() noexcept;

private:
    std::size_t maximum_bytes_;
    std::uint8_t header_[4]{};
    std::size_t header_size_ = 0;
    std::uint32_t expected_ = 0;
    std::string body_;
    bool failed_ = false;
};

std::vector<std::uint8_t> encode_frame(std::string_view utf8_json,
                                      std::size_t maximum_bytes = kMaxMessageBytes);
bool valid_utf8(std::string_view text) noexcept;
std::wstring utf8_to_wide(std::string_view text);
std::string wide_to_utf8(std::wstring_view text);

using ConnectionId = std::uint64_t;

// Browser tabs arrive from the native-messaging bridge. Native entries are
// supplied by the desktop process and never enter the browser TabIndex.
enum class TabSource { Browser, Window, UiTab };

struct TabKey {
    ConnectionId connection_id = 0;
    std::int64_t tab_id = 0;
    TabSource source = TabSource::Browser;
    std::wstring target_id;
    bool operator==(const TabKey&) const = default;
};

struct Tab {
    TabKey key;
    std::int64_t window_id = 0;
    std::int64_t index = 0;
    std::wstring title;
    std::wstring url;
    std::wstring browser;
    std::wstring profile_label;
    bool active = false;
    bool pinned = false;
    std::wstring application;
    std::uintptr_t window_handle = 0;
    std::uint32_t process_id = 0;
    std::uint64_t process_creation_time = 0;
    std::vector<int> runtime_id;
    bool operator==(const Tab&) const = default;
};

struct ConnectionInfo {
    ConnectionId connection_id = 0;
    std::string session_id;
    std::string profile_id;
    std::wstring browser;
    std::wstring profile_label;
    bool synchronized = false;
    std::size_t tab_count = 0;
};

// Owns in-memory data only. apply/search/disconnect are thread-safe. A hello
// must precede index messages; a snapshot must precede incremental updates.
// Failed validation leaves the existing connection and its tabs unchanged.
class TabIndex {
public:
    bool apply(ConnectionId connection_id, const Json& message);
    bool disconnect(ConnectionId connection_id);
    std::vector<Tab> search(std::wstring_view query, std::size_t limit = 1000) const;
    std::vector<ConnectionInfo> connections() const;
    std::size_t connection_count() const;
    std::size_t tab_count() const;
    bool contains(TabKey key) const;

private:
    struct State {
        ConnectionInfo info;
        std::map<std::int64_t, Tab> tabs;
    };
    mutable std::mutex mutex_;
    std::map<ConnectionId, State> states_;
};

} // namespace glint
