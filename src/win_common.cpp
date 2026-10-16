#include "win_common.hpp"

#include <sddl.h>
#include <userenv.h>

#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace glint::win {

std::wstring current_user_sid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return L"unknown";
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::byte> buffer(size);
    std::wstring result = L"unknown";
    if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        auto* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
        LPWSTR sid = nullptr;
        if (ConvertSidToStringSidW(user->User.Sid, &sid)) {
            result.assign(sid);
            LocalFree(sid);
        }
    }
    CloseHandle(token);
    return result;
}

std::wstring pipe_name() {
    return L"\\\\.\\pipe\\Glint." + current_user_sid();
}

bool write_all(HANDLE handle, const void* data, std::size_t size) {
    const auto* cursor = static_cast<const std::byte*>(data);
    while (size != 0) {
        const DWORD chunk = static_cast<DWORD>(size > 1u << 20 ? 1u << 20 : size);
        DWORD written = 0;
        if (!WriteFile(handle, cursor, chunk, &written, nullptr) || written == 0) return false;
        cursor += written;
        size -= written;
    }
    return true;
}

bool read_some(HANDLE handle, void* data, std::size_t size, DWORD& count) {
    return ReadFile(handle, data, static_cast<DWORD>(size), &count, nullptr) != FALSE;
}

std::wstring last_error_text(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(flags, nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text = length ? std::wstring(buffer, length) : L"Windows error " + std::to_wstring(error);
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
    return text;
}

std::wstring local_app_data() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    return length && length < MAX_PATH ? std::wstring(path, length) : L".";
}

} // namespace glint::win
