#pragma once
#include "update_controller.hpp"
#include <windows.h>

namespace glint {
class UpdateWindow {
public:
    UpdateWindow(HWND owner, std::function<void()> check, std::function<void()> install,
        std::function<void(bool)> automatic);
    ~UpdateWindow();
    void show(const update::Snapshot& state);
    void refresh(const update::Snapshot& state);
    bool translate(MSG& message);
    bool visible() const;
private:
    HWND owner_ = nullptr, window_ = nullptr;
    HWND heading_ = nullptr, versions_ = nullptr, status_ = nullptr, progress_ = nullptr;
    HWND detail_ = nullptr, automatic_ = nullptr, check_ = nullptr, install_ = nullptr, download_ = nullptr;
    HFONT font_ = nullptr, heading_font_ = nullptr;
    UINT dpi_ = 96;
    update::Snapshot state_;
    std::function<void()> check_action_, install_action_;
    std::function<void(bool)> automatic_action_;
    bool create();
    void layout();
    void fonts();
    static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM);
};
} // namespace glint
