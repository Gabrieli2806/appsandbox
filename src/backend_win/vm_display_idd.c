/*
 * vm_display_idd.c -- Host-side IDD frame receiver + D3D11 renderer.
 *
 * Connects to the guest VM over AF_HYPERV sockets:
 *   :0002  Frame channel — receives frames, renders via D3D11 textured quad
 *   :0003  Input channel — forwards keyboard/mouse events to guest
 *   :0004  Audio channel — receives audio from guest, renders via WASAPI
 *
 * Clipboard sync (:0005/:0006) is handled by vm_clipboard.c.
 *
 * Pure C, compiled as C.
 */

#include <winsock2.h>
#include <windows.h>

#define COBJMACROS
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>

#pragma warning(push)
#pragma warning(disable: 4201) /* nameless struct/union in SDK headers */
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#pragma warning(pop)

#include <stdio.h>
#include <math.h>
#include <stdarg.h>

#include "vm_display_idd.h"
#include "vm_video_decode.h"
#include "vm_display_d3d12.h"
#include "../core/asb_display_protocol.h"
#include "asb_core.h"
#include "../core/protocol.h"
#include "vm_clipboard.h"
#include "vm_agent.h"
#include "hcs_vm.h"
#include "ui.h"
#include "resource.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "imm32.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif


/* ---- Hyper-V socket definitions ---- */

#define AF_HYPERV       34
#define HV_PROTOCOL_RAW 1

typedef struct _SOCKADDR_HV {
    ADDRESS_FAMILY Family;
    USHORT Reserved;
    GUID VmId;
    GUID ServiceId;
} SOCKADDR_HV;

/* These three are superseded by hcs_service_guid(os_type, port, ...) — kept
   for grep. Windows VMs reach byte-identical GUIDs via the helper; Linux
   VMs reach vsock-template GUIDs. */
static const GUID FRAME_SERVICE_GUID =
    { 0xa5b0cafe, 0x0002, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* Input channel service GUID — connects to agent for SendInput injection */
static const GUID INPUT_SERVICE_GUID =
    { 0xa5b0cafe, 0x0003, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* Audio capture channel — connects to guest audio helper (guest→host) */
static const GUID AUDIO_SERVICE_GUID =
    { 0xa5b0cafe, 0x0004, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* ---- Audio wire protocol (mirror of tools/agent/appsandbox-audio.c) ---- */

#define AUDIO_HEADER_MAGIC  0x31415341  /* 'ASA1' */

#pragma pack(push, 1)
typedef struct AudioHeader {
    UINT32 magic;
    UINT32 sample_rate;
    UINT16 channels;
    UINT16 bits_per_sample;
    UINT16 format_tag;      /* 1=PCM, 3=IEEE_FLOAT */
    UINT16 block_align;
} AudioHeader;

typedef struct AudioFrameHeader {
    UINT32 bytes;
} AudioFrameHeader;
#pragma pack(pop)

/* Clipboard reader-apply message (posted by vm_clipboard.c to our wndproc) */
#define WM_CLIP_READER_APPLY (WM_APP + 11)

/* ---- Frame protocol constants ----
   The wire protocol itself (magics, message layouts, protocol versions) is
   src/core/asb_display_protocol.h, shared with the guest display driver;
   the names below are this file's aliases for it. */

#define FRAME_MAGIC         VDD_FRAME_MAGIC
#define DEFAULT_WIDTH       1920
#define DEFAULT_HEIGHT      1080
#define MAX_DIRTY_RECTS     64
/* Largest frame the guest may send (8K BGRA). The receive buffer grows on
   demand up to this size. */
#define MAX_FRAME_WIDTH     7680
#define MAX_FRAME_HEIGHT    4320
#define MAX_FRAME_DATA_SIZE ((UINT)MAX_FRAME_WIDTH * MAX_FRAME_HEIGHT * 4)

/* Mode request (ASMR) and hello (ASHQ/ASVH): see the shared header. */
#define MODE_REQUEST_MAGIC  VDD_MODE_REQUEST_MAGIC
/* Frames received with no answer to the query before the guest counts as an
   older driver. Idle guests resend a frame every ~100 ms, so this is ~2 s. */
#define HELLO_FRAME_GRACE   20
typedef VDD_WIRE_MODE_REQUEST ModeRequest;

/* Performance mode (driver protocol 3+): ASPF request (ModeRequest shape),
   ASPS status, ASVE video access units. */
#define PERF_REQUEST_MAGIC  VDD_PERF_REQUEST_MAGIC
#define PERF_STATUS_MAGIC   VDD_PERF_STATUS_MAGIC
#define VIDEO_MAGIC         VDD_VIDEO_MAGIC
#define PERF_FLAG_KEYFRAME  VDD_PERF_FLAG_KEYFRAME
#define VIDEO_FLAG_KEY      VDD_VIDEO_FLAG_KEY
#define VDD_PROTOCOL_VIDEO  3
#define MAX_VIDEO_AU        (32u * 1024 * 1024)
#define MAX_VIDEO_QUEUE     (64u * 1024 * 1024)

typedef VDD_WIRE_VIDEO_HEADER VideoHeader;

#pragma pack(push, 1)
/* One queued access unit, followed by `len` bytes. */
typedef struct VideoQEntry {
    UINT32 len;
    UINT32 width;
    UINT32 height;
    UINT32 flags;
    UINT32 codec;
} VideoQEntry;
#pragma pack(pop)

#define STREAM_QUALITY      0   /* raw frames (exact pixels) */
#define STREAM_PERFORMANCE  1   /* GPU-encoded video (games), the default */

/* Video codec choice (performance mode). Auto = HEVC when the guest driver
   and this PC can do it, else H.264. */
#define CODEC_AUTO          0
#define CODEC_H264          1
#define CODEC_HEVC          2
#define VDD_PROTOCOL_HEVC   4

/* Display resolution choices (system menu > Display). RES_ADAPTIVE follows
   the window's client size. */
#define RES_ADAPTIVE        (-1)
typedef struct { UINT w, h; } ResPreset;
static const ResPreset g_res_presets[] = {
    { 1280,  720 }, { 1600,  900 }, { 1920, 1080 }, { 1920, 1200 },
    { 2560, 1440 }, { 3440, 1440 }, { 3840, 2160 },
};
#define RES_DEFAULT_PRESET  2   /* 1920x1080, the previous fixed mode */
static const UINT g_refresh_rates[] = { 60, 120, 144 };

/* Wait after the last window resize before asking the guest for a new size,
   so a drag produces one mode change instead of dozens. */
#define ADAPTIVE_DEBOUNCE_MS 400

/* ---- Window messages ---- */

#define WM_VM_DISPLAY_CLOSED    (WM_APP + 5)
#define WM_IDD_FRAME_READY      (WM_USER + 100)
#define WM_IDD_FOCUS            (WM_USER + 101)
#define WM_IDD_INPUT_READY      (WM_USER + 102)
#define WM_IDD_CURSOR_CHANGED   (WM_USER + 103)
#define WM_IDD_FRAME_CONNECTED  (WM_USER + 104)
#define WM_IDD_SET_MODE         (WM_USER + 105)  /* wp = res_choice + 1, lp = Hz */
#define WM_IDD_SET_STREAM       (WM_USER + 106)  /* wp = STREAM_* */
#define WM_IDD_VIDEO_STATUS     (WM_USER + 107)  /* guest reported video on/off; wp=1 prefs changed */
#define WM_IDD_SET_CODEC        (WM_USER + 108)  /* wp = CODEC_* */
#define WM_IDD_SET_BITRATE      (WM_USER + 109)  /* wp = Mbit/s, 0 = auto */
#define WM_IDD_SET_ICON         (WM_USER + 110)  /* reload the VM's custom icon */

/* Timer for Present cadence when no frames arrive */
#define IDT_PRESENT     2001
#define PRESENT_MS      16   /* fallback repaint; frames themselves repaint immediately */
#define IDT_ADAPTIVE    2002 /* debounce adaptive resolution after resizes */
#define IDT_APPLY_MODE  2003 /* re-apply the chosen mode after the frame channel connects */
#define IDT_TOOLBAR     2004 /* hides the floating toolbar once the pointer leaves it */

/* Debug log window */
#define IDC_LOG_LIST      3001
#define MAX_LOG_LINES     200
#define LOG_WINDOW_W      800
#define LOG_WINDOW_H      300

/* ---- HLSL shaders (inline strings) ---- */

static const char g_vs_hlsl[] =
    "struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "VS_OUT main(uint id : SV_VertexID) {\n"
    "    VS_OUT o;\n"
    "    o.uv = float2((id << 1) & 2, id & 2);\n"
    "    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return o;\n"
    "}\n";

static const char g_ps_hlsl[] =
    "Texture2D tex : register(t0);\n"
    "SamplerState samp : register(s0);\n"
    "float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {\n"
    "    return tex.Sample(samp, uv);\n"
    "}\n";

/* Adaptive resolution backdrop. While the guest catches up with a new
   window shape, the whole window is drawn in one pass: a blurred, dimmed
   copy of the picture scaled to cover the window (small mip level + 5x5
   tent), and the sharp picture on top whose edges fade into it over a
   feather band, so there is no visible seam.
   b0: a = cover scale xy, lod, dim; b = backdrop texel xy, fade, 0;
       r = picture rect in window uv (x0 y0 x1 y1); f = feather xy in uv. */
static const char g_ps_blur_hlsl[] =
    "Texture2D pic : register(t0);\n"
    "Texture2D bg : register(t1);\n"
    "SamplerState samp : register(s0);\n"
    "cbuffer P : register(b0) { float4 a; float4 b; float4 r; float4 f; };\n"
    "float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {\n"
    "    float2 c = 0.5 + (uv - 0.5) * a.xy;\n"
    "    float4 s = 0; float w = 0;\n"
    "    [unroll] for (int y = -2; y <= 2; y++)\n"
    "    [unroll] for (int x = -2; x <= 2; x++) {\n"
    "        float k = (3 - abs(x)) * (3 - abs(y));\n"
    "        s += bg.SampleLevel(samp, c + float2(x, y) * b.xy * 1.5, a.z) * k;\n"
    "        w += k;\n"
    "    }\n"
    "    float3 back = (s / w).rgb * a.w * b.z;\n"
    "    float2 puv = (uv - r.xy) / (r.zw - r.xy);\n"
    "    float3 sharp = pic.SampleLevel(samp, saturate(puv), 0).rgb;\n"
    "    float2 d = min(uv - r.xy, r.zw - uv) / f.xy;\n"
    "    float m = smoothstep(0.0, 1.0, saturate(min(d.x, d.y)));\n"
    "    return float4(lerp(back, sharp, m), 1);\n"
    "}\n";

/* ---- Frame and cursor headers from the wire (shared header) ---- */

typedef VDD_WIRE_FRAME_HEADER FrameHeader;
typedef VDD_WIRE_CURSOR_HEADER CursorHeader;
#define CURSOR_MAGIC        VDD_CURSOR_MAGIC
#define MAX_CURSOR_SIZE     (256 * 256 * 4 * 2)  /* 2x for MASKED_COLOR double-height */

/* ---- Display context ---- */

struct VmDisplayIdd {
    UINT64       vm_id;
    UINT_PTR     token;
    wchar_t      vm_name[256];     /* copy of vm->name for safe logging after VM teardown */
    GUID         runtime_id;       /* copy of vm->runtime_id for safe HvSocket after teardown */
    wchar_t      os_type[32];      /* copy of vm->os_type for picking the service GUID variant */
    HINSTANCE    hInstance;
    HWND         main_hwnd;
    HWND         hwnd;
    volatile BOOL open;
    volatile BOOL stop;
    BOOL          borderless_fullscreen;

    /* Floating toolbar (owned popup over the top edge of the render area). */
    HWND          toolbar_hwnd;
    BOOL          toolbar_pinned;
    int           toolbar_hot;        /* item under the pointer, -1 none */
    RECT          toolbar_items[12];  /* client rects of the items, for hit tests */
    int           toolbar_count;
    LONG_PTR      windowed_style;
    WINDOWPLACEMENT windowed_placement;

    /* D3D11 */
    ID3D11Device            *device;
    ID3D11DeviceContext     *ctx;
    IDXGISwapChain          *swap_chain;
    ID3D11RenderTargetView  *rtv;
    ID3D11Texture2D         *frame_tex;
    ID3D11ShaderResourceView *frame_srv;
    UINT                     tex_width, tex_height;   /* size of frame_tex */
    ID3D11VertexShader      *vs;
    ID3D11PixelShader       *ps;
    ID3D11PixelShader       *ps_blur;     /* adaptive backdrop */
    Dx12Presenter           *dx12;        /* Settings > Renderer = D3D12: presents instead of swap_chain */
    ID3D11Buffer            *cb_blur;
    ID3D11Texture2D         *bg_tex;      /* frame copy with mips for the backdrop */
    /* Backdrop animation: fade in when bars appear; the picture rect eases
       to its new place when the window or the guest resolution changes. */
    ULONGLONG                bd_start;    /* tick the bars appeared, 0 = none */
    float                    anim_rect[4];/* shown picture rect (x, y, w, h) */
    BOOL                     anim_valid;
    ULONGLONG                anim_tick;
    BOOL                     anim_active; /* keep repainting until settled */
    ID3D11ShaderResourceView *bg_srv;
    UINT                     bg_w, bg_h;
    BOOL                     bg_stale;
    ID3D11SamplerState      *sampler;

    /* Frame buffer (CPU-side, updated by recv thread) */
    BYTE          *frame_buf;
    UINT           frame_width;
    UINT           frame_height;
    UINT           frame_stride;
    CRITICAL_SECTION frame_cs;
    volatile BOOL  frame_dirty;

    UINT           render_count;     /* number of renders (for one-shot logging) */
    volatile UINT  recv_count;       /* number of frames received over HvSocket */

    /* Frame channel socket, shared with the window thread for mode requests.
       frame_send_lock guards it against the recv thread closing it. */
    volatile SOCKET frame_socket;
    SRWLOCK        frame_send_lock;

    /* Display mode (persisted in display_settings.json). res_choice is an
       index into g_res_presets or RES_ADAPTIVE. Mode changes are only sent to
       Windows guests (the Linux guest has its own display path). */
    int            res_choice;
    UINT           refresh_hz;
    BOOL           mode_supported;
    BOOL           guest_linux;      /* Linux guest: the display daemon applies modes itself */
    /* Guest driver generation on the current frame connection: -1 unknown,
       0 older (no hello before the first frame), >= 2 from its hello. */
    volatile LONG  guest_driver;
    UINT           frames_since_connect;

    /* Performance mode (GPU video). stream_mode is the user's choice,
       video_active what the guest driver reported. Encoded access units are
       queued under frame_cs (VideoQEntry + bytes, back to back) by the recv
       thread and decoded on the window thread, which owns the D3D context. */
    int            stream_mode;
    int            codec_choice;     /* CODEC_* */
    UINT           bitrate_mbps;     /* video bit rate, 0 = automatic */
    volatile LONG  video_codec;      /* codec the guest is sending (VM_VIDEO_CODEC_*) */
    BOOL           hevc_unavailable; /* HEVC decoding failed here: use H.264 */
    volatile LONG  video_active;
    volatile LONG  video_status_hr;
    BYTE          *vq_buf;           /* filled by the recv thread */
    UINT           vq_len, vq_cap;
    BYTE          *vq_work;          /* being decoded by the window thread */
    UINT           vq_work_cap;
    BOOL           vq_wait_key;      /* drop until a key frame (queue overflow) */
    VmVideoDecoder *vdec;
    BOOL           vdec_unavailable; /* this PC cannot decode: stay raw */
    BOOL           showing_video;    /* last picture drawn came from the decoder */
    UINT           req_width, req_height, req_refresh;  /* last mode requested */

    /* Frame rate shown in the title (frames received from the guest). */
    UINT           fps_value;
    UINT           fps_last_recv;
    ULONGLONG      fps_tick;

    /* Input forwarding */
    volatile SOCKET input_socket;   /* input socket for keyboard/mouse forwarding */
    SRWLOCK        input_lock;
    volatile LONG  keyboard_version;
    volatile LONG  mouse_version;
    volatile BOOL  frame_connected;
    BOOL           relative_mouse;
    BOOL           input_sizing;
    UINT           mouse_buttons;
    BOOL           raw_absolute_valid;
    HANDLE         raw_absolute_device;
    POINT          raw_absolute_position;
    BOOL           mouse_sync_pending;
    UINT32         mouse_sync_id;
    ULONGLONG      mouse_sync_deadline;
    POINT          mouse_sync_origin;
    SOCKET         mouse_sync_socket;
    InputPacket    mouse_reply;
    int            mouse_reply_size;
    BOOL           mouse_in;        /* TRUE while cursor is inside the render area */
    BOOL           tracking;        /* TrackMouseEvent active */

    /* Keyboard hotkey handling */
    wchar_t        vhdx_path[MAX_PATH]; /* copy of vm->vhdx_path for settings file */
    volatile BOOL  transmit_hotkeys; /* TRUE = capture host hotkeys + send to guest */
    volatile BOOL  input_focused;    /* TRUE while our top-level window is active */
    HHOOK          kbd_hook;          /* WH_KEYBOARD_LL handle, NULL when not installed */
    /* Legacy keys use VK indices; physical keys use scan/E0 or 512 + function VK. */
    BYTE           held_down[768];
    InputPacket    held_keys[768];
    BYTE           hook_routes[768];
    BOOL           input_menu_active;

    /* Guest cursor */
    HCURSOR        guest_cursor;    /* current cursor created from guest bitmap */
    UINT32         cursor_shape_id; /* tracks which shape is current */
    BOOL           cursor_visible;  /* guest cursor visibility */

    /* Debug log window (separate top-level window) */
    HWND           log_hwnd;        /* top-level log window */
    HWND           log_list_hwnd;   /* listbox inside log window */
    HWND           render_hwnd;     /* child window for D3D11 rendering */

    /* Clipboard (extracted to vm_clipboard.c) */
    VmClipboard      clipboard;

    /* Clipboard sharing toggles (system-menu overrides, persisted in
       display_settings.json). Default TRUE = share in both directions. */
    volatile BOOL    clip_share_host_to_guest;
    volatile BOOL    clip_share_guest_to_host;

    /* Audio playback channel (:0004 — guest→host render) */
    volatile SOCKET  audio_socket;
    HANDLE           audio_recv_thread;
    volatile BOOL    audio_muted;

    /* Threads */
    HANDLE         recv_thread;
    HANDLE         gamepad_thread;   /* forwards host controllers (XInput) */
    HICON          custom_icon_big;  /* icon.ico beside the disk, if any */
    HICON          custom_icon_small;
    HANDLE         window_thread;
};

/* ---- Forward declarations ---- */

static LRESULT CALLBACK idd_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static DWORD WINAPI     idd_window_thread_proc(LPVOID param);
static DWORD WINAPI     idd_recv_thread_proc(LPVOID param);
static void idd_update_relative_mouse(VmDisplayIdd *d);
static void idd_resume_absolute_mouse(VmDisplayIdd *d, const InputPacket *reply);
static void idd_poll_mouse_position(VmDisplayIdd *d);
static void window_to_vm_coords(HWND hwnd, int wx, int wy, UINT vm_w, UINT vm_h,
                                UINT *vx, UINT *vy);
static void idd_toggle_borderless_fullscreen(VmDisplayIdd *d, HWND hwnd);

/* ---- Window class ---- */

static const wchar_t *IDD_DISPLAY_CLASS = L"AppSandboxIddDisplay";
static const wchar_t *IDD_RENDER_CLASS  = L"AppSandboxIddRender";
static const wchar_t *IDD_TOOLBAR_CLASS = L"AppSandboxIddToolbar";
static LRESULT CALLBACK idd_toolbar_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static void tb_refresh(VmDisplayIdd *d);
static void tb_show(VmDisplayIdd *d, UINT linger_ms);
static void tb_hide(VmDisplayIdd *d);
static void tb_position(VmDisplayIdd *d);
static void tb_on_timer(VmDisplayIdd *d);
static const wchar_t *IDD_LOG_CLASS     = L"AppSandboxIddLog";

/* System menu command IDs — must be < 0xF000 and have low 4 bits clear */
#define IDM_AUDIO_MUTE     0x1000
#define IDM_XMIT_HOTKEYS   0x1010
#define IDM_SHOW_LOG       0x1020
#define IDM_CLIP_H2G       0x1030
#define IDM_CLIP_G2H       0x1040
#define IDM_BORDERLESS_FULLSCREEN 0x1050
/* Display submenu: adaptive, presets at IDM_RES_BASE + i*0x10, refresh rates
   at IDM_HZ_BASE + i*0x10 (system menu IDs keep the low 4 bits clear). */
#define IDM_RES_ADAPTIVE   0x1100
#define IDM_RES_BASE       0x1110
#define IDM_HZ_BASE        0x1200
#define IDM_CODEC_AUTO     0x1320
#define IDM_CODEC_H264     0x1330
#define IDM_CODEC_HEVC     0x1340
#define IDM_STREAM_QUALITY 0x1300
#define IDM_STREAM_PERF    0x1310
static BOOL g_idd_class_registered;
static WNDPROC g_orig_listbox_proc;
static SRWLOCK g_mouse_capture_lock = SRWLOCK_INIT;
static HWND g_mouse_capture_hwnd;

/* Listbox subclass — handles Ctrl+A / Ctrl+C */
static LRESULT CALLBACK idd_log_listbox_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000)) {
        if (wp == 'C') {
            LRESULT sel_count = SendMessageW(hwnd, LB_GETSELCOUNT, 0, 0);
            if (sel_count > 0) {
                int *indices = (int *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)sel_count * sizeof(int));
                if (indices) {
                    LRESULT i;
                    SIZE_T total = 0;
                    wchar_t *text;
                    SendMessageW(hwnd, LB_GETSELITEMS, (WPARAM)sel_count, (LPARAM)indices);
                    for (i = 0; i < sel_count; i++)
                        total += (SIZE_T)SendMessageW(hwnd, LB_GETTEXTLEN, (WPARAM)indices[i], 0) + 2;
                    total++;
                    text = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total * sizeof(wchar_t));
                    if (text) {
                        wchar_t *p = text;
                        for (i = 0; i < sel_count; i++) {
                            LRESULT len = SendMessageW(hwnd, LB_GETTEXT, (WPARAM)indices[i], (LPARAM)p);
                            p += len;
                            *p++ = L'\r'; *p++ = L'\n';
                        }
                        if (OpenClipboard(hwnd)) {
                            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)(p - text + 1) * sizeof(wchar_t));
                            if (hMem) {
                                void *dst = GlobalLock(hMem);
                                if (dst) {
                                    memcpy(dst, text, (SIZE_T)(p - text + 1) * sizeof(wchar_t));
                                    GlobalUnlock(hMem);
                                    EmptyClipboard();
                                    SetClipboardData(CF_UNICODETEXT, hMem);
                                } else {
                                    GlobalFree(hMem);
                                }
                            }
                            CloseClipboard();
                        }
                        HeapFree(GetProcessHeap(), 0, text);
                    }
                    HeapFree(GetProcessHeap(), 0, indices);
                }
            }
            return 0;
        }
        if (wp == 'A') {
            LRESULT cnt = SendMessageW(hwnd, LB_GETCOUNT, 0, 0);
            SendMessageW(hwnd, LB_SELITEMRANGE, TRUE, MAKELPARAM(0, (WORD)(cnt - 1)));
            return 0;
        }
    }
    return CallWindowProcW(g_orig_listbox_proc, hwnd, msg, wp, lp);
}

/* Log window proc — resizes listbox child to fill the window */
static LRESULT CALLBACK idd_log_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    static HBRUSH s_dark_brush = NULL;

    switch (msg) {
    case WM_SIZE: {
        HWND list = GetDlgItem(hwnd, IDC_LOG_LIST);
        if (list) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            MoveWindow(list, 0, 0, rc.right, rc.bottom, TRUE);
        }
        return 0;
    }
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        /* Match AppSandbox #log-panel: --ctrl-bg #2d2d2d, text #ccc */
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, RGB(204, 204, 204));
        SetBkColor(hdc, RGB(45, 45, 45));
        if (!s_dark_brush) s_dark_brush = CreateSolidBrush(RGB(45, 45, 45));
        return (LRESULT)s_dark_brush;
    }
    case WM_CLOSE:
        /* Just hide — the main IDD window owns the lifecycle */
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Render child window proc — forwards input + paint to parent for handling */
static LRESULT CALLBACK idd_render_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        /* Validate this window's update region; repaint once for expose/resize
           (rendering is otherwise push-driven by received frames). */
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        SendMessageW(GetParent(hwnd), WM_IDD_FRAME_READY, 0, 0);
        return 0;
    }
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_MOUSELEAVE:
    case WM_KEYDOWN: case WM_KEYUP:
    case WM_SYSKEYDOWN: case WM_SYSKEYUP:
    case WM_SETCURSOR:
        return SendMessageW(GetParent(hwnd), msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ensure_idd_class(HINSTANCE hInst)
{
    WNDCLASSEXW wc;
    if (g_idd_class_registered) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = idd_wnd_proc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPSANDBOX));
    wc.hIconSm       = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPSANDBOX));
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = IDD_DISPLAY_CLASS;
    RegisterClassExW(&wc);

    /* Render child — D3D11 swap chain targets this window.
       hCursor=NULL so WM_SETCURSOR can set the guest cursor. */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = idd_render_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = NULL;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = IDD_RENDER_CLASS;
    RegisterClassExW(&wc);

    /* Separate log window — dark background to match AppSandbox main window */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = idd_log_proc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPSANDBOX));
    wc.hIconSm       = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPSANDBOX));
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(30, 30, 30));
    wc.lpszClassName = IDD_LOG_CLASS;
    RegisterClassExW(&wc);

    /* Floating toolbar — painted with GDI, never takes focus. */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = idd_toolbar_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = IDD_TOOLBAR_CLASS;
    RegisterClassExW(&wc);

    g_idd_class_registered = TRUE;
}



/* ---- Debug log panel ---- */

static void idd_log(VmDisplayIdd *d, const wchar_t *fmt, ...)
{
    wchar_t buf[512];
    va_list ap;
    LRESULT count;

    if (!d || !d->log_list_hwnd || d->stop) return;

    va_start(ap, fmt);
    vswprintf_s(buf, 512, fmt, ap);
    va_end(ap);

    count = SendMessageW(d->log_list_hwnd, LB_ADDSTRING, 0, (LPARAM)buf);

    /* Trim old entries */
    while (count > MAX_LOG_LINES) {
        SendMessageW(d->log_list_hwnd, LB_DELETESTRING, 0, 0);
        count--;
    }

    /* Scroll to bottom and force repaint even when not focused */
    SendMessageW(d->log_list_hwnd, LB_SETTOPINDEX, (WPARAM)(count - 1), 0);
    UpdateWindow(d->log_list_hwnd);
}

/* Also into the main App Sandbox log (for messages users need to see). */
static void idd_log_main(VmDisplayIdd *d, const wchar_t *fmt, ...)
{
    wchar_t buf[512];
    va_list ap;
    if (!d) return;
    va_start(ap, fmt);
    vswprintf_s(buf, 512, fmt, ap);
    va_end(ap);
    idd_log(d, L"%s", buf);
    ui_log(L"[%s] %s", d->vm_name, buf);
}

/* ---- Send input packet to guest ---- */

static BOOL send_input_locked(VmDisplayIdd *d, UINT32 type, UINT32 p1, UINT32 p2, UINT32 p3)
{
    InputPacket pkt;
    SOCKET s;
    int ret;

    s = d->input_socket;
    if (s == INVALID_SOCKET) return FALSE;

    pkt.magic  = INPUT_MAGIC;
    pkt.type   = type;
    pkt.param1 = p1;
    pkt.param2 = p2;
    pkt.param3 = p3;

    /* Non-blocking send — drop packet if buffer full rather than stall the UI */
    ret = send(s, (const char *)&pkt, (int)sizeof(pkt), 0);
    if (ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            return FALSE;
        }
        idd_log(d, L"INPUT SEND ERR %d - flagging for reconnect.", err);
        /* Mark dead — recv thread owns the socket and will close + reconnect */
        d->input_socket = INVALID_SOCKET;
        PostMessageW(d->hwnd, WM_IDD_INPUT_READY, 0, 0);
        return FALSE;
    }
    if (ret != (int)sizeof(pkt)) {
        /* Partial send on a stream socket: the guest reads fixed-size 20-byte
           InputPackets, so a truncated packet permanently misaligns the wire.
           Flag for reconnect so the channel resynchronises. */
        idd_log(d, L"INPUT SEND short (%d/%d) - flagging for reconnect.",
                ret, (int)sizeof(pkt));
        d->input_socket = INVALID_SOCKET;
        PostMessageW(d->hwnd, WM_IDD_INPUT_READY, 0, 0);
        return FALSE;
    }

    return TRUE;
}

static void send_input(VmDisplayIdd *d, UINT32 type, UINT32 p1, UINT32 p2, UINT32 p3)
{
    AcquireSRWLockExclusive(&d->input_lock);
    send_input_locked(d, type, p1, p2, p3);
    ReleaseSRWLockExclusive(&d->input_lock);
}

/* send_input that reports whether the packet went out. */
static BOOL send_input_locked_safe(VmDisplayIdd *d, UINT32 type, UINT32 p1, UINT32 p2, UINT32 p3)
{
    BOOL ok;
    AcquireSRWLockExclusive(&d->input_lock);
    ok = send_input_locked(d, type, p1, p2, p3);
    ReleaseSRWLockExclusive(&d->input_lock);
    return ok;
}

/* ==================================================================
 * Game controllers
 *
 * Like Moonlight: the host's controllers (XInput: Xbox controllers and
 * anything Steam Input or DS4Windows presents as one) are read about 250
 * times a second while the VM window has focus and sent on the input
 * channel; the guest helper plays them through virtual Xbox 360 pads
 * (ViGEmBus). When the window loses focus the pads are centred.
 * ================================================================== */

typedef struct {
    DWORD dwPacketNumber;
    WORD  wButtons;
    BYTE  bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} IddXInputState;
typedef DWORD (WINAPI *PFN_IddXInputGetState)(DWORD, IddXInputState *);

static DWORD WINAPI idd_gamepad_thread_proc(LPVOID param)
{
    VmDisplayIdd *d = (VmDisplayIdd *)param;
    HMODULE xi = LoadLibraryExW(L"xinput1_4.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    PFN_IddXInputGetState get = NULL;
    HANDLE timer;
    BOOL connected[4] = { FALSE }, centred[4] = { FALSE };
    DWORD last_packet[4] = { 0 };
    ULONGLONG last_send[4] = { 0 }, last_scan = 0;
    int i;

    if (!xi) return 0;
    /* Ordinal 100 (XInputGetStateEx) also reports the Guide button. */
    get = (PFN_IddXInputGetState)(void *)GetProcAddress(xi, (LPCSTR)100);
    if (!get) get = (PFN_IddXInputGetState)(void *)GetProcAddress(xi, "XInputGetState");
    if (!get) { FreeLibrary(xi); return 0; }

    timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(NULL, FALSE, NULL);

    while (!d->stop) {
        ULONGLONG now = GetTickCount64();
        BOOL scan = now - last_scan >= 1000;   /* absent pads are slow to query */
        BOOL active = d->input_focused && d->frame_connected;
        LARGE_INTEGER due;

        if (scan) last_scan = now;
        for (i = 0; i < 4; i++) {
            IddXInputState st;
            UINT32 p1, p2, p3;
            if (!connected[i] && !scan) continue;
            ZeroMemory(&st, sizeof(st));
            if (get((DWORD)i, &st) != ERROR_SUCCESS) {
                if (connected[i]) {
                    connected[i] = FALSE;
                    send_input(d, INPUT_GAMEPAD_REMOVE | ((UINT32)i << 8), 0, 0, 0);
                    idd_log_main(d, L"Controller %d disconnected.", i + 1);
                }
                continue;
            }
            if (!connected[i]) {
                connected[i] = TRUE;
                centred[i] = FALSE;
                last_packet[i] = st.dwPacketNumber - 1;
                idd_log_main(d, L"Controller %d connected; it is sent to the VM while its window has focus.", i + 1);
            }
            if (!active) {
                if (!centred[i]) {   /* release everything once */
                    centred[i] = send_input_locked_safe(d, INPUT_GAMEPAD | ((UINT32)i << 8), 0, 0, 0);
                    last_packet[i] = st.dwPacketNumber - 1;
                }
                continue;
            }
            centred[i] = FALSE;
            /* On change, and every 500 ms so a dropped packet heals. */
            if (st.dwPacketNumber == last_packet[i] && now - last_send[i] < 500) continue;
            p1 = (UINT32)st.wButtons | ((UINT32)st.bLeftTrigger << 16) | ((UINT32)st.bRightTrigger << 24);
            p2 = (UINT32)(USHORT)st.sThumbLX | ((UINT32)(USHORT)st.sThumbLY << 16);
            p3 = (UINT32)(USHORT)st.sThumbRX | ((UINT32)(USHORT)st.sThumbRY << 16);
            if (send_input_locked_safe(d, INPUT_GAMEPAD | ((UINT32)i << 8), p1, p2, p3)) {
                last_packet[i] = st.dwPacketNumber;
                last_send[i] = now;
            }
        }

        due.QuadPart = -40000;   /* 4 ms */
        if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
            WaitForSingleObject(timer, 50);
        else
            Sleep(4);
    }
    for (i = 0; i < 4; i++)
        if (connected[i]) send_input(d, INPUT_GAMEPAD_REMOVE | ((UINT32)i << 8), 0, 0, 0);
    if (timer) CloseHandle(timer);
    FreeLibrary(xi);
    return 0;
}

static BOOL idd_clip_mouse(VmDisplayIdd *d)
{
    RECT rc;
    if (!GetClientRect(d->render_hwnd, &rc) || IsRectEmpty(&rc)) return FALSE;
    MapWindowPoints(d->render_hwnd, NULL, (POINT *)&rc, 2);
    return ClipCursor(&rc);
}

static void idd_cancel_mouse_sync(VmDisplayIdd *d)
{
    POINT point;
    if (!d->mouse_sync_pending) return;
    d->mouse_sync_pending = FALSE;
    if (GetForegroundWindow() == d->hwnd && GetCursorPos(&point) &&
        WindowFromPoint(point) == d->render_hwnd)
        SetCursor(!d->cursor_visible ? NULL :
                  d->guest_cursor ? d->guest_cursor : LoadCursorW(NULL, IDC_ARROW));
}

static void idd_update_relative_mouse(VmDisplayIdd *d)
{
    POINT pt = {0};
    BOOL active = !d->stop && d->frame_connected &&
                   d->mouse_version == INPUT_MOUSE_VERSION &&
                   d->input_socket != INVALID_SOCKET && d->input_focused &&
                   !d->input_menu_active && !d->input_sizing &&
                   GetForegroundWindow() == d->hwnd && !IsIconic(d->hwnd) &&
                   GetCursorPos(&pt) && WindowFromPoint(pt) == d->render_hwnd;
    BOOL capture = active && !d->cursor_visible;
    BOOL sync_absolute = FALSE;
    if (!active || capture) idd_cancel_mouse_sync(d);
    AcquireSRWLockExclusive(&g_mouse_capture_lock);
    if (capture == d->relative_mouse &&
        (!capture || g_mouse_capture_hwnd == d->hwnd)) {
        ReleaseSRWLockExclusive(&g_mouse_capture_lock);
        return;
    }
    if (capture) {
        RAWINPUTDEVICE mouse = { 0x01, 0x02, 0, d->hwnd };
        d->relative_mouse = FALSE;
        if (RegisterRawInputDevices(&mouse, 1, sizeof(mouse))) {
            if (idd_clip_mouse(d)) {
                g_mouse_capture_hwnd = d->hwnd;
                d->relative_mouse = TRUE;
                d->mouse_in = TRUE;
                d->raw_absolute_valid = FALSE;
                SetCursor(NULL);
            } else {
                mouse.dwFlags = RIDEV_REMOVE;
                mouse.hwndTarget = NULL;
                RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
            }
        }
    } else {
        if (g_mouse_capture_hwnd == d->hwnd) {
            sync_absolute = d->relative_mouse && active && d->cursor_visible;
            RAWINPUTDEVICE mouse = { 0x01, 0x02, RIDEV_REMOVE, NULL };
            RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
            ClipCursor(NULL);
            g_mouse_capture_hwnd = NULL;
        }
        d->relative_mouse = FALSE;
        d->raw_absolute_valid = FALSE;
    }
    ReleaseSRWLockExclusive(&g_mouse_capture_lock);
    if (sync_absolute && d->frame_width && d->frame_height) {
        if (d->mouse_version == INPUT_MOUSE_VERSION) {
            if (++d->mouse_sync_id == 0) ++d->mouse_sync_id;
            d->mouse_sync_origin = pt;
            d->mouse_sync_deadline = GetTickCount64() + 250;
            AcquireSRWLockExclusive(&d->input_lock);
            d->mouse_sync_socket = d->input_socket;
            d->mouse_sync_pending = send_input_locked(d, INPUT_MOUSE_POSITION_QUERY,
                                                      d->mouse_sync_id, 0, 0);
            ReleaseSRWLockExclusive(&d->input_lock);
            if (d->mouse_sync_pending) {
                SetCursor(NULL);
                return;
            }
        }
        idd_resume_absolute_mouse(d, NULL);
    }
}

static void idd_flush_mouse_buttons(VmDisplayIdd *d)
{
    for (UINT button = INPUT_BTN_LEFT; button <= INPUT_BTN_MIDDLE; button++) {
        if (d->mouse_buttons & (1u << button))
            send_input(d, INPUT_MOUSE_BUTTON, button, 0, 0);
    }
    d->mouse_buttons = 0;
    if (GetCapture() == d->render_hwnd) ReleaseCapture();
}

/* ==================================================================
 * Per-VM display settings (display_settings.json beside disk.vhdx)
 *
 * Mirrors the vm_state.json pattern in asb_core.c but is owned entirely
 * by the IDD display: the file is created lazily the first time a VM's
 * display opens, so both new and pre-existing VMs get one on demand.
 * ================================================================== */

static void idd_display_settings_path(const wchar_t *vhdx_path, wchar_t *out, size_t out_chars)
{
    wchar_t dir[MAX_PATH];
    const wchar_t *last_slash;
    wcscpy_s(dir, MAX_PATH, vhdx_path);
    last_slash = wcsrchr(dir, L'\\');
    if (last_slash) dir[last_slash - dir] = L'\0';
    swprintf_s(out, out_chars, L"%s\\display_settings.json", dir);
}

static void idd_display_settings_save(const wchar_t *vhdx_path, BOOL transmit_hotkeys,
                                      BOOL share_h2g, BOOL share_g2h,
                                      int res_choice, UINT refresh_hz, int stream_mode,
                                      int codec, UINT bitrate_mbps)
{
    wchar_t path[MAX_PATH];
    char res[32];
    FILE *f;
    if (!vhdx_path || vhdx_path[0] == L'\0') return;
    idd_display_settings_path(vhdx_path, path, MAX_PATH);
    if (res_choice == RES_ADAPTIVE)
        strcpy_s(res, sizeof(res), "adaptive");
    else if (res_choice >= 0 && res_choice < (int)ARRAYSIZE(g_res_presets))
        sprintf_s(res, sizeof(res), "%ux%u", g_res_presets[res_choice].w,
                  g_res_presets[res_choice].h);
    else
        strcpy_s(res, sizeof(res), "1920x1080");
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;
    fprintf(f, "{\"transmitKeyboardHotkeys\":%d,\"shareHostToGuest\":%d,\"shareGuestToHost\":%d,"
               "\"resolution\":\"%s\",\"refreshHz\":%u,\"streamMode\":\"%s\",\"codec\":\"%s\",\"bitrateMbps\":%u}\n",
            transmit_hotkeys ? 1 : 0, share_h2g ? 1 : 0, share_g2h ? 1 : 0,
            res, refresh_hz ? refresh_hz : 60,
            stream_mode == STREAM_QUALITY ? "quality" : "performance",
            codec == CODEC_H264 ? "h264" : codec == CODEC_HEVC ? "hevc" : "auto",
            bitrate_mbps);
    fclose(f);
}

/* Video bit rate in Mbit/s from display_settings.json (0 = automatic). */
static UINT idd_display_settings_bitrate(const wchar_t *vhdx_path)
{
    wchar_t path[MAX_PATH];
    char buf[512];
    FILE *f;
    UINT v = 0;
    const char *p;
    if (!vhdx_path || !vhdx_path[0]) return 0;
    idd_display_settings_path(vhdx_path, path, MAX_PATH);
    if (_wfopen_s(&f, path, L"r") != 0 || !f) return 0;
    if (fgets(buf, sizeof(buf), f) && (p = strstr(buf, "\"bitrateMbps\":")) != NULL)
        if (sscanf_s(p + 14, "%u", &v) != 1) v = 0;
    fclose(f);
    return v > 500 ? 500 : v;
}

/* Read the persisted setting; if the file is absent, create it with the
   defaults (hotkeys off, clipboard sharing on) and return FALSE. Returns the
   transmit-hotkeys value; the two clipboard flags are written back through
   the output params. */
static BOOL idd_display_settings_load_or_create(const wchar_t *vhdx_path,
                                                BOOL *share_h2g, BOOL *share_g2h,
                                                int *res_choice, UINT *refresh_hz,
                                                int *stream_mode, int *codec)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    char buf[512];
    BOOL transmit = FALSE;
    BOOL h2g = TRUE, g2h = TRUE;
    int res = RES_DEFAULT_PRESET;
    UINT hz = 60;
    int stream = STREAM_PERFORMANCE;
    int cdc = CODEC_AUTO;
    const char *p;

    if (!vhdx_path || vhdx_path[0] == L'\0') goto out;
    idd_display_settings_path(vhdx_path, path, MAX_PATH);

    if (_wfopen_s(&f, path, L"r") != 0 || !f) {
        /* Lazy creation: file doesn't exist yet (new or pre-existing VM). */
        idd_display_settings_save(vhdx_path, FALSE, TRUE, TRUE, RES_DEFAULT_PRESET, 60,
                                  STREAM_PERFORMANCE, CODEC_AUTO, 0);
        goto out;
    }
    if (fgets(buf, sizeof(buf), f)) {
        if (strstr(buf, "\"transmitKeyboardHotkeys\":1"))
            transmit = TRUE;
        /* Clipboard flags: missing keys keep the defaults above. */
        if (strstr(buf, "\"shareHostToGuest\""))
            h2g = strstr(buf, "\"shareHostToGuest\":1") != NULL;
        if (strstr(buf, "\"shareGuestToHost\""))
            g2h = strstr(buf, "\"shareGuestToHost\":1") != NULL;
        /* Display mode: missing or unknown values keep 1920x1080 @ 60 Hz. */
        p = strstr(buf, "\"resolution\":\"");
        if (p) {
            unsigned int w = 0, h = 0;
            p += 14;
            if (strncmp(p, "adaptive\"", 9) == 0) {
                res = RES_ADAPTIVE;
            } else if (sscanf_s(p, "%ux%u", &w, &h) == 2) {
                int i;
                for (i = 0; i < (int)ARRAYSIZE(g_res_presets); i++)
                    if (g_res_presets[i].w == w && g_res_presets[i].h == h) { res = i; break; }
            }
        }
        p = strstr(buf, "\"refreshHz\":");
        if (p) {
            unsigned int v = 0;
            int i;
            if (sscanf_s(p + 12, "%u", &v) == 1)
                for (i = 0; i < (int)ARRAYSIZE(g_refresh_rates); i++)
                    if (g_refresh_rates[i] == v) { hz = v; break; }
        }
        /* Performance is the default; only an explicit choice of quality
           keeps raw frames. */
        if (strstr(buf, "\"streamMode\":\"quality\""))
            stream = STREAM_QUALITY;
        if (strstr(buf, "\"codec\":\"h264\"")) cdc = CODEC_H264;
        else if (strstr(buf, "\"codec\":\"hevc\"")) cdc = CODEC_HEVC;
    }
    fclose(f);

out:
    if (share_h2g) *share_h2g = h2g;
    if (share_g2h) *share_g2h = g2h;
    if (res_choice) *res_choice = res;
    if (refresh_hz) *refresh_hz = hz;
    if (stream_mode) *stream_mode = stream;
    if (codec) *codec = cdc;
    return transmit;
}

/* Parse "adaptive" / "WxH" into a res_choice. FALSE if it is not offered. */
static BOOL idd_res_choice_from_text(const wchar_t *res, int *choice)
{
    unsigned int w = 0, h = 0;
    int i;
    if (!res) return FALSE;
    if (_wcsicmp(res, L"adaptive") == 0) { *choice = RES_ADAPTIVE; return TRUE; }
    if (swscanf_s(res, L"%ux%u", &w, &h) != 2) return FALSE;
    for (i = 0; i < (int)ARRAYSIZE(g_res_presets); i++)
        if (g_res_presets[i].w == w && g_res_presets[i].h == h) { *choice = i; return TRUE; }
    return FALSE;
}

void vm_display_idd_saved_mode(const wchar_t *vhdx_path, wchar_t *res, size_t res_chars,
                               UINT *hz)
{
    int choice = RES_DEFAULT_PRESET;
    UINT rate = 60;
    wchar_t path[MAX_PATH];

    /* Read only: do not create the file just because the UI looked. */
    if (vhdx_path && vhdx_path[0]) {
        idd_display_settings_path(vhdx_path, path, MAX_PATH);
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
            idd_display_settings_load_or_create(vhdx_path, NULL, NULL, &choice, &rate, NULL, NULL);
    }
    if (res && res_chars) {
        if (choice == RES_ADAPTIVE)
            wcscpy_s(res, res_chars, L"adaptive");
        else
            swprintf_s(res, res_chars, L"%ux%u", g_res_presets[choice].w, g_res_presets[choice].h);
    }
    if (hz) *hz = rate;
}

BOOL vm_display_idd_set_mode(VmDisplayIdd *display, const wchar_t *vhdx_path,
                             const wchar_t *res, UINT hz)
{
    int choice, i;
    BOOL hz_ok = FALSE;
    BOOL h2g = TRUE, g2h = TRUE, hotkeys;
    int old_choice, stream, cdc;
    UINT old_hz;

    if (!idd_res_choice_from_text(res, &choice)) return FALSE;
    for (i = 0; i < (int)ARRAYSIZE(g_refresh_rates); i++)
        if (g_refresh_rates[i] == hz) hz_ok = TRUE;
    if (!hz_ok) return FALSE;

    if (display && display->open && display->hwnd && IsWindow(display->hwnd)) {
        PostMessageW(display->hwnd, WM_IDD_SET_MODE, (WPARAM)(choice + 1), (LPARAM)hz);
        return TRUE;
    }
    if (!vhdx_path || !vhdx_path[0]) return FALSE;
    hotkeys = idd_display_settings_load_or_create(vhdx_path, &h2g, &g2h, &old_choice, &old_hz,
                                                  &stream, &cdc);
    idd_display_settings_save(vhdx_path, hotkeys, h2g, g2h, choice, hz, stream, cdc,
                              idd_display_settings_bitrate(vhdx_path));
    return TRUE;
}

BOOL vm_display_idd_saved_performance(const wchar_t *vhdx_path)
{
    int stream = STREAM_PERFORMANCE;
    wchar_t path[MAX_PATH];
    if (vhdx_path && vhdx_path[0]) {
        idd_display_settings_path(vhdx_path, path, MAX_PATH);
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
            idd_display_settings_load_or_create(vhdx_path, NULL, NULL, NULL, NULL, &stream, NULL);
    }
    return stream == STREAM_PERFORMANCE;
}

const wchar_t *vm_display_idd_saved_codec(const wchar_t *vhdx_path)
{
    int cdc = CODEC_AUTO;
    wchar_t path[MAX_PATH];
    if (vhdx_path && vhdx_path[0]) {
        idd_display_settings_path(vhdx_path, path, MAX_PATH);
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
            idd_display_settings_load_or_create(vhdx_path, NULL, NULL, NULL, NULL, NULL, &cdc);
    }
    return cdc == CODEC_H264 ? L"h264" : cdc == CODEC_HEVC ? L"hevc" : L"auto";
}

BOOL vm_display_idd_set_codec(VmDisplayIdd *display, const wchar_t *vhdx_path, const wchar_t *codec)
{
    BOOL h2g = TRUE, g2h = TRUE, hotkeys;
    int choice, stream, old;
    UINT hz;
    int cdc;
    if (!codec) return FALSE;
    if (_wcsicmp(codec, L"h264") == 0) cdc = CODEC_H264;
    else if (_wcsicmp(codec, L"hevc") == 0) cdc = CODEC_HEVC;
    else if (_wcsicmp(codec, L"auto") == 0) cdc = CODEC_AUTO;
    else return FALSE;
    if (display && display->open && display->hwnd && IsWindow(display->hwnd)) {
        PostMessageW(display->hwnd, WM_IDD_SET_CODEC, (WPARAM)cdc, 0);
        return TRUE;
    }
    if (!vhdx_path || !vhdx_path[0]) return FALSE;
    hotkeys = idd_display_settings_load_or_create(vhdx_path, &h2g, &g2h, &choice, &hz, &stream, &old);
    idd_display_settings_save(vhdx_path, hotkeys, h2g, g2h, choice, hz, stream, cdc,
                              idd_display_settings_bitrate(vhdx_path));
    return TRUE;
}

UINT vm_display_idd_saved_bitrate(const wchar_t *vhdx_path)
{
    return idd_display_settings_bitrate(vhdx_path);
}

BOOL vm_display_idd_set_bitrate(VmDisplayIdd *display, const wchar_t *vhdx_path, UINT mbps)
{
    BOOL h2g = TRUE, g2h = TRUE, hotkeys;
    int choice, stream, cdc;
    UINT hz;
    if (mbps > 500) return FALSE;
    if (display && display->open && display->hwnd && IsWindow(display->hwnd)) {
        PostMessageW(display->hwnd, WM_IDD_SET_BITRATE, (WPARAM)mbps, 0);
        return TRUE;
    }
    if (!vhdx_path || !vhdx_path[0]) return FALSE;
    hotkeys = idd_display_settings_load_or_create(vhdx_path, &h2g, &g2h, &choice, &hz, &stream, &cdc);
    idd_display_settings_save(vhdx_path, hotkeys, h2g, g2h, choice, hz, stream, cdc, mbps);
    return TRUE;
}

BOOL vm_display_idd_hevc_supported(void)
{
    return vm_video_hevc_supported();
}

/* Codec the guest is sending: L"H.264", L"HEVC" or NULL when not video. */
const wchar_t *vm_display_idd_video_codec(VmDisplayIdd *display)
{
    if (!display || !display->open || !display->frame_connected || !display->video_active) return NULL;
    return display->video_codec == VM_VIDEO_CODEC_HEVC ? L"HEVC" : L"H.264";
}

void vm_display_idd_icon_changed(VmDisplayIdd *display)
{
    if (display && display->open && display->hwnd && IsWindow(display->hwnd))
        PostMessageW(display->hwnd, WM_IDD_SET_ICON, 0, 0);
}

/* Window and taskbar icon: the VM's icon.ico if it has one, otherwise the
   app icon. */
static void idd_apply_icon(VmDisplayIdd *d)
{
    wchar_t path[MAX_PATH], *slash;
    HICON icon_big = NULL, icon_small = NULL;
    if (!d || !d->hwnd) return;
    wcscpy_s(path, MAX_PATH, d->vhdx_path);
    slash = wcsrchr(path, L'\\');
    if (slash) {
        wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"icon.ico");
        icon_big = (HICON)LoadImageW(NULL, path, IMAGE_ICON, GetSystemMetrics(SM_CXICON) * 2,
                                GetSystemMetrics(SM_CYICON) * 2, LR_LOADFROMFILE);
        icon_small = (HICON)LoadImageW(NULL, path, IMAGE_ICON, GetSystemMetrics(SM_CXSMICON) * 2,
                                  GetSystemMetrics(SM_CYSMICON) * 2, LR_LOADFROMFILE);
    }
    if (!icon_big || !icon_small) {
        if (icon_big) DestroyIcon(icon_big);
        if (icon_small) DestroyIcon(icon_small);
        icon_big = icon_small = NULL;
    }
    SendMessageW(d->hwnd, WM_SETICON, ICON_BIG, (LPARAM)(icon_big ? icon_big : LoadIconW(d->hInstance, MAKEINTRESOURCEW(IDI_APPSANDBOX))));
    SendMessageW(d->hwnd, WM_SETICON, ICON_SMALL, (LPARAM)(icon_small ? icon_small : LoadIconW(d->hInstance, MAKEINTRESOURCEW(IDI_APPSANDBOX))));
    if (d->custom_icon_big) DestroyIcon(d->custom_icon_big);
    if (d->custom_icon_small) DestroyIcon(d->custom_icon_small);
    d->custom_icon_big = icon_big;
    d->custom_icon_small = icon_small;
}

/* VM window preferences changed (Settings): refresh title and toolbar. */
void vm_display_idd_prefs_changed(VmDisplayIdd *display)
{
    if (display && display->open && display->hwnd && IsWindow(display->hwnd))
        PostMessageW(display->hwnd, WM_IDD_VIDEO_STATUS, 1, 0);
}

BOOL vm_display_idd_set_performance(VmDisplayIdd *display, const wchar_t *vhdx_path,
                                    BOOL performance)
{
    BOOL h2g = TRUE, g2h = TRUE, hotkeys;
    int choice, cdc;
    UINT hz;
    int mode = performance ? STREAM_PERFORMANCE : STREAM_QUALITY;

    if (display && display->open && display->hwnd && IsWindow(display->hwnd)) {
        PostMessageW(display->hwnd, WM_IDD_SET_STREAM, (WPARAM)mode, 0);
        return TRUE;
    }
    if (!vhdx_path || !vhdx_path[0]) return FALSE;
    hotkeys = idd_display_settings_load_or_create(vhdx_path, &h2g, &g2h, &choice, &hz, NULL, &cdc);
    idd_display_settings_save(vhdx_path, hotkeys, h2g, g2h, choice, hz, mode, cdc,
                              idd_display_settings_bitrate(vhdx_path));
    return TRUE;
}

/* 1 while the guest sends GPU video, 0 for raw frames, -1 not connected. */
int vm_display_idd_video_active(VmDisplayIdd *display)
{
    if (!display || !display->open || !display->frame_connected) return -1;
    if (display->video_active) return 1;
    if (display->stream_mode == STREAM_PERFORMANCE &&
        (display->video_status_hr < 0 || display->vdec_unavailable))
        return 2;
    return 0;
}

int vm_display_idd_guest_driver(VmDisplayIdd *display)
{
    if (!display || !display->open || !display->frame_connected) return -1;
    return (int)display->guest_driver;
}

/* Save every per-VM display setting from the live display state. */
static void idd_save_settings(VmDisplayIdd *d)
{
    idd_display_settings_save(d->vhdx_path, d->transmit_hotkeys,
                              d->clip_share_host_to_guest,
                              d->clip_share_guest_to_host,
                              d->res_choice, d->refresh_hz, d->stream_mode, d->codec_choice,
                              d->bitrate_mbps);
}

/* The codec to ask the guest for: 1 H.264, 2 HEVC. HEVC needs a protocol 4
   driver and an HEVC decoder on this PC. */
static UINT idd_wanted_codec(VmDisplayIdd *d)
{
    BOOL hevc_ok = d->guest_driver >= VDD_PROTOCOL_HEVC && !d->hevc_unavailable &&
                   vm_video_hevc_supported();
    if (d->codec_choice == CODEC_H264) return 1;
    if (d->codec_choice == CODEC_HEVC && !hevc_ok && d->guest_driver >= 0)
        idd_log_main(d, L"HEVC is not available (%s); using H.264.",
                     d->guest_driver < VDD_PROTOCOL_HEVC ? L"update the VM's guest components"
                     : !vm_video_hevc_supported() ? L"install \"HEVC Video Extensions\" from the Microsoft Store on this PC"
                     : L"decoding failed");
    return hevc_ok ? 2 : 1;
}

/* Tell the guest driver which stream to send (protocol 3 drivers only).
   flags: PERF_FLAG_KEYFRAME asks for a key frame. */
static void idd_send_perf(VmDisplayIdd *d, UINT32 flags)
{
    ModeRequest req;
    BOOL ok = FALSE;
    if (!d || d->guest_driver < VDD_PROTOCOL_VIDEO) return;
    req.magic = PERF_REQUEST_MAGIC;
    req.width = (d->stream_mode == STREAM_PERFORMANCE && !d->vdec_unavailable) ? idd_wanted_codec(d) : 0;
    req.height = d->bitrate_mbps * 1000;   /* kbit/s: 0 = the guest picks from size and refresh */
    req.refresh = flags;
    if (req.width && !(flags & PERF_FLAG_KEYFRAME))
        InterlockedExchange(&d->video_status_hr, 0);
    AcquireSRWLockExclusive(&d->frame_send_lock);
    if (d->frame_socket != INVALID_SOCKET)
        ok = send(d->frame_socket, (const char *)&req, (int)sizeof(req), 0) == (int)sizeof(req);
    ReleaseSRWLockExclusive(&d->frame_send_lock);
    if (!(flags & PERF_FLAG_KEYFRAME) || !ok)
        idd_log_main(d, L"Stream mode %s requested (%s).",
                req.width == 2 ? L"performance (GPU video, HEVC)" :
                req.width ? L"performance (GPU video, H.264)" : L"quality (raw frames)",
                ok ? L"sent" : L"not connected");
}

/* ==================================================================
 * Display mode (resolution / refresh rate)
 *
 * Two steps, both owned by this window:
 *   1. ask the guest VDD to offer the mode (frame channel, ModeRequest);
 *      a size it does not list yet makes it re-plug its monitor;
 *   2. ask the input helper in the guest's session to switch to it
 *      (INPUT_SET_DISPLAY), which retries until the mode is offered.
 * ================================================================== */

/* The mode the current choice asks for. Adaptive uses the render area in
   physical pixels (the process is per-monitor DPI aware), rounded to what
   the VDD accepts. */
static BOOL idd_target_mode(VmDisplayIdd *d, UINT *w, UINT *h, UINT *hz)
{
    *hz = d->refresh_hz ? d->refresh_hz : 60;
    if (d->res_choice == RES_ADAPTIVE) {
        RECT rc;
        HWND target = d->render_hwnd ? d->render_hwnd : d->hwnd;
        if (!target || !GetClientRect(target, &rc) || rc.right <= 0 || rc.bottom <= 0)
            return FALSE;
        *w = (UINT)rc.right & ~7u;
        *h = (UINT)rc.bottom & ~1u;
        if (*w < 640) *w = 640;
        if (*h < 480) *h = 480;
        if (*w > MAX_FRAME_WIDTH) *w = MAX_FRAME_WIDTH;
        if (*h > MAX_FRAME_HEIGHT) *h = MAX_FRAME_HEIGHT;
        return TRUE;
    }
    if (d->res_choice < 0 || d->res_choice >= (int)ARRAYSIZE(g_res_presets))
        d->res_choice = RES_DEFAULT_PRESET;
    *w = g_res_presets[d->res_choice].w;
    *h = g_res_presets[d->res_choice].h;
    return TRUE;
}

/* Send the chosen mode to the guest. force re-sends even when it matches the
   last request (after the frame channel reconnects). */
static void idd_apply_display_mode(VmDisplayIdd *d, BOOL force)
{
    UINT w, h, hz;
    ModeRequest req;
    BOOL vdd_ok = FALSE, input_ok;

    if (!d || !d->mode_supported) return;
    if (d->guest_driver == 0) {
        idd_log(d, L"Display mode not changed: this VM's display driver is too old.");
        return;
    }
    if (!idd_target_mode(d, &w, &h, &hz)) return;
    if (!force && w == d->req_width && h == d->req_height && hz == d->req_refresh)
        return;

    req.magic = MODE_REQUEST_MAGIC;
    req.width = w;
    req.height = h;
    req.refresh = hz;
    AcquireSRWLockExclusive(&d->frame_send_lock);
    if (d->frame_socket != INVALID_SOCKET)
        vdd_ok = send(d->frame_socket, (const char *)&req, (int)sizeof(req), 0) == (int)sizeof(req);
    ReleaseSRWLockExclusive(&d->frame_send_lock);

    /* Windows guests switch in the session through the input helper; on
       Linux the display daemon that received the request does it. */
    if (d->guest_linux) {
        input_ok = TRUE;
    } else {
        AcquireSRWLockExclusive(&d->input_lock);
        input_ok = send_input_locked(d, INPUT_SET_DISPLAY, w, h, hz);
        ReleaseSRWLockExclusive(&d->input_lock);
    }

    if (vdd_ok && input_ok) {
        d->req_width = w;
        d->req_height = h;
        d->req_refresh = hz;
    }
    idd_log(d, L"Display mode %ux%u @ %u Hz requested (driver %s, session %s).",
            w, h, hz, vdd_ok ? L"ok" : L"not connected", input_ok ? L"ok" : L"not connected");
}

/* Radio checks in the Display submenu. */
static void idd_update_display_menu(VmDisplayIdd *d)
{
    HMENU sysmenu, sub = NULL;
    int i, count;
    if (!d || !d->hwnd) return;
    sysmenu = GetSystemMenu(d->hwnd, FALSE);
    if (!sysmenu) return;
    count = GetMenuItemCount(sysmenu);
    for (i = 0; i < count; i++) {
        HMENU h = GetSubMenu(sysmenu, i);
        if (h && GetMenuState(h, IDM_RES_ADAPTIVE, MF_BYCOMMAND) != (UINT)-1) { sub = h; break; }
    }
    if (!sub) return;
    CheckMenuItem(sub, IDM_RES_ADAPTIVE,
                  MF_BYCOMMAND | (d->res_choice == RES_ADAPTIVE ? MF_CHECKED : MF_UNCHECKED));
    for (i = 0; i < (int)ARRAYSIZE(g_res_presets); i++)
        CheckMenuItem(sub, IDM_RES_BASE + i * 0x10,
                      MF_BYCOMMAND | (d->res_choice == i ? MF_CHECKED : MF_UNCHECKED));
    for (i = 0; i < (int)ARRAYSIZE(g_refresh_rates); i++)
        CheckMenuItem(sub, IDM_HZ_BASE + i * 0x10,
                      MF_BYCOMMAND | (d->refresh_hz == g_refresh_rates[i] ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(sub, IDM_CODEC_AUTO,
                  MF_BYCOMMAND | (d->codec_choice == CODEC_AUTO ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(sub, IDM_CODEC_H264,
                  MF_BYCOMMAND | (d->codec_choice == CODEC_H264 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(sub, IDM_CODEC_HEVC,
                  MF_BYCOMMAND | (d->codec_choice == CODEC_HEVC ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(sub, IDM_STREAM_QUALITY,
                  MF_BYCOMMAND | (d->stream_mode == STREAM_QUALITY ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(sub, IDM_STREAM_PERF,
                  MF_BYCOMMAND | (d->stream_mode == STREAM_PERFORMANCE ? MF_CHECKED : MF_UNCHECKED));
}

/* Build the Display submenu (resolution + refresh rate). */
static void idd_add_display_menu(VmDisplayIdd *d, HMENU sysmenu)
{
    HMENU sub;
    int i;
    if (!d->mode_supported) return;
    sub = CreatePopupMenu();
    if (!sub) return;
    AppendMenuW(sub, MF_STRING, IDM_RES_ADAPTIVE, L"Adaptive (fit window)");
    AppendMenuW(sub, MF_SEPARATOR, 0, NULL);
    for (i = 0; i < (int)ARRAYSIZE(g_res_presets); i++) {
        wchar_t label[64];
        swprintf_s(label, 64, L"%u x %u", g_res_presets[i].w, g_res_presets[i].h);
        AppendMenuW(sub, MF_STRING, IDM_RES_BASE + i * 0x10, label);
    }
    AppendMenuW(sub, MF_SEPARATOR, 0, NULL);
    for (i = 0; i < (int)ARRAYSIZE(g_refresh_rates); i++) {
        wchar_t label[32];
        swprintf_s(label, 32, L"%u Hz", g_refresh_rates[i]);
        AppendMenuW(sub, MF_STRING, IDM_HZ_BASE + i * 0x10, label);
    }
    /* Linux guests do not encode video yet. */
    if (!d->guest_linux) {
        AppendMenuW(sub, MF_SEPARATOR, 0, NULL);
        AppendMenuW(sub, MF_STRING, IDM_STREAM_QUALITY, L"Quality mode (exact pixels)");
        AppendMenuW(sub, MF_STRING, IDM_STREAM_PERF, L"Performance mode (GPU video, games)");
        AppendMenuW(sub, MF_SEPARATOR, 0, NULL);
        AppendMenuW(sub, MF_STRING, IDM_CODEC_AUTO, L"Codec: Auto");
        AppendMenuW(sub, MF_STRING, IDM_CODEC_H264, L"Codec: H.264");
        AppendMenuW(sub, MF_STRING, IDM_CODEC_HEVC, L"Codec: HEVC (H.265)");
    }
    AppendMenuW(sysmenu, MF_POPUP, (UINT_PTR)sub, L"Display");
}

/* Window title: name, guest resolution and the frame rate received from the
   guest. Refreshed at most twice a second. */
static void idd_update_title(VmDisplayIdd *d, BOOL force)
{
    ULONGLONG now = GetTickCount64();
    wchar_t title[320];
    UINT recv;

    if (!d || !d->hwnd) return;
    if (!force && d->fps_tick && now - d->fps_tick < 500) return;

    recv = d->recv_count;
    if (d->fps_tick && now > d->fps_tick)
        d->fps_value = (UINT)(((ULONGLONG)(recv - d->fps_last_recv) * 1000 + (now - d->fps_tick) / 2)
                              / (now - d->fps_tick));
    d->fps_last_recv = recv;
    d->fps_tick = now;

    if (!asb_get_viewer_title_stats()) {
        /* Settings > VM windows: name only. */
        swprintf_s(title, ARRAYSIZE(title), L"%s%s", d->audio_muted ? L"\U0001F507 " : L"", d->vm_name);
    } else
    swprintf_s(title, ARRAYSIZE(title), L"%s%s - %ux%u - %u FPS%s%s",
               d->audio_muted ? L"\U0001F507 " : L"",
               d->vm_name, d->frame_width, d->frame_height,
               d->frame_connected ? d->fps_value : 0,
               (d->frame_connected && d->video_active)
                   ? (d->video_codec == VM_VIDEO_CODEC_HEVC
                          ? (d->dx12 ? L" - GPU HEVC - D3D12" : L" - GPU HEVC")
                          : (d->dx12 ? L" - GPU H.264 - D3D12" : L" - GPU H.264"))
                   : (d->dx12 ? L" - D3D12" : L""),
               (d->mode_supported && d->frame_connected && d->guest_driver == 0)
                   ? L" - old display driver (1920x1080 only)" : L"");
    SetWindowTextW(d->hwnd, title);
    tb_refresh(d);
}

/* ==================================================================
 * Floating toolbar
 *
 * An owned, non-activating popup centred over the top of the render area.
 * It appears when the pointer touches the top edge (and for a few seconds
 * when the window opens), hides again once the pointer leaves it unless
 * pinned, and stays away while the mouse is captured for games (relative
 * mode). Buttons post the same WM_SYSCOMMAND items as the title-bar menu,
 * so both stay in sync.
 * ================================================================== */

enum {
    TB_NAME, TB_DISPLAY, TB_FPS, TB_SEP, TB_CLIP, TB_AUDIO, TB_KEYS,
    TB_FULL, TB_PIN, TB_MORE, TB_COUNT
};

#define TB_TRIGGER_PX   6      /* pointer this close to the top edge shows it */
#define TB_MARGIN_PX    10     /* gap between the window top and the toolbar */
#define TB_ACCENT       RGB(0x4E, 0xA6, 0xF5)

static int tb_scale(VmDisplayIdd *d, int px)
{
    UINT dpi = d->hwnd ? GetDpiForWindow(d->hwnd) : 96;
    return MulDiv(px, dpi ? (int)dpi : 96, 96);
}

static HFONT tb_font(VmDisplayIdd *d, BOOL icons, int pt)
{
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = -tb_scale(d, pt);
    lf.lfWeight = icons ? FW_NORMAL : FW_SEMIBOLD;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, LF_FACESIZE, icons ? L"Segoe Fluent Icons" : L"Segoe UI");
    {
        HFONT f = CreateFontIndirectW(&lf);
        if (icons && f) {
            /* Windows 10 has Segoe MDL2 Assets with the same code points. */
            HDC dc = GetDC(NULL);
            wchar_t face[LF_FACESIZE] = L"";
            HGDIOBJ old = SelectObject(dc, f);
            GetTextFaceW(dc, LF_FACESIZE, face);
            SelectObject(dc, old);
            ReleaseDC(NULL, dc);
            if (_wcsicmp(face, L"Segoe Fluent Icons") != 0) {
                DeleteObject(f);
                wcscpy_s(lf.lfFaceName, LF_FACESIZE, L"Segoe MDL2 Assets");
                f = CreateFontIndirectW(&lf);
            }
        }
        return f;
    }
}

/* Text or glyph for one item, and whether it is a "toggled on" state. */
static void tb_item(VmDisplayIdd *d, int item, wchar_t *text, size_t cap, BOOL *icon, BOOL *on)
{
    *icon = TRUE;
    *on = FALSE;
    text[0] = 0;
    switch (item) {
    case TB_NAME:    *icon = FALSE; wcscpy_s(text, cap, d->vm_name); break;
    case TB_DISPLAY:
        *icon = FALSE;
        if (d->res_choice == RES_ADAPTIVE)
            swprintf_s(text, cap, L"Adaptive %ux%u \u00B7 %u Hz \u25BE", d->frame_width, d->frame_height, d->refresh_hz);
        else
            swprintf_s(text, cap, L"%ux%u \u00B7 %u Hz \u25BE", d->frame_width, d->frame_height, d->refresh_hz);
        break;
    case TB_FPS:     *icon = FALSE; swprintf_s(text, cap, L"%u FPS%s", d->frame_connected ? d->fps_value : 0,
                              (d->frame_connected && d->video_active)
                                  ? (d->video_codec == VM_VIDEO_CODEC_HEVC ? L" \u00B7 HEVC" : L" \u00B7 H.264") : L""); break;
    case TB_CLIP:    wcscpy_s(text, cap, L"\xE77F"); *on = d->clip_share_host_to_guest && d->clip_share_guest_to_host; break;
    case TB_AUDIO:   wcscpy_s(text, cap, d->audio_muted ? L"\xE74F" : L"\xE767"); *on = !d->audio_muted; break;
    case TB_KEYS:    wcscpy_s(text, cap, L"\xE765"); *on = d->transmit_hotkeys; break;
    case TB_FULL:    wcscpy_s(text, cap, d->borderless_fullscreen ? L"\xE73F" : L"\xE740"); *on = d->borderless_fullscreen; break;
    case TB_PIN:     wcscpy_s(text, cap, d->toolbar_pinned ? L"\xE77A" : L"\xE718"); *on = d->toolbar_pinned; break;
    case TB_MORE:    wcscpy_s(text, cap, L"\xE712"); break;
    default: break;
    }
}

static const wchar_t *tb_tooltip(VmDisplayIdd *d, int item)
{
    switch (item) {
    case TB_DISPLAY: return L"Resolution and refresh rate";
    case TB_CLIP:    return d->clip_share_host_to_guest && d->clip_share_guest_to_host
                         ? L"Clipboard shared (click to stop)" : L"Clipboard not shared (click to share)";
    case TB_AUDIO:   return d->audio_muted ? L"Unmute audio" : L"Mute audio";
    case TB_KEYS:    return L"Send Windows shortcuts (Win, Alt+Tab) to the VM";
    case TB_FULL:    return L"Borderless fullscreen (F11)";
    case TB_PIN:     return d->toolbar_pinned ? L"Let the toolbar hide" : L"Keep the toolbar visible";
    case TB_MORE:    return L"More options";
    default:         return NULL;
    }
}

/* Measure items and size the window; positions are kept for hit tests. */
static SIZE tb_layout(VmDisplayIdd *d, HDC dc)
{
    HFONT text_font = tb_font(d, FALSE, 13), icon_font = tb_font(d, TRUE, 15);
    int pad = tb_scale(d, 6), btn = tb_scale(d, 36), h = tb_scale(d, 44);
    int x = pad, item;
    SIZE out;
    for (item = 0; item < TB_COUNT; item++) {
        wchar_t text[96];
        BOOL icon, on;
        int w;
        tb_item(d, item, text, ARRAYSIZE(text), &icon, &on);
        if (item == TB_SEP) {
            w = tb_scale(d, 9);
        } else if (icon) {
            w = btn;
        } else {
            SIZE ts = { 0, 0 };
            SelectObject(dc, text_font);
            GetTextExtentPoint32W(dc, text, (int)wcslen(text), &ts);
            w = ts.cx + tb_scale(d, item == TB_NAME ? 22 : 20);
            if (item == TB_FPS && w < tb_scale(d, 70)) w = tb_scale(d, 70);
        }
        SetRect(&d->toolbar_items[item], x, pad, x + w, h - pad);
        x += w + (item == TB_SEP ? 0 : tb_scale(d, 2));
    }
    d->toolbar_count = TB_COUNT;
    DeleteObject(text_font);
    DeleteObject(icon_font);
    out.cx = x + pad;
    out.cy = h;
    return out;
}

static void tb_position(VmDisplayIdd *d)
{
    RECT rc;
    POINT top;
    HDC dc;
    SIZE sz;
    if (!d->toolbar_hwnd || !d->render_hwnd) return;
    dc = GetDC(d->toolbar_hwnd);
    sz = tb_layout(d, dc);
    ReleaseDC(d->toolbar_hwnd, dc);
    GetClientRect(d->render_hwnd, &rc);
    top.x = rc.left + (rc.right - rc.left - sz.cx) / 2;
    top.y = rc.top + tb_scale(d, TB_MARGIN_PX);
    ClientToScreen(d->render_hwnd, &top);
    SetWindowPos(d->toolbar_hwnd, HWND_TOP, top.x, top.y, sz.cx, sz.cy,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

static void tb_hide(VmDisplayIdd *d)
{
    if (!d->toolbar_hwnd || !IsWindowVisible(d->toolbar_hwnd)) return;
    ShowWindow(d->toolbar_hwnd, SW_HIDE);
    KillTimer(d->hwnd, IDT_TOOLBAR);
    d->toolbar_hot = -1;
}

static void tb_show(VmDisplayIdd *d, UINT linger_ms)
{
    if (!d->toolbar_hwnd || d->relative_mouse || IsIconic(d->hwnd)) return;
    if (!asb_get_viewer_toolbar()) { tb_hide(d); return; }   /* Settings > VM windows */
    tb_position(d);
    if (!IsWindowVisible(d->toolbar_hwnd))
        ShowWindow(d->toolbar_hwnd, SW_SHOWNOACTIVATE);
    InvalidateRect(d->toolbar_hwnd, NULL, FALSE);
    SetTimer(d->hwnd, IDT_TOOLBAR, linger_ms, NULL);
}

/* Repaint with fresh values (FPS, toggles) if it is showing. */
static void tb_refresh(VmDisplayIdd *d)
{
    if (d && d->toolbar_hwnd && IsWindowVisible(d->toolbar_hwnd)) {
        tb_position(d);
        InvalidateRect(d->toolbar_hwnd, NULL, FALSE);
    }
}

/* Timer: hide once the pointer is neither on the toolbar nor near the top. */
static void tb_on_timer(VmDisplayIdd *d)
{
    POINT pt;
    RECT tr, band;
    if (!d->toolbar_hwnd || !IsWindowVisible(d->toolbar_hwnd)) { KillTimer(d->hwnd, IDT_TOOLBAR); return; }
    SetTimer(d->hwnd, IDT_TOOLBAR, 400, NULL);
    if (d->relative_mouse) { tb_hide(d); return; }
    if (d->toolbar_pinned) return;
    GetCursorPos(&pt);
    GetWindowRect(d->toolbar_hwnd, &tr);
    band = tr;
    InflateRect(&band, tb_scale(d, 24), tb_scale(d, 24));
    if (PtInRect(&band, pt)) return;
    if (GetCapture() == d->toolbar_hwnd) return;
    tb_hide(d);
}

static void tb_paint(VmDisplayIdd *d, HWND hwnd)
{
    PAINTSTRUCT ps;
    RECT rc;
    HDC dc = BeginPaint(hwnd, &ps), mem;
    HBITMAP bmp;
    HFONT text_font, icon_font, small_font;
    int item;

    GetClientRect(hwnd, &rc);
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    SelectObject(mem, bmp);
    tb_layout(d, mem);
    text_font = tb_font(d, FALSE, 13);
    small_font = tb_font(d, FALSE, 12);
    icon_font = tb_font(d, TRUE, 15);

    {
        HBRUSH bg = CreateSolidBrush(RGB(0x14, 0x17, 0x1D));
        HPEN border = CreatePen(PS_SOLID, 1, RGB(0x2E, 0x34, 0x41));
        HGDIOBJ ob = SelectObject(mem, bg), op = SelectObject(mem, border);
        FillRect(mem, &rc, bg);
        RoundRect(mem, 0, 0, rc.right, rc.bottom, tb_scale(d, 14), tb_scale(d, 14));
        SelectObject(mem, ob);
        SelectObject(mem, op);
        DeleteObject(bg);
        DeleteObject(border);
    }
    SetBkMode(mem, TRANSPARENT);

    for (item = 0; item < TB_COUNT; item++) {
        RECT r = d->toolbar_items[item];
        wchar_t text[96];
        BOOL icon, on;
        COLORREF fg = RGB(0xE8, 0xEA, 0xF0);
        tb_item(d, item, text, ARRAYSIZE(text), &icon, &on);

        if (item == TB_SEP) {
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(0x2E, 0x34, 0x41));
            HGDIOBJ op = SelectObject(mem, pen);
            int cx = (r.left + r.right) / 2;
            MoveToEx(mem, cx, r.top + tb_scale(d, 6), NULL);
            LineTo(mem, cx, r.bottom - tb_scale(d, 6));
            SelectObject(mem, op);
            DeleteObject(pen);
            continue;
        }
        if (item != TB_NAME && item != TB_FPS) {
            COLORREF fill = (item == d->toolbar_hot) ? RGB(0x2A, 0x30, 0x3D)
                          : (on && icon) ? RGB(0x1C, 0x2A, 0x3A)
                          : (item == TB_DISPLAY) ? RGB(0x1F, 0x24, 0x30) : RGB(0x14, 0x17, 0x1D);
            HBRUSH b = CreateSolidBrush(fill);
            HPEN np = (HPEN)GetStockObject(NULL_PEN);
            HGDIOBJ ob = SelectObject(mem, b), op = SelectObject(mem, np);
            RoundRect(mem, r.left, r.top, r.right + 1, r.bottom + 1, tb_scale(d, 10), tb_scale(d, 10));
            SelectObject(mem, ob);
            SelectObject(mem, op);
            DeleteObject(b);
        }
        if (on && icon) fg = TB_ACCENT;
        if (item == TB_FPS) fg = d->frame_connected ? RGB(0x5F, 0xD9, 0x93) : RGB(0xA7, 0xB0, 0xC0);
        if (item == TB_NAME) fg = RGB(0xE8, 0xEA, 0xF0);
        SetTextColor(mem, fg);
        SelectObject(mem, icon ? icon_font : (item == TB_FPS ? small_font : text_font));
        DrawTextW(mem, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    DeleteObject(text_font);
    DeleteObject(small_font);
    DeleteObject(icon_font);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static int tb_hit(VmDisplayIdd *d, int x, int y)
{
    POINT pt = { x, y };
    int item;
    for (item = 0; item < d->toolbar_count; item++)
        if (item != TB_NAME && item != TB_FPS && item != TB_SEP &&
            PtInRect(&d->toolbar_items[item], pt))
            return item;
    return -1;
}

/* Show a menu under an item and run the chosen command like the title-bar
   menu would. */
static void tb_menu(VmDisplayIdd *d, HMENU menu, int item)
{
    RECT r = d->toolbar_items[item];
    POINT pt = { r.left, r.bottom + tb_scale(d, 4) };
    UINT cmd;
    if (!menu) return;
    ClientToScreen(d->toolbar_hwnd, &pt);
    SetForegroundWindow(d->hwnd);
    cmd = (UINT)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
                               pt.x, pt.y, 0, d->hwnd, NULL);
    if (cmd) PostMessageW(d->hwnd, WM_SYSCOMMAND, cmd, 0);
}

static HMENU tb_display_submenu(VmDisplayIdd *d)
{
    HMENU sysmenu = GetSystemMenu(d->hwnd, FALSE);
    int i, count = sysmenu ? GetMenuItemCount(sysmenu) : 0;
    for (i = 0; i < count; i++) {
        HMENU h = GetSubMenu(sysmenu, i);
        if (h && GetMenuState(h, IDM_RES_ADAPTIVE, MF_BYCOMMAND) != (UINT)-1) return h;
    }
    return NULL;
}

static void tb_click(VmDisplayIdd *d, int item)
{
    switch (item) {
    case TB_DISPLAY:
        tb_menu(d, tb_display_submenu(d), item);
        break;
    case TB_CLIP: {
        /* One button for both directions: on if either is off. */
        BOOL target = !(d->clip_share_host_to_guest && d->clip_share_guest_to_host);
        if (d->clip_share_host_to_guest != target) PostMessageW(d->hwnd, WM_SYSCOMMAND, IDM_CLIP_H2G, 0);
        if (d->clip_share_guest_to_host != target) PostMessageW(d->hwnd, WM_SYSCOMMAND, IDM_CLIP_G2H, 0);
        break;
    }
    case TB_AUDIO: PostMessageW(d->hwnd, WM_SYSCOMMAND, IDM_AUDIO_MUTE, 0); break;
    case TB_KEYS:  PostMessageW(d->hwnd, WM_SYSCOMMAND, IDM_XMIT_HOTKEYS, 0); break;
    case TB_FULL:  PostMessageW(d->hwnd, WM_SYSCOMMAND, IDM_BORDERLESS_FULLSCREEN, 0); break;
    case TB_PIN:   d->toolbar_pinned = !d->toolbar_pinned; break;
    case TB_MORE:  tb_menu(d, GetSystemMenu(d->hwnd, FALSE), item); break;
    default: break;
    }
    /* Toggles land via posted messages; repaint after they are handled. */
    PostMessageW(d->toolbar_hwnd, WM_APP + 1, 0, 0);
}

static LRESULT CALLBACK idd_toolbar_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    VmDisplayIdd *d = (VmDisplayIdd *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        break;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (d) { tb_paint(d, hwnd); return 0; }
        break;
    case WM_MOUSEMOVE:
        if (d) {
            int hot = tb_hit(d, (int)(short)LOWORD(lp), (int)(short)HIWORD(lp));
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            if (hot != d->toolbar_hot) {
                const wchar_t *tip = hot >= 0 ? tb_tooltip(d, hot) : NULL;
                d->toolbar_hot = hot;
                InvalidateRect(hwnd, NULL, FALSE);
                /* Tooltip-like hint in the log line of the title is enough;
                   keep the toolbar itself compact. */
                (void)tip;
            }
            SetCursor(LoadCursorW(NULL, IDC_ARROW));
        }
        return 0;
    case WM_MOUSELEAVE:
        if (d) { d->toolbar_hot = -1; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) ReleaseCapture();
        if (d) {
            int item = tb_hit(d, (int)(short)LOWORD(lp), (int)(short)HIWORD(lp));
            if (item >= 0) tb_click(d, item);
        }
        return 0;
    case WM_APP + 1:
        if (d) tb_refresh(d);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ==================================================================
 * Keyboard hotkey capture
 * ================================================================== */

/* Host shortcuts withheld from the guest in Default mode.
   alt_down reflects GetKeyState(VK_MENU) in the window procedure. */
static BOOL idd_is_reserved_hotkey(DWORD vk, BOOL alt_down)
{
    switch (vk) {
    case VK_LWIN:
    case VK_RWIN:
    case VK_SNAPSHOT:   /* PrintScreen */
    case VK_APPS:       /* Menu / context key */
        return TRUE;
    case VK_TAB:
        return alt_down;                                  /* Alt+Tab */
    case VK_ESCAPE:
        /* Alt+Esc / Ctrl+Esc. GetAsyncKeyState reads real-time physical state,
           which is reliable both in the wndproc and inside the low-level hook
           (where the synchronized GetKeyState value may not be updated yet). */
        return alt_down || (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    default:
        return FALSE;
    }
}

/* Forward a key event to the guest and track held state so a later focus
   change can release anything still down. Runs on the window thread only. */
static void idd_forward_key(VmDisplayIdd *d, DWORD vk, DWORD scan, BOOL ext, BOOL up)
{
    UINT32 type = INPUT_KEY, index = vk;
    InputPacket pkt;
    AcquireSRWLockExclusive(&d->input_lock);
    if (d->keyboard_version == INPUT_KEYBOARD_VERSION) {
        type = INPUT_KEY_PHYSICAL;
        if (vk == VK_PAUSE || vk == VK_CANCEL) { scan = 0; ext = FALSE; }
        /* Windows flags right Shift and Num Lock as extended without a physical E0 prefix. */
        if (scan == 0x36 || scan == 0x45) ext = FALSE;
        if (vk == VK_PACKET || scan > 255) goto done;
        if (scan == 0 && vk != VK_CANCEL && vk != VK_PAUSE &&
            vk != VK_SNAPSHOT && vk != VK_SLEEP &&
            !(vk >= VK_BROWSER_BACK && vk <= VK_LAUNCH_APP2)) goto done;
        index = scan ? scan + (ext ? 256 : 0) : 512 + vk;
    }
    pkt.magic = INPUT_MAGIC;
    pkt.type = type;
    pkt.param1 = vk;
    pkt.param2 = scan;
    pkt.param3 = ext ? INPUT_KEY_EXTENDED : 0;
    if (type == INPUT_KEY_PHYSICAL &&
        index < ARRAYSIZE(d->held_down) && d->held_down[index])
        pkt = d->held_keys[index];
    if (type == INPUT_KEY_PHYSICAL && up && (scan == 0xF1 || scan == 0xF2) &&
        !d->held_down[index]) {
        /* Some Korean keyboard drivers report only the key release. */
        if (!send_input_locked(d, pkt.type, pkt.param1, pkt.param2, pkt.param3)) goto done;
        d->held_down[index] = 1;
        d->held_keys[index] = pkt;
    }
    if (up) pkt.param3 |= INPUT_KEY_UP;
    if (send_input_locked(d, pkt.type, pkt.param1, pkt.param2, pkt.param3) &&
        index < ARRAYSIZE(d->held_down)) {
        d->held_down[index] = !up;
        if (!up) d->held_keys[index] = pkt;
    }
done:
    ReleaseSRWLockExclusive(&d->input_lock);
}

/* Send key-up for every key we believe is still held in the guest, then
   clear tracking. Called when our window loses activation, when Transmit
   mode is turned off, and on teardown. */
static void idd_flush_held_keys(VmDisplayIdd *d)
{
    AcquireSRWLockExclusive(&d->input_lock);
    for (int i = 0; i < ARRAYSIZE(d->held_down); i++) {
        if (d->held_down[i]) {
            const InputPacket *pkt = &d->held_keys[i];
            send_input_locked(d, pkt->type, pkt->param1, pkt->param2,
                              pkt->param3 | INPUT_KEY_UP);
            d->held_down[i] = 0;
        }
    }
    ReleaseSRWLockExclusive(&d->input_lock);
}

/* Thread-local owner: a WH_KEYBOARD_LL callback runs on the thread that
   installed it (our window thread), so this resolves each display's context
   without a global registry, keeping multiple displays independent. */
static __declspec(thread) VmDisplayIdd *t_hook_display;

enum { KEY_ROUTE_HOST = 1, KEY_ROUTE_GUEST, KEY_ROUTE_BOTH };

static BOOL idd_is_modifier(DWORD vk)
{
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
           (vk >= VK_LSHIFT && vk <= VK_RMENU);
}

static LRESULT CALLBACK idd_ll_keyboard_proc(int code, WPARAM wp, LPARAM lp)
{
    VmDisplayIdd *d = t_hook_display;
    if (code == HC_ACTION && d && !d->stop) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
        BOOL up = (wp == WM_KEYUP || wp == WM_SYSKEYUP);
        BOOL ext = (k->flags & LLKHF_EXTENDED) != 0;
        BOOL focused = d->input_focused && GetForegroundWindow() == d->hwnd;
        if (d->keyboard_version != INPUT_KEYBOARD_VERSION) {
            if (focused && d->transmit_hotkeys) {
                idd_forward_key(d, k->vkCode, k->scanCode, ext, up);
                return 1;
            }
        } else {
            DWORD scan = (k->vkCode == VK_PAUSE || k->vkCode == VK_CANCEL)
                         ? 0 : k->scanCode;
            if (scan == 0x36 || scan == 0x45) ext = FALSE;
            if (scan > 255 || k->vkCode > 255) return CallNextHookEx(NULL, code, wp, lp);
            UINT index = scan ? scan + (ext ? 256 : 0) : 512 + k->vkCode;
            BYTE route = d->hook_routes[index];
            BOOL pulse = k->vkCode == VK_PAUSE || k->vkCode == VK_CANCEL;
            if (!focused || d->input_menu_active) {
                d->hook_routes[index] = up || pulse ? 0 : KEY_ROUTE_HOST;
                return CallNextHookEx(NULL, code, wp, lp);
            }
            if (!route) {
                if (up && scan != 0xF1 && scan != 0xF2)
                    return CallNextHookEx(NULL, code, wp, lp);
                if (d->transmit_hotkeys) {
                    route = KEY_ROUTE_GUEST;
                } else if (idd_is_reserved_hotkey(k->vkCode, (k->flags & LLKHF_ALTDOWN) != 0) ||
                           (GetAsyncKeyState(VK_LWIN) & 0x8000) ||
                           (GetAsyncKeyState(VK_RWIN) & 0x8000)) {
                    route = KEY_ROUTE_HOST;
                } else {
                    route = idd_is_modifier(k->vkCode) ? KEY_ROUTE_BOTH : KEY_ROUTE_GUEST;
                }
            }
            /* Releases and repeats follow the key-down destination even if modifiers changed. */
            d->hook_routes[index] = up || pulse ? 0 : route;
            if (route != KEY_ROUTE_HOST)
                idd_forward_key(d, k->vkCode, k->scanCode, ext, up);
            if (route == KEY_ROUTE_GUEST) return 1;
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static void idd_install_kbd_hook(VmDisplayIdd *d)
{
    if (d->kbd_hook) return;
    ZeroMemory(d->hook_routes, sizeof(d->hook_routes));
    AcquireSRWLockShared(&d->input_lock);
    if (d->keyboard_version == INPUT_KEYBOARD_VERSION) {
        for (UINT i = 0; i < ARRAYSIZE(d->held_down); i++)
            if (d->held_down[i]) d->hook_routes[i] = KEY_ROUTE_BOTH;
    }
    ReleaseSRWLockShared(&d->input_lock);
    t_hook_display = d;
    d->kbd_hook = SetWindowsHookExW(WH_KEYBOARD_LL, idd_ll_keyboard_proc,
                                     d->hInstance, 0);
    if (!d->kbd_hook)
        idd_log(d, L"Hotkey hook install failed (err %lu).", GetLastError());
    else
        idd_log(d, L"Hotkey capture enabled.");
}

static void idd_remove_kbd_hook(VmDisplayIdd *d)
{
    if (d->kbd_hook) {
        UnhookWindowsHookEx(d->kbd_hook);
        d->kbd_hook = NULL;
        idd_log(d, L"Hotkey capture disabled.");
    }
    t_hook_display = NULL;
}

static void idd_update_kbd_hook(VmDisplayIdd *d)
{
    if (d->transmit_hotkeys || d->keyboard_version == INPUT_KEYBOARD_VERSION)
        idd_install_kbd_hook(d);
    else
        idd_remove_kbd_hook(d);
}

/* Compute letterboxed/pillarboxed viewport within client rect */
static void compute_letterbox(UINT client_w, UINT client_h,
                              UINT frame_w, UINT frame_h,
                              float *out_x, float *out_y,
                              float *out_w, float *out_h)
{
    float scale_x, scale_y, scale;
    if (client_w == 0 || client_h == 0 || frame_w == 0 || frame_h == 0) {
        *out_x = 0; *out_y = 0; *out_w = 0; *out_h = 0;
        return;
    }
    scale_x = (float)client_w / (float)frame_w;
    scale_y = (float)client_h / (float)frame_h;
    scale = scale_x < scale_y ? scale_x : scale_y;
    *out_w = (float)frame_w * scale;
    *out_h = (float)frame_h * scale;
    *out_x = ((float)client_w - *out_w) * 0.5f;
    *out_y = ((float)client_h - *out_h) * 0.5f;
}

/* Map window client coordinates to VM framebuffer coordinates */
static void window_to_vm_coords(HWND hwnd, int wx, int wy,
                                 UINT vm_w, UINT vm_h,
                                 UINT *vx, UINT *vy)
{
    RECT rc;
    float vp_x, vp_y, vp_w, vp_h;
    float local_x, local_y;

    GetClientRect(hwnd, &rc);
    compute_letterbox((UINT)rc.right, (UINT)rc.bottom, vm_w, vm_h,
                      &vp_x, &vp_y, &vp_w, &vp_h);

    if (vp_w <= 0 || vp_h <= 0) {
        *vx = 0;
        *vy = 0;
        return;
    }

    local_x = ((float)wx - vp_x) / vp_w * (float)vm_w;
    local_y = ((float)wy - vp_y) / vp_h * (float)vm_h;

    if (local_x < 0) local_x = 0;
    if (local_y < 0) local_y = 0;
    *vx = (UINT)local_x;
    *vy = (UINT)local_y;
    if (*vx >= vm_w) *vx = vm_w - 1;
    if (*vy >= vm_h) *vy = vm_h - 1;
}

static void idd_resume_absolute_mouse(VmDisplayIdd *d, const InputPacket *reply)
{
    POINT point;
    UINT vx, vy;
    if (d->stop || !d->frame_connected || !d->input_focused ||
        d->input_menu_active || d->input_sizing || d->relative_mouse ||
        !d->cursor_visible || !d->frame_width || !d->frame_height ||
        GetForegroundWindow() != d->hwnd || IsIconic(d->hwnd) ||
        !GetCursorPos(&point) || WindowFromPoint(point) != d->render_hwnd) {
        idd_cancel_mouse_sync(d);
        return;
    }

    if (reply && reply->param1 < d->frame_width && reply->param2 < d->frame_height) {
        RECT rc;
        float x, y, width, height;
        LONG dx = point.x - d->mouse_sync_origin.x;
        LONG dy = point.y - d->mouse_sync_origin.y;
        if (GetClientRect(d->render_hwnd, &rc) && !IsRectEmpty(&rc)) {
            compute_letterbox(rc.right, rc.bottom, d->frame_width, d->frame_height,
                              &x, &y, &width, &height);
            POINT target = {
                (LONG)(x + (reply->param1 + 0.5f) * width / d->frame_width) + dx,
                (LONG)(y + (reply->param2 + 0.5f) * height / d->frame_height) + dy
            };
            if (target.x < (LONG)x) target.x = (LONG)x;
            if (target.y < (LONG)y) target.y = (LONG)y;
            if (target.x >= (LONG)(x + width)) target.x = (LONG)(x + width) - 1;
            if (target.y >= (LONG)(y + height)) target.y = (LONG)(y + height) - 1;
            if (ClientToScreen(d->render_hwnd, &target)) {
                SetCursorPos(target.x, target.y);
                GetCursorPos(&point);
            }
        }
    }
    d->mouse_sync_pending = FALSE;
    ScreenToClient(d->render_hwnd, &point);
    window_to_vm_coords(d->render_hwnd, point.x, point.y,
                        d->frame_width, d->frame_height, &vx, &vy);
    send_input(d, INPUT_MOUSE_MOVE, vx, vy, 0);
    SetCursor(d->guest_cursor ? d->guest_cursor : LoadCursorW(NULL, IDC_ARROW));
}

static void idd_poll_mouse_position(VmDisplayIdd *d)
{
    InputPacket reply;
    BOOL received = FALSE, disconnected = FALSE;
    if (!d->mouse_sync_pending) return;
    idd_update_relative_mouse(d);
    if (!d->mouse_sync_pending) return;
    AcquireSRWLockExclusive(&d->input_lock);
    if (d->input_socket != INVALID_SOCKET && d->input_socket == d->mouse_sync_socket) {
        for (int i = 0; i < 16; i++) {
            int n = recv(d->input_socket, (char *)&d->mouse_reply + d->mouse_reply_size,
                         (int)sizeof(InputPacket) - d->mouse_reply_size, 0);
            if (n <= 0) {
                disconnected = n == 0 || WSAGetLastError() != WSAEWOULDBLOCK;
                if (disconnected) {
                    d->input_socket = INVALID_SOCKET;
                    PostMessageW(d->hwnd, WM_IDD_INPUT_READY, 0, 0);
                }
                break;
            }
            d->mouse_reply_size += n;
            if (d->mouse_reply_size != sizeof(InputPacket)) continue;
            d->mouse_reply_size = 0;
            if (d->mouse_reply.magic == INPUT_MAGIC &&
                d->mouse_reply.type == INPUT_MOUSE_POSITION_REPLY &&
                d->mouse_reply.param3 == d->mouse_sync_id) {
                reply = d->mouse_reply;
                received = TRUE;
                break;
            }
        }
    } else {
        disconnected = TRUE;
    }
    ReleaseSRWLockExclusive(&d->input_lock);
    if (disconnected)
        idd_cancel_mouse_sync(d);
    else if (received)
        idd_resume_absolute_mouse(d, &reply);
    else if (GetTickCount64() >= d->mouse_sync_deadline)
        idd_resume_absolute_mouse(d, NULL);
}

/* ---- Reliable recv: read exactly `len` bytes ---- */

static BOOL recv_exact(SOCKET s, void *buf, int len)
{
    char *p = (char *)buf;
    int remaining = len;
    while (remaining > 0) {
        int n = recv(s, p, remaining, 0);
        if (n <= 0) return FALSE;
        p += n;
        remaining -= n;
    }
    return TRUE;
}

/* ---- Non-blocking connect with timeout ---- */

static SOCKET connect_to_hv_service(const GUID *vm_runtime_id, const GUID *service_guid, int timeout_ms)
{
    SOCKET s;
    SOCKADDR_HV addr;
    u_long nonblock;
    fd_set wfds, efds;
    struct timeval tv;
    DWORD sock_timeout;
    static const GUID zero_guid = {0};

    if (memcmp(vm_runtime_id, &zero_guid, sizeof(GUID)) == 0)
        return INVALID_SOCKET;

    s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    /* Non-blocking connect */
    nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);

    memset(&addr, 0, sizeof(addr));
    addr.Family   = AF_HYPERV;
    addr.VmId     = *vm_runtime_id;
    addr.ServiceId = *service_guid;

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(s);
            return INVALID_SOCKET;
        }

        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(s, &wfds);
        FD_SET(s, &efds);
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        if (select(0, NULL, &wfds, &efds, &tv) <= 0 || FD_ISSET(s, &efds)) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }

    /* Back to blocking with recv timeout */
    nonblock = 0;
    ioctlsocket(s, FIONBIO, &nonblock);
    sock_timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&sock_timeout, sizeof(sock_timeout));

    return s;
}

static SOCKET connect_input(VmDisplayIdd *d)
{
    static volatile LONG request_id;
    GUID svc;
    UINT32 ready = 0;
    InputPacket queries[] = {
        { INPUT_MAGIC, INPUT_KEYBOARD_QUERY, INPUT_KEYBOARD_VERSION, 0, 0 },
        { INPUT_MAGIC, INPUT_MOUSE_QUERY, INPUT_MOUSE_VERSION, 0, 0 }
    };
    InputPacket reply = {0};
    int got = 0, version = 1, mouse_version = 0;
    BOOL keyboard_reply = FALSE, mouse_reply = FALSE;
    ULONGLONG deadline;
    u_long nonblock = 1;
    DWORD zero_timeout = 0;
    hcs_service_guid(d->os_type, 3, &svc);
    SOCKET s = connect_to_hv_service(&d->runtime_id, &svc, 1000);
    if (s == INVALID_SOCKET) return s;
    if (!recv_exact(s, &ready, sizeof(ready)) || ready != INPUT_READY_MAGIC)
        goto failed;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&zero_timeout, sizeof(zero_timeout));
    if (ioctlsocket(s, FIONBIO, &nonblock) != 0) goto failed;
    queries[0].param2 = queries[1].param2 = (UINT32)InterlockedIncrement(&request_id);
    if (send(s, (const char *)queries, sizeof(queries), 0) != sizeof(queries))
        goto failed;
    /* Old helpers ignore the query. A late or incomplete reply cannot change
       the mode after the socket has been made available to the window thread. */
    deadline = GetTickCount64() + 500;
    while ((!keyboard_reply || !mouse_reply) && !d->stop) {
        fd_set read_set;
        struct timeval timeout;
        LONGLONG remaining = (LONGLONG)(deadline - GetTickCount64());
        if (remaining <= 0) break;
        timeout.tv_sec = 0;
        timeout.tv_usec = (long)remaining * 1000;
        FD_ZERO(&read_set);
        FD_SET(s, &read_set);
        int result = select(0, &read_set, NULL, NULL, &timeout);
        if (result == SOCKET_ERROR) goto failed;
        if (result == 0) break;
        int n = recv(s, (char *)&reply + got, (int)sizeof(reply) - got, 0);
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
        if (n <= 0) goto failed;
        got += n;
        if (got == sizeof(reply)) {
            if (reply.magic == INPUT_MAGIC && reply.param2 == queries[0].param2 &&
                reply.param3 == 0) {
                if (reply.type == INPUT_KEYBOARD_REPLY) {
                    keyboard_reply = TRUE;
                    if (reply.param1 == INPUT_KEYBOARD_VERSION)
                        version = INPUT_KEYBOARD_VERSION;
                } else if (reply.type == INPUT_MOUSE_REPLY) {
                    mouse_reply = TRUE;
                    if (reply.param1 == INPUT_MOUSE_VERSION)
                        mouse_version = INPUT_MOUSE_VERSION;
                }
            }
            got = 0;
        }
    }
    if (d->stop) goto failed;
    AcquireSRWLockExclusive(&d->input_lock);
    ZeroMemory(d->held_down, sizeof(d->held_down));
    d->keyboard_version = version;
    d->mouse_version = mouse_version;
    d->mouse_reply = reply;
    d->mouse_reply_size = got;
    d->input_socket = s;
    ReleaseSRWLockExclusive(&d->input_lock);
    PostMessageW(d->hwnd, WM_IDD_INPUT_READY, 0, 0);
    idd_log(d, L"Input connected + ready (keyboard v%d).", version);
    return s;
failed:
    closesocket(s);
    return INVALID_SOCKET;
}

/* ==================================================================
 * Audio playback — guest -> host
 * ================================================================== */

static const CLSID AUDIO_CLSID_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const IID AUDIO_IID_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const IID AUDIO_IID_IAudioClient =
    { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const IID AUDIO_IID_IAudioRenderClient =
    { 0xF294ACFC, 0x3146, 0x4483, { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };

static DWORD WINAPI audio_recv_thread_proc(LPVOID param)
{
    VmDisplayIdd *d = (VmDisplayIdd *)param;
    HRESULT hr;
    BOOL com_ok = FALSE;
    IMMDeviceEnumerator *pEnum = NULL;
    IMMDevice *pDev = NULL;
    IAudioClient *pAC = NULL;
    IAudioRenderClient *pRC = NULL;
    WAVEFORMATEX *renderfmt = NULL;
    BYTE *scratch = NULL;
    UINT32 scratch_cap = 0;
    UINT32 buf_frames = 0;
    UINT32 render_block_align = 0;
    AudioHeader hdr;
    BOOL stream_started = FALSE;
    BOOL session_logged = FALSE;
    int  header_misses = 0;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE)
        com_ok = TRUE;

    /* Outer retry loop — keep trying to connect to the guest capture helper */
    while (!d->stop) {
        GUID svc; hcs_service_guid(d->os_type, 4, &svc);
        SOCKET s = connect_to_hv_service(&d->runtime_id, &svc, 1000);
        if (s == INVALID_SOCKET) {
            int wait;
            for (wait = 0; wait < 2000 && !d->stop; wait += 200)
                Sleep(200);
            continue;
        }

        /* Blocking recv for the session */
        {
            DWORD no_timeout = 0;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&no_timeout, sizeof(no_timeout));
        }
        d->audio_socket = s;

        /* Read one-shot AudioHeader.
           The guest closes without sending a header when the VAD endpoint
           isn't ready yet (e.g. before user login / audio stack init).
           Log only the first miss per run so we don't spam. */
        if (!recv_exact(s, &hdr, sizeof(hdr)) || hdr.magic != AUDIO_HEADER_MAGIC) {
            if (header_misses == 0)
                idd_log(d, L"Audio header bad/absent - guest not ready, retrying quietly.");
            header_misses++;
            goto session_cleanup;
        }

        idd_log(d, L"Audio connected (GUID :0004).");
        session_logged = TRUE;
        header_misses = 0;

        idd_log(d, L"Audio header: %lu Hz, %u ch, %u bits, tag=%u.",
                 hdr.sample_rate, hdr.channels, hdr.bits_per_sample, hdr.format_tag);

        /* Set up WASAPI render on the default endpoint */
        hr = CoCreateInstance(&AUDIO_CLSID_MMDeviceEnumerator, NULL,
                               CLSCTX_ALL, &AUDIO_IID_IMMDeviceEnumerator,
                               (void **)&pEnum);
        if (FAILED(hr)) { idd_log(d, L"Audio: CoCreateInstance failed 0x%08lX.", hr); goto session_cleanup; }

        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(pEnum, eRender, eConsole, &pDev);
        if (FAILED(hr)) { idd_log(d, L"Audio: GetDefaultAudioEndpoint failed 0x%08lX.", hr); goto session_cleanup; }

        hr = IMMDevice_Activate(pDev, &AUDIO_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&pAC);
        if (FAILED(hr)) { idd_log(d, L"Audio: Activate failed 0x%08lX.", hr); goto session_cleanup; }

        /* Build the source format exactly matching what the guest is sending.
           We deliberately do NOT call GetMixFormat: we never use the host's
           mix format (Initialize runs with AUTOCONVERTPCM, so WASAPI converts
           our renderfmt to whatever the endpoint needs). On some endpoints
           (seen on AMD HD Audio) GetMixFormat returns
           AUDCLNT_E_UNSUPPORTED_FORMAT for the configured default format,
           which would needlessly kill the whole audio path. */
        renderfmt = (WAVEFORMATEX *)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
        if (!renderfmt) goto session_cleanup;
        ZeroMemory(renderfmt, sizeof(WAVEFORMATEX));
        renderfmt->wFormatTag      = hdr.format_tag;
        renderfmt->nChannels       = hdr.channels;
        renderfmt->nSamplesPerSec  = hdr.sample_rate;
        renderfmt->wBitsPerSample  = hdr.bits_per_sample;
        renderfmt->nBlockAlign     = hdr.block_align ? hdr.block_align
                                     : (WORD)((hdr.channels * hdr.bits_per_sample) / 8);
        renderfmt->nAvgBytesPerSec = renderfmt->nSamplesPerSec * renderfmt->nBlockAlign;
        renderfmt->cbSize          = 0;
        render_block_align         = renderfmt->nBlockAlign;

        /* 40ms buffer. HV socket is effectively memcpy and the guest polls
           every 5ms, so scheduler jitter is the only thing we need to absorb. */
        hr = IAudioClient_Initialize(pAC, AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                      400000, 0, renderfmt, NULL);
        if (FAILED(hr)) {
            idd_log(d, L"Audio: IAudioClient::Initialize failed 0x%08lX.", hr);
            goto session_cleanup;
        }

        hr = IAudioClient_GetBufferSize(pAC, &buf_frames);
        if (FAILED(hr)) goto session_cleanup;

        hr = IAudioClient_GetService(pAC, &AUDIO_IID_IAudioRenderClient, (void **)&pRC);
        if (FAILED(hr)) goto session_cleanup;

        hr = IAudioClient_Start(pAC);
        if (FAILED(hr)) goto session_cleanup;
        stream_started = TRUE;

        idd_log(d, L"Audio render started: buf=%lu frames.", buf_frames);

        /* Receive loop */
        while (!d->stop) {
            AudioFrameHeader fh;
            UINT32 frames_in_payload;
            UINT32 padding;
            UINT32 free_frames;
            UINT32 frames_to_write;
            BYTE *dst;

            if (!recv_exact(s, &fh, sizeof(fh))) break;
            if (fh.bytes == 0 || fh.bytes > 4 * 1024 * 1024) break;

            if (fh.bytes > scratch_cap) {
                BYTE *nb = scratch
                    ? (BYTE *)HeapReAlloc(GetProcessHeap(), 0, scratch, fh.bytes)
                    : (BYTE *)HeapAlloc(GetProcessHeap(), 0, fh.bytes);
                if (!nb) break;
                scratch = nb;
                scratch_cap = fh.bytes;
            }
            if (!recv_exact(s, scratch, (int)fh.bytes)) break;

            /* Host-side mute: keep draining the socket but don't push to WASAPI */
            if (d->audio_muted) continue;

            frames_in_payload = fh.bytes / render_block_align;
            if (frames_in_payload == 0) continue;

            /* Push into WASAPI buffer; drop if no room (low-latency, no stalling) */
            hr = IAudioClient_GetCurrentPadding(pAC, &padding);
            if (FAILED(hr)) break;
            free_frames = buf_frames - padding;
            frames_to_write = frames_in_payload;
            if (frames_to_write > free_frames)
                frames_to_write = free_frames;
            if (frames_to_write == 0)
                continue;

            hr = IAudioRenderClient_GetBuffer(pRC, frames_to_write, &dst);
            if (FAILED(hr)) break;
            memcpy(dst, scratch, (size_t)frames_to_write * render_block_align);
            IAudioRenderClient_ReleaseBuffer(pRC, frames_to_write, 0);
        }

session_cleanup:
        if (stream_started && pAC) {
            IAudioClient_Stop(pAC);
            stream_started = FALSE;
        }
        if (pRC)    { IAudioRenderClient_Release(pRC);    pRC = NULL; }
        if (pAC)    { IAudioClient_Release(pAC);          pAC = NULL; }
        if (pDev)   { IMMDevice_Release(pDev);            pDev = NULL; }
        if (pEnum)  { IMMDeviceEnumerator_Release(pEnum); pEnum = NULL; }
        if (renderfmt) { CoTaskMemFree(renderfmt); renderfmt = NULL; }

        if (d->audio_socket != INVALID_SOCKET) {
            closesocket(d->audio_socket);
            d->audio_socket = INVALID_SOCKET;
        }
        if (session_logged) {
            idd_log(d, L"Audio session ended.");
            session_logged = FALSE;
        }

        if (d->stop) break;
        /* Back off when the guest keeps rejecting us (VAD not ready) */
        Sleep(header_misses > 3 ? 5000 : 500);
    }

    if (scratch) HeapFree(GetProcessHeap(), 0, scratch);
    if (com_ok)  CoUninitialize();
    d->audio_recv_thread = NULL;
    return 0;
}


/* ==================================================================
 * D3D11 initialization and teardown
 * ================================================================== */

static BOOL d3d_compile_shader(const char *hlsl, const char *entry,
                               const char *target, ID3DBlob **out)
{
    ID3DBlob *errors = NULL;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), NULL, NULL, NULL,
                            entry, target, 0, 0, out, &errors);
    if (FAILED(hr)) {
        if (errors) {
            ui_log(L"Shader compile error: %S",
                   (const char *)errors->lpVtbl->GetBufferPointer(errors));
            errors->lpVtbl->Release(errors);
        }
        return FALSE;
    }
    if (errors) errors->lpVtbl->Release(errors);
    return TRUE;
}

static void idd_video_log(void *user, const wchar_t *msg);

static BOOL d3d_init(VmDisplayIdd *d)
{
    DXGI_SWAP_CHAIN_DESC scd;
    D3D_FEATURE_LEVEL feature_level;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
    D3D11_SAMPLER_DESC sd;
    ID3DBlob *vs_blob = NULL;
    ID3DBlob *ps_blob = NULL;
    HRESULT hr;

    /* Create device and swap chain */
    ZeroMemory(&scd, sizeof(scd));
    scd.BufferCount                        = 1;
    scd.BufferDesc.Width                   = DEFAULT_WIDTH;
    scd.BufferDesc.Height                  = DEFAULT_HEIGHT;
    scd.BufferDesc.Format                  = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator   = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow                       = d->render_hwnd;
    scd.SampleDesc.Count                   = 1;
    scd.Windowed                           = TRUE;
    scd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    /* Renderer D3D12 (Settings > VM windows): a D3D11 device for decoding
       and uploads, D3D12 for drawing and presenting. Any failure falls back
       to the D3D11 swap chain below. */
    if (asb_get_viewer_renderer() == 12) {
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                               NULL, 0, D3D11_SDK_VERSION, &d->device, &feature_level, &d->ctx);
        if (SUCCEEDED(hr))
            d->dx12 = dx12_create(d->render_hwnd, d->device, d->ctx, g_vs_hlsl, g_ps_hlsl,
                                  g_ps_blur_hlsl, idd_video_log, d);
        if (!d->dx12) {
            idd_log_main(d, L"Direct3D 12 renderer unavailable; using Direct3D 11.");
            if (d->ctx)    { d->ctx->lpVtbl->Release(d->ctx);       d->ctx = NULL; }
            if (d->device) { d->device->lpVtbl->Release(d->device); d->device = NULL; }
        }
    }

    if (!d->dx12) {
    /* Flip model with two buffers: the compositor shows our latest frame
       without an extra copy, and with Present(0) a newer frame replaces a
       queued one instead of waiting behind it (smoother, lower latency).
       Video support lets the performance mode decode on this device. */
    scd.BufferCount = 2;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = D3D11CreateDeviceAndSwapChain(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        NULL, 0, D3D11_SDK_VERSION,
        &scd, &d->swap_chain, &d->device, &feature_level, &d->ctx);
    if (FAILED(hr)) {
        /* Older systems: the previous blit-model swap chain. */
        scd.BufferCount = 1;
        scd.SwapEffect  = DXGI_SWAP_EFFECT_DISCARD;
        hr = D3D11CreateDeviceAndSwapChain(
            NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            NULL, 0, D3D11_SDK_VERSION,
            &scd, &d->swap_chain, &d->device, &feature_level, &d->ctx);
    }
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(
            NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
            NULL, 0, D3D11_SDK_VERSION,
            &scd, &d->swap_chain, &d->device, &feature_level, &d->ctx);

    if (FAILED(hr)) {
        ui_log(L"IDD: D3D11CreateDeviceAndSwapChain failed (0x%08X)", hr);
        return FALSE;
    }
    {
        /* At most one frame queued ahead of the display. */
        IDXGIDevice1 *dxgi1 = NULL;
        if (SUCCEEDED(d->device->lpVtbl->QueryInterface(d->device, &IID_IDXGIDevice1, (void **)&dxgi1)) && dxgi1) {
            dxgi1->lpVtbl->SetMaximumFrameLatency(dxgi1, 1);
            dxgi1->lpVtbl->Release(dxgi1);
        }
    }

    /* Create render target view from back buffer */
    {
        ID3D11Texture2D *back_buf = NULL;
        hr = d->swap_chain->lpVtbl->GetBuffer(d->swap_chain, 0,
                                       &IID_ID3D11Texture2D, (void **)&back_buf);
        if (FAILED(hr)) {
            ui_log(L"IDD: GetBuffer failed (0x%08X)", hr);
            return FALSE;
        }
        hr = d->device->lpVtbl->CreateRenderTargetView(d->device,
                (ID3D11Resource *)back_buf, NULL, &d->rtv);
        back_buf->lpVtbl->Release(back_buf);
        if (FAILED(hr)) {
            ui_log(L"IDD: CreateRenderTargetView failed (0x%08X)", hr);
            return FALSE;
        }
    }
    } /* !d->dx12 */

    /* Create frame texture (dynamic, CPU-writable) */
    ZeroMemory(&td, sizeof(td));
    td.Width              = DEFAULT_WIDTH;
    td.Height             = DEFAULT_HEIGHT;
    d->tex_width          = DEFAULT_WIDTH;
    d->tex_height         = DEFAULT_HEIGHT;
    td.MipLevels          = 1;
    td.ArraySize          = 1;
    td.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count   = 1;
    td.Usage              = D3D11_USAGE_DYNAMIC;
    td.BindFlags          = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags     = D3D11_CPU_ACCESS_WRITE;

    hr = d->device->lpVtbl->CreateTexture2D(d->device, &td, NULL, &d->frame_tex);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreateTexture2D failed (0x%08X)", hr);
        return FALSE;
    }

    /* Shader resource view for the frame texture */
    ZeroMemory(&srv_desc, sizeof(srv_desc));
    srv_desc.Format                    = DXGI_FORMAT_B8G8R8A8_UNORM;
    srv_desc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels       = 1;
    srv_desc.Texture2D.MostDetailedMip = 0;

    hr = d->device->lpVtbl->CreateShaderResourceView(d->device,
            (ID3D11Resource *)d->frame_tex, &srv_desc, &d->frame_srv);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreateShaderResourceView failed (0x%08X)", hr);
        return FALSE;
    }

    /* Compile and create vertex shader */
    if (!d3d_compile_shader(g_vs_hlsl, "main", "vs_4_0", &vs_blob))
        return FALSE;
    hr = d->device->lpVtbl->CreateVertexShader(d->device,
            vs_blob->lpVtbl->GetBufferPointer(vs_blob),
            vs_blob->lpVtbl->GetBufferSize(vs_blob),
            NULL, &d->vs);
    vs_blob->lpVtbl->Release(vs_blob);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreateVertexShader failed (0x%08X)", hr);
        return FALSE;
    }

    /* Compile and create pixel shader */
    if (!d3d_compile_shader(g_ps_hlsl, "main", "ps_4_0", &ps_blob))
        return FALSE;
    hr = d->device->lpVtbl->CreatePixelShader(d->device,
            ps_blob->lpVtbl->GetBufferPointer(ps_blob),
            ps_blob->lpVtbl->GetBufferSize(ps_blob),
            NULL, &d->ps);
    ps_blob->lpVtbl->Release(ps_blob);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreatePixelShader failed (0x%08X)", hr);
        return FALSE;
    }

    /* Backdrop shader + constants (optional: without it the bars stay black). */
    {
        ID3DBlob *blob = NULL;
        D3D11_BUFFER_DESC bd;
        if (d3d_compile_shader(g_ps_blur_hlsl, "main", "ps_4_0", &blob)) {
            d->device->lpVtbl->CreatePixelShader(d->device,
                blob->lpVtbl->GetBufferPointer(blob), blob->lpVtbl->GetBufferSize(blob),
                NULL, &d->ps_blur);
            blob->lpVtbl->Release(blob);
        }
        ZeroMemory(&bd, sizeof(bd));
        bd.ByteWidth = 64;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        d->device->lpVtbl->CreateBuffer(d->device, &bd, NULL, &d->cb_blur);
    }

    /* Sampler state (linear filtering) */
    ZeroMemory(&sd, sizeof(sd));
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;

    hr = d->device->lpVtbl->CreateSamplerState(d->device, &sd, &d->sampler);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreateSamplerState failed (0x%08X)", hr);
        return FALSE;
    }

    return TRUE;
}

static void d3d_resize_swap_chain(VmDisplayIdd *d)
{
    RECT rc;
    HRESULT hr;
    ID3D11Texture2D *back_buf = NULL;

    if (d->dx12) {
        GetClientRect(d->render_hwnd, &rc);
        dx12_resize(d->dx12, (UINT)rc.right, (UINT)rc.bottom);
        return;
    }
    if (!d->swap_chain) return;

    /* Release old render target */
    if (d->rtv) {
        d->ctx->lpVtbl->OMSetRenderTargets(d->ctx, 0, NULL, NULL);
        d->rtv->lpVtbl->Release(d->rtv);
        d->rtv = NULL;
    }

    GetClientRect(d->render_hwnd, &rc);
    idd_log(d, L"Resize: render_hwnd client=%dx%d, frame=%ux%u",
            rc.right, rc.bottom, d->frame_width, d->frame_height);
    if (rc.right == 0 || rc.bottom == 0) return;

    hr = d->swap_chain->lpVtbl->ResizeBuffers(d->swap_chain, 0,
            (UINT)rc.right, (UINT)rc.bottom,
            DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        ui_log(L"IDD: ResizeBuffers failed (0x%08X)", hr);
        return;
    }

    hr = d->swap_chain->lpVtbl->GetBuffer(d->swap_chain, 0,
                                   &IID_ID3D11Texture2D, (void **)&back_buf);
    if (SUCCEEDED(hr)) {
        d->device->lpVtbl->CreateRenderTargetView(d->device,
                (ID3D11Resource *)back_buf, NULL, &d->rtv);
        back_buf->lpVtbl->Release(back_buf);
    }
}

/* Re-create the frame texture when the guest resolution changed. Called with
   frame_cs held (frame_width/height are updated by the recv thread under it). */
static BOOL d3d_ensure_frame_texture(VmDisplayIdd *d)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
    ID3D11Texture2D *tex = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    HRESULT hr;

    if (d->frame_tex && d->tex_width == d->frame_width && d->tex_height == d->frame_height)
        return TRUE;
    if (d->frame_width == 0 || d->frame_height == 0) return FALSE;

    ZeroMemory(&td, sizeof(td));
    td.Width              = d->frame_width;
    td.Height             = d->frame_height;
    td.MipLevels          = 1;
    td.ArraySize          = 1;
    td.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count   = 1;
    td.Usage              = D3D11_USAGE_DYNAMIC;
    td.BindFlags          = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags     = D3D11_CPU_ACCESS_WRITE;
    hr = d->device->lpVtbl->CreateTexture2D(d->device, &td, NULL, &tex);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreateTexture2D %ux%u failed (0x%08X)", d->frame_width, d->frame_height, hr);
        return FALSE;
    }

    ZeroMemory(&srv_desc, sizeof(srv_desc));
    srv_desc.Format                    = DXGI_FORMAT_B8G8R8A8_UNORM;
    srv_desc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels       = 1;
    hr = d->device->lpVtbl->CreateShaderResourceView(d->device, (ID3D11Resource *)tex,
                                                     &srv_desc, &srv);
    if (FAILED(hr)) {
        tex->lpVtbl->Release(tex);
        ui_log(L"IDD: CreateShaderResourceView %ux%u failed (0x%08X)",
               d->frame_width, d->frame_height, hr);
        return FALSE;
    }

    if (d->frame_srv) d->frame_srv->lpVtbl->Release(d->frame_srv);
    if (d->frame_tex) d->frame_tex->lpVtbl->Release(d->frame_tex);
    d->frame_tex  = tex;
    d->frame_srv  = srv;
    d->tex_width  = d->frame_width;
    d->tex_height = d->frame_height;
    idd_log(d, L"Frame texture now %ux%u.", d->tex_width, d->tex_height);
    return TRUE;
}

static void idd_video_log(void *user, const wchar_t *msg)
{
    idd_log_main((VmDisplayIdd *)user, L"%s", msg);
}

/* Decode the queued access units on the window thread. Returns TRUE when a
   new picture is ready. */
static BOOL idd_decode_video(VmDisplayIdd *d)
{
    BYTE *work;
    UINT len, off = 0;
    BOOL got = FALSE, need_key = FALSE;

    EnterCriticalSection(&d->frame_cs);
    {
        UINT cap = d->vq_cap;
        work = d->vq_buf;
        len = d->vq_len;
        d->vq_buf = d->vq_work;          /* the recv thread fills the other buffer */
        d->vq_cap = d->vq_work_cap;
        d->vq_len = 0;
        d->vq_work = work;
        d->vq_work_cap = cap;
    }
    LeaveCriticalSection(&d->frame_cs);
    if (!work || !len) return FALSE;

    while (off + sizeof(VideoQEntry) <= len) {
        VideoQEntry e;
        const BYTE *au;
        BOOL key;
        int r;
        memcpy(&e, work + off, sizeof(e));
        au = work + off + sizeof(e);
        off += (UINT)sizeof(e) + e.len;
        if (off > len) break;
        key = (e.flags & VIDEO_FLAG_KEY) != 0;

        if (d->vdec_unavailable) continue;
        if (d->vdec && (vm_video_decoder_width(d->vdec) != e.width ||
                        vm_video_decoder_height(d->vdec) != e.height ||
                        vm_video_decoder_codec(d->vdec) != e.codec)) {
            vm_video_decoder_destroy(d->vdec);
            d->vdec = NULL;
            d->showing_video = FALSE;
        }
        if (!d->vdec) {
            if (!key) { need_key = TRUE; continue; }
            d->vdec = vm_video_decoder_create(d->device, d->ctx, e.codec, e.width, e.height,
                                              idd_video_log, d);
            if (!d->vdec && e.codec == VM_VIDEO_CODEC_HEVC) {
                /* No working HEVC decoder here: ask for H.264 instead. */
                d->hevc_unavailable = TRUE;
                idd_log_main(d, L"HEVC decoding is not available on this PC; switching to H.264.");
                idd_send_perf(d, PERF_FLAG_KEYFRAME);
                need_key = FALSE;
                continue;
            }
            if (!d->vdec) {
                /* Windows N editions lack the H.264 decoder (Media Feature
                   Pack). Fall back to raw frames for this session. */
                d->vdec_unavailable = TRUE;
                idd_log_main(d, L"This PC cannot decode H.264 video (on Windows N install the "
                           L"Media Feature Pack). Using raw frames.");
                idd_send_perf(d, 0);
                continue;
            }
        }
        r = vm_video_decoder_decode(d->vdec, au, e.len);
        if (r < 0) {
            vm_video_decoder_destroy(d->vdec);
            d->vdec = NULL;
            need_key = TRUE;
            continue;
        }
        if (r > 0) got = TRUE;
    }
    if (need_key) {
        EnterCriticalSection(&d->frame_cs);
        d->vq_wait_key = TRUE;
        LeaveCriticalSection(&d->frame_cs);
        idd_send_perf(d, PERF_FLAG_KEYFRAME);
    }
    if (got) d->showing_video = TRUE;
    return got;
}

/* Keep a mip-mapped copy of the shown picture for the backdrop. */
static BOOL d3d_bg_update(VmDisplayIdd *d, ID3D11ShaderResourceView *src)
{
    ID3D11Resource *res = NULL;
    UINT fw = d->frame_width, fh = d->frame_height;
    if (!d->bg_tex || d->bg_w != fw || d->bg_h != fh) {
        D3D11_TEXTURE2D_DESC td;
        if (d->bg_srv) { d->bg_srv->lpVtbl->Release(d->bg_srv); d->bg_srv = NULL; }
        if (d->bg_tex) { d->bg_tex->lpVtbl->Release(d->bg_tex); d->bg_tex = NULL; }
        ZeroMemory(&td, sizeof(td));
        td.Width = fw;
        td.Height = fh;
        td.MipLevels = 0;                     /* full chain */
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(d->device->lpVtbl->CreateTexture2D(d->device, &td, NULL, &d->bg_tex))) return FALSE;
        if (FAILED(d->device->lpVtbl->CreateShaderResourceView(d->device, (ID3D11Resource *)d->bg_tex,
                                                               NULL, &d->bg_srv))) {
            d->bg_tex->lpVtbl->Release(d->bg_tex);
            d->bg_tex = NULL;
            return FALSE;
        }
        d->bg_w = fw;
        d->bg_h = fh;
        d->bg_stale = TRUE;
    }
    if (d->bg_stale) {
        D3D11_BOX box;
        src->lpVtbl->GetResource(src, &res);
        if (!res) return FALSE;
        box.left = 0; box.top = 0; box.front = 0;
        box.right = fw; box.bottom = fh; box.back = 1;
        d->ctx->lpVtbl->CopySubresourceRegion(d->ctx, (ID3D11Resource *)d->bg_tex, 0, 0, 0, 0,
                                              res, 0, &box);
        res->lpVtbl->Release(res);
        d->ctx->lpVtbl->GenerateMips(d->ctx, d->bg_srv);
        d->bg_stale = FALSE;
    }
    return TRUE;
}

static float ease_out_cubic(float t)
{
    t = 1.0f - t;
    return 1.0f - t * t * t;
}

/* Constants for the backdrop blend shader (b0, 16 floats) for a picture
   at rect (x, y, w, h client pixels). Also refreshes the mip-mapped copy.
   FALSE = backdrop not available. */
static BOOL d3d_blend_consts(VmDisplayIdd *d, ID3D11ShaderResourceView *src,
                             UINT cw, UINT ch, const float *rect, float fade, float *c)
{
    float fa, ca, lod = 0.0f, mw, feather;
    UINT fw = d->frame_width, fh = d->frame_height, lw, lh;
    BOOL bars_x, bars_y;

    if (!src || !fw || !fh || !cw || !ch) return FALSE;
    if (!d3d_bg_update(d, src)) return FALSE;

    fa = (float)fw / (float)fh;
    ca = (float)cw / (float)ch;
    for (mw = (float)fw; mw > 48.0f; mw *= 0.5f) lod += 1.0f;   /* ~48 px wide mip */
    lw = fw >> (UINT)lod; if (!lw) lw = 1;
    lh = fh >> (UINT)lod; if (!lh) lh = 1;
    /* Feather: about 6% of the picture's short side, at least 24 px. */
    feather = (rect[2] < rect[3] ? rect[2] : rect[3]) * 0.06f;
    if (feather < 24.0f) feather = 24.0f;
    bars_x = rect[0] > 0.5f || rect[0] + rect[2] < (float)cw - 0.5f;
    bars_y = rect[1] > 0.5f || rect[1] + rect[3] < (float)ch - 0.5f;

    c[0] = ca > fa ? 1.0f : ca / fa;      /* cover */
    c[1] = ca > fa ? fa / ca : 1.0f;
    c[2] = lod;
    c[3] = 0.6f;                          /* dim */
    c[4] = 1.0f / (float)lw;
    c[5] = 1.0f / (float)lh;
    c[6] = fade;
    c[7] = 0.0f;
    c[8]  = rect[0] / (float)cw;
    c[9]  = rect[1] / (float)ch;
    c[10] = (rect[0] + rect[2]) / (float)cw;
    c[11] = (rect[1] + rect[3]) / (float)ch;
    /* No fade along edges that touch the window (no bars there). */
    c[12] = bars_x ? feather / (float)cw : 1e-6f;
    c[13] = bars_y ? feather / (float)ch : 1e-6f;
    c[14] = c[15] = 0.0f;
    return TRUE;
}

/* Draw the picture (rect x,y,w,h in client pixels) blended into the
   blurred backdrop over the whole client area. FALSE = not drawn. */
static BOOL d3d_draw_adaptive(VmDisplayIdd *d, ID3D11ShaderResourceView *src,
                              UINT cw, UINT ch, const float *rect, float fade)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_VIEWPORT full;
    ID3D11ShaderResourceView *views[2];
    float consts[16];

    if (!d->ps_blur || !d->cb_blur) return FALSE;
    if (!d3d_blend_consts(d, src, cw, ch, rect, fade, consts)) return FALSE;
    if (FAILED(d->ctx->lpVtbl->Map(d->ctx, (ID3D11Resource *)d->cb_blur, 0,
                                   D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return FALSE;
    memcpy(m.pData, consts, sizeof(consts));
    d->ctx->lpVtbl->Unmap(d->ctx, (ID3D11Resource *)d->cb_blur, 0);

    ZeroMemory(&full, sizeof(full));
    full.Width = (float)cw;
    full.Height = (float)ch;
    full.MaxDepth = 1.0f;
    views[0] = src;
    views[1] = d->bg_srv;
    d->ctx->lpVtbl->RSSetViewports(d->ctx, 1, &full);
    d->ctx->lpVtbl->VSSetShader(d->ctx, d->vs, NULL, 0);
    d->ctx->lpVtbl->PSSetShader(d->ctx, d->ps_blur, NULL, 0);
    d->ctx->lpVtbl->PSSetConstantBuffers(d->ctx, 0, 1, &d->cb_blur);
    d->ctx->lpVtbl->PSSetShaderResources(d->ctx, 0, 2, views);
    d->ctx->lpVtbl->PSSetSamplers(d->ctx, 0, 1, &d->sampler);
    d->ctx->lpVtbl->Draw(d->ctx, 3, 0);
    views[0] = views[1] = NULL;
    d->ctx->lpVtbl->PSSetShaderResources(d->ctx, 0, 2, views);
    return TRUE;
}

static void d3d_render_frame(VmDisplayIdd *d)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_VIEWPORT vp;
    RECT rc;
    HRESULT hr;
    float clear_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    BOOL frame_uploaded = FALSE;

    if (!d->device || !d->ctx || (!d->dx12 && (!d->swap_chain || !d->rtv)))
        return;

    /* Performance mode: decode every queued access unit (each one depends on
       the previous) and show the last picture. */
    if (d->vq_len && idd_decode_video(d))
        frame_uploaded = TRUE;

    /* Upload frame data to GPU texture if dirty */
    if (d->frame_dirty) {
        EnterCriticalSection(&d->frame_cs);
        if (!d3d_ensure_frame_texture(d)) {
            d->frame_dirty = FALSE;
            LeaveCriticalSection(&d->frame_cs);
            return;
        }
        hr = d->ctx->lpVtbl->Map(d->ctx,
                (ID3D11Resource *)d->frame_tex, 0,
                D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            UINT row;
            UINT copy_stride = d->frame_width * 4;
            if (copy_stride > mapped.RowPitch)
                copy_stride = mapped.RowPitch;
            if (copy_stride > d->frame_stride)
                copy_stride = d->frame_stride;

            for (row = 0; row < d->frame_height && row < d->tex_height; row++) {
                memcpy((BYTE *)mapped.pData + row * mapped.RowPitch,
                       d->frame_buf + row * d->frame_stride,
                       copy_stride);
            }
            d->ctx->lpVtbl->Unmap(d->ctx,
                    (ID3D11Resource *)d->frame_tex, 0);
        }
        d->frame_dirty = FALSE;
        LeaveCriticalSection(&d->frame_cs);
        frame_uploaded = TRUE;
        d->showing_video = FALSE;
    }

    /* Compute letterboxed viewport within client area */
    GetClientRect(d->render_hwnd, &rc);
    {
        float vp_x, vp_y, vp_w, vp_h;
        compute_letterbox((UINT)rc.right, (UINT)rc.bottom,
                          d->frame_width, d->frame_height,
                          &vp_x, &vp_y, &vp_w, &vp_h);
        ZeroMemory(&vp, sizeof(vp));
        vp.TopLeftX = vp_x;
        vp.TopLeftY = vp_y;
        vp.Width    = vp_w;
        vp.Height   = vp_h;
        vp.MaxDepth = 1.0f;
    }

    /* Title with resolution and FPS (self-throttled to twice a second;
       setting it per frame was measurable at high frame rates). */
    if (frame_uploaded) {
        idd_update_title(d, FALSE);
        d->bg_stale = TRUE;
    }
    d->render_count++;

    if (!d->dx12) {
        d->ctx->lpVtbl->OMSetRenderTargets(d->ctx, 1, &d->rtv, NULL);
        d->ctx->lpVtbl->ClearRenderTargetView(d->ctx, d->rtv, clear_color);
    }

    d->ctx->lpVtbl->IASetPrimitiveTopology(d->ctx,
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d->ctx->lpVtbl->IASetInputLayout(d->ctx, NULL);

    {
        ID3D11ShaderResourceView *srv = d->frame_srv;
        float target[4], shown[4];
        ULONGLONG now = GetTickCount64();
        BOOL bars;
        if (d->showing_video && d->vdec)
            srv = vm_video_decoder_srv(d->vdec);

        /* Ease the shown picture rect toward where it belongs (~180 ms)
           instead of jumping when the window or resolution changes. */
        target[0] = vp.TopLeftX; target[1] = vp.TopLeftY;
        target[2] = vp.Width;    target[3] = vp.Height;
        if (!d->anim_valid || d->res_choice != RES_ADAPTIVE) {
            memcpy(d->anim_rect, target, sizeof(target));
            d->anim_valid = TRUE;
            d->anim_active = FALSE;
        } else {
            float dt = (float)(now - d->anim_tick) / 1000.0f;
            /* Frame-rate independent exponential approach (~60 ms time
               constant): settles in about 200 ms. */
            float k = dt > 0.5f ? 1.0f : 1.0f - expf(-dt / 0.06f);
            int i;
            BOOL moving = FALSE;
            for (i = 0; i < 4; i++) {
                float diff = target[i] - d->anim_rect[i];
                if (diff > 0.5f || diff < -0.5f) {
                    d->anim_rect[i] += diff * k;
                    moving = TRUE;
                } else {
                    d->anim_rect[i] = target[i];
                }
            }
            d->anim_active = moving;
        }
        d->anim_tick = now;
        memcpy(shown, d->anim_rect, sizeof(shown));

        /* Adaptive sizes are rounded (width to 8 px), so a few pixels of
           bar are normal once settled: only real gaps get the backdrop. */
        bars = d->res_choice == RES_ADAPTIVE &&
               ((float)rc.right - shown[2] > 12.0f || (float)rc.bottom - shown[3] > 12.0f);
        if (d->dx12) {
            /* Direct3D 12 presents; D3D11 only provides the textures. */
            float consts[16];
            BOOL blend = FALSE;
            if (bars) {
                float fade;
                if (!d->bd_start) d->bd_start = now;
                fade = (float)(now - d->bd_start) / 220.0f;
                if (fade >= 1.0f) fade = 1.0f; else d->anim_active = TRUE;
                blend = d3d_blend_consts(d, srv, (UINT)rc.right, (UINT)rc.bottom, shown,
                                         ease_out_cubic(fade), consts);
            } else {
                d->bd_start = 0;
            }
            dx12_present(d->dx12, srv, shown, blend, blend ? d->bg_srv : NULL, consts);
            return;
        }
        if (bars) {
            float fade;
            if (!d->bd_start) d->bd_start = now;
            fade = (float)(now - d->bd_start) / 220.0f;             /* fade in */
            if (fade >= 1.0f) fade = 1.0f; else d->anim_active = TRUE;
            fade = ease_out_cubic(fade);
            if (d3d_draw_adaptive(d, srv, (UINT)rc.right, (UINT)rc.bottom, shown, fade)) {
                d->swap_chain->lpVtbl->Present(d->swap_chain, 0, 0);
                return;
            }
        } else {
            d->bd_start = 0;
        }

        vp.TopLeftX = shown[0]; vp.TopLeftY = shown[1];
        vp.Width = shown[2];    vp.Height = shown[3];
        d->ctx->lpVtbl->RSSetViewports(d->ctx, 1, &vp);
        d->ctx->lpVtbl->VSSetShader(d->ctx, d->vs, NULL, 0);
        d->ctx->lpVtbl->PSSetShader(d->ctx, d->ps, NULL, 0);
        d->ctx->lpVtbl->PSSetShaderResources(d->ctx, 0, 1, &srv);
        d->ctx->lpVtbl->PSSetSamplers(d->ctx, 0, 1, &d->sampler);

        /* Draw fullscreen triangle (3 vertices, no vertex buffer) */
        d->ctx->lpVtbl->Draw(d->ctx, 3, 0);
    }

    d->swap_chain->lpVtbl->Present(d->swap_chain, 0, 0);
}

static void d3d_cleanup(VmDisplayIdd *d)
{
    if (d->vdec)       { vm_video_decoder_destroy(d->vdec);              d->vdec = NULL; }
    if (d->dx12)       { dx12_destroy(d->dx12);                          d->dx12 = NULL; }
    if (d->bg_srv)     { d->bg_srv->lpVtbl->Release(d->bg_srv);         d->bg_srv = NULL; }
    if (d->bg_tex)     { d->bg_tex->lpVtbl->Release(d->bg_tex);         d->bg_tex = NULL; }
    if (d->cb_blur)    { d->cb_blur->lpVtbl->Release(d->cb_blur);       d->cb_blur = NULL; }
    if (d->ps_blur)    { d->ps_blur->lpVtbl->Release(d->ps_blur);       d->ps_blur = NULL; }
    if (d->sampler)    { d->sampler->lpVtbl->Release(d->sampler);       d->sampler = NULL; }
    if (d->ps)         { d->ps->lpVtbl->Release(d->ps);                 d->ps = NULL; }
    if (d->vs)         { d->vs->lpVtbl->Release(d->vs);                 d->vs = NULL; }
    if (d->frame_srv)  { d->frame_srv->lpVtbl->Release(d->frame_srv);   d->frame_srv = NULL; }
    if (d->frame_tex)  { d->frame_tex->lpVtbl->Release(d->frame_tex);   d->frame_tex = NULL; }
    if (d->rtv)        { d->rtv->lpVtbl->Release(d->rtv);               d->rtv = NULL; }
    if (d->swap_chain) { d->swap_chain->lpVtbl->Release(d->swap_chain); d->swap_chain = NULL; }
    if (d->ctx)        { d->ctx->lpVtbl->Release(d->ctx);               d->ctx = NULL; }
    if (d->device)     { d->device->lpVtbl->Release(d->device);         d->device = NULL; }
}

/* ==================================================================
 * Guest cursor — create HCURSOR from received bitmap
 * ================================================================== */

/* Cursor types from IddCx IDDCX_CURSOR_SHAPE_TYPE */
#define CURSOR_TYPE_MASKED_COLOR  1
#define CURSOR_TYPE_ALPHA         2

static HCURSOR create_cursor_from_bitmap(UINT width, UINT height,
                                          UINT xhot, UINT yhot,
                                          UINT cursor_type, UINT pitch,
                                          UINT shape_data_size,
                                          const BYTE *shape_data)
{
    HCURSOR result = NULL;
    BITMAPINFO bmi;
    HBITMAP hColor = NULL, hMask = NULL;
    ICONINFO ii;
    HDC hdc;

    if (width == 0 || height == 0 || width > 256 || height > 256)
        return NULL;

    /* shape_data holds exactly shape_data_size bytes; both copy paths read up
       to (height-1)*pitch + width*4 bytes. Require the stride to cover a row
       and the total to cover all rows, else a tiny buffer with large
       width/height/pitch would over-read. */
    if (pitch < width * 4 || (UINT64)pitch * height > (UINT64)shape_data_size)
        return NULL;

    hdc = GetDC(NULL);

    if (cursor_type == CURSOR_TYPE_MASKED_COLOR) {
        /* MASKED_COLOR (IddCx 1.10 / QueryHardwareCursor3): single 32bpp BGRA
           image where the alpha channel encodes the AND mask.
           Height is the ACTUAL cursor height (not doubled).
           Per pixel: A = AND mask (0xFF = transparent/XOR, 0x00 = opaque),
                      B,G,R = XOR color values.
           We extract A into a 1bpp monochrome hbmMask and BGR into hbmColor. */
        UINT mask_row_bytes = (width + 7) / 8;
        /* CreateBitmap consumes WORD-aligned rows, unlike a DWORD-aligned
           DIB. */
        UINT mask_pitch = (mask_row_bytes + 1) & ~1u;
        void *color_bits = NULL;
        BYTE *mask_buf;
        UINT row, col;

        /* Build 1bpp AND mask from alpha channel */
        mask_buf = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                      mask_pitch * height);
        if (!mask_buf) { ReleaseDC(NULL, hdc); return NULL; }

        for (row = 0; row < height; row++) {
            const BYTE *src_row = shape_data + row * pitch;
            BYTE *dst_row = mask_buf + row * mask_pitch;
            for (col = 0; col < width; col++) {
                BYTE alpha = src_row[col * 4 + 3];  /* A channel */
                if (alpha != 0)
                    dst_row[col / 8] |= (0x80 >> (col & 7));
            }
        }
        hMask = CreateBitmap((int)width, (int)height, 1, 1, mask_buf);
        HeapFree(GetProcessHeap(), 0, mask_buf);

        /* XOR color bitmap (32bpp, top-down) — copy full BGRA data */
        ZeroMemory(&bmi, sizeof(bmi));
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = (LONG)width;
        bmi.bmiHeader.biHeight      = -(LONG)height;
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        hColor = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &color_bits, NULL, 0);
        if (hColor && color_bits) {
            UINT dst_pitch = width * 4;
            for (row = 0; row < height; row++) {
                const BYTE *src = shape_data + row * pitch;
                BYTE *dst = (BYTE *)color_bits + row * dst_pitch;
                for (col = 0; col < width; col++) {
                    dst[col * 4 + 0] = src[col * 4 + 0];  /* B */
                    dst[col * 4 + 1] = src[col * 4 + 1];  /* G */
                    dst[col * 4 + 2] = src[col * 4 + 2];  /* R */
                    dst[col * 4 + 3] = 0;                  /* A = 0, AND mask handles transparency */
                }
            }
        }

        ReleaseDC(NULL, hdc);

        if (!hColor || !hMask) {
            if (hColor) DeleteObject(hColor);
            if (hMask)  DeleteObject(hMask);
            return NULL;
        }

        ii.fIcon    = FALSE;
        ii.xHotspot = xhot;
        ii.yHotspot = yhot;
        ii.hbmMask  = hMask;
        ii.hbmColor = hColor;

    } else {
        /* ALPHA (type 2): 32bpp BGRA with premultiplied alpha.
           Height is the real cursor height.

           The 32-bpp DIB section with BI_RGB doesn't tell Windows that
           the top byte is alpha — Windows treats it as 24-bit RGB +
           padding and, with an all-zero AND mask, renders EVERY pixel
           opaque. For cursors whose transparent regions are stored as
           (0,0,0,0) (e.g. ours from Linux), that paints a black square
           around the cursor. Windows-VDD cursors happen not to trigger
           this because their transparent regions have non-zero RGB.

           Fix: build the AND mask from the alpha channel — bit set
           (1) = transparent, bit cleared (0) = opaque. Threshold low
           (any alpha > 0 = opaque) preserves anti-aliased edges, the
           alpha channel then handles smooth blending of those edges.

           Use BITMAPV4HEADER with explicit alpha mask instead of
           BITMAPINFOHEADER+BI_RGB. The latter is ambiguous at 32-bpp:
           some Windows paths treat it as XRGB (alpha ignored, RGB
           rendered at full opacity → cursor body looks too bright).
           V4 with bV4AlphaMask = 0xFF000000 settles it. */
        void *color_bits = NULL;
        UINT row, col, dst_pitch;
        UINT mask_row_bytes, mask_pitch;
        BYTE *mask_buf;
        BITMAPV4HEADER bv4;

        ZeroMemory(&bv4, sizeof(bv4));
        bv4.bV4Size          = sizeof(BITMAPV4HEADER);
        bv4.bV4Width         = (LONG)width;
        bv4.bV4Height        = -(LONG)height;
        bv4.bV4Planes        = 1;
        bv4.bV4BitCount      = 32;
        bv4.bV4V4Compression = BI_BITFIELDS;
        bv4.bV4RedMask       = 0x00FF0000;
        bv4.bV4GreenMask     = 0x0000FF00;
        bv4.bV4BlueMask      = 0x000000FF;
        bv4.bV4AlphaMask     = 0xFF000000;

        hColor = CreateDIBSection(hdc, (BITMAPINFO *)&bv4,
                                  DIB_RGB_COLORS, &color_bits, NULL, 0);
        if (!hColor || !color_bits) {
            ReleaseDC(NULL, hdc);
            return NULL;
        }

        dst_pitch = width * 4;
        for (row = 0; row < height; row++) {
            memcpy((BYTE *)color_bits + row * dst_pitch,
                   shape_data + row * pitch,
                   dst_pitch);
        }

        /* AND mask, 1bpp, WORD-aligned for CreateBitmap. Bit set = transparent. */
        mask_row_bytes = (width + 7) / 8;
        mask_pitch     = (mask_row_bytes + 1) & ~1u;
        mask_buf = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                      (size_t)mask_pitch * height);
        if (!mask_buf) {
            DeleteObject(hColor);
            ReleaseDC(NULL, hdc);
            return NULL;
        }
        for (row = 0; row < height; row++) {
            const BYTE *src_row = shape_data + row * pitch;
            BYTE *dst_row = mask_buf + row * mask_pitch;
            for (col = 0; col < width; col++) {
                if (src_row[col * 4 + 3] == 0)
                    dst_row[col / 8] |= (0x80 >> (col & 7));
            }
        }
        hMask = CreateBitmap((int)width, (int)height, 1, 1, mask_buf);
        HeapFree(GetProcessHeap(), 0, mask_buf);

        ReleaseDC(NULL, hdc);

        if (!hMask) {
            DeleteObject(hColor);
            return NULL;
        }

        ii.fIcon    = FALSE;
        ii.xHotspot = xhot;
        ii.yHotspot = yhot;
        ii.hbmMask  = hMask;
        ii.hbmColor = hColor;
    }

    result = (HCURSOR)CreateIconIndirect(&ii);

    DeleteObject(hColor);
    DeleteObject(hMask);

    return result;
}

/* ==================================================================
 * Recv thread - connects to VM, receives frames, updates frame_buf
 * ================================================================== */

static void clip_log_callback(const wchar_t *msg, void *user_data)
{
    VmDisplayIdd *d = (VmDisplayIdd *)user_data;
    idd_log(d, L"%s", msg);
}

/* Frame size for video pictures. Keeps frame_buf the size of the frame (the
   raw path relies on that) and tells the rest of the viewer the new size.
   Called with frame_cs held. */
static BOOL idd_video_frame_size_locked(VmDisplayIdd *d, UINT w, UINT h)
{
    BYTE *nb;
    if (w == d->frame_width && h == d->frame_height) return TRUE;
    nb = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)w * 4 * h);
    if (!nb) return FALSE;
    if (d->frame_buf) HeapFree(GetProcessHeap(), 0, d->frame_buf);
    d->frame_buf = nb;
    d->frame_width = w;
    d->frame_height = h;
    d->frame_stride = w * 4;
    idd_log(d, L"Resolution changed to %ux%u (video).", w, h);
    return TRUE;
}

/* Queue one access unit for the window thread. Called with frame_cs held.
   Returns FALSE when the queue overflowed (the caller asks for a key frame). */
static BOOL idd_video_enqueue_locked(VmDisplayIdd *d, const VideoHeader *vh, const BYTE *data)
{
    VideoQEntry e;
    UINT need = (UINT)sizeof(e) + vh->size;
    BOOL key = (vh->flags & VIDEO_FLAG_KEY) != 0;

    if (d->vq_wait_key && !key) return TRUE;   /* useless without its key frame */
    if (key) d->vq_wait_key = FALSE;
    if (d->vq_len + need > MAX_VIDEO_QUEUE) {
        /* The window thread is not keeping up (window being dragged, etc.):
           drop the backlog and restart from a key frame. */
        d->vq_len = 0;
        d->vq_wait_key = TRUE;
        return FALSE;
    }
    if (d->vq_cap < d->vq_len + need) {
        UINT cap = d->vq_cap ? d->vq_cap : 1024 * 1024;
        BYTE *nb;
        while (cap < d->vq_len + need) cap *= 2;
        nb = d->vq_buf ? (BYTE *)HeapReAlloc(GetProcessHeap(), 0, d->vq_buf, cap)
                       : (BYTE *)HeapAlloc(GetProcessHeap(), 0, cap);
        if (!nb) { d->vq_len = 0; d->vq_wait_key = TRUE; return FALSE; }
        d->vq_buf = nb;
        d->vq_cap = cap;
    }
    e.len = vh->size;
    e.width = vh->width;
    e.height = vh->height;
    e.flags = vh->flags;
    e.codec = vh->codec == VM_VIDEO_CODEC_HEVC ? VM_VIDEO_CODEC_HEVC : VM_VIDEO_CODEC_H264;
    memcpy(d->vq_buf + d->vq_len, &e, sizeof(e));
    memcpy(d->vq_buf + d->vq_len + sizeof(e), data, vh->size);
    d->vq_len += need;
    return TRUE;
}

static DWORD WINAPI idd_recv_thread_proc(LPVOID param)
{
    VmDisplayIdd *d = (VmDisplayIdd *)param;
    WSADATA wsa;
    BYTE *recv_buf = NULL;

    WSAStartup(MAKEWORD(2, 2), &wsa);

    /* Receive buffer for frame pixel data: starts at 1080p and grows when the
       guest switches to a larger mode (up to MAX_FRAME_DATA_SIZE). */
    UINT recv_buf_size = DEFAULT_WIDTH * DEFAULT_HEIGHT * 4;
    recv_buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, recv_buf_size);
    if (!recv_buf) {
        ui_log(L"IDD recv: failed to allocate receive buffer");
        return 1;
    }

    /* Tell the agent to respawn input helper in console session */
    VmInstance *vm = asb_find_vm_by_id(d->vm_id);
    if (!d->stop && vm && vm->agent_online) {
        idd_log(d, L"Sending idd_connect to agent...");
        vm_agent_send(vm, "idd_connect", NULL, 0, 5000);
    }

    /* Input socket lives independently of the frame channel — survives
       frame disconnections so the user can still send input to wake
       the VM screen if the display path goes inactive. */
    {
        SOCKET input_s = INVALID_SOCKET;

    while (!d->stop) {
        SOCKET s;
        FrameHeader hdr;

        /* Ensure input channel is connected (independent of frame channel) */
        if (input_s == INVALID_SOCKET)
            input_s = connect_input(d);

        /* Clipboard module (handles :0005 + :0006 internally) */
        if (!d->clipboard) {
            d->clipboard = vm_clipboard_create(&d->runtime_id, d->os_type,
                                               d->hwnd, clip_log_callback, d);
            if (d->clipboard) {
                idd_log(d, L"Clipboard module created.");
                /* Apply the persisted sharing toggles (default: both on). */
                vm_clipboard_set_share_directions(d->clipboard,
                                                  d->clip_share_host_to_guest,
                                                  d->clip_share_guest_to_host);
            }
        }

        /* Ensure audio recv thread is running (:0004, guest→host).
           The thread handles connecting on its own — the helper may not be up yet. */
        if (!d->audio_recv_thread) {
            d->audio_recv_thread = CreateThread(NULL, 0, audio_recv_thread_proc, d, 0, NULL);
            idd_log(d, L"Audio: Started recv thread (will connect when helper is available).");
        }

        /* Try to connect frame channel (VDD driver, GUID :0002) */
        idd_log(d, L"Connecting to frame service...");
        {
            GUID svc; hcs_service_guid(d->os_type, 2, &svc);
            s = connect_to_hv_service(&d->runtime_id, &svc, 3000);
        }
        if (s == INVALID_SOCKET) {
            int wait;
            idd_log(d, L"Connection failed, retrying in 3s.");
            for (wait = 0; wait < 3000 && !d->stop; wait += 500)
                Sleep(500);
            continue;
        }

        idd_log(d, L"Frame channel connected.");
        InterlockedExchange(&d->guest_driver, -1);
        InterlockedExchange(&d->video_active, 0);
        InterlockedExchange(&d->video_status_hr, 0);
        EnterCriticalSection(&d->frame_cs);
        d->vq_len = 0;
        d->vq_wait_key = FALSE;
        LeaveCriticalSection(&d->frame_cs);
        d->frames_since_connect = 0;
        /* Ask the guest driver whether it takes resolution changes (Windows
           guests only; older drivers never read this and just keep sending). */
        if (d->mode_supported) {
            ModeRequest q = { VDD_HELLO_QUERY_MAGIC, 0, 0, 0 };
            send(s, (const char *)&q, (int)sizeof(q), 0);
        }
        AcquireSRWLockExclusive(&d->frame_send_lock);
        d->frame_socket = s;
        ReleaseSRWLockExclusive(&d->frame_send_lock);
        PostMessageW(d->hwnd, WM_IDD_FRAME_CONNECTED, 0, 0);
        d->cursor_visible = TRUE;
        PostMessageW(d->hwnd, WM_IDD_CURSOR_CHANGED, 0, 0);

        /* Receive loop — reads magic first to dispatch frame vs cursor */
        while (!d->stop) {
            RECT dirty_rects[MAX_DIRTY_RECTS];
            UINT32 data_size;
            UINT32 rect_count;
            UINT32 i;
            UINT32 magic;

            /* Peek at magic to determine message type */
            if (!recv_exact(s, &magic, sizeof(magic)))
                break;

            if (magic == CURSOR_MAGIC) {
                /* Read rest of cursor header (already read magic) */
                CursorHeader chdr;
                chdr.magic = magic;
                if (!recv_exact(s, (BYTE *)&chdr + sizeof(UINT32),
                                sizeof(CursorHeader) - sizeof(UINT32)))
                    break;

                BOOL cursor_changed = d->cursor_visible != (chdr.visible != 0);
                d->cursor_visible = chdr.visible != 0;

                if (chdr.shape_updated && chdr.shape_data_size > 0) {
                    BYTE *cursor_buf;
                    if (chdr.shape_data_size > MAX_CURSOR_SIZE) {
                        idd_log(d, L"Cursor data too large (%u), reconnecting.",
                               chdr.shape_data_size);
                        break;
                    }
                    cursor_buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0,
                                                    chdr.shape_data_size);
                    if (!cursor_buf) break;

                    if (!recv_exact(s, cursor_buf, (int)chdr.shape_data_size)) {
                        HeapFree(GetProcessHeap(), 0, cursor_buf);
                        break;
                    }

                    /* Create new cursor from bitmap */
                    {
                        HCURSOR new_cursor = create_cursor_from_bitmap(
                            chdr.width, chdr.height, chdr.xhot, chdr.yhot,
                            chdr.cursor_type, chdr.pitch,
                            chdr.shape_data_size, cursor_buf);
                        if (new_cursor) {
                            HCURSOR old = d->guest_cursor;
                            d->guest_cursor = new_cursor;
                            d->cursor_shape_id = chdr.shape_id;
                            if (old) DestroyCursor(old);
                            cursor_changed = TRUE;
                        }
                    }

                    HeapFree(GetProcessHeap(), 0, cursor_buf);
                }
                if (cursor_changed)
                    PostMessageW(d->hwnd, WM_IDD_CURSOR_CHANGED, 0, 0);
                continue;  /* back to message loop */
            }

            if (magic == VDD_HELLO_MAGIC) {
                UINT32 rest[3];
                if (!recv_exact(s, rest, (int)sizeof(rest)))
                    break;
                InterlockedExchange(&d->guest_driver, (LONG)rest[0]);
                idd_log_main(d, L"Guest display driver: protocol %u (resolution changes supported%s).",
                        rest[0], rest[0] >= VDD_PROTOCOL_VIDEO ? L", performance mode" : L"");
                if (rest[0] >= VDD_PROTOCOL_VIDEO && d->stream_mode == STREAM_PERFORMANCE)
                    idd_send_perf(d, PERF_FLAG_KEYFRAME);
                if (d->main_hwnd) PostMessageW(d->main_hwnd, WM_APP + 32, 0, 0);
                continue;
            }

            if (magic == PERF_STATUS_MAGIC) {
                UINT32 rest[3];   /* mode, codec, hresult */
                if (!recv_exact(s, rest, (int)sizeof(rest)))
                    break;
                InterlockedExchange(&d->video_active, rest[0] == 1 ? 1 : 0);
                InterlockedExchange(&d->video_codec, rest[1] == VM_VIDEO_CODEC_HEVC ? VM_VIDEO_CODEC_HEVC
                                                                                    : VM_VIDEO_CODEC_H264);
                InterlockedExchange(&d->video_status_hr, (LONG)rest[2]);
                if (rest[0] == 1)
                    idd_log_main(d, L"Performance mode on: the guest GPU encodes %s video.",
                                 rest[1] == VM_VIDEO_CODEC_HEVC ? L"HEVC" : L"H.264");
                else if ((LONG)rest[2] < 0)
                    idd_log_main(d, L"Performance mode unavailable in the guest (0x%08X): no hardware "
                               L"video encoder on its GPU. Showing raw frames.", rest[2]);
                else
                    idd_log_main(d, L"Quality mode: raw frames.");
                PostMessageW(d->hwnd, WM_IDD_VIDEO_STATUS, 0, 0);
                continue;
            }

            if (magic == VIDEO_MAGIC) {
                VideoHeader vh;
                BOOL ok;
                vh.magic = magic;
                if (!recv_exact(s, (BYTE *)&vh + sizeof(UINT32), sizeof(vh) - sizeof(UINT32)))
                    break;
                if (vh.width < 64 || vh.height < 64 || vh.width > MAX_FRAME_WIDTH ||
                    vh.height > MAX_FRAME_HEIGHT || vh.size == 0 || vh.size > MAX_VIDEO_AU) {
                    idd_log(d, L"Invalid video frame %ux%u (%u bytes), reconnecting.",
                            vh.width, vh.height, vh.size);
                    break;
                }
                if (vh.size > recv_buf_size) {
                    BYTE *bigger = (BYTE *)HeapReAlloc(GetProcessHeap(), 0, recv_buf, vh.size);
                    if (!bigger) break;
                    recv_buf = bigger;
                    recv_buf_size = vh.size;
                }
                if (!recv_exact(s, recv_buf, (int)vh.size))
                    break;

                EnterCriticalSection(&d->frame_cs);
                if (!idd_video_frame_size_locked(d, vh.width, vh.height)) {
                    LeaveCriticalSection(&d->frame_cs);
                    break;
                }
                ok = idd_video_enqueue_locked(d, &vh, recv_buf);
                d->recv_count++;
                LeaveCriticalSection(&d->frame_cs);
                if (!ok) {
                    idd_log(d, L"Video backlog dropped, asking for a key frame.");
                    idd_send_perf(d, PERF_FLAG_KEYFRAME);
                }
                if (!d->video_active) InterlockedExchange(&d->video_active, 1);
                if (!d->frame_connected) {
                    d->frame_connected = TRUE;
                    PostMessageW(d->hwnd, WM_IDD_CURSOR_CHANGED, 0, 0);
                }
                if (d->hwnd && IsWindow(d->hwnd))
                    PostMessageW(d->hwnd, WM_IDD_FRAME_READY, 0, 0);
                if (d->input_socket == INVALID_SOCKET && input_s != INVALID_SOCKET) {
                    closesocket(input_s);
                    input_s = INVALID_SOCKET;
                }
                if (input_s == INVALID_SOCKET)
                    input_s = connect_input(d);
                continue;
            }

            if (magic != FRAME_MAGIC) {
                idd_log(d, L"Bad magic 0x%08X, reconnecting.", magic);
                break;
            }

            /* Frames but no answer to the hello query: an older driver. */
            if (d->mode_supported && ++d->frames_since_connect >= HELLO_FRAME_GRACE &&
                InterlockedCompareExchange(&d->guest_driver, 0, -1) == -1) {
                idd_log(d, L"Guest display is older than this App Sandbox: it stays at "
                           L"1920x1080 and ignores resolution changes.%s",
                        d->guest_linux ? L" Create a new Ubuntu VM to get the new display components."
                                       : L" Update the VM's guest components (or create a new VM).");
                if (d->main_hwnd) PostMessageW(d->main_hwnd, WM_APP + 32, 0, 0);
            }

            /* Read rest of frame header (already read magic) */
            hdr.magic = magic;
            if (!recv_exact(s, (BYTE *)&hdr + sizeof(UINT32),
                            sizeof(FrameHeader) - sizeof(UINT32)))
                break;

            /* Sanity checks */
            if (hdr.width == 0 || hdr.height == 0 ||
                hdr.width > MAX_FRAME_WIDTH || hdr.height > MAX_FRAME_HEIGHT ||
                hdr.stride < hdr.width * 4) {
                idd_log(d, L"Invalid frame dimensions %ux%u stride %u.",
                       hdr.width, hdr.height, hdr.stride);
                break;
            }

            rect_count = hdr.dirty_rect_count;
            if (rect_count > MAX_DIRTY_RECTS) {
                idd_log(d, L"Too many dirty rects (%u), reconnecting.", rect_count);
                break;
            }

            /* Read dirty rects */
            if (rect_count > 0) {
                if (!recv_exact(s, dirty_rects, (int)(rect_count * sizeof(RECT))))
                    break;
            }

            /* Read data_size */
            if (!recv_exact(s, &data_size, 4))
                break;

            if (data_size > MAX_FRAME_DATA_SIZE) {
                idd_log(d, L"Frame data too large (%u bytes), reconnecting.", data_size);
                break;
            }

            /* Read pixel data */
            if (data_size > recv_buf_size) {
                BYTE *bigger = (BYTE *)HeapReAlloc(GetProcessHeap(), 0, recv_buf, data_size);
                if (!bigger) {
                    idd_log(d, L"Cannot grow receive buffer to %u bytes, reconnecting.", data_size);
                    break;
                }
                recv_buf = bigger;
                recv_buf_size = data_size;
            }
            if (data_size > 0) {
                if (!recv_exact(s, recv_buf, (int)data_size))
                    break;
            }

            /* Update CPU-side frame buffer */
            EnterCriticalSection(&d->frame_cs);

            /* Reallocate frame_buf if resolution changed */
            if (hdr.width != d->frame_width || hdr.height != d->frame_height) {
                UINT new_stride = hdr.width * 4;
                UINT new_size   = new_stride * hdr.height;
                BYTE *new_buf   = (BYTE *)HeapAlloc(GetProcessHeap(),
                                                     HEAP_ZERO_MEMORY, new_size);
                if (new_buf) {
                    if (d->frame_buf)
                        HeapFree(GetProcessHeap(), 0, d->frame_buf);
                    d->frame_buf    = new_buf;
                    d->frame_width  = hdr.width;
                    d->frame_height = hdr.height;
                    d->frame_stride = new_stride;
                    idd_log(d, L"Frame resolution changed: %ux%u (stride=%u)",
                            hdr.width, hdr.height, hdr.stride);
                    idd_log(d, L"Resolution changed to %ux%u.", hdr.width, hdr.height);
                } else {
                    LeaveCriticalSection(&d->frame_cs);
                    break;
                }
            }

            if (rect_count == 0 && hdr.width == d->frame_width && hdr.height == d->frame_height &&
                hdr.stride == d->frame_stride &&
                data_size >= (UINT)d->frame_stride * d->frame_height) {
                /* Full frame in exactly the frame buffer's layout: swap the
                   buffers instead of copying the whole picture (about 15 MB
                   per frame at 2560x1440). The old frame buffer becomes the
                   next receive buffer. */
                BYTE *old = d->frame_buf;
                SIZE_T cap = HeapSize(GetProcessHeap(), 0, old);
                d->frame_buf = recv_buf;
                recv_buf = old;
                recv_buf_size = (cap == (SIZE_T)-1) ? (UINT)d->frame_stride * d->frame_height : (UINT)cap;
            } else if (rect_count == 0) {
                /* Full frame update */
                UINT row;
                UINT copy_w = hdr.width * 4;
                BYTE *src = recv_buf;
                /* Bound source rows by the bytes actually received: width/height/
                   stride and data_size are independent guest-controlled fields, so
                   only data_size/stride rows of recv_buf hold valid pixels. */
                UINT src_rows = hdr.stride ? (UINT)(data_size / hdr.stride) : 0;
                if (copy_w > d->frame_stride) copy_w = d->frame_stride;
                if (copy_w > hdr.stride)      copy_w = hdr.stride;

                for (row = 0; row < hdr.height && row < d->frame_height &&
                              row < src_rows; row++) {
                    memcpy(d->frame_buf + row * d->frame_stride,
                           src + row * hdr.stride,
                           copy_w);
                }
            } else {
                /* Dirty rect updates — pixel data is per-rect rows concatenated */
                BYTE *src = recv_buf;
                for (i = 0; i < rect_count; i++) {
                    LONG left   = dirty_rects[i].left;
                    LONG top    = dirty_rects[i].top;
                    LONG right  = dirty_rects[i].right;
                    LONG bottom = dirty_rects[i].bottom;
                    UINT rect_w, rect_h, row;
                    UINT rect_row_bytes;

                    /* Clamp to frame bounds */
                    if (left < 0) left = 0;
                    if (top  < 0) top  = 0;
                    if (right  > (LONG)d->frame_width)  right  = (LONG)d->frame_width;
                    if (bottom > (LONG)d->frame_height) bottom = (LONG)d->frame_height;
                    if (left >= right || top >= bottom) continue;

                    rect_w = (UINT)(right - left);
                    rect_h = (UINT)(bottom - top);
                    rect_row_bytes = rect_w * 4;

                    /* Refuse to read past the bytes actually received into recv_buf. */
                    if ((size_t)(src - recv_buf) + (size_t)rect_row_bytes * rect_h >
                        (size_t)data_size)
                        break;

                    for (row = 0; row < rect_h; row++) {
                        UINT dst_y = (UINT)top + row;
                        memcpy(d->frame_buf + dst_y * d->frame_stride + (UINT)left * 4,
                               src + row * rect_row_bytes,
                               rect_row_bytes);
                    }
                    src += rect_row_bytes * rect_h;
                }
            }

            d->frame_dirty = TRUE;
            d->recv_count++;
            LeaveCriticalSection(&d->frame_cs);
            if (!d->frame_connected) {
                d->frame_connected = TRUE;
                PostMessageW(d->hwnd, WM_IDD_CURSOR_CHANGED, 0, 0);
            }

            /* Signal the window thread to repaint */
            if (d->hwnd && IsWindow(d->hwnd))
                PostMessageW(d->hwnd, WM_IDD_FRAME_READY, 0, 0);

            /* Reconnect input socket if send_input flagged it dead */
            if (d->input_socket == INVALID_SOCKET && input_s != INVALID_SOCKET) {
                closesocket(input_s);
                input_s = INVALID_SOCKET;
                idd_log(d, L"Input socket closed, will reconnect...");
            }
            if (input_s == INVALID_SOCKET)
                input_s = connect_input(d);
        }

        /* Frame channel lost — close it but keep input alive */
        d->frame_connected = FALSE;
        d->cursor_visible = TRUE;
        PostMessageW(d->hwnd, WM_IDD_CURSOR_CHANGED, 0, 0);
        AcquireSRWLockExclusive(&d->frame_send_lock);
        d->frame_socket = INVALID_SOCKET;
        ReleaseSRWLockExclusive(&d->frame_send_lock);
        closesocket(s);
        idd_log(d, L"Frame channel disconnected, reconnecting...");

        /* Check if input is still alive (send_input may have flagged it dead) */
        if (d->input_socket == INVALID_SOCKET && input_s != INVALID_SOCKET) {
            closesocket(input_s);
            input_s = INVALID_SOCKET;
            idd_log(d, L"Input socket flagged dead, will reconnect.");
        }

        /* Wait before reconnecting frame channel. Kept short: a display mode
           change re-plugs the guest monitor and drops this channel. */
        {
            int wait;
            for (wait = 0; wait < 1000 && !d->stop; wait += 250)
                Sleep(250);
        }
    }

    /* Final cleanup — close input socket on thread exit */
    AcquireSRWLockExclusive(&d->input_lock);
    d->input_socket = INVALID_SOCKET;
    d->keyboard_version = 1;
    d->mouse_version = 0;
    ZeroMemory(d->held_down, sizeof(d->held_down));
    ReleaseSRWLockExclusive(&d->input_lock);
    PostMessageW(d->hwnd, WM_IDD_INPUT_READY, 0, 0);
    if (input_s != INVALID_SOCKET) {
        closesocket(input_s);
    }
    idd_log(d, L"Input disconnected.");

    } /* end input_s scope */

    if (recv_buf)
        HeapFree(GetProcessHeap(), 0, recv_buf);

    WSACleanup();
    return 0;
}

/* ==================================================================
 * Window thread — creates window, initializes D3D11, runs message pump
 * ================================================================== */

static DWORD WINAPI idd_window_thread_proc(LPVOID param)
{
    VmDisplayIdd *d = (VmDisplayIdd *)param;
    wchar_t title[300];
    MSG msg;

    ensure_idd_class(d->hInstance);

    swprintf_s(title, 300, L"%s - IDD Display", d->vm_name);

    /* Outer window size so the client area matches the chosen resolution
       (1920x1080 in adaptive mode; the guest then follows the window). */
    {
        DWORD style   = WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN;
        DWORD exstyle = 0;
        RECT wr = { 0, 0, 1920, 1080 };
        if (d->res_choice >= 0 && d->res_choice < (int)ARRAYSIZE(g_res_presets)) {
            wr.right  = (LONG)g_res_presets[d->res_choice].w;
            wr.bottom = (LONG)g_res_presets[d->res_choice].h;
        }
        AdjustWindowRectEx(&wr, style, FALSE, exstyle);

        d->hwnd = CreateWindowExW(
            exstyle, IDD_DISPLAY_CLASS, title, style,
            CW_USEDEFAULT, CW_USEDEFAULT,
            wr.right - wr.left, wr.bottom - wr.top,
            NULL, NULL, d->hInstance, d);
    }

    if (!d->hwnd) {
        ui_log(L"IDD: CreateWindowEx failed (0x%08X)", GetLastError());
        d->open = FALSE;
        return 1;
    }

    /* Dark mode title bar to match AppSandbox main window */
    {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(d->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    }

    /* Add options to the system menu (right-click title bar) */
    {
        HMENU sysmenu = GetSystemMenu(d->hwnd, FALSE);
        if (sysmenu) {
            AppendMenuW(sysmenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(sysmenu, MF_STRING, IDM_AUDIO_MUTE, L"Mute audio");
            AppendMenuW(sysmenu, MF_STRING, IDM_XMIT_HOTKEYS, L"Transmit Keyboard Hotkeys");
            AppendMenuW(sysmenu, MF_STRING, IDM_BORDERLESS_FULLSCREEN, L"Toggle Borderless Fullscreen\tF11");
            idd_add_display_menu(d, sysmenu);
            AppendMenuW(sysmenu, MF_STRING, IDM_SHOW_LOG, L"Show Log");
            CheckMenuItem(sysmenu, IDM_XMIT_HOTKEYS,
                          MF_BYCOMMAND | (d->transmit_hotkeys ? MF_CHECKED : MF_UNCHECKED));
            AppendMenuW(sysmenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(sysmenu, MF_STRING, IDM_CLIP_H2G, L"Share Host Clipboard with Guest");
            AppendMenuW(sysmenu, MF_STRING, IDM_CLIP_G2H, L"Share Guest Clipboard with Host");
            CheckMenuItem(sysmenu, IDM_CLIP_H2G,
                          MF_BYCOMMAND | (d->clip_share_host_to_guest ? MF_CHECKED : MF_UNCHECKED));
            CheckMenuItem(sysmenu, IDM_CLIP_G2H,
                          MF_BYCOMMAND | (d->clip_share_guest_to_host ? MF_CHECKED : MF_UNCHECKED));
        }
        idd_update_display_menu(d);
    }

    /* Bring the display window to the foreground on open */
    ShowWindow(d->hwnd, SW_SHOW);
    BringWindowToTop(d->hwnd);
    SetForegroundWindow(d->hwnd);

    /* Render child fills entire client area */
    {
        RECT rc;
        GetClientRect(d->hwnd, &rc);

        d->render_hwnd = CreateWindowExW(
            0, IDD_RENDER_CLASS, NULL,
            WS_CHILD | WS_VISIBLE,
            0, 0, rc.right, rc.bottom,
            d->hwnd, NULL, d->hInstance, NULL);
        if (d->render_hwnd)
            ImmAssociateContextEx(d->render_hwnd, NULL, 0);
    }

    /* Floating toolbar: shown for a few seconds now so it is discovered,
       then whenever the pointer touches the top edge. */
    d->toolbar_hot = -1;
    d->toolbar_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, IDD_TOOLBAR_CLASS, L"",
        WS_POPUP, 0, 0, 10, 10, d->hwnd, NULL, d->hInstance, d);
    if (d->toolbar_hwnd) {
        DWORD corner = 2; /* DWMWCP_ROUND */
        BOOL dark = TRUE;
        DwmSetWindowAttribute(d->toolbar_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
        DwmSetWindowAttribute(d->toolbar_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        tb_show(d, 4000);
    }

    /* Separate top-level log window */
    {
        wchar_t log_title[300];
        HFONT font;
        swprintf_s(log_title, 300, L"%s - IDD Log", d->vm_name);

        /* Created hidden — shown on demand via the "Show Log" system-menu item. */
        d->log_hwnd = CreateWindowExW(
            0, L"AppSandboxIddLog", log_title,
            WS_OVERLAPPEDWINDOW | WS_VSCROLL,
            CW_USEDEFAULT, CW_USEDEFAULT, LOG_WINDOW_W, LOG_WINDOW_H,
            NULL, NULL, d->hInstance, NULL);

        if (d->log_hwnd) {
            /* Dark mode title bar to match AppSandbox main window */
            BOOL dark = TRUE;
            DwmSetWindowAttribute(d->log_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

            /* Fill the log window with a listbox */
            RECT lrc;
            GetClientRect(d->log_hwnd, &lrc);
            d->log_list_hwnd = CreateWindowExW(
                0, L"LISTBOX", NULL,
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_EXTENDEDSEL | LBS_HASSTRINGS,
                0, 0, lrc.right, lrc.bottom,
                d->log_hwnd, (HMENU)(INT_PTR)IDC_LOG_LIST, d->hInstance, NULL);

            /* Subclass the listbox so Ctrl+A / Ctrl+C work when it has focus */
            if (d->log_list_hwnd)
                g_orig_listbox_proc = (WNDPROC)SetWindowLongPtrW(
                    d->log_list_hwnd, GWLP_WNDPROC, (LONG_PTR)idd_log_listbox_proc);

            font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                               FIXED_PITCH | FF_MODERN, L"Consolas");
            if (font && d->log_list_hwnd)
                SendMessageW(d->log_list_hwnd, WM_SETFONT, (WPARAM)font, TRUE);
        }
        idd_log(d, L"IDD display started.");
    }

    /* Initialize D3D11 */
    if (!d3d_init(d)) {
        ui_log(L"IDD: D3D11 initialization failed.");
        DestroyWindow(d->hwnd);
        d->hwnd = NULL;
        d->open = FALSE;
        return 1;
    }

    idd_apply_icon(d);

    /* Game controllers (best effort; no thread, no controllers). */
    d->gamepad_thread = CreateThread(NULL, 0, idd_gamepad_thread_proc, d, 0, NULL);

    /* Start the recv thread now that the window and D3D11 are ready */
    d->recv_thread = CreateThread(NULL, 0, idd_recv_thread_proc, d, 0, NULL);
    if (!d->recv_thread) {
        ui_log(L"IDD: Failed to create recv thread.");
        d3d_cleanup(d);
        DestroyWindow(d->hwnd);
        d->hwnd = NULL;
        d->open = FALSE;
        return 1;
    }

    /* Start a present timer for steady rendering */
    SetTimer(d->hwnd, IDT_PRESENT, PRESENT_MS, NULL);

    idd_update_kbd_hook(d);

    /* Message pump */
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}

/* ==================================================================
 * Window procedure
 * ================================================================== */

static void idd_toggle_borderless_fullscreen(VmDisplayIdd *d, HWND hwnd)
{
    HMENU sysmenu;

    if (!d->borderless_fullscreen) {
        MONITORINFO monitor = { sizeof(monitor) };
        HMONITOR hmonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);

        d->windowed_placement.length = sizeof(d->windowed_placement);
        if (!GetWindowPlacement(hwnd, &d->windowed_placement) ||
            !GetMonitorInfoW(hmonitor, &monitor))
            return;

        d->windowed_style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        SetWindowLongPtrW(hwnd, GWL_STYLE,
                          (d->windowed_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(hwnd, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                     monitor.rcMonitor.right - monitor.rcMonitor.left,
                     monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        d->borderless_fullscreen = TRUE;
    } else {
        SetWindowLongPtrW(hwnd, GWL_STYLE, d->windowed_style);
        SetWindowPlacement(hwnd, &d->windowed_placement);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        d->borderless_fullscreen = FALSE;
    }

    sysmenu = GetSystemMenu(hwnd, FALSE);
    if (sysmenu) {
        CheckMenuItem(sysmenu, IDM_BORDERLESS_FULLSCREEN,
                      MF_BYCOMMAND | (d->borderless_fullscreen ? MF_CHECKED : MF_UNCHECKED));
    }
    idd_log(d, d->borderless_fullscreen
                   ? L"Borderless fullscreen: ON."
                   : L"Borderless fullscreen: OFF.");
}

static LRESULT CALLBACK idd_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    VmDisplayIdd *d;

    if (msg == WM_CREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        AddClipboardFormatListener(hwnd);
        ImmAssociateContextEx(hwnd, NULL, 0);
        return 0;
    }

    d = (VmDisplayIdd *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    if (d && d->mouse_sync_pending &&
        (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN ||
         msg == WM_MBUTTONDOWN || msg == WM_MOUSEWHEEL))
        idd_resume_absolute_mouse(d, NULL);

    switch (msg) {
    case WM_SYSCOMMAND:
        if (d && d->mode_supported) {
            UINT id = (UINT)(wp & 0xFFF0);
            BOOL handled = FALSE;
            if (id == IDM_RES_ADAPTIVE) {
                d->res_choice = RES_ADAPTIVE;
                handled = TRUE;
            } else if (id >= IDM_RES_BASE &&
                       id < IDM_RES_BASE + ARRAYSIZE(g_res_presets) * 0x10) {
                d->res_choice = (int)((id - IDM_RES_BASE) / 0x10);
                handled = TRUE;
            } else if (id >= IDM_HZ_BASE &&
                       id < IDM_HZ_BASE + ARRAYSIZE(g_refresh_rates) * 0x10) {
                d->refresh_hz = g_refresh_rates[(id - IDM_HZ_BASE) / 0x10];
                handled = TRUE;
            } else if (id == IDM_CODEC_AUTO || id == IDM_CODEC_H264 || id == IDM_CODEC_HEVC) {
                PostMessageW(hwnd, WM_IDD_SET_CODEC,
                             id == IDM_CODEC_HEVC ? CODEC_HEVC : id == IDM_CODEC_H264 ? CODEC_H264 : CODEC_AUTO, 0);
                return 0;
            } else if (id == IDM_STREAM_QUALITY || id == IDM_STREAM_PERF) {
                PostMessageW(hwnd, WM_IDD_SET_STREAM,
                             id == IDM_STREAM_PERF ? STREAM_PERFORMANCE : STREAM_QUALITY, 0);
                return 0;
            }
            if (handled) {
                idd_update_display_menu(d);
                idd_save_settings(d);
                idd_apply_display_mode(d, TRUE);
                return 0;
            }
        }
        if (d && (wp & 0xFFF0) == IDM_BORDERLESS_FULLSCREEN) {
            idd_toggle_borderless_fullscreen(d, hwnd);
            return 0;
        }
        if (d && (wp & 0xFFF0) == IDM_AUDIO_MUTE) {
            HMENU sysmenu = GetSystemMenu(hwnd, FALSE);
            wchar_t title[300];
            d->audio_muted = !d->audio_muted;
            if (sysmenu) {
                CheckMenuItem(sysmenu, IDM_AUDIO_MUTE,
                              MF_BYCOMMAND | (d->audio_muted ? MF_CHECKED : MF_UNCHECKED));
            }
            if (d->audio_muted)
                swprintf_s(title, 300, L"\U0001F507 %s - IDD Display", d->vm_name);
            else
                swprintf_s(title, 300, L"%s - IDD Display", d->vm_name);
            SetWindowTextW(hwnd, title);
            idd_log(d, d->audio_muted ? L"Audio muted." : L"Audio unmuted.");
            return 0;
        }
        if (d && (wp & 0xFFF0) == IDM_XMIT_HOTKEYS) {
            HMENU sysmenu = GetSystemMenu(hwnd, FALSE);
            d->transmit_hotkeys = !d->transmit_hotkeys;
            if (sysmenu) {
                CheckMenuItem(sysmenu, IDM_XMIT_HOTKEYS,
                              MF_BYCOMMAND | (d->transmit_hotkeys ? MF_CHECKED : MF_UNCHECKED));
            }
            if (!d->transmit_hotkeys) {
                /* Release anything the guest may be holding from this mode. */
                idd_flush_held_keys(d);
            }
            idd_update_kbd_hook(d);
            idd_save_settings(d);
            idd_log(d, d->transmit_hotkeys
                        ? L"Transmit Keyboard Hotkeys: ON."
                        : L"Transmit Keyboard Hotkeys: OFF.");
            return 0;
        }
        if (d && (wp & 0xFFF0) == IDM_CLIP_H2G) {
            HMENU sysmenu = GetSystemMenu(hwnd, FALSE);
            d->clip_share_host_to_guest = !d->clip_share_host_to_guest;
            if (sysmenu) {
                CheckMenuItem(sysmenu, IDM_CLIP_H2G,
                              MF_BYCOMMAND | (d->clip_share_host_to_guest ? MF_CHECKED : MF_UNCHECKED));
            }
            if (d->clipboard)
                vm_clipboard_set_share_directions(d->clipboard,
                                                  d->clip_share_host_to_guest,
                                                  d->clip_share_guest_to_host);
            idd_save_settings(d);
            idd_log(d, d->clip_share_host_to_guest
                        ? L"Share host clipboard with guest: ON."
                        : L"Share host clipboard with guest: OFF.");
            return 0;
        }
        if (d && (wp & 0xFFF0) == IDM_CLIP_G2H) {
            HMENU sysmenu = GetSystemMenu(hwnd, FALSE);
            d->clip_share_guest_to_host = !d->clip_share_guest_to_host;
            if (sysmenu) {
                CheckMenuItem(sysmenu, IDM_CLIP_G2H,
                              MF_BYCOMMAND | (d->clip_share_guest_to_host ? MF_CHECKED : MF_UNCHECKED));
            }
            if (d->clipboard)
                vm_clipboard_set_share_directions(d->clipboard,
                                                  d->clip_share_host_to_guest,
                                                  d->clip_share_guest_to_host);
            idd_save_settings(d);
            idd_log(d, d->clip_share_guest_to_host
                        ? L"Share guest clipboard with host: ON."
                        : L"Share guest clipboard with host: OFF.");
            return 0;
        }
        if (d && (wp & 0xFFF0) == IDM_SHOW_LOG) {
            /* Reveal the log window (hidden by default). Closing it via its
               own [X] just hides it again (see idd_log_proc WM_CLOSE), so
               this item can re-open it any number of times. */
            if (d->log_hwnd && IsWindow(d->log_hwnd)) {
                if (IsIconic(d->log_hwnd))
                    ShowWindow(d->log_hwnd, SW_RESTORE);
                else
                    ShowWindow(d->log_hwnd, SW_SHOW);
                BringWindowToTop(d->log_hwnd);
                SetForegroundWindow(d->log_hwnd);
            }
            return 0;
        }
        break;

    case WM_CLOSE:
        if (d) {
            BOOL user_initiated = d->open;

            RemoveClipboardFormatListener(hwnd);

            /* Remove the hotkey hook and release any keys still held in the
               guest before tearing the window down. */
            idd_remove_kbd_hook(d);
            idd_flush_held_keys(d);
            idd_flush_mouse_buttons(d);

            /* Stop recv threads */
            d->stop = TRUE;
            idd_update_relative_mouse(d);

            /* Destroy clipboard module */
            if (d->clipboard) {
                vm_clipboard_destroy(d->clipboard);
                d->clipboard = NULL;
            }

            /* Wait for audio recv thread (:0004) */
            if (d->audio_recv_thread) {
                if (d->audio_socket != INVALID_SOCKET) {
                    closesocket(d->audio_socket);
                    d->audio_socket = INVALID_SOCKET;
                }
                WaitForSingleObject(d->audio_recv_thread, 2000);
                CloseHandle(d->audio_recv_thread);
                d->audio_recv_thread = NULL;
            }

            if (d->gamepad_thread) {
                WaitForSingleObject(d->gamepad_thread, 1000);
                CloseHandle(d->gamepad_thread);
                d->gamepad_thread = NULL;
            }

            /* Wait briefly for recv thread to exit */
            if (d->recv_thread) {
                WaitForSingleObject(d->recv_thread, 2000);
                CloseHandle(d->recv_thread);
                d->recv_thread = NULL;
            }

            d->open = FALSE;

            /* Close the separate log window */
            if (d->log_hwnd && IsWindow(d->log_hwnd))
                DestroyWindow(d->log_hwnd);
            d->log_hwnd = NULL;
            d->log_list_hwnd = NULL;

            /* Clean up D3D11 */
            d3d_cleanup(d);

            if (d->custom_icon_big)   { DestroyIcon(d->custom_icon_big);   d->custom_icon_big = NULL; }
            if (d->custom_icon_small) { DestroyIcon(d->custom_icon_small); d->custom_icon_small = NULL; }

            /* Clean up guest cursor */
            if (d->guest_cursor) {
                DestroyCursor(d->guest_cursor);
                d->guest_cursor = NULL;
            }

            /* Notify main UI only if user closed the window */
            if (user_initiated && d->main_hwnd && d->vm_id)
                PostMessageW(d->main_hwnd, WM_VM_DISPLAY_CLOSED,
                             (WPARAM)d->token, (LPARAM)d->vm_id);
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_PRESENT);
        KillTimer(hwnd, IDT_TOOLBAR);
        if (d && d->toolbar_hwnd) {
            DestroyWindow(d->toolbar_hwnd);
            d->toolbar_hwnd = NULL;
        }
        if (d) idd_remove_kbd_hook(d);  /* safety net if WM_CLOSE was bypassed */
        if (d) {
            d->stop = TRUE;
            idd_update_relative_mouse(d);
        }
        if (d) d->hwnd = NULL;
        PostQuitMessage(0);
        return 0;

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        DWORD style   = (DWORD)GetWindowLongW(hwnd, GWL_STYLE);
        DWORD exstyle = (DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE);
        RECT wr;
        /* Minimum: 320x180 client area */
        wr.left = 0; wr.top = 0; wr.right = 320; wr.bottom = 180;
        AdjustWindowRectEx(&wr, style, FALSE, exstyle);
        mmi->ptMinTrackSize.x = wr.right - wr.left;
        mmi->ptMinTrackSize.y = wr.bottom - wr.top;
        /* Max: native frame size (adaptive mode resizes freely; the guest
           follows the window instead). */
        if (d && d->res_choice != RES_ADAPTIVE &&
            d->frame_width > 0 && d->frame_height > 0) {
            wr.left = 0; wr.top = 0;
            wr.right = (LONG)d->frame_width; wr.bottom = (LONG)d->frame_height;
            AdjustWindowRectEx(&wr, style, FALSE, exstyle);
            mmi->ptMaxTrackSize.x = wr.right - wr.left;
            mmi->ptMaxTrackSize.y = wr.bottom - wr.top;
        }
        return 0;
    }

    case WM_SIZE:
        if (d) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (d->render_hwnd)
                MoveWindow(d->render_hwnd, 0, 0, rc.right, rc.bottom, TRUE);
            d3d_resize_swap_chain(d);
            if (d->relative_mouse) {
                AcquireSRWLockExclusive(&g_mouse_capture_lock);
                if (g_mouse_capture_hwnd == hwnd) idd_clip_mouse(d);
                ReleaseSRWLockExclusive(&g_mouse_capture_lock);
            }
            idd_update_relative_mouse(d);
            /* Adaptive: ask the guest for the new size once resizing settles. */
            if (d->res_choice == RES_ADAPTIVE && wp != SIZE_MINIMIZED)
                SetTimer(hwnd, IDT_ADAPTIVE, ADAPTIVE_DEBOUNCE_MS, NULL);
            if (wp == SIZE_MINIMIZED) tb_hide(d);
            else tb_refresh(d);
        }
        return 0;

    case WM_MOVE:
        if (d) tb_refresh(d);
        if (d && d->relative_mouse) {
            AcquireSRWLockExclusive(&g_mouse_capture_lock);
            if (g_mouse_capture_hwnd == hwnd) idd_clip_mouse(d);
            ReleaseSRWLockExclusive(&g_mouse_capture_lock);
            idd_update_relative_mouse(d);
        }
        break;

    case WM_ENTERSIZEMOVE:
    case WM_EXITSIZEMOVE:
        if (d) {
            d->input_sizing = msg == WM_ENTERSIZEMOVE;
            idd_update_relative_mouse(d);
            if (msg == WM_EXITSIZEMOVE && d->res_choice == RES_ADAPTIVE)
                SetTimer(hwnd, IDT_ADAPTIVE, 100, NULL);
        }
        break;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        if (d) d3d_render_frame(d);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_TIMER:
        if (wp == IDT_PRESENT && d) {
            if (d->mouse_sync_pending) idd_poll_mouse_position(d);
            if (d->frame_dirty || d->anim_active)
                d3d_render_frame(d);
            idd_update_title(d, FALSE);
        } else if (wp == IDT_ADAPTIVE && d) {
            /* Still dragging: wait for the drag to end. */
            if (d->input_sizing) return 0;
            KillTimer(hwnd, IDT_ADAPTIVE);
            idd_apply_display_mode(d, FALSE);
        } else if (wp == IDT_TOOLBAR && d) {
            tb_on_timer(d);
        } else if (wp == IDT_APPLY_MODE && d) {
            KillTimer(hwnd, IDT_APPLY_MODE);
            idd_apply_display_mode(d, TRUE);
        }
        return 0;

    case WM_IDD_SET_STREAM:
        /* Quality / performance, from the menu or the management UI. */
        if (d) {
            d->stream_mode = ((int)wp == STREAM_PERFORMANCE) ? STREAM_PERFORMANCE : STREAM_QUALITY;
            d->vdec_unavailable = FALSE;      /* choosing it again retries */
            InterlockedExchange(&d->video_status_hr, 0);
            idd_update_display_menu(d);
            idd_save_settings(d);
            if (d->guest_driver >= 0 && d->guest_driver < VDD_PROTOCOL_VIDEO &&
                d->stream_mode == STREAM_PERFORMANCE)
                idd_log_main(d, L"Performance mode needs the updated guest display driver: "
                           L"update the VM's guest components.");
            idd_send_perf(d, PERF_FLAG_KEYFRAME);
            idd_update_title(d, TRUE);
            if (d->main_hwnd) PostMessageW(d->main_hwnd, WM_APP + 32, 0, 0);
        }
        return 0;

    case WM_IDD_SET_ICON:
        if (d) idd_apply_icon(d);
        return 0;

    case WM_IDD_SET_CODEC:
        if (d) {
            d->codec_choice = (int)wp;
            d->hevc_unavailable = FALSE;      /* choosing it again retries */
            idd_update_display_menu(d);
            idd_save_settings(d);
            idd_send_perf(d, PERF_FLAG_KEYFRAME);
            if (d->main_hwnd) PostMessageW(d->main_hwnd, WM_APP + 32, 0, 0);
        }
        return 0;

    case WM_IDD_SET_BITRATE:
        if (d) {
            d->bitrate_mbps = (UINT)wp;
            idd_save_settings(d);
            idd_send_perf(d, PERF_FLAG_KEYFRAME);
            if (d->main_hwnd) PostMessageW(d->main_hwnd, WM_APP + 32, 0, 0);
        }
        return 0;

    case WM_IDD_VIDEO_STATUS:
        if (d && wp == 1 && !asb_get_viewer_toolbar()) tb_hide(d);
        if (d) {
            idd_update_title(d, TRUE);
            if (d->main_hwnd) PostMessageW(d->main_hwnd, WM_APP + 32, 0, 0);
        }
        return 0;

    case WM_IDD_SET_MODE:
        /* From the management UI (vm_display_idd_set_mode). */
        if (d && d->mode_supported) {
            d->res_choice = (int)wp - 1;
            d->refresh_hz = (UINT)lp;
            idd_update_display_menu(d);
            idd_save_settings(d);
            idd_apply_display_mode(d, TRUE);
        }
        return 0;

    case WM_IDD_FRAME_CONNECTED:
        /* The guest driver (re)started, e.g. after a reboot or a monitor
           re-plug: give the input helper a moment, then re-send the mode. */
        if (d) SetTimer(hwnd, IDT_APPLY_MODE, 1500, NULL);
        return 0;

    case WM_IDD_FRAME_READY:
        if (d) d3d_render_frame(d);
        return 0;

    case WM_CLIPBOARDUPDATE:
        if (d && d->clipboard) {
            vm_clipboard_on_clipboard_update(d->clipboard);
        }
        return 0;

    case WM_CLIP_READER_APPLY:
        if (d && d->clipboard) {
            vm_clipboard_on_reader_apply(d->clipboard);
        }
        return 0;

    /* Posted by vm_display_idd_focus() from another thread to raise an
       already-open window. Runs on the window's own thread. Restores from
       minimized (the creation path never had to handle that) then brings
       the window forward; SetForegroundWindow succeeds because the user
       just clicked our foreground main window to trigger this. */
    case WM_IDD_FOCUS:
        if (IsIconic(hwnd))
            ShowWindow(hwnd, SW_RESTORE);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        return 0;

    case WM_IDD_INPUT_READY:
        if (d && !d->stop) {
            idd_cancel_mouse_sync(d);
            idd_update_kbd_hook(d);
            idd_update_relative_mouse(d);
        }
        return 0;

    case WM_IDD_CURSOR_CHANGED:
        if (d && d->render_hwnd) {
            idd_update_relative_mouse(d);
            POINT pt;
            if (GetCursorPos(&pt) && WindowFromPoint(pt) == d->render_hwnd)
                SendMessageW(d->render_hwnd, WM_SETCURSOR,
                             (WPARAM)d->render_hwnd,
                             MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
        }
        return 0;

    case WM_ENTERMENULOOP:
        if (d) {
            d->input_menu_active = TRUE;
            if (d->keyboard_version == INPUT_KEYBOARD_VERSION) idd_flush_held_keys(d);
            idd_flush_mouse_buttons(d);
            idd_update_relative_mouse(d);
        }
        break;

    case WM_EXITMENULOOP:
        if (d) {
            d->input_menu_active = FALSE;
            idd_update_relative_mouse(d);
        }
        break;

    case WM_SETFOCUS:
        if (d && d->clipboard)
            vm_clipboard_set_sync_enabled(d->clipboard, TRUE);
        break;

    case WM_KILLFOCUS:
        if (d && d->clipboard)
            vm_clipboard_set_sync_enabled(d->clipboard, FALSE);
        break;

    /* Top-level activation gates keyboard forwarding. Tracking activation
       (not WM_KILLFOCUS) is correct because focus moves between this window
       and its render child without losing activation. Losing activation —
       e.g. the user clicks another window or Alt+Tabs away with a key held —
       flushes held keys so nothing sticks down in the guest. */
    case WM_ACTIVATE:
        if (d) {
            d->input_focused = (LOWORD(wp) != WA_INACTIVE);
            if (!d->input_focused) {
                idd_flush_held_keys(d);
                idd_flush_mouse_buttons(d);
                /* The toolbar's own menus take activation briefly; hide only
                   when another app takes over. */
                if (!d->toolbar_pinned && (HWND)lp != d->toolbar_hwnd &&
                    GetForegroundWindow() != d->hwnd)
                    tb_hide(d);
            }
            idd_update_relative_mouse(d);
        }
        break;

    case WM_ERASEBKGND:
        return 1;  /* We handle all painting via D3D11 */

    /* ---- Mouse tracking (events forwarded from render child) ---- */
    case WM_INPUT:
        if (d && d->relative_mouse && d->input_focused &&
            d->frame_connected && !d->cursor_visible &&
            d->mouse_version == INPUT_MOUSE_VERSION &&
            GetForegroundWindow() == hwnd) {
            RAWINPUT raw = {0};
            UINT size = sizeof(raw);
            if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &raw, &size,
                                sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
                raw.header.dwType == RIM_TYPEMOUSE) {
                LONG dx = raw.data.mouse.lLastX, dy = raw.data.mouse.lLastY;
                if (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
                    BOOL virtual_desktop = (raw.data.mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
                    POINT pt = {
                        MulDiv(dx, GetSystemMetrics(virtual_desktop ? SM_CXVIRTUALSCREEN : SM_CXSCREEN), 65535),
                        MulDiv(dy, GetSystemMetrics(virtual_desktop ? SM_CYVIRTUALSCREEN : SM_CYSCREEN), 65535)
                    };
                    dx = dy = 0;
                    if (d->raw_absolute_valid && d->raw_absolute_device == raw.header.hDevice) {
                        dx = pt.x - d->raw_absolute_position.x;
                        dy = pt.y - d->raw_absolute_position.y;
                    }
                    d->raw_absolute_valid = TRUE;
                    d->raw_absolute_device = raw.header.hDevice;
                    d->raw_absolute_position = pt;
                } else {
                    d->raw_absolute_valid = FALSE;
                }
                if (dx || dy) {
                    send_input(d, INPUT_MOUSE_RELATIVE, (UINT32)dx, (UINT32)dy, 0);
                }
            }
        }
        break;

    case WM_MOUSEMOVE:
        if (d) {
            if (!d->tracking && d->render_hwnd) {
                TRACKMOUSEEVENT tme;
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = d->render_hwnd;
                tme.dwHoverTime = 0;
                TrackMouseEvent(&tme);
                d->tracking = TRUE;
            }
            d->mouse_in = TRUE;
            if (!d->relative_mouse && !d->cursor_visible)
                idd_update_relative_mouse(d);
            /* Pointer at the top edge: reveal the toolbar. */
            if (!d->relative_mouse && (int)(short)HIWORD(lp) <= tb_scale(d, TB_TRIGGER_PX))
                tb_show(d, 800);

            if (!d->relative_mouse && !d->mouse_sync_pending && d->frame_width && d->frame_height) {
                UINT vx, vy;
                window_to_vm_coords(d->render_hwnd,
                                    (int)(short)LOWORD(lp), (int)(short)HIWORD(lp),
                                    d->frame_width, d->frame_height, &vx, &vy);
                send_input(d, INPUT_MOUSE_MOVE, vx, vy, 0);
            }
        }
        return 0;

    case WM_MOUSELEAVE:
        if (d) {
            d->mouse_in = FALSE;
            d->tracking = FALSE;
            idd_update_relative_mouse(d);
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            if (d && (!d->cursor_visible || d->mouse_sync_pending))
                SetCursor(NULL);
            else if (d && d->guest_cursor)
                SetCursor(d->guest_cursor);
            else
                SetCursor(LoadCursorW(NULL, IDC_ARROW));
            return TRUE;
        }
        break;

    /* ---- Mouse button/wheel forwarding (only when cursor is in render area) ---- */
    case WM_LBUTTONDOWN:
        if (d && d->mouse_in) {
            if (d->render_hwnd) SetCapture(d->render_hwnd);
            send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_LEFT, 1, 0);
            d->mouse_buttons |= 1u << INPUT_BTN_LEFT;
        }
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        if (d && (d->mouse_in || (d->mouse_buttons & (1u << INPUT_BTN_LEFT)))) {
            send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_LEFT, 0, 0);
            d->mouse_buttons &= ~(1u << INPUT_BTN_LEFT);
        }
        return 0;

    case WM_RBUTTONDOWN:
        if (d && d->mouse_in) {
            send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_RIGHT, 1, 0);
            d->mouse_buttons |= 1u << INPUT_BTN_RIGHT;
        }
        return 0;
    case WM_RBUTTONUP:
        if (d && (d->mouse_in || (d->mouse_buttons & (1u << INPUT_BTN_RIGHT)))) {
            send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_RIGHT, 0, 0);
            d->mouse_buttons &= ~(1u << INPUT_BTN_RIGHT);
        }
        return 0;

    case WM_MBUTTONDOWN:
        if (d && d->mouse_in) {
            send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_MIDDLE, 1, 0);
            d->mouse_buttons |= 1u << INPUT_BTN_MIDDLE;
        }
        return 0;
    case WM_MBUTTONUP:
        if (d && (d->mouse_in || (d->mouse_buttons & (1u << INPUT_BTN_MIDDLE)))) {
            send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_MIDDLE, 0, 0);
            d->mouse_buttons &= ~(1u << INPUT_BTN_MIDDLE);
        }
        return 0;

    case WM_MOUSEWHEEL:
        if (d && d->mouse_in) send_input(d, INPUT_MOUSE_WHEEL, (UINT32)(INT32)GET_WHEEL_DELTA_WPARAM(wp), 0, 0);
        return 0;

    /* ---- Keyboard input forwarding (gated on window activation) ----
       In Transmit mode key events are captured by the low-level hook
       and never reach here. The mode check below covers Default mode: a
       reserved hotkey is neither forwarded to the guest nor consumed — it
       falls through to DefWindowProc so the host handles it normally (this
       is what avoids the old stuck-key bug). Normal keys are forwarded while
       the window is the active foreground window. */
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
    {
        UINT32 scan = (UINT32)((lp >> 16) & 0xFF);
        if (wp == VK_F11) {
            if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) &&
                !(lp & (1L << 30)) && d)
                idd_toggle_borderless_fullscreen(d, hwnd);
            return 0;  /* Never forward F11 to the guest. */
        }
        BOOL ext = (lp & (1 << 24)) != 0;
        BOOL up  = (msg == WM_KEYUP || msg == WM_SYSKEYUP);
        if (!d) break;
        if (!d->transmit_hotkeys &&
            idd_is_reserved_hotkey((DWORD)wp, (GetKeyState(VK_MENU) & 0x8000) != 0))
            break;  /* Default mode: let the host handle this hotkey. */
        if (d->kbd_hook && d->keyboard_version == INPUT_KEYBOARD_VERSION)
            return 0;  /* The hook already forwarded modifiers passed through to the host. */
        if (d->input_focused)
            idd_forward_key(d, (DWORD)wp, scan, ext, up);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ==================================================================
 * Public API
 * ================================================================== */

static volatile LONG64 g_next_display_token;
UINT_PTR vm_display_idd_token(VmDisplayIdd *display) { return display ? display->token : 0; }

VmDisplayIdd *vm_display_idd_create(VmInstance *vm, HINSTANCE hInstance, HWND main_hwnd)
{
    VmDisplayIdd *d;
    BOOL h2g = TRUE, g2h = TRUE;

    if (!vm) return NULL;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    d = (VmDisplayIdd *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                   sizeof(VmDisplayIdd));
    if (!d) return NULL;

    d->vm_id        = vm->unique_id;
    d->token        = ((UINT_PTR)InterlockedIncrement64(&g_next_display_token) << 1) | 1;
    wcscpy_s(d->vm_name, 256, vm->name);
    wcscpy_s(d->vhdx_path, MAX_PATH, vm->vhdx_path);
    d->runtime_id   = vm->runtime_id;
    wcscpy_s(d->os_type, 32, vm->os_type);
    d->hInstance    = hInstance;
    d->main_hwnd   = main_hwnd;
    d->open         = TRUE;
    d->stop         = FALSE;
    InitializeSRWLock(&d->input_lock);
    d->keyboard_version   = 1;
    d->input_socket       = INVALID_SOCKET;
    d->audio_socket       = INVALID_SOCKET;
    d->clipboard          = NULL;
    d->cursor_visible     = TRUE;

    /* Load the per-VM display settings, creating display_settings.json with
       the defaults (hotkeys off, clipboard sharing on) if this VM doesn't
       have one yet. The hook itself is installed later, on the window thread,
       once the window exists. */
    d->transmit_hotkeys = idd_display_settings_load_or_create(vm->vhdx_path, &h2g, &g2h,
                                                              &d->res_choice, &d->refresh_hz,
                                                              &d->stream_mode, &d->codec_choice);
    d->bitrate_mbps = idd_display_settings_bitrate(vm->vhdx_path);
    d->clip_share_host_to_guest = h2g;
    d->clip_share_guest_to_host = g2h;
    d->guest_linux = (_wcsicmp(vm->os_type, L"Linux") == 0);
    d->mode_supported = (_wcsicmp(vm->os_type, L"Windows") == 0) || d->guest_linux;
    d->frame_socket = INVALID_SOCKET;
    InitializeSRWLock(&d->frame_send_lock);
    d->guest_driver = -1;

    /* Initialize frame buffer at default resolution */
    d->frame_width  = DEFAULT_WIDTH;
    d->frame_height = DEFAULT_HEIGHT;
    d->frame_stride = DEFAULT_WIDTH * 4;
    d->frame_buf    = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                         d->frame_stride * d->frame_height);
    if (!d->frame_buf) {
        HeapFree(GetProcessHeap(), 0, d);
        return NULL;
    }

    InitializeCriticalSection(&d->frame_cs);

    /* Start the window thread (which will then start the recv thread) */
    d->window_thread = CreateThread(NULL, 0, idd_window_thread_proc, d, 0, NULL);
    if (!d->window_thread) {
        ui_log(L"IDD: Failed to create window thread.");
        DeleteCriticalSection(&d->frame_cs);
        HeapFree(GetProcessHeap(), 0, d->frame_buf);
        HeapFree(GetProcessHeap(), 0, d);
        return NULL;
    }

    return d;
}

void vm_display_idd_destroy(VmDisplayIdd *display)
{
    BOOL quit_requested = FALSE;
    int quit_code = 0;
    if (!display) return;

    /* Signal stop */
    display->stop = TRUE;
    display->open = FALSE;

    /* Close the window to unblock the message pump */
    if (display->hwnd && IsWindow(display->hwnd))
        PostMessageW(display->hwnd, WM_CLOSE, 0, 0);

    /* Wait for window thread (pumping messages to stay responsive) */
    if (display->window_thread) {
        DWORD result;
        do {
            result = MsgWaitForMultipleObjects(
                1, &display->window_thread, FALSE, 5000, QS_ALLINPUT);
            if (result == WAIT_OBJECT_0 + 1) {
                MSG msg;
                while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT) {
                        quit_requested = TRUE;
                        quit_code = (int)msg.wParam;
                        continue;
                    }
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
        } while (result == WAIT_OBJECT_0 + 1);
        CloseHandle(display->window_thread);
    }

    /* recv_thread is cleaned up by WM_CLOSE handler, but guard just in case */
    if (display->recv_thread) {
        WaitForSingleObject(display->recv_thread, 3000);
        CloseHandle(display->recv_thread);
    }

    /* Clipboard is cleaned up by WM_CLOSE handler, but guard */
    if (display->clipboard) {
        vm_clipboard_destroy(display->clipboard);
        display->clipboard = NULL;
    }

    /* audio_recv_thread guard */
    if (display->audio_recv_thread) {
        if (display->audio_socket != INVALID_SOCKET) {
            closesocket(display->audio_socket);
            display->audio_socket = INVALID_SOCKET;
        }
        WaitForSingleObject(display->audio_recv_thread, 3000);
        CloseHandle(display->audio_recv_thread);
    }

    DeleteCriticalSection(&display->frame_cs);
    if (display->vq_buf) HeapFree(GetProcessHeap(), 0, display->vq_buf);
    if (display->vq_work) HeapFree(GetProcessHeap(), 0, display->vq_work);

    if (display->clipboard) {
        vm_clipboard_destroy(display->clipboard);
        display->clipboard = NULL;
    }

    if (display->frame_buf)
        HeapFree(GetProcessHeap(), 0, display->frame_buf);

    HeapFree(GetProcessHeap(), 0, display);
    if (quit_requested) PostQuitMessage(quit_code);
}

BOOL vm_display_idd_is_active(VmDisplayIdd *display)
{
    return display && display->open && !display->stop;
}

BOOL vm_display_idd_is_open(VmDisplayIdd *display)
{
    if (!display) return FALSE;
    return display->open && display->hwnd && IsWindow(display->hwnd);
}

void vm_display_idd_focus(VmDisplayIdd *display)
{
    if (!display || !display->open ||
        !display->hwnd || !IsWindow(display->hwnd))
        return;
    /* Marshal to the window thread; that thread owns the window and runs
       the activation (restore-if-minimized + foreground) in WM_IDD_FOCUS. */
    PostMessageW(display->hwnd, WM_IDD_FOCUS, 0, 0);
}
