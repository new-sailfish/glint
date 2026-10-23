#pragma once
#include "browser_launch.hpp"
#include "glint/core.hpp"
#include <windows.h>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace glint {
class SetupWindow {
public:
    SetupWindow(HWND owner, std::function<void()> show_search);
    ~SetupWindow();
    void show();
    void refresh_language();
    bool translate(MSG& message);
    void update_connection(std::vector<ConnectionInfo> connections, const std::wstring& hotkey);
private:
    struct Caption { const wchar_t* english; const wchar_t* chinese; };
    struct Control {
        HWND window; int x, y, width, height, font, page; bool stretch;
        const wchar_t* english = nullptr;
        const wchar_t* chinese = nullptr;
    };
    struct Feedback { std::wstring english, chinese; };
    HWND owner_, window_ = nullptr, heading_ = nullptr, progress_ = nullptr;
    HWND notice_ = nullptr, primary_ = nullptr, back_ = nullptr, browser_status_ = nullptr;
    HWND address_ = nullptr, developer_hint_ = nullptr, load_hint_ = nullptr;
    HWND connection_ = nullptr, connection_detail_ = nullptr, manual_id_ = nullptr;
    HWND repair_id_ = nullptr, repair_result_ = nullptr;
    HWND illustration_ = nullptr;
    std::function<void()> show_search_;
    std::filesystem::path extension_directory_;
    std::vector<Control> controls_;
    std::vector<ConnectionInfo> connections_;
    browser_launch::Browser browser_ = browser_launch::Browser::Chrome;
    std::optional<browser_launch::Browser> manual_browser_;
    std::optional<browser_launch::Browser> repair_browser_;
    HFONT fonts_[3]{};
    UINT dpi_ = 96;
    int scroll_ = 0, page_ = 0;
    bool manual_flow_ = false, dismissing_ = false;
    std::wstring hotkey_ = L"Ctrl+Shift+Space";
    Feedback notice_feedback_, repair_feedback_;
    static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM);
    bool create();
    HWND add(const wchar_t* type, const wchar_t* text, DWORD style, int id,
             int x, int y, int width, int height, int page, int font = 0, bool stretch = false);
    HWND add(const wchar_t* type, Caption text, DWORD style, int id,
             int x, int y, int width, int height, int page, int font = 0, bool stretch = false);
    void set_feedback(HWND target, std::wstring english, std::wstring chinese);
    void set_failure(HWND target, const std::wstring& detail, Caption retry);
    int px(int value) const;
    void paint(HDC dc) const;
    void set_fonts();
    void layout();
    void scroll_to(int position);
    void ensure_visible(HWND child);
    void command(int id);
    void dismiss();
    void set_page(int page);
    void update_page_text();
    bool prepare();
    void register_manual();
    void register_repair();
    bool copy_text(const std::wstring& text, Caption success);
    void open_extensions();
    void update_status();
    void update_browser_status();
    HWND page_focus() const;
};
} // namespace glint
