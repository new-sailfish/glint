#pragma once

#include <windows.h>
#include <commctrl.h>

namespace glint {

inline bool navigate_search_results(HWND edit, HWND list, UINT key) {
    // Keyboards can translate Fn+Up into Home or PageUp.
    const bool top_navigation = key == VK_HOME || key == VK_PRIOR;
    if (key != VK_UP && key != VK_DOWN && !top_navigation) return false;

    const HWND focused = GetFocus();
    if (focused != edit && focused != list) return false;
    if (focused == edit && top_navigation) return false;
    if (focused == edit && key == VK_UP) return true;

    const int count = ListView_GetItemCount(list);
    const int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    const int focused_row = top_navigation ? ListView_GetNextItem(list, -1, LVNI_FOCUSED) : selected;
    const int boundary_row = focused_row < 0 ? selected : focused_row;
    if (count == 0 || (focused == list && (key == VK_UP || top_navigation) && boundary_row <= 0)) {
        if (focused != edit) {
            DWORD start = 0, end = 0;
            SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
            SetFocus(edit);
            SendMessageW(edit, EM_SETSEL, start, end);
        }
        return true;
    }
    if (top_navigation) return false; // Preserve native Home/PageUp movement within the list.

    int next = 0;
    if (focused == edit) {
        next = 0;
    } else if (key == VK_UP) {
        next = selected - 1;
    } else {
        next = selected < 0 ? 0 : (selected + 1 < count ? selected + 1 : count - 1);
    }

    SetFocus(list);
    ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(list, next, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(list, next, FALSE);
    return true;
}

} // namespace glint
