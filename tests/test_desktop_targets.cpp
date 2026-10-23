#include "desktop_targets.hpp"
#include "recent_results.hpp"

#include <windows.h>

#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using glint::Tab;
using glint::TabSource;
using glint::desktop::UiAdapter;
using glint::desktop::WindowFacts;

namespace {

WindowFacts notepad_window() {
    return {
        .window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(0x1234)),
        .process_id = 42,
        .process_creation_time = 987654321,
        .title = L"notes.txt - Notepad",
        .image_name = L"Notepad.exe",
        .visible = true,
        .cloaked = false,
        .tool_window = false,
        .shell_window = false,
    };
}

void window_filter_rejects_non_searchable_surfaces() {
    constexpr DWORD own_process = 100;
    auto facts = notepad_window();
    assert(glint::desktop::should_include_window(facts, own_process));

    facts.visible = false;
    assert(!glint::desktop::should_include_window(facts, own_process));
    facts.visible = true;
    facts.cloaked = true;
    assert(!glint::desktop::should_include_window(facts, own_process));
    facts.cloaked = false;
    facts.tool_window = true;
    assert(!glint::desktop::should_include_window(facts, own_process));
    facts.tool_window = false;
    facts.shell_window = true;
    assert(!glint::desktop::should_include_window(facts, own_process));
    facts.shell_window = false;
    facts.class_name = L"Windows.UI.Core.CoreWindow";
    facts.image_name = L"UwpApp.exe";
    assert(glint::desktop::should_include_window(facts, own_process));
    facts.class_name = L"ApplicationFrameWindow";
    facts.image_name = L"ApplicationFrameHost.exe";
    assert(glint::desktop::should_include_window(facts, own_process));
    facts.class_name.clear();
    facts.image_name = L"Notepad.exe";
    facts.title.clear();
    assert(!glint::desktop::should_include_window(facts, own_process));
    facts.title = L"notes.txt - Notepad";
    facts.process_id = own_process;
    assert(!glint::desktop::should_include_window(facts, own_process));
    facts.process_id = 42;
    facts.image_name = L"chrome.exe";
    assert(!glint::desktop::should_include_window(facts, own_process));
    facts.image_name = L"msedge.exe";
    assert(!glint::desktop::should_include_window(facts, own_process));
    facts.image_name = L"TextInputHost.exe";
    assert(!glint::desktop::should_include_window(facts, own_process));
}

void application_names_prefer_descriptions_and_have_browser_fallbacks() {
    assert(glint::desktop::application_name(L"Notepad.exe", L"Notepad") == L"Notepad");
    assert(glint::desktop::application_name(L"sublime_text.exe", L"") == L"Sublime Text");
    assert(glint::desktop::application_name(L"Code.exe", L"") == L"Visual Studio Code");
    assert(glint::desktop::uia_adapter(L"Notepad.exe") == UiAdapter::Notepad);
    assert(glint::desktop::uia_adapter(L"sublime_text.exe") == UiAdapter::None);
}

void tab_item_title_uses_the_file_text_not_the_status_suffix() {
    const std::vector<std::wstring> children = {L"pool_96c93713.2fa.txt", L"关闭标签页"};
    assert(glint::desktop::choose_tab_title(
               L"pool_96c93713.2fa.txt. 未修改。", children) ==
           L"pool_96c93713.2fa.txt");
    assert(glint::desktop::choose_tab_title(
               L"notes.txt. 已修改。", {}) == L"notes.txt");
    assert(glint::desktop::choose_tab_title(L"notes.txt - Notepad", {}) == L"notes.txt");
}

void window_targets_have_stable_keys_and_activation_identity() {
    const auto facts = notepad_window();
    const auto first = glint::desktop::make_window_target(facts, L"Notepad");
    const auto second = glint::desktop::make_window_target(facts, L"Notepad");
    assert(first.key == second.key);
    assert(first.key.source == TabSource::Window);
    assert(first.key.tab_id > 0);
    assert(first.title == L"notes.txt - Notepad");
    assert(first.application == L"Notepad");
    assert(first.window_handle == reinterpret_cast<std::uintptr_t>(facts.window));
    assert(first.process_id == facts.process_id);
    assert(first.process_creation_time == facts.process_creation_time);

    auto moved = facts;
    moved.window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(0x5678));
    assert(glint::desktop::make_window_target(moved, L"Notepad").key != first.key);
}

void uia_targets_preserve_runtime_id_and_selected_state() {
    const auto facts = notepad_window();
    const auto target = glint::desktop::make_uia_tab_target(
        facts, L"Notepad", 2, L"notes.txt", true, {42, 77664444, 4, 38});
    assert(target.key.source == TabSource::UiTab);
    assert(target.key.target_id.find(L"u:") == 0);
    assert(target.index == 2);
    assert(target.title == L"notes.txt");
    assert(target.active);
    assert(target.runtime_id == std::vector<int>({42, 77664444, 4, 38}));

    const auto other = glint::desktop::make_uia_tab_target(
        facts, L"Notepad", 3, L"other.txt", false, {42, 77664444, 4, 39});
    assert(target.key != other.key);
}

void desktop_search_matches_titles_applications_and_case() {
    const auto facts = notepad_window();
    std::vector<Tab> targets = {
        glint::desktop::make_window_target(facts, L"Notepad"),
        glint::desktop::make_uia_tab_target(
            facts, L"Notepad", 0, L"Release Notes", false, {1, 2}),
    };
    assert(glint::desktop::search_targets(targets, L"release").size() == 1);
    assert(glint::desktop::search_targets(targets, L"notepad").size() == 2);
    assert(glint::desktop::search_targets(targets, L"notes.txt").size() >= 1);
    assert(glint::desktop::search_targets(targets, L"").size() == 2);
}

void recency_tracks_foreground_transitions_without_refreshing_background_tabs() {
    auto facts = notepad_window();
    auto first = glint::desktop::make_uia_tab_target(facts, L"Notepad", 0, L"First", true, {1});
    auto second = glint::desktop::make_uia_tab_target(facts, L"Notepad", 1, L"Second", false, {2});
    std::vector<Tab> targets{first, second};
    glint::desktop::TargetRecencyTracker history;
    history.update(targets, nullptr, 1000);
    assert(targets[0].last_accessed == 0); // Selected in a background window is not visited.
    history.update(targets, facts.window, 2000);
    assert(targets[0].last_accessed == 2000 && targets[1].last_accessed == 0);
    targets[0].title = L"First renamed";
    history.update(targets, facts.window, 3000);
    assert(targets[0].last_accessed == 2000);
    targets[0].active = false;
    targets[1].active = true;
    history.update(targets, facts.window, 4000);
    assert(targets[0].last_accessed == 2000 && targets[1].last_accessed == 4000);
    history.update(targets, nullptr, 5000);
    history.update(targets, facts.window, 6000);
    assert(targets[1].last_accessed == 6000);

    // A temporary UIA fallback does not erase already observed tab history.
    std::vector<Tab> fallback{glint::desktop::make_window_target(facts, L"Notepad")};
    history.update(fallback, nullptr, 7000);
    targets = {first, second};
    history.update(targets, nullptr, 8000);
    assert(targets[0].last_accessed == 2000 && targets[1].last_accessed == 6000);
    facts.foreground = true;
    fallback = {glint::desktop::make_window_target(facts, L"Notepad")};
    history.update(fallback, facts.window, 9000);
    assert(fallback[0].last_accessed == 9000);
    history.update(fallback, facts.window, 10000);
    assert(fallback[0].last_accessed == 9000);
}

void recent_results_merge_sources_and_keep_unknown_times_unknown() {
    std::vector<Tab> targets;
    for (int i = 8; i >= 1; --i) {
        Tab target;
        target.key = {1, i};
        target.last_accessed = i == 8 ? 0 : i * 1000;
        targets.push_back(target);
    }
    targets[1].key.source = TabSource::Window;
    const auto recent = glint::select_recent_results(targets);
    assert(recent.size() == 6 && recent.front().last_accessed == 7000);
    assert(recent.front().key.source == TabSource::Window);
    assert(recent.back().last_accessed == 2000);
    assert(glint::select_recent_results(targets, 0).empty());
    assert(glint::select_recent_results({}, 6).empty());
    const auto all = glint::select_recent_results(targets, 20);
    assert(all.size() == 8 && all.back().last_accessed == 0);
    for (auto& target : targets) target.last_accessed = 0;
    const auto unknown = glint::select_recent_results(targets, 20);
    std::reverse(targets.begin(), targets.end());
    assert(glint::select_recent_results(targets, 20) == unknown);
    assert(unknown[0].last_accessed == 0);
}

void direct_foreground_observation_survives_an_older_background_scan() {
    auto first_facts = notepad_window();
    auto second_facts = first_facts;
    second_facts.window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(0x5678));
    auto first = glint::desktop::make_window_target(first_facts, L"First app");
    auto second = glint::desktop::make_window_target(second_facts, L"Second app");
    std::vector<Tab> targets{first, second};
    glint::desktop::TargetRecencyTracker history;
    // A live foreground HWND is authoritative even before active flags refresh.
    assert(!targets[1].active);
    history.update(targets, first_facts.window, 1000);
    history.update(targets, second_facts.window, 3000);
    assert(targets[0].last_accessed == 1000 && targets[1].last_accessed == 3000);
    std::vector<Tab> delayed_scan{first, second};
    history.update(delayed_scan, first_facts.window, 2000);
    assert(delayed_scan[0].last_accessed == 1000 && delayed_scan[1].last_accessed == 3000);
    history.update(delayed_scan, second_facts.window, 4000);
    // The delayed scan must not revert the tracked foreground identity.
    assert(delayed_scan[1].last_accessed == 3000);
}

} // namespace

int main() {
    try {
        window_filter_rejects_non_searchable_surfaces();
        application_names_prefer_descriptions_and_have_browser_fallbacks();
        tab_item_title_uses_the_file_text_not_the_status_suffix();
        window_targets_have_stable_keys_and_activation_identity();
        uia_targets_preserve_runtime_id_and_selected_state();
        desktop_search_matches_titles_applications_and_case();
        recency_tracks_foreground_transitions_without_refreshing_background_tabs();
        recent_results_merge_sources_and_keep_unknown_times_unknown();
        direct_foreground_observation_survives_an_older_background_scan();
        std::cout << "desktop target tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << "desktop target test failed: " << error.what() << '\n';
        return 1;
    }
}
