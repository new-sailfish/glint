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

} // namespace glint
