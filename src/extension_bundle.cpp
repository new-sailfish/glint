#include "language.hpp"
#include "extension_bundle.hpp"
#include "glint/core.hpp"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace glint::extension_bundle {
namespace {

enum : int {
    ResourceManifest = 101,
    ResourceBackground = 102,
    ResourceBackgroundCore = 103,
    ResourceOptionsHtml = 104,
    ResourceOptionsJs = 105,
    ResourceIcon16 = 106,
    ResourceIcon32 = 107,
    ResourceIcon48 = 108,
    ResourceIcon128 = 109,
    ResourceLocaleEnglish = 110,
    ResourceLocaleChinese = 111,
    ResourceLocaleChineseTaiwan = 112,
};

struct ResourceSpec {
    int id;
    const wchar_t* filename;
};

constexpr std::array<ResourceSpec, 12> kResources = {{
    {ResourceManifest, L"manifest.json"},
    {ResourceBackground, L"background.js"},
    {ResourceBackgroundCore, L"background-core.js"},
    {ResourceOptionsHtml, L"options.html"},
    {ResourceOptionsJs, L"options.js"},
    {ResourceIcon16, L"icon-16.png"},
    {ResourceIcon32, L"icon-32.png"},
    {ResourceIcon48, L"icon-48.png"},
    {ResourceIcon128, L"icon-128.png"},
    {ResourceLocaleEnglish, L"_locales/en/messages.json"},
    {ResourceLocaleChinese, L"_locales/zh_CN/messages.json"},
    {ResourceLocaleChineseTaiwan, L"_locales/zh_TW/messages.json"},
}};

using BundleContents = std::array<std::vector<std::byte>, kResources.size()>;

std::wstring windows_error(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(flags, nullptr, code, i18n::message_language(),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text = length ? std::wstring(buffer, length)
                               : i18n::text(L"Windows error ", L"Windows 错误 ") + std::to_wstring(code);
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
        text.pop_back();
    }
    return text;
}

void set_error(std::wstring& error, std::wstring_view operation, DWORD code) {
    error.assign(operation);
    error.append(L": ");
    error.append(windows_error(code));
}

// Deny write/delete sharing during preflight, including in-place conversion of
// an empty directory to a junction. Before atomic renames, keep each destination
// nonempty with an undeletable temporary file and relax write sharing: Windows
// needs it to rename children. Pinned child directories protect every ancestor.
class PathGuard {
public:
    ~PathGuard() {
        for (const auto& item : guards_) CloseHandle(item.handle);
        for (const auto& item : directories_) CloseHandle(item.handle);
    }

    bool check(const std::filesystem::path& path, bool is_directory, std::wstring& error,
               DWORD* attributes = nullptr, bool allow_other_leaf_type = false) {
        if (attributes) *attributes = INVALID_FILE_ATTRIBUTES;
        if (!path.is_absolute() || path.native().find(L'\0') != std::wstring::npos) {
            error = i18n::text(L"The extension path must be an absolute path without null characters.",
                L"扩展路径必须是完整的绝对路径，且不能包含空字符。");
            return false;
        }
        for (const auto& part : path.relative_path()) {
            if (part == L"." || part == L".." || part.native().find(L':') != std::wstring::npos) {
                error = i18n::text(L"The extension path contains an unsafe component.", L"扩展路径包含不安全的部分。");
                return false;
            }
        }
        auto current = path.root_path();
        DWORD current_attributes = INVALID_FILE_ATTRIBUTES;
        if (!inspect(current, true, current_attributes, error)) return false;
        if (current_attributes == INVALID_FILE_ATTRIBUTES) return true;
        const auto relative = path.relative_path();
        for (auto part = relative.begin(); part != relative.end(); ++part) {
            if (part->empty()) continue;
            current /= *part;
            auto next = part;
            ++next;
            if (!inspect(current, next != relative.end() || is_directory, current_attributes, error,
                next == relative.end() && allow_other_leaf_type)) return false;
            if (current_attributes == INVALID_FILE_ATTRIBUTES) return true;
        }
        if (attributes) *attributes = current_attributes;
        return true;
    }

    bool create_directory(const std::filesystem::path& path, std::wstring& error) {
        auto current = path.root_path();
        for (const auto& part : path.relative_path()) {
            if (part.empty()) continue;
            current /= part;
            if (!CreateDirectoryW(current.c_str(), nullptr)) {
                const DWORD code = GetLastError();
                if (code != ERROR_ALREADY_EXISTS) {
                    set_error(error, i18n::text(L"create extension folder", L"创建扩展文件夹"), code);
                    return false;
                }
            }
            DWORD attributes = INVALID_FILE_ATTRIBUTES;
            if (!inspect(current, true, attributes, error)) return false;
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                set_error(error, i18n::text(L"open extension folder", L"打开扩展文件夹"), ERROR_PATH_NOT_FOUND);
                return false;
            }
        }
        return true;
    }

    bool protect_contents(const std::filesystem::path& directory, std::wstring& error) {
        for (const auto& item : guards_) if (item.path == directory) return true;
        const auto base = L".glint-refresh-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-";
        for (unsigned int attempt = 0; attempt != 100; ++attempt) {
            const auto path = directory / (base + std::to_wstring(attempt) + L".tmp");
            const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (handle != INVALID_HANDLE_VALUE) {
                guards_.push_back({directory, handle});
                return true;
            }
            const DWORD code = GetLastError();
            if (code != ERROR_FILE_EXISTS) {
                set_error(error, i18n::text(L"protect extension folder", L"保护扩展文件夹"), code);
                return false;
            }
        }
        set_error(error, i18n::text(L"protect extension folder", L"保护扩展文件夹"), ERROR_FILE_EXISTS);
        return false;
    }

    bool allow_child_renames(std::wstring& error) {
        for (auto& item : directories_) {
            const HANDLE relaxed = CreateFileW(item.path.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (relaxed == INVALID_HANDLE_VALUE) {
                set_error(error, i18n::text(L"prepare extension folder update", L"准备更新扩展文件夹"), GetLastError());
                return false;
            }
            CloseHandle(item.handle);
            item.handle = relaxed;
        }
        return true;
    }

private:
    struct HeldPath { std::filesystem::path path; HANDLE handle; };
    std::vector<HeldPath> directories_;
    std::vector<HeldPath> guards_;

    bool inspect(const std::filesystem::path& path, bool is_directory, DWORD& attributes,
                 std::wstring& error, bool allow_other_type = false) {
        attributes = INVALID_FILE_ATTRIBUTES;
        const HANDLE handle = CreateFileW(path.c_str(), is_directory ? GENERIC_READ : FILE_READ_ATTRIBUTES,
            is_directory ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return true;
            set_error(error, std::wstring(i18n::text(L"inspect extension path ", L"检查扩展路径 ")) + path.wstring(), code);
            return false;
        }
        BY_HANDLE_FILE_INFORMATION information{};
        const bool read = GetFileInformationByHandle(handle, &information) != FALSE;
        const DWORD code = GetLastError();
        if (!read || (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            (!allow_other_type && ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != is_directory)) {
            CloseHandle(handle);
            if (!read) set_error(error, i18n::text(L"inspect extension path", L"检查扩展路径"), code);
            else error = i18n::text(L"The extension path contains a reparse point or an unexpected file type.",
                L"扩展路径包含重解析点或不符合预期的文件类型。");
            return false;
        }
        attributes = information.dwFileAttributes;
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) directories_.push_back({path, handle});
        else CloseHandle(handle);
        return true;
    }
};

bool preflight_bundle(const std::filesystem::path& directory, PathGuard& guard,
                      bool& has_runtime, bool& has_original_structure, std::wstring& error,
                      bool allow_unknown_structure = false) {
    has_runtime = false;
    has_original_structure = true;
    for (const auto& spec : kResources) {
        DWORD attributes = INVALID_FILE_ATTRIBUTES;
        if (!guard.check(directory / spec.filename, false, error, &attributes, allow_unknown_structure)) return false;
        const bool exists = attributes != INVALID_FILE_ATTRIBUTES;
        const bool regular = exists && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
        has_runtime = has_runtime || regular;
        // The first fixed-key release predates localization. Its nine root
        // files identify the supported layout; locale files can be added later.
        if ((spec.id <= ResourceIcon128 && !regular) || (exists && !regular)) has_original_structure = false;
    }
    return true;
}

bool local_app_data(std::filesystem::path& root, std::wstring& error) {
    const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required != 0) {
        std::wstring value(required, L'\0');
        const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), required);
        if (copied != 0 && copied < required) {
            value.resize(copied);
            root = std::filesystem::path(value);
            return true;
        }
    }

    PWSTR known_folder = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &known_folder);
    if (SUCCEEDED(result) && known_folder) {
        root = std::filesystem::path(known_folder);
        CoTaskMemFree(known_folder);
        return true;
    }
    if (known_folder) CoTaskMemFree(known_folder);
    set_error(error, i18n::text(L"find LocalAppData", L"查找本地应用数据目录"), HRESULT_CODE(result));
    return false;
}

bool load_resource(HMODULE module, const ResourceSpec& spec, std::vector<std::byte>& bytes,
                   std::wstring& error) {
    // RT_RCDATA is declared as an ANSI resource-name macro in some Windows
    // SDK headers.  Pass the numeric resource type explicitly so the W
    // overload is selected consistently with UNICODE builds.
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(spec.id),
                                         MAKEINTRESOURCEW(10));
    if (!resource) {
        set_error(error, std::wstring(i18n::text(L"find embedded ", L"查找内嵌文件 ")) + spec.filename, GetLastError());
        return false;
    }
    const HGLOBAL loaded = LoadResource(module, resource);
    if (!loaded) {
        set_error(error, std::wstring(i18n::text(L"load embedded ", L"加载内嵌文件 ")) + spec.filename, GetLastError());
        return false;
    }
    const DWORD size = SizeofResource(module, resource);
    const auto* data = static_cast<const std::byte*>(LockResource(loaded));
    if (size != 0 && !data) {
        set_error(error, std::wstring(i18n::text(L"read embedded ", L"读取内嵌文件 ")) + spec.filename, ERROR_INVALID_DATA);
        return false;
    }
    if (size == 0) {
        bytes.clear();
    } else {
        bytes.assign(data, data + size);
    }
    return true;
}

bool matches_file(const std::filesystem::path& path, const std::vector<std::byte>& expected,
                  bool& matches, std::wstring& error) {
    matches = false;
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return true;
        set_error(error, i18n::text(L"read existing extension file", L"读取现有扩展文件"), code);
        return false;
    }

    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(handle, &size) != FALSE;
    if (!ok) {
        set_error(error, i18n::text(L"read existing extension file size", L"读取现有扩展文件大小"), GetLastError());
    } else if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) != expected.size()) {
        ok = true;
    } else {
        std::vector<std::byte> actual(expected.size());
        std::size_t offset = 0;
        while (offset < actual.size()) {
            const DWORD chunk = static_cast<DWORD>((std::min)(actual.size() - offset,
                static_cast<std::size_t>(1u << 20)));
            DWORD read = 0;
            if (!ReadFile(handle, actual.data() + offset, chunk, &read, nullptr) || read == 0) {
                set_error(error, i18n::text(L"read existing extension file", L"读取现有扩展文件"), GetLastError());
                ok = false;
                break;
            }
            offset += read;
        }
        if (ok) matches = actual == expected;
    }
    CloseHandle(handle);
    return ok;
}

bool write_file_atomically(const std::filesystem::path& target, const std::vector<std::byte>& bytes,
                           int resource_id, std::wstring& error) {
    const std::wstring base = target.wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(resource_id);
    std::filesystem::path temporary;
    HANDLE handle = INVALID_HANDLE_VALUE;
    for (unsigned int attempt = 0; attempt != 100; ++attempt) {
        temporary = base + L"-" + std::to_wstring(attempt);
        handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS) {
            set_error(error, i18n::text(L"create temporary extension file", L"创建扩展临时文件"), GetLastError());
            return false;
        }
    }
    if (handle == INVALID_HANDLE_VALUE) {
        set_error(error, i18n::text(L"create temporary extension file", L"创建扩展临时文件"), ERROR_FILE_EXISTS);
        return false;
    }

    bool ok = true;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>((std::min)(bytes.size() - offset,
            static_cast<std::size_t>(1u << 20)));
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) || written == 0) {
            set_error(error, i18n::text(L"write temporary extension file", L"写入扩展临时文件"), GetLastError());
            ok = false;
            break;
        }
        offset += written;
    }
    if (ok && !FlushFileBuffers(handle)) {
        set_error(error, i18n::text(L"flush temporary extension file", L"保存扩展临时文件"), GetLastError());
        ok = false;
    }
    if (!CloseHandle(handle)) {
        if (ok) set_error(error, i18n::text(L"close temporary extension file", L"关闭扩展临时文件"), GetLastError());
        ok = false;
    }
    if (!ok) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        set_error(error, i18n::text(L"replace extension file", L"替换扩展文件"), GetLastError());
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

bool load_bundle(BundleContents& contents, std::wstring& error) {
    const HMODULE module = GetModuleHandleW(nullptr);
    for (std::size_t i = 0; i != kResources.size(); ++i) {
        if (!load_resource(module, kResources[i], contents[i], error)) return false;
    }
    return true;
}

bool refresh_bundle(const std::filesystem::path& directory, const BundleContents& contents,
                    PathGuard& guard, bool had_runtime, bool* changed_existing, std::wstring& error) {
    for (const auto& spec : kResources) {
        const auto parent = (directory / spec.filename).parent_path();
        if (!guard.create_directory(parent, error) || !guard.protect_contents(parent, error)) return false;
    }
    if (!guard.allow_child_renames(error)) return false;
    for (std::size_t i = 0; i != kResources.size(); ++i) {
        const auto target = directory / kResources[i].filename;
        bool matches = false;
        if (!matches_file(target, contents[i], matches, error)) return false;
        if (!matches) {
            if (!write_file_atomically(target, contents[i], kResources[i].id, error)) return false;
            if (had_runtime && changed_existing) *changed_existing = true;
        }
    }
    return true;
}

bool read_manifest(const std::filesystem::path& path, std::string& contents, std::wstring& error) {
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        set_error(error, i18n::text(L"read adjacent extension manifest", L"读取相邻扩展清单"), GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(handle, &size) != FALSE;
    if (!ok) {
        set_error(error, i18n::text(L"read adjacent extension manifest size", L"读取相邻扩展清单大小"), GetLastError());
    } else if (size.QuadPart > 0 && size.QuadPart <= 64 * 1024) {
        contents.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(handle, contents.data(), static_cast<DWORD>(contents.size()), &read, nullptr) != FALSE;
        if (!ok || read != contents.size()) {
            set_error(error, i18n::text(L"read adjacent extension manifest", L"读取相邻扩展清单"),
                ok ? ERROR_HANDLE_EOF : GetLastError());
            ok = false;
        }
    }
    CloseHandle(handle);
    return ok;
}

bool has_bounded_json_depth(std::string_view text) {
    // The shared JSON parser is recursive. Bound unknown manifests before
    // parsing, while allowing braces that occur inside JSON strings.
    unsigned int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (const char ch : text) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') quoted = true;
        else if (ch == '{' || ch == '[') {
            if (++depth > 32) return false;
        } else if (ch == '}' || ch == ']') {
            if (depth == 0) return false;
            --depth;
        }
    }
    return depth == 0 && !quoted;
}

bool string_equals(const Json& object, std::string_view key, std::string_view expected) {
    const auto* value = object.find(key);
    return value && value->is_string() && value->as_string() == expected;
}

bool is_glint_manifest(const std::string& manifest, const std::vector<std::byte>& embedded) {
    if (!has_bounded_json_depth(manifest)) return false;
    try {
        const auto candidate = Json::parse(manifest);
        const auto current = Json::parse(std::string_view(reinterpret_cast<const char*>(embedded.data()), embedded.size()));
        if (!string_equals(candidate, "key", current.at("key").as_string()) ||
            !candidate.at("manifest_version").is_integer() || candidate.at("manifest_version").as_integer() != 3 ||
            (!string_equals(candidate, "name", "Glint Tab Search Connector") &&
             !string_equals(candidate, "name", "__MSG_extensionName__")) ||
            !candidate.at("version").is_string() || candidate.at("version").as_string().empty() ||
            !string_equals(candidate.at("background"), "service_worker", "background.js") ||
            !string_equals(candidate, "options_page", "options.html") ||
            !string_equals(candidate.at("action"), "default_popup", "options.html")) return false;
        for (const auto* size : {"16", "32", "48", "128"}) {
            if (!string_equals(candidate.at("icons"), size, std::string("icon-") + size + ".png")) return false;
        }
        return true;
    } catch (const ProtocolError&) {
        return false;
    }
}

} // namespace

bool ensure_extracted(std::filesystem::path& directory, std::wstring& error, bool* changed_existing) {
    directory.clear();
    error.clear();
    if (changed_existing) *changed_existing = false;

    std::filesystem::path local_data;
    if (!local_app_data(local_data, error)) return false;
    // The pre-key bundle used Glint/extension. Reusing that path after adding
    // a manifest key can make Chrome's old path-ID record invalidate the new
    // installation on every startup. Keep this public-key directory stable
    // and leave the old directory untouched for existing browser installs.
    directory = local_data / L"Glint" / L"browser-extension";

    PathGuard guard;
    bool had_runtime = false;
    bool has_original_structure = false;
    if (!preflight_bundle(directory, guard, had_runtime, has_original_structure, error)) return false;

    BundleContents contents;
    if (!load_bundle(contents, error)) return false;
    return refresh_bundle(directory, contents, guard, had_runtime, changed_existing, error);
}

bool sync_adjacent(const std::filesystem::path& executable_directory,
                   bool& changed_existing, std::wstring& error) {
    changed_existing = false;
    error.clear();
    const auto directory = executable_directory / L"browser-extension";
    PathGuard guard;
    DWORD attributes = INVALID_FILE_ATTRIBUTES;
    if (!guard.check(directory, true, error, &attributes, true)) return false;
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return true;
    if (!guard.check(directory / L"manifest.json", false, error, &attributes, true)) return false;
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return true;

    BundleContents contents;
    if (!load_bundle(contents, error)) return false;
    std::string manifest;
    if (!read_manifest(directory / L"manifest.json", manifest, error)) return false;
    if (!is_glint_manifest(manifest, contents[0])) return true;
    bool had_runtime = false;
    bool has_original_structure = false;
    if (!preflight_bundle(directory, guard, had_runtime, has_original_structure, error, true)) return false;
    if (!has_original_structure) return true;
    return refresh_bundle(directory, contents, guard, had_runtime, &changed_existing, error);
}

} // namespace glint::extension_bundle
