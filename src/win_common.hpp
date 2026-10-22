#pragma once

#include <windows.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace glint::win {

std::wstring current_user_sid();
std::wstring pipe_name();
bool write_all(HANDLE handle, const void* data, std::size_t size);
bool read_some(HANDLE handle, void* data, std::size_t size, DWORD& count);
// For handles opened with FILE_FLAG_OVERLAPPED. Independent read/write events
// keep a pending read from serializing the opposite direction of a duplex pipe.
bool read_pipe_some(HANDLE handle, void* data, std::size_t size, DWORD& count, HANDLE stop_event = nullptr);
bool write_pipe_all(HANDLE handle, const void* data, std::size_t size, HANDLE stop_event = nullptr,
                    DWORD timeout_ms = INFINITE);
std::wstring last_error_text(DWORD error = GetLastError());
std::wstring local_app_data();

} // namespace glint::win
