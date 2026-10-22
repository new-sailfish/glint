#include "win_common.hpp"

#include <sddl.h>
#include <userenv.h>

#include <algorithm>
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

namespace {
bool pipe_io(HANDLE handle, void* data, DWORD size, DWORD& count, bool writing, HANDLE stop_event,
             DWORD timeout_ms = INFINITE) {
    count = 0;
    if (stop_event && WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) {
        SetLastError(ERROR_OPERATION_ABORTED);
        return false;
    }
    OVERLAPPED operation{};
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!operation.hEvent) return false;
    BOOL success = writing ? WriteFile(handle, data, size, &count, &operation)
                           : ReadFile(handle, data, size, &count, &operation);
    DWORD error = success ? ERROR_SUCCESS : GetLastError();
    if (!success && error == ERROR_IO_PENDING) {
        DWORD cancelled_error = ERROR_SUCCESS;
        if (stop_event) {
            const HANDLE events[] = {operation.hEvent, stop_event};
            const DWORD wait = WaitForMultipleObjects(2, events, FALSE, timeout_ms);
            if (wait != WAIT_OBJECT_0) {
                const DWORD wait_error = wait == WAIT_FAILED ? GetLastError() : ERROR_OPERATION_ABORTED;
                CancelIoEx(handle, &operation);
                cancelled_error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : wait_error;
            }
        } else {
            const DWORD wait = WaitForSingleObject(operation.hEvent, timeout_ms);
            if (wait != WAIT_OBJECT_0) {
                const DWORD wait_error = wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
                CancelIoEx(handle, &operation);
                cancelled_error = wait_error;
            }
        }
        // Always drain completion before destroying OVERLAPPED or its event.
        success = GetOverlappedResult(handle, &operation, &count, TRUE);
        error = success ? ERROR_SUCCESS : GetLastError();
        if (cancelled_error != ERROR_SUCCESS) { success = FALSE; error = cancelled_error; }
    }
    CloseHandle(operation.hEvent);
    SetLastError(error);
    return success != FALSE;
}
} // namespace

bool read_pipe_some(HANDLE handle, void* data, std::size_t size, DWORD& count, HANDLE stop_event) {
    const DWORD chunk = static_cast<DWORD>((std::min)(size, static_cast<std::size_t>(1u << 20)));
    return pipe_io(handle, data, chunk, count, false, stop_event);
}

bool write_pipe_all(HANDLE handle, const void* data, std::size_t size, HANDLE stop_event, DWORD timeout_ms) {
    const auto* cursor = static_cast<const std::byte*>(data);
    const ULONGLONG started = GetTickCount64();
    while (size != 0) {
        DWORD remaining = INFINITE;
        if (timeout_ms != INFINITE) {
            const ULONGLONG elapsed = GetTickCount64() - started;
            if (elapsed >= timeout_ms) { SetLastError(ERROR_TIMEOUT); return false; }
            remaining = timeout_ms - static_cast<DWORD>(elapsed);
        }
        const DWORD chunk = static_cast<DWORD>((std::min)(size, static_cast<std::size_t>(1u << 20)));
        DWORD written = 0;
        if (!pipe_io(handle, const_cast<std::byte*>(cursor), chunk, written, true, stop_event, remaining) || written == 0) return false;
        cursor += written;
        size -= written;
    }
    return true;
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
