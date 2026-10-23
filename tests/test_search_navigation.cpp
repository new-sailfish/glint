#include "search_navigation.hpp"
#include "search_results.hpp"

#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class SearchControls {
public:
    explicit SearchControls(int result_count = 3) {
        parent_ = CreateWindowExW(0, L"STATIC", L"Glint search navigation tests", WS_POPUP,
                                 0, 0, 480, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(parent_ != nullptr, "create test-owned parent window");
        try {
            edit = CreateWindowExW(0, L"EDIT", L"query text", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                   0, 0, 480, 30, parent_, nullptr, GetModuleHandleW(nullptr), nullptr);
            list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT,
                                   0, 30, 480, 270, parent_, nullptr, GetModuleHandleW(nullptr), nullptr);
            unrelated = CreateWindowExW(0, L"BUTTON", L"Unrelated", WS_CHILD | WS_VISIBLE,
                                        0, 0, 20, 20, parent_, nullptr, GetModuleHandleW(nullptr), nullptr);
            require(edit && list && unrelated, "create real EDIT, list-view, and unrelated controls");
            require(input_method.attach(edit), "track input-method messages on the native EDIT");
            require(SetWindowSubclass(edit, observe_edit, 1, reinterpret_cast<DWORD_PTR>(this)),
                    "observe keys delivered to the native EDIT");
            for (int i = 0; i != result_count; ++i) {
                auto label = L"Result " + std::to_wstring(i);
                LVITEMW item{};
                item.mask = LVIF_TEXT;
                item.iItem = i;
                item.pszText = label.data();
                require(ListView_InsertItem(list, &item) == i, "insert result in test list");
            }
        } catch (...) {
            DestroyWindow(parent_);
            throw;
        }
    }

    ~SearchControls() { DestroyWindow(parent_); }
    SearchControls(const SearchControls&) = delete;
    SearchControls& operator=(const SearchControls&) = delete;

    void focus(HWND control) {
        SetFocus(control);
        require(GetFocus() == control, "focus a control owned by this test thread");
    }

    void select(int index) {
        ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        if (index >= 0) {
            ListView_SetItemState(list, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
    }

    void press(UINT key) const {
        const HWND target = GetFocus();
        if (!glint::navigate_search_results(edit, list, key)) {
            SendMessageW(target, WM_KEYDOWN, key, 1);
        }
    }

    void require_selection(int index) const {
        require(ListView_GetSelectedCount(list) == 1, "exactly one result is selected");
        require(ListView_GetNextItem(list, -1, LVNI_SELECTED) == index, "expected result is selected");
        require(ListView_GetNextItem(list, -1, LVNI_FOCUSED) == index, "expected result has list focus");
    }

    void set_edit_selection(DWORD start, DWORD end) const {
        SendMessageW(edit, EM_SETSEL, start, end);
    }

    void require_edit_unchanged(DWORD start, DWORD end) const {
        wchar_t value[32]{};
        GetWindowTextW(edit, value, static_cast<int>(std::size(value)));
        require(std::wstring(value) == L"query text", "navigation preserves the query text");
        DWORD actual_start = 0, actual_end = 0;
        SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&actual_start), reinterpret_cast<LPARAM>(&actual_end));
        require(actual_start == start && actual_end == end, "navigation preserves the existing edit selection or caret");
    }

    void refresh(std::wstring_view query, std::vector<glint::Tab> next_rows, bool reset = false) {
        result_selection.prepare(list, query, rows, reset);
        rows = std::move(next_rows);
        ListView_DeleteAllItems(list);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(i);
            item.pszText = rows[i].title.data();
            require(ListView_InsertItem(list, &item) == static_cast<int>(i), "insert refreshed result");
        }
        result_selection.restore(edit, list, rows);
    }

    bool route_key(UINT key, UINT message = WM_KEYDOWN) {
        MSG input{};
        input.hwnd = GetFocus();
        input.message = message;
        input.wParam = key;
        input.lParam = 1;
        return glint::handle_search_key(input, edit, list, input_method, [this] { ++activations; });
    }

    HWND edit = nullptr, list = nullptr, unrelated = nullptr;
    glint::SearchInputMethod input_method;
    glint::SearchResultSelection result_selection;
    std::vector<glint::Tab> rows;
    int activations = 0, edit_keydowns = 0, edit_returns = 0;
    bool complete_ime_on_keydown = false;

private:
    static LRESULT CALLBACK observe_edit(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                         UINT_PTR, DWORD_PTR data) {
        auto* controls = reinterpret_cast<SearchControls*>(data);
        if (message == WM_KEYDOWN) {
            ++controls->edit_keydowns;
            if (controls->complete_ime_on_keydown) {
                SendMessageW(window, WM_IME_NOTIFY, IMN_CLOSECANDIDATE, 1);
                SendMessageW(window, WM_IME_ENDCOMPOSITION, 0, 0);
            }
        }
        if (message == WM_CHAR && wparam == VK_RETURN) ++controls->edit_returns;
        return DefSubclassProc(window, message, wparam, lparam);
    }

    HWND parent_ = nullptr;
};

std::vector<glint::Tab> result_rows(std::initializer_list<glint::TabKey> keys) {
    std::vector<glint::Tab> rows;
    for (const auto key : keys) {
        glint::Tab row;
        row.key = key;
        row.title = L"Tab " + std::to_wstring(key.tab_id);
        rows.push_back(std::move(row));
    }
    return rows;
}

void refresh_selects_first_without_moving_edit_caret() {
    SearchControls controls(0);
    controls.focus(controls.edit);
    controls.set_edit_selection(2, 7);
    controls.refresh(L"query", result_rows({{1, 10}, {1, 20}}));
    controls.require_selection(0);
    require(GetFocus() == controls.edit, "first result is highlighted while query keeps focus");
    controls.require_edit_unchanged(2, 7);
    require(controls.route_key(VK_RETURN), "Enter activates the initially highlighted result");
    require(controls.activations == 1, "first result can be activated immediately");
}

void background_refresh_preserves_tab_identity_after_reordering() {
    for (const bool edit_focus : {false, true}) {
        SearchControls controls(0);
        controls.refresh(L"query", result_rows({{1, 10}, {1, 20}, {2, 20}}));
        controls.select(2);
        controls.focus(edit_focus ? controls.edit : controls.list);
        controls.set_edit_selection(3, 3);
        controls.refresh(L"query", result_rows({{1, 20}, {2, 20}, {1, 10}}));
        controls.require_selection(1);
        require(GetFocus() == (edit_focus ? controls.edit : controls.list), "background refresh retains control focus");
        controls.require_edit_unchanged(3, 3);
    }
}

void changed_query_and_explicit_reset_choose_first() {
    SearchControls controls(0);
    const auto rows = result_rows({{1, 10}, {1, 20}});
    controls.refresh(L"query", rows);
    controls.select(1);
    controls.focus(controls.edit);
    controls.set_edit_selection(4, 4);
    controls.refresh(L"changed", rows);
    controls.require_selection(0);
    controls.select(1);
    controls.refresh(L"changed", rows, true);
    controls.require_selection(0);
    controls.require_edit_unchanged(4, 4);
}

void removed_selection_falls_back_to_first() {
    SearchControls controls(0);
    controls.refresh(L"query", result_rows({{1, 10}, {2, 20}}));
    controls.select(1);
    controls.focus(controls.list);
    controls.refresh(L"query", result_rows({{1, 30}, {1, 20}}));
    controls.require_selection(0);
    require(GetFocus() == controls.list, "fallback selection keeps list focus");
}

void empty_refresh_clears_selection_and_returns_list_focus_to_edit() {
    SearchControls controls(0);
    controls.refresh(L"query", result_rows({{1, 10}, {1, 20}}));
    controls.select(1);
    controls.focus(controls.list);
    controls.set_edit_selection(1, 8);
    controls.refresh(L"query", {});
    require(ListView_GetSelectedCount(controls.list) == 0, "empty results have no selected row");
    require(GetFocus() == controls.edit, "empty refresh returns list focus to query");
    controls.require_edit_unchanged(1, 8);
    controls.refresh(L"query", result_rows({{3, 30}}));
    controls.require_selection(0);
    require(GetFocus() == controls.edit, "first arriving result keeps query focus");
}

void refresh_does_not_take_focus_from_unrelated_control() {
    SearchControls controls(0);
    controls.focus(controls.unrelated);
    controls.refresh(L"query", result_rows({{1, 10}}));
    controls.require_selection(0);
    require(GetFocus() == controls.unrelated, "refresh never steals focus from another control");
    controls.refresh(L"query", {});
    require(GetFocus() == controls.unrelated, "empty refresh never steals unrelated focus");
}

void composition_enter_reaches_native_edit_without_activation() {
    SearchControls controls;
    controls.select(0);
    controls.focus(controls.edit);
    controls.set_edit_selection(3, 3);
    SendMessageW(controls.edit, WM_IME_STARTCOMPOSITION, 0, 0);
    require(controls.route_key(VK_RETURN), "route composition Enter directly to native EDIT");
    require(controls.activations == 0 && controls.edit_keydowns == 1,
            "composition Enter reaches native EDIT without switching tabs");
    controls.require_edit_unchanged(3, 3);
    SendMessageW(controls.edit, WM_IME_ENDCOMPOSITION, 0, 0);
    require(controls.route_key(VK_RETURN), "ordinary Enter activates after composition ends");
    require(controls.activations == 1, "composition-end restores ordinary result activation");
}

void candidate_enter_reaches_edit_until_all_candidate_lists_close() {
    SearchControls controls;
    controls.select(0);
    controls.focus(controls.edit);
    SendMessageW(controls.edit, WM_IME_NOTIFY, IMN_OPENCANDIDATE, 1);
    SendMessageW(controls.edit, WM_IME_NOTIFY, IMN_OPENCANDIDATE, 2);
    SendMessageW(controls.edit, WM_IME_NOTIFY, IMN_CLOSECANDIDATE, 1);
    require(controls.route_key(VK_RETURN), "route candidate-selection Enter to native EDIT");
    require(controls.activations == 0 && controls.edit_keydowns == 1,
            "an open candidate list owns Enter even after another candidate list closes");
    SendMessageW(controls.edit, WM_IME_NOTIFY, IMN_CLOSECANDIDATE, 2);
    require(controls.route_key(VK_RETURN) && controls.activations == 1,
            "ordinary Enter activates once all candidate lists close");
}

void translated_process_key_reaches_native_edit() {
    SearchControls controls;
    controls.select(0);
    controls.focus(controls.edit);
    SendMessageW(controls.edit, WM_IME_STARTCOMPOSITION, 0, 0);
    SendMessageW(controls.edit, WM_IME_ENDCOMPOSITION, 0, 0);
    require(controls.route_key(VK_PROCESSKEY), "bypass dialog-key processing for VK_PROCESSKEY");
    require(controls.activations == 0 && controls.edit_keydowns == 1,
            "translated input-method key reaches native EDIT");
}

void completing_composition_during_enter_does_not_reuse_it_for_activation() {
    SearchControls controls;
    controls.select(0);
    controls.focus(controls.edit);
    SendMessageW(controls.edit, WM_IME_STARTCOMPOSITION, 0, 0);
    SendMessageW(controls.edit, WM_IME_NOTIFY, IMN_OPENCANDIDATE, 1);
    controls.complete_ime_on_keydown = true;
    require(controls.route_key(VK_RETURN), "native EDIT completes composition while handling Enter");
    require(!controls.input_method.active(), "native notifications closed composition and candidates");
    require(controls.activations == 0 && controls.edit_keydowns == 1,
            "the confirming Enter is never reused to activate a result after native processing");
    controls.complete_ime_on_keydown = false;
    require(controls.route_key(VK_RETURN) && controls.activations == 1,
            "a separate Enter after composition completion activates the selected result");
}

void enter_without_results_reaches_native_edit() {
    SearchControls controls(0);
    controls.focus(controls.edit);
    controls.set_edit_selection(2, 7);
    require(controls.route_key(VK_RETURN), "handle Enter without results before dialog translation");
    require(controls.activations == 0 && controls.edit_keydowns == 1,
            "Enter without results reaches native EDIT without activation");
    require(controls.route_key(VK_RETURN, WM_CHAR), "translated Enter character also reaches native EDIT");
    require(controls.edit_returns == 1, "native EDIT receives Enter character");
    controls.require_edit_unchanged(2, 7);
}

void input_method_owns_navigation_and_escape_until_focus_leaves_edit() {
    SearchControls controls;
    controls.select(0);
    controls.focus(controls.edit);
    SendMessageW(controls.edit, WM_IME_STARTCOMPOSITION, 0, 0);
    for (const UINT key : {VK_UP, VK_DOWN, VK_ESCAPE})
        require(controls.route_key(key), "input method receives its own navigation and cancel keys");
    require(controls.edit_keydowns == 3 && controls.activations == 0,
            "composition navigation remains inside the native EDIT");
    controls.focus(controls.list);
    require(controls.route_key(VK_RETURN) && controls.activations == 1, "list Enter is ordinary activation");
    controls.focus(controls.edit);
    require(controls.route_key(VK_RETURN) && controls.activations == 2,
            "leaving edit focus clears stale composition state");
}

void search_key_routing_leaves_ordinary_keys_and_unrelated_focus_alone() {
    SearchControls controls;
    controls.select(0);
    controls.focus(controls.edit);
    for (const UINT key : {VK_UP, VK_DOWN, VK_ESCAPE, VK_TAB, 0x41})
        require(!controls.route_key(key), "ordinary non-Enter key remains with existing message routing");
    controls.focus(controls.unrelated);
    require(!controls.route_key(VK_RETURN), "unrelated controls keep their own Enter behavior");
    require(controls.activations == 0 && controls.edit_keydowns == 0, "ordinary routing causes no side effects");
}

void up_from_first_returns_to_query() {
    SearchControls controls;
    controls.set_edit_selection(2, 7);
    controls.select(0);
    controls.focus(controls.list);
    require(glint::navigate_search_results(controls.edit, controls.list, VK_UP), "consume Up from the first result");
    require(GetFocus() == controls.edit, "Up from the first result focuses the query");
    controls.require_edit_unchanged(2, 7);
}

void up_from_second_selects_previous() {
    SearchControls controls;
    controls.select(1);
    controls.focus(controls.list);
    require(glint::navigate_search_results(controls.edit, controls.list, VK_UP), "consume Up from the second result");
    require(GetFocus() == controls.list, "Up within results retains list focus");
    controls.require_selection(0);
}

void down_from_last_stays_at_last() {
    SearchControls controls;
    controls.select(2);
    controls.focus(controls.list);
    require(glint::navigate_search_results(controls.edit, controls.list, VK_DOWN), "consume Down from the last result");
    require(GetFocus() == controls.list, "Down from the last result retains list focus");
    controls.require_selection(2);
}

void down_from_query_enters_first_even_with_retained_selection() {
    SearchControls controls;
    controls.set_edit_selection(3, 3);
    controls.select(2);
    controls.focus(controls.edit);
    require(glint::navigate_search_results(controls.edit, controls.list, VK_DOWN), "consume Down from the query");
    require(GetFocus() == controls.list, "Down from the query focuses results");
    controls.require_selection(0);
    controls.require_edit_unchanged(3, 3);
}

void up_from_query_keeps_focus_and_retained_selection() {
    for (const int selected : {-1, 0, 1, 2}) {
        SearchControls controls;
        controls.set_edit_selection(1, 8);
        controls.select(selected);
        controls.focus(controls.edit);
        for (int repeat = 0; repeat != 3; ++repeat) {
            require(glint::navigate_search_results(controls.edit, controls.list, VK_UP), "consume Up from the query");
            require(GetFocus() == controls.edit, "Up from the query retains input focus");
            require(ListView_GetNextItem(controls.list, -1, LVNI_SELECTED) == selected,
                    "Up from the query preserves the existing result selection");
            controls.require_edit_unchanged(1, 8);
        }
    }
}

void down_without_selection_enters_first() {
    SearchControls controls;
    controls.focus(controls.list);
    require(glint::navigate_search_results(controls.edit, controls.list, VK_DOWN), "consume Down with no selected result");
    require(GetFocus() == controls.list, "Down with no selection retains list focus");
    controls.require_selection(0);
}

void up_without_selection_returns_to_query() {
    SearchControls controls;
    controls.set_edit_selection(4, 4);
    controls.focus(controls.list);
    require(glint::navigate_search_results(controls.edit, controls.list, VK_UP), "consume Up with no selected result");
    require(GetFocus() == controls.edit, "Up with no selection focuses the query");
    controls.require_edit_unchanged(4, 4);
}

void empty_results_keep_or_return_query_focus() {
    for (const UINT key : {VK_UP, VK_DOWN}) {
        SearchControls controls(0);
        controls.set_edit_selection(2, 6);
        controls.focus(controls.edit);
        require(glint::navigate_search_results(controls.edit, controls.list, key), "consume navigation from query with no results");
        require(GetFocus() == controls.edit, "an empty result list keeps query focus");
        controls.require_edit_unchanged(2, 6);
        controls.focus(controls.list);
        require(glint::navigate_search_results(controls.edit, controls.list, key), "consume navigation from an empty list");
        require(GetFocus() == controls.edit, "an empty focused result list returns to the query");
        controls.require_edit_unchanged(2, 6);
    }
}

void unrelated_keys_are_not_consumed() {
    SearchControls controls;
    controls.set_edit_selection(1, 5);
    controls.select(1);
    for (const HWND source : {controls.edit, controls.list}) {
        controls.focus(source);
        for (const UINT key : {VK_RETURN, VK_ESCAPE, VK_TAB, VK_LEFT, VK_RIGHT, 0x41}) {
            require(!glint::navigate_search_results(controls.edit, controls.list, key), "leave unrelated keys to the caller");
            require(GetFocus() == source, "unrelated keys preserve focus");
            controls.require_selection(1);
            controls.require_edit_unchanged(1, 5);
        }
    }
}

void unrelated_focus_is_not_consumed() {
    SearchControls controls;
    controls.select(1);
    controls.focus(controls.unrelated);
    for (const UINT key : {VK_UP, VK_DOWN}) {
        require(!glint::navigate_search_results(controls.edit, controls.list, key), "leave keys from unrelated controls to the caller");
        require(GetFocus() == controls.unrelated, "navigation does not steal unrelated focus");
        controls.require_selection(1);
    }
}

void selecting_a_result_clears_previous_selection() {
    SearchControls controls;
    ListView_SetItemState(controls.list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(controls.list, 2, LVIS_SELECTED, LVIS_SELECTED);
    controls.focus(controls.edit);
    require(glint::navigate_search_results(controls.edit, controls.list, VK_DOWN), "consume Down with multiple retained selections");
    controls.require_selection(0);
}

void up_from_query_does_not_scroll_long_results() {
    SearchControls controls(100);
    require(ListView_GetTopIndex(controls.list) == 0, "long result list initially shows the first row");
    controls.select(0);
    controls.focus(controls.edit);
    require(glint::navigate_search_results(controls.edit, controls.list, VK_UP), "consume Up from query above a long list");
    require(GetFocus() == controls.edit, "Up above a long list retains input focus");
    controls.require_selection(0);
    require(ListView_GetTopIndex(controls.list) == 0, "Up from the query does not scroll the result list");
}

void arrows_traverse_results_and_return_to_query() {
    SearchControls controls;
    controls.set_edit_selection(3, 7);
    controls.focus(controls.edit);
    for (const auto& [key, index] : {std::pair<UINT, int>{VK_DOWN, 0}, {VK_DOWN, 1}, {VK_DOWN, 2},
                                   {VK_DOWN, 2}, {VK_UP, 1}, {VK_UP, 0}}) {
        require(glint::navigate_search_results(controls.edit, controls.list, key), "consume arrow navigation through results");
        require(GetFocus() == controls.list, "navigation among results retains list focus");
        controls.require_selection(index);
        controls.require_edit_unchanged(3, 7);
    }
    require(glint::navigate_search_results(controls.edit, controls.list, VK_UP), "consume Up returning to the query");
    require(GetFocus() == controls.edit, "Up before the first result returns to the query");
    controls.require_edit_unchanged(3, 7);
    require(glint::navigate_search_results(controls.edit, controls.list, VK_UP), "consume another Up after returning to the query");
    require(GetFocus() == controls.edit, "another Up keeps focus in the query");
    controls.require_selection(0);
    controls.require_edit_unchanged(3, 7);
}

void top_keys_reach_first_then_return_to_query() {
    for (const UINT key : {VK_HOME, VK_PRIOR}) {
        SearchControls controls;
        controls.set_edit_selection(2, 7);
        controls.select(2);
        controls.focus(controls.list);
        controls.press(key);
        require(GetFocus() == controls.list, "first Home or PageUp retains result-list focus");
        controls.require_selection(0);
        controls.press(key);
        require(GetFocus() == controls.edit, "another Home or PageUp from the first result returns to the query");
        controls.require_edit_unchanged(2, 7);
        controls.press(key);
        require(GetFocus() == controls.edit, "continued Home or PageUp stays in the query");
        controls.require_selection(0);
        controls.require_edit_unchanged(key == VK_HOME ? 0 : 2, key == VK_HOME ? 0 : 7);
    }
}

void page_up_preserves_native_paging_until_first() {
    SearchControls controls(100);
    controls.set_edit_selection(1, 8);
    controls.select(99);
    ListView_EnsureVisible(controls.list, 99, FALSE);
    controls.focus(controls.list);
    controls.press(VK_PRIOR);
    const int previous = ListView_GetNextItem(controls.list, -1, LVNI_SELECTED);
    require(GetFocus() == controls.list && previous > 0 && previous < 99,
            "PageUp within a long list retains native paging rather than jumping to the top");
    for (int page = 0; page != 100 && ListView_GetNextItem(controls.list, -1, LVNI_SELECTED) > 0; ++page) {
        controls.press(VK_PRIOR);
        require(GetFocus() == controls.list, "paging to the first result retains list focus");
    }
    controls.require_selection(0);
    controls.press(VK_PRIOR);
    require(GetFocus() == controls.edit, "PageUp at the top of a long list returns to the query");
    controls.require_edit_unchanged(1, 8);
}

void top_keys_from_empty_or_unselected_list_return_to_query() {
    for (const UINT key : {VK_HOME, VK_PRIOR}) {
        for (const int count : {0, 3}) {
            SearchControls controls(count);
            controls.set_edit_selection(4, 4);
            controls.focus(controls.list);
            controls.press(key);
            require(GetFocus() == controls.edit, "Home or PageUp with no selected result returns to the query");
            controls.require_edit_unchanged(4, 4);
        }
    }
}

void top_keys_do_not_steal_unrelated_focus() {
    SearchControls controls;
    controls.select(1);
    controls.focus(controls.unrelated);
    for (const UINT key : {VK_HOME, VK_PRIOR}) {
        controls.press(key);
        require(GetFocus() == controls.unrelated, "Home or PageUp leaves unrelated control focus alone");
        controls.require_selection(1);
    }
}

void top_keys_follow_focused_row_when_selection_differs() {
    for (const UINT key : {VK_HOME, VK_PRIOR}) {
        for (const int focused : {0, 2}) {
            SearchControls controls;
            controls.set_edit_selection(2, 7);
            controls.select(focused == 0 ? 2 : 0);
            ListView_SetItemState(controls.list, -1, 0, LVIS_FOCUSED);
            ListView_SetItemState(controls.list, focused, LVIS_FOCUSED, LVIS_FOCUSED);
            controls.focus(controls.list);
            controls.press(key);
            require(GetFocus() == (focused == 0 ? controls.edit : controls.list),
                    "Home or PageUp uses the keyboard-focused row to determine the top boundary");
            controls.require_edit_unchanged(2, 7);
        }
    }
}
} // namespace

int main() {
    INITCOMMONCONTROLSEX common_controls{sizeof(common_controls), ICC_LISTVIEW_CLASSES};
    if (!InitCommonControlsEx(&common_controls)) {
        std::cerr << "FAIL initialize list-view controls\n";
        return 1;
    }
    const std::pair<const char*, void (*)()> tests[] = {
        {"Refresh selects first while preserving edit focus and caret", refresh_selects_first_without_moving_edit_caret},
        {"Background refresh preserves full TabKey after reordering", background_refresh_preserves_tab_identity_after_reordering},
        {"Changed query and explicit reset choose first", changed_query_and_explicit_reset_choose_first},
        {"Removed selected tab falls back to first", removed_selection_falls_back_to_first},
        {"Empty refresh clears selection and restores query focus", empty_refresh_clears_selection_and_returns_list_focus_to_edit},
        {"Refresh preserves unrelated focus", refresh_does_not_take_focus_from_unrelated_control},
        {"Composition Enter reaches native EDIT", composition_enter_reaches_native_edit_without_activation},
        {"Candidate Enter reaches native EDIT until all lists close", candidate_enter_reaches_edit_until_all_candidate_lists_close},
        {"Translated VK_PROCESSKEY reaches native EDIT", translated_process_key_reaches_native_edit},
        {"Completing IME during Enter never reuses it for activation", completing_composition_during_enter_does_not_reuse_it_for_activation},
        {"Enter without results reaches native EDIT", enter_without_results_reaches_native_edit},
        {"IME owns navigation and Escape while edit has focus", input_method_owns_navigation_and_escape_until_focus_leaves_edit},
        {"Key routing preserves ordinary keys and unrelated focus", search_key_routing_leaves_ordinary_keys_and_unrelated_focus_alone},
        {"Up from first returns to query", up_from_first_returns_to_query},
        {"Up from second selects previous", up_from_second_selects_previous},
        {"Down from last stays at last", down_from_last_stays_at_last},
        {"Down from query enters first", down_from_query_enters_first_even_with_retained_selection},
        {"Up from query keeps focus and selection", up_from_query_keeps_focus_and_retained_selection},
        {"Down without selection enters first", down_without_selection_enters_first},
        {"Up without selection returns to query", up_without_selection_returns_to_query},
        {"Empty results keep or return query focus", empty_results_keep_or_return_query_focus},
        {"Unrelated keys are not consumed", unrelated_keys_are_not_consumed},
        {"Unrelated focus is not consumed", unrelated_focus_is_not_consumed},
        {"Selecting a result clears previous selection", selecting_a_result_clears_previous_selection},
        {"Up from query does not scroll long results", up_from_query_does_not_scroll_long_results},
        {"Arrows traverse results and return to query", arrows_traverse_results_and_return_to_query},
        {"Home and PageUp reach first then return to query", top_keys_reach_first_then_return_to_query},
        {"PageUp preserves native paging until first", page_up_preserves_native_paging_until_first},
        {"Home and PageUp from empty or unselected list return to query", top_keys_from_empty_or_unselected_list_return_to_query},
        {"Home and PageUp do not steal unrelated focus", top_keys_do_not_steal_unrelated_focus},
        {"Home and PageUp follow the keyboard-focused row", top_keys_follow_focused_row_when_selection_differs},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << std::size(tests) - failures << '/' << std::size(tests) << " search navigation tests passed\n";
    return failures == 0 ? 0 : 1;
}
