#pragma once

#include <windows.h>
#include <commctrl.h>

namespace glint {

inline void focus_search_edit(HWND edit) {
    DWORD start = 0, end = 0;
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    SetFocus(edit);
    SendMessageW(edit, EM_SETSEL, start, end);
}

// IME state belongs to the native EDIT. Observe its notifications instead of
// treating Enter as an application shortcut while a candidate is being chosen.
class SearchInputMethod {
public:
    SearchInputMethod() = default;
    ~SearchInputMethod() {
        if (edit_) RemoveWindowSubclass(edit_, subclass_proc, subclass_id());
    }
    SearchInputMethod(const SearchInputMethod&) = delete;
    SearchInputMethod& operator=(const SearchInputMethod&) = delete;

    bool attach(HWND edit) {
        if (edit_) RemoveWindowSubclass(edit_, subclass_proc, subclass_id());
        edit_ = nullptr;
        composing_ = false;
        candidate_lists_ = 0;
        if (!edit || !SetWindowSubclass(edit, subclass_proc, subclass_id(), reinterpret_cast<DWORD_PTR>(this))) return false;
        edit_ = edit;
        return true;
    }

    bool active() const noexcept { return composing_ || candidate_lists_ != 0; }

private:
    UINT_PTR subclass_id() const noexcept { return reinterpret_cast<UINT_PTR>(this); }

    static LRESULT CALLBACK subclass_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                          UINT_PTR id, DWORD_PTR data) {
        auto* self = reinterpret_cast<SearchInputMethod*>(data);
        if (message == WM_IME_STARTCOMPOSITION) self->composing_ = true;
        if (message == WM_IME_ENDCOMPOSITION) self->composing_ = false;
        if (message == WM_IME_NOTIFY) {
            if (wparam == IMN_OPENCANDIDATE) self->candidate_lists_ |= static_cast<DWORD_PTR>(lparam);
            if (wparam == IMN_CLOSECANDIDATE) self->candidate_lists_ &= ~static_cast<DWORD_PTR>(lparam);
        }
        if (message == WM_KILLFOCUS || message == WM_NCDESTROY) {
            self->composing_ = false;
            self->candidate_lists_ = 0;
        }
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(window, subclass_proc, id);
            self->edit_ = nullptr;
        }
        return DefSubclassProc(window, message, wparam, lparam);
    }

    HWND edit_ = nullptr;
    bool composing_ = false;
    DWORD_PTR candidate_lists_ = 0;
};

// Call before shortcuts and IsDialogMessage: the latter can consume Enter
// before the EDIT/IME receives it, even when there is no selected result.
template<class Activate>
inline bool handle_search_key(MSG& message, HWND edit, HWND list,
                              const SearchInputMethod& input_method, Activate activate) {
    const HWND focused = GetFocus();
    if (message.hwnd != focused || (focused != edit && focused != list)) return false;
    const bool key_down = message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN;
    const bool native_edit_key = focused == edit &&
        ((key_down && (input_method.active() || message.wParam == VK_PROCESSKEY)) ||
         (message.message == WM_CHAR && message.wParam == VK_RETURN));
    if (native_edit_key) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
        return true;
    }
    if (message.message != WM_KEYDOWN || message.wParam != VK_RETURN) return false;

    const int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (selected >= 0 && selected < ListView_GetItemCount(list)) {
        activate();
    } else {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return true;
}

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
            focus_search_edit(edit);
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

// Recent cards share the result list's selection, but keep the native EDIT
// ready for typing and IME composition. Call after handle_search_key so an
// open candidate list retains Tab and arrow keys.
inline bool navigate_recent_results(HWND edit, HWND list, UINT key, bool reverse = false, int columns = 2) {
    if (key != VK_TAB && key != VK_LEFT && key != VK_RIGHT && key != VK_UP && key != VK_DOWN) return false;
    const HWND focused = GetFocus();
    if (focused != edit && focused != list) return false;
    if ((GetKeyState(VK_CONTROL) | GetKeyState(VK_MENU) | GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000)
        return false;

    const int count = ListView_GetItemCount(list);
    if (focused != edit) focus_search_edit(edit);
    if (count == 0) return true;
    const int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    int next = selected;
    if (selected < 0 || selected >= count) {
        next = key == VK_TAB && reverse ? count - 1 : 0;
    } else if (key == VK_TAB) {
        next = reverse ? (selected == 0 ? count - 1 : selected - 1)
                       : (selected + 1 == count ? 0 : selected + 1);
    } else {
        if (columns < 1) columns = 1;
        const int column = selected % columns;
        if (key == VK_LEFT && column > 0) --next;
        if (key == VK_RIGHT && column + 1 < columns && selected + 1 < count) ++next;
        if (key == VK_UP && selected >= columns) next -= columns;
        // A short final row still receives Down from either column above it.
        if (key == VK_DOWN && columns < count - (selected - column))
            next = columns < count - selected ? selected + columns : count - 1;
    }
    ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(list, next, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(list, next, FALSE);
    return true;
}

// Transfer text input before TranslateMessage/IsDialogMessage can turn the
// first keystroke into list type-ahead or start an IME on the wrong control.
// WM_CHAR also covers already translated Unicode text and surrogate pairs.
inline bool handle_search_typing(MSG& message, HWND edit, HWND list) {
    if (message.hwnd != list || GetFocus() != list) return false;
    if ((GetKeyState(VK_CONTROL) | GetKeyState(VK_MENU) | GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000)
        return false;

    bool text_input = message.message == WM_CHAR && message.wParam >= 0x20 && message.wParam != 0x7f;
    if (message.message == WM_KEYDOWN) {
        const UINT key = static_cast<UINT>(message.wParam);
        const UINT character = MapVirtualKeyW(key, MAPVK_VK_TO_CHAR) & 0x7fffffff;
        text_input = key == VK_PROCESSKEY || key == VK_PACKET || key == VK_BACK ||
            (character >= 0x20 && character != 0x7f);
    }
    if (!text_input) return false;

    focus_search_edit(edit);
    MSG input = message;
    input.hwnd = edit;
    TranslateMessage(&input);
    DispatchMessageW(&input);
    return true;
}

} // namespace glint
