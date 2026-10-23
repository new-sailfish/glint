#pragma once
#include <windows.h>
#include "ui_theme.hpp"
#include "language.hpp"

namespace glint::setup {
namespace illustration_detail {
constexpr COLORREF ink = RGB(34, 48, 67);
constexpr COLORREF muted = RGB(83, 101, 123);
constexpr COLORREF line = RGB(215, 225, 236);
constexpr COLORREF blue = RGB(44, 97, 189);
constexpr COLORREF blue_wash = RGB(234, 241, 255);
constexpr COLORREF white = RGB(255, 255, 255);
constexpr COLORREF green = RGB(35, 124, 86);
constexpr COLORREF background = RGB(245, 248, 253);

// All coordinates below describe a 632 x 180 logical canvas. The caller's
// device context, selected objects, mapping, and clipping are restored.
class Painter {
public:
    explicit Painter(HDC dc, HFONT base) : dc_(dc) {
        LOGFONTW face{};
        if (!base || !GetObjectW(base, sizeof(face), &face)) {
            face.lfCharSet = DEFAULT_CHARSET;
            lstrcpyW(face.lfFaceName, L"Segoe UI");
        }
        face.lfWidth = 0;
        face.lfEscapement = face.lfOrientation = 0;
        face.lfQuality = CLEARTYPE_QUALITY;
        face.lfHeight = -12;
        face.lfWeight = FW_NORMAL;
        body_ = CreateFontIndirectW(&face);
        face.lfHeight = -11;
        small_ = CreateFontIndirectW(&face);
        face.lfHeight = -13;
        face.lfWeight = FW_SEMIBOLD;
        strong_ = CreateFontIndirectW(&face);
    }
    ~Painter() {
        if (body_) DeleteObject(body_);
        if (small_) DeleteObject(small_);
        if (strong_) DeleteObject(strong_);
    }
    Painter(const Painter&) = delete;
    Painter& operator=(const Painter&) = delete;

    void box(int x, int y, int width, int height, COLORREF fill = white,
             COLORREF border = line, int radius = 6) const {
        HBRUSH brush = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 1, border);
        HGDIOBJ old_brush = SelectObject(dc_, brush);
        HGDIOBJ old_pen = SelectObject(dc_, pen);
        RoundRect(dc_, x, y, x + width, y + height, radius, radius);
        SelectObject(dc_, old_pen);
        SelectObject(dc_, old_brush);
        DeleteObject(pen);
        DeleteObject(brush);
    }
    void text(const wchar_t* value, int x, int y, int width, int height,
              COLORREF color = ink, int emphasis = 0,
              UINT format = DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX) const {
        HFONT font = emphasis == 2 ? strong_ : (emphasis == 1 ? small_ : body_);
        HGDIOBJ old = font ? SelectObject(dc_, font) : nullptr;
        RECT rect{x, y, x + width, y + height};
        SetTextColor(dc_, color);
        DrawTextW(dc_, value, -1, &rect, format);
        if (old) SelectObject(dc_, old);
    }
    void stroke(int x1, int y1, int x2, int y2, COLORREF color = line,
                int width = 1) const {
        HPEN pen = CreatePen(PS_SOLID, width, color);
        HGDIOBJ old = SelectObject(dc_, pen);
        MoveToEx(dc_, x1, y1, nullptr);
        LineTo(dc_, x2, y2);
        SelectObject(dc_, old);
        DeleteObject(pen);
    }
    void circle(int x, int y, int size, COLORREF fill) const {
        HBRUSH brush = CreateSolidBrush(fill);
        HGDIOBJ old_brush = SelectObject(dc_, brush);
        HGDIOBJ old_pen = SelectObject(dc_, GetStockObject(NULL_PEN));
        Ellipse(dc_, x, y, x + size, y + size);
        SelectObject(dc_, old_pen);
        SelectObject(dc_, old_brush);
        DeleteObject(brush);
    }
    void browser(bool edge, bool address_highlight = false) const {
        box(20, 38, 394, 126, white);
        box(21, 39, 392, 34, background, background);
        stroke(21, 74, 413, 74);
        stroke(35, 54, 41, 54, muted);
        stroke(35, 54, 39, 50, muted);
        stroke(35, 54, 39, 58, muted);
        box(52, 44, 308, 23, address_highlight ? blue_wash : white,
            address_highlight ? blue : line, 10);
        text(edge ? L"edge://extensions/" : L"chrome://extensions/",
            62, 48, 284, 17, address_highlight ? blue : muted, 1);
        circle(375, 47, 17, blue_wash);
        text(L"P", 375, 47, 17, 17, blue, 1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    void callout(const wchar_t* title, const wchar_t* detail) const {
        text(title, 438, 64, 178, 25, blue, 2);
        text(detail, 438, 94, 174, 62, muted);
    }
    void arrow(int x1, int y1, int x2, int y2, COLORREF color = blue) const {
        stroke(x1, y1, x2, y2, color, 2);
        const int direction = x2 >= x1 ? -1 : 1;
        stroke(x2, y2, x2 + direction * 5, y2 - 4, color, 2);
        stroke(x2, y2, x2 + direction * 5, y2 + 4, color, 2);
    }
    void toggle(int x, int y) const {
        box(x, y, 28, 15, blue, blue, 15);
        circle(x + 15, y + 3, 9, white);
    }
    void folder(int x, int y, COLORREF color = RGB(227, 173, 62)) const {
        box(x, y, 15, 8, color, color, 3);
        box(x, y + 5, 31, 22, RGB(248, 213, 130), color, 3);
    }
    void choose(bool edge) const {
        browser(edge);
        text(edge ? L"Microsoft Edge" : L"Google Chrome", 40, 86, 230, 22, ink, 2);
        box(40, 118, 189, 29, blue_wash, blue);
        circle(51, 125, 14, RGB(212, 226, 252));
        text(i18n::text(L"Current profile", L"当前配置文件"), 75, 125, 140, 19, blue);
        callout(i18n::text(L"Choose a profile", L"选好浏览器和配置文件"), i18n::text(L"Complete the next steps in the profile you want to connect.", L"在你想连接的配置文件中\n完成后面的操作。"));
    }
    void address(bool edge) const {
        browser(edge, true);
        text(i18n::text(L"Extensions", L"扩展管理"), 42, 90, 270, 23, ink, 2);
        stroke(42, 123, 300, 123);
        stroke(42, 138, 240, 138);
        callout(i18n::text(L"Paste the address", L"粘贴到浏览器地址栏"), i18n::text(L"Press Ctrl+L, Ctrl+V,\nthen Enter.", L"按 Ctrl+L、Ctrl+V，\n再按回车。"));
        arrow(429, 53, 366, 53);
    }
    void developer(bool edge) const {
        browser(edge);
        if (edge) {
            box(29, 81, 147, 73, background, background);
            text(i18n::text(L"Extensions", L"扩展"), 40, 88, 100, 18, ink, 2);
            box(35, 122, 135, 26, blue_wash, blue);
            text(i18n::text(L"Developer mode", L"开发人员模式"), 42, 128, 92, 17, blue, 1);
            toggle(136, 128);
            text(i18n::text(L"Installed extensions", L"已安装的扩展"), 182, 92, 208, 24, ink, 2);
            box(182, 121, 121, 26);
            text(i18n::text(L"Load unpacked", L"加载解压缩的扩展"), 188, 127, 111, 18, muted, 1);
            callout(i18n::text(L"Enable developer mode", L"打开开发人员模式"), i18n::text(L"Edge: find the switch in the left sidebar.", L"Edge：开关在页面\n左侧边栏。"));
        } else {
            text(i18n::text(L"Extensions", L"扩展程序"), 40, 91, 130, 23, ink, 2);
            box(261, 84, 141, 30, blue_wash, blue);
            text(i18n::text(L"Developer mode", L"开发者模式"), 270, 92, 96, 19, blue, 1);
            toggle(365, 91);
            box(40, 125, 144, 27);
            text(i18n::text(L"Load unpacked", L"加载已解压的扩展程序"), 47, 132, 132, 17, muted, 1);
            callout(i18n::text(L"Enable developer mode", L"打开开发者模式"), i18n::text(L"Chrome: find the switch at the top right.", L"Chrome：开关在页面\n右上角。"));
            arrow(430, 99, 408, 99);
        }
    }
    void load() const {
        box(20, 38, 394, 126);
        text(i18n::text(L"Select the extension folder", L"选择扩展文件夹"), 35, 46, 330, 20, ink, 2);
        stroke(21, 72, 413, 72);
        box(29, 80, 80, 36, background, background);
        text(i18n::text(L"This PC", L"此电脑"), 39, 89, 65, 20, muted, 1);
        folder(125, 83);
        text(L"browser-extension", 169, 91, 230, 21, ink, 1);
        text(i18n::text(L"Folder:", L"文件夹："), 31, 130, 55, 20, muted, 1);
        box(84, 123, 211, 29, blue_wash, blue);
        text(L"…\\Glint\\browser-extension", 91, 131, 198, 18, blue, 1,
            DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        box(305, 123, 96, 29, blue, blue);
        text(i18n::text(L"Select Folder", L"选择文件夹"), 310, 130, 86, 18, white, 1, DT_CENTER | DT_TOP | DT_SINGLELINE);
        callout(i18n::text(L"Paste the folder path", L"粘贴路径并确认"), i18n::text(L"Paste into “Folder” at the bottom, then click “Select Folder”.", L"在底部“文件夹”框粘贴，\n直接点击“选择文件夹”。"));
        arrow(430, 137, 407, 137);
    }
    void connection(bool edge, bool connected) const {
        box(43, 61, 148, 78);
        text(edge ? L"Microsoft Edge" : L"Google Chrome", 50, 78, 134, 22, ink, 2,
            DT_CENTER | DT_TOP | DT_SINGLELINE);
        text(i18n::text(L"Current profile", L"当前配置文件"), 51, 107, 132, 18, muted, 1, DT_CENTER | DT_TOP | DT_SINGLELINE);
        const COLORREF state_color = connected ? green : muted;
        arrow(205, 101, 262, 101, state_color);
        box(277, 61, 127, 78, connected ? RGB(235, 247, 240) : background,
            connected ? RGB(168, 210, 185) : line);
        POINT logo_points[] = {{298, 77}, {318, 97}};
        LPtoDP(dc_, logo_points, 2);
        RECT logo{logo_points[0].x, logo_points[0].y, logo_points[1].x, logo_points[1].y};
        ui::paint_logo(dc_, logo);
        text(L"Glint", 326, 78, 57, 23, ink, 2, DT_LEFT | DT_TOP | DT_SINGLELINE);
        if (connected) {
            stroke(323, 114, 330, 121, green, 3);
            stroke(330, 121, 342, 107, green, 3);
        } else {
            circle(325, 109, 5, muted);
            circle(337, 109, 5, muted);
            circle(349, 109, 5, muted);
        }
        callout(connected ? i18n::text(L"Connected", L"连接已完成") : i18n::text(L"Waiting for connection", L"等待扩展连接"),
            connected ? i18n::text(L"Tabs sync automatically.\nYou can start searching.", L"标签页会自动同步。\n现在可以开始搜索。") : i18n::text(L"Keep your browser open after loading the extension. Glint detects it automatically.", L"加载扩展后保持浏览器\n打开，Glint 会自动检测。"));
    }
    void manual(bool edge) const {
        browser(edge);
        text(i18n::text(L"Glint · Extension details", L"Glint · 扩展详情"), 41, 86, 300, 23, ink, 2);
        text(i18n::text(L"ID", L"扩展 ID"), 41, 119, 62, 20, muted, 1);
        box(102, 113, 296, 31, blue_wash, blue);
        text(i18n::text(L"abcdefghijkl… (32 characters)", L"abcdefghijkl…（32 位）"), 111, 122, 277, 20, blue, 1);
        callout(i18n::text(L"Copy the extension ID", L"复制扩展详情中的 ID"), i18n::text(L"For manual connection only.\nCopy the full 32-character ID.", L"仅用于手动连接。\n复制完整的 32 位 ID。"));
    }
private:
    HDC dc_;
    HFONT body_ = nullptr, small_ = nullptr, strong_ = nullptr;
};
} // namespace illustration_detail

// Steps: 0 browser/profile, 1 address bar, 2 developer mode, 3 folder
// selection, 4 connection state, 5 manual connection, 6 troubleshooting.
inline void paint_illustration(HDC dc, RECT bounds, int step, bool edge,
                               bool connected, HFONT font = nullptr) {
    if (!dc || bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
    const int saved = SaveDC(dc);
    if (!saved) return;
    IntersectClipRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom);
    SetMapMode(dc, MM_ANISOTROPIC);
    SetWindowOrgEx(dc, 0, 0, nullptr);
    SetWindowExtEx(dc, 632, 180, nullptr);
    SetViewportOrgEx(dc, bounds.left, bounds.top, nullptr);
    SetViewportExtEx(dc, bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr);
    SetBkMode(dc, TRANSPARENT);
    {
        illustration_detail::Painter painter(dc, font);
        painter.box(0, 0, 632, 180, illustration_detail::white, illustration_detail::line, 10);
        painter.text(i18n::text(L"How it works", L"操作示意"), 15, 10, 180, 20, illustration_detail::muted, 1);
        switch (step) {
        case 0: painter.choose(edge); break;
        case 1: painter.address(edge); break;
        case 2: painter.developer(edge); break;
        case 3: painter.load(); break;
        case 5: painter.manual(edge); break;
        default: painter.connection(edge, connected); break;
        }
    }
    RestoreDC(dc, saved);
}
} // namespace glint::setup
