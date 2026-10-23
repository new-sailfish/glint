#include "desktop_targets.hpp"

#include "language.hpp"

#include <dwmapi.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace glint::desktop {
namespace {

std::int64_t access_time_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::wstring lower(std::wstring_view input) {
    std::wstring value(input);
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return value;
}

std::wstring trim(std::wstring_view input) {
    std::size_t begin = 0;
    std::size_t end = input.size();
    const auto blank = [](wchar_t character) { return character == L'\0' || iswspace(character); };
    while (begin < end && blank(input[begin])) ++begin;
    while (end > begin && blank(input[end - 1])) --end;
    return std::wstring(input.substr(begin, end - begin));
}

std::wstring file_name(std::wstring_view path) {
    const auto slash = path.find_last_of(L"\\/");
    return std::wstring(slash == std::wstring_view::npos ? path : path.substr(slash + 1));
}

bool ends_with_case_insensitive(std::wstring_view value, std::wstring_view suffix) {
    if (suffix.size() > value.size()) return false;
    return CompareStringOrdinal(value.data() + value.size() - suffix.size(),
               static_cast<int>(suffix.size()), suffix.data(),
               static_cast<int>(suffix.size()), TRUE) == CSTR_EQUAL;
}

bool equal_case_insensitive(std::wstring_view left, std::wstring_view right) {
    return left.size() == right.size() &&
        CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
            right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

std::int64_t stable_key(std::wstring_view identity) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const wchar_t character : identity) {
        hash ^= static_cast<std::uint64_t>(character);
        hash *= 1099511628211ull;
    }
    const auto positive = static_cast<std::int64_t>(hash & 0x7fff'ffff'ffff'ffffull);
    return positive ? positive : 1;
}

std::wstring window_identity(const WindowFacts& facts) {
    return L"w:" + std::to_wstring(facts.process_id) + L":" +
        std::to_wstring(facts.process_creation_time) + L":" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(facts.window));
}

std::wstring uia_identity(const WindowFacts& facts, std::span<const int> runtime_id) {
    return L"u:" + std::to_wstring(facts.process_id) + L":" +
        std::to_wstring(facts.process_creation_time) + L":" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(facts.window)) + L":" +
        runtime_id_text(runtime_id);
}

void note_uia_failure(bool* uia_failure) {
    if (uia_failure) *uia_failure = true;
}

std::vector<int> element_runtime_id(
    IUIAutomationElement* element, bool* uia_failure = nullptr) {
    if (!element) return {};
    SAFEARRAY* array = nullptr;
    if (FAILED(element->GetRuntimeId(&array))) {
        note_uia_failure(uia_failure);
        return {};
    }
    if (!array) return {};
    std::vector<int> result;
    LONG lower_bound = 0;
    LONG upper_bound = -1;
    if (FAILED(SafeArrayGetLBound(array, 1, &lower_bound)) ||
        FAILED(SafeArrayGetUBound(array, 1, &upper_bound))) {
        note_uia_failure(uia_failure);
    } else {
        for (LONG index = lower_bound; index <= upper_bound; ++index) {
            int value = 0;
            if (FAILED(SafeArrayGetElement(array, &index, &value))) {
                note_uia_failure(uia_failure);
                result.clear();
                break;
            }
            result.push_back(value);
        }
    }
    SafeArrayDestroy(array);
    return result;
}

std::wstring element_name(
    IUIAutomationElement* element, bool* uia_failure = nullptr) {
    if (!element) return {};
    BSTR value = nullptr;
    if (FAILED(element->get_CurrentName(&value))) {
        note_uia_failure(uia_failure);
        return {};
    }
    if (!value) return {};
    std::wstring result(value, SysStringLen(value));
    SysFreeString(value);
    return result;
}

bool element_selected(
    IUIAutomationElement* element, bool* uia_failure = nullptr) {
    if (!element) return false;
    VARIANT value;
    VariantInit(&value);
    const HRESULT result = element->GetCurrentPropertyValue(
        UIA_SelectionItemIsSelectedPropertyId, &value);
    if (FAILED(result)) note_uia_failure(uia_failure);
    const bool selected = SUCCEEDED(result) && value.vt == VT_BOOL &&
        value.boolVal != VARIANT_FALSE;
    VariantClear(&value);
    return selected;
}

ComPtr<IUIAutomationCondition> control_type_condition(
    IUIAutomation* automation, CONTROLTYPEID type, bool* uia_failure = nullptr) {
    if (!automation) return {};
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_I4;
    value.lVal = static_cast<LONG>(type);
    ComPtr<IUIAutomationCondition> condition;
    if (FAILED(automation->CreatePropertyCondition(
            UIA_ControlTypePropertyId, value, &condition)) || !condition) {
        note_uia_failure(uia_failure);
    }
    return condition;
}

std::vector<std::wstring> child_text(
    IUIAutomation* automation, IUIAutomationElement* tab,
    bool* uia_failure = nullptr) {
    std::vector<std::wstring> result;
    if (!automation || !tab) return result;
    auto condition = control_type_condition(automation, UIA_TextControlTypeId, uia_failure);
    if (!condition) return result;
    ComPtr<IUIAutomationElementArray> children;
    if (FAILED(tab->FindAll(TreeScope_Descendants, condition.Get(), &children)) || !children) {
        note_uia_failure(uia_failure);
        return result;
    }
    int count = 0;
    if (FAILED(children->get_Length(&count))) {
        note_uia_failure(uia_failure);
        return result;
    }
    for (int index = 0; index < count; ++index) {
        ComPtr<IUIAutomationElement> child;
        if (SUCCEEDED(children->GetElement(index, &child)) && child) {
            auto name = trim(element_name(child.Get(), uia_failure));
            if (!name.empty()) result.push_back(std::move(name));
        } else {
            note_uia_failure(uia_failure);
        }
    }
    return result;
}

ComPtr<IUIAutomation> create_automation() {
    ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&automation)))) return {};
    ComPtr<IUIAutomation2> extended;
    if (SUCCEEDED(automation.As(&extended))) {
        extended->put_ConnectionTimeout(2000);
        extended->put_TransactionTimeout(5000);
    }
    return automation;
}

std::vector<Tab> scan_notepad_tabs(
    IUIAutomation* automation, const WindowFacts& facts, std::wstring_view application,
    bool& uia_failure) {
    std::vector<Tab> result;
    if (!automation) {
        note_uia_failure(&uia_failure);
        return result;
    }
    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation->ElementFromHandle(facts.window, &root)) || !root) {
        note_uia_failure(&uia_failure);
        return result;
    }
    auto condition = control_type_condition(
        automation, UIA_TabItemControlTypeId, &uia_failure);
    if (!condition) return result;
    ComPtr<IUIAutomationElementArray> tabs;
    if (FAILED(root->FindAll(TreeScope_Descendants, condition.Get(), &tabs)) || !tabs) {
        note_uia_failure(&uia_failure);
        return result;
    }
    int count = 0;
    if (FAILED(tabs->get_Length(&count))) {
        note_uia_failure(&uia_failure);
        return result;
    }
    std::map<std::wstring, bool> identities;
    for (int index = 0; index < count; ++index) {
        ComPtr<IUIAutomationElement> tab;
        if (FAILED(tabs->GetElement(index, &tab)) || !tab) {
            note_uia_failure(&uia_failure);
            return {};
        }
        auto runtime_id = element_runtime_id(tab.Get(), &uia_failure);
        if (uia_failure) return {};
        if (runtime_id.empty() || !identities.emplace(runtime_id_text(runtime_id), true).second)
            return {};
        auto children = child_text(automation, tab.Get(), &uia_failure);
        if (uia_failure) return {};
        auto title = choose_tab_title(element_name(tab.Get(), &uia_failure), children);
        if (uia_failure) return {};
        if (title.empty()) return {};
        ComPtr<IUnknown> unknown;
        ComPtr<IUIAutomationSelectionItemPattern> pattern;
        if (FAILED(tab->GetCurrentPattern(UIA_SelectionItemPatternId, &unknown)) ||
            !unknown || FAILED(unknown.As(&pattern)) || !pattern) {
            note_uia_failure(&uia_failure);
            return {};
        }
        const bool selected = element_selected(tab.Get(), &uia_failure);
        if (uia_failure) return {};
        result.push_back(make_uia_tab_target(
            facts, application, static_cast<std::size_t>(index), title,
            selected,
            std::move(runtime_id)));
    }
    return result;
}

std::uint64_t process_creation_time(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) |
        created.dwLowDateTime;
}

std::wstring process_image(HANDLE process) {
    std::wstring image(32768, L'\0');
    DWORD size = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(process, 0, image.data(), &size)) return {};
    image.resize(size);
    return image;
}

std::wstring file_description(const std::filesystem::path& image) {
    if (image.empty()) return {};
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(image.c_str(), &ignored);
    if (!size) return {};
    std::vector<std::byte> data(size);
    if (!GetFileVersionInfoW(image.c_str(), 0, size, data.data())) return {};

    struct Translation { WORD language; WORD code_page; };
    Translation* translations = nullptr;
    UINT bytes = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
            reinterpret_cast<void**>(&translations), &bytes) ||
        bytes < sizeof(Translation) || !translations) return {};

    wchar_t code[9]{};
    swprintf_s(code, L"%04x%04x", translations[0].language, translations[0].code_page);
    const std::wstring key = L"\\StringFileInfo\\" + std::wstring(code) + L"\\FileDescription";
    wchar_t* value = nullptr;
    if (!VerQueryValueW(data.data(), key.c_str(), reinterpret_cast<void**>(&value), &bytes) ||
        !value) return {};
    // VerQueryValue reports character count for StringFileInfo values.
    return trim(std::wstring_view(value, bytes));
}

bool shell_class(std::wstring_view class_name) {
    static constexpr std::wstring_view classes[] = {
        L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
        L"TaskListThumbnailWnd", L"MultitaskingViewFrame", L"ForegroundStaging",
        L"ApplicationManager_DesktopShellWindow",
    };
    for (const auto candidate : classes) {
        if (equal_case_insensitive(class_name, candidate)) return true;
    }
    return false;
}

struct EnumeratedWindow {
    WindowFacts facts;
    std::wstring file_description;
};

struct EnumerationContext {
    DWORD own_process_id = 0;
    HWND shell_window = nullptr;
    HWND foreground_window = nullptr;
    std::unordered_map<std::wstring, struct DescriptionCacheEntry>* descriptions = nullptr;
    std::vector<EnumeratedWindow> windows;
};

struct DescriptionCacheEntry {
    std::filesystem::file_time_type modified{};
    std::uintmax_t size = 0;
    std::wstring description;
};

BOOL CALLBACK enumerate_window(HWND window, LPARAM parameter) {
    auto& context = *reinterpret_cast<EnumerationContext*>(parameter);
    const int title_length = GetWindowTextLengthW(window);
    if (title_length <= 0) return TRUE;
    std::wstring title(static_cast<std::size_t>(title_length) + 1, L'\0');
    GetWindowTextW(window, title.data(), title_length + 1);
    title.resize(static_cast<std::size_t>(title_length));

    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    std::wstring class_name(256, L'\0');
    const int class_length = GetClassNameW(window, class_name.data(), static_cast<int>(class_name.size()));
    if (class_length > 0) class_name.resize(static_cast<std::size_t>(class_length));
    else class_name.clear();

    DWORD cloaked = 0;
    const bool is_cloaked = SUCCEEDED(DwmGetWindowAttribute(
        window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;

    std::wstring image;
    std::uint64_t created = 0;
    if (HANDLE process = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, process_id)) {
        image = process_image(process);
        created = process_creation_time(process);
        CloseHandle(process);
    }

    const auto extended_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    WindowFacts facts{
        .window = window,
        .process_id = process_id,
        .process_creation_time = created,
        .title = std::move(title),
        .image_name = file_name(image),
        .visible = IsWindowVisible(window) != FALSE,
        .cloaked = is_cloaked,
        .tool_window = (extended_style & WS_EX_TOOLWINDOW) != 0 &&
            (extended_style & WS_EX_APPWINDOW) == 0,
        .shell_window = window == context.shell_window || shell_class(class_name),
        .class_name = std::move(class_name),
        .foreground = window == context.foreground_window,
    };
    if (!should_include_window(facts, context.own_process_id)) return TRUE;

    std::wstring description;
    if (!image.empty()) {
        auto& descriptions = *context.descriptions;
        auto found = descriptions.find(image);
        std::error_code time_error;
        std::error_code size_error;
        const auto modified = std::filesystem::last_write_time(image, time_error);
        const auto size = std::filesystem::file_size(image, size_error);
        if (found == descriptions.end() || time_error || size_error ||
            found->second.modified != modified || found->second.size != size) {
            found = descriptions.insert_or_assign(image, DescriptionCacheEntry{
                modified, size, file_description(image)}).first;
        }
        description = found->second.description;
    }
    context.windows.push_back({std::move(facts), std::move(description)});
    return TRUE;
}

struct ScanResult {
    std::vector<Tab> targets;
    bool uia_retryable = false;
    HWND foreground_window = nullptr;
    std::int64_t foreground_observed_at = 0;
};

ScanResult scan_windows(
    IUIAutomation* automation,
    std::unordered_map<std::wstring, DescriptionCacheEntry>& descriptions) {
    EnumerationContext context;
    context.own_process_id = GetCurrentProcessId();
    context.shell_window = GetShellWindow();
    context.foreground_window = GetForegroundWindow();
    const auto foreground_observed_at = access_time_now();
    context.descriptions = &descriptions;
    EnumWindows(enumerate_window, reinterpret_cast<LPARAM>(&context));

    ScanResult result;
    result.foreground_window = context.foreground_window;
    result.foreground_observed_at = foreground_observed_at;
    for (auto& window : context.windows) {
        auto application = application_name(window.facts.image_name, window.file_description);
        if (application.empty()) application = window.facts.class_name;
        if (application.empty()) application = i18n::text(L"Desktop window", L"桌面窗口");

        if (uia_adapter(window.facts.image_name) == UiAdapter::Notepad) {
            bool uia_failure = false;
            auto tabs = scan_notepad_tabs(
                automation, window.facts, application, uia_failure);
            result.uia_retryable |= uia_failure;
            if (!tabs.empty()) {
                result.targets.insert(result.targets.end(), std::make_move_iterator(tabs.begin()),
                    std::make_move_iterator(tabs.end()));
                continue;
            }
        }
        result.targets.push_back(make_window_target(window.facts, application));
    }
    return result;
}

bool runtime_id_matches(
    IUIAutomationElement* element, std::span<const int> expected,
    bool* uia_failure = nullptr) {
    return element_runtime_id(element, uia_failure) ==
        std::vector<int>(expected.begin(), expected.end());
}

std::wstring tab_title(
    IUIAutomation* automation, IUIAutomationElement* element,
    bool* uia_failure = nullptr) {
    return choose_tab_title(
        element_name(element, uia_failure), child_text(automation, element, uia_failure));
}

bool select_uia_tab(IUIAutomation* automation, const Tab& target,
    std::wstring& error, bool& uia_retryable) {
    ComPtr<IUIAutomationElement> root;
    if (!automation || FAILED(automation->ElementFromHandle(
            reinterpret_cast<HWND>(target.window_handle), &root)) || !root) {
        uia_retryable = true;
        error = i18n::text(L"The application window is not accessible.",
            L"无法访问应用窗口。");
        return false;
    }
    auto condition = control_type_condition(automation, UIA_TabItemControlTypeId);
    ComPtr<IUIAutomationElementArray> tabs;
    if (!condition ||
        FAILED(root->FindAll(TreeScope_Descendants, condition.Get(), &tabs)) || !tabs) {
        uia_retryable = true;
        error = i18n::text(L"The application no longer exposes its tabs.",
            L"应用当前无法提供标签页。");
        return false;
    }
    int count = 0;
    if (FAILED(tabs->get_Length(&count))) {
        uia_retryable = true;
        error = i18n::text(L"The application did not provide its tab list.",
            L"应用未能提供标签页列表。");
        return false;
    }
    ComPtr<IUIAutomationElement> selected;
    if (!target.runtime_id.empty()) {
        for (int index = 0; index < count; ++index) {
            ComPtr<IUIAutomationElement> candidate;
            if (FAILED(tabs->GetElement(index, &candidate)) || !candidate) {
                uia_retryable = true;
                break;
            }
            if (runtime_id_matches(candidate.Get(), target.runtime_id, &uia_retryable)) {
                selected = std::move(candidate);
                break;
            }
            if (uia_retryable) break;
        }
    }
    if (uia_retryable) {
        error = i18n::text(L"The application did not respond while switching tabs.",
            L"切换标签页时应用没有响应。");
        return false;
    }
    if (!selected && target.index >= 0 && target.index < count) {
        ComPtr<IUIAutomationElement> candidate;
        if (FAILED(tabs->GetElement(static_cast<int>(target.index), &candidate)) || !candidate) {
            uia_retryable = true;
        } else if (equal_case_insensitive(
                       tab_title(automation, candidate.Get(), &uia_retryable), target.title)) {
            selected = std::move(candidate);
        }
    }
    if (uia_retryable) {
        error = i18n::text(L"The application did not respond while switching tabs.",
            L"切换标签页时应用没有响应。");
        return false;
    }
    if (!selected) {
        for (int index = 0; index < count; ++index) {
            ComPtr<IUIAutomationElement> candidate;
            if (FAILED(tabs->GetElement(index, &candidate)) || !candidate) {
                uia_retryable = true;
                break;
            }
            if (equal_case_insensitive(
                    tab_title(automation, candidate.Get(), &uia_retryable), target.title)) {
                selected = std::move(candidate);
                break;
            }
            if (uia_retryable) break;
        }
    }
    if (uia_retryable) {
        error = i18n::text(L"The application did not respond while switching tabs.",
            L"切换标签页时应用没有响应。");
        return false;
    }
    if (!selected) {
        error = i18n::text(L"The selected tab is no longer available.",
            L"选中的标签页已不可用。");
        return false;
    }
    ComPtr<IUnknown> unknown;
    ComPtr<IUIAutomationSelectionItemPattern> pattern;
    if (FAILED(selected->GetCurrentPattern(UIA_SelectionItemPatternId, &unknown)) ||
        !unknown || FAILED(unknown.As(&pattern)) || !pattern || FAILED(pattern->Select())) {
        uia_retryable = true;
        error = i18n::text(L"The application did not accept the tab selection.",
            L"应用未能切换标签页。");
        return false;
    }
    return true;
}

bool activate_window(const Tab& target, std::wstring& error) {
    HWND window = reinterpret_cast<HWND>(target.window_handle);
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (!IsWindow(window) || !process_id || process_id != target.process_id) {
        error = i18n::text(L"The selected window is no longer available.",
            L"选中的窗口已不可用。");
        return false;
    }
    if (target.process_creation_time) {
        if (HANDLE process = OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, process_id)) {
            const auto created = process_creation_time(process);
            CloseHandle(process);
            if (created && created != target.process_creation_time) {
                error = i18n::text(L"The selected window was replaced.",
                    L"选中的窗口已发生变化。");
                return false;
            }
        }
    }

    if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
    BringWindowToTop(window);
    SetForegroundWindow(window);
    for (int attempt = 0; attempt < 8; ++attempt) {
        const HWND foreground = GetForegroundWindow();
        if (foreground == window || GetAncestor(foreground, GA_ROOT) == window) return true;
        Sleep(50);
    }
    error = i18n::text(L"The window did not come to the foreground. Please try again.",
        L"窗口未能切到前台，请重试。");
    return false;
}

bool activate_target(IUIAutomation* automation, const Tab& target,
    std::wstring& error, bool& uia_retryable) {
    if (target.key.source == TabSource::UiTab &&
        !select_uia_tab(automation, target, error, uia_retryable))
        return false;
    return activate_window(target, error);
}

} // namespace

bool is_connected_browser_image(std::wstring_view image_name) {
    const auto name = lower(file_name(image_name));
    return name == L"chrome.exe" || name == L"msedge.exe";
}

bool should_include_window(const WindowFacts& facts, DWORD own_process_id) {
    if (!facts.window || !facts.visible || facts.cloaked || facts.tool_window ||
        facts.shell_window || shell_class(facts.class_name) || facts.title.empty() || !facts.process_id ||
        facts.process_id == own_process_id) return false;
    const auto image = lower(file_name(facts.image_name));
    return !is_connected_browser_image(image) &&
        image != L"glintbridge.exe" && image != L"textinputhost.exe";
}

UiAdapter uia_adapter(std::wstring_view image_name) {
    if (lower(file_name(image_name)) == L"notepad.exe") return UiAdapter::Notepad;
    return UiAdapter::None;
}

std::wstring application_name(
    std::wstring_view image_name, std::wstring_view file_description) {
    auto description = trim(file_description);
    if (!description.empty()) return description;
    const auto name = lower(file_name(image_name));
    if (name == L"notepad.exe") return L"Notepad";
    if (name == L"sublime_text.exe") return L"Sublime Text";
    if (name == L"code.exe") return L"Visual Studio Code";
    auto fallback = file_name(image_name);
    if (fallback.size() > 4 && ends_with_case_insensitive(fallback, L".exe"))
        fallback.resize(fallback.size() - 4);
    return trim(fallback);
}

std::wstring choose_tab_title(
    std::wstring_view accessible_name, std::span<const std::wstring> child_text_values) {
    for (const auto& candidate : child_text_values) {
        auto value = trim(candidate);
        if (!value.empty() && !equal_case_insensitive(value, L"关闭标签页") &&
            !equal_case_insensitive(value, L"Close tab")) return value;
    }

    std::wstring value = trim(accessible_name);
    static constexpr std::wstring_view suffixes[] = {
        L" - Notepad", L". 未修改。", L". 已修改。", L". Unmodified.", L". Modified.",
    };
    for (const auto suffix : suffixes) {
        if (ends_with_case_insensitive(value, suffix)) {
            value.resize(value.size() - suffix.size());
            if (!value.empty() && value.back() == L'.') value.pop_back();
            break;
        }
    }
    while (!value.empty() && (value.back() == L'*' || iswspace(value.back()))) value.pop_back();
    return value;
}

std::wstring runtime_id_text(std::span<const int> runtime_id) {
    std::wstring result;
    for (const int part : runtime_id) {
        if (!result.empty()) result.push_back(L',');
        result += std::to_wstring(part);
    }
    return result;
}

Tab make_window_target(const WindowFacts& facts, std::wstring_view application) {
    const auto identity = window_identity(facts);
    Tab target;
    target.key = {0, stable_key(identity), TabSource::Window, identity};
    target.index = 0;
    target.title = facts.title;
    target.url = std::wstring(application);
    target.browser = std::wstring(application);
    target.application = std::wstring(application);
    target.active = facts.foreground;
    target.window_handle = reinterpret_cast<std::uintptr_t>(facts.window);
    target.process_id = facts.process_id;
    target.process_creation_time = facts.process_creation_time;
    return target;
}

Tab make_uia_tab_target(
    const WindowFacts& facts, std::wstring_view application, std::size_t index,
    std::wstring_view title, bool active, std::vector<int> runtime_id) {
    const auto identity = uia_identity(facts, runtime_id);
    Tab target;
    target.key = {0, stable_key(identity), TabSource::UiTab, identity};
    target.index = static_cast<std::int64_t>(index);
    target.title = std::wstring(title);
    target.url = facts.title;
    target.browser = std::wstring(application);
    target.application = std::wstring(application);
    target.active = active;
    target.window_handle = reinterpret_cast<std::uintptr_t>(facts.window);
    target.process_id = facts.process_id;
    target.process_creation_time = facts.process_creation_time;
    target.runtime_id = std::move(runtime_id);
    return target;
}

std::vector<Tab> search_targets(
    std::span<const Tab> targets, std::wstring_view query, std::size_t limit) {
    const auto needle = lower(query);
    std::vector<Tab> result;
    if (!limit) return result;
    for (const auto& target : targets) {
        if (needle.empty() || lower(target.title).find(needle) != std::wstring::npos ||
            lower(target.url).find(needle) != std::wstring::npos ||
            lower(target.application).find(needle) != std::wstring::npos) {
            result.push_back(target);
            if (result.size() >= limit) break;
        }
    }
    return result;
}

void TargetRecencyTracker::update(
    std::span<Tab> targets, HWND foreground_window, std::int64_t now) {
    std::optional<TabKey> foreground;
    // A scan can finish after a more recent direct foreground observation.
    // Merge its rows with history without letting its old focus sample win.
    if (now >= latest_observation_) {
        for (auto& target : targets) {
            if (foreground_window && (target.key.source == TabSource::Window || target.active) &&
                target.window_handle == reinterpret_cast<std::uintptr_t>(foreground_window)) {
                foreground = target.key;
                if (foreground != foreground_target_ && now > 0) {
                    auto& previous = access_times_[target.key.target_id];
                    previous = (std::max)(previous, now);
                }
                break;
            }
        }
        foreground_target_ = std::move(foreground);
        latest_observation_ = now;
    }
    for (auto& target : targets) {
        const auto found = access_times_.find(target.key.target_id);
        target.last_accessed = found == access_times_.end() ? 0 : found->second;
    }
}

class DesktopTargetService::Impl : public std::enable_shared_from_this<DesktopTargetService::Impl> {
public:
    ~Impl() { stop(); }

    void start(HWND window, UINT message) {
        if (thread_.joinable()) return;
        {
            std::lock_guard lock(mutex_);
            running_ = true;
            finished_ = false;
            notification_window_ = window;
            notification_message_ = message;
        }
        thread_ = std::thread([self = shared_from_this()] { self->run(); });
    }

    void stop() {
        {
            std::lock_guard lock(mutex_);
            running_ = false;
        }
        activation_ready_.notify_all();
        if (thread_.joinable()) {
            std::unique_lock lock(mutex_);
            if (finished_changed_.wait_for(lock, std::chrono::milliseconds(250), [this] {
                    return finished_;
                })) {
                lock.unlock();
                thread_.join();
            } else {
                // A cross-process UI Automation call can outlive its configured
                // timeout. The worker owns this Impl through shared_ptr, so it
                // may finish after the UI is gone without leaving a dangling
                // callback. User exit must not wait indefinitely for UIA.
                thread_.detach();
            }
        }
        {
            std::lock_guard lock(mutex_);
            notification_window_ = nullptr;
        }
    }

    std::vector<Tab> search(std::wstring_view query, std::size_t limit) const {
        std::vector<Tab> targets;
        {
            std::lock_guard lock(mutex_);
            targets = targets_;
        }
        return search_targets(targets, query, limit);
    }

    std::size_t target_count() const {
        std::lock_guard lock(mutex_);
        return targets_.size();
    }

    void observe_foreground(HWND foreground_window) {
        const auto observed_at = access_time_now();
        std::lock_guard lock(mutex_);
        recency_.update(targets_, foreground_window, observed_at);
    }

    bool request_activation(const Tab& target) {
        {
            std::lock_guard lock(mutex_);
            if (!running_ || target.key.source == TabSource::Browser ||
                activations_.size() >= 8) return false;
            activations_.push_back(target);
        }
        activation_ready_.notify_one();
        return true;
    }

    std::vector<std::wstring> take_activation_errors() {
        std::vector<std::wstring> errors;
        std::lock_guard lock(mutex_);
        errors.swap(activation_errors_);
        return errors;
    }

private:
    void publish(ScanResult next) {
        bool changed = false;
        HWND notification_window = nullptr;
        {
            std::lock_guard lock(mutex_);
            recency_.update(next.targets, next.foreground_window, next.foreground_observed_at);
            if (next.targets != targets_) {
                targets_ = std::move(next.targets);
                changed = true;
            }
            notification_window = notification_window_;
        }
        if (changed && notification_window)
            PostMessageW(notification_window, notification_message_, 0, 0);
    }

    void process_activations(ComPtr<IUIAutomation>& automation) {
        std::deque<Tab> pending;
        {
            std::lock_guard lock(mutex_);
            pending.swap(activations_);
        }
        if (pending.empty()) return;
        bool report = false;
        for (const auto& target : pending) {
            std::wstring error;
            bool uia_retryable = false;
            bool activated = activate_target(
                automation.Get(), target, error, uia_retryable);
            if (!activated && uia_retryable) {
                automation.Reset();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                automation = create_automation();
                error.clear();
                uia_retryable = false;
                activated = activate_target(
                    automation.Get(), target, error, uia_retryable);
            }
            if (!activated) {
                std::lock_guard lock(mutex_);
                activation_errors_.push_back(std::move(error));
                report = true;
            }
        }
        if (report && notification_window_)
            PostMessageW(notification_window_, notification_message_, 0, 0);
    }

    void run() {
        const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto automation = create_automation();
        std::unordered_map<std::wstring, DescriptionCacheEntry> descriptions;
        while (true) {
            if (!automation) automation = create_automation();
            process_activations(automation);
            auto next = scan_windows(automation.Get(), descriptions);
            if (next.uia_retryable) {
                automation.Reset();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                automation = create_automation();
                if (automation) next = scan_windows(automation.Get(), descriptions);
            }
            publish(std::move(next));
            std::unique_lock lock(mutex_);
            if (!running_) break;
            activation_ready_.wait_for(lock, std::chrono::seconds(1), [this] {
                return !running_ || !activations_.empty();
            });
        }
        automation.Reset();
        if (SUCCEEDED(com_result)) CoUninitialize();
        {
            std::lock_guard lock(mutex_);
            finished_ = true;
        }
        finished_changed_.notify_all();
    }

    HWND notification_window_ = nullptr;
    UINT notification_message_ = 0;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable activation_ready_;
    std::condition_variable finished_changed_;
    std::vector<Tab> targets_;
    TargetRecencyTracker recency_;
    std::deque<Tab> activations_;
    std::vector<std::wstring> activation_errors_;
    bool running_ = false;
    bool finished_ = false;
};

DesktopTargetService::DesktopTargetService() : impl_(std::make_shared<Impl>()) {}
DesktopTargetService::~DesktopTargetService() { impl_->stop(); }
void DesktopTargetService::start(HWND window, UINT message) { impl_->start(window, message); }
void DesktopTargetService::stop() { impl_->stop(); }
std::vector<Tab> DesktopTargetService::search(std::wstring_view query, std::size_t limit) const {
    return impl_->search(query, limit);
}
std::size_t DesktopTargetService::target_count() const { return impl_->target_count(); }
void DesktopTargetService::observe_foreground(HWND foreground_window) { impl_->observe_foreground(foreground_window); }
bool DesktopTargetService::request_activation(const Tab& target) {
    return impl_->request_activation(target);
}
std::vector<std::wstring> DesktopTargetService::take_activation_errors() {
    return impl_->take_activation_errors();
}

} // namespace glint::desktop
