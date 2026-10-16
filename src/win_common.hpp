#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace glint::win {

std::wstring current_user_sid();
std::wstring pipe_name();
bool write_all(HANDLE handle, const void* data, std::size_t size);
bool read_some(HANDLE handle, void* data, std::size_t size, DWORD& count);
std::wstring last_error_text(DWORD error = GetLastError());
std::wstring local_app_data();

} // namespace glint::win
