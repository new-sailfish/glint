#pragma once

#include <windows.h>

#include <string_view>

namespace glint {

// Keeps one browser-owned host alive until browser EOF. Desktop pipe sessions
// reconnect independently and offline metadata is consumed without retention.
// input and output remain owned by the caller and use synchronous handles.
// A connected desktop_status includes generation; browser messages must echo
// that value as desktopGeneration to prevent old-session metadata crossing over.
int relay_native_messages(HANDLE input, HANDLE output, std::wstring_view pipe_name);

} // namespace glint
