#define WINVER       0x0601
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <stdarg.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dlc.h"
#include "logcap.h"
#include "swapreq.h"

/* GDI+ flat API declarations for item previews; mingw's gdiplus.h is C++ only. */
typedef struct {
    UINT32 GdiplusVersion;
    void  *DebugEventCallback;
    BOOL   SuppressBackgroundThread;
    BOOL   SuppressExternalCodecs;
} GpStartupInput;

int  WINAPI GdiplusStartup(ULONG_PTR *token, const GpStartupInput *in, void *out);
void WINAPI GdiplusShutdown(ULONG_PTR token);
int  WINAPI GdipLoadImageFromFile(const WCHAR *file, void **image);
int  WINAPI GdipDisposeImage(void *image);
int  WINAPI GdipGetImageWidth(void *image, UINT *w);
int  WINAPI GdipGetImageHeight(void *image, UINT *h);
int  WINAPI GdipCreateFromHDC(HDC hdc, void **graphics);
int  WINAPI GdipDeleteGraphics(void *graphics);
int  WINAPI GdipSetInterpolationMode(void *graphics, int mode);
int  WINAPI GdipSetPixelOffsetMode(void *graphics, int mode);
int  WINAPI GdipDrawImageRectI(void *graphics, void *image, int x, int y,
                               int w, int h);
int  WINAPI GdipCreateBitmapFromScan0(INT w, INT h, INT stride, INT format,
                                      BYTE *scan0, void **bitmap);
#define GDIP_NEAREST   5     /* InterpolationModeNearestNeighbor */
#define GDIP_BICUBIC   7     /* InterpolationModeHighQualityBicubic */
#define GDIP_HALFPIXEL 2     /* PixelOffsetModeHalf */
#define GDIP_24BPP_RGB 0x21808   /* PixelFormat24bppRGB */

#define APP_TITLE L"Tamagotchi Color Launcher"
#define EMU_EXE   L"tamaemu-sdl.exe"
#define EMU_LOG   L"emu_run.log"
#define LOG_DIR   L"logs"
#define LIB_DIR   L"tamagotchi_dlc"

enum {
    IDC_ROM = 1001, IDC_ROM_BR, IDC_SAV, IDC_SAV_BR, IDC_SAV_MATCH,
    IDC_LIB, IDC_LIB_BR, IDC_AWAKE, IDC_BUTTONS,
    IDC_TABS, IDC_LIST,
    IDC_REFRESH, IDC_ADD, IDC_INSTALL, IDC_PLAY,
    IDC_STATUS, IDC_HINT, IDC_WELCOME, IDC_DEVICE,
    IDC_ONTOP,
    IDC_KEY_A, IDC_KEY_B, IDC_KEY_C, /* appended; consecutive - code does IDC_KEY_A + i */
    IDC_HELP_AGAIN,                 
    IDC_RESUME
};

/* Button glyphs use the system UI font. U+1F4C1 needs a UTF-16 surrogate pair. */
#define GLYPH_FOLDER  L"\xD83D\xDCC1"
#define GLYPH_REFRESH L"\x21BB"
#define GLYPH_DOWN    L"\x2193"
#define GLYPH_PLAY    L"\x25B6"

/* Per-user settings, including one DLC directory per device. */
#define REG_KEY     L"Software\\maragotchi\\tama-launcher"
#define REG_WELCOME L"WelcomeSeen"
#define REG_BUTTONS L"ShowButtons"
#define REG_ONTOP   L"AlwaysOnTop"
#define REG_RESUME  L"ResumeLastSession"
#define REG_KEYS    L"Keys"

/* Layout uses 96-DPI units and right-anchored rows; S() scales at startup. */
#define CLIENTW   780
#define M         10
#define LBLX      M
#define LBLW      86
#define EDX       (M + LBLW + 4)
#define EDH       22
#define BRW       56                        /* folder glyph + "..." */
#define MATCHW    86
#define MATCHX    (CLIENTW - M - MATCHW)
#define BRX       (MATCHX - 4 - BRW)
#define EDW       (BRX - 4 - EDX)
#define HELPW     32
#define HELPX     (CLIENTW - M - HELPW)
/* Rows with narrow buttons extend their edit boxes to fill the gap. */
#define ROMBRX    (HELPX - 4 - BRW)
#define ROMEDW    (ROMBRX - 4 - EDX)
#define LIBBRX    (CLIENTW - M - BRW)
#define LIBEDW    (LIBBRX - 4 - EDX)

/* Device tabs span the top edge and include room for the underline. */
#define DEVSY     0
#define DEVSH     26

#define ROW0      (DEVSY + DEVSH + 4)
#define ROWSTEP   28
#define OPTY      (ROW0 + 3*ROWSTEP)        /* the two checkboxes */
#define HINTY     (OPTY + 32)
#define TABSY     (HINTY + 22)
#define TABSH     398             /* item grid height */
#define BTNY      (TABSY + TABSH + 10)
#define STATLY    (BTNY + 36)
#define STATY     (STATLY + 20)
#define STATH     112             /* two columns: 7 lines covers the 4U's 13 stores */
#define CLIENTH   (STATY + STATH + M)

static int   g_dpi = 96;
#define S(x)  MulDiv((x), g_dpi, 96)

/* State */
static HWND      g_main, g_devtabs, g_tabs, g_list, g_status;
static HFONT     g_font, g_mono, g_bold;
static wchar_t   g_root[MAX_PATH];         /* folder holding tamaemu-sdl.exe */
static const DlcDevice *g_dev;             /* selected machine; never NULL */
static DlcItem  *g_items;                  /* the whole library, all tabs */
static int       g_nitems;
static unsigned char *g_checked;           /* parallel to g_items */
static int       g_populating;             /* suppress LVN_ITEMCHANGED echo */
static int       g_save_follows_rom = 1;
static int       g_resume = 1;
static wchar_t   g_devnote[160];           /* "Save looks like ...", or "" */
static int       g_tab_has_sets;           /* the visible tab shows a "[n]" tile */
static HIMAGELIST g_imgs;                  /* the visible tab's thumbnails */
static ULONG_PTR  g_gdip;

/* Helpers */

/* The one encoding boundary: UTF-16 UI <-> the ANSI paths dlc.c's fopen wants. */
static void w2a(const wchar_t *w, char *out, int outsz)
{
    if (WideCharToMultiByte(CP_ACP, 0, w, -1, out, outsz, NULL, NULL) == 0)
        out[0] = '\0';
}
static void a2w(const char *a, wchar_t *out, int outsz)
{
    if (MultiByteToWideChar(CP_ACP, 0, a, -1, out, outsz) == 0)
        out[0] = L'\0';
}

/* Content names are UTF-8. Invalid input falls back to ANSI. */
static void u2w(const char *u, wchar_t *out, int outsz)
{
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u, -1,
                            out, outsz) == 0)
        a2w(u, out, outsz);
}

static int file_exists(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static void join(wchar_t *out, int outsz, const wchar_t *dir, const wchar_t *leaf)
{
    _snwprintf(out, (size_t)outsz, L"%s\\%s", dir, leaf);
    out[outsz - 1] = L'\0';
}

static void get_text(int id, wchar_t *out, int outsz)
{
    GetDlgItemTextW(g_main, id, out, outsz);
}
static void set_text(int id, const wchar_t *s)
{
    SetDlgItemTextW(g_main, id, s);
}

/* Append to a wide string. Clamp _snwprintf's negative truncation result. */
static int wappend(wchar_t *buf, int cap, int at, const wchar_t *fmt, ...)
{
    if (at < 0 || at >= cap - 1) return at < 0 ? 0 : cap - 1;
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf(buf + at, (size_t)(cap - at - 1), fmt, ap);
    va_end(ap);
    if (n < 0) at = cap - 1;                /* truncated - buffer is now full */
    else if (at + n > cap - 1) at = cap - 1;
    else at += n;
    buf[at] = L'\0';
    return at;
}

/* Unchecked box asks for a cold start. */
static int append_resume_flag(wchar_t *cmd, int cap, int at, int resume)
{
    return resume ? at : wappend(cmd, cap, at, L" --restart");
}

#ifdef LAUNCHER_DEBUG
static void launcher_debug_resume_check(void)
{
    wchar_t cmd[32] = L"tamaemu-sdl.exe";
    int at = (int)wcslen(cmd);
    assert(append_resume_flag(cmd, 32, at, 1) == at);
    assert(!wcscmp(cmd, L"tamaemu-sdl.exe"));
    at = append_resume_flag(cmd, 32, at, 0);
    assert(at == (int)wcslen(L"tamaemu-sdl.exe --restart"));
    assert(!wcscmp(cmd, L"tamaemu-sdl.exe --restart"));
}
#endif

static void say(UINT icon, const wchar_t *title, const wchar_t *fmt, ...)
{
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, sizeof buf / sizeof buf[0] - 1, fmt, ap);
    va_end(ap);
    buf[sizeof buf / sizeof buf[0] - 1] = L'\0';
    MessageBoxW(g_main, buf, title, MB_OK | icon);
}

#define EMU_LOG_LIMIT (2ULL * 1024ULL * 1024ULL)

typedef struct LogCapture {
    HANDLE read;
    LogCap log;
} LogCapture;

static DWORD WINAPI log_capture_thread(void *arg)
{
    LogCapture *capture = (LogCapture *)arg;
    char buf[64 * 1024];
    DWORD got;

    while (ReadFile(capture->read, buf, sizeof buf, &got, NULL) && got) {
        /* Keep draining even if the disk becomes unavailable, so the child
         * cannot deadlock on a full pipe. */
        (void)logcap_write(&capture->log, buf, got);
    }
    CloseHandle(capture->read);
    logcap_close(&capture->log);
    free(capture);
    return 0;
}

/* Welcome dialog */

/* Missing registry values return the caller's default. */
static int reg_get(const wchar_t *name, int dflt)
{
    HKEY k;
    DWORD v = 0, sz = sizeof v;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return dflt;
    if (RegQueryValueExW(k, name, NULL, NULL, (BYTE *)&v, &sz) != ERROR_SUCCESS) {
        RegCloseKey(k);
        return dflt;
    }
    RegCloseKey(k);
    return (int)v;
}

static void reg_set(const wchar_t *name, int v)   /* stores the value, not a flag */
{
    HKEY k;
    /* Registry write failures are nonfatal; the setting lasts for this session. */
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_WRITE,
                        NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    DWORD dv = (DWORD)v;
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&dv, sizeof dv);
    RegCloseKey(k);
}

/* Leave room for a terminator when reading REG_SZ values. Empty means unset. */
static void reg_get_str(const wchar_t *name, wchar_t *out, int outsz)
{
    HKEY k;
    memset(out, 0, (size_t)outsz * sizeof *out);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return;
    DWORD type = 0, sz = (DWORD)(outsz - 1) * sizeof *out;
    if (RegQueryValueExW(k, name, NULL, &type, (BYTE *)out, &sz) != ERROR_SUCCESS
        || type != REG_SZ)
        out[0] = L'\0';
    RegCloseKey(k);
}

static void reg_set_str(const wchar_t *name, const wchar_t *v)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_WRITE,
                        NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)v,
                   (DWORD)((wcslen(v) + 1) * sizeof *v));
    RegCloseKey(k);
}

/* A/B/C bindings use one three-character REG_SZ. S and N are reserved. */
static wchar_t g_keys[4] = L"ZXC";   /* display case; lowercased for --keys */
static int     g_key_capture = -1;   /* 0..2 while a Keys button waits for a press */

static int key_char_ok(wchar_t c)
{
    if (c == L'S' || c == L'N') return 0;
    return (c >= L'A' && c <= L'Z') || (c >= L'1' && c <= L'9');
}

/* Malformed saved bindings fall back to the default. */
static void load_keys(void)
{
    wchar_t v[8];
    reg_get_str(REG_KEYS, v, 8);
    for (int i = 0; i < 3; i++)
        if (v[i] >= L'a' && v[i] <= L'z') v[i] = (wchar_t)(v[i] - 32);
    if (wcslen(v) != 3 || !key_char_ok(v[0]) || !key_char_ok(v[1])
        || !key_char_ok(v[2]) || v[0] == v[1] || v[0] == v[2] || v[1] == v[2])
        return;
    wcscpy(g_keys, v);
}

static HWND g_help_wnd;   /* the How-to-use dialog; the key buttons' parent */

static void key_capture_end(int commit, wchar_t c)
{
    if (g_key_capture < 0) return;
    int i = g_key_capture;
    g_key_capture = -1;
    if (commit) {
        g_keys[i] = c;
        reg_set_str(REG_KEYS, g_keys);
    }
    wchar_t t[2] = { g_keys[i], 0 };
    SetDlgItemTextW(g_help_wnd, IDC_KEY_A + i, t);
}

static LRESULT CALLBACK key_btn_proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp,
                                     UINT_PTR sub, DWORD_PTR ref)
{
    (void)ref;
    int i = (int)sub;                          /* 0=A 1=B 2=C */
    switch (msg) {
    case WM_GETDLGCODE:
        /* IsDialogMessageW would otherwise eat the keypress as a mnemonic */
        if (g_key_capture == i) return DLGC_WANTALLKEYS;
        break;
    case WM_KEYDOWN:
        if (g_key_capture == i) {
            if (wp == VK_ESCAPE) { key_capture_end(0, 0); return 0; }
            /* Letter VK and SDL codes agree; digit keys may vary by layout. */
            wchar_t c = ((wp >= 'A' && wp <= 'Z') || (wp >= '1' && wp <= '9'))
                        ? (wchar_t)wp : 0;
            if (!c || !key_char_ok(c)
                || c == g_keys[(i + 1) % 3] || c == g_keys[(i + 2) % 3]) {
                MessageBeep(MB_OK);            /* reserved, taken, or off-charset */
                key_capture_end(0, 0);
                return 0;
            }
            key_capture_end(1, c);
            return 0;                          /* swallowed: no button "click" */
        }
        break;
    case WM_CHAR:
        if (g_key_capture == i) return 0;      /* Space must not re-click the button */
        break;
    case WM_KILLFOCUS:
        if (g_key_capture == i) key_capture_end(0, 0);
        break;
    }
    return DefSubclassProc(hw, msg, wp, lp);
}

/* How-to-use dialog with Intro, ROMs & DLC, and Controls tabs. */

#define HELP_PAGES  3
#define HELP_MAXCTL 14

static HWND g_help_page[HELP_PAGES][HELP_MAXCTL];
static int  g_help_nctl[HELP_PAGES];

static const wchar_t HELP_INTRO[] =
    L"Thanks for downloading!\n\n"
    L"This emulator works for Tamagotchi Color versions from Plus Color to "
    L"the 4U+. Anything released after the 4U+ will not be supported on "
    L"this emulator.\n\n"
    L"ROMs are NOT included.\n\n"
    /* Escapes keep the source ASCII. */
    L"Have fun! (\xFF89\x25D5\x30EE\x25D5)\xFF89" L"*:\xFF65\xFF9F\x2727";

static const wchar_t HELP_IMPORT[] =
    L"Select the three dots next to the first bar and then select the ROM "
    L"you wish to play.\n\n"
    L"A .sav will be generated for your ROM, piggy backing off of the "
    L"original name of it.\n\n"
    L"Please make sure you're loading ROMs into the correct device tab! "
    L"The IDL emulator supports all IDL kinds, so you can load a Princess "
    L"Spacy ROM or an IDLE ROM.";

static const wchar_t HELP_DLC1[] =
    L"Point the DLC selector to the folder that houses your DLC.";
/* Use a separate label so the warning can be bold. */
static const wchar_t HELP_DLC_WARN[] =
    L"The folder can ONLY contain items for the device you have selected!";
static const wchar_t HELP_DLC2[] =
    L"Seriously, don't mix and match unless you're 100% certain it works "
    L"on normal hardware. Each tab remembers your decision for later and "
    L"does not change unless you change it. You can install more than one "
    L"item, and selections are remembered between tabs.";

static void help_show_page(int page)
{
    for (int p = 0; p < HELP_PAGES; p++)
        for (int i = 0; i < g_help_nctl[p]; i++)
            ShowWindow(g_help_page[p][i], p == page ? SW_SHOW : SW_HIDE);
}

/* Pages 0-2 build tab visibility lists; page -1 is always visible. */
static HWND help_mk(int page, const wchar_t *cls, const wchar_t *text,
                    DWORD style, int x, int y, int w, int h, int id, HFONT f)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | style,
                             S(x), S(y), S(w), S(h), g_help_wnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : g_font), TRUE);
    if (page >= 0 && g_help_nctl[page] < HELP_MAXCTL)
        g_help_page[page][g_help_nctl[page]++] = c;
    return c;
}

static LRESULT CALLBACK help_wndproc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CTLCOLORSTATIC:
        /* Keep child controls white with the dialog background. */
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_NOTIFY: {
        LPNMHDR nh = (LPNMHDR)lp;
        if (nh->code == TCN_SELCHANGE)
            help_show_page(TabCtrl_GetCurSel(nh->hwndFrom));
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: case IDCANCEL:       /* OK, Enter, or Esc via IsDialogMessage */
            PostMessageW(hw, WM_CLOSE, 0, 0);
            return 0;
        case IDC_KEY_A: case IDC_KEY_B: case IDC_KEY_C: {
            int i = LOWORD(wp) - IDC_KEY_A;
            if (g_key_capture == i) return 0;         /* already waiting */
            if (g_key_capture >= 0) key_capture_end(0, 0);
            g_key_capture = i;
            SetDlgItemTextW(hw, LOWORD(wp), L"...");
            SetFocus(GetDlgItem(hw, LOWORD(wp)));
            return 0;
        }
        }
        return 0;
    case WM_CLOSE:
        /* OK, Esc and the X agree: the checkbox is what it says it is */
        reg_set(REG_WELCOME,
                IsDlgButtonChecked(hw, IDC_HELP_AGAIN) == BST_CHECKED);
        DestroyWindow(hw);
        return 0;
    case WM_DESTROY:
        g_key_capture = -1;             /* a capture cannot outlive its button */
        g_help_wnd = NULL;
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

static void show_welcome(void)
{
    static int registered;
    if (!registered) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc   = help_wndproc;
        wc.hInstance     = GetModuleHandleW(NULL);
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"tamaemu_help";
        RegisterClassW(&wc);
        registered = 1;
    }

    memset(g_help_nctl, 0, sizeof g_help_nctl);

    RECT wr = { 0, 0, S(470), S(428) };
    AdjustWindowRect(&wr, WS_CAPTION | WS_SYSMENU | WS_POPUP, FALSE);
    RECT mr;
    GetWindowRect(g_main, &mr);
    int x = mr.left + ((mr.right - mr.left) - (wr.right - wr.left)) / 2;
    int y = mr.top  + ((mr.bottom - mr.top) - (wr.bottom - wr.top)) / 3;

    g_help_wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"tamaemu_help",
                                 L"How to use",
                                 WS_CAPTION | WS_SYSMENU | WS_POPUP,
                                 x, y, wr.right - wr.left, wr.bottom - wr.top,
                                 g_main, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_help_wnd) {
        /* Fall back to plain help text if tab creation fails. */
        say(MB_ICONINFORMATION, APP_TITLE, L"%s", HELP_INTRO);
        return;
    }

    HWND tabs = CreateWindowExW(0, WC_TABCONTROLW, L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                0, 0, S(470), S(26),
                                g_help_wnd, NULL, GetModuleHandleW(NULL), NULL);
    SendMessageW(tabs, WM_SETFONT, (WPARAM)g_font, TRUE);
    const wchar_t *names[HELP_PAGES] =
        { L"Intro", L"ROMS && DLC", L"Controls" };
    for (int i = 0; i < HELP_PAGES; i++) {
        TCITEMW ti;
        memset(&ti, 0, sizeof ti);
        ti.mask    = TCIF_TEXT;
        ti.pszText = (wchar_t *)names[i];
        TabCtrl_InsertItem(tabs, i, &ti);
    }

    help_mk(0, L"STATIC", HELP_INTRO,  0, 14, 40, 442, 236, 0, NULL);
    help_mk(1, L"STATIC", HELP_IMPORT, 0, 14, 40, 442, 150, 0, NULL);
    help_mk(1, L"STATIC", L"To Install DLC:", 0, 14, 196, 442, 18, 0, g_bold);
    help_mk(1, L"STATIC", HELP_DLC1,     0, 14, 218, 442, 16, 0, NULL);
    help_mk(1, L"STATIC", HELP_DLC_WARN, 0, 14, 236, 442, 32, 0, g_bold);
    help_mk(1, L"STATIC", HELP_DLC2, 0, 14, 270, 442, 64, 0, NULL);

    /* Key bindings use their own aligned column. */
    help_mk(2, L"STATIC",
            L"Speed up:\nSlow down:\nBack to real time:",
            0, 14, 52, 120, 54, 0, NULL);
    help_mk(2, L"STATIC", L"+\n-\n0", 0, 140, 52, 40, 54, 0, NULL);
    help_mk(2, L"STATIC", L"ESC quits.", 0, 14, 116, 120, 18, 0, NULL);

    help_mk(2, L"STATIC", L"Experimental, can be buggy:",
            0, 14, 156, 220, 18, 0, g_bold);
    help_mk(2, L"STATIC",
            L"Toggle NFC/Touch:\nToggle always on:",
            0, 14, 178, 120, 36, 0, NULL);
    help_mk(2, L"STATIC", L"N\nS", 0, 140, 178, 40, 36, 0, NULL);

    help_mk(2, L"STATIC", L"Rebind Controls", 0, 280, 52, 130, 18, 0, g_bold);
    for (int i = 0; i < 3; i++) {
        wchar_t lab[3] = { L"ABC"[i], L':', 0 };
        help_mk(2, L"STATIC", lab, 0, 280, 80 + i * 28, 20, 18, 0, NULL);
        wchar_t t[2] = { g_keys[i], 0 };
        HWND kb = help_mk(2, L"BUTTON", t, WS_TABSTOP | BS_PUSHBUTTON,
                          306, 78 + i * 28, 26, 22, IDC_KEY_A + i, NULL);
        SetWindowSubclass(kb, key_btn_proc, (UINT_PTR)i, 0);
    }

    help_mk(-1, L"STATIC", L"", WS_VISIBLE | SS_ETCHEDHORZ,
            14, 358, 442, 2, 0, NULL);
    help_mk(-1, L"STATIC",
            L"You can read this again at any time with the  ?  button.",
            WS_VISIBLE, 14, 366, 442, 16, 0, NULL);
    help_mk(-1, L"BUTTON", L"Don't show this again",
            WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            14, 392, 180, 20, IDC_HELP_AGAIN, NULL);
    help_mk(-1, L"BUTTON", L"OK",
            WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            372, 390, 84, 26, IDOK, NULL);

    /* Pre-tick a hidden welcome so the checkbox can turn it back on. */
    CheckDlgButton(g_help_wnd, IDC_HELP_AGAIN,
                   reg_get(REG_WELCOME, 0) ? BST_CHECKED : BST_UNCHECKED);
    help_show_page(0);

    ShowWindow(g_help_wnd, SW_SHOW);
    EnableWindow(g_main, FALSE);
    MSG m;
    while (g_help_wnd && GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_help_wnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    EnableWindow(g_main, TRUE);
    SetForegroundWindow(g_main);
}

/* Paths */

/* Use the executable folder, or its parent when launched from tools/. */
static void find_root(void)
{
    wchar_t exe[MAX_PATH], probe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *slash = wcsrchr(exe, L'\\');
    if (slash) *slash = L'\0';
    wcscpy(g_root, exe);

    join(probe, MAX_PATH, g_root, EMU_EXE);
    if (file_exists(probe)) return;

    slash = wcsrchr(g_root, L'\\');
    if (slash) {
        wchar_t up[MAX_PATH];
        *slash = L'\0';
        wcscpy(up, g_root);
        join(probe, MAX_PATH, up, EMU_EXE);
        if (file_exists(probe)) return;
        *slash = L'\\';
    }
}

/* Device names become alphanumeric keys for per-device ROM settings. */
static void dev_key(const DlcDevice *d, wchar_t *out, size_t outsz)
{
    size_t k = 0;
    for (const char *p = d ? d->name : "ps"; *p && k + 1 < outsz; p++)
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9'))
            out[k++] = (wchar_t)*p;
    out[k] = L'\0';
}

static void rom_setting_for(const DlcDevice *d, wchar_t *out, size_t outsz)
{
    wchar_t key[32];
    dev_key(d, key, 32);
    _snwprintf(out, outsz, L"RomPath_%s", key);
    out[outsz - 1] = L'\0';
}

/* Remember one DLC folder per device. */
static void dlc_memo_for(const DlcDevice *d, wchar_t *out, size_t outsz)
{
    wchar_t key[32];
    dev_key(d, key, 32);
    _snwprintf(out, outsz, L"DlcDir_%s", key);
    out[outsz - 1] = L'\0';
}

/* Use the device's remembered folder, or tamagotchi_dlc beside the executable. */
static void default_lib(const DlcDevice *d, wchar_t *out, int outsz)
{
    wchar_t name[64];
    dlc_memo_for(d, name, 64);
    reg_get_str(name, out, outsz);
    if (out[0] && GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return;
    join(out, outsz, g_root, LIB_DIR);
}

/* Save only existing library folders. */
static void remember_lib(const DlcDevice *d, const wchar_t *lib)
{
    DWORD a = lib[0] ? GetFileAttributesW(lib) : INVALID_FILE_ATTRIBUTES;
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return;
    wchar_t name[64];
    dlc_memo_for(d, name, 64);
    reg_set_str(name, lib);
}

/* Use TAMAEMU_ROM first, then this device's saved ROM path. */
static void default_rom(const DlcDevice *d, wchar_t *out, int outsz)
{
    out[0] = L'\0';

    wchar_t env[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"TAMAEMU_ROM", env, MAX_PATH);
    if (n > 0 && n < MAX_PATH && file_exists(env)) {
        wcsncpy(out, env, (size_t)outsz - 1);
        out[outsz - 1] = L'\0';
        return;
    }

    wchar_t name[64];
    rom_setting_for(d, name, 64);
    reg_get_str(name, out, outsz);
    if (out[0] && !file_exists(out)) out[0] = L'\0';
}

/* Save only existing ROM paths. */
static void remember_rom(const DlcDevice *d, const wchar_t *rom)
{
    if (!rom[0] || !file_exists(rom)) return;
    wchar_t name[64];
    rom_setting_for(d, name, 64);
    reg_set_str(name, rom);
}

/* Browse dialogs */

static int pick_file(const wchar_t *title, const wchar_t *filter,
                     wchar_t *path, int pathsz)
{
    wchar_t buf[MAX_PATH];
    wcsncpy(buf, path, MAX_PATH - 1);
    buf[MAX_PATH - 1] = L'\0';

    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner   = g_main;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = title;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER
                    | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return 0;
    wcsncpy(path, buf, (size_t)pathsz - 1);
    path[pathsz - 1] = L'\0';
    return 1;
}

static int pick_folder(const wchar_t *title, wchar_t *path, int pathsz)
{
    BROWSEINFOW bi;
    wchar_t out[MAX_PATH];
    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = g_main;
    bi.pszDisplayName = out;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
    LPITEMIDLIST idl = SHBrowseForFolderW(&bi);
    if (!idl) return 0;
    int ok = SHGetPathFromIDListW(idl, out);
    CoTaskMemFree(idl);
    if (!ok) return 0;
    wcsncpy(path, out, (size_t)pathsz - 1);
    path[pathsz - 1] = L'\0';
    return 1;
}

/* Item list */

static const wchar_t *current_tab_name(wchar_t *buf, int bufsz)
{
    int sel = TabCtrl_GetCurSel(g_tabs);
    buf[0] = L'\0';
    if (sel >= 0 && sel < dlc_tab_count(g_dev))
        a2w(dlc_tab_at(g_dev, sel), buf, bufsz);
    return buf;
}

/* Wallpapers use a wide preview; other records use square previews. */
static void tab_box(const char *tab, int *w, int *h)
{
    if (!strcmp(tab, "Wallpapers")) { *w = 128; *h = 80; }
    else                            { *w =  92; *h = 92; }
}

/* 4U sprites store dimensions, max index, flags, a big-endian palette, then
 * pixels. 4bpp uses the low nibble first; larger palettes use 8bpp. */

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

/* Return a valid sprite block's unpadded size, or zero. */
static size_t icon_body_ok(const uint8_t *p, size_t n, size_t off)
{
    if (off + 8 > n - 2) return 0;
    unsigned w = p[off], h = p[off+1], mx = p[off+2];
    if (w < 4 || w > 160 || h < 4 || h > 160 || mx < 1) return 0;
    if (be16(p + off + 4) != 0x01FF) return 0;
    size_t f = 4 + 2 * (mx + 1) + (mx <= 16 ? (w * h + 1) / 2 : w * h);
    if (off + f > n - 2) return 0;
    return f;
}

/* Return the first sprite block offset, or zero. */
static size_t find_4u_icon(const uint8_t *p, size_t n)
{
    if (n < 0x70 || memcmp(p, "TAMAGO", 6) != 0) return 0;
    /* Prefer a fully valid length-prefixed chain. */
    for (size_t off = 0x60; off + 2 <= n - 2; off += 2) {
        unsigned cnt = be16(p + off);
        if (cnt < 1 || cnt > 128) continue;
        size_t o = off + 2, first = 0;
        int ok = 1;
        for (unsigned i = 0; i < cnt; i++) {
            if (o + 2 > n - 2) { ok = 0; break; }
            size_t blen = be16(p + o);
            if (blen < 12 || o + 2 + blen > n - 2) { ok = 0; break; }
            size_t f = icon_body_ok(p, n, o + 2);
            if (!f || blen < f || blen >= f + 96) { ok = 0; break; }
            if (!first) first = o + 2;
            o += 2 + blen;
        }
        if (ok && first) return first;
    }
    /* Minigames and outings may contain a standalone block. */
    for (size_t off = 0x60; off + 8 <= n - 2; off++)
        if (icon_body_ok(p, n, off)) return off;
    return 0;
}

/* Decode a sprite to caller-owned, top-down 24bpp BGR with aligned rows. */
static uint8_t *decode_4u_icon(const uint8_t *p, size_t n, size_t off,
                               int *ow, int *oh, int *ostride)
{
    if (!icon_body_ok(p, n, off)) return NULL;
    unsigned w = p[off], h = p[off+1], mx = p[off+2];
    const uint8_t *pal = p + off + 4;
    const uint8_t *px  = pal + 2 * (mx + 1);
    int key = (be16(pal + 2) == 0x07E0);        /* chroma green at entry 1 */
    int stride = ((int)w * 3 + 3) & ~3;
    uint8_t *bits = malloc((size_t)stride * h);
    if (!bits) return NULL;
    memset(bits, 0xFF, (size_t)stride * h);
    for (unsigned y = 0; y < h; y++) {
        uint8_t *row = bits + (size_t)y * stride;
        for (unsigned x = 0; x < w; x++) {
            unsigned k = y * w + x, v;
            if (mx <= 16) v = (k & 1) ? (px[k / 2] >> 4) : (px[k / 2] & 0xF);
            else          v = px[k];
            if ((v == 0 && key) || v + 1 > mx) continue;
            uint16_t c = be16(pal + 2 * (v + 1));
            unsigned r =  c        & 0x1F;      /* R and B live swapped */
            unsigned g = (c >>  5) & 0x3F;
            unsigned b = (c >> 11) & 0x1F;
            row[x*3 + 0] = (uint8_t)((b << 3) | (b >> 2));
            row[x*3 + 1] = (uint8_t)((g << 2) | (g >> 4));
            row[x*3 + 2] = (uint8_t)((r << 3) | (r >> 2));
        }
    }
    *ow = (int)w; *oh = (int)h; *ostride = stride;
    return bits;
}

/* Scale tiny previews by whole numbers. Use a 4U sprite when no JPEG exists. */
static HBITMAP make_thumb(const wchar_t *path, int bw, int bh)
{
    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = bw;
    bi.bmiHeader.biHeight      = -bh;              /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp) { DeleteDC(mem); ReleaseDC(NULL, screen); return NULL; }
    HGDIOBJ old = SelectObject(mem, bmp);

    RECT r = { 0, 0, bw, bh };
    void *img = NULL;
    uint8_t *recbits = NULL;                   /* 4U icon pixels; freed last */
    int loaded = (GdipLoadImageFromFile(path, &img) == 0);
    if (!loaded) {
        /* Fall back to the first 4U sprite block. */
        char apath[DLC_PATH_MAX];
        uint8_t *rec = NULL;
        size_t reclen = 0;
        char err[DLC_ERR_MAX];
        w2a(path, apath, sizeof apath);
        if (dlc_extract_payload(apath, &rec, &reclen, err, sizeof err) == 0) {
            size_t off = find_4u_icon(rec, reclen);
            if (off) {
                int iw = 0, ih = 0, istride = 0;
                recbits = decode_4u_icon(rec, reclen, off, &iw, &ih, &istride);
                if (recbits &&
                    GdipCreateBitmapFromScan0(iw, ih, istride, GDIP_24BPP_RGB,
                                              recbits, &img) == 0)
                    loaded = 1;
            }
            free(rec);
        }
    }
    if (!loaded) {
        HBRUSH grey = CreateSolidBrush(RGB(221, 221, 221));
        FillRect(mem, &r, grey);
        DeleteObject(grey);
    } else {
        FillRect(mem, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
        UINT iw = 0, ih = 0;
        GdipGetImageWidth(img, &iw);
        GdipGetImageHeight(img, &ih);
        if (iw > 0 && ih > 0) {
            int sx = bw / (int)iw, sy = bh / (int)ih;
            int scale = sx < sy ? sx : sy;
            if (scale < 1) scale = 1;
            int tw = (int)iw * scale, th = (int)ih * scale;
            if (tw > bw || th > bh) {              /* still too big - fit it */
                double fx = (double)bw / (double)tw, fy = (double)bh / (double)th;
                double f = fx < fy ? fx : fy;
                tw = (int)(tw * f);
                th = (int)(th * f);
                if (tw < 1) tw = 1;
                if (th < 1) th = 1;
            }
            void *g = NULL;
            if (GdipCreateFromHDC(mem, &g) == 0) {
                GdipSetInterpolationMode(g, scale > 1 ? GDIP_NEAREST : GDIP_BICUBIC);
                GdipSetPixelOffsetMode(g, GDIP_HALFPIXEL);
                GdipDrawImageRectI(g, img, (bw - tw) / 2, (bh - th) / 2, tw, th);
                GdipDeleteGraphics(g);
            }
        }
        GdipDisposeImage(img);
    }
    free(recbits);                 /* only after the bitmap over it is gone */

    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    return bmp;
}

/* Character sets share a folder and a digit-ending base before their _N suffix. */

#define SET_MAX 32

/* Split parts[0] into folder and base without the final _N suffix. */
static size_t set_base_of(const DlcItem *it, char *out, size_t outsz)
{
    if (it->nparts != 1) return 0;
    const char *p = it->parts[0];
    const char *dot = strrchr(p, '.');
    size_t stem = dot ? (size_t)(dot - p) : strlen(p);
    size_t us = stem;
    while (us > 0 && p[us-1] >= '0' && p[us-1] <= '9') us--;
    if (us == 0 || us == stem || p[us-1] != '_') return 0;
    us--;                                     /* the '_' itself goes too */
    if (us == 0 || !(p[us-1] >= '0' && p[us-1] <= '9')) return 0;
    if (us >= outsz) return 0;
    memcpy(out, p, us);
    out[us] = '\0';
    return us;
}

/* Return the full set with its character first, then library order. */
static int set_of(int idx, int *members, int max)
{
    char base[DLC_PATH_MAX], other[DLC_PATH_MAX];
    if (!set_base_of(&g_items[idx], base, sizeof base)) return 0;
    int n = 0;
    for (int i = 0; i < g_nitems && n < max; i++)
        if (set_base_of(&g_items[i], other, sizeof other) &&
            strcmp(other, base) == 0)
            members[n++] = i;
    for (int i = 1; i < n; i++)
        if (!strcmp(g_items[members[i]].tab, "Characters")) {
            int c = members[i];
            memmove(members + 1, members, (size_t)i * sizeof *members);
            members[0] = c;
            break;
        }
    return n;
}

static int can_install(void)
{
    if (!g_dev || g_dev->nkinds == 0) return 0;
    for (int i = 0; i < g_nitems; i++)
        if (g_checked[i]) return 1;
    return 0;
}

static void refresh_install_enabled(void)
{
    EnableWindow(GetDlgItem(g_main, IDC_INSTALL), can_install());
}

#define SETBARH  44                /* the button strip, in 96-dpi units */

static int  g_set_members[SET_MAX];
static int  g_set_count;
static HWND g_set_wnd, g_set_list, g_set_ok;
static unsigned char g_set_was[SET_MAX];   /* ticks on entry, for Cancel */
static int  g_set_install;                 /* Install pressed, not Cancel */

static LRESULT CALLBACK set_wndproc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NOTIFY: {
        LPNMHDR nh = (LPNMHDR)lp;
        if (nh->hwndFrom == g_set_list && nh->code == NM_CLICK) {
            NMITEMACTIVATE *ia = (NMITEMACTIVATE *)lp;
            if (ia->iItem >= 0) {
                LVHITTESTINFO ht;
                memset(&ht, 0, sizeof ht);
                ht.pt = ia->ptAction;
                ListView_HitTest(g_set_list, &ht);
                if (!(ht.flags & LVHT_ONITEMSTATEICON))
                    ListView_SetCheckState(g_set_list, ia->iItem,
                        !ListView_GetCheckState(g_set_list, ia->iItem));
            }
            return 0;
        }
        if (nh->hwndFrom == g_set_list && nh->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *nl = (NMLISTVIEW *)lp;
            if (nl->uChanged & LVIF_STATE) {
                UINT o = nl->uOldState & LVIS_STATEIMAGEMASK;
                UINT n = nl->uNewState & LVIS_STATEIMAGEMASK;
                if (o != n && nl->lParam >= 0 && nl->lParam < g_nitems) {
                    g_checked[nl->lParam] = (n == INDEXTOSTATEIMAGEMASK(2));
                    EnableWindow(g_set_ok, can_install());
                }
            }
            return 0;
        }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {             /* Install, or Enter */
            g_set_install = 1;
            DestroyWindow(hw);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) {         /* Cancel, or Esc via IsDialogMessage */
            DestroyWindow(hw);
            return 0;
        }
        break;
    case WM_CLOSE:                            /* the X means Cancel too */
        DestroyWindow(hw);
        return 0;
    case WM_DESTROY:
        g_set_wnd = NULL;
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

static void open_set_dialog(int idx)
{
    g_set_count = set_of(idx, g_set_members, SET_MAX);
    if (g_set_count < 2) return;

    static int registered;
    if (!registered) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc   = set_wndproc;
        wc.hInstance     = GetModuleHandleW(NULL);
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"tamaemu_set";
        RegisterClassW(&wc);
        registered = 1;
    }

    /* Use the character member as the set name, or the clicked item. */
    int title_ix = idx;
    for (int i = 0; i < g_set_count; i++)
        if (!strcmp(g_items[g_set_members[i]].tab, "Characters"))
            { title_ix = g_set_members[i]; break; }
    wchar_t title[560], disp[512];
    u2w(g_items[title_ix].display, disp, 512);
    _snwprintf(title, 560, L"%s  \x2014  full set", disp);
    title[559] = L'\0';

    /* Taller rows leave room for two-line names. */
    int bw = S(92), bh = S(92);
    int cols = g_set_count < 4 ? g_set_count : 4;
    int rows = (g_set_count + cols - 1) / cols;
    int cw = cols * (bw + S(22)) + S(24) + GetSystemMetrics(SM_CXVSCROLL);
    /* ch tracks list height without the button strip. */
    int ch = rows * (bh + S(58)) + S(20);
    RECT wr = { 0, 0, cw, ch + S(SETBARH) };
    AdjustWindowRect(&wr, WS_CAPTION | WS_SYSMENU | WS_POPUP, FALSE);
    RECT mr;
    GetWindowRect(g_main, &mr);
    int x = mr.left + ((mr.right - mr.left) - (wr.right - wr.left)) / 2;
    int y = mr.top  + ((mr.bottom - mr.top) - (wr.bottom - wr.top)) / 3;

    g_set_wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"tamaemu_set", title,
                                WS_CAPTION | WS_SYSMENU | WS_POPUP,
                                x, y, wr.right - wr.left, wr.bottom - wr.top,
                                g_main, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_set_wnd) return;
    g_set_list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | LVS_ICON |
                                 LVS_AUTOARRANGE | LVS_SINGLESEL,
                                 0, 0, cw, ch,
                                 g_set_wnd, NULL, GetModuleHandleW(NULL), NULL);
    ListView_SetExtendedListViewStyle(g_set_list, LVS_EX_CHECKBOXES);
    SendMessageW(g_set_list, WM_SETFONT, (WPARAM)g_font, TRUE);

    /* Position buttons after the final dialog width is known. */
    g_set_ok = CreateWindowExW(0, L"BUTTON", GLYPH_DOWN L"  Install selected",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP
                               | BS_DEFPUSHBUTTON,
                               0, 0, 0, 0, g_set_wnd,
                               (HMENU)(INT_PTR)IDOK, GetModuleHandleW(NULL), NULL);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP
                                  | BS_PUSHBUTTON,
                                  0, 0, 0, 0, g_set_wnd,
                                  (HMENU)(INT_PTR)IDCANCEL,
                                  GetModuleHandleW(NULL), NULL);
    SendMessageW(g_set_ok, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(cancel,   WM_SETFONT, (WPARAM)g_font, TRUE);

    HIMAGELIST il = ImageList_Create(bw, bh, ILC_COLOR24, g_set_count, 4);
    for (int i = 0; i < g_set_count; i++) {
        int gi = g_set_members[i];
        wchar_t wpath[DLC_PATH_MAX];
        u2w(g_items[gi].display, disp, 512);
        a2w(g_items[gi].parts[0], wpath, DLC_PATH_MAX);
        int image = -1;
        HBITMAP bm = make_thumb(wpath, bw, bh);
        if (bm) { image = ImageList_Add(il, bm, NULL); DeleteObject(bm); }
        LVITEMW lv;
        memset(&lv, 0, sizeof lv);
        lv.mask    = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        lv.iItem   = i;
        lv.pszText = disp;
        lv.lParam  = (LPARAM)gi;
        lv.iImage  = image;
        ListView_InsertItem(g_set_list, &lv);
        g_set_was[i] = g_checked[gi];        /* what Cancel puts back */
        ListView_SetCheckState(g_set_list, i, g_checked[gi] ? TRUE : FALSE);
    }
    EnableWindow(g_set_ok, can_install());
    HIMAGELIST prev = ListView_SetImageList(g_set_list, il, LVSIL_NORMAL);
    if (prev && prev != il) ImageList_Destroy(prev);
    ListView_SetIconSpacing(g_set_list, bw + S(22), bh + S(38));

    /* Remeasure after resizing because the column count may change. */
    RECT work = { 0, 0, 0, 0 };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int maxw = (work.right - work.left) * 9 / 10;
    int maxh = (work.bottom - work.top) * 8 / 10;
    /* A final fit pass catches another column fold. */
    for (int pass = 0; pass < 2; pass++) {
        ListView_Arrange(g_set_list, LVA_DEFAULT);
        RECT need = { 0, 0, 0, 0 };
        for (int i = 0; i < g_set_count; i++) {
            RECT ir;
            if (!ListView_GetItemRect(g_set_list, i, &ir, LVIR_BOUNDS)) continue;
            if (ir.right  > need.right)  need.right  = ir.right;
            if (ir.bottom > need.bottom) need.bottom = ir.bottom;
        }
        if (need.right <= 0 || need.bottom <= 0) break;
        /* Reserve scrollbar width before trimming. */
        if (pass == 0) cw = need.right + S(12) + GetSystemMetrics(SM_CXVSCROLL);
        ch = need.bottom + S(12);
        /* The window height includes the button strip; ch does not. */
        RECT fit = { 0, 0, cw, ch + S(SETBARH) };
        AdjustWindowRect(&fit, WS_CAPTION | WS_SYSMENU | WS_POPUP, FALSE);
        int fw = fit.right - fit.left, fh = fit.bottom - fit.top;
        if (fw > maxw) { cw -= fw - maxw; fw = maxw; }
        if (fh > maxh) { ch -= fh - maxh; fh = maxh; }
        /* Tall sets give space to the buttons and scroll the list. */
        if (ch < S(60)) ch = S(60);
        if (cw < S(240)) cw = S(240);        /* the two buttons need this much */
        x = mr.left + ((mr.right - mr.left) - fw) / 2;
        y = mr.top  + ((mr.bottom - mr.top) - fh) / 3;
        if (x < work.left) x = work.left;
        if (y < work.top)  y = work.top;
        SetWindowPos(g_set_wnd, NULL, x, y, fw, fh, SWP_NOZORDER);
        SetWindowPos(g_set_list, NULL, 0, 0, cw, ch, SWP_NOZORDER);
        /* Right-align the buttons with Install outermost. */
        int bh2 = S(28), by = ch + (S(SETBARH) - bh2) / 2;
        int okw = S(144), cnw = S(84);
        SetWindowPos(g_set_ok, NULL, cw - S(10) - cnw - S(6) - okw, by,
                     okw, bh2, SWP_NOZORDER);
        SetWindowPos(cancel,   NULL, cw - S(10) - cnw, by,
                     cnw, bh2, SWP_NOZORDER);
    }

    ShowWindow(g_set_wnd, SW_SHOW);
    g_set_install = 0;
    EnableWindow(g_main, FALSE);
    MSG m;
    while (g_set_wnd && GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_set_wnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    EnableWindow(g_main, TRUE);
    SetForegroundWindow(g_main);

    /* Cancel, X, and Esc restore only this set's prior selections. */
    if (!g_set_install)
        for (int i = 0; i < g_set_count; i++)
            g_checked[g_set_members[i]] = g_set_was[i];

    /* Sync visible checkboxes with the modal's selections. */
    int nrows = ListView_GetItemCount(g_list);
    g_populating = 1;
    for (int r = 0; r < nrows; r++) {
        LVITEMW lv;
        memset(&lv, 0, sizeof lv);
        lv.mask  = LVIF_PARAM;
        lv.iItem = r;
        if (ListView_GetItem(g_list, &lv) && lv.lParam >= 0 &&
            lv.lParam < g_nitems)
            ListView_SetCheckState(g_list, r,
                                   g_checked[lv.lParam] ? TRUE : FALSE);
    }
    g_populating = 0;
    refresh_install_enabled();     /* g_populating suppressed the per-row echo */

    /* Use the main Install handler after the modal closes. */
    if (g_set_install)
        PostMessageW(g_main, WM_COMMAND, MAKEWPARAM(IDC_INSTALL, BN_CLICKED), 0);
}

/* File/device mismatches outrank store and library status on the hint line. */
static void refresh_hint(void)
{
    if (g_devnote[0]) { set_text(IDC_HINT, g_devnote); return; }
    if (!g_dev || g_dev->nkinds == 0) {
        wchar_t msg[256], wt[64];
        a2w(g_dev ? g_dev->title : "This device", wt, 64);
        _snwprintf(msg, 256,
            L"The %s store map is not known yet, so downloads are off for it. "
            L"Play works normally.", wt);
        msg[255] = L'\0';
        set_text(IDC_HINT, msg);
        return;
    }
    if (g_nitems <= 0) {
        set_text(IDC_HINT,
            L"No downloads found in the DLC folder above - point it at your "
            L"own folder with \"...\", or use \"Add files...\". You can still "
            L"Play without any.");
        return;
    }
    set_text(IDC_HINT, g_tab_has_sets
        ? L"Select what items you wish to install. Selections stay between "
          L"tabs.   A tile marked [n] belongs to a set - double-click it to "
          L"see the whole set."
        : L"Select what items you wish to install. Selections stay between "
          L"tabs.");
}

/* Check marks live in g_checked so rebuilding a tab does not lose selections. */
static void populate_list(void)
{
    wchar_t tab[DLC_TAB_MAX];
    current_tab_name(tab, DLC_TAB_MAX);
    char taba[DLC_TAB_MAX];
    w2a(tab, taba, sizeof taba);

    int n = 0;
    for (int i = 0; i < g_nitems; i++)
        if (!strcmp(g_items[i].tab, taba)) n++;

    HCURSOR oldcur = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    int bwl = 0, bhl = 0;
    tab_box(taba, &bwl, &bhl);
    int bw = S(bwl), bh = S(bhl);

    g_populating = 1;
    ListView_DeleteAllItems(g_list);

    HIMAGELIST il = ImageList_Create(bw, bh, ILC_COLOR24, n > 0 ? n : 1, 16);
    int row = 0, sets = 0;
    for (int i = 0; i < g_nitems; i++) {
        if (strcmp(g_items[i].tab, taba) != 0) continue;
        wchar_t disp[512], wpath[DLC_PATH_MAX];
        u2w(g_items[i].display, disp, 512);
        a2w(g_items[i].parts[0], wpath, DLC_PATH_MAX);

        /* Mark multi-part cards with their set size. */
        {
            int members[SET_MAX];
            int ns = set_of(i, members, SET_MAX);
            if (ns >= 2) {
                size_t dl = wcslen(disp);
                _snwprintf(disp + dl, 512 - dl, L"  [%d]", ns);
                disp[511] = L'\0';
                sets = 1;
            }
        }

        int image = -1;
        if (il) {
            HBITMAP bm = make_thumb(wpath, bw, bh);
            if (bm) {
                image = ImageList_Add(il, bm, NULL);
                DeleteObject(bm);
            }
        }

        LVITEMW lv;
        memset(&lv, 0, sizeof lv);
        lv.mask     = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        lv.iItem    = row;
        lv.pszText  = disp;
        lv.lParam   = (LPARAM)i;
        lv.iImage   = image;
        ListView_InsertItem(g_list, &lv);
        ListView_SetCheckState(g_list, row, g_checked[i] ? TRUE : FALSE);
        row++;
    }

    /* Replace the image list before freeing the old one. */
    HIMAGELIST prev = ListView_SetImageList(g_list, il, LVSIL_NORMAL);
    if (prev && prev != il) ImageList_Destroy(prev);
    g_imgs = il;
    ListView_SetIconSpacing(g_list, bw + S(22), bh + S(38));

    InvalidateRect(g_list, NULL, TRUE);
    g_populating = 0;
    SetCursor(oldcur);

    /* Mention sets only when the visible tab contains one. */
    g_tab_has_sets = sets;
    refresh_hint();

#ifdef LAUNCHER_DEBUG
    {
        RECT lr = {0,0,0,0}, cl = {0,0,0,0};
        if (g_list) { GetWindowRect(g_list, &lr); GetClientRect(g_list, &cl); }
        FILE *d = fopen("launcher_debug.txt", "a");
        if (d) {
            fprintf(d, "populate tab='%s' sel=%d nitems=%d rows=%d "
                       "list=%p count=%d rect=%ldx%ld client=%ldx%ld "
                       "style=%08lx visible=%d\n",
                    taba, TabCtrl_GetCurSel(g_tabs), g_nitems, row,
                    (void *)g_list, g_list ? ListView_GetItemCount(g_list) : -1,
                    lr.right - lr.left, lr.bottom - lr.top,
                    cl.right, cl.bottom,
                    g_list ? (unsigned long)GetWindowLongPtrW(g_list, GWL_STYLE) : 0,
                    g_list ? IsWindowVisible(g_list) : -1);
            /* Verify row placement, not only item count. */
            if (g_list) {
                LVCOLUMNW c;
                memset(&c, 0, sizeof c);
                c.mask = LVCF_WIDTH;
                int gotcol = ListView_GetColumn(g_list, 0, &c);
                fprintf(d, "  col0 got=%d cx=%d  top=%d perpage=%d\n",
                        gotcol, (int)c.cx, ListView_GetTopIndex(g_list),
                        ListView_GetCountPerPage(g_list));
                for (int r = 0; r < 3 && r < ListView_GetItemCount(g_list); r++) {
                    RECT ir = {0,0,0,0};
                    wchar_t t[128] = L"";
                    ListView_GetItemRect(g_list, r, &ir, LVIR_BOUNDS);
                    ListView_GetItemText(g_list, r, 0, t, 128);
                    fprintf(d, "  row%d rect=%ld,%ld..%ld,%ld text='%ls'\n",
                            r, ir.left, ir.top, ir.right, ir.bottom, t);
                }
            }
            fclose(d);
        }
    }
#endif
}

static void update_tab_labels(void)
{
    for (int t = 0; t < dlc_tab_count(g_dev); t++) {
        const char *name = dlc_tab_at(g_dev, t);
        int n = 0;
        for (int i = 0; i < g_nitems; i++)
            if (!strcmp(g_items[i].tab, name)) n++;

        wchar_t wname[DLC_TAB_MAX], label[64];
        a2w(name, wname, DLC_TAB_MAX);
        if (n) _snwprintf(label, 64, L"%s (%d)", wname, n);
        else   _snwprintf(label, 64, L"%s", wname);
        label[63] = L'\0';

        TCITEMW ti;
        memset(&ti, 0, sizeof ti);
        ti.mask = TCIF_TEXT;
        ti.pszText = label;
        TabCtrl_SetItem(g_tabs, t, &ti);
    }
}

static void clear_selection(void)
{
    for (int i = 0; i < g_nitems; i++) g_checked[i] = 0;
    populate_list();
    refresh_install_enabled();
}

static void rescan_library(void)
{
    wchar_t lib[MAX_PATH];
    get_text(IDC_LIB, lib, MAX_PATH);
    char liba[MAX_PATH * 2];
    w2a(lib, liba, sizeof liba);

    free(g_items); g_items = NULL;
    free(g_checked); g_checked = NULL;
    g_nitems = 0;

    HCURSOR old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    if (dlc_scan_library(g_dev, liba, &g_items, &g_nitems) != 0) {
        g_items = NULL;
        g_nitems = 0;
    }
    SetCursor(old);

    g_checked = calloc((size_t)(g_nitems > 0 ? g_nitems : 1), 1);

    /* An empty library does not block Play. */
    refresh_hint();

    update_tab_labels();
    populate_list();
    refresh_install_enabled();     /* a rescan clears the ticks */
}

/* Add external payloads to their normal tabs and preselect them. */
static void add_files(void)
{
    static wchar_t buf[64 * 1024];          /* multi-select needs real room */
    buf[0] = L'\0';

    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner   = g_main;
    ofn.lpstrFilter = L"Payloads (*.jpg;*.jpeg;*.bin)\0*.jpg;*.jpeg;*.bin\0"
                      L"All files\0*.*\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = sizeof buf / sizeof buf[0];
    ofn.lpstrTitle  = L"Add DLC / game / content payloads";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER
                    | OFN_ALLOWMULTISELECT | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;

    /* One file  -> "C:\dir\file.jpg\0\0"      (buf is already the full path)
     * Many      -> "C:\dir\0file1.jpg\0file2.jpg\0\0" */
    wchar_t *first = buf + wcslen(buf) + 1;
    int multi = (*first != L'\0');
    wchar_t *p = multi ? first : buf;
    int added = 0, skipped = 0;

    for (; *p; p += wcslen(p) + 1) {
        wchar_t full[DLC_PATH_MAX];
        if (multi) join(full, DLC_PATH_MAX, buf, p);
        else { wcsncpy(full, p, DLC_PATH_MAX - 1); full[DLC_PATH_MAX - 1] = L'\0'; }

        char fulla[DLC_PATH_MAX];
        w2a(full, fulla, sizeof fulla);

        DlcItem it;
        memset(&it, 0, sizeof it);
        dlc_tab_for(g_dev, fulla, it.tab, sizeof it.tab,
                    it.display, sizeof it.display);
        if (!it.tab[0]) {                   /* recipe, stamp card, unknown */
            skipped++;
            if (!multi) break;
            continue;
        }
        size_t dl = strlen(it.display);
        snprintf(it.display + dl, sizeof it.display - dl, "   [added]");
        it.nparts = 1;
        snprintf(it.parts[0], DLC_PATH_MAX, "%s", fulla);

        DlcItem *ni = realloc(g_items, (size_t)(g_nitems + 1) * sizeof *ni);
        unsigned char *nc = realloc(g_checked, (size_t)(g_nitems + 1));
        if (!ni || !nc) {
            if (ni) g_items = ni;
            if (nc) g_checked = nc;
            say(MB_ICONERROR, L"Out of memory", L"Could not add more files.");
            break;
        }
        g_items = ni;
        g_checked = nc;
        g_items[g_nitems] = it;
        g_checked[g_nitems] = 1;
        g_nitems++;
        added++;
        if (!multi) break;                  /* buf held the one full path */
    }

    if (added) {
        update_tab_labels();
        populate_list();
        refresh_install_enabled();     /* added files arrive pre-ticked */
    }
    /* Report files that are unreadable or have no route for this device. */
    if (skipped)
        say(MB_ICONINFORMATION, L"Not added",
            L"%d of the file%s you picked could not be added for the device "
            L"selected.\n\n"
            L"Either it has no store on this machine - recipes and stamp cards "
            L"have nowhere to go - or it is not a payload this can read. If "
            L"you expected it to work, check the device tab is the right one.%s",
            skipped, skipped == 1 ? L"" : L"s",
            added ? L"\n\nThe rest were added." : L"");
}

/* Save file and status panel */

static void savepath(wchar_t *out, int outsz)
{
    get_text(IDC_SAV, out, outsz);
}

/* Make the same default save path as the emulator. The Save box can still
 * point somewhere else. */
static void default_savepath(wchar_t *out, int outsz)
{
    wchar_t rom[MAX_PATH], dev[32];
    get_text(IDC_ROM, rom, MAX_PATH);
    a2w(g_dev->name, dev, 32);
    const wchar_t *slash = wcsrchr(rom, L'/');
    const wchar_t *backslash = wcsrchr(rom, L'\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    const wchar_t *filename = slash ? slash + 1 : rom;
    size_t parent_len = (size_t)(filename - rom);
    _snwprintf(out, (size_t)outsz, L"%.*ls%s%s%s%s.sav",
               (int)parent_len, rom, L"saves\\tamagotchi_", dev, L"\\", filename);
    out[outsz - 1] = L'\0';
}

/* Read the selected device's flash image, or explain why not. Caller frees. */
static uint8_t *load_save(const wchar_t *path, wchar_t *why, int whysz)
{
    uint32_t imgsz = dlc_image_size(g_dev);
    why[0] = L'\0';
    FILE *f = _wfopen(path, L"rb");
    if (!f) { _snwprintf(why, (size_t)whysz, L"cannot open this file"); return NULL; }
    if (_fseeki64(f, 0, SEEK_END) != 0) { fclose(f);
        _snwprintf(why, (size_t)whysz, L"cannot read this file"); return NULL; }
    long long sz = _ftelli64(f);
    if (sz != (long long)imgsz) {
        fclose(f);
        _snwprintf(why, (size_t)whysz,
                   L"not a %u MB save image (this file is %lld bytes)",
                   imgsz >> 20, sz);
        return NULL;
    }
    rewind(f);
    uint8_t *img = malloc(imgsz);
    if (!img || fread(img, 1, imgsz, f) != imgsz) {
        free(img); fclose(f);
        _snwprintf(why, (size_t)whysz, L"could not read the whole file");
        return NULL;
    }
    fclose(f);
    return img;
}

/* Read store usage from the current Save path instead of cached state. */
static void refresh_status(void)
{
    /* A ROM or Save edit can reach this without a device switch. */
    if (!g_dev || g_dev->nkinds == 0) {
        SetWindowTextW(g_status,
            L"(this device's download stores are not mapped yet, so there is "
            L"nothing to count)");
        return;
    }

    wchar_t sav[MAX_PATH];
    savepath(sav, MAX_PATH);
#ifdef LAUNCHER_DEBUG
    {
        FILE *d = fopen("launcher_debug.txt", "a");
        if (d) { fprintf(d, "refresh_status sav='%ls' exists=%d\n",
                         sav, file_exists(sav)); fclose(d); }
    }
#endif

    if (!sav[0]) {
        SetWindowTextW(g_status, L"(no save selected)");
        return;
    }
    if (!file_exists(sav)) {
        SetWindowTextW(g_status,
            L"(no file at this path yet)\r\n\r\n"
            L"Play once and get past the clock/name setup to create it, "
            L"or pick an existing .sav with the Save \"...\" button.");
        return;
    }

    wchar_t why[256];
    uint8_t *img = load_save(sav, why, 256);
    if (!img) {
        wchar_t msg[512];
        _snwprintf(msg, 512, L"cannot show usage: %s", why);
        msg[511] = L'\0';
        SetWindowTextW(g_status, msg);
        return;
    }

    DlcUsage u[16];
    int n = dlc_store_usage(g_dev, img, u, 16);
    free(img);

    /* VDP recipes have routes but no usage count. */
    {
        int keep = 0;
        for (int i = 0; i < n; i++)
            if (strcmp(u[i].label, "VDP recipes") != 0) u[keep++] = u[i];
        n = keep;
    }
#ifdef LAUNCHER_DEBUG
    {
        FILE *d = fopen("launcher_debug.txt", "a");
        if (d) {
            fprintf(d, "  usage n=%d:", n);
            for (int i = 0; i < n; i++)
                fprintf(d, " %s=%d/%d", u[i].label, u[i].used, u[i].max);
            fprintf(d, "\n");
            fclose(d);
        }
    }
#endif

    /* Fill two fixed-width cells per line so counts and bars stay aligned. */
    enum { COL = 46, GUTTER = 2 };        /* 17 label + 5 count + 2 + 20 bar cells */
    wchar_t text[4096], cell[2][96], bar[80];
    int at = 0;
    text[0] = L'\0';
    for (int i = 0; i < n; i += 2) {
        for (int c = 0; c < 2; c++) {
            cell[c][0] = L'\0';
            if (i + c >= n) continue;
            const DlcUsage *e = &u[i + c];
            wchar_t label[64];
            a2w(e->label, label, 64);
            int nb = 0;
            for (int k = 0; k < e->max && nb < 78; k++)
                bar[nb++] = (k < e->used) ? L'\x2588' : L'\xB7';
            bar[nb] = L'\0';
            _snwprintf(cell[c], 96, L"%-17s%2d/%-2d  %s%s",
                       label, e->used, e->max, bar,
                       e->used >= e->max ? L"  FULL" : L"");
            cell[c][95] = L'\0';
        }
        /* Pad the left cell only when a right cell follows. */
        if (cell[1][0])
            at = wappend(text, 4096, at, L"%-*s%*s%s\r\n",
                         COL, cell[0], GUTTER, L"", cell[1]);
        else
            at = wappend(text, 4096, at, L"%s\r\n", cell[0]);
    }
    SetWindowTextW(g_status, text);
}

/* Populate tabs before TabCtrl_AdjustRect sizes the list. */
static void rebuild_tabs(void)
{
    TabCtrl_DeleteAllItems(g_tabs);
    for (int t = 0; t < dlc_tab_count(g_dev); t++) {
        wchar_t wname[DLC_TAB_MAX];
        a2w(dlc_tab_at(g_dev, t), wname, DLC_TAB_MAX);
        TCITEMW ti;
        memset(&ti, 0, sizeof ti);
        ti.mask = TCIF_TEXT;
        ti.pszText = wname;
        TabCtrl_InsertItem(g_tabs, t, &ti);
    }
}

/* nkinds controls whether the selected device supports downloadable records. */
static void refresh_device_ui(void)
{
    int can = g_dev && g_dev->nkinds > 0;

    EnableWindow(GetDlgItem(g_main, IDC_LIST),    can);
    EnableWindow(GetDlgItem(g_main, IDC_TABS),    can);
    EnableWindow(GetDlgItem(g_main, IDC_REFRESH), can);
    EnableWindow(GetDlgItem(g_main, IDC_ADD),     can);
    EnableWindow(GetDlgItem(g_main, IDC_LIB),     can);
    EnableWindow(GetDlgItem(g_main, IDC_LIB_BR),  can);

    rebuild_tabs();

    /* Device changes invalidate filtered items and selections. */
    rescan_library();      /* refresh_hint, update_tab_labels, populate_list */
}

/* Check an image signature without changing devices. */
static const DlcDevice *device_of_image_w(const wchar_t *path)
{
    char a[MAX_PATH * 2];
    if (!path[0] || !file_exists(path)) return NULL;
    w2a(path, a, sizeof a);
    return dlc_device_of_image(a);
}

/* Report a mismatched Save before a mismatched ROM because Install writes it. */
static void refresh_devnote(void)
{
    wchar_t rom[MAX_PATH], sav[MAX_PATH];
    get_text(IDC_ROM, rom, MAX_PATH);
    savepath(sav, MAX_PATH);

    const DlcDevice *rom_dev = device_of_image_w(rom);
    const DlcDevice *sav_dev = device_of_image_w(sav);

    const wchar_t *which;
    const DlcDevice *actual;
    if (g_dev && sav_dev && sav_dev != g_dev)      { which = L"Save"; actual = sav_dev; }
    else if (g_dev && rom_dev && rom_dev != g_dev) { which = L"ROM";  actual = rom_dev; }
    else {
        g_devnote[0] = L'\0';
        refresh_hint();
        return;
    }
    wchar_t wt[64], wsel[64];
    a2w(actual->title, wt, 64);
    a2w(g_dev->title, wsel, 64);
    _snwprintf(g_devnote, 160, L"%s looks like %s, not %s.", which, wt, wsel);
    g_devnote[159] = L'\0';
    refresh_hint();
}

static void on_rom_changed(void)
{
    if (g_save_follows_rom) {
        wchar_t sav[MAX_PATH + 64];
        default_savepath(sav, MAX_PATH + 64);
        set_text(IDC_SAV, sav);
    }
    refresh_status();
    refresh_devnote();
}

/* Once someone types a save path, leave it alone until it matches the default again. */
static void on_save_edited(void)
{
    wchar_t sav[MAX_PATH + 64], want[MAX_PATH + 64];
    savepath(sav, MAX_PATH + 64);
    default_savepath(want, MAX_PATH + 64);
    g_save_follows_rom = (wcscmp(sav, want) == 0);
    refresh_status();
    refresh_devnote();
}

static int require_save(wchar_t *sav, int savsz)
{
    savepath(sav, savsz);
    if (!sav[0] || !file_exists(sav)) {
        say(MB_ICONWARNING, L"No save file",
            L"No file exists yet at:\n%s\n\n"
            L"Either pick an existing .sav with the Save \"...\" button, or "
            L"Play once and get past the clock/name setup so the game creates "
            L"one, then install.", sav);
        return 0;
    }
    return 1;
}

/* Installation */

/* Gather selected payloads across every tab into one install pass. */
static int gather_selected(char ***out)
{
    int n = 0;
    for (int i = 0; i < g_nitems; i++)
        if (g_checked[i]) n += g_items[i].nparts;
    if (!n) { *out = NULL; return 0; }

    char **paths = malloc((size_t)n * sizeof *paths);
    int k = 0;
    for (int i = 0; i < g_nitems; i++) {
        if (!g_checked[i]) continue;
        for (int j = 0; j < g_items[i].nparts; j++)
            paths[k++] = g_items[i].parts[j];
    }
    *out = paths;
    return n;
}

/* Return free slots for kind, or -1 when the image cannot be read. */
static int slots_of(const char *sava, int kind, DlcSlot *out, int max, int *nfree)
{
    FILE *f = fopen(sava, "rb");
    if (!f) return -1;
    uint32_t imgsz = dlc_image_size(g_dev);
    uint8_t *img = malloc(imgsz);
    if (!img || fread(img, 1, imgsz, f) != imgsz) {
        free(img); fclose(f); return -1;
    }
    fclose(f);
    int n = dlc_store_slots(g_dev, img, kind, out, max);
    *nfree = 0;
    for (int i = 0; i < n; i++) if (!out[i].occupied) (*nfree)++;
    free(img);
    return n;
}

/* Route the payload to a store; tab names are presentation only. */
static int route_payload(const char *path, char *id, size_t idsz)
{
    uint8_t *p = NULL;
    size_t plen = 0;
    char err[DLC_ERR_MAX];
    if (id && idsz) id[0] = '\0';
    if (dlc_extract_payload(path, &p, &plen, err, sizeof err) != 0) return -1;

    int typ = -1, kind = -1;
    const char *label = NULL;
    char myid[DLC_ID_MAX], name[DLC_NAME_MAX];
    dlc_parse_header(p, plen, &typ, myid, sizeof myid, name, sizeof name);
    int cat  = (plen > dlc_cat_off(g_dev)) ? p[dlc_cat_off(g_dev)] : -1;
    int flag = (plen > 0x51) ? p[0x51] : 0;
    dlc_route(g_dev, typ, cat, myid, flag, p, plen, &label, &kind);
    free(p);
    if (id && idsz) snprintf(id, idsz, "%s", myid);
    return kind;
}

/* Making room */

/* Ask which occupied slots to empty, one store at a time. */
static HWND g_room_wnd, g_room_list, g_room_ok;
static int  g_room_taken;
/* Read selected slots before destroying the list control. */
static int  g_room_pick[64], g_room_npick;

/* Match installed records by ASCII ID; unmatched records keep embedded names. */
static void name_slots(const DlcKind *k, const DlcSlot *slots, int ns,
                       wchar_t (*names)[DLC_NAME_MAX])
{
    for (int i = 0; i < ns; i++) {
        const char *own = slots[i].name[0] ? slots[i].name : slots[i].id;
        u2w(own[0] ? own : "(unnamed)", names[i], DLC_NAME_MAX);
    }
    HCURSOR old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    for (int i = 0; i < g_nitems; i++) {
        for (int j = 0; j < g_items[i].nparts; j++) {
            char id[DLC_ID_MAX];
            if (route_payload(g_items[i].parts[j], id, sizeof id) != k->kind)
                continue;
            if (!id[0]) continue;
            for (int s = 0; s < ns; s++)
                if (slots[s].occupied && !strcmp(slots[s].id, id))
                    u2w(g_items[i].display, names[s], DLC_NAME_MAX);
        }
    }
    SetCursor(old);
}

static void room_refresh_ok(void)
{
    int n = 0, rows = ListView_GetItemCount(g_room_list);
    for (int i = 0; i < rows; i++)
        if (ListView_GetCheckState(g_room_list, i)) n++;
    /* Remaining overflow is reported after any nonempty selection. */
    EnableWindow(g_room_ok, n >= 1);
}

static LRESULT CALLBACK room_wndproc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NOTIFY: {
        LPNMHDR nh = (LPNMHDR)lp;
        if (nh->hwndFrom == g_room_list && nh->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *nl = (NMLISTVIEW *)lp;
            if (nl->uChanged & LVIF_STATE) room_refresh_ok();
            return 0;
        }
        return 0;
    }
    case WM_COMMAND:
        /* Enter can bypass the button state, so validate the selection here. */
        if (LOWORD(wp) == IDOK) {
            int n = 0, rows = ListView_GetItemCount(g_room_list);
            for (int i = 0; i < rows && n < 64; i++) {
                if (!ListView_GetCheckState(g_room_list, i)) continue;
                LVITEMW lv;
                memset(&lv, 0, sizeof lv);
                lv.mask  = LVIF_PARAM;
                lv.iItem = i;
                if (ListView_GetItem(g_room_list, &lv)) g_room_pick[n++] = (int)lv.lParam;
            }
            if (n < 1) { MessageBeep(MB_ICONWARNING); return 0; }
            g_room_npick = n;
            g_room_taken = 1;
            DestroyWindow(hw);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) { DestroyWindow(hw); return 0; }
        break;
    case WM_CLOSE:
        DestroyWindow(hw);
        return 0;
    case WM_DESTROY:
        g_room_wnd = NULL;
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

/* Return selected slots. Cancel makes no room but does not abort installation. */
static int pick_slots_to_free(const DlcKind *k, const DlcSlot *slots, int ns,
                              DlcFree *out, int max)
{
    static int registered;
    if (!registered) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc   = room_wndproc;
        wc.hInstance     = GetModuleHandleW(NULL);
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"tamaemu_room";
        RegisterClassW(&wc);
        registered = 1;
    }

    int nocc = 0;
    for (int i = 0; i < ns; i++) if (slots[i].occupied) nocc++;
    if (nocc < 1) return 0;

    const int W = 460, ROWH = 20;
    int listh = nocc * ROWH + 8;
    if (listh > 240) listh = 240;
    const int barh = 44;

    wchar_t wlabel[64], head[512];
    a2w(k->label, wlabel, 64);
    _snwprintf(head, 512,
        L"No room left in %s!\n\n"
        L"Select one or more items to remove, or cancel to continue install.",
        wlabel);
    head[511] = L'\0';

    /* Measure wrapped text; th and the derived positions are device pixels. */
    int th;
    {
        HDC dc = GetDC(NULL);
        HFONT old = (HFONT)SelectObject(dc, g_font);
        RECT tr = { 0, 0, S(W - 20), 0 };
        DrawTextW(dc, head, -1, &tr, DT_CALCRECT | DT_WORDBREAK);
        SelectObject(dc, old);
        ReleaseDC(NULL, dc);
        th = tr.bottom - tr.top;
    }
    int ytext = S(10);
    int ylist = ytext + th + S(8);
    int ybar  = ylist + S(listh);

    RECT wr = { 0, 0, S(W), ybar + S(barh) };
    AdjustWindowRect(&wr, WS_CAPTION | WS_SYSMENU | WS_POPUP, FALSE);
    RECT mr;
    GetWindowRect(g_main, &mr);
    int x = mr.left + ((mr.right - mr.left) - (wr.right - wr.left)) / 2;
    int y = mr.top  + ((mr.bottom - mr.top) - (wr.bottom - wr.top)) / 3;

    g_room_wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"tamaemu_room",
                                 L"Make room", WS_CAPTION | WS_SYSMENU | WS_POPUP,
                                 x, y, wr.right - wr.left, wr.bottom - wr.top,
                                 g_main, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_room_wnd) return -1;

    HWND st = CreateWindowExW(0, L"STATIC", head, WS_CHILD | WS_VISIBLE,
                              S(10), ytext, S(W - 20), th, g_room_wnd,
                              NULL, GetModuleHandleW(NULL), NULL);
    g_room_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP
                                  | LVS_REPORT | LVS_NOCOLUMNHEADER
                                  | LVS_SINGLESEL,
                                  S(10), ylist, S(W - 20), S(listh),
                                  g_room_wnd, NULL, GetModuleHandleW(NULL), NULL);
    ListView_SetExtendedListViewStyle(g_room_list,
        LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
    {
        LVCOLUMNW c;
        memset(&c, 0, sizeof c);
        c.mask = LVCF_WIDTH;
        c.cx   = S(W - 20) - GetSystemMetrics(SM_CXVSCROLL) - S(6);
        ListView_InsertColumn(g_room_list, 0, &c);
    }
    /* List occupied slots only; lParam carries the slot number. */
    /* Keep the 32 KiB name buffer off the stack. */
    static wchar_t names[64][DLC_NAME_MAX];
    name_slots(k, slots, ns < 64 ? ns : 64, names);

    int row = 0;
    for (int i = 0; i < ns && i < 64; i++) {
        if (!slots[i].occupied) continue;
        wchar_t line[DLC_NAME_MAX + 32];
        _snwprintf(line, DLC_NAME_MAX + 32, L"slot %d    %s", slots[i].slot, names[i]);
        line[DLC_NAME_MAX + 31] = L'\0';
        LVITEMW lv;
        memset(&lv, 0, sizeof lv);
        lv.mask    = LVIF_TEXT | LVIF_PARAM;
        lv.iItem   = row++;
        lv.pszText = line;
        lv.lParam  = (LPARAM)slots[i].slot;
        ListView_InsertItem(g_room_list, &lv);
    }

    int by = ybar + (S(barh) - S(28)) / 2;
    g_room_ok = CreateWindowExW(0, L"BUTTON", L"Remove and install",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP
                                | BS_DEFPUSHBUTTON,
                                S(W - 10 - 84 - 6 - 140), by, S(140), S(28),
                                g_room_wnd, (HMENU)(INT_PTR)IDOK,
                                GetModuleHandleW(NULL), NULL);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP
                                  | BS_PUSHBUTTON,
                                  S(W - 10 - 84), by, S(84), S(28),
                                  g_room_wnd, (HMENU)(INT_PTR)IDCANCEL,
                                  GetModuleHandleW(NULL), NULL);
    SendMessageW(st,          WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_room_list, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_room_ok,   WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(cancel,      WM_SETFONT, (WPARAM)g_font, TRUE);

    g_room_taken = 0;
    g_room_npick = 0;
    room_refresh_ok();

    ShowWindow(g_room_wnd, SW_SHOW);
    EnableWindow(g_main, FALSE);
    MSG m;
    while (g_room_wnd && GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_room_wnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    EnableWindow(g_main, TRUE);
    SetForegroundWindow(g_main);
    if (!g_room_taken) return 0;
    (void)row;

    int n = 0;
    for (int i = 0; i < g_room_npick && n < max; i++) {
        out[n].kind = k->kind;
        out[n].slot = g_room_pick[i];
        n++;
    }
    return n;
}

/* Ask once per full store; Cancel skips that store only. */
static void make_room(const char *sava, DlcFree *out, int max, int *nout)
{
    *nout = 0;
    if (!g_dev) return;

    for (int ki = 0; ki < g_dev->nkinds; ki++) {
        const DlcKind *k = &g_dev->kinds[ki];

        DlcSlot slots[64];
        int nfree = 0;
        int ns = slots_of(sava, k->kind, slots, 64, &nfree);
        if (ns <= 0) continue;

        /* Existing IDs consume no new slot and need no replacement prompt. */
        int want = 0;
        for (int i = 0; i < g_nitems; i++) {
            if (!g_checked[i]) continue;
            for (int j = 0; j < g_items[i].nparts; j++) {
                char id[DLC_ID_MAX];
                if (route_payload(g_items[i].parts[j], id, sizeof id) != k->kind)
                    continue;
                int dup = 0;
                for (int s = 0; s < ns && !dup; s++)
                    if (slots[s].occupied && id[0] && !strcmp(slots[s].id, id))
                        dup = 1;
                if (!dup) want++;
            }
        }
        if (want <= nfree) continue;

        *nout += pick_slots_to_free(k, slots, ns, out + *nout, max - *nout);
    }
}

/* Return 1 when a running emulator consumes the swap request. */
static int swap_via_running_emu(const wchar_t *sav, const SwapWrite *w, int nw)
{
    char sava[MAX_PATH * 2];
    w2a(sav, sava, sizeof sava);
    char req[MAX_PATH * 2 + 8];
    snprintf(req, sizeof req, "%s.swap", sava);

    if (swapreq_write(req, w, nw) != 0) return 0;

    for (int i = 0; i < 20; i++) {          /* up to ~2 s, 100 ms apart */
        Sleep(100);
        FILE *f = fopen(req, "rb");
        if (!f) return 1;                   /* gone: a running emulator took it */
        fclose(f);
    }
    remove(req);                            /* nobody took it - clean up */
    return 0;
}

static int do_install(void)
{
    wchar_t sav[MAX_PATH];
    if (!require_save(sav, MAX_PATH)) return 0;

    char **paths = NULL;
    int n = gather_selected(&paths);
    if (!n) {
        say(MB_ICONINFORMATION, L"Nothing selected",
            L"Select one or more items first.");
        return 0;
    }

    char sava[MAX_PATH * 2];
    w2a(sav, sava, sizeof sava);

    DlcResult *res = calloc((size_t)n, sizeof *res);
    char err[256] = "";

    /* Resolve full stores before writing any content kind. */
    DlcFree frees[32];
    int nfrees = 0;
    make_room(sava, frees, 32, &nfrees);

    /* A running emulator must apply replacements to its in-memory save. */
    int live = 0;
    if (nfrees == 1 && n == 1 && frees[0].kind == dlc_game_kind(g_dev)) {
        uint8_t *rec = NULL;
        size_t reclen = 0;
        const DlcKind *k = dlc_kind(g_dev, frees[0].kind);
        if (k && dlc_extract_payload(paths[0], &rec, &reclen, err, sizeof err) == 0) {
            if (reclen <= k->slotsz) {
                SwapWrite w[2];
                w[0].off   = k->base + (uint32_t)frees[0].slot * k->slotsz;
                w[0].len   = k->slotsz;
                w[0].bytes = NULL;              /* free the slot */
                w[1].off   = w[0].off;
                w[1].len   = (uint32_t)reclen;
                w[1].bytes = rec;               /* then write the new record */

                /* Replacing a slot is unsafe only while that game is running. */
                if (MessageBoxW(g_main,
                        L"If your Tamagotchi is running, make sure you are NOT "
                        L"playing a downloaded game right now - backing out to "
                        L"the main screen is enough.\n\nInstall?",
                        L"Install", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) {
                    free(rec);
                    free(paths);
                    free(res);
                    return 0;
                }
                live = swap_via_running_emu(sav, w, 2);
            }
            free(rec);
        }
        err[0] = '\0';
    }

    int rc = 0;
    if (!live) {
        HCURSOR old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
        rc = dlc_inject(g_dev, sava, (const char *const *)paths, n, 0, 1,
                        frees, nfrees, res, err, sizeof err);
        SetCursor(old);
    }
    free(paths);

    if (rc != 0) {
        wchar_t werr[512];
        a2w(err, werr, 512);
        say(MB_ICONERROR, L"Install failed", L"%s", werr);
        free(res);
        return 0;
    }

    /* Live swaps need confirmation independent of res[]. */
    if (live) {
        say(MB_ICONINFORMATION, L"Installed",
            L"Sent to the running Tamagotchi - slot %d now holds the new game.\n\n"
            L"Open the Game Center to see it.", frees[0].slot);
        free(res);
        clear_selection();
        refresh_status();
        refresh_devnote();
        return 1;
    }

    /* List every failure and cap successes to fit the dialog. */
    int ok = 0, bad = 0;
    for (int i = 0; i < n; i++) { if (res[i].error[0]) bad++; else ok++; }

    wchar_t msg[4096];
    int at = wappend(msg, 4096, 0, L"%d of %d payload%s installed into:\n%s\n\n",
                     ok, n, n == 1 ? L"" : L"s", sav);
    const int SHOW = 20;
    int shown = 0;
    for (int i = 0; i < n && at < 3600; i++) {
        if (!res[i].error[0]) continue;
        wchar_t f[300], e[300];
        a2w(res[i].file, f, 300);
        a2w(res[i].error, e, 300);
        at = wappend(msg, 4096, at, L"! %s: %s\n", f, e);
    }
    for (int i = 0; i < n && at < 3600 && shown < SHOW; i++) {
        if (res[i].error[0]) continue;
        wchar_t f[300], l[64];
        a2w(res[i].file, f, 300);
        a2w(res[i].label ? res[i].label : "", l, 64);
        at = wappend(msg, 4096, at, L"%s  ->  %s\n", f, l);
        shown++;
    }
    if (ok > shown)
        at = wappend(msg, 4096, at, L"... and %d more\n", ok - shown);

    /* iD outfits can install successfully but remain hidden by the device's
     * character-sex filter. */
    for (int i = 0; i < n; i++)
        if (!res[i].error[0] && res[i].label && !strcmp(res[i].label, "outfit")) {
            wappend(msg, 4096, at,
                L"\nNote: iD outfits are boy- or girl-specific, and the file "
                L"does not say which. One made for the other sex installs "
                L"fine but never appears in the Photo Studio's list.\n");
            break;
        }

    MessageBoxW(g_main, msg, bad ? L"Installed with warnings" : L"Installed",
                MB_OK | (bad ? MB_ICONWARNING : MB_ICONINFORMATION));
    free(res);

    clear_selection();
    refresh_status();
    refresh_devnote();
    return bad == 0;
}

/* The launcher does not expose destructive save wiping. */

/* Play */

/* Detach the emulator so launcher exit does not end the game. */
static void do_play(void)
{
    wchar_t emu[MAX_PATH], rom[MAX_PATH], sav[MAX_PATH];
    join(emu, MAX_PATH, g_root, EMU_EXE);
    get_text(IDC_ROM, rom, MAX_PATH);
    savepath(sav, MAX_PATH);

    if (!file_exists(emu)) {
        say(MB_ICONERROR, L"Emulator missing", L"Not found:\n%s", emu);
        return;
    }
    if (!rom[0] || !file_exists(rom)) {
        wchar_t wt[64];
        a2w(g_dev->title, wt, 64);
        say(MB_ICONERROR, L"ROM missing",
            L"%s\n\nPick your %s firmware dump (.bin) with the ROM \"...\" "
            L"button. The firmware is not shipped with this project.",
            rom[0] ? rom : L"(no ROM selected)", wt);
        return;
    }
    remember_rom(g_dev, rom);

    int awake   = (IsDlgButtonChecked(g_main, IDC_AWAKE)   == BST_CHECKED);
    int buttons = (IsDlgButtonChecked(g_main, IDC_BUTTONS) == BST_CHECKED);
    int ontop   = (IsDlgButtonChecked(g_main, IDC_ONTOP)   == BST_CHECKED);
    g_resume    = (IsDlgButtonChecked(g_main, IDC_RESUME)  == BST_CHECKED);

    /* Use the save file the player picked. Otherwise, let the emulator make
     * the normal folder beside the ROM. */
    enum { CMDCAP = MAX_PATH * 3 + 128 };
    wchar_t cmd[CMDCAP];
    int at = g_save_follows_rom
           ? wappend(cmd, CMDCAP, 0, L"\"%s\" \"%s\"", emu, rom)
           : wappend(cmd, CMDCAP, 0, L"\"%s\" \"%s\" --sav \"%s\"", emu, rom, sav);
    wchar_t wname[32];
    a2w(g_dev->name, wname, 32);
    at = wappend(cmd, CMDCAP, at, L" --device %s", wname);
    at = wappend(cmd, CMDCAP, at, L" --persist-ram");
    at = append_resume_flag(cmd, CMDCAP, at, g_resume);
    /* No --rtc-mult: the game clock is changed in the emulator with +/-. */
    if (awake)     at = wappend(cmd, CMDCAP, at, L" --stay-awake");
    if (!buttons)  at = wappend(cmd, CMDCAP, at, L" --no-buttons");
    if (ontop)     at = wappend(cmd, CMDCAP, at, L" --on-top");
    /* Always pass bindings so the log records the active keys. */
    wchar_t lk[4];
    for (int i = 0; i < 3; i++)
        lk[i] = (wchar_t)(g_keys[i] >= L'A' && g_keys[i] <= L'Z'
                          ? g_keys[i] + 32 : g_keys[i]);
    lk[3] = 0;
    at = wappend(cmd, CMDCAP, at, L" --keys %s", lk);
#ifdef LAUNCHER_DEBUG
    /* IR wire logging is debug-only because it can add several MiB per session. */
    at = wappend(cmd, CMDCAP, at, L" --ir-log");
#endif
    (void)at;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    /* Capture through a pipe so the logger can enforce a live size cap. */
    wchar_t logdir[MAX_PATH];
    wchar_t logpath[MAX_PATH];
    join(logdir, MAX_PATH, g_root, LOG_DIR);
    CreateDirectoryW(logdir, NULL); /* ERROR_ALREADY_EXISTS is harmless. */
    LogCapture *capture = (LogCapture *)calloc(1, sizeof *capture);
    HANDLE pipe_write = INVALID_HANDLE_VALUE;
    HANDLE capture_thread = NULL;
    int log_open = -1;
    for (int n = 1; n <= 8 && log_open != 0; n++) {
        wchar_t leaf[64];
        if (n == 1) wcscpy(leaf, EMU_LOG);
        else        _snwprintf(leaf, 64, L"emu_run_%d.log", n);
        leaf[63] = 0;
        join(logpath, MAX_PATH, logdir, leaf);
        if (capture)
            log_open = logcap_open(&capture->log, logpath, EMU_LOG_LIMIT,
                                   LOGCAP_RETAINED);
    }
    if (log_open != 0 && capture) {
        free(capture);
        capture = NULL;
    }

    if (capture && CreatePipe(&capture->read, &pipe_write, &sa, 0)) {
        SetHandleInformation(capture->read, HANDLE_FLAG_INHERIT, 0);
        {
            char cmda[CMDCAP + 16];
            int n = _snprintf(cmda, sizeof cmda, "[launcher] ");
            w2a(cmd, cmda + n, (int)sizeof cmda - n - 2);
            size_t len = strlen(cmda);
            if (len + 2 < sizeof cmda) {
                cmda[len++] = '\r';
                cmda[len++] = '\n';
                (void)logcap_write(&capture->log, cmda, len);
            }
        }
        capture_thread = CreateThread(NULL, 0, log_capture_thread, capture, 0, NULL);
        if (!capture_thread) {
            CloseHandle(capture->read);
            capture->read = NULL;
            logcap_close(&capture->log);
            free(capture);
            capture = NULL;
            CloseHandle(pipe_write);
            pipe_write = INVALID_HANDLE_VALUE;
        }
    } else if (capture) {
        logcap_close(&capture->log);
        free(capture);
        capture = NULL;
    }
    HANDLE hnul = CreateFileW(L"NUL", GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    if (capture && pipe_write != INVALID_HANDLE_VALUE && hnul != INVALID_HANDLE_VALUE) {
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdInput  = hnul;
        si.hStdOutput = pipe_write;
        si.hStdError  = pipe_write;
    }

    BOOL ok = CreateProcessW(emu, cmd, NULL, NULL,
                             si.dwFlags ? TRUE : FALSE,
                             DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                             NULL, g_root, &si, &pi);
    DWORD gle = GetLastError();

    if (pipe_write != INVALID_HANDLE_VALUE) CloseHandle(pipe_write);
    if (hnul != INVALID_HANDLE_VALUE) CloseHandle(hnul);

    if (!ok) {
        if (capture_thread) {
            WaitForSingleObject(capture_thread, INFINITE);
            CloseHandle(capture_thread);
        }
        say(MB_ICONERROR, L"Could not start the emulator",
            L"CreateProcess failed (error %lu) for:\n%s", gle, emu);
        return;
    }
    if (capture_thread) CloseHandle(capture_thread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

/* Main window */

static HWND mk(const wchar_t *cls, const wchar_t *text, DWORD style,
               int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             S(x), S(y), S(w), S(h), g_main,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static void build_ui(void)
{
    const DWORD ES = WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL;
    const DWORD BS = WS_TABSTOP | BS_PUSHBUTTON;

    /* Keep device tabs on one row so they cannot push the controls below them. */
    g_devtabs = mk(WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | WS_TABSTOP,
                   0, DEVSY, CLIENTW, DEVSH, IDC_DEVICE);
    for (int i = 0; i < dlc_device_count(); i++) {
        wchar_t wt[64];
        a2w(dlc_device_at(i)->shortname, wt, 64);
        TCITEMW ti;
        memset(&ti, 0, sizeof ti);
        ti.mask = TCIF_TEXT;
        ti.pszText = wt;
        TabCtrl_InsertItem(g_devtabs, i, &ti);    /* tab index == device index */
    }
    TabCtrl_SetCurSel(g_devtabs, 0);              /* the P's */

    mk(L"STATIC", L"ROM (.bin):", 0, LBLX, ROW0 + 4, LBLW, 18, 0);
    mk(L"EDIT", L"", ES, EDX, ROW0, ROMEDW, EDH, IDC_ROM);
    mk(L"BUTTON", GLYPH_FOLDER L" ...", BS, ROMBRX, ROW0, BRW, EDH, IDC_ROM_BR);
    mk(L"BUTTON", L"?", BS, HELPX, ROW0, HELPW, EDH, IDC_WELCOME);

    mk(L"STATIC", L"Save (.sav):", 0, LBLX, ROW0 + ROWSTEP + 4, LBLW, 18, 0);
    mk(L"EDIT", L"", ES, EDX, ROW0 + ROWSTEP, EDW, EDH, IDC_SAV);
    mk(L"BUTTON", GLYPH_FOLDER L" ...", BS, BRX, ROW0 + ROWSTEP, BRW, EDH,
       IDC_SAV_BR);
    mk(L"BUTTON", L"Match ROM", BS, MATCHX, ROW0 + ROWSTEP, MATCHW, EDH,
       IDC_SAV_MATCH);

    mk(L"STATIC", L"DLC Folder:", 0, LBLX, ROW0 + 2*ROWSTEP + 4, LBLW, 18, 0);
    mk(L"EDIT", L"", ES, EDX, ROW0 + 2*ROWSTEP, LIBEDW, EDH, IDC_LIB);
    mk(L"BUTTON", GLYPH_FOLDER L" ...", BS, LIBBRX, ROW0 + 2*ROWSTEP, BRW, EDH,
       IDC_LIB_BR);

    /* Game-clock speed is adjusted in the emulator; scripts may use --rtc-mult. */
    mk(L"BUTTON", L"Stay awake", WS_TABSTOP | BS_AUTOCHECKBOX,
       LBLX, OPTY + 1, 92, 20, IDC_AWAKE);
    mk(L"BUTTON", L"Show A/B/C buttons", WS_TABSTOP | BS_AUTOCHECKBOX,
       LBLX + 100, OPTY + 1, 140, 20, IDC_BUTTONS);
    mk(L"BUTTON", L"Always on top", WS_TABSTOP | BS_AUTOCHECKBOX,
       LBLX + 250, OPTY + 1, 110, 20, IDC_ONTOP);
    mk(L"BUTTON", L"Resume last session", WS_TABSTOP | BS_AUTOCHECKBOX,
       LBLX + 370, OPTY + 1, 150, 20, IDC_RESUME);
    /* Stay awake defaults on so device sleep is not mistaken for a freeze. */
    CheckDlgButton(g_main, IDC_AWAKE, BST_CHECKED);
    /* Button visibility is a persistent preference. */
    CheckDlgButton(g_main, IDC_BUTTONS,
                   reg_get(REG_BUTTONS, 1) ? BST_CHECKED : BST_UNCHECKED);
    /* Always-on-top is persistent and defaults off. */
    CheckDlgButton(g_main, IDC_ONTOP,
                   reg_get(REG_ONTOP, 0) ? BST_CHECKED : BST_UNCHECKED);
    /* Resume by default; leave it unchecked to start cold. */
    g_resume = reg_get(REG_RESUME, 1) ? 1 : 0;
    CheckDlgButton(g_main, IDC_RESUME,
                   g_resume ? BST_CHECKED : BST_UNCHECKED);

    /* Load bindings at startup; Play reads them even if Help was never opened. */
    load_keys();

    /* refresh_hint owns this text. */
    mk(L"STATIC", L"", 0, M, HINTY, CLIENTW - 2*M, 18, IDC_HINT);

    g_tabs = mk(WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | WS_TABSTOP,
                M, TABSY, CLIENTW - 2*M, TABSH, IDC_TABS);
    /* Populate tabs before TabCtrl_AdjustRect measures them. */
    rebuild_tabs();

    /* Keep the list as a sibling so WM_NOTIFY reaches the main window. */
    RECT tr;
    GetWindowRect(g_tabs, &tr);
    MapWindowPoints(NULL, g_main, (POINT *)&tr, 2);
    TabCtrl_AdjustRect(g_tabs, FALSE, &tr);
    g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS
                             | LVS_ICON | LVS_AUTOARRANGE | LVS_SHOWSELALWAYS,
                             tr.left, tr.top, tr.right - tr.left,
                             tr.bottom - tr.top, g_main,
                             (HMENU)(INT_PTR)IDC_LIST, GetModuleHandleW(NULL), NULL);
    /* Keep the list above the tab display area it overlaps. */
    SetWindowPos(g_list, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SendMessageW(g_list, WM_SETFONT, (WPARAM)g_font, TRUE);
    /* LABELTIP so a name too long for its cell can still be read on hover. */
    ListView_SetExtendedListViewStyle(g_list,
        LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
#ifdef LAUNCHER_DEBUG
    ListView_SetBkColor(g_list, RGB(255, 235, 190));
#endif

    /* Leave the center gap available for wider captions and glyphs. */
    mk(L"BUTTON", GLYPH_REFRESH L"  Refresh",      BS, M,   BTNY,  96, 28,
       IDC_REFRESH);
    mk(L"BUTTON", L"+  Add files...",              BS, 112, BTNY, 128, 28,
       IDC_ADD);
    mk(L"BUTTON", GLYPH_DOWN L"  Install selected", BS,
       CLIENTW - M - 96 - 6 - 144, BTNY, 144, 28, IDC_INSTALL);
    mk(L"BUTTON", GLYPH_PLAY L"  Play", BS | BS_DEFPUSHBUTTON,
       CLIENTW - M - 96, BTNY, 96, 28, IDC_PLAY);

    mk(L"STATIC", L"Downloaded in this save (used / max):", 0,
       M, STATLY, CLIENTW - 2*M, 18, 0);
    g_status = mk(L"EDIT", L"",
                  WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
                  M, STATY, CLIENTW - 2*M, STATH, IDC_STATUS);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)g_mono, TRUE);

    refresh_device_ui();
}

#ifdef LAUNCHER_DEBUG
/* Check control bounds and unintended overlap using client coordinates. */
static BOOL CALLBACK dump_child(HWND c, LPARAM lp)
{
    FILE *d = (FILE *)lp;
    RECT r;
    wchar_t cls[64], txt[80];
    GetWindowRect(c, &r);
    MapWindowPoints(NULL, g_main, (POINT *)&r, 2);
    GetClassNameW(c, cls, 64);
    GetWindowTextW(c, txt, 80);
    /* Flatten captions to ASCII for one-line diagnostics. */
    char asc[81];
    int na = 0;
    for (int i = 0; i < 40 && txt[i]; i++)
        asc[na++] = (txt[i] >= 0x20 && txt[i] < 0x7F) ? (char)txt[i] : '.';
    asc[na] = '\0';
    fprintf(d, "  id=%-5d %-20ls %4ld,%4ld %4ldx%-4ld \"%s\"\n",
            GetDlgCtrlID(c), cls, r.left, r.top,
            r.right - r.left, r.bottom - r.top, asc);
    return TRUE;
}

static void dump_layout(void)
{
    RECT cr;
    GetClientRect(g_main, &cr);
    FILE *d = fopen("launcher_debug.txt", "a");
    if (!d) return;
    fprintf(d, "dpi=%d client=%ldx%ld (expect %dx%d)\n",
            g_dpi, cr.right, cr.bottom, S(CLIENTW), S(CLIENTH));
    EnumChildWindows(g_main, dump_child, (LPARAM)d);

    HWND kids[64];
    int nk = 0;
    for (HWND c = GetWindow(g_main, GW_CHILD); c && nk < 64;
         c = GetWindow(c, GW_HWNDNEXT)) kids[nk++] = c;
    for (int i = 0; i < nk; i++) {
        RECT a;
        GetWindowRect(kids[i], &a);
        MapWindowPoints(NULL, g_main, (POINT *)&a, 2);
        if (a.right > cr.right || a.bottom > cr.bottom || a.left < 0 || a.top < 0)
            fprintf(d, "  OVERFLOW id=%d %ld,%ld..%ld,%ld\n",
                    GetDlgCtrlID(kids[i]), a.left, a.top, a.right, a.bottom);
        for (int j = i + 1; j < nk; j++) {
            RECT b, x;
            GetWindowRect(kids[j], &b);
            MapWindowPoints(NULL, g_main, (POINT *)&b, 2);
            /* the list deliberately sits over the tab control's display area */
            int ids[2] = { GetDlgCtrlID(kids[i]), GetDlgCtrlID(kids[j]) };
            if ((ids[0] == IDC_TABS && ids[1] == IDC_LIST) ||
                (ids[1] == IDC_TABS && ids[0] == IDC_LIST)) continue;
            if (IntersectRect(&x, &a, &b))
                fprintf(d, "  OVERLAP id=%d and id=%d\n", ids[0], ids[1]);
        }
    }
    fclose(d);
}
#endif

static LRESULT CALLBACK wndproc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND: {
        int id   = LOWORD(wp);
        int code = HIWORD(wp);

        if (code == EN_KILLFOCUS) {
            if (id == IDC_ROM) { on_rom_changed(); return 0; }
            if (id == IDC_SAV) { on_save_edited(); return 0; }
            return 0;
        }
        /* Enter applies the focused text field. */
        if (id == IDOK) {
            HWND f = GetFocus();
            if (f == GetDlgItem(hw, IDC_ROM))      on_rom_changed();
            else if (f == GetDlgItem(hw, IDC_SAV)) on_save_edited();
            else if (f == GetDlgItem(hw, IDC_LIB)) {
                wchar_t lib[MAX_PATH];
                get_text(IDC_LIB, lib, MAX_PATH);
                remember_lib(g_dev, lib);
                rescan_library();
            }
            else                                   do_play();
            return 0;
        }
        if (id == IDCANCEL) { DestroyWindow(hw); return 0; }

        switch (id) {
        case IDC_ROM_BR: {
            wchar_t rom[MAX_PATH];
            get_text(IDC_ROM, rom, MAX_PATH);
            wchar_t wt[64], title[96];
            a2w(g_dev->title, wt, 64);
            _snwprintf(title, 96, L"Select the %s ROM (.bin)", wt);
            title[95] = L'\0';
            if (pick_file(title,
                          L"ROM images (*.bin)\0*.bin\0All files\0*.*\0",
                          rom, MAX_PATH)) {
                set_text(IDC_ROM, rom);
                remember_rom(g_dev, rom);
                on_rom_changed();
            }
            return 0;
        }
        case IDC_SAV_BR: {
            wchar_t sav[MAX_PATH];
            savepath(sav, MAX_PATH);
            if (pick_file(L"Select a save (.sav) to view / edit / play",
                          L"Save files (*.sav)\0*.sav\0All files\0*.*\0",
                          sav, MAX_PATH)) {
                set_text(IDC_SAV, sav);
                g_save_follows_rom = 0;
                refresh_status();
                refresh_devnote();
            }
            return 0;
        }
        case IDC_SAV_MATCH:
            g_save_follows_rom = 1;
            on_rom_changed();
            return 0;
        case IDC_LIB_BR: {
            wchar_t lib[MAX_PATH];
            get_text(IDC_LIB, lib, MAX_PATH);
            if (pick_folder(L"Select your DLC folder", lib, MAX_PATH)) {
                set_text(IDC_LIB, lib);
                remember_lib(g_dev, lib);
                rescan_library();
            }
            return 0;
        }
        /* Refresh rereads the save and its device note. */
        case IDC_REFRESH:  rescan_library(); refresh_status(); refresh_devnote(); return 0;
        case IDC_ADD:      add_files();                        return 0;
        case IDC_INSTALL:  do_install();                       return 0;
        case IDC_PLAY:     do_play();                          return 0;
        case IDC_WELCOME:  show_welcome();                     return 0;
        case IDC_BUTTONS:
            reg_set(REG_BUTTONS,
                    IsDlgButtonChecked(g_main, IDC_BUTTONS) == BST_CHECKED);
            return 0;
        case IDC_ONTOP:
            reg_set(REG_ONTOP,
                    IsDlgButtonChecked(g_main, IDC_ONTOP) == BST_CHECKED);
            return 0;
        case IDC_RESUME:
            g_resume = IsDlgButtonChecked(g_main, IDC_RESUME) == BST_CHECKED;
            reg_set(REG_RESUME, g_resume);
            return 0;
        }
        return 0;
    }

    case WM_NOTIFY: {
        LPNMHDR nh = (LPNMHDR)lp;

        /* Each device remembers its own ROM and library paths. */
        if (nh->idFrom == IDC_DEVICE && nh->code == TCN_SELCHANGE) {
            /* Re-selecting the same device preserves a hand-picked Save path. */
            const DlcDevice *d = dlc_device_at(TabCtrl_GetCurSel(g_devtabs));
            if (d && d != g_dev) {
                g_dev = d;
                /* Set the device library before refresh_device_ui rescans it. */
                wchar_t lib[MAX_PATH];
                default_lib(g_dev, lib, MAX_PATH);
                set_text(IDC_LIB, lib);
                refresh_device_ui();      /* enables, tabs, and a rescan */
                wchar_t rom[MAX_PATH];
                default_rom(g_dev, rom, MAX_PATH);
                set_text(IDC_ROM, rom);
                g_save_follows_rom = 1;
                on_rom_changed();
            }
            return 0;
        }

        if (nh->idFrom == IDC_TABS && nh->code == TCN_SELCHANGE) {
            populate_list();
            return 0;
        }

        /* Toggle tile clicks; the control handles direct checkbox clicks. */
        if (nh->idFrom == IDC_LIST && nh->code == NM_CLICK) {
            NMITEMACTIVATE *ia = (NMITEMACTIVATE *)lp;
#ifdef LAUNCHER_DEBUG
            {
                FILE *d = fopen("launcher_debug.txt", "a");
                if (d) { fprintf(d, "NM_CLICK item=%d pt=%ld,%ld\n",
                                 ia->iItem, ia->ptAction.x, ia->ptAction.y);
                         fclose(d); }
            }
#endif
            if (ia->iItem >= 0) {
                LVHITTESTINFO ht;
                memset(&ht, 0, sizeof ht);
                ht.pt = ia->ptAction;
                ListView_HitTest(g_list, &ht);
                if (!(ht.flags & LVHT_ONITEMSTATEICON))
                    ListView_SetCheckState(g_list, ia->iItem,
                        !ListView_GetCheckState(g_list, ia->iItem));
            }
            return 0;
        }
        /* Undo the tile click before opening its multi-part set. */
        if (nh->idFrom == IDC_LIST && nh->code == NM_DBLCLK) {
            NMITEMACTIVATE *ia = (NMITEMACTIVATE *)lp;
            if (ia->iItem >= 0) {
                LVHITTESTINFO ht;
                memset(&ht, 0, sizeof ht);
                ht.pt = ia->ptAction;
                ListView_HitTest(g_list, &ht);
                LVITEMW lv;
                memset(&lv, 0, sizeof lv);
                lv.mask  = LVIF_PARAM;
                lv.iItem = ia->iItem;
                if (!(ht.flags & LVHT_ONITEMSTATEICON) &&
                    ListView_GetItem(g_list, &lv) &&
                    lv.lParam >= 0 && lv.lParam < g_nitems) {
                    int members[SET_MAX];
                    if (set_of((int)lv.lParam, members, SET_MAX) >= 2) {
                        ListView_SetCheckState(g_list, ia->iItem,
                            !ListView_GetCheckState(g_list, ia->iItem));
                        open_set_dialog((int)lv.lParam);
                    }
                }
            }
            return 0;
        }
        if (nh->idFrom == IDC_LIST && nh->code == LVN_ITEMCHANGED && !g_populating) {
            NMLISTVIEW *nl = (NMLISTVIEW *)lp;
            if (nl->uChanged & LVIF_STATE) {
                UINT o = nl->uOldState & LVIS_STATEIMAGEMASK;
                UINT n = nl->uNewState & LVIS_STATEIMAGEMASK;
                if (o != n && nl->lParam >= 0 && nl->lParam < g_nitems) {
                    g_checked[nl->lParam] = (n == INDEXTOSTATEIMAGEMASK(2));
                    refresh_install_enabled();
                }
            }
            return 0;
        }
        return 0;
    }

    case WM_CTLCOLORSTATIC:
        /* Keep read-only status fields white. */
        if ((HWND)lp == g_status) {
            SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        return DefWindowProcW(hw, msg, wp, lp);

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE prev, LPSTR cmdline, int show)
{
    (void)prev; (void)cmdline;

#ifdef LAUNCHER_DEBUG
    launcher_debug_resume_check();
#endif

    /* Select the default device before building its tabs. */
    g_dev = dlc_device_default();

    OleInitialize(NULL);                    /* SHBrowseForFolder's new UI */
    {
        GpStartupInput gsi;
        memset(&gsi, 0, sizeof gsi);
        gsi.GdiplusVersion = 1;
        GdiplusStartup(&g_gdip, &gsi, NULL);      /* item previews */
    }
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof icc;
    icc.dwICC  = ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES
               | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    HDC screen = GetDC(NULL);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(NULL, screen);

    NONCLIENTMETRICSW ncm;
    memset(&ncm, 0, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0))
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    if (!g_font) g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    {
        LOGFONTW lf;
        if (GetObjectW(g_font, sizeof lf, &lf)) {
            lf.lfWeight = FW_BOLD;
            g_bold = CreateFontIndirectW(&lf);
        }
        if (!g_bold) g_bold = g_font;
    }
    g_mono = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize        = sizeof wc;
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = hi;
    wc.hIcon         = LoadIconW(hi, MAKEINTRESOURCEW(1));
    wc.hIconSm       = wc.hIcon;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"TamaLauncher";
    if (!RegisterClassExW(&wc)) return 1;

    RECT rc = { 0, 0, S(CLIENTW), S(CLIENTH) };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX
                | WS_CLIPCHILDREN;
    AdjustWindowRectEx(&rc, style, FALSE, WS_EX_CONTROLPARENT);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    int wx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
    int wy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 3;

    g_main = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, APP_TITLE,
                             style, wx < 0 ? 0 : wx, wy < 0 ? 0 : wy, ww, wh,
                             NULL, NULL, hi, NULL);
    if (!g_main) return 1;

    find_root();
    build_ui();

    wchar_t rom[MAX_PATH], sav[MAX_PATH + 64], lib[MAX_PATH];
    default_rom(g_dev, rom, MAX_PATH);
    set_text(IDC_ROM, rom);
    if (rom[0]) {
        default_savepath(sav, MAX_PATH + 64);
        set_text(IDC_SAV, sav);
    }
    default_lib(g_dev, lib, MAX_PATH);
    set_text(IDC_LIB, lib);

    rescan_library();
    refresh_status();
    /* Validate programmatic paths explicitly. */
    refresh_devnote();

    ShowWindow(g_main, show);
    UpdateWindow(g_main);

    /* Show first-run help after the main window appears. */
    if (!reg_get(REG_WELCOME, 0)) show_welcome();
#ifdef LAUNCHER_DEBUG
    dump_layout();
#endif

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_main, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    free(g_items);
    free(g_checked);
    if (g_imgs) ImageList_Destroy(g_imgs);
    if (g_mono) DeleteObject(g_mono);
    if (g_bold && g_bold != g_font) DeleteObject(g_bold);
    if (g_gdip) GdiplusShutdown(g_gdip);
    OleUninitialize();
    return 0;
}
