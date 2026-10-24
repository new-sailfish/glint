#pragma once

#include "glint/core.hpp"
#include "ui_theme.hpp"
#include "process_icons.hpp"

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <string>

namespace glint {

inline void draw_application_icon(icons::Icon icon, HDC dc, int x, int y, int size) {
    if (icons::draw_icon(icon, dc, x, y, size)) return;
    // LoadIcon returns a shared USER resource, not an owned Shell extraction.
    if (const HICON placeholder = LoadIconW(nullptr, IDI_APPLICATION))
        DrawIconEx(dc, x, y, placeholder, size, size, 0, nullptr, DI_NORMAL);
}

// Keep the native list-view's selection, hit testing and accessibility in both
// presentations. Only the empty-query tile layout and painting are specialized.
inline int layout_search_cards(HWND list, UINT dpi, int width, int height, std::size_t count) {
    const auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
    const int columns = width >= px(480) ? 2 : 1;
    const int rows = (std::max)(1, (static_cast<int>(count) + columns - 1) / columns);
    const int tile_height = (std::clamp)(height / rows, px(76), px(98));
    const int scrollbar = tile_height * rows > height ? GetSystemMetricsForDpi(SM_CXVSCROLL, dpi) : 0;
    LVTILEVIEWINFO tiles{sizeof(tiles)};
    tiles.dwMask = LVTVIM_TILESIZE | LVTVIM_COLUMNS;
    tiles.dwFlags = LVTVIF_FIXEDSIZE;
    tiles.sizeTile = {(std::max)(px(120), (width - scrollbar - px(2)) / columns), tile_height};
    tiles.cLines = 2;
    ListView_SetTileViewInfo(list, &tiles);
    // Updating tile dimensions leaves the native control's item positions
    // cached across a resize; arrange again when switching column counts.
    ListView_Arrange(list, LVA_DEFAULT);
    return columns;
}

inline LRESULT draw_search_card(NMLVCUSTOMDRAW& draw, const Tab& tab,
                               std::wstring_view source, UINT dpi, HFONT font, HFONT small_font,
                               icons::Icon icon = {}) {
    const auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
    const HWND list = draw.nmcd.hdr.hwndFrom;
    const int item = static_cast<int>(draw.nmcd.dwItemSpec);
    const bool selected = (ListView_GetItemState(list, item, LVIS_SELECTED) & LVIS_SELECTED) != 0;
    const bool contrast = ui::high_contrast();
    const COLORREF background = contrast ? GetSysColor(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW)
        : selected ? ui::wash() : ui::surface();
    const COLORREF ink = contrast ? GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT) : ui::ink();
    const COLORREF muted = contrast ? ink : ui::muted();
    RECT bounds{};
    ListView_GetItemRect(list, item, &bounds, LVIR_BOUNDS);
    HDC dc = draw.nmcd.hdc;
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom);
    FillRect(dc, &bounds, contrast ? GetSysColorBrush(COLOR_WINDOW) : ui::background_brush());
    InflateRect(&bounds, -px(4), -px(4));
    HBRUSH brush = CreateSolidBrush(background);
    HPEN pen = CreatePen(PS_SOLID, (std::max)(1, px(selected ? 2 : 1)),
        contrast ? ink : selected ? ui::accent() : ui::line());
    SelectObject(dc, brush); SelectObject(dc, pen);
    RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom, px(9), px(9));
    SetBkMode(dc, TRANSPARENT);
    RECT content = bounds; InflateRect(&content, -px(12), -px(8));
    SelectObject(dc, small_font);
    SetTextColor(dc, muted);
    RECT label = content; label.bottom = label.top + px(18); label.right -= px(26);
    draw_application_icon(icon, dc, label.left, label.top + px(1), px(16));
    label.left += px(22);
    DrawTextW(dc, source.data(), static_cast<int>(source.size()), &label,
        DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (selected) {
        RECT marker = content; marker.left = marker.right - px(22); marker.bottom = marker.top + px(18);
        SetTextColor(dc, contrast ? ink : ui::accent());
        DrawTextW(dc, L"↵", -1, &marker, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, font);
    SetTextColor(dc, ink);
    RECT title = content; title.top += px(21); title.bottom = title.top + px(22);
    DrawTextW(dc, tab.title.c_str(), -1, &title, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (content.bottom - title.bottom >= px(16)) {
        SelectObject(dc, small_font); SetTextColor(dc, muted);
        RECT detail = content; detail.top = title.bottom + px(2);
        DrawTextW(dc, tab.url.c_str(), -1, &detail, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    if (GetFocus() == list && selected) {
        RECT focus = bounds; InflateRect(&focus, -px(4), -px(4)); DrawFocusRect(dc, &focus);
    }
    RestoreDC(dc, saved);
    DeleteObject(pen); DeleteObject(brush);
    return CDRF_SKIPDEFAULT;
}

} // namespace glint
