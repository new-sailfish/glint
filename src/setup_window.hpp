#pragma once

#include <windows.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace glint {

// A modeless native window: installing a browser never blocks the search UI.
class SetupWindow {
public:
    SetupWindow(HWND owner, std::function<void()> show_search);
    ~SetupWindow();
    void show();
    bool translate(MSG& message);
    void update_connection(std::size_t profiles, std::size_t tabs, const std::wstring& hotkey);

private:
    struct Control {
        HWND window;
        int x, y, width, height;
        int font;
        bool stretch;
    };
    HWND owner_;
    HWND window_ = nullptr;
    HWND chrome_id_ = nullptr;
    HWND edge_id_ = nullptr;
    HWND result_ = nullptr;
    HWND connection_ = nullptr;
    HWND search_ = nullptr;
    std::function<void()> show_search_;
    std::filesystem::path extension_directory_;
    std::vector<Control> controls_;
    HFONT fonts_[3]{};
    UINT dpi_ = 96;
    int scroll_ = 0;
    std::size_t profiles_ = 0;
    std::size_t tabs_ = 0;
    std::wstring hotkey_ = L"Ctrl+Shift+Space";

    static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM);
    bool create();
    HWND add(const wchar_t* type, const wchar_t* text, DWORD style, int id,
             int x, int y, int width, int height, int font = 0, bool stretch = false);
    int px(int value) const;
    void set_fonts();
    void layout();
    void scroll_to(int position);
    void ensure_visible(HWND child);
    void command(int id);
    void dismiss();
    void register_ids();
    void copy_text(const std::wstring& text);
    void open(const wchar_t* path, const wchar_t* parameters = nullptr);
    void update_status();
};

} // namespace glint
