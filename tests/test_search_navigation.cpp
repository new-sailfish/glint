#include "search_navigation.hpp"

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

    HWND edit = nullptr, list = nullptr, unrelated = nullptr;

private:
    HWND parent_ = nullptr;
};

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
