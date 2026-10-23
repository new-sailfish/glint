#pragma once

#include "glint/core.hpp"
#include "search_navigation.hpp"

#include <optional>

namespace glint {

class SearchResultSelection {
public:
    // Capture identity before replacing rows; positions can change on updates.
    void prepare(HWND list, std::wstring_view query, const std::vector<Tab>& rows, bool reset = false) {
        selected_key_.reset();
        if (!reset && query == query_) {
            const int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
            if (selected >= 0 && static_cast<std::size_t>(selected) < rows.size())
                selected_key_ = rows[static_cast<std::size_t>(selected)].key;
        }
        query_ = query;
    }

    void restore(HWND edit, HWND list, const std::vector<Tab>& rows) const {
        ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        if (rows.empty()) {
            if (GetFocus() == list) focus_search_edit(edit);
            return;
        }
        int selected = 0;
        if (selected_key_) {
            for (std::size_t i = 0; i < rows.size(); ++i) {
                if (rows[i].key == *selected_key_) {
                    selected = static_cast<int>(i);
                    break;
                }
            }
        }
        // Selection/focused-row state is separate from keyboard focus. Leave
        // the EDIT and its caret alone while highlighting the initial result.
        ListView_SetItemState(list, selected, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, selected, FALSE);
    }

private:
    std::wstring query_;
    std::optional<TabKey> selected_key_;
};

} // namespace glint
