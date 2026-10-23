#include "desktop_targets.hpp"

#include <windows.h>

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

} // namespace

int main() {
    try {
        window_filter_rejects_non_searchable_surfaces();
        application_names_prefer_descriptions_and_have_browser_fallbacks();
        tab_item_title_uses_the_file_text_not_the_status_suffix();
        window_targets_have_stable_keys_and_activation_identity();
        uia_targets_preserve_runtime_id_and_selected_state();
        desktop_search_matches_titles_applications_and_case();
        std::cout << "desktop target tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << "desktop target test failed: " << error.what() << '\n';
        return 1;
    }
}
