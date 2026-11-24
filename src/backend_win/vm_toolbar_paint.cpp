/*
 * vm_toolbar_paint.cpp -- the VM window's toolbar, drawn like a WinUI
 * command bar. See vm_toolbar_paint.h.
 *
 * Colours are WinUI's dark-theme tokens (TextFillColorPrimary/Secondary,
 * SubtleFillColorSecondary/Tertiary, DividerStrokeColorDefault,
 * AccentFillColorDefault...), sizes are WinUI's compact command bar (40 x 40
 * buttons, 4 px corners, 16 px glyphs, Segoe UI Variable 14 px text).
 */

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <math.h>

#include "vm_toolbar_paint.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

#define SAFE_RELEASE(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)

static INIT_ONCE        g_once = INIT_ONCE_STATIC_INIT;
static ID2D1Factory    *g_d2d;
static IDWriteFactory  *g_dw;
static wchar_t          g_text_face[64] = L"Segoe UI";
static wchar_t          g_icon_face[64] = L"Segoe MDL2 Assets";

/* Sizes in DIPs. */
#define TBP_TEXT_DIP     14.0f
#define TBP_SMALL_DIP    12.0f
#define TBP_GLYPH_DIP    16.0f
#define TBP_CHEVRON_DIP  12.0f
#define TBP_PAD_DIP      12.0f
#define TBP_GAP_DIP       8.0f
#define TBP_BUTTON_DIP   40.0f
#define TBP_SEP_DIP      13.0f
#define TBP_RADIUS_DIP    4.0f
#define TBP_DOT_DIP       8.0f

static BOOL has_family(IDWriteFontCollection *fc, const wchar_t *name)
{
    UINT32 index = 0;
    BOOL exists = FALSE;
    return fc && SUCCEEDED(fc->FindFamilyName(name, &index, &exists)) && exists;
}

static BOOL CALLBACK init_once(PINIT_ONCE, PVOID, PVOID *)
{
    IDWriteFontCollection *fc = nullptr;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory),
                                 nullptr, (void **)&g_d2d)))
        g_d2d = nullptr;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   (IUnknown **)&g_dw)))
        g_dw = nullptr;
    if (g_dw && SUCCEEDED(g_dw->GetSystemFontCollection(&fc, FALSE))) {
        if (has_family(fc, L"Segoe UI Variable Text"))
            wcscpy_s(g_text_face, ARRAYSIZE(g_text_face), L"Segoe UI Variable Text");
        if (has_family(fc, L"Segoe Fluent Icons"))
            wcscpy_s(g_icon_face, ARRAYSIZE(g_icon_face), L"Segoe Fluent Icons");
        fc->Release();
    }
    return TRUE;
}

static BOOL ready(void)
{
    InitOnceExecuteOnce(&g_once, init_once, nullptr, nullptr);
    return g_d2d && g_dw;
}

static float px(float dip, UINT dpi) { return dip * (float)(dpi ? dpi : 96) / 96.0f; }

static IDWriteTextFormat *make_format(BOOL icon, float size_px, DWRITE_FONT_WEIGHT weight,
                                      DWRITE_TEXT_ALIGNMENT align)
{
    IDWriteTextFormat *f = nullptr;
    if (FAILED(g_dw->CreateTextFormat(icon ? g_icon_face : g_text_face, nullptr, weight,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      size_px, L"", &f)))
        return nullptr;
    f->SetTextAlignment(align);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return f;
}

static float text_width(IDWriteTextFormat *f, const wchar_t *text)
{
    IDWriteTextLayout *l = nullptr;
    DWRITE_TEXT_METRICS m = {};
    if (!f || !text || !text[0]) return 0.0f;
    if (FAILED(g_dw->CreateTextLayout(text, (UINT32)wcslen(text), f, 4096.0f, 256.0f, &l)))
        return 0.0f;
    l->GetMetrics(&m);
    l->Release();
    return m.widthIncludingTrailingWhitespace;
}

static DWRITE_FONT_WEIGHT weight_of(int kind)
{
    return kind == TBP_LABEL ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
}

static float size_of(int kind, UINT dpi)
{
    return px(kind == TBP_STATUS ? TBP_SMALL_DIP : TBP_TEXT_DIP, dpi);
}

extern "C" int tbp_item_width(int kind, const wchar_t *text, const wchar_t *glyph, UINT dpi)
{
    float w = 0.0f;
    IDWriteTextFormat *f;
    if (kind == TBP_BUTTON) return (int)ceilf(px(TBP_BUTTON_DIP, dpi));
    if (kind == TBP_SEPARATOR) return (int)ceilf(px(TBP_SEP_DIP, dpi));
    if (!ready()) return (int)ceilf(px(120.0f, dpi));
    f = make_format(FALSE, size_of(kind, dpi), weight_of(kind), DWRITE_TEXT_ALIGNMENT_LEADING);
    w = text_width(f, text);
    SAFE_RELEASE(f);
    w += 2.0f * px(TBP_PAD_DIP, dpi);
    if (kind == TBP_LABEL && glyph && glyph[0]) w += px(TBP_GLYPH_DIP + TBP_GAP_DIP, dpi);
    if (kind == TBP_DROPDOWN) w += px(TBP_GAP_DIP + TBP_CHEVRON_DIP, dpi);
    if (kind == TBP_STATUS) w += px(TBP_DOT_DIP + TBP_GAP_DIP, dpi);
    return (int)ceilf(w);
}

static D2D1_COLOR_F rgba(UINT32 rgb, float a)
{
    return D2D1::ColorF((float)((rgb >> 16) & 0xFF) / 255.0f, (float)((rgb >> 8) & 0xFF) / 255.0f,
                        (float)(rgb & 0xFF) / 255.0f, a);
}

static D2D1_RECT_F rectf(const RECT &r)
{
    return D2D1::RectF((float)r.left, (float)r.top, (float)r.right, (float)r.bottom);
}

static void draw_text(ID2D1RenderTarget *rt, IDWriteTextFormat *f, const wchar_t *text,
                      D2D1_RECT_F r, ID2D1Brush *brush)
{
    if (!f || !text || !text[0]) return;
    rt->DrawText(text, (UINT32)wcslen(text), f, r, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

/* A 32-bit premultiplied bitmap the size of the window with a Direct2D
   target on it; canvas_end copies it to the window (alpha included, which
   is what lets the backdrop show through). */
struct Canvas {
    HDC                   mem;
    HBITMAP               bmp;
    HGDIOBJ               old;
    ID2D1DCRenderTarget  *rt;
    ID2D1SolidColorBrush *brush;
};

static BOOL canvas_begin(Canvas *c, HDC dc, int w, int h)
{
    BITMAPINFO bi = {};
    RECT bind = { 0, 0, w, h };
    void *bits = nullptr;
    ZeroMemory(c, sizeof(*c));
    if (w <= 0 || h <= 0 || !ready()) return FALSE;
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    c->mem = CreateCompatibleDC(dc);
    c->bmp = c->mem ? CreateDIBSection(c->mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!c->bmp) return FALSE;
    c->old = SelectObject(c->mem, c->bmp);
    {
        D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        if (FAILED(g_d2d->CreateDCRenderTarget(&p, &c->rt)) || FAILED(c->rt->BindDC(c->mem, &bind)))
            return FALSE;
    }
    if (FAILED(c->rt->CreateSolidColorBrush(rgba(0xFFFFFF, 1.0f), &c->brush))) return FALSE;
    c->rt->BeginDraw();
    c->rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    return TRUE;
}

/* Finish drawing (when draw is TRUE), copy to dc and free everything. */
static BOOL canvas_end(Canvas *c, HDC dc, int w, int h, BOOL draw)
{
    BOOL ok = FALSE;
    if (draw && c->rt && SUCCEEDED(c->rt->EndDraw()))
        ok = BitBlt(dc, 0, 0, w, h, c->mem, 0, 0, SRCCOPY);
    SAFE_RELEASE(c->brush);
    SAFE_RELEASE(c->rt);
    if (c->old) SelectObject(c->mem, c->old);
    if (c->bmp) DeleteObject(c->bmp);
    if (c->mem) DeleteDC(c->mem);
    ZeroMemory(c, sizeof(*c));
    return ok;
}

extern "C" BOOL tbp_paint(HDC dc, int w, int h, UINT dpi, const TbpItem *items, int count,
                          BOOL solid, UINT32 accent)
{
    Canvas cv;
    ID2D1DCRenderTarget *rt;
    ID2D1SolidColorBrush *brush;
    IDWriteTextFormat *f_label = nullptr, *f_text = nullptr, *f_small = nullptr;
    IDWriteTextFormat *f_glyph = nullptr, *f_chevron = nullptr, *f_glyph_lead = nullptr;
    BOOL ok = FALSE;
    float radius = px(TBP_RADIUS_DIP, dpi), pad = px(TBP_PAD_DIP, dpi), gap = px(TBP_GAP_DIP, dpi);
    int i;

    if (!canvas_begin(&cv, dc, w, h)) { canvas_end(&cv, dc, w, h, FALSE); return FALSE; }
    rt = cv.rt;
    brush = cv.brush;

    f_label = make_format(FALSE, px(TBP_TEXT_DIP, dpi), DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    f_text = make_format(FALSE, px(TBP_TEXT_DIP, dpi), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    f_small = make_format(FALSE, px(TBP_SMALL_DIP, dpi), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    f_glyph = make_format(TRUE, px(TBP_GLYPH_DIP, dpi), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    f_glyph_lead = make_format(TRUE, px(TBP_GLYPH_DIP, dpi), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    f_chevron = make_format(TRUE, px(TBP_CHEVRON_DIP, dpi), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);

    /* Over the system acrylic: a tint layer like WinUI's acrylic brushes
       (dark #2C2C2C, high luminosity opacity) so text stays readable over
       any picture. Without the backdrop: the flyout fallback colour. */
    rt->Clear(solid ? rgba(0x2C2C2C, 1.0f) : rgba(0x2C2C2C, 0.72f));

    for (i = 0; i < count; i++) {
        const TbpItem *it = &items[i];
        D2D1_RECT_F r = rectf(it->rc);
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, radius, radius);

        switch (it->kind) {
        case TBP_SEPARATOR: {
            float x = floorf((r.left + r.right) / 2.0f) + 0.5f, half = px(12.0f, dpi);
            float cy = (r.top + r.bottom) / 2.0f;
            brush->SetColor(rgba(0xFFFFFF, 0.0837f));
            rt->DrawLine(D2D1::Point2F(x, cy - half), D2D1::Point2F(x, cy + half), brush, 1.0f);
            break;
        }
        case TBP_LABEL: {
            D2D1_RECT_F t = r;
            t.left += pad;
            if (it->glyph && it->glyph[0]) {
                brush->SetColor(rgba(0xFFFFFF, 0.786f));
                draw_text(rt, f_glyph_lead, it->glyph, t, brush);
                t.left += px(TBP_GLYPH_DIP, dpi) + gap;
            }
            brush->SetColor(rgba(0xFFFFFF, 1.0f));
            draw_text(rt, f_label, it->text, t, brush);
            break;
        }
        case TBP_STATUS: {
            D2D1_RECT_F t = r;
            t.left += pad;
            if (it->dot) {
                float d = px(TBP_DOT_DIP, dpi), cy = (r.top + r.bottom) / 2.0f;
                brush->SetColor(rgba(it->dot, 1.0f));
                rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(t.left + d / 2.0f, cy), d / 2.0f, d / 2.0f), brush);
                t.left += d + gap;
            }
            brush->SetColor(rgba(0xFFFFFF, 0.786f));
            draw_text(rt, f_small, it->text, t, brush);
            break;
        }
        case TBP_DROPDOWN:
        case TBP_BUTTON: {
            BOOL accent_fill = it->kind == TBP_BUTTON && it->on;
            if (accent_fill) {
                /* AccentFillColorDefault / Secondary (hover) / Tertiary (pressed). */
                brush->SetColor(rgba(accent, it->pressed ? 0.8f : it->hot ? 0.9f : 1.0f));
                rt->FillRoundedRectangle(rr, brush);
            } else if (it->pressed || it->hot) {
                /* SubtleFillColorTertiary (pressed) / Secondary (hover). */
                brush->SetColor(rgba(0xFFFFFF, it->pressed ? 0.0419f : 0.0605f));
                rt->FillRoundedRectangle(rr, brush);
            }
            if (accent_fill)
                brush->SetColor(rgba(0x000000, it->pressed ? 0.5f : 1.0f));   /* TextOnAccentFillColor */
            else
                brush->SetColor(rgba(0xFFFFFF, it->pressed ? 0.786f : 1.0f));
            if (it->kind == TBP_BUTTON) {
                draw_text(rt, f_glyph, it->text, r, brush);
            } else {
                D2D1_RECT_F t = r, c = r;
                t.left += pad;
                t.right -= pad + px(TBP_CHEVRON_DIP, dpi) + gap;
                draw_text(rt, f_text, it->text, t, brush);
                c.right -= pad;
                c.left = c.right - px(TBP_CHEVRON_DIP, dpi);
                brush->SetColor(rgba(0xFFFFFF, 0.786f));
                draw_text(rt, f_chevron, L"\xE70D", c, brush);   /* ChevronDown */
            }
            break;
        }
        default:
            break;
        }
    }

    ok = canvas_end(&cv, dc, w, h, TRUE);
    SAFE_RELEASE(f_label);
    SAFE_RELEASE(f_text);
    SAFE_RELEASE(f_small);
    SAFE_RELEASE(f_glyph);
    SAFE_RELEASE(f_glyph_lead);
    SAFE_RELEASE(f_chevron);
    return ok;
}

/* ---- Menu flyout: WinUI MenuFlyout metrics (DIPs) ---- */
#define TBP_MENU_ROW_DIP      36.0f   /* 32 px item + 2 px margin above and below */
#define TBP_MENU_SEP_DIP       9.0f
#define TBP_MENU_PAD_DIP       2.0f   /* presenter padding above / below */
#define TBP_MENU_INSET_DIP     4.0f   /* item margin left and right */
#define TBP_MENU_TEXT_PAD_DIP 12.0f
#define TBP_MENU_COL_DIP      28.0f   /* a 16 px glyph and 12 px of space */
#define TBP_MENU_ACCEL_GAP    24.0f
#define TBP_MENU_MIN_DIP     200.0f

extern "C" int tbp_menu_row_height(int kind, UINT dpi)
{
    return (int)ceilf(px(kind == TBP_ROW_SEPARATOR ? TBP_MENU_SEP_DIP : TBP_MENU_ROW_DIP, dpi));
}

extern "C" int tbp_menu_padding(UINT dpi)
{
    return (int)ceilf(px(TBP_MENU_PAD_DIP, dpi));
}

extern "C" int tbp_menu_width(const TbpMenuRow *rows, int count, BOOL check_col, BOOL icon_col, UINT dpi)
{
    IDWriteTextFormat *f;
    float text_w = 0.0f, accel_w = 0.0f, w;
    BOOL any_sub = FALSE;
    int i;
    if (!ready()) return (int)ceilf(px(TBP_MENU_MIN_DIP, dpi));
    f = make_format(FALSE, px(TBP_TEXT_DIP, dpi), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    for (i = 0; i < count; i++) {
        float t;
        if (rows[i].kind != TBP_ROW_ITEM) continue;
        t = text_width(f, rows[i].text);
        if (t > text_w) text_w = t;
        t = text_width(f, rows[i].accel);
        if (t > accel_w) accel_w = t;
        if (rows[i].submenu) any_sub = TRUE;
    }
    SAFE_RELEASE(f);
    w = 2.0f * px(TBP_MENU_INSET_DIP + TBP_MENU_TEXT_PAD_DIP, dpi) + text_w;
    if (check_col) w += px(TBP_MENU_COL_DIP, dpi);
    if (icon_col) w += px(TBP_MENU_COL_DIP, dpi);
    if (accel_w > 0.0f) w += px(TBP_MENU_ACCEL_GAP, dpi) + accel_w;
    if (any_sub) w += px(TBP_MENU_ACCEL_GAP, dpi) + px(TBP_CHEVRON_DIP, dpi);
    if (w < px(TBP_MENU_MIN_DIP, dpi)) w = px(TBP_MENU_MIN_DIP, dpi);
    return (int)ceilf(w);
}

extern "C" BOOL tbp_paint_menu(HDC dc, int w, int h, UINT dpi, const TbpMenuRow *rows, int count,
                               BOOL check_col, BOOL icon_col, BOOL solid)
{
    Canvas cv;
    ID2D1DCRenderTarget *rt;
    ID2D1SolidColorBrush *brush;
    IDWriteTextFormat *f_text = nullptr, *f_accel = nullptr, *f_glyph = nullptr, *f_chevron = nullptr;
    float inset = px(TBP_MENU_INSET_DIP, dpi), tpad = px(TBP_MENU_TEXT_PAD_DIP, dpi);
    float col = px(TBP_MENU_COL_DIP, dpi), glyph = px(TBP_GLYPH_DIP, dpi);
    float radius = px(TBP_RADIUS_DIP, dpi), vmargin = px(2.0f, dpi);
    BOOL ok;
    int i;

    if (!canvas_begin(&cv, dc, w, h)) { canvas_end(&cv, dc, w, h, FALSE); return FALSE; }
    rt = cv.rt;
    brush = cv.brush;
    f_text = make_format(FALSE, px(TBP_TEXT_DIP, dpi), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    f_accel = make_format(FALSE, px(TBP_TEXT_DIP, dpi), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    f_glyph = make_format(TRUE, px(TBP_GLYPH_DIP, dpi), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    f_chevron = make_format(TRUE, px(TBP_CHEVRON_DIP, dpi), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);

    /* AcrylicBackgroundFillColorDefault is nearly opaque: menus read like
       solid surfaces with a hint of what is behind. */
    rt->Clear(solid ? rgba(0x2C2C2C, 1.0f) : rgba(0x2C2C2C, 0.86f));

    for (i = 0; i < count; i++) {
        const TbpMenuRow *r = &rows[i];
        D2D1_RECT_F rf = rectf(r->rc);
        float x;
        float fg_a = r->enabled ? 1.0f : 0.3628f;   /* TextFillColorPrimary / Disabled */

        if (r->kind == TBP_ROW_SEPARATOR) {
            float y = floorf((rf.top + rf.bottom) / 2.0f) + 0.5f;
            brush->SetColor(rgba(0xFFFFFF, 0.0837f));   /* DividerStrokeColorDefault */
            rt->DrawLine(D2D1::Point2F(rf.left, y), D2D1::Point2F(rf.right, y), brush, 1.0f);
            continue;
        }
        if (r->hot && r->enabled) {
            D2D1_RECT_F hb = D2D1::RectF(rf.left + inset, rf.top + vmargin, rf.right - inset, rf.bottom - vmargin);
            brush->SetColor(rgba(0xFFFFFF, 0.0605f));   /* SubtleFillColorSecondary */
            rt->FillRoundedRectangle(D2D1::RoundedRect(hb, radius, radius), brush);
        }
        x = rf.left + inset + tpad;
        if (check_col) {
            if (r->checked) {
                brush->SetColor(rgba(0xFFFFFF, fg_a));
                draw_text(rt, f_glyph, L"\xE73E", D2D1::RectF(x, rf.top, x + glyph, rf.bottom), brush);   /* CheckMark */
            }
            x += col;
        }
        if (icon_col) {
            if (r->glyph && r->glyph[0]) {
                brush->SetColor(rgba(0xFFFFFF, fg_a));
                draw_text(rt, f_glyph, r->glyph, D2D1::RectF(x, rf.top, x + glyph, rf.bottom), brush);
            }
            x += col;
        }
        {
            float right = rf.right - inset - tpad;
            if (r->submenu) {
                brush->SetColor(rgba(0xFFFFFF, r->enabled ? 0.786f : 0.3628f));
                draw_text(rt, f_chevron, L"\xE76C",   /* ChevronRight */
                          D2D1::RectF(right - px(TBP_CHEVRON_DIP, dpi), rf.top, right, rf.bottom), brush);
                right -= px(TBP_CHEVRON_DIP, dpi) + px(TBP_GAP_DIP, dpi);
            }
            if (r->accel && r->accel[0]) {
                brush->SetColor(rgba(0xFFFFFF, r->enabled ? 0.786f : 0.3628f));   /* TextFillColorSecondary */
                draw_text(rt, f_accel, r->accel, D2D1::RectF(x, rf.top, right, rf.bottom), brush);
            }
            brush->SetColor(rgba(0xFFFFFF, fg_a));
            draw_text(rt, f_text, r->text, D2D1::RectF(x, rf.top, right, rf.bottom), brush);
        }
    }

    ok = canvas_end(&cv, dc, w, h, TRUE);
    SAFE_RELEASE(f_text);
    SAFE_RELEASE(f_accel);
    SAFE_RELEASE(f_glyph);
    SAFE_RELEASE(f_chevron);
    return ok;
}

extern "C" UINT32 tbp_accent_color(void)
{
    /* AccentPalette: eight RGBA entries from lightest to darkest; [1] is
       "light 2", the shade WinUI fills accent controls with in dark mode. */
    BYTE pal[32];
    DWORD size = sizeof(pal), type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, &type, pal, &size) == ERROR_SUCCESS &&
        size >= 8)
        return ((UINT32)pal[4] << 16) | ((UINT32)pal[5] << 8) | pal[6];
    return 0x60CDFF;
}
