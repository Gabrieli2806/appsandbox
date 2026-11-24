/*
 * vm_toolbar_paint.h -- draws the VM window's floating toolbar in the style
 * of a WinUI command bar (Direct2D + DirectWrite).
 *
 * The toolbar window lets the system acrylic backdrop show through where
 * nothing is drawn: the picture is painted with premultiplied alpha into a
 * 32-bit bitmap and copied to the window, whose frame covers the whole
 * client area. With solid = TRUE (no backdrop on this Windows) the
 * background is painted opaque instead.
 */
#ifndef VM_TOOLBAR_PAINT_H
#define VM_TOOLBAR_PAINT_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TBP_LABEL,      /* text with an optional leading glyph (the VM's name) */
    TBP_STATUS,     /* small text with a coloured dot (frame rate, waiting) */
    TBP_DROPDOWN,   /* text button with a chevron (resolution) */
    TBP_BUTTON,     /* icon button; on = checked toggle (accent fill) */
    TBP_SEPARATOR
} TbpKind;

typedef struct {
    RECT           rc;      /* device pixels, in the toolbar's client area */
    int            kind;    /* TbpKind */
    const wchar_t *text;    /* label text, or the glyph of a button */
    const wchar_t *glyph;   /* TBP_LABEL: glyph before the text, or NULL */
    BOOL           on;      /* TBP_BUTTON: checked */
    BOOL           hot;     /* pointer over it */
    BOOL           pressed; /* mouse button down on it */
    UINT32         dot;     /* TBP_STATUS: 0xRRGGBB of the dot, 0 = none */
} TbpItem;

/* Width in device pixels an item needs at this DPI (text measured with the
   fonts tbp_paint uses). */
int tbp_item_width(int kind, const wchar_t *text, const wchar_t *glyph, UINT dpi);

/* Paint the toolbar into dc (w x h device pixels). accent is 0xRRGGBB.
   FALSE when Direct2D is unavailable (the caller paints something plain). */
BOOL tbp_paint(HDC dc, int w, int h, UINT dpi, const TbpItem *items, int count,
               BOOL solid, UINT32 accent);

/* ---- Menu flyout (the toolbar's Display and More menus) ---- */

typedef enum { TBP_ROW_ITEM, TBP_ROW_SEPARATOR } TbpRowKind;

typedef struct {
    RECT           rc;       /* the whole row, device pixels */
    int            kind;     /* TbpRowKind */
    const wchar_t *text;
    const wchar_t *accel;    /* right-aligned key text ("F11"), or NULL */
    const wchar_t *glyph;    /* icon, or NULL */
    BOOL           checked;
    BOOL           enabled;
    BOOL           submenu;  /* draws a chevron */
    BOOL           hot;
} TbpMenuRow;

/* Height of a row in device pixels. */
int tbp_menu_row_height(int kind, UINT dpi);

/* Vertical padding above the first and below the last row. */
int tbp_menu_padding(UINT dpi);

/* Width the flyout needs for these rows. check_col / icon_col reserve the
   columns for check marks and icons (shared by every row, as in WinUI). */
int tbp_menu_width(const TbpMenuRow *rows, int count, BOOL check_col, BOOL icon_col, UINT dpi);

BOOL tbp_paint_menu(HDC dc, int w, int h, UINT dpi, const TbpMenuRow *rows, int count,
                    BOOL check_col, BOOL icon_col, BOOL solid);

/* The accent colour WinUI uses for filled controls on a dark surface
   (the system accent's "light 2" shade), as 0xRRGGBB. */
UINT32 tbp_accent_color(void);

#ifdef __cplusplus
}
#endif

#endif
