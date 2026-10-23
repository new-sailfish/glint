#pragma once

#include "hotkey.hpp"

#include <windows.h>

#include <functional>
#include <string>

namespace glint {

class HotkeyWindow {
public:
    using Apply = std::function<bool(hotkey::Binding, std::wstring&)>;
    HotkeyWindow(HWND owner, Apply apply);
    ~HotkeyWindow();
    HotkeyWindow(const HotkeyWindow&) = delete;
    HotkeyWindow& operator=(const HotkeyWindow&) = delete;
    void show(hotkey::Binding current);
    void refresh_language();
    bool translate(MSG& message);
    void record_registered(hotkey::Binding binding);
    [[nodiscard]] bool visible() const;

private:
    HWND owner_ = nullptr, window_ = nullptr, recorder_ = nullptr;
    HWND heading_ = nullptr, instruction_ = nullptr, label_ = nullptr, hint_ = nullptr;
    HWND error_ = nullptr, defaults_ = nullptr, save_ = nullptr, cancel_ = nullptr;
    HFONT font_ = nullptr, heading_font_ = nullptr;
    UINT dpi_ = 96;
    hotkey::Binding draft_{};
    hotkey::Binding before_modifiers_{};
    bool recording_modifiers_ = false, apply_failed_ = false;
    Apply apply_;
    static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM);
    bool create();
    void paint(HDC dc) const;
    void layout();
    void update_fonts();
    void update_draft();
    void record(UINT key, bool down);
    void save();
    void dismiss();
};

} // namespace glint
