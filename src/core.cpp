#include "glint/core.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <cwctype>

namespace glint {
namespace {

[[noreturn]] void fail(const char* message) { throw ProtocolError(message); }

class Parser {
public:
    explicit Parser(std::string_view input) : input_(input) {}

    Json parse() {
        skip();
        if (pos_ >= input_.size()) fail("empty JSON");
        Json result = value();
        skip();
        if (pos_ != input_.size()) fail("trailing JSON data");
        return result;
    }

private:
    std::string_view input_;
    std::size_t pos_ = 0;

    void skip() {
        while (pos_ < input_.size() && (input_[pos_] == ' ' || input_[pos_] == '\t' ||
                                        input_[pos_] == '\r' || input_[pos_] == '\n')) ++pos_;
    }
    char take() {
        if (pos_ >= input_.size()) fail("unexpected end of JSON");
        return input_[pos_++];
    }
    void expect(char c) {
        if (take() != c) fail("malformed JSON");
    }
    Json value() {
        skip();
        if (pos_ >= input_.size()) fail("missing JSON value");
        switch (input_[pos_]) {
        case 'n': literal("null"); return Json(nullptr);
        case 't': literal("true"); return Json(true);
        case 'f': literal("false"); return Json(false);
        case '"': return Json(string());
        case '[': return array();
        case '{': return object();
        default:
            if (input_[pos_] == '-' || (input_[pos_] >= '0' && input_[pos_] <= '9')) return number();
            fail("invalid JSON value");
        }
    }
    void literal(std::string_view text) {
        if (input_.substr(pos_, text.size()) != text) fail("invalid JSON literal");
        pos_ += text.size();
    }
    static int hex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    std::uint32_t escaped_codepoint() {
        std::uint32_t cp = 0;
        for (int i = 0; i < 4; ++i) {
            const int h = hex(take());
            if (h < 0) fail("bad unicode escape");
            cp = (cp << 4) | static_cast<unsigned>(h);
        }
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (take() != '\\' || take() != 'u') fail("unpaired unicode surrogate");
            std::uint32_t low = 0;
            for (int i = 0; i < 4; ++i) {
                const int h = hex(take());
                if (h < 0) fail("bad unicode surrogate");
                low = (low << 4) | static_cast<unsigned>(h);
            }
            if (low < 0xDC00 || low > 0xDFFF) fail("bad unicode surrogate");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            fail("unpaired unicode surrogate");
        }
        return cp;
    }
    static void append_codepoint(std::string& out, std::uint32_t cp) {
        if (cp <= 0x7F) out.push_back(static_cast<char>(cp));
        else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    std::string string() {
        expect('"');
        std::string out;
        while (pos_ < input_.size()) {
            const unsigned char c = static_cast<unsigned char>(input_[pos_++]);
            if (c == '"') {
                if (!valid_utf8(out)) fail("invalid UTF-8 string");
                return out;
            }
            if (c < 0x20) fail("control character in JSON string");
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                continue;
            }
            const char escaped = take();
            switch (escaped) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': append_codepoint(out, escaped_codepoint()); break;
            default: fail("unknown JSON escape");
            }
        }
        fail("unterminated JSON string");
    }
    Json array() {
        expect('['); skip();
        Json::Array out;
        if (pos_ < input_.size() && input_[pos_] == ']') { ++pos_; return out; }
        for (;;) {
            out.push_back(value()); skip();
            if (pos_ >= input_.size()) fail("unterminated JSON array");
            if (input_[pos_] == ']') { ++pos_; return out; }
            expect(','); skip();
        }
    }
    Json object() {
        expect('{'); skip();
        Json::Object out;
        if (pos_ < input_.size() && input_[pos_] == '}') { ++pos_; return out; }
        for (;;) {
            if (pos_ >= input_.size() || input_[pos_] != '"') fail("JSON object key must be string");
            std::string key = string(); skip(); expect(':');
            const bool inserted = out.emplace(std::move(key), value()).second;
            if (!inserted) fail("duplicate JSON object key");
            skip();
            if (pos_ >= input_.size()) fail("unterminated JSON object");
            if (input_[pos_] == '}') { ++pos_; return out; }
            expect(','); skip();
        }
    }
    Json number() {
        const std::size_t begin = pos_;
        if (input_[pos_] == '-') ++pos_;
        if (pos_ >= input_.size()) fail("bad JSON number");
        if (input_[pos_] == '0') ++pos_;
        else {
            if (input_[pos_] < '1' || input_[pos_] > '9') fail("bad JSON number");
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
        }
        bool floating = false;
        if (pos_ < input_.size() && input_[pos_] == '.') {
            floating = true; ++pos_;
            if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') fail("bad JSON number");
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
        }
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            floating = true; ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
            if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') fail("bad JSON exponent");
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
        }
        const auto token = input_.substr(begin, pos_ - begin);
        if (!floating) {
            std::int64_t result = 0;
            auto parsed = std::from_chars(token.data(), token.data() + token.size(), result);
            if (parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size()) return result;
        }
        std::string copy(token);
        char* end = nullptr;
        const double result = std::strtod(copy.c_str(), &end);
        if (end != copy.c_str() + copy.size() || !std::isfinite(result)) fail("bad JSON number");
        return result;
    }
};

void dump_string(std::string& out, std::string_view value) {
    if (!valid_utf8(value)) fail("cannot dump invalid UTF-8");
    static constexpr char hex[] = "0123456789abcdef";
    out.push_back('"');
    for (const unsigned char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out.push_back(hex[c >> 4]); out.push_back(hex[c & 0xF]);
            } else out.push_back(static_cast<char>(c));
        }
    }
    out.push_back('"');
}

void dump_value(std::string& out, const Json& value) {
    const auto& v = value.value();
    if (std::holds_alternative<std::nullptr_t>(v)) { out += "null"; return; }
    if (const auto* b = std::get_if<bool>(&v)) { out += *b ? "true" : "false"; return; }
    if (const auto* i = std::get_if<std::int64_t>(&v)) { out += std::to_string(*i); return; }
    if (const auto* d = std::get_if<double>(&v)) {
        if (!std::isfinite(*d)) fail("cannot dump non-finite number");
        std::ostringstream stream; stream.precision(17); stream << *d; out += stream.str(); return;
    }
    if (const auto* s = std::get_if<std::string>(&v)) { dump_string(out, *s); return; }
    if (const auto* a = std::get_if<Json::Array>(&v)) {
        out.push_back('[');
        for (std::size_t i = 0; i < a->size(); ++i) { if (i) out.push_back(','); dump_value(out, (*a)[i]); }
        out.push_back(']'); return;
    }
    const auto& object = std::get<Json::Object>(v);
    out.push_back('{'); std::size_t i = 0;
    for (const auto& [key, item] : object) { if (i++) out.push_back(','); dump_string(out, key); out.push_back(':'); dump_value(out, item); }
    out.push_back('}');
}

std::wstring lower(std::wstring_view input) {
    std::wstring value(input);
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

const Json* field(const Json& object, std::string_view name) {
    return object.is_object() ? object.find(name) : nullptr;
}
std::int64_t integer_field(const Json& object, std::string_view name, bool required = true) {
    const Json* item = field(object, name);
    if (!item) { if (required) fail("missing integer field"); return 0; }
    if (!item->is_integer()) fail("integer field has wrong type");
    return item->as_integer();
}
std::string string_field(const Json& object, std::string_view name, bool required = true) {
    const Json* item = field(object, name);
    if (!item) { if (required) fail("missing string field"); return {}; }
    if (!item->is_string()) fail("string field has wrong type");
    return item->as_string();
}
bool bool_field(const Json& object, std::string_view name, bool fallback = false) {
    const Json* item = field(object, name);
    if (!item) return fallback;
    if (!item->is_bool()) fail("boolean field has wrong type");
    return item->as_bool();
}

Tab parse_tab(const Json& item, ConnectionId id, const ConnectionInfo& info) {
    if (!item.is_object()) fail("tab must be object");
    Tab tab;
    tab.key = {id, integer_field(item, "tabId")};
    tab.window_id = integer_field(item, "windowId", false);
    tab.index = integer_field(item, "index", false);
    tab.title = utf8_to_wide(string_field(item, "title", false));
    tab.url = utf8_to_wide(string_field(item, "url", false));
    tab.browser = info.browser;
    tab.profile_label = info.profile_label;
    tab.active = bool_field(item, "active");
    tab.pinned = bool_field(item, "pinned");
    // Recency is owned by Glint and confirmed only after a successful result
    // activation. Validate the additive field for protocol compatibility, but
    // never import browser-provided timestamps into the recent-card history.
    const auto reported_access = integer_field(item, "lastAccessed", false);
    if (reported_access < 0) fail("lastAccessed must be nonnegative");
    tab.last_accessed = 0;
    return tab;
}

} // namespace

bool valid_utf8(std::string_view text) noexcept {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i++]);
        std::uint32_t cp = 0; std::size_t continuation = 0;
        if (c <= 0x7F) continue;
        if (c >= 0xC2 && c <= 0xDF) { cp = c & 0x1F; continuation = 1; }
        else if (c >= 0xE0 && c <= 0xEF) { cp = c & 0x0F; continuation = 2; }
        else if (c >= 0xF0 && c <= 0xF4) { cp = c & 0x07; continuation = 3; }
        else return false;
        if (i + continuation > text.size()) return false;
        for (std::size_t j = 0; j < continuation; ++j) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (next & 0x3F);
        }
        if ((continuation == 2 && cp < 0x800) || (continuation == 3 && cp < 0x10000) || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) return false;
    }
    return true;
}

std::wstring utf8_to_wide(std::string_view text) {
    if (!valid_utf8(text)) fail("invalid UTF-8");
    std::wstring out;
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i++]);
        std::uint32_t cp = 0; std::size_t n = 0;
        if (c <= 0x7F) cp = c;
        else if (c <= 0xDF) { cp = c & 0x1F; n = 1; }
        else if (c <= 0xEF) { cp = c & 0x0F; n = 2; }
        else { cp = c & 0x07; n = 3; }
        for (std::size_t j = 0; j < n; ++j) cp = (cp << 6) | (static_cast<unsigned char>(text[i++]) & 0x3F);
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp > 0xFFFF) { cp -= 0x10000; out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10))); out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF))); }
            else out.push_back(static_cast<wchar_t>(cp));
        } else out.push_back(static_cast<wchar_t>(cp));
    }
    return out;
}

std::string wide_to_utf8(std::wstring_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        std::uint32_t cp = static_cast<std::uint32_t>(text[i]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (i + 1 >= text.size()) fail("unpaired wide surrogate");
                const auto low = static_cast<std::uint32_t>(text[++i]);
                if (low < 0xDC00 || low > 0xDFFF) fail("unpaired wide surrogate");
                cp = 0x10000 + ((cp - 0xD800) << 10) + low - 0xDC00;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) fail("unpaired wide surrogate");
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) fail("invalid wide codepoint");
        if (cp <= 0x7F) out.push_back(static_cast<char>(cp));
        else if (cp <= 0x7FF) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else if (cp <= 0xFFFF) { out.push_back(static_cast<char>(0xE0 | (cp >> 12))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else { out.push_back(static_cast<char>(0xF0 | (cp >> 18))); out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    }
    return out;
}

Json Json::parse(std::string_view input) {
    if (input.empty() || input.size() > kMaxMessageBytes) fail("JSON message exceeds limit");
    return Parser(input).parse();
}
Json::Json(std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) fail("JSON integer out of range");
    value_ = static_cast<std::int64_t>(value);
}
std::string Json::dump() const { std::string out; dump_value(out, *this); return out; }
bool Json::is_null() const noexcept { return std::holds_alternative<std::nullptr_t>(value_); }
bool Json::is_object() const noexcept { return std::holds_alternative<Object>(value_); }
bool Json::is_array() const noexcept { return std::holds_alternative<Array>(value_); }
bool Json::is_string() const noexcept { return std::holds_alternative<std::string>(value_); }
bool Json::is_integer() const noexcept { return std::holds_alternative<std::int64_t>(value_); }
bool Json::is_bool() const noexcept { return std::holds_alternative<bool>(value_); }
const std::string& Json::as_string() const { if (!is_string()) fail("JSON value is not string"); return std::get<std::string>(value_); }
std::int64_t Json::as_integer() const { if (!is_integer()) fail("JSON value is not integer"); return std::get<std::int64_t>(value_); }
bool Json::as_bool() const { if (!is_bool()) fail("JSON value is not boolean"); return std::get<bool>(value_); }
const Json::Array& Json::as_array() const { if (!is_array()) fail("JSON value is not array"); return std::get<Array>(value_); }
const Json::Object& Json::as_object() const { if (!is_object()) fail("JSON value is not object"); return std::get<Object>(value_); }
Json::Array& Json::as_array() { if (!is_array()) fail("JSON value is not array"); return std::get<Array>(value_); }
Json::Object& Json::as_object() { if (!is_object()) fail("JSON value is not object"); return std::get<Object>(value_); }
const Json* Json::find(std::string_view key) const noexcept { if (!is_object()) return nullptr; auto it = std::get<Object>(value_).find(key); return it == std::get<Object>(value_).end() ? nullptr : &it->second; }
Json* Json::find(std::string_view key) noexcept { if (!is_object()) return nullptr; auto it = std::get<Object>(value_).find(key); return it == std::get<Object>(value_).end() ? nullptr : &it->second; }
const Json& Json::at(std::string_view key) const { const auto* item = find(key); if (!item) fail("missing JSON object key"); return *item; }
Json& Json::operator[](std::string key) { if (!is_object()) value_ = Object{}; return std::get<Object>(value_)[std::move(key)]; }

FrameReader::FrameReader(std::size_t maximum_bytes) : maximum_bytes_(maximum_bytes) {
    if (maximum_bytes_ == 0 || maximum_bytes_ > std::numeric_limits<std::uint32_t>::max()) fail("invalid frame size limit");
}
std::vector<std::string> FrameReader::feed(std::span<const std::uint8_t> bytes) {
    if (failed_) fail("frame reader is failed");
    std::vector<std::string> frames;
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        if (header_size_ < 4) {
            const auto count = std::min<std::size_t>(4 - header_size_, bytes.size() - pos);
            std::memcpy(header_ + header_size_, bytes.data() + pos, count); header_size_ += count; pos += count;
            if (header_size_ < 4) break;
            expected_ = static_cast<std::uint32_t>(header_[0]) | (static_cast<std::uint32_t>(header_[1]) << 8) | (static_cast<std::uint32_t>(header_[2]) << 16) | (static_cast<std::uint32_t>(header_[3]) << 24);
            if (expected_ == 0 || expected_ > maximum_bytes_) { failed_ = true; fail("invalid frame length"); }
            body_.clear(); body_.reserve(expected_);
        }
        const auto count = std::min<std::size_t>(expected_ - body_.size(), bytes.size() - pos);
        body_.append(reinterpret_cast<const char*>(bytes.data() + pos), count); pos += count;
        if (body_.size() == expected_) {
            if (!valid_utf8(body_)) { failed_ = true; fail("frame is not UTF-8"); }
            frames.push_back(std::move(body_)); body_.clear(); header_size_ = 0; expected_ = 0;
        }
    }
    return frames;
}
bool FrameReader::has_partial_frame() const noexcept { return header_size_ != 0 || !body_.empty(); }
void FrameReader::reset() noexcept { header_size_ = 0; expected_ = 0; body_.clear(); failed_ = false; }

std::vector<std::uint8_t> encode_frame(std::string_view utf8_json, std::size_t maximum_bytes) {
    if (utf8_json.empty() || utf8_json.size() > maximum_bytes || utf8_json.size() > std::numeric_limits<std::uint32_t>::max()) fail("invalid frame size");
    if (!valid_utf8(utf8_json)) fail("frame is not UTF-8");
    const auto length = static_cast<std::uint32_t>(utf8_json.size());
    std::vector<std::uint8_t> out(4 + utf8_json.size());
    out[0] = static_cast<std::uint8_t>(length); out[1] = static_cast<std::uint8_t>(length >> 8); out[2] = static_cast<std::uint8_t>(length >> 16); out[3] = static_cast<std::uint8_t>(length >> 24);
    std::memcpy(out.data() + 4, utf8_json.data(), utf8_json.size());
    return out;
}

bool TabIndex::apply(ConnectionId connection_id, const Json& message) {
    if (connection_id == 0 || !message.is_object()) return false;
    const auto* type_json = field(message, "type");
    if (!type_json || !type_json->is_string()) return false;
    const auto type = type_json->as_string();
    std::lock_guard lock(mutex_);
    try {
        if (integer_field(message, "version") != kProtocolVersion) return false;
    } catch (const ProtocolError&) { return false; }
    if (type == "hello") {
        try {
            State state;
            state.info.connection_id = connection_id;
            state.info.session_id = string_field(message, "sessionId");
            state.info.profile_id = string_field(message, "profileId");
            state.info.browser = utf8_to_wide(string_field(message, "browser"));
            state.info.profile_label = utf8_to_wide(string_field(message, "profileLabel", false));
            states_[connection_id] = std::move(state);
            return true;
        } catch (const ProtocolError&) { return false; }
    }
    auto state_it = states_.find(connection_id);
    if (state_it == states_.end()) return false;
    auto& state = state_it->second;
    if (type == "snapshot") {
        const auto* tabs = field(message, "tabs");
        if (!tabs || !tabs->is_array()) return false;
        std::map<std::int64_t, Tab> replacement;
        try {
            for (const auto& item : tabs->as_array()) {
                Tab tab = parse_tab(item, connection_id, state.info);
                const auto existing = state.tabs.find(tab.key.tab_id);
                if (existing != state.tabs.end())
                    tab.last_accessed = std::max(tab.last_accessed, existing->second.last_accessed);
                if (!replacement.emplace(tab.key.tab_id, std::move(tab)).second) return false;
            }
        } catch (const ProtocolError&) { return false; }
        std::erase_if(state.activations, [&replacement](const auto& pending) {
            return !replacement.contains(pending.second.tab_id);
        });
        state.tabs = std::move(replacement); state.info.synchronized = true; state.info.tab_count = state.tabs.size(); return true;
    }
    if (!state.info.synchronized) return false;
    try {
        if (type == "upsert") {
            const Json* item = field(message, "tab");
            if (!item) item = &message;
            Tab tab = parse_tab(*item, connection_id, state.info);
            const auto existing = state.tabs.find(tab.key.tab_id);
            if (existing != state.tabs.end())
                tab.last_accessed = std::max(tab.last_accessed, existing->second.last_accessed);
            state.tabs[tab.key.tab_id] = std::move(tab); state.info.tab_count = state.tabs.size(); return true;
        }
        if (type == "remove") {
            const auto tab_id = integer_field(message, "tabId");
            std::erase_if(state.activations, [tab_id](const auto& pending) {
                return pending.second.tab_id == tab_id;
            });
            state.tabs.erase(tab_id); state.info.tab_count = state.tabs.size(); return true;
        }
        if (type == "activate_result") {
            const auto request_id = string_field(message, "requestId");
            const auto* ok = field(message, "ok");
            if (!ok || !ok->is_bool()) return false;
            const auto pending = state.activations.find(request_id);
            if (pending == state.activations.end()) return true;
            if (ok->as_bool()) {
                const auto tab = state.tabs.find(pending->second.tab_id);
                if (tab != state.tabs.end())
                    tab->second.last_accessed = std::max(tab->second.last_accessed, pending->second.accessed_at);
            }
            state.activations.erase(pending);
            return true;
        }
    } catch (const ProtocolError&) { return false; }
    return false;
}

bool TabIndex::disconnect(ConnectionId connection_id) {
    std::lock_guard lock(mutex_); return states_.erase(connection_id) != 0;
}

bool TabIndex::track_activation(TabKey key, std::string request_id, std::int64_t accessed_at) {
    if (key.source != TabSource::Browser || request_id.empty() || accessed_at <= 0) return false;
    std::lock_guard lock(mutex_);
    const auto state = states_.find(key.connection_id);
    if (state == states_.end() || !state->second.tabs.contains(key.tab_id)) return false;
    auto& pending = state->second.activations;
    // Bound outstanding requests even if a connected extension stops replying.
    if (pending.size() >= 64) {
        const auto oldest = std::min_element(pending.begin(), pending.end(), [](const auto& left, const auto& right) {
            return left.second.accessed_at < right.second.accessed_at;
        });
        pending.erase(oldest);
    }
    pending.insert_or_assign(std::move(request_id), State::Activation{key.tab_id, accessed_at});
    return true;
}

std::vector<Tab> TabIndex::search(std::wstring_view query, std::size_t limit) const {
    const std::wstring needle = lower(query); std::vector<Tab> result;
    if (limit == 0) return result;
    std::lock_guard lock(mutex_);
    for (const auto& state_entry : states_) {
        const auto& state = state_entry.second;
        for (const auto& tab_entry : state.tabs) {
            const auto& tab = tab_entry.second;
            if (needle.empty() || lower(tab.title).find(needle) != std::wstring::npos || lower(tab.url).find(needle) != std::wstring::npos) {
                result.push_back(tab); if (result.size() >= limit) return result;
            }
        }
    }
    return result;
}
std::vector<ConnectionInfo> TabIndex::connections() const {
    std::vector<ConnectionInfo> result; std::lock_guard lock(mutex_);
    for (const auto& state_entry : states_) result.push_back(state_entry.second.info); return result;
}
std::size_t TabIndex::connection_count() const { std::lock_guard lock(mutex_); return states_.size(); }
std::size_t TabIndex::tab_count() const { std::lock_guard lock(mutex_); std::size_t count = 0; for (const auto& state_entry : states_) count += state_entry.second.tabs.size(); return count; }
bool TabIndex::contains(TabKey key) const {
    if (key.source != TabSource::Browser) return false;
    std::lock_guard lock(mutex_);
    const auto it = states_.find(key.connection_id);
    return it != states_.end() && it->second.tabs.contains(key.tab_id);
}

} // namespace glint
