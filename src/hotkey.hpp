#pragma once

#include <windows.h>

#include <string>

namespace glint::hotkey {

struct Binding {
    UINT modifiers = MOD_CONTROL | MOD_SHIFT;
    UINT key = VK_SPACE;
    bool operator==(const Binding&) const = default;
};

inline constexpr Binding kDefault{};

bool valid(Binding binding);
std::wstring name(Binding binding);
Binding load();
bool save(Binding binding, std::wstring& error);

class Registration {
public:
    Registration() = default;
    explicit Registration(HWND window) : window_(window) {}
    ~Registration();
    Registration(const Registration&) = delete;
    Registration& operator=(const Registration&) = delete;

    Binding current() const { return current_; }
    int id() const { return id_; }
    bool active() const { return active_; }

    bool start(HWND window, Binding binding, std::wstring& error);
    bool change(Binding binding, std::wstring& error);
    void stop();

private:
    HWND window_ = nullptr;
    Binding current_{};
    int id_ = 1;
    bool active_ = false;
};

} // namespace glint::hotkey
