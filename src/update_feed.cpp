#include "update_feed.hpp"
#include "glint/core.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

namespace glint::update {
namespace {
constexpr std::string_view kApiRoot = "https://api.github.com/repos/new-sailfish/glint";
constexpr std::string_view kGithubRoot = "https://github.com/new-sailfish/glint";
constexpr std::string_view kAssetRoot = "https://release-assets.githubusercontent.com/";
constexpr std::string_view kBinaryName = "Glint.exe";
constexpr std::string_view kManifestName = "release-manifest.json";
constexpr std::string_view kPagesRoot = "https://new-sailfish.github.io/glint/updates/";
constexpr std::array<std::string_view, 4> kManifestRoots{
    "https://raw.githubusercontent.com/new-sailfish/glint/update-feed/", kPagesRoot,
    "https://gcore.jsdelivr.net/gh/new-sailfish/glint@update-feed/",
    "https://testingcf.jsdelivr.net/gh/new-sailfish/glint@update-feed/"};
constexpr std::array<std::string_view, 4> kProxyRoots{
    "https://ghfast.top/", "https://gh-proxy.com/", "https://ghfile.geekertao.top/", "https://ghproxy.net/"};

std::string origin(std::string_view url) {
    return std::string(url.substr(0, url.find('/', 8)));
}

// Keep a limited endpoint quiet without preventing independent hosts from serving updates.
class SourceFailures {
  public:
    void check(std::string_view url) const {
        const auto found = limited_.find(origin(url));
        if (found != limited_.end())
            throw found->second;
    }
    void record(std::string_view url, const Error &error) {
        if (error.kind == FailureKind::Cancelled || error.kind == FailureKind::Filesystem)
            throw error;
        if (error.kind == FailureKind::RateLimit) {
            limited_.insert_or_assign(error.source_origin.empty() ? origin(url) : error.source_origin, error);
            const auto delay = error.retry_after_seconds ? error.retry_after_seconds : 3600;
            earliest_retry_ = earliest_retry_ ? std::min(earliest_retry_, delay) : delay;
        } else {
            if (error.kind == FailureKind::Invalid || !failure_ || failure_->kind != FailureKind::Invalid)
                failure_ = error;
        }
    }
    [[noreturn]] void raise() const {
        if (failure_)
            throw Error(std::string("All update sources failed. ") + failure_->what(), failure_->kind);
        throw Error("All update sources are temporarily rate-limited. Please try again later.",
                    FailureKind::RateLimit, earliest_retry_);
    }

  private:
    std::map<std::string, Error> limited_;
    std::optional<Error> failure_;
    std::uint64_t earliest_retry_ = 0;
};

std::vector<feed::Request> manifest_sources(const std::optional<Release> &candidate) {
    const auto suffix = candidate ? "v" + candidate->version + "/" + std::string(kManifestName)
                                  : std::string(kManifestName);
    const auto direct = candidate ? candidate->manifest_url
                                 : std::string(kGithubRoot) + "/releases/latest/download/" + std::string(kManifestName);
    std::vector<feed::Request> sources{{direct}};
    if (candidate && !candidate->manifest_api_url.empty())
        sources.push_back({candidate->manifest_api_url, true});
    for (const auto root : kManifestRoots)
        sources.push_back({std::string(root) + suffix});
    for (const auto root : kProxyRoots)
        sources.push_back({std::string(root) + direct});
    return sources;
}

std::vector<feed::Request> binary_sources(const Release &release) {
    std::vector<feed::Request> sources{{release.binary_url, false, feed::kBinaryLimit}};
    if (!release.binary_api_url.empty())
        sources.push_back({release.binary_api_url, true, feed::kBinaryLimit});
    sources.push_back({std::string(kPagesRoot) + "v" + release.version + "/" + std::string(kBinaryName),
                       false, feed::kBinaryLimit});
    for (const auto root : kProxyRoots)
        sources.push_back({std::string(root) + release.binary_url, false, feed::kBinaryLimit});
    return sources;
}
struct Version {
    std::array<std::uint32_t, 3> values{};
};
[[noreturn]] void invalid(const char *message) {
    throw Error(message, FailureKind::Invalid);
}
void check_cancel(const Cancel &cancel) {
    if (cancel && cancel())
        throw Error("update cancelled", FailureKind::Cancelled);
}
Version parse_version(std::string_view text) {
    Version result;
    std::size_t begin = 0;
    for (std::size_t part = 0; part < 3; ++part) {
        const auto end = text.find('.', begin);
        const auto token = text.substr(begin, end == std::string_view::npos ? text.size() - begin : end - begin);
        if (token.empty() || (token.size() > 1 && token.front() == '0'))
            invalid("version is not canonical numeric x.y.z");
        std::uint32_t value = 0;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || value > 65535)
            invalid("version component out of range");
        result.values[part] = value;
        if (end == std::string_view::npos) {
            if (part != 2)
                invalid("version must contain three components");
            begin = text.size();
        } else {
            if (part == 2)
                invalid("version has extra components");
            begin = end + 1;
        }
    }
    if (begin != text.size())
        invalid("version has extra components");
    return result;
}
bool version_equal(std::string_view a, std::string_view b) {
    try {
        return parse_version(a).values == parse_version(b).values;
    } catch (...) {
        return false;
    }
}
bool version_newer(std::string_view candidate, std::string_view current) {
    return parse_version(candidate).values > parse_version(current).values;
}
std::string pinned_url(std::string_view version, std::string_view name) {
    return std::string(kGithubRoot) + "/releases/download/v" + std::string(version) + "/" + std::string(name);
}
bool is_hex_hash(std::string_view value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}
std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
const glint::Json *field(const glint::Json &object, std::string_view name) {
    return object.is_object() ? object.find(name) : nullptr;
}
std::string string_field(const glint::Json &object, std::string_view name, bool required = true) {
    const auto *value = field(object, name);
    if (!value || !value->is_string()) {
        if (required)
            invalid("release metadata has a missing string field");
        return {};
    }
    return value->as_string();
}
std::int64_t integer_field(const glint::Json &object, std::string_view name, bool required = true) {
    const auto *value = field(object, name);
    if (!value || !value->is_integer()) {
        if (required)
            invalid("release metadata has a missing integer field");
        return 0;
    }
    return value->as_integer();
}
bool bool_field(const glint::Json &object, std::string_view name, bool fallback = false) {
    const auto *value = field(object, name);
    if (!value)
        return fallback;
    if (!value->is_bool())
        invalid("release metadata has a non-boolean field");
    return value->as_bool();
}
glint::Json parse_json(std::string_view text) {
    if (text.size() > feed::kMetadataLimit)
        invalid("release metadata exceeds 1 MiB");
    // Bound the shared JSON parser's recursive descent before entering it.
    int depth = 0;
    bool quoted = false, escaped = false;
    for (const char c : text) {
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
        } else if (c == '"')
            quoted = true;
        else if (c == '[' || c == '{') {
            if (++depth > 32)
                invalid("release metadata nesting exceeds limit");
        } else if (c == ']' || c == '}')
            --depth;
    }
    try {
        return glint::Json::parse(text);
    } catch (...) {
        invalid("release metadata is malformed JSON");
    }
}
bool host_prefix(std::string_view url, std::string_view prefix, std::string_view path_start) {
    if (url.substr(0, prefix.size()) != prefix)
        return false;
    const auto path = url.substr(prefix.size());
    if (path.empty() || path.front() != '/' || path.find("..") != std::string_view::npos ||
        path.find('%') != std::string_view::npos || path.find('?') != std::string_view::npos ||
        path.find('#') != std::string_view::npos)
        return false;
    return path.substr(0, path_start.size()) == path_start;
}
void ensure_safe_tree(const std::filesystem::path &path) {
    if (path.empty() || path.native().find(L'\0') != std::wstring::npos)
        throw Error("invalid update cache path", FailureKind::Filesystem);
    std::error_code ec;
    auto current = std::filesystem::absolute(path, ec);
    if (ec)
        throw Error("cannot resolve update cache path", FailureKind::Filesystem);
    while (!current.empty()) {
        const auto attrs = GetFileAttributesW(current.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            const auto code = GetLastError();
            if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
                throw Error("cannot inspect update cache path", FailureKind::Filesystem);
        } else if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
            throw Error("update cache contains a reparse point", FailureKind::Filesystem);
        const auto parent = current.parent_path();
        if (parent == current)
            break;
        current = parent;
    }
}
class FileHandle {
  public:
    explicit FileHandle(HANDLE handle = INVALID_HANDLE_VALUE) : handle_(handle) {}
    ~FileHandle() {
        close();
    }
    FileHandle(const FileHandle &) = delete;
    FileHandle &operator=(const FileHandle &) = delete;
    FileHandle(FileHandle &&other) noexcept : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    operator HANDLE() const {
        return handle_;
    }
    void close() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

  private:
    HANDLE handle_;
};
std::vector<FileHandle> lock_directories(const std::filesystem::path &path) {
    std::vector<FileHandle> locks;
    auto current = std::filesystem::absolute(path);
    while (!current.empty()) {
        FileHandle handle(CreateFileW(current.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                      nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle, &info) ||
            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw Error("unsafe update cache directory", FailureKind::Filesystem);
        locks.push_back(std::move(handle));
        const auto parent = current.parent_path();
        if (parent == current)
            break;
        current = parent;
    }
    return locks;
}
FileHandle open_binary_read(const std::filesystem::path &path) {
    ensure_safe_tree(path);
    FileHandle handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
        throw Error("cannot safely open executable", FailureKind::Filesystem);
    return handle;
}
void write_sink(HANDLE file, std::uint64_t &received, std::uint64_t limit, const Cancel &cancel,
                const Progress &progress, std::span<const char> bytes, std::uint64_t total) {
    check_cancel(cancel);
    if (bytes.size() > limit - std::min(limit, received))
        invalid("download exceeds size limit");
    DWORD written = 0;
    if (!WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size())
        throw Error("cannot write downloaded update", FailureKind::Filesystem);
    received += written;
    if (progress)
        progress(received, total);
}
class Handle {
  public:
    explicit Handle(HINTERNET value = nullptr) : value_(value) {}
    ~Handle() {
        if (value_)
            WinHttpCloseHandle(value_);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    operator HINTERNET() const {
        return value_;
    }

  private:
    HINTERNET value_ = nullptr;
};
std::wstring response_header(HINTERNET request, DWORD query, const wchar_t *name = WINHTTP_HEADER_NAME_BY_INDEX) {
    DWORD bytes = 0;
    if (!WinHttpQueryHeaders(request, query, name, nullptr, &bytes, WINHTTP_NO_HEADER_INDEX) &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return {};
    if (bytes > 32768)
        invalid("update response header exceeds limit");
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1);
    if (!WinHttpQueryHeaders(request, query, name, buffer.data(), &bytes, WINHTTP_NO_HEADER_INDEX))
        return {};
    return buffer.data();
}
std::uint64_t decimal(std::wstring_view text) {
    if (text.empty() || text.size() > 18)
        return 0;
    std::uint64_t result = 0;
    for (const auto c : text) {
        if (c < L'0' || c > L'9')
            return 0;
        result = result * 10 + static_cast<unsigned>(c - L'0');
    }
    return result;
}
std::uint64_t retry_after(HINTERNET request) {
    const auto value = response_header(request, WINHTTP_QUERY_RETRY_AFTER);
    const auto seconds = decimal(value);
    if (seconds)
        return std::clamp<std::uint64_t>(seconds, 1, 86400);
    SYSTEMTIME system{};
    FILETIME date{};
    if (!value.empty() && WinHttpTimeToSystemTime(value.c_str(), &system) && SystemTimeToFileTime(&system, &date)) {
        ULARGE_INTEGER then{}, now{};
        then.LowPart = date.dwLowDateTime;
        then.HighPart = date.dwHighDateTime;
        GetSystemTimeAsFileTime(&date);
        now.LowPart = date.dwLowDateTime;
        now.HighPart = date.dwHighDateTime;
        if (then.QuadPart > now.QuadPart)
            return std::clamp<std::uint64_t>((then.QuadPart - now.QuadPart) / 10000000, 1, 86400);
    }
    const auto reset = decimal(response_header(request, WINHTTP_QUERY_CUSTOM, L"X-RateLimit-Reset"));
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    return reset > now ? std::min<std::uint64_t>(reset - now, 86400) : 3600;
}
feed::SourceCooldowns &source_cooldowns() {
    static feed::SourceCooldowns cooldowns;
    return cooldowns;
}
std::string wide_to_utf8(std::wstring_view input) {
    const int size = input.empty() ? 0
                                   : WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                                                         static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(std::max(0, size)), '\0');
    if (size)
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), result.data(),
                            size, nullptr, nullptr);
    return result;
}
void winhttp_once(const feed::Request &request, const feed::Sink &sink, const Cancel &cancel, DWORD proxy_mode,
                  std::chrono::steady_clock::time_point deadline) {
    if (!feed::allowed_url(request.url))
        invalid("update URL is not trusted");
    const auto check = [&] {
        check_cancel(cancel);
        if (std::chrono::steady_clock::now() > deadline)
            throw Error("update request deadline exceeded", FailureKind::Network);
    };
    Handle session(WinHttpOpen(L"Glint-Updater/1", proxy_mode, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session || !WinHttpSetTimeouts(session, 1500, 1500, 1500, 1000))
        throw Error("unable to initialize HTTPS", FailureKind::Network);
    std::string current = request.url;
    for (int redirects = 0; redirects <= 5; ++redirects) {
        check();
        source_cooldowns().check(current);
        const auto separator = current.find('/', 8);
        const auto host = current.substr(8, separator - 8), path = current.substr(separator);
        const std::wstring whost(host.begin(), host.end()), wpath(path.begin(), path.end());
        Handle connect(WinHttpConnect(session, whost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
        if (!connect)
            throw Error("unable to connect to update host", FailureKind::Network);
        Handle handle(WinHttpOpenRequest(connect, L"GET", wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        if (!handle)
            throw Error("unable to open update request", FailureKind::Network);
        // WinHTTP redirects must be disabled: validate each destination before it is requested.
        DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER, retries = 1,
              autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
        if (!WinHttpSetOption(handle, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy, sizeof(redirect_policy)) ||
            !WinHttpSetOption(handle, WINHTTP_OPTION_CONNECT_RETRIES, &retries, sizeof(retries)) ||
            !WinHttpSetOption(handle, WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof(autologon)))
            throw Error("unable to secure HTTPS request", FailureKind::Network);
        const wchar_t *headers =
            request.octet_stream ? L"Accept: application/octet-stream\r\n" : L"Accept: application/json\r\n";
        if (!WinHttpSendRequest(handle, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            check();
            throw Error("update request failed", FailureKind::Network);
        }
        check();
        if (!WinHttpReceiveResponse(handle, nullptr)) {
            check();
            throw Error("update response failed", FailureKind::Network);
        }
        check();
        DWORD status = 0, bytes = sizeof(status);
        if (!WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &bytes, WINHTTP_NO_HEADER_INDEX))
            throw Error("update response has no status", FailureKind::Network);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            if (redirects == 5)
                invalid("update exceeded five redirects");
            auto destination = wide_to_utf8(response_header(handle, WINHTTP_QUERY_LOCATION));
            if (destination.starts_with('/') && !destination.starts_with("//"))
                destination = current.substr(0, separator) + destination;
            if (!feed::allowed_url(destination))
                invalid("update redirected to an untrusted URL");
            current = std::move(destination);
            continue;
        }
        if (status == 429 ||
            (status == 403 && (response_header(handle, WINHTTP_QUERY_CUSTOM, L"X-RateLimit-Remaining") == L"0" ||
                               !response_header(handle, WINHTTP_QUERY_RETRY_AFTER).empty()))) {
            const auto delay = retry_after(handle);
            source_cooldowns().limit(current, delay);
            throw Error("update service rate limit", FailureKind::RateLimit, delay, origin(current));
        }
        if (status != 200)
            throw Error("update service returned HTTP " + std::to_string(status), FailureKind::Network);
        const auto length_text = response_header(handle, WINHTTP_QUERY_CONTENT_LENGTH);
        const auto content_length = decimal(length_text);
        if ((!length_text.empty() && (!content_length || content_length > request.maximum_bytes)))
            invalid("update response exceeds size limit");
        std::array<char, 64 * 1024> chunk{};
        std::uint64_t received = 0;
        for (;;) {
            check();
            DWORD read = 0;
            if (!WinHttpReadData(handle, chunk.data(), static_cast<DWORD>(chunk.size()), &read)) {
                check();
                throw Error("update response read failed", FailureKind::Network);
            }
            if (!read)
                break;
            if (read > request.maximum_bytes - std::min(request.maximum_bytes, received))
                invalid("update response exceeds size limit");
            sink(std::span<const char>(chunk.data(), read));
            received += read;
        }
        if (content_length && received != content_length)
            throw Error("update response was truncated", FailureKind::Network);
        return;
    }
}
feed::Fetch default_fetch() {
    return [](const feed::Request &request, const feed::Sink &sink, const Cancel &cancel, const Progress &) {
        check_cancel(cancel);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(request.maximum_bytes <= feed::kMetadataLimit ? 30 : 180);
        bool delivered = false;
        const auto output = [&](std::span<const char> bytes) {
            delivered = true;
            sink(bytes);
        };
        try {
            winhttp_once(request, output, cancel, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, deadline);
        } catch (const Error &error) {
            // A bounded direct attempt helps when Windows proxy discovery is unavailable.
            // After streaming begins, the candidate loop must discard .part before retrying.
            if (error.kind != FailureKind::Network || delivered)
                throw;
            check_cancel(cancel);
            winhttp_once(request, output, cancel, WINHTTP_ACCESS_TYPE_NO_PROXY, deadline);
        }
    };
}
std::string collect(const feed::Fetch &fetch, const feed::Request &request, const Cancel &cancel) {
    std::string result;
    fetch(request,
          [&](std::span<const char> bytes) {
              if (bytes.size() >
                  request.maximum_bytes - std::min(request.maximum_bytes, static_cast<std::uint64_t>(result.size())))
                  invalid("response exceeds size limit");
              result.append(bytes.data(), bytes.size());
          },
          cancel, {});
    return result;
}
void assert_release_urls(Release &release) {
    parse_version(release.version);
    const auto expected = pinned_url(release.version, kBinaryName);
    if (release.binary_url.empty())
        release.binary_url = expected;
    if (release.binary_url != expected ||
        (!release.binary_api_url.empty() &&
         (!release.binary_api_url.starts_with(std::string(kApiRoot) + "/releases/assets/") ||
          !feed::allowed_url(release.binary_api_url))))
        invalid("release has an untrusted binary URL");
    if (!is_hex_hash(release.sha256))
        invalid("release has no trusted SHA256");
    release.sha256 = lowercase(release.sha256);
    if (release.size > feed::kBinaryLimit)
        invalid("release binary exceeds 128 MiB");
}
void check_pe64(const std::filesystem::path &path) {
    auto file = open_binary_read(path);
    LARGE_INTEGER size{};
    DWORD bytes = 0;
    IMAGE_DOS_HEADER dos{};
    if (!GetFileSizeEx(file, &size) || !ReadFile(file, &dos, sizeof(dos), &bytes, nullptr) || bytes != sizeof(dos) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE)
        invalid("download is not a Windows executable");
    if (dos.e_lfanew < static_cast<LONG>(sizeof(dos)) ||
        static_cast<std::uint64_t>(dos.e_lfanew) + sizeof(IMAGE_NT_HEADERS64) >
            static_cast<std::uint64_t>(size.QuadPart))
        invalid("download has an invalid PE header offset");
    LARGE_INTEGER offset{};
    offset.QuadPart = dos.e_lfanew;
    IMAGE_NT_HEADERS64 nt{};
    if (!SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) || !ReadFile(file, &nt, sizeof(nt), &bytes, nullptr) ||
        bytes != sizeof(nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
        !(nt.FileHeader.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) ||
        (nt.FileHeader.Characteristics & IMAGE_FILE_DLL) || nt.FileHeader.NumberOfSections == 0 ||
        nt.FileHeader.NumberOfSections > 96)
        invalid("download is not an x64 PE application");
}
void check_native_version(const std::filesystem::path &path, std::string_view expected) {
    const auto parsed = parse_version(expected);
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size || size > feed::kMetadataLimit)
        invalid("executable has no bounded native version information");
    std::vector<std::byte> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
        invalid("cannot read executable version information");
    VS_FIXEDFILEINFO *info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&info), &length) || !info ||
        length < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != VS_FFI_SIGNATURE || info->dwFileType != VFT_APP)
        invalid("executable has invalid version information");
    const auto major = HIWORD(info->dwProductVersionMS), minor = LOWORD(info->dwProductVersionMS),
               patch = HIWORD(info->dwProductVersionLS), revision = LOWORD(info->dwProductVersionLS);
    const auto fmajor = HIWORD(info->dwFileVersionMS), fminor = LOWORD(info->dwFileVersionMS),
               fpatch = HIWORD(info->dwFileVersionLS), frevision = LOWORD(info->dwFileVersionLS);
    if (major != parsed.values[0] || minor != parsed.values[1] || patch != parsed.values[2] || revision != 0 ||
        fmajor != major || fminor != minor || fpatch != patch || frevision != 0)
        invalid("native product/file version does not match release");
    struct Translation {
        WORD language;
        WORD codepage;
    };
    Translation *translations = nullptr;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void **>(&translations),
                        &length) ||
        length < sizeof(Translation))
        invalid("executable has no version strings");
    const auto translation_count = length / sizeof(Translation);
    const std::wstring version(expected.begin(), expected.end());
    for (unsigned i = 0; i < translation_count; ++i) {
        for (const auto name : {L"ProductVersion", L"FileVersion"}) {
            wchar_t query[96]{};
            swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\%s", translations[i].language, translations[i].codepage,
                       name);
            wchar_t *value = nullptr;
            UINT chars = 0;
            if (!VerQueryValueW(data.data(), query, reinterpret_cast<void **>(&value), &chars) || !value || chars < 2 ||
                chars > 64)
                invalid("executable has invalid version strings");
            const auto wanted = std::wstring_view(name) == L"FileVersion" ? version + L".0" : version;
            if (std::wstring_view(value, chars - 1) != wanted)
                invalid("executable version strings do not match release");
        }
    }
}
} // namespace

std::string file_sha256(const std::filesystem::path &path) {
    auto file = open_binary_read(path);
    struct Hash {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        std::vector<unsigned char> object;
        ~Hash() {
            if (hash)
                BCryptDestroyHash(hash);
            if (algorithm)
                BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    } context;
    if (BCryptOpenAlgorithmProvider(&context.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw Error("cannot initialize SHA256", FailureKind::Filesystem);
    DWORD object_size = 0, result_size = 0;
    if (BCryptGetProperty(context.algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
                          sizeof(object_size), &result_size, 0) < 0)
        throw Error("cannot initialize SHA256", FailureKind::Filesystem);
    context.object.resize(object_size);
    if (BCryptCreateHash(context.algorithm, &context.hash, context.object.data(), object_size, nullptr, 0, 0) < 0)
        throw Error("cannot initialize SHA256", FailureKind::Filesystem);
    std::array<unsigned char, 64 * 1024> buffer{};
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr))
            throw Error("cannot read executable", FailureKind::Filesystem);
        if (!count)
            break;
        if (BCryptHashData(context.hash, buffer.data(), count, 0) < 0)
            throw Error("cannot hash executable", FailureKind::Filesystem);
    }
    std::array<unsigned char, 32> digest{};
    if (BCryptFinishHash(context.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
        throw Error("cannot hash executable", FailureKind::Filesystem);
    std::string result;
    result.reserve(64);
    for (const auto value : digest) {
        constexpr char chars[] = "0123456789abcdef";
        result.push_back(chars[value >> 4]);
        result.push_back(chars[value & 15]);
    }
    return result;
}
void verify_binary(const std::filesystem::path &path, const Release &release) {
    auto directories = lock_directories(path.parent_path());
    auto locked = open_binary_read(path);
    LARGE_INTEGER native_size{};
    if (!GetFileSizeEx(locked, &native_size))
        throw Error("cannot read executable size", FailureKind::Filesystem);
    const auto size = static_cast<std::uint64_t>(native_size.QuadPart);
    if (size == 0 || size > feed::kBinaryLimit)
        invalid("downloaded executable size is invalid");
    if (release.size && size != release.size)
        invalid("downloaded executable size does not match release");
    if (!is_hex_hash(release.sha256))
        invalid("release has no trusted SHA256");
    if (file_sha256(path) != lowercase(release.sha256))
        invalid("downloaded executable SHA256 mismatch");
    check_pe64(path);
    check_native_version(path, release.version);
}

namespace feed {
SourceCooldowns::SourceCooldowns(std::function<std::int64_t()> now) : now_(std::move(now)) {
    if (!now_)
        now_ = [] {
            return std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now().time_since_epoch()).count();
        };
}
void SourceCooldowns::check(std::string_view url) {
    std::lock_guard lock(mutex_);
    const auto found = until_.find(origin(url));
    if (found == until_.end())
        return;
    const auto remaining = found->second - now_();
    if (remaining > 0)
        throw Error("Update source is temporarily rate-limited: " + found->first,
                    FailureKind::RateLimit, static_cast<std::uint64_t>(remaining), found->first);
    until_.erase(found);
}
void SourceCooldowns::limit(std::string_view url, std::uint64_t seconds) {
    const auto delay = static_cast<std::int64_t>(std::min<std::uint64_t>(seconds ? seconds : 3600, 86400));
    std::lock_guard lock(mutex_);
    auto &until = until_[origin(url)];
    until = std::max(until, now_() + delay);
}
bool newer_version(std::string_view candidate, std::string_view current) {
    return version_newer(candidate, current);
}
bool allowed_feed_path(std::string_view path, bool binary) {
    if (path == kManifestName)
        return true;
    if (!path.starts_with('v'))
        return false;
    const auto slash = path.find('/');
    if (slash == std::string_view::npos)
        return false;
    try {
        parse_version(path.substr(1, slash - 1));
    } catch (...) {
        return false;
    }
    const auto name = path.substr(slash + 1);
    return name == kManifestName || (binary && name == kBinaryName);
}
bool allowed_url(std::string_view url) {
    if (url.size() > 16384 ||
        std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 32 || c >= 127 || c == '\\'; }))
        return false;
    if (url.substr(0, kAssetRoot.size()) == kAssetRoot) {
        const auto path = url.substr(kAssetRoot.size());
        return !path.empty() && path.front() != '/' && path.find('#') == std::string_view::npos;
    }
    if (url.find('?') != std::string_view::npos ||
        url.find('#') != std::string_view::npos || url.find('%') != std::string_view::npos)
        return false;
    for (const auto root : kManifestRoots) {
        if (url.starts_with(root))
            return allowed_feed_path(url.substr(root.size()), root == kPagesRoot);
    }
    for (const auto root : kProxyRoots) {
        if (url.starts_with(root)) {
            const auto destination = url.substr(root.size());
            return destination.starts_with(std::string(kGithubRoot) + "/releases/") && allowed_url(destination);
        }
    }
    if (url.find('@') != std::string_view::npos)
        return false;
    if (host_prefix(url, kGithubRoot, "/releases/download/v")) {
        const auto begin = kGithubRoot.size() + std::string_view("/releases/download/v").size();
        const auto slash = url.find('/', begin);
        if (slash == std::string_view::npos)
            return false;
        try {
            parse_version(url.substr(begin, slash - begin));
        } catch (...) {
            return false;
        }
        return url.substr(slash + 1) == kBinaryName || url.substr(slash + 1) == "release-manifest.json";
    }
    if (url == std::string(kGithubRoot) + "/releases/latest/download/release-manifest.json")
        return true;
    if (url == std::string(kApiRoot) + "/releases/latest")
        return true;
    const auto tags = std::string(kApiRoot) + "/releases/tags/v";
    if (url.starts_with(tags)) {
        try {
            parse_version(url.substr(tags.size()));
            return true;
        } catch (...) {
            return false;
        }
    }
    if (host_prefix(url, kApiRoot, "/releases/assets/")) {
        const auto id = url.substr(kApiRoot.size() + std::string_view("/releases/assets/").size());
        return !id.empty() && std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    }
    return false;
}
Release parse_latest(std::string_view text) {
    const auto json = parse_json(text);
    const auto tag = string_field(json, "tag_name");
    if (tag.size() < 2 || tag.front() != 'v')
        invalid("release tag is not numeric");
    const auto version = tag.substr(1);
    parse_version(version);
    if (bool_field(json, "draft") || bool_field(json, "prerelease"))
        invalid("draft or prerelease release");
    Release release;
    release.version = version;
    release.page_url = std::string(kGithubRoot) + "/releases/tag/v" + version;
    const auto *assets = field(json, "assets");
    if (!assets || !assets->is_array())
        invalid("release has no assets");
    for (const auto &item : assets->as_array()) {
        const auto name = string_field(item, "name");
        if (name != kBinaryName && name != "release-manifest.json")
            continue;
        const auto url = string_field(item, "browser_download_url");
        const auto api = string_field(item, "url", false);
        const auto size = integer_field(item, "size");
        if (size <= 0 || static_cast<std::uint64_t>(size) > (name == kBinaryName ? kBinaryLimit : kMetadataLimit))
            invalid("release asset size is invalid");
        if (url != pinned_url(version, name))
            invalid("release asset URL is not pinned to its version");
        if (!api.empty() && (!api.starts_with(std::string(kApiRoot) + "/releases/assets/") || !allowed_url(api)))
            invalid("release API asset URL is invalid");
        if (name == kBinaryName) {
            if (!release.binary_url.empty())
                invalid("release has duplicate binary assets");
            release.binary_url = url;
            release.binary_api_url = api;
            release.size = static_cast<std::uint64_t>(size);
        } else {
            if (!release.manifest_url.empty())
                invalid("release has duplicate manifest assets");
            release.manifest_url = url;
            release.manifest_api_url = api;
        }
    }
    if (release.binary_url.empty() || release.manifest_url.empty())
        invalid("release is missing binary or manifest asset");
    return release;
}
Release parse_manifest(std::string_view text, Release release) {
    const auto json = parse_json(text);
    if (string_field(json, "Product") != "Glint" || string_field(json, "Binary") != kBinaryName)
        invalid("manifest product or binary mismatch");
    const auto version = string_field(json, "Version");
    parse_version(version);
    if (string_field(json, "FileVersion") != version + ".0")
        invalid("manifest file version mismatch");
    if (!release.version.empty() && !version_equal(release.version, version))
        invalid("manifest version mismatch");
    release.version = version;
    release.page_url = std::string(kGithubRoot) + "/releases/tag/v" + version;
    if (release.manifest_url.empty())
        release.manifest_url = pinned_url(version, "release-manifest.json");
    const auto *files = field(json, "Files");
    if (!files || !files->is_array())
        invalid("manifest has no file list");
    release.sha256.clear();
    for (const auto &item : files->as_array()) {
        if (string_field(item, "Path") != kBinaryName)
            continue;
        if (!release.sha256.empty())
            invalid("manifest has duplicate binary entries");
        release.sha256 = lowercase(string_field(item, "SHA256"));
        if (!is_hex_hash(release.sha256))
            invalid("manifest binary SHA256 is invalid");
    }
    assert_release_urls(release);
    return release;
}
std::optional<Release> find_release(std::string_view current_version, const Fetch &fetch, const Cancel &cancel) {
    parse_version(current_version);
    check_cancel(cancel);
    SourceFailures failures;
    const auto read_manifest = [&](const Request &request, const Release &candidate) -> std::optional<Release> {
        check_cancel(cancel);
        try {
            failures.check(request.url);
            return parse_manifest(collect(fetch, request, cancel), candidate);
        } catch (const Error &error) {
            failures.record(request.url, error);
            return std::nullopt;
        }
    };
    const auto selected = [&](Release release) -> std::optional<Release> {
        return newer_version(release.version, current_version) ? std::optional<Release>(std::move(release))
                                                               : std::nullopt;
    };
    const auto latest = manifest_sources(std::nullopt);
    const auto official_count = 1 + kManifestRoots.size();
    // Ordinary checks need only the published manifest, never the rate-limited API.
    for (std::size_t i = 0; i < official_count; ++i) {
        if (auto release = read_manifest(latest[i], {}))
            return selected(std::move(*release));
    }
    std::optional<Release> candidate;
    const auto api = std::string(kApiRoot) + "/releases/latest";
    check_cancel(cancel);
    try {
        failures.check(api);
        candidate = parse_latest(collect(fetch, {api}, cancel));
    } catch (const Error &api_error) {
        failures.record(api, api_error);
    }
    if (candidate) {
        for (const auto &request : manifest_sources(candidate)) {
            if (auto release = read_manifest(request, *candidate))
                return selected(std::move(*release));
        }
    } else {
        for (std::size_t i = official_count; i < latest.size(); ++i) {
            if (auto release = read_manifest(latest[i], {}))
                return selected(std::move(*release));
        }
    }
    failures.raise();
}
Package download_release(Release release, const std::filesystem::path &cache_root, const Fetch &fetch,
                         const Cancel &cancel, const Progress &progress) {
    assert_release_urls(release);
    check_cancel(cancel);
    ensure_safe_tree(cache_root);
    std::error_code ec;
    std::filesystem::create_directories(cache_root, ec);
    if (ec)
        throw Error("cannot create update cache", FailureKind::Filesystem);
    auto root_locks = lock_directories(cache_root);
    const auto directory = cache_root / ("v" + release.version + "-" + release.sha256.substr(0, 16));
    ensure_safe_tree(directory);
    std::filesystem::create_directory(directory, ec);
    if (ec)
        throw Error("cannot create update staging", FailureKind::Filesystem);
    auto staging_locks = lock_directories(directory);
    const auto executable = directory / "Glint.exe";
    ensure_safe_tree(executable);
    const auto normalized_package = [&] {
        release.size = std::filesystem::file_size(executable, ec);
        if (ec)
            throw Error("cannot read verified update size", FailureKind::Filesystem);
        return Package{release, executable, directory};
    };
    if (std::filesystem::is_regular_file(executable, ec) && !ec) {
        try {
            verify_binary(executable, release);
            return normalized_package();
        } catch (const Error &error) {
            if (error.kind != FailureKind::Invalid)
                throw;
        }
    }
    // Keep verified cache entries stable, but never share a partial file between
    // overlapping attempts or different running copies of Glint.
    std::array<unsigned char, 16> nonce{};
    if (BCryptGenRandom(nullptr, nonce.data(), static_cast<ULONG>(nonce.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw Error("cannot create download attempt identity", FailureKind::Filesystem);
    std::string attempt_name = ".attempt-";
    constexpr char hex[] = "0123456789abcdef";
    for (auto byte : nonce) { attempt_name += hex[byte >> 4]; attempt_name += hex[byte & 15]; }
    const auto attempt_directory = directory / attempt_name;
    if (!CreateDirectoryW(attempt_directory.c_str(), nullptr))
        throw Error("cannot create unique download attempt", FailureKind::Filesystem);
    struct AttemptCleanup {
        std::filesystem::path directory;
        ~AttemptCleanup() {
            std::error_code ignored;
            std::filesystem::remove(directory / "Glint.exe.part", ignored);
            std::filesystem::remove(directory, ignored);
        }
    } cleanup{attempt_directory};
    auto attempt_locks = lock_directories(attempt_directory);
    const auto part = attempt_directory / "Glint.exe.part";
    SourceFailures failures;
    auto sources = binary_sources(release);
    const bool needs_api_asset = release.binary_api_url.empty();
    for (std::size_t index = 0; index < sources.size(); ++index) {
        check_cancel(cancel);
        if (index == 1 && needs_api_asset) {
            // Manifest-first discovery normally needs no API. Look up its asset ID
            // only if the direct EXE failed, using the chosen tag rather than latest.
            const auto tagged = std::string(kApiRoot) + "/releases/tags/v" + release.version;
            try {
                failures.check(tagged);
                const auto candidate = parse_latest(collect(fetch, {tagged}, cancel));
                if (candidate.version != release.version || (release.size && candidate.size != release.size))
                    invalid("backup API asset does not match the selected release");
                if (!candidate.binary_api_url.empty()) {
                    release.binary_api_url = candidate.binary_api_url;
                    release.size = candidate.size;
                    sources.insert(sources.begin() + index, {release.binary_api_url, true, kBinaryLimit});
                }
            } catch (const Error &error) {
                failures.record(tagged, error);
            }
        }
        auto request = sources[index];
        try {
            failures.check(request.url);
        } catch (const Error &error) {
            failures.record(request.url, error);
            continue;
        }
        ensure_safe_tree(part);
        std::filesystem::remove(part, ec);
        if (ec)
            throw Error("cannot remove incomplete update", FailureKind::Filesystem);
        FileHandle file(CreateFileW(part.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (file == INVALID_HANDLE_VALUE)
            throw Error("cannot create update staging file", FailureKind::Filesystem);
        const auto clean_part = [&] {
            file.close();
            std::error_code ignored;
            std::filesystem::remove(part, ignored);
        };
        std::uint64_t received = 0;
        try {
            const auto limit = release.size ? release.size : kBinaryLimit;
            request.maximum_bytes = limit;
            fetch(request,
                  [&](std::span<const char> bytes) {
                      write_sink(file, received, limit, cancel, progress, bytes, release.size);
                  },
                  cancel, {});
            check_cancel(cancel);
            if (!FlushFileBuffers(file))
                throw Error("cannot flush downloaded update", FailureKind::Filesystem);
            file.close();
            if (release.size && received != release.size)
                throw Error("downloaded executable was truncated", FailureKind::Network);
            verify_binary(part, release);
            check_cancel(cancel);
            ensure_safe_tree(executable);
            if (!MoveFileExW(part.c_str(), executable.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw Error("cannot commit downloaded update", FailureKind::Filesystem);
            verify_binary(executable, release);
            return normalized_package();
        } catch (const Error &error) {
            clean_part();
            failures.record(request.url, error);
        } catch (...) {
            clean_part();
            throw;
        }
    }
    failures.raise();
}
} // namespace feed
std::optional<Release> find_release(std::string_view current_version, const Cancel &cancel) {
    return feed::find_release(current_version, default_fetch(), cancel);
}
Package download_release(Release release, const std::filesystem::path &cache_root, const Cancel &cancel,
                         const Progress &progress) {
    return feed::download_release(std::move(release), cache_root, default_fetch(), cancel, progress);
}
} // namespace glint::update
