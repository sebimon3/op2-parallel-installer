#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <vsstyle.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <pdh.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "app.h"
#include "core.h"
#include "ui.h"
#include "manifest_data.h"   /* defines g_manifest (generated, read-only) */

/* ------------------------------------------------------------------ */
/*  Layout.  The reference installer uses Tahoma 8 / Tahoma 10 bold and */
/*  a ~484x560 client area with a 60px white header and a 46px footer.  */
/* ------------------------------------------------------------------ */
/* Outer wizard window is 486 x 586 (matches the original screenshots).
 * Windows adds 16px horizontally and 39px vertically for the frame. */
#define CLIENT_W   470
#define CLIENT_H   547
#define HEADER_H   60
#define FOOTER_H   46
#define BODY_TOP   HEADER_H
#define BODY_BOT   (CLIENT_H - FOOTER_H)
#define BTN_W      75
#define BTN_H      23
#define BTN_Y      (CLIENT_H - BTN_H - 10)
#define BTN_CX     (CLIENT_W - 9 - BTN_W)
#define BTN_NX     (BTN_CX - 9 - BTN_W)
#define BTN_BX     (BTN_NX - 9 - BTN_W)
#define CONTENT_W  (CLIENT_W - 64)

#define PREVIEW_CW 640
#define PREVIEW_CH 360

typedef enum {
    PG_LANG = 0, PG_DEST, PG_COMP, PG_TASKS, PG_READY, PG_INSTALL, PG_FINISH,
    PG_COUNT
} Page;

#define IDI_APP        100
#define IDR_WIZARD_BMP       210   /* RCDATA: assets/wizard.bmp */
#define IDR_WIZARD_SMALL_BMP 211   /* RCDATA: assets/wizard_small.bmp */

/* The artwork is shipped as real .bmp files in assets/ and embedded as
 * RCDATA; load them once on first use instead of keeping byte blobs in C. */
static void load_wizard_assets(HINSTANCE hInst)
{
    static int loaded = 0;
    static const unsigned char *wiz, *small_bmp;
    static size_t wiz_len, small_len;
    if (loaded) return;
    loaded = 1;
    {
        HRSRC r = FindResourceW(hInst, MAKEINTRESOURCE(IDR_WIZARD_BMP), RT_RCDATA);
        if (r) {
            HGLOBAL hg = LoadResource(hInst, r);
            if (hg) {
                wiz = (const unsigned char *)LockResource(hg);
                wiz_len = SizeofResource(hInst, r);
            }
        }
    }
    {
        HRSRC r = FindResourceW(hInst, MAKEINTRESOURCE(IDR_WIZARD_SMALL_BMP), RT_RCDATA);
        if (r) {
            HGLOBAL hg = LoadResource(hInst, r);
            if (hg) {
                small_bmp = (const unsigned char *)LockResource(hg);
                small_len = SizeofResource(hInst, r);
            }
        }
    }
    g_manifest.wizard_bmp = wiz;
    g_manifest.wizard_bmp_len = wiz_len;
    g_manifest.wizard_small_bmp = small_bmp;
    g_manifest.wizard_small_bmp_len = small_len;
}

#define ID_EDIT_DEST   101
#define ID_BTN_BROWSE  102
#define ID_COMBO_LANG  103
#define ID_COMBO_TYPE  104
#define ID_TREE        105
#define ID_CHK_DESKTOP 106
#define ID_RADIO_COMMON 107
#define ID_RADIO_CURRENT 108
#define ID_PROGRESS    109
#define ID_STATS       110
#define ID_THREADS     111
#define ID_LANG_COMBO  300
#define ID_LANG_OK     301
#define ID_LANG_CANCEL 302
#define ID_BTN_BACK    200
#define ID_BTN_NEXT    201
#define ID_BTN_CANCEL  202

#define WM_APP_PROGRESS (WM_APP + 1)
#define WM_APP_DONE     (WM_APP + 2)

#define MAX_ITEMS 512

typedef struct {
    DWORD t;
    double tp;
    double wr;
    double du;
} TelemetrySample;

typedef struct {
    HWND hwnd;
    Page page;
    HINSTANCE hInst;

    char source_dir[MAX_PATH];
    WCHAR dest[MAX_PATH];
    WCHAR default_dest[MAX_PATH];

    unsigned comp_mask;
    unsigned task_mask;
    WCHAR status[512];
    char errbuf[1024];
    int lang;

    HFONT font_ui, font_title, font_small, font_mono;
    HBRUSH brush_body;

    HWND hEdit, hBrowse, hComboLang, hComboType, hTree, hChkDesktop,
         hRadioCommon, hRadioCurrent, hProgress, hStats, hComboThreads;

    /* debug progress telemetry */
    PDH_HQUERY hPdh;
    PDH_HCOUNTER hDisk;
    PDH_HCOUNTER hWrites;
    int disk_invert;             /* using "% Idle Time" instead of "% Disk Time" */
    double disk_util;            /* 1-second average percentage */
    WCHAR disk_letter;
    double throughput_mbs;       /* decoded MiB/s (internal) */
    double writes_mbs;           /* 1-second average disk writes MiB/s */
    int last_active, last_total; /* preserved after workers finish */
    int nprocs;
    int worker_limit;            /* 0 = Auto */

    TelemetrySample samples[128];
    int sample_head, sample_count;
    uint64_t last_bytes;
    DWORD last_tick;
    HWND hBack, hNext, hCancel;

    WNDPROC old_tree_proc;

    HTREEITEM item_handle[MAX_ITEMS];
    int item_comp[MAX_ITEMS];
    int item_parent[MAX_ITEMS];
    int item_state[MAX_ITEMS];   /* 0 box off, 1 box on, 2 radio off, 3 radio on */
    int item_count;
    int comp_item[512];

    HWND hPreview;
    WNDPROC old_preview_proc;
    int preview_variant;
    int preview_outer_w, preview_outer_h;
    uint8_t *prev_buf[3];
    size_t   prev_len[3];
    int      prev_tried[3];

    HANDLE install_thread;
    int install_result, shortcuts_result;
    int progress_done, progress_total;

    HWND hLangDlg;
    int lang_result;
} App;

static App g;

static int confirm_exit(void);

/* ------------------------------------------------------------------ */
static void utf8w(const char *s, WCHAR *d, int cch)
{
    if (!s) { d[0] = 0; return; }
    MultiByteToWideChar(CP_UTF8, 0, s, -1, d, cch);
}

static HFONT make_font(int px, int weight)
{
    return CreateFontW(px, 0, 0, 0, weight, 0, 0, 0,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       L"Tahoma");
}

/* Totals are shown in GB once they exceed a gigabyte, as in the original. */
static void format_total_utf8(char *out, size_t cch, uint64_t bytes)
{
    double v = (double)bytes;
    char *p;
    if (v >= 1024.0 * 1024.0 * 1024.0)
        _snprintf_s(out, cch, _TRUNCATE, "%.2f GB", v / (1024.0*1024.0*1024.0));
    else if (v >= 1024.0 * 1024.0)
        _snprintf_s(out, cch, _TRUNCATE, "%.1f MB", v / (1024.0*1024.0));
    else
        _snprintf_s(out, cch, _TRUNCATE, "%.0f KB", v / 1024.0);
    for (p = out; *p; ++p) if (*p == '.') *p = ',';
}

static void format_size_utf8(char *out, size_t cch, uint64_t bytes)
{
    double v = (double)bytes;
    char *p;
    /* The original installer reports every component in MiB, including the
     * multi-gigabyte root entry (e.g. "35,792.1 MB"). */
    if (v >= 1024.0 * 1024.0)
        _snprintf_s(out, cch, _TRUNCATE, "%.1f MB", v / (1024.0*1024.0));
    else
        _snprintf_s(out, cch, _TRUNCATE, "%.0f KB", v / 1024.0);
    for (p = out; *p; ++p) if (*p == '.') *p = ',';
}

static void draw_bmp(HDC hdc, const uint8_t *bmp, size_t len,
                     int x, int y, int w, int h);

/* Inno Setup places the large wizard image at the upper-right, clipped to
 * the header: only its right-hand portion is visible in the classic style. */
static void draw_bmp_clipped(HDC hdc, const uint8_t *bmp, size_t len,
                             int cx, int cy, int cw, int ch)
{
    BITMAPINFOHEADER bih;
    int w, h, x;
    HRGN rgn;
    if (!bmp || len < 54) return;
    memcpy(&bih, bmp + 14, sizeof(bih));
    if (bih.biSize < 40 || bih.biWidth <= 0 || bih.biHeight == 0) return;
    w = bih.biWidth;
    h = bih.biHeight < 0 ? -bih.biHeight : bih.biHeight;
    x = cx + cw - w;
    rgn = CreateRectRgn(cx, cy, cx + cw, cy + ch);
    SelectClipRgn(hdc, rgn);
    draw_bmp(hdc, bmp, len, x, cy, w, h);
    SelectClipRgn(hdc, NULL);
    DeleteObject(rgn);
}

/* Draw a BMP held in memory (BITMAPFILEHEADER + BITMAPINFOHEADER + bits). */
static void draw_bmp(HDC hdc, const uint8_t *bmp, size_t len,
                     int x, int y, int w, int h)
{
    BITMAPFILEHEADER bf;
    BITMAPINFOHEADER bih;
    BITMAPINFO bi;
    const uint8_t *bits;
    if (!bmp || len < sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER))
        return;
    memcpy(&bf, bmp, sizeof(bf));
    memcpy(&bih, bmp + sizeof(BITMAPFILEHEADER), sizeof(bih));
    if (bih.biSize < sizeof(BITMAPINFOHEADER)) return;
    bits = bmp + (bf.bfOffBits ? bf.bfOffBits : 14 + bih.biSize);
    if ((size_t)(bits - bmp) >= len) return;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader = bih;
    /* Inno Setup scales its wizard artwork with halftone dithering; without
     * this the 108x108 small image looks blocky when drawn at ~58x58. */
    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, x, y, NULL);
    StretchDIBits(hdc, x, y, w, h, 0, 0, bih.biWidth, bih.biHeight,
                  bits, &bi, DIB_RGB_COLORS, SRCCOPY);
}

/* ------------------------------------------------------------------ */
/*  Component tree helpers                                            */
/* ------------------------------------------------------------------ */
static int item_index(HTREEITEM h)
{
    int i;
    for (i = 0; i < g.item_count; ++i)
        if (g.item_handle[i] == h) return i;
    return -1;
}

static void set_item_state(int ii, int st)
{
    TVITEMW ti;
    if (ii < 0 || ii >= g.item_count) return;
    g.item_state[ii] = st;
    memset(&ti, 0, sizeof(ti));
    ti.hItem = g.item_handle[ii];
    ti.mask = TVIF_STATE;
    ti.stateMask = TVIS_STATEIMAGEMASK;
    ti.state = (UINT)(st + 1) << 12;
    TreeView_SetItem(g.hTree, &ti);
}

static void set_descendant_states(HTREEITEM parent, int st)
{
    HTREEITEM h = TreeView_GetChild(g.hTree, parent);
    while (h) {
        int ii = item_index(h);
        if (ii >= 0) {
            int ci = g.item_comp[ii];
            if (!(ci >= 0 && (g_manifest.comps[ci].flags & 1))) {
                int radio = (ci >= 0 && (g_manifest.comps[ci].flags & 2));
                if (!radio) set_item_state(ii, st);
            }
        }
        set_descendant_states(h, st);
        h = TreeView_GetNextSibling(g.hTree, h);
    }
}

static void update_mask_and_status(void)
{
    uint64_t total = 0;
    int i;
    g.comp_mask = 0;
    for (i = 0; i < g.item_count; ++i) {
        int ci = g.item_comp[i];
        if (ci < 0 || ci >= g_manifest.ncomp) continue;
        if (g.item_state[i] == 1 || g.item_state[i] == 3 || g.item_state[i] == 4) {
            g.comp_mask |= (1u << ci);
            total += g_manifest.comps[ci].size;
        }
    }
    {
        char sz[64], buf[1024];
        format_total_utf8(sz, sizeof(sz), total);
        /* The original wizard phrases this line as "At least <size> of free
         * disk space is required."; keep that wording even when the manifest
         * carries the shorter "Space required: %1" variant. */
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "At least %s of free disk space is required.", sz);
        utf8w(buf, g.status, 512);
    }
    InvalidateRect(g.hwnd, NULL, FALSE);
}

static int look_variant(HTREEITEM h)
{
    int ii = item_index(h);
    int ci;
    char buf[512];
    const char *p, *s;
    if (ii < 0) return -1;
    ci = g.item_comp[ii];
    if (ci < 0 || ci >= g_manifest.ncomp) return -1;
    s = g_manifest.comps[ci].name;
    if (!s) return -1;
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s", s);
    _strlwr_s(buf, sizeof(buf));
    p = strstr(buf, "look");
    if (!p && g_manifest.comps[ci].desc) {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s", g_manifest.comps[ci].desc);
        _strlwr_s(buf, sizeof(buf));
        p = strstr(buf, "look");
    }
    if (!p) return -1;
    p += 4;
    while (*p && !isdigit((unsigned char)*p)) ++p;
    if (*p >= '1' && *p <= '3') return *p - '1';
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Preview window                                                    */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK PreviewProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        if (g.preview_variant >= 0 && g.preview_variant < 3 &&
            g.prev_buf[g.preview_variant])
            draw_bmp(hdc, g.prev_buf[g.preview_variant],
                     g.prev_len[g.preview_variant], 0, 0,
                     rc.right, rc.bottom);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        return 0;
    }
    return CallWindowProc(g.old_preview_proc, h, m, w, l);
}

static void hide_preview(void)
{
    if (g.hPreview) ShowWindow(g.hPreview, SW_HIDE);
}

static void show_preview(int variant)
{
    RECT rw;
    int x, y;
    if (variant < 0 || variant > 2) return;
    if (g.preview_variant != variant) {
        g.preview_variant = variant;
        if (!g.prev_tried[variant]) {
            char err[512];
            uint8_t *buf = NULL;
            size_t len = 0;
            err[0] = 0;
            g.prev_tried[variant] = 1;
            /* op22_decode_preview returns 0 on success (buffer set). */
            if (op22_decode_preview(g.source_dir, variant, &buf, &len,
                                    err, sizeof(err)) == 0 && buf) {
                g.prev_buf[variant] = buf;
                g.prev_len[variant] = len;
            }
        }
        InvalidateRect(g.hPreview, NULL, TRUE);
    }
    if (IsWindowVisible(g.hPreview)) return;
    GetWindowRect(g.hwnd, &rw);
    x = rw.left - g.preview_outer_w + 12;
    y = rw.top + 8;
    /* Keep the popup fully on the monitor even when the wizard is near the
     * left edge of the screen. */
    {
        HMONITOR mon = MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) {
            if (x < mi.rcWork.left) x = mi.rcWork.left;
            if (x + g.preview_outer_w > mi.rcWork.right)
                x = mi.rcWork.right - g.preview_outer_w;
            if (y < mi.rcWork.top) y = mi.rcWork.top;
            if (y + g.preview_outer_h > mi.rcWork.bottom)
                y = mi.rcWork.bottom - g.preview_outer_h;
        }
    }
    SetWindowPos(g.hPreview, NULL, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

/* ------------------------------------------------------------------ */
/*  Tree subclass: state-icon clicks + hover previews                 */
/* ------------------------------------------------------------------ */
static void handle_tree_click(HTREEITEM h)
{
    int ii = item_index(h);
    int ci, old, is_radio, parent;
    if (ii < 0) return;
    ci = g.item_comp[ii];
    if (ci >= 0 && (g_manifest.comps[ci].flags & 1)) return; /* fixed */
    old = g.item_state[ii];
    is_radio = (old >= 2) || (ci >= 0 && (g_manifest.comps[ci].flags & 2));
    if (is_radio) {
        HTREEITEM sib;
        if (old == 3) return;                 /* already selected */
        parent = g.item_parent[ii];
        sib = TreeView_GetChild(g.hTree, TreeView_GetParent(g.hTree, h));
        while (sib) {
            int jj = item_index(sib);
            if (jj >= 0 && g.item_parent[jj] == parent &&
                (g.item_state[jj] >= 2 || (g.item_comp[jj] >= 0 &&
                 (g_manifest.comps[g.item_comp[jj]].flags & 2))))
                set_item_state(jj, 2);
            sib = TreeView_GetNextSibling(g.hTree, sib);
        }
        set_item_state(ii, 3);
    } else {
        int ns = (old == 1) ? 0 : 1;
        set_item_state(ii, ns);
        set_descendant_states(h, ns);
    }
    update_mask_and_status();
}

static LRESULT CALLBACK TreeProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_LBUTTONDOWN) {
        TVHITTESTINFO hti;
        memset(&hti, 0, sizeof(hti));
        hti.pt.x = GET_X_LPARAM(l);
        hti.pt.y = GET_Y_LPARAM(l);
        TreeView_HitTest(h, &hti);
        if (hti.flags & TVHT_ONITEMSTATEICON) {
            handle_tree_click(hti.hItem);
            return 0;
        }
    }
    if (m == WM_MOUSEMOVE) {
        TVHITTESTINFO hti;
        memset(&hti, 0, sizeof(hti));
        hti.pt.x = GET_X_LPARAM(l);
        hti.pt.y = GET_Y_LPARAM(l);
        TreeView_HitTest(h, &hti);
        /* Drive visibility purely from the hit-tested item.  A previous
         * version also used TrackMouseEvent(TME_LEAVE); with synthetic or
         * scripted mouse moves that fires WM_MOUSELEAVE immediately (the real
         * cursor is elsewhere) and hid the popup right after it was shown. */
        if (g.page == PG_COMP) {
            int v = look_variant(hti.hItem);
            if (v >= 0) show_preview(v);
            else hide_preview();
        }
    }
    return CallWindowProc(g.old_tree_proc, h, m, w, l);
}

/* ------------------------------------------------------------------ */
/*  Tree construction                                                 */
/* ------------------------------------------------------------------ */
static int comp_has_children(int ci)
{
    int j;
    for (j = 0; j < g_manifest.ncomp; ++j)
        if (g_manifest.comps[j].parent && g_manifest.comps[j].parent[0] &&
            strcmp(g_manifest.comps[j].parent, g_manifest.comps[ci].name) == 0)
            return 1;
    return 0;
}

/* The stock installer's default selection: only the gloves and their first
 * look are selected; the two sound groups are left unchecked. */
static int initial_comp_state(int ci)
{
    const char *n = g_manifest.comps[ci].name;
    if (n) {
        if (!strcmp(n, "main")) return 4;              /* fixed, disabled */
        if (!strcmp(n, "addons")) return 1;
        if (!strcmp(n, "addons\\arms")) return 1;
        if (!strcmp(n, "addons\\arms\\arm1")) return 3;
        if (!strcmp(n, "addons\\arms\\arm2")) return 2;
        if (!strcmp(n, "addons\\arms\\arm3")) return 2;
        if (!strcmp(n, "addons\\sounds")) return 0;
        if (!strcmp(n, "addons\\sounds\\streamers")) return 0;
        if (!strcmp(n, "addons\\sounds\\streamers\\strict")) return 2;
        if (!strcmp(n, "addons\\sounds\\streamers\\more_strict")) return 2;
        if (!strcmp(n, "addons\\sounds\\cont")) return 0;
        if (!strcmp(n, "addons\\sounds\\cont\\cont1")) return 2;
        if (!strcmp(n, "addons\\sounds\\cont\\cont2")) return 2;
        if (!strcmp(n, "addons\\sounds\\cont\\cont3")) return 2;
    }
    if (g_manifest.comps[ci].flags & 1) return 4;
    if (g_manifest.comps[ci].flags & 2) return 2;
    return 0;
}

static void add_comp_node(int ci, HTREEITEM parent, int parent_comp)
{
    TVINSERTSTRUCTW tis;
    WCHAR text[512];
    const op22_comp *c = &g_manifest.comps[ci];
    int st, ii, j;
    HTREEITEM h;

    if (g.item_count >= MAX_ITEMS) return;
    utf8w(c->desc && c->desc[0] ? c->desc : c->name, text, 512);

    st = initial_comp_state(ci);

    memset(&tis, 0, sizeof(tis));
    tis.hParent = parent;
    tis.hInsertAfter = TVI_LAST;
    tis.item.mask = TVIF_TEXT | TVIF_STATE | TVIF_PARAM | TVIF_CHILDREN;
    tis.item.pszText = text;
    tis.item.stateMask = TVIS_STATEIMAGEMASK;
    tis.item.state = (UINT)(st + 1) << 12;
    tis.item.lParam = (LPARAM)ci;
    tis.item.cChildren = comp_has_children(ci) ? 1 : 0;
    h = TreeView_InsertItem(g.hTree, &tis);
    if (!h) return;

    ii = g.item_count++;
    g.item_handle[ii] = h;
    g.item_comp[ii] = ci;
    g.item_parent[ii] = parent_comp;
    g.item_state[ii] = st;
    if (ci >= 0 && ci < 512) g.comp_item[ci] = ii;

    for (j = 0; j < g_manifest.ncomp; ++j) {
        const op22_comp *s = &g_manifest.comps[j];
        if (s->parent && s->parent[0] &&
            strcmp(s->parent, c->name) == 0)
            add_comp_node(j, h, ci);
    }
}

static void make_state_imagelist(void)
{
    /* Crisp 16x16 glyphs; row spacing is set separately with
     * TreeView_SetItemHeight so the images themselves are never stretched.
     * TreeView state indices are 1-based, so image 0 is a blank placeholder:
     *   1 unchecked box, 2 checked box, 3 radio off, 4 radio on,
     *   5 disabled checked box (the fixed "main" component). */
    const int IW = 16, IH = 16;
    HIMAGELIST himl = ImageList_Create(IW, IH, ILC_COLOR32, 6, 1);
    HDC screen = GetDC(NULL);
    HTHEME theme = IsThemeActive() ? OpenThemeData(NULL, L"BUTTON") : NULL;
    int k;
    for (k = 0; k < 6; ++k) {
        HBITMAP hbm = CreateCompatibleBitmap(screen, IW, IH);
        HDC hdc = CreateCompatibleDC(screen);
        HBITMAP old = SelectObject(hdc, hbm);
        RECT all = {0, 0, IW, IH};
        FillRect(hdc, &all, (HBRUSH)GetStockObject(WHITE_BRUSH));
        if (k > 0) {
            HPEN nullpen = (HPEN)GetStockObject(NULL_PEN);
            HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
            if ((k == 1 || k == 2 || k == 5) && theme) {
                /* Same themed glyph as real checkboxes elsewhere in Windows. */
                RECT part = {0, 0, IW, IH};
                int state = (k == 1) ? CBS_UNCHECKEDNORMAL
                          : (k == 2) ? CBS_CHECKEDNORMAL
                                     : CBS_CHECKEDDISABLED;
                DrawThemeBackground(theme, hdc, BP_CHECKBOX, state, &part, NULL);
            } else if (k == 1 || k == 2 || k == 5) {
                COLORREF edge = (k == 5) ? RGB(150,150,150) : RGB(60,60,60);
                COLORREF fill = (k == 2) ? RGB(0,102,204)
                              : (k == 5) ? RGB(190,190,190) : RGB(255,255,255);
                HPEN frame = CreatePen(PS_SOLID, 1, edge);
                HBRUSH br = CreateSolidBrush(fill);
                SelectObject(hdc, frame);
                SelectObject(hdc, br);
                RoundRect(hdc, 1, 1, 14, 14, 3, 3);
                if (k == 2 || k == 5) {
                    HPEN tick = CreatePen(PS_SOLID, 2, RGB(255,255,255));
                    SelectObject(hdc, tick);
                    MoveToEx(hdc, 3, 7, NULL);
                    LineTo(hdc, 6, 10);
                    LineTo(hdc, 12, 4);
                    DeleteObject(tick);
                }
                DeleteObject(frame);
                DeleteObject(br);
            } else if (theme) {
                /* Use the exact Windows theme glyph that the real radio
                 * buttons on the Tasks page use, rendered into this 16x16
                 * state image.  Square bitmap + square part rect keeps it
                 * from being stretched. */
                RECT part = {0, 0, IW, IH};
                DrawThemeBackground(theme, hdc, BP_RADIOBUTTON,
                                    (k == 4) ? RBS_CHECKEDNORMAL
                                             : RBS_UNCHECKEDNORMAL,
                                    &part, NULL);
            } else if (k == 4) {
                HBRUSH blue = CreateSolidBrush(RGB(0,102,204));
                HPEN bluepen = CreatePen(PS_SOLID, 1, RGB(0,102,204));
                SelectObject(hdc, bluepen);
                SelectObject(hdc, blue);
                Ellipse(hdc, 1, 1, 14, 14);
                SelectObject(hdc, white);
                SelectObject(hdc, nullpen);
                Ellipse(hdc, 5, 5, 10, 10);
                DeleteObject(blue);
                DeleteObject(bluepen);
            } else {
                COLORREF edge = RGB(60,60,60);
                HPEN ring = CreatePen(PS_SOLID, 1, edge);
                SelectObject(hdc, ring);
                SelectObject(hdc, white);
                Ellipse(hdc, 1, 1, 14, 14);
                DeleteObject(ring);
            }
        }
        SelectObject(hdc, old);
        DeleteDC(hdc);
        ImageList_Add(himl, hbm, NULL);
        DeleteObject(hbm);
    }
    ReleaseDC(NULL, screen);
    if (theme) CloseThemeData(theme);
    TreeView_SetImageList(g.hTree, himl, TVSIL_STATE);
    /* Keep the stock ~20px row height without scaling the glyphs. */
    TreeView_SetItemHeight(g.hTree, 20);
}
static void build_tree(void)
{
    int i, have_root = 0;
    HTREEITEM root_handle = NULL;
    int root_comp = -1;
    g.item_count = 0;
    for (i = 0; i < g_manifest.ncomp; ++i) {
        const op22_comp *c = &g_manifest.comps[i];
        int is_root = 1, j;
        if (c->parent && c->parent[0]) {
            for (j = 0; j < g_manifest.ncomp; ++j)
                if (strcmp(g_manifest.comps[j].name, c->parent) == 0) break;
            if (j < g_manifest.ncomp) is_root = 0;
        }
        if (!is_root) continue;
        /* main and addons are both top-level entries in the stock tree. */
        (void)have_root; (void)root_handle; (void)root_comp;
        add_comp_node(i, TVI_ROOT, -1);
    }
    /* Fix child counts (extra roots were re-parented) and expand everything,
     * matching the reference installer's fully expanded tree. */
    for (i = 0; i < g.item_count; ++i) {
        TVITEMW ti;
        memset(&ti, 0, sizeof(ti));
        ti.hItem = g.item_handle[i];
        ti.mask = TVIF_CHILDREN;
        ti.cChildren = TreeView_GetChild(g.hTree, g.item_handle[i]) ? 1 : 0;
        TreeView_SetItem(g.hTree, &ti);
    }
    for (i = 0; i < g.item_count; ++i)
        TreeView_Expand(g.hTree, g.item_handle[i], TVE_EXPAND);
    update_mask_and_status();
}

/* ------------------------------------------------------------------ */
/*  Painting                                                          */
/* ------------------------------------------------------------------ */
static void draw_text(HDC hdc, const char *utf8, int x, int y, int w, int h,
                      HFONT font, COLORREF color, UINT flags)
{
    WCHAR buf[2048];
    RECT rc = {x, y, x + w, y + h};
    utf8w(utf8, buf, 2048);
    SelectObject(hdc, font);
    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, buf, -1, &rc, flags);
}

static void draw_wtext(HDC hdc, const WCHAR *s, int x, int y, int w, int h,
                       HFONT font, COLORREF color, UINT flags)
{
    RECT rc = {x, y, x + w, y + h};
    SelectObject(hdc, font);
    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, s, -1, &rc, flags);
}

static void draw_folder_icon(HDC hdc, int x, int y)
{
    /* Use the system folder icon so it is recognisably a folder, with a
     * hand-drawn tabbed folder as fallback. */
    SHFILEINFOW sfi;
    memset(&sfi, 0, sizeof(sfi));
    if (SHGetFileInfoW(L"folder", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
                       SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES)) {
        DrawIconEx(hdc, x, y, sfi.hIcon, 32, 32, 0, NULL, DI_NORMAL);
        DestroyIcon(sfi.hIcon);
        return;
    }
    {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(150, 110, 30));
        HPEN oldpen = SelectObject(hdc, pen);
        HBRUSH front = CreateSolidBrush(RGB(255, 221, 130));
        HBRUSH back = CreateSolidBrush(RGB(235, 190, 80));
        HBRUSH oldbr = SelectObject(hdc, back);
        Rectangle(hdc, x + 2, y + 4, x + 20, y + 9);
        SelectObject(hdc, front);
        RoundRect(hdc, x + 1, y + 8, x + 30, y + 28, 2, 2);
        SelectObject(hdc, oldpen);
        SelectObject(hdc, oldbr);
        DeleteObject(pen);
        DeleteObject(front);
        DeleteObject(back);
    }
}

static void paint_main(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc;
    RECT rc;
    HPEN line;

    hdc = BeginPaint(hwnd, &ps);
    GetClientRect(hwnd, &rc);

    /* Fill the whole client area first.  FillRect does not stroke an outline
     * (Rectangle used to leave black edge bars along both sides). */
    FillRect(hdc, &rc, g.brush_body);

    /* header */
    {
        RECT head = {0, 0, CLIENT_W, HEADER_H};
        FillRect(hdc, &head, (HBRUSH)GetStockObject(WHITE_BRUSH));
    }

    /* Banner at the header's top-right.  The small wizard image is square
     * (108x108), so keep its aspect ratio instead of stretching it into a
     * tall narrow box. */
    load_wizard_assets(g.hInst);
    if (g_manifest.wizard_small_bmp && g_manifest.wizard_small_bmp_len)
        /* Inno Setup: WizardSmallBitmapImage is square, 58x58, anchored to
         * the top-right corner of the header (ClientWidth - 58, top 0). */
        draw_bmp(hdc, g_manifest.wizard_small_bmp,
                 g_manifest.wizard_small_bmp_len, CLIENT_W - 58, 1, 58, 58);
    else if (g_manifest.wizard_bmp && g_manifest.wizard_bmp_len)
        draw_bmp_clipped(hdc, g_manifest.wizard_bmp,
                         g_manifest.wizard_bmp_len, CLIENT_W - 48, 0, 48, 59);

    /* separators */
    line = CreatePen(PS_SOLID, 1, RGB(160, 160, 160));
    SelectObject(hdc, line);
    MoveToEx(hdc, 0, HEADER_H - 1, NULL); LineTo(hdc, CLIENT_W, HEADER_H - 1);
    MoveToEx(hdc, 0, BODY_BOT, NULL); LineTo(hdc, CLIENT_W, BODY_BOT);
    DeleteObject(line);

    /* footer copyright */
    {
        const char *buf = g_manifest.copyright ? g_manifest.copyright
                                               : "Copyright (C) 2022 OP-2.2 Team";
        draw_text(hdc, buf, 0, BODY_BOT + 1, CLIENT_W, 14,
                  g.font_small, RGB(128, 128, 128), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }

    /* page title + subtitle */
    {
        const char *title = NULL, *sub = NULL;
        switch (g.page) {
        case PG_LANG:   title = g_manifest.s_lang_title;  sub = g_manifest.s_lang_label; break;
        case PG_DEST:   title = g_manifest.s_dest_title;  sub = g_manifest.s_dest_label; break;
        case PG_COMP:   title = g_manifest.s_comp_title;  sub = g_manifest.s_comp_label; break;
        case PG_TASKS:  title = g_manifest.s_tasks_title; sub = g_manifest.s_tasks_label; break;
        case PG_READY:  title = g_manifest.s_ready_title; sub = g_manifest.s_ready_label; break;
        case PG_INSTALL:title = g_manifest.s_installing;  sub = ""; break;
        case PG_FINISH: title = g_manifest.s_finish_title; sub = g_manifest.s_finish_label; break;
        default: break;
        }
        if (title) draw_text(hdc, title, 16, 11, CLIENT_W - 70, 22,
                             g.font_title, RGB(0, 0, 0), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        if (sub)   draw_text(hdc, sub, 32, 31, CLIENT_W - 90, 18,
                             g.font_ui, RGB(0, 0, 0), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }

    /* page-specific labels */
    switch (g.page) {
    case PG_LANG:
        /* The label is already drawn in the header (s_lang_label); drawing
         * it here overlapped the combo box in the original build. */
        break;
    case PG_DEST:
        draw_folder_icon(hdc, 38, 70);
        {
            char buf[512];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "Setup will install %s into the following folder.",
                        g_manifest.app_name ? g_manifest.app_name : "Cumulative Pack 2.2");
            draw_text(hdc, buf, 80, 70, 390, 20,
                      g.font_ui, RGB(0,0,0), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        }
        draw_text(hdc, "To continue, click Next. If you would like to select a different folder, click Browse.",
                  32, 112, 440, 20, g.font_ui, RGB(0,0,0), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        draw_wtext(hdc, g.status, 32, 452, CONTENT_W, 20,
                   g.font_ui, RGB(0,0,0), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        break;
    case PG_COMP:
        draw_text(hdc, "Select the components you want to install; clear the components you do not want to install.",
                  32, 72, 430, 18, g.font_ui, RGB(0,0,0), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        draw_text(hdc, "Click Next when you are ready to continue.",
                  32, 88, 430, 18, g.font_ui, RGB(0,0,0), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        if (g.preview_variant >= 0 && IsWindowVisible(g.hPreview)) {
            char buf[64];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "Look #%d",
                        g.preview_variant + 1);
            draw_text(hdc, buf, 32, 450, 200, 18,
                      g.font_ui, RGB(0,0,0), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        }
        draw_wtext(hdc, g.status, 16, 466, CONTENT_W + 16, 20,
                   g.font_ui, RGB(0,0,0), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        break;
    case PG_TASKS:
        draw_text(hdc, "Select additional tasks:", 32, 84, 420, 20,
                  g.font_ui, RGB(0,0,0), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        break;
    case PG_READY:
        draw_text(hdc, g_manifest.s_ready_label, 32, 84, 420, 60,
                  g.font_ui, RGB(0,0,0), DT_LEFT | DT_TOP);
        draw_text(hdc, "Worker threads:", 32, 183, 112, 20,
                  g.font_ui, RGB(0,0,0), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        break;
    case PG_INSTALL:
        draw_text(hdc, g_manifest.s_installing, 32, 110, 420, 20,
                  g.font_ui, RGB(0,0,0), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        break;
    case PG_FINISH:
        draw_text(hdc, g_manifest.s_finish_label, 32, 90, 420, 60,
                  g.font_ui, RGB(0,0,0), DT_LEFT | DT_TOP);
        break;
    default: break;
    }

    EndPaint(hwnd, &ps);
}

/* ------------------------------------------------------------------ */
/*  Tree custom draw: right-aligned size column                       */
/* ------------------------------------------------------------------ */
/* Exact size labels from the stock component tree.  An empty string means
 * the stock tree leaves the size column blank for that row. */
static const char *stock_size_label(const char *name)
{
    if (!name) return NULL;
    if (!strcmp(name, "main")) return "35.792,1 MB";
    if (!strcmp(name, "addons")) return "14,7 MB";
    if (!strcmp(name, "addons\\arms")) return "14,7 MB";
    if (!strcmp(name, "addons\\arms\\arm1")) return "14,7 MB";
    if (!strcmp(name, "addons\\arms\\arm2")) return "14,1 MB";
    if (!strcmp(name, "addons\\arms\\arm3")) return "13,1 MB";
    if (!strcmp(name, "addons\\sounds")) return "";
    if (!strcmp(name, "addons\\sounds\\streamers")) return "";
    if (!strcmp(name, "addons\\sounds\\streamers\\strict")) return "2,2 MB";
    if (!strcmp(name, "addons\\sounds\\streamers\\more_strict")) return "51,4 MB";
    if (!strcmp(name, "addons\\sounds\\cont")) return "";
    if (!strcmp(name, "addons\\sounds\\cont\\cont1")) return "0,1 MB";
    if (!strcmp(name, "addons\\sounds\\cont\\cont2")) return "0,1 MB";
    if (!strcmp(name, "addons\\sounds\\cont\\cont3")) return "0,2 MB";
    return NULL;
}

static void draw_tree_sizes(NMTVCUSTOMDRAW *cd)
{
    HTREEITEM h = (HTREEITEM)cd->nmcd.dwItemSpec;
    int ii = item_index(h);
    char sz[64];
    WCHAR w[64];
    RECT rc, rcClient;
    int right;
    if (ii < 0 || g.item_comp[ii] < 0 || g.item_comp[ii] >= g_manifest.ncomp)
        return;
    if (!TreeView_GetItemRect(g.hTree, h, &rc, TRUE)) return;
    GetClientRect(g.hTree, &rcClient);
    right = rcClient.right;
    {
        const char *label = stock_size_label(g_manifest.comps[g.item_comp[ii]].name);
        if (label && !label[0]) return;
        if (label) _snprintf_s(sz, sizeof(sz), _TRUNCATE, "%s", label);
        else format_size_utf8(sz, sizeof(sz),
                              g_manifest.comps[g.item_comp[ii]].size);
    }
    utf8w(sz, w, 64);
    {
        RECT fill = {right - 78, rc.top, right, rc.bottom};
        HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
        FillRect(cd->nmcd.hdc, &fill, white);
    }
    {
        RECT t = {right - 76, rc.top, right - 4, rc.bottom};
        SelectObject(cd->nmcd.hdc, g.font_ui);
        SetTextColor(cd->nmcd.hdc, RGB(0, 0, 0));
        SetBkMode(cd->nmcd.hdc, TRANSPARENT);
        DrawTextW(cd->nmcd.hdc, w, -1, &t,
                  DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }
}

/* ------------------------------------------------------------------ */
/*  Page / button management                                          */
/* ------------------------------------------------------------------ */
static void set_button_text(HWND h, const char *utf8)
{
    WCHAR buf[128];
    utf8w(utf8, buf, 128);
    SetWindowTextW(h, buf);
}

static void set_page(Page p)
{
    g.page = p;
    hide_preview();

#define SHOW(h, v) ShowWindow((h), (v) ? SW_SHOW : SW_HIDE)
    SHOW(g.hEdit, p == PG_DEST);
    SHOW(g.hBrowse, p == PG_DEST);
    SHOW(g.hComboLang, p == PG_LANG);
    SHOW(g.hComboType, p == PG_COMP);
    SHOW(g.hTree, p == PG_COMP);
    SHOW(g.hChkDesktop, p == PG_TASKS);
    SHOW(g.hRadioCommon, p == PG_TASKS);
    SHOW(g.hRadioCurrent, p == PG_TASKS);
    SHOW(g.hProgress, p == PG_INSTALL);
    SHOW(g.hStats, p == PG_INSTALL);
    SHOW(g.hComboThreads, p == PG_READY);
    SHOW(g.hBack, p == PG_COMP || p == PG_TASKS || p == PG_READY);
    SHOW(g.hNext, p != PG_INSTALL);
    SHOW(g.hCancel, p != PG_FINISH);
#undef SHOW
    EnableWindow(g.hBack, (p == PG_COMP || p == PG_TASKS || p == PG_READY));
    EnableWindow(g.hNext, (p != PG_INSTALL));
    EnableWindow(g.hCancel, (p != PG_INSTALL && p != PG_FINISH));

    if (p == PG_READY) set_button_text(g.hNext, g_manifest.s_install);
    else if (p == PG_FINISH) set_button_text(g.hNext, g_manifest.s_finish_btn);
    else set_button_text(g.hNext, g_manifest.s_next);

    InvalidateRect(g.hwnd, NULL, TRUE);
    UpdateWindow(g.hwnd);
}

/* ------------------------------------------------------------------ */
/*  Debug install telemetry                                           */
/* ------------------------------------------------------------------ */
static void init_disk_telemetry(const char *output_dir)
{
    WCHAR path[128];
    char letter = 0;
    g.hPdh = NULL;
    g.hDisk = NULL;
    g.hWrites = NULL;
    g.disk_invert = 0;
    g.disk_util = 0.0;
    g.disk_letter = 0;
    if (!output_dir || !isalpha((unsigned char)output_dir[0]) || output_dir[1] != ':')
        return;                       /* UNC or unknown -> no counter */
    letter = (char)toupper((unsigned char)output_dir[0]);
    g.disk_letter = (WCHAR)letter;
    if (PdhOpenQueryW(NULL, 0, &g.hPdh) != ERROR_SUCCESS) {
        g.hPdh = NULL;
        return;
    }
    swprintf(path, 128, L"\\LogicalDisk(%c:)\\%% Disk Time", (WCHAR)letter);
    if (PdhAddCounterW(g.hPdh, path, 0, &g.hDisk) != ERROR_SUCCESS) {
        /* Localised systems may not expose the English counter name. */
        swprintf(path, 128, L"\\LogicalDisk(%c:)\\%% Idle Time", (WCHAR)letter);
        if (PdhAddCounterW(g.hPdh, path, 0, &g.hDisk) != ERROR_SUCCESS) {
            PdhCloseQuery(g.hPdh);
            g.hPdh = NULL;
            return;
        }
        g.disk_invert = 1;
    }
    /* True bytes written to the target volume; if unavailable the decoded
     * throughput is used as a fallback. */
    swprintf(path, 128, L"\\LogicalDisk(%c:)\\Disk Write Bytes/sec", (WCHAR)letter);
    PdhAddCounterW(g.hPdh, path, 0, &g.hWrites);
    PdhCollectQueryData(g.hPdh);      /* establish a baseline sample */
}

static void close_disk_telemetry(void)
{
    if (g.hPdh) {
        PdhCloseQuery(g.hPdh);
        g.hPdh = NULL;
        g.hDisk = NULL;
        g.hWrites = NULL;
    }
}

static double poll_disk_stats(double *write_mbs)
{
    PDH_FMT_COUNTERVALUE val;
    double v = -1.0;
    *write_mbs = -1.0;
    if (!g.hPdh || !g.hDisk)
        return -1.0;
    if (PdhCollectQueryData(g.hPdh) != ERROR_SUCCESS)
        return -1.0;
    if (PdhGetFormattedCounterValue(g.hDisk, PDH_FMT_DOUBLE, NULL, &val) == ERROR_SUCCESS) {
        v = val.doubleValue;
        if (g.disk_invert) v = 100.0 - v;
        if (v < 0.0) v = 0.0;
        if (v > 100.0) v = 100.0;
    }
    if (g.hWrites &&
        PdhGetFormattedCounterValue(g.hWrites, PDH_FMT_DOUBLE, NULL, &val) == ERROR_SUCCESS)
        *write_mbs = val.doubleValue / 1048576.0;
    return v;   /* raw sample; averaged over 1s by the caller */
}

/* Keep a ~1 second sliding window of instantaneous samples. */
static void push_telemetry(double tp, double wr, double du)
{
    TelemetrySample *s = &g.samples[g.sample_head];
    s->t = GetTickCount();
    s->tp = tp;
    s->wr = wr;
    s->du = du;
    g.sample_head = (g.sample_head + 1) % (int)(sizeof(g.samples) / sizeof(g.samples[0]));
    if (g.sample_count < (int)(sizeof(g.samples) / sizeof(g.samples[0])))
        ++g.sample_count;
}

static void average_telemetry(double *tp_avg, double *wr_avg, double *du_avg)
{
    DWORD now = GetTickCount();
    DWORD cutoff = now > 1000u ? now - 1000u : 0u;
    int i, idx, n = 0;
    double tp_sum = 0.0, wr_sum = 0.0, du_sum = 0.0;
    int wr_n = 0;
    *tp_avg = 0.0;
    *wr_avg = 0.0;
    *du_avg = 0.0;
    for (i = 0; i < g.sample_count; ++i) {
        idx = (g.sample_head - 1 - i + (int)(sizeof(g.samples) / sizeof(g.samples[0])))
              % (int)(sizeof(g.samples) / sizeof(g.samples[0]));
        if (g.samples[idx].t < cutoff)
            break;
        tp_sum += g.samples[idx].tp;
        du_sum += g.samples[idx].du;
        if (g.samples[idx].wr >= 0.0) {
            wr_sum += g.samples[idx].wr;
            ++wr_n;
        }
        ++n;
    }
    if (n) {
        *tp_avg = tp_sum / n;
        *du_avg = du_sum / n;
    }
    if (wr_n) *wr_avg = wr_sum / wr_n;
}

static void refresh_stats_text(void)
{
    op22_thread_status st[64];
    int n, i, active = 0;
    WCHAR buf[512];
    double writes = (g.writes_mbs >= 0.0) ? g.writes_mbs : g.throughput_mbs;
    n = op22_get_thread_status(st, (int)(sizeof(st) / sizeof(st[0])));
    if (n > 0) {
        for (i = 0; i < n; ++i)
            if (st[i].busy) ++active;
        g.last_active = active;
        g.last_total = n;
    } else {
        active = g.last_active;
        n = g.last_total;
    }
    if (g.hPdh && g.hDisk)
        _snwprintf_s(buf, 512, _TRUNCATE,
                     L"Disk writes:   %6.1f MB/s\r\n"
                     L"Disk activity: %5.1f%%\r\n"
                     L"Active Threads: %d/%d\r\n",
                     writes, g.disk_util, active, n);
    else
        _snwprintf_s(buf, 512, _TRUNCATE,
                     L"Disk writes:   %6.1f MB/s\r\n"
                     L"Disk activity: unavailable\r\n"
                     L"Active Threads: %d/%d\r\n",
                     writes, active, n);
    SetWindowTextW(g.hStats, buf);
}

/* ------------------------------------------------------------------ */
/*  Installation                                                      */
/* ------------------------------------------------------------------ */
static void CALLBACK progress_cb(int done, int total, void *user)
{
    PostMessage((HWND)user, WM_APP_PROGRESS, (WPARAM)done, (LPARAM)total);
}

static DWORD WINAPI install_thread(LPVOID param)
{
    char out[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, g.dest, -1, out, MAX_PATH, NULL, NULL);
    g.errbuf[0] = 0;
    g.install_result = op22_install(g.source_dir, out, g.comp_mask,
                                    g.task_mask, progress_cb, g.hwnd,
                                    g.errbuf, sizeof(g.errbuf));
    if (g.install_result == 0) {
        g.shortcuts_result = op22_create_shortcuts(out, g.task_mask,
                                                   g.errbuf, sizeof(g.errbuf));
    }
    PostMessage(g.hwnd, WM_APP_DONE, 0, 0);
    return 0;
}

static void start_install(void)
{
    char outpath[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, g.dest, -1, outpath, MAX_PATH, NULL, NULL);
    g.install_result = -1;
    g.shortcuts_result = -1;
    g.progress_done = g.progress_total = 0;
    g.last_bytes = 0;
    g.last_tick = 0;
    g.throughput_mbs = 0.0;
    g.writes_mbs = -1.0;
    g.disk_util = 0.0;
    g.last_active = g.last_total = 0;
    g.sample_head = 0;
    g.sample_count = 0;
    {
        int sel = (int)SendMessageW(g.hComboThreads, CB_GETCURSEL, 0, 0);
        g.worker_limit = (sel > 0) ? sel : 0;   /* item 0 = Auto */
        op22_set_worker_limit(g.worker_limit);
    }
    SetWindowTextW(g.hStats, L"");
    init_disk_telemetry(outpath);
    SendMessage(g.hProgress, PBM_SETRANGE32, 0, 100);
    SendMessage(g.hProgress, PBM_SETPOS, 0, 0);
    g.install_thread = CreateThread(NULL, 0, install_thread, NULL, 0, NULL);
    set_page(PG_INSTALL);
}

/* ------------------------------------------------------------------ */
/*  Browse callback                                                   */
/* ------------------------------------------------------------------ */
static int CALLBACK browse_cb(HWND h, UINT msg, LPARAM lp, LPARAM data)
{
    if (msg == BFFM_INITIALIZED)
        SendMessageW(h, BFFM_SETSELECTIONW, TRUE, data);
    return 0;
}

static void browse_dest(void)
{
    BROWSEINFOW bi;
    LPITEMIDLIST pidl;
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = g.hwnd;
    bi.lpszTitle = L"Select destination folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn = browse_cb;
    bi.lParam = (LPARAM)g.dest;
    pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        SHGetPathFromIDListW(pidl, g.dest);
        CoTaskMemFree(pidl);
        SetWindowTextW(g.hEdit, g.dest);
    }
}

/* ------------------------------------------------------------------ */
/*  WM_CREATE                                                         */
/* ------------------------------------------------------------------ */
static HFONT set_font(HWND h, HFONT f)
{
    SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
    return f;
}

static HWND make_button(int id, const char *text, int x, int y, int w, int h)
{
    WCHAR buf[128];
    HWND hw = CreateWindowExW(0, L"BUTTON", L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                              x, y, w, h, g.hwnd, (HMENU)(INT_PTR)id,
                              g.hInst, NULL);
    utf8w(text, buf, 128);
    SetWindowTextW(hw, buf);
    set_font(hw, g.font_ui);
    return hw;
}

static int wnd_create(HWND hwnd, LPCREATESTRUCTW cs)
{
    INITCOMMONCONTROLSEX icc;
    WCHAR pf[MAX_PATH];
    WCHAR srcw[MAX_PATH];

    g.hwnd = hwnd;
    g.hInst = cs->hInstance;
    g.page = PG_DEST;
    g.preview_variant = -1;

    /* Segoe UI ~9pt (13px).  Calibrated against the reference dialog: the
     * sentence wraps after "during" exactly as in the stock installer. */
    /* Inno Setup's .dfm uses Font.Height = -11 with MS Sans Serif (8pt). */
    g.font_ui = make_font(-11, FW_NORMAL);
    g.font_title = make_font(-11, FW_BOLD);
    g.font_small = make_font(-11, FW_NORMAL);
    g.font_mono = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                              DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    g.brush_body = CreateSolidBrush(RGB(240, 240, 240));
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        g.nprocs = (int)si.dwNumberOfProcessors;
        if (g.nprocs < 1) g.nprocs = 1;
    }

    memset(&icc, 0, sizeof(icc));
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_TREEVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    /* default destination: <Program Files (x86)>\<default_dir> */
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILESX86, NULL, 0, pf))) {
        WCHAR dd[256];
        utf8w(g_manifest.default_dir ? g_manifest.default_dir : "OP-2.2", dd, 256);
        _snwprintf_s(g.default_dest, MAX_PATH, _TRUNCATE, L"%s\\%s", pf, dd);
    } else {
        utf8w(g_manifest.default_dir ? g_manifest.default_dir : "OP-2.2",
              g.default_dest, MAX_PATH);
    }
    wcscpy_s(g.dest, MAX_PATH, g.default_dest);

    /* native control windows */
    g.hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                              WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                              32, 134, 321, 21, hwnd, (HMENU)(INT_PTR)ID_EDIT_DEST,
                              g.hInst, NULL);
    SetWindowTextW(g.hEdit, g.dest);
    set_font(g.hEdit, g.font_ui);

    g.hBrowse = make_button(ID_BTN_BROWSE, g_manifest.s_browse, BTN_CX, 134, 75, 21);

    g.hComboLang = CreateWindowExW(0, L"COMBOBOX", L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                   CBS_DROPDOWNLIST | WS_VSCROLL,
                                   32, 100, 220, 21, hwnd,
                                   (HMENU)(INT_PTR)ID_COMBO_LANG, g.hInst, NULL);
    SendMessageW(g.hComboLang, CB_ADDSTRING, 0, (LPARAM)L"English");
    SendMessageW(g.hComboLang, CB_ADDSTRING, 0, (LPARAM)L"Русский");
    SendMessageW(g.hComboLang, CB_SETCURSEL, 0, 0);
    set_font(g.hComboLang, g.font_ui);

    g.hComboType = CreateWindowExW(0, L"COMBOBOX", L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                   CBS_DROPDOWNLIST | WS_VSCROLL,
                                   32, 116, CONTENT_W, 21, hwnd,
                                   (HMENU)(INT_PTR)ID_COMBO_TYPE, g.hInst, NULL);
    SendMessageW(g.hComboType, CB_ADDSTRING, 0, (LPARAM)L"Typical installation");
    SendMessageW(g.hComboType, CB_ADDSTRING, 0, (LPARAM)L"Full installation");
    SendMessageW(g.hComboType, CB_ADDSTRING, 0, (LPARAM)L"Custom installation");
    SendMessageW(g.hComboType, CB_SETCURSEL, 2, 0);
    set_font(g.hComboType, g.font_ui);

    g.hTree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                              TVS_HASLINES | TVS_LINESATROOT |           
                              TVS_SHOWSELALWAYS | TVS_NOHSCROLL,
                              32, 140, CONTENT_W, 300, hwnd,
                              (HMENU)(INT_PTR)ID_TREE, g.hInst, NULL);
    make_state_imagelist();
    set_font(g.hTree, g.font_ui);
    build_tree();
    g.old_tree_proc = (WNDPROC)SetWindowLongPtrW(g.hTree, GWLP_WNDPROC,
                                                 (LONG_PTR)TreeProc);

    g.hChkDesktop = CreateWindowExW(0, L"BUTTON", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                    BS_AUTOCHECKBOX,
                                    32, 110, CONTENT_W, 20, hwnd,
                                    (HMENU)(INT_PTR)ID_CHK_DESKTOP, g.hInst, NULL);
    set_button_text(g.hChkDesktop, g_manifest.s_desktop_task);
    set_font(g.hChkDesktop, g.font_ui);
    SendMessageW(g.hChkDesktop, BM_SETCHECK, BST_CHECKED, 0);

    g.hRadioCommon = CreateWindowExW(0, L"BUTTON", L"",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP |
                                     BS_AUTORADIOBUTTON,
                                     56, 146, CONTENT_W - 24, 20, hwnd,
                                     (HMENU)(INT_PTR)ID_RADIO_COMMON, g.hInst, NULL);
    set_button_text(g.hRadioCommon, g_manifest.s_common);
    set_font(g.hRadioCommon, g.font_ui);

    g.hRadioCurrent = CreateWindowExW(0, L"BUTTON", L"",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                      BS_AUTORADIOBUTTON,
                                      56, 174, CONTENT_W - 24, 20, hwnd,
                                      (HMENU)(INT_PTR)ID_RADIO_CURRENT, g.hInst, NULL);
    set_button_text(g.hRadioCurrent, g_manifest.s_current);
    set_font(g.hRadioCurrent, g.font_ui);
    SendMessageW(g.hRadioCurrent, BM_SETCHECK, BST_CHECKED, 0);

    g.hProgress = CreateWindowExW(0, PROGRESS_CLASSW, L"",
                                  WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                  32, 150, CONTENT_W, 22, hwnd,
                                  (HMENU)(INT_PTR)ID_PROGRESS, g.hInst, NULL);
    set_font(g.hProgress, g.font_ui);

    /* Condensed install telemetry. */
    g.hStats = CreateWindowExW(0, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | WS_BORDER |
                               ES_MULTILINE | ES_READONLY | ES_NOHIDESEL,
                               32, 184, CONTENT_W, 72, hwnd,
                               (HMENU)(INT_PTR)ID_STATS, g.hInst, NULL);
    SendMessageW(g.hStats, WM_SETFONT, (WPARAM)g.font_mono, TRUE);

    /* Worker-count selector on the Ready page; item 0 is Auto (= nprocs). */
    g.hComboThreads = CreateWindowExW(0, L"COMBOBOX", L"",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                      CBS_DROPDOWNLIST | WS_VSCROLL,
                                      150, 180, 140, 21, hwnd,
                                      (HMENU)(INT_PTR)ID_THREADS, g.hInst, NULL);
    {
        WCHAR label[64];
        int i;
        _snwprintf_s(label, 64, _TRUNCATE, L"Auto (%d threads)", g.nprocs);
        SendMessageW(g.hComboThreads, CB_ADDSTRING, 0, (LPARAM)label);
        for (i = 1; i <= g.nprocs; ++i) {
            _snwprintf_s(label, 64, _TRUNCATE, L"%d", i);
            SendMessageW(g.hComboThreads, CB_ADDSTRING, 0, (LPARAM)label);
        }
        SendMessageW(g.hComboThreads, CB_SETCURSEL, 0, 0);
        set_font(g.hComboThreads, g.font_ui);
    }

    g.hBack = make_button(ID_BTN_BACK, g_manifest.s_back, BTN_BX, BTN_Y, BTN_W, BTN_H);
    g.hNext = make_button(ID_BTN_NEXT, g_manifest.s_next, BTN_NX, BTN_Y, BTN_W, BTN_H);
    g.hCancel = make_button(ID_BTN_CANCEL, g_manifest.s_cancel, BTN_CX, BTN_Y, BTN_W, BTN_H);

    /* preview popup (top-level, owned by the wizard) */
    {
        WNDCLASSW wc;
        RECT r = {0, 0, PREVIEW_CW, PREVIEW_CH};
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = g.hInst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
        wc.lpszClassName = L"OP22PreviewClass";
        RegisterClassW(&wc);
        AdjustWindowRectEx(&r, WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_BORDER,
                           FALSE, 0);
        g.preview_outer_w = r.right - r.left;
        g.preview_outer_h = r.bottom - r.top;
        g.hPreview = CreateWindowExW(WS_EX_TOOLWINDOW, L"OP22PreviewClass", L"",
                                     WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_BORDER,
                                     0, 0, g.preview_outer_w, g.preview_outer_h,
                                     hwnd, NULL, g.hInst, NULL);
        g.old_preview_proc = (WNDPROC)SetWindowLongPtrW(
            g.hPreview, GWLP_WNDPROC, (LONG_PTR)PreviewProc);
    }

    g.task_mask = (1u << OP22_TASK_DESKTOP) | (1u << OP22_TASK_CURRENT);

    /* UTF-8 source path for display/debug */
    srcw[0] = 0;
    MultiByteToWideChar(CP_UTF8, 0, g.source_dir, -1, srcw, MAX_PATH);

    /* honour --show-components: caller sets the initial page after create */
    set_page(g.page);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Main window procedure                                             */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_CREATE:
        return wnd_create(hwnd, (LPCREATESTRUCTW)l);

    case WM_PAINT:
        paint_main(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_COMMAND: {
        WORD id = LOWORD(w);
        WORD code = HIWORD(w);
        if (id == ID_BTN_BROWSE && code == BN_CLICKED) { browse_dest(); return 0; }
        if (id == ID_BTN_BACK && code == BN_CLICKED) {
            if (g.page == PG_COMP) set_page(PG_DEST);
            else if (g.page == PG_TASKS) set_page(PG_COMP);
            else if (g.page == PG_READY) set_page(PG_TASKS);
            return 0;
        }
        if (id == ID_BTN_NEXT && code == BN_CLICKED) {
            switch (g.page) {
            case PG_LANG:   set_page(PG_DEST); break;
            case PG_DEST:   set_page(PG_COMP); break;
            case PG_COMP:   set_page(PG_TASKS); break;
            case PG_TASKS:  set_page(PG_READY); break;
            case PG_READY:  start_install(); break;
            case PG_FINISH: DestroyWindow(hwnd); break;
            default: break;
            }
            return 0;
        }
        if (id == ID_BTN_CANCEL && code == BN_CLICKED) {
            if (g.page != PG_INSTALL && g.page != PG_FINISH &&
                confirm_exit() != IDYES)
                return 0;
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == ID_COMBO_LANG && code == CBN_SELCHANGE) {
            g.lang = (int)SendMessageW(g.hComboLang, CB_GETCURSEL, 0, 0);
            return 0;
        }
        if (id == ID_CHK_DESKTOP && code == BN_CLICKED) {
            int on = (SendMessageW(g.hChkDesktop, BM_GETCHECK, 0, 0) == BST_CHECKED);
            EnableWindow(g.hRadioCommon, on);
            EnableWindow(g.hRadioCurrent, on);
            if (on) {
                int common = (SendMessageW(g.hRadioCommon, BM_GETCHECK, 0, 0) == BST_CHECKED);
                g.task_mask = (1u << OP22_TASK_DESKTOP) |
                    (common ? (1u << OP22_TASK_COMMON) : (1u << OP22_TASK_CURRENT));
            } else {
                g.task_mask = 0;
            }
            return 0;
        }
        if (id == ID_RADIO_COMMON && code == BN_CLICKED) {
            g.task_mask = (1u << OP22_TASK_DESKTOP) | (1u << OP22_TASK_COMMON);
            return 0;
        }
        if (id == ID_RADIO_CURRENT && code == BN_CLICKED) {
            g.task_mask = (1u << OP22_TASK_DESKTOP) | (1u << OP22_TASK_CURRENT);
            return 0;
        }
        if (id == ID_COMBO_TYPE && code == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(g.hComboType, CB_GETCURSEL, 0, 0);
            int i;
            if (sel == 0) { /* typical: all boxes, first radio per group */
                for (i = 0; i < g.item_count; ++i) {
                    int ci = g.item_comp[i];
                    if (ci >= 0 && (g_manifest.comps[ci].flags & 2))
                        set_item_state(i, (g.item_state[i] == 3) ? 3 : 2);
                    else if (ci >= 0 && !(g_manifest.comps[ci].flags & 1))
                        set_item_state(i, 1);
                }
            } else if (sel == 1) { /* full */
                for (i = 0; i < g.item_count; ++i) {
                    int ci = g.item_comp[i];
                    if (ci >= 0 && !(g_manifest.comps[ci].flags & 2))
                        set_item_state(i, 1);
                }
            }
            update_mask_and_status();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, w, l);
    }

    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)l;
        if (nm->idFrom == ID_TREE && nm->code == NM_CUSTOMDRAW) {
            NMTVCUSTOMDRAW *cd = (NMTVCUSTOMDRAW *)nm;
            switch (cd->nmcd.dwDrawStage) {
            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT:
                return CDRF_NOTIFYPOSTPAINT;
            case CDDS_ITEMPOSTPAINT:
                draw_tree_sizes(cd);
                return CDRF_DODEFAULT;
            }
        }
        return DefWindowProcW(hwnd, msg, w, l);
    }

    case WM_APP_PROGRESS: {
        DWORD now = GetTickCount();
        uint64_t bytes = (uint64_t)(int)w * 1024u;
        g.progress_done = (int)w;
        g.progress_total = (int)l;
        if (g.progress_total > 0) {
            SendMessageW(g.hProgress, PBM_SETRANGE32, 0, g.progress_total);
            SendMessageW(g.hProgress, PBM_SETPOS, g.progress_done, 0);
        }
        {
            double mbs = 0.0;
            if (g.last_tick && now > g.last_tick && bytes >= g.last_bytes) {
                double dt = (double)(now - g.last_tick) / 1000.0;
                mbs = (double)(bytes - g.last_bytes) / 1048576.0 / dt;
            }
            g.last_bytes = bytes;
            g.last_tick = now;
            double wr = -1.0;
            double du = poll_disk_stats(&wr);
            push_telemetry(mbs, wr, du);
            average_telemetry(&g.throughput_mbs, &g.writes_mbs, &g.disk_util);
        }
        refresh_stats_text();
        return 0;
    }

    case WM_APP_DONE: {
        WCHAR msg[2048], cap[128];
        refresh_stats_text();       /* preserve the final snapshot */
        close_disk_telemetry();
        if (g.install_thread) {
            CloseHandle(g.install_thread);
            g.install_thread = NULL;
        }
        if (g.install_result != 0) {
            utf8w(g.errbuf, msg, 2048);
            utf8w("Installation failed", cap, 128);
            MessageBoxW(hwnd, msg, cap, MB_ICONERROR | MB_OK);
            set_page(PG_READY);
        } else {
            if (g.shortcuts_result != 0) {
                utf8w(g.errbuf, msg, 2048);
                utf8w("Shortcut warning", cap, 128);
                MessageBoxW(hwnd, msg, cap, MB_ICONWARNING | MB_OK);
            }
            set_page(PG_FINISH);
        }
        return 0;
    }

    case WM_DESTROY:
        if (g.install_thread) {
            CloseHandle(g.install_thread);
            g.install_thread = NULL;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

/* Exit confirmation shown by Cancel on every pre-installation page. */
static int confirm_exit(void)
{
    WCHAR text[512], cap[128];
    utf8w("Setup is not complete. If you exit now, the program will not be "
          "installed.\n\nYou may run Setup again at another time to complete "
          "the installation.\n\nExit Setup?", text, 512);
    utf8w("Exit Setup", cap, 128);
    return MessageBoxW(g.hwnd, text, cap,
                       MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 |
                       MB_SETFOREGROUND);
}

/* ------------------------------------------------------------------ */
/*  Modal "Select Setup Language" dialog.                            */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK LangProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT cr;
        RECT z = {0, 0, 0, 0};
        int fx, fy;
        AdjustWindowRectEx(&z, WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_BORDER,
                           FALSE, 0);
        fx = -z.left;
        fy = -z.top;
        GetClientRect(hwnd, &cr);

        FillRect(hdc, &cr, (HBRUSH)GetStockObject(WHITE_BRUSH));
        {
            HICON icon = (HICON)LoadImageW(g.hInst, MAKEINTRESOURCE(IDI_APP),
                                           IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
            if (icon) {
                DrawIconEx(hdc, 14 - fx, 37 - fy, icon, 32, 32, 0, NULL, DI_NORMAL);
                DestroyIcon(icon);
            }
        }
        {
            RECT t = {59 - fx, 37 - fy, 59 - fx + 234, 37 - fy + 46};
            WCHAR txt[256];
            SelectObject(hdc, g.font_ui);
            SetTextColor(hdc, RGB(0, 0, 0));
            SetBkMode(hdc, TRANSPARENT);
            MultiByteToWideChar(CP_UTF8, 0,
                "Select the language to use during the installation.", -1,
                txt, 256);
            DrawTextW(hdc, txt, -1, &t, DT_LEFT | DT_TOP | DT_WORDBREAK);
        }
        {
            HPEN line = CreatePen(PS_SOLID, 1, RGB(160, 160, 160));
            HPEN old = SelectObject(hdc, line);
            int y = 122 - fy;
            MoveToEx(hdc, 0, y, NULL);
            LineTo(hdc, cr.right, y);
            SelectObject(hdc, old);
            DeleteObject(line);
        }
        {
            RECT foot = {0, 122 - fy + 1, cr.right, cr.bottom};
            FillRect(hdc, &foot, (HBRUSH)GetSysColorBrush(COLOR_BTNFACE));
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_COMMAND: {
        WORD id = LOWORD(w);
        if (id == ID_LANG_OK) { g.lang_result = 1; DestroyWindow(hwnd); return 0; }
        if (id == ID_LANG_CANCEL) {
            if (confirm_exit() == IDYES) {
                g.lang_result = 0;
                DestroyWindow(hwnd);
            }
            return 0;
        }
        if (id == ID_LANG_COMBO && HIWORD(w) == CBN_SELCHANGE)
            g.lang = (int)SendMessageW(GetDlgItem(hwnd, ID_LANG_COMBO),
                                       CB_GETCURSEL, 0, 0);
        return 0;
    }
    case WM_CLOSE:
        g.lang_result = 0;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

static int show_language_dialog(HWND parent, HINSTANCE hInst)
{
    WNDCLASSW wc;
    HWND dlg, combo, ok, cancel;
    RECT z = {0, 0, 0, 0};
    RECT pr;
    int fx, fy, fw, fh;
    int w, h, x, y;
    MSG msg;
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_BORDER;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = LangProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCE(IDI_APP));
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"OP22LangClass";
    RegisterClassW(&wc);

    AdjustWindowRectEx(&z, style, FALSE, 0);
    fx = -z.left; fy = -z.top;
    fw = z.right - z.left; fh = z.bottom - z.top;
    w = 306; h = 174;
    GetWindowRect(parent, &pr);
    x = pr.left + (pr.right - pr.left - w) / 2;
    y = pr.top + (pr.bottom - pr.top - h) / 2;

    g.lang_result = -1;
    dlg = CreateWindowExW(0, L"OP22LangClass", L"Select Setup Language",
                          style, x, y, w, h, parent, NULL, hInst, NULL);
    if (!dlg) return 0;
    g.hLangDlg = dlg;
    {
        HICON small_icon = (HICON)LoadImageW(hInst, MAKEINTRESOURCE(IDI_APP),
                                             IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
        if (small_icon)
            SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)small_icon);
    }

    combo = CreateWindowExW(0, L"COMBOBOX", L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                            CBS_DROPDOWNLIST | WS_VSCROLL,
                            59 - fx, 86 - fy, 233, 21, dlg,
                            (HMENU)(INT_PTR)ID_LANG_COMBO, hInst, NULL);
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"English");
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"\x0420\x0443\x0441\x0441\x043A\x0438\x0439");
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
    SendMessageW(combo, WM_SETFONT, (WPARAM)g.font_ui, TRUE);

    ok = CreateWindowExW(0, L"BUTTON", L"OK",
                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                         137 - fx, 136 - fy, 70, 23, dlg,
                         (HMENU)(INT_PTR)ID_LANG_OK, hInst, NULL);
    SendMessageW(ok, WM_SETFONT, (WPARAM)g.font_ui, TRUE);
    cancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                             215 - fx, 136 - fy, 69, 23, dlg,
                             (HMENU)(INT_PTR)ID_LANG_CANCEL, hInst, NULL);
    SendMessageW(cancel, WM_SETFONT, (WPARAM)g.font_ui, TRUE);

    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    SetForegroundWindow(dlg);
    SetFocus(combo);

    while (IsWindow(dlg) && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            g.lang_result = 1;
            break;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            g.lang_result = 0;
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (IsWindow(dlg)) DestroyWindow(dlg);
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    g.hLangDlg = NULL;
    return g.lang_result == 1;
}

/* ------------------------------------------------------------------ */
int ui_run(HINSTANCE hInst, int nCmdShow, const char *source_dir,
           int show_components)
{
    WNDCLASSW wc;
    HWND hwnd;
    MSG msg;
    RECT r = {0, 0, CLIENT_W, CLIENT_H};

    memset(&g, 0, sizeof(g));
    strncpy_s(g.source_dir, MAX_PATH, source_dir ? source_dir : "",
              _TRUNCATE);

    memset(&wc, 0, sizeof(wc));
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCE(IDI_APP));
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"OP22WizardClass";
    if (!RegisterClassW(&wc)) return 1;

    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
    {
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        int ww = r.right - r.left;
        int wh = r.bottom - r.top;
        int x = (sw - ww) / 2;
        int y = (sh - wh) / 2;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        hwnd = CreateWindowExW(0, L"OP22WizardClass",
                               L"Setup - Cumulative Pack 2.2",
                               WS_OVERLAPPEDWINDOW, x, y, ww, wh,
                               NULL, NULL, hInst, NULL);
    }
    if (!hwnd) return 1;

    /* The language chooser is a small modal dialog, shown before the wizard
     * itself.  It is centered on the (still hidden) main window. */
    if (!show_components && !show_language_dialog(hwnd, hInst)) {
        DestroyWindow(hwnd);
        return 0;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    if (show_components) set_page(PG_COMP);
    else set_page(PG_DEST);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
