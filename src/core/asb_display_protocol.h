/*
 * asb_display_protocol.h -- the display wire protocol between the guest's
 * App Sandbox display driver (tools/vdd) and the host viewer
 * (src/backend_win/vm_display_idd.c). One definition for both sides.
 *
 * Compatibility rules (see AGENTS.md):
 *  - Every message is little-endian and starts with a 32-bit magic.
 *  - New messages need a VDD_PROTOCOL_VERSION bump; the host gates them on
 *    the version the driver reports in VDD_WIRE_HELLO. Old drivers never
 *    answer the hello query; old hosts never send it.
 *  - Never change the layout of an existing message.
 *
 * Needs <windows.h> types (UINT32, INT32, UINT64) included first.
 */
#ifndef ASB_DISPLAY_PROTOCOL_H
#define ASB_DISPLAY_PROTOCOL_H

/* ============================================================================
 *  Wire protocol: ASFR frame header (sent over HvSocket to host)
 * ============================================================================ */
#define VDD_FRAME_MAGIC     0x52465341  /* "ASFR" little-endian */

#pragma pack(push, 1)
typedef struct _VDD_WIRE_FRAME_HEADER {
    UINT32  magic;              /* VDD_FRAME_MAGIC */
    UINT32  width;
    UINT32  height;
    UINT32  stride;
    UINT64  frame_seq;
    UINT32  dirty_rect_count;   /* 0 = full frame */
} VDD_WIRE_FRAME_HEADER;

/* Wire protocol: ASCR cursor update (sent over HvSocket to host) */
#define VDD_CURSOR_MAGIC    0x52435341  /* "ASCR" little-endian */

typedef struct _VDD_WIRE_CURSOR_HEADER {
    UINT32  magic;              /* VDD_CURSOR_MAGIC */
    INT32   x;                  /* cursor position X (can be negative) */
    INT32   y;                  /* cursor position Y (can be negative) */
    UINT32  visible;            /* 0 = hidden, 1 = visible */
    UINT32  shape_updated;      /* 0 = position only, 1 = shape data follows */
    UINT32  shape_id;           /* monotonic shape identifier */
    UINT32  width;              /* cursor width in pixels */
    UINT32  height;             /* cursor height in pixels */
    UINT32  pitch;              /* row pitch in bytes */
    UINT32  xhot;               /* hotspot X */
    UINT32  yhot;               /* hotspot Y */
    UINT32  cursor_type;        /* 1=MASKED_COLOR, 2=ALPHA */
    UINT32  shape_data_size;    /* bytes of shape data following header (0 if !shape_updated) */
} VDD_WIRE_CURSOR_HEADER;
/* Wire protocol: ASMR mode request (host -> guest on the same frame channel).
 * The host asks for a resolution/refresh. If the mode is not in the list yet
 * the driver adds it and re-plugs the virtual monitor; the guest agent then
 * applies the mode in the interactive session (set_display command). */
#define VDD_MODE_REQUEST_MAGIC 0x524D5341  /* "ASMR" little-endian */

/* Wire protocol: hello. The host asks with an ASHQ message (same 16-byte
 * shape as a mode request) and this driver answers ASVH. Older drivers never
 * read the channel, so a host that gets frames but no answer knows the guest
 * cannot change resolution; older hosts never ask, so they never see ASVH. */
#define VDD_HELLO_QUERY_MAGIC 0x51485341  /* "ASHQ" little-endian */
#define VDD_HELLO_MAGIC     0x48565341  /* "ASVH" little-endian */
/* 2: mode requests. 3: performance mode (GPU-encoded video, ASPF/ASPS/ASVE).
   4: HEVC (ASPF mode 2). */
#define VDD_PROTOCOL_VERSION 4

typedef struct _VDD_WIRE_HELLO {
    UINT32  magic;              /* VDD_HELLO_MAGIC */
    UINT32  version;            /* VDD_PROTOCOL_VERSION */
    UINT32  reserved[2];
} VDD_WIRE_HELLO;

typedef struct _VDD_WIRE_MODE_REQUEST {
    UINT32  magic;              /* VDD_MODE_REQUEST_MAGIC */
    UINT32  width;
    UINT32  height;
    UINT32  refresh;            /* Hz */
} VDD_WIRE_MODE_REQUEST;

/* Wire protocol: performance mode (protocol 3).
 * Host -> guest ASPF (16 bytes, same shape as a mode request):
 *   { magic, mode (0 = raw frames, 1 = H.264 video, 2 = HEVC video), kbps (0 = auto), flags }
 *   flags bit 0 asks for a key frame (the host decoder restarted).
 * Guest -> host ASPS: { magic, mode in use, codec, HRESULT of the last start }.
 * Guest -> host ASVE: header + `size` bytes of one Annex-B access unit. */
#define VDD_PERF_REQUEST_MAGIC  0x46505341  /* "ASPF" little-endian */
#define VDD_PERF_STATUS_MAGIC   0x53505341  /* "ASPS" little-endian */
#define VDD_VIDEO_MAGIC         0x45565341  /* "ASVE" little-endian */
#define VDD_PERF_FLAG_KEYFRAME  0x1
#define VDD_CODEC_H264          1
#define VDD_CODEC_HEVC          2
#define VDD_VIDEO_FLAG_KEY      0x1

typedef struct _VDD_WIRE_PERF_STATUS {
    UINT32  magic;              /* VDD_PERF_STATUS_MAGIC */
    UINT32  mode;               /* 0 raw, 1 video */
    UINT32  codec;              /* VDD_CODEC_H264 when mode == 1 */
    INT32   hresult;            /* why video could not start (mode 0) */
} VDD_WIRE_PERF_STATUS;

typedef struct _VDD_WIRE_VIDEO_HEADER {
    UINT32  magic;              /* VDD_VIDEO_MAGIC */
    UINT32  width;
    UINT32  height;
    UINT32  codec;              /* VDD_CODEC_H264 */
    UINT32  flags;              /* VDD_VIDEO_FLAG_KEY */
    UINT32  size;               /* bytes of bitstream that follow */
    UINT64  seq;
} VDD_WIRE_VIDEO_HEADER;
#pragma pack(pop)

/* Layout checks: these sizes are part of the protocol. */
#ifdef __cplusplus
static_assert(sizeof(VDD_WIRE_FRAME_HEADER) == 28, "ASFR header size");
static_assert(sizeof(VDD_WIRE_CURSOR_HEADER) == 52, "ASCR header size");
static_assert(sizeof(VDD_WIRE_HELLO) == 16, "ASVH size");
static_assert(sizeof(VDD_WIRE_MODE_REQUEST) == 16, "ASMR size");
static_assert(sizeof(VDD_WIRE_PERF_STATUS) == 16, "ASPS size");
static_assert(sizeof(VDD_WIRE_VIDEO_HEADER) == 32, "ASVE header size");
#else
typedef char asb_check_asfr[sizeof(VDD_WIRE_FRAME_HEADER) == 28 ? 1 : -1];
typedef char asb_check_ascr[sizeof(VDD_WIRE_CURSOR_HEADER) == 52 ? 1 : -1];
typedef char asb_check_asvh[sizeof(VDD_WIRE_HELLO) == 16 ? 1 : -1];
typedef char asb_check_asmr[sizeof(VDD_WIRE_MODE_REQUEST) == 16 ? 1 : -1];
typedef char asb_check_asps[sizeof(VDD_WIRE_PERF_STATUS) == 16 ? 1 : -1];
typedef char asb_check_asve[sizeof(VDD_WIRE_VIDEO_HEADER) == 32 ? 1 : -1];
#endif

#endif /* ASB_DISPLAY_PROTOCOL_H */
