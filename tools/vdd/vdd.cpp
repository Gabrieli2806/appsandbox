/*++

Module Name:

    vdd.cpp

Abstract:

    AppSandbox Virtual Display Driver - Single 1920x1080 monitor with
    direct HvSocket frame transport to the host (no agent middleman).

    IddCx lifecycle:
    - DllMain
    - DriverEntry with WDF_OBJECT_ATTRIBUTES_INIT + EvtDriverUnload
    - DeviceAdd with PnP callbacks, IddCx config, EvtCleanupCallback,
      heap-allocated context (IndirectDeviceContextWrapper)
    - D0Entry -> InitAdapter (context type on adapter object)
    - AdapterInitFinished -> FinishInit -> CreateMonitor
    - ParseMonitorDescription, GetDefaultModes, QueryTargetModes
    - AssignSwapChain with direct HvSocket frame transport to host
    - UnassignSwapChain with cleanup

Environment:

    Windows User-Mode Driver Framework 2

--*/

#include "vdd.h"

#include <stdlib.h>  /* malloc, free */
#include <stdarg.h>  /* va_list, va_start, va_end */
#include <stdio.h>   /* _snprintf, _vsnprintf */
#include <cfgmgr32.h> /* CM_Locate_DevNode, CM_Disable_DevNode */
#include <d3dkmthk.h>  /* D3DKMTSetProcessSchedulingPriorityClass */
#include <setupapi.h>  /* SetupDiGetClassDevs, SetupDiEnumDeviceInfo */
#include "../transport/asb_transport.h"  /* ivshmem frame channel (Windows-VMs-on-Mac path only) */
#include "vdd_encoder.h"  /* performance mode: hardware H.264 */
#include <objbase.h>     /* CoInitializeEx */

/* ========================================================================= */
/*  File-based logging (persists across crashes, no debugger needed)         */
/* ========================================================================= */

#define VDD_LOG_PATH L"C:\\ProgramData\\AppSandboxVDD.log"

static void VddLog(const char* fmt, ...)
{
    HANDLE hFile = CreateFileW(VDD_LOG_PATH, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return;

    /* Timestamp */
    SYSTEMTIME sysTime;
    GetLocalTime(&sysTime);
    char buf[2048];
    int pos = sprintf_s(buf, sizeof(buf), "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
                        sysTime.wYear, sysTime.wMonth, sysTime.wDay,
                        sysTime.wHour, sysTime.wMinute, sysTime.wSecond, sysTime.wMilliseconds);
    if (pos < 0) pos = 0;

    /* Format message */
    va_list args;
    va_start(args, fmt);
    int added = vsprintf_s(buf + pos, sizeof(buf) - (size_t)pos - 4, fmt, args);
    va_end(args);
    if (added > 0) pos += added;

    buf[pos++] = '\r';
    buf[pos++] = '\n';

    DWORD written;
    WriteFile(hFile, buf, (DWORD)pos, &written, NULL);
    FlushFileBuffers(hFile);
    CloseHandle(hFile);

    /* Also send to debug output for good measure */
    buf[pos - 2] = '\n';
    buf[pos - 1] = '\0';
    OutputDebugStringA(buf);
}

/* ========================================================================= */
/*  EDID with computed checksum                                              */
/* ========================================================================= */

static const BYTE* VddGetEdid()
{
    static BYTE edid[128];
    static BOOL computed = FALSE;

    if (!computed)
    {
        memcpy(edid, VDD_EDID, 127);
        BYTE sum = 0;
        for (int i = 0; i < 127; i++)
            sum += edid[i];
        edid[127] = (BYTE)(256 - sum);
        computed = TRUE;
    }
    return edid;
}

/* ========================================================================= */
/*  Monitor mode helper                                                      */
/* ========================================================================= */

static void VddCreateMonitorMode(DISPLAYCONFIG_VIDEO_SIGNAL_INFO* sig, const VDD_MODE* m,
                                 UINT vSyncDivider)
{
    /* totalSize == activeSize, pixelRate = refresh * w * h.
       IddCx validates these relationships; using actual blanking values causes
       STATUS_INVALID_PARAMETER from IddCxMonitorArrival. */
    memset(sig, 0, sizeof(*sig));
    sig->totalSize.cx                           = m->width;
    sig->totalSize.cy                           = m->height;
    sig->activeSize.cx                          = m->width;
    sig->activeSize.cy                          = m->height;
    sig->AdditionalSignalInfo.vSyncFreqDivider  = vSyncDivider;
    sig->AdditionalSignalInfo.videoStandard     = 255;
    sig->vSyncFreq.Numerator                    = m->refresh;
    sig->vSyncFreq.Denominator                  = 1;
    sig->hSyncFreq.Numerator                    = m->refresh * m->height;
    sig->hSyncFreq.Denominator                  = 1;
    sig->scanLineOrdering                       = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    sig->pixelRate                               = (UINT64)m->refresh * m->width * m->height;
}

/* ========================================================================= */
/*  Mode list                                                                */
/*                                                                           */
/*  The OS offers the intersection of monitor modes (ParseMonitorDescription) */
/*  and target modes (QueryTargetModes); both return the same list. Presets   */
/*  at 60/120/144 Hz are always there. A custom size requested by the host is */
/*  added to a small ring and the monitor is re-plugged so the OS re-reads   */
/*  the list; the guest agent then switches to it.                           */
/* ========================================================================= */

/* ParseMonitorDescription has no monitor/adapter argument; the driver supports
   one adapter with one monitor, so it reads the list through this pointer. */
static VDD_DEVICE_CONTEXT* volatile g_pVddContext = nullptr;

static BOOL VddModeEqual(const VDD_MODE* a, const VDD_MODE* b)
{
    return a->width == b->width && a->height == b->height && a->refresh == b->refresh;
}

/* Rebuild ctx->modes from presets + custom ring. Caller holds modeLock
   exclusively. Keeps the preferred mode pointing at the same mode if present. */
static void VddRebuildModeListLocked(VDD_DEVICE_CONTEXT* ctx, const VDD_MODE* preferred)
{
    UINT n = 0;
    VDD_MODE def = { VDD_WIDTH, VDD_HEIGHT, VDD_REFRESH };

    if (ctx->legacySingleMode) {
        ctx->modes[0] = def;
        ctx->modeCount = 1;
        ctx->preferredMode = 0;
        return;
    }

    for (UINT r = 0; r < g_NumRefreshRates; r++) {
        for (UINT i = 0; i < g_NumResolutions && n < VDD_MAX_MODES; i++) {
            VDD_MODE m = { g_SupportedResolutions[i].width, g_SupportedResolutions[i].height,
                           g_SupportedRefreshRates[r].numerator };
            ctx->modes[n++] = m;
        }
    }
    for (UINT c = 0; c < ctx->customCount && n < VDD_MAX_MODES; c++) {
        BOOL dup = FALSE;
        for (UINT k = 0; k < n; k++)
            if (VddModeEqual(&ctx->modes[k], &ctx->customModes[c])) { dup = TRUE; break; }
        if (!dup) ctx->modes[n++] = ctx->customModes[c];
    }
    ctx->modeCount = n;

    ctx->preferredMode = 0;
    if (!preferred) preferred = &def;
    for (UINT k = 0; k < n; k++) {
        if (VddModeEqual(&ctx->modes[k], preferred)) { ctx->preferredMode = k; break; }
        if (VddModeEqual(&ctx->modes[k], &def)) ctx->preferredMode = k;
    }
}

/* Copy the current list out under the shared lock. Returns the count. */
static UINT VddSnapshotModes(VDD_DEVICE_CONTEXT* ctx, VDD_MODE* out, UINT* preferred)
{
    UINT n;
    AcquireSRWLockShared(&ctx->modeLock);
    n = ctx->modeCount;
    memcpy(out, ctx->modes, n * sizeof(VDD_MODE));
    if (preferred) *preferred = ctx->preferredMode;
    ReleaseSRWLockShared(&ctx->modeLock);
    return n;
}

/* Fallback when no context exists yet (should not happen in practice). */
static UINT VddDefaultModes(VDD_MODE* out, UINT* preferred)
{
    VDD_MODE def = { VDD_WIDTH, VDD_HEIGHT, VDD_REFRESH };
    out[0] = def;
    if (preferred) *preferred = 0;
    return 1;
}

static UINT VddCurrentModes(VDD_DEVICE_CONTEXT* ctx, VDD_MODE* out, UINT* preferred)
{
    if (!ctx) ctx = g_pVddContext;
    return ctx ? VddSnapshotModes(ctx, out, preferred) : VddDefaultModes(out, preferred);
}

/* Handle a mode request from the host. Validates and normalises it; if the
   mode is not offered yet, adds it to the custom ring and schedules a monitor
   re-plug. Safe to call from any thread. */
static void VddRequestMode(VDD_DEVICE_CONTEXT* ctx, UINT width, UINT height, UINT refresh)
{
    VDD_MODE m;
    BOOL known = FALSE, replug = FALSE;

    if (!ctx || ctx->legacySingleMode) return;

    /* Width a multiple of 8 and height even keep every scan-out path happy. */
    width &= ~7u;
    height &= ~1u;
    if (width < VDD_MIN_WIDTH || height < VDD_MIN_HEIGHT ||
        width > VDD_MAX_WIDTH || height > VDD_MAX_HEIGHT ||
        refresh < VDD_MIN_REFRESH || refresh > VDD_MAX_REFRESH) {
        VddLog("ModeRequest: rejected %ux%u@%u (out of range)", width, height, refresh);
        return;
    }
    m.width = width; m.height = height; m.refresh = refresh;

    AcquireSRWLockExclusive(&ctx->modeLock);
    for (UINT k = 0; k < ctx->modeCount; k++)
        if (VddModeEqual(&ctx->modes[k], &m)) { known = TRUE; ctx->preferredMode = k; break; }
    if (!known) {
        ctx->customModes[ctx->customNext] = m;
        ctx->customNext = (ctx->customNext + 1) % VDD_MAX_CUSTOM_MODES;
        if (ctx->customCount < VDD_MAX_CUSTOM_MODES) ctx->customCount++;
        VddRebuildModeListLocked(ctx, &m);
        replug = TRUE;
    }
    ReleaseSRWLockExclusive(&ctx->modeLock);

    ctx->lastRefresh = refresh;
    VddLog("ModeRequest: %ux%u@%u (%s)", width, height, refresh,
           known ? "already offered" : "added");
    if (replug && ctx->hModeTimer) {
        ctx->pendingMode = m;
        InterlockedExchange(&ctx->modeStage, 1);
        WdfTimerStart(ctx->hModeTimer, WDF_REL_TIMEOUT_IN_MS(30));
    }
}

/* ========================================================================= */
/*  Host frame transport                                                     */
/*                                                                           */
/*  The frame channel (listen/accept/send on the ch2 service a5b0cafe-0002)  */
/*  goes through asb_transport: asb_listen(ASB_CH_DISPLAY)/asb_accept/       */
/*  asb_send resolve to AF_HYPERV HvSocket on a Windows host and to the      */
/*  ivshmem ch2 stream slot on a macOS host.                                 */
/* ========================================================================= */

/* Send exactly len bytes over the transport. Returns 0 on success, -1 on failure
   (asb_send loops until all bytes are sent on both backends). */
static int VddSendAll(AsbConn* c, const char* data, int len)
{
    return asb_send(c, data, len) == len ? 0 : -1;
}

/* Close a client connection and clear *pc. PC: graceful shutdown(SD_BOTH) then closesocket; ivshmem:
   asb_close marks the slot CLOSING. */
static void VddCloseConn(AsbConn* volatile* pc)
{
    AsbConn* c = *pc;
    if (!c) return;
    {
        unsigned long long s = asb_conn_socket_u64(c);
        if (s != ~0ull) shutdown((SOCKET)s, SD_BOTH);   /* PC only; ivshmem returns ~0 */
    }
    asb_close(c);
    *pc = NULL;
}

/* ========================================================================= */
/*  Network listener thread (accepts host connections over HvSocket)         */
/* ========================================================================= */

/* One-shot transport backend selection (PC AF_HYPERV vs Mac ivshmem). Done off the loader lock
   (in the network thread, never in DllMain) since the ivshmem probe enumerates device interfaces. */
static volatile LONG g_transportInited = 0;
static void VddEnsureTransportInit(void)
{
    if (InterlockedCompareExchange(&g_transportInited, 1, 0) == 0)
        asb_transport_init();   /* selects ivshmem if the host published the directory, else AF_HYPERV */
}

static DWORD WINAPI VddNetworkThread(LPVOID lpParameter)
{
    VDD_SWAP_PROC* proc = (VDD_SWAP_PROC*)lpParameter;
    AsbListener* l = NULL;

    VddLog("Network: starting");
    VddEnsureTransportInit();

    while (!proc->bStopNetwork) {
        /* (Re)create the listener. PC: socket(AF_HYPERV)+bind(ch2 GUID, wildcard)+listen;
           Mac: bind the ivshmem ch2 slot. */
        l = asb_listen(ASB_CH_DISPLAY);
        if (!l) {
            VddLog("Network: asb_listen(ch2) failed, retrying in 3s (transport=%s)",
                   asb_transport_is_ivshmem() ? "ivshmem" : "hyperv");
            for (int w = 0; w < 3000 && !proc->bStopNetwork; w += 500)
                Sleep(500);
            continue;
        }

        VddLog("Network: listening for host connections (transport=%s)",
               asb_transport_is_ivshmem() ? "ivshmem" : "hyperv");

        /* Accept loop — 1s timeout so we re-check bStopNetwork between accepts. */
        while (!proc->bStopNetwork) {
            AsbConn* nc = asb_accept(l, 1000);
            if (!nc)
                continue;

            VddLog("Network: host connected");

            /* If a previous pending connection wasn't picked up yet, close it. */
            {
                AsbConn* old = (AsbConn*)InterlockedExchangePointer(
                    (PVOID volatile*)&proc->hPendingConn, nc);
                if (old) {
                    VddCloseConn(&old);
                    VddLog("Network: replaced unclaimed pending connection");
                }
            }
        } /* end accept loop */

        asb_close_listener(l);
        l = NULL;
    } /* end retry loop */

    /* Close any pending connection. */
    {
        AsbConn* pending = (AsbConn*)InterlockedExchangePointer(
            (PVOID volatile*)&proc->hPendingConn, NULL);
        if (pending) VddCloseConn(&pending);
    }

    if (l) asb_close_listener(l);

    VddLog("Network: stopped");
    return 0;
}

/* ========================================================================= */
/*  D3D11 device — cached in VDD_DEVICE_CONTEXT across swap chain transitions
 *  to avoid destroying the D3D device during IddCx's WdfObjectDelete
 *  teardown (which crashes WUDFHost.exe).                                    */
/* ========================================================================= */

static HRESULT VddGetOrCreateD3DDevice(VDD_DEVICE_CONTEXT* ctx, LUID adapterLuid,
                                        ID3D11Device** ppDevice, ID3D11DeviceContext** ppCtx)
{
    /* Check if cached device matches the requested LUID and is still healthy */
    if (ctx->pCachedDevice &&
        ctx->cachedDeviceLuid.HighPart == adapterLuid.HighPart &&
        ctx->cachedDeviceLuid.LowPart == adapterLuid.LowPart)
    {
        HRESULT reason = ctx->pCachedDevice->GetDeviceRemovedReason();
        if (reason == S_OK)
        {
            VddLog("GetOrCreateD3DDevice: reusing cached device (LUID=%08X:%08X)",
                   adapterLuid.HighPart, adapterLuid.LowPart);
            *ppDevice = ctx->pCachedDevice;
            *ppCtx = ctx->pCachedCtx;
            return S_OK;
        }
        VddLog("GetOrCreateD3DDevice: cached device removed (reason=0x%08X), recreating",
               reason);
        ctx->pCachedCtx->Release();
        ctx->pCachedDevice->Release();
        ctx->pCachedDevice = nullptr;
        ctx->pCachedCtx = nullptr;
    }

    /* Release any stale cached device with a different LUID */
    if (ctx->pCachedDevice)
    {
        VddLog("GetOrCreateD3DDevice: releasing stale cached device (LUID mismatch)");
        ctx->pCachedCtx->Release();
        ctx->pCachedDevice->Release();
        ctx->pCachedDevice = nullptr;
        ctx->pCachedCtx = nullptr;
    }

    VddLog("GetOrCreateD3DDevice: creating new device (LUID=%08X:%08X)",
           adapterLuid.HighPart, adapterLuid.LowPart);

    IDXGIFactory5* pFactory = nullptr;
    HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory5), (void**)&pFactory);
    if (FAILED(hr))
    {
        VddLog("GetOrCreateD3DDevice: CreateDXGIFactory2 FAILED hr=0x%08X", hr);
        return hr;
    }

    IDXGIAdapter1* pAdapter = nullptr;
    hr = pFactory->EnumAdapterByLuid(adapterLuid, __uuidof(IDXGIAdapter1), (void**)&pAdapter);
    pFactory->Release();

    if (FAILED(hr))
    {
        VddLog("GetOrCreateD3DDevice: EnumAdapterByLuid FAILED hr=0x%08X", hr);
        return hr;
    }

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    ID3D11Device* pDevice = nullptr;
    ID3D11DeviceContext* pDevCtx = nullptr;

    /* Video support lets the performance mode convert and encode frames on
       this device; adapters without it still get a plain device. */
    hr = D3D11CreateDevice(
        pAdapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        levels, ARRAYSIZE(levels),
        D3D11_SDK_VERSION,
        &pDevice,
        nullptr,
        &pDevCtx);
    if (FAILED(hr)) {
        VddLog("GetOrCreateD3DDevice: no video support on this adapter (0x%08X)", hr);
        hr = D3D11CreateDevice(
            pAdapter,
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, ARRAYSIZE(levels),
            D3D11_SDK_VERSION,
            &pDevice,
            nullptr,
            &pDevCtx);
    }

    pAdapter->Release();

    if (FAILED(hr))
    {
        VddLog("GetOrCreateD3DDevice: D3D11CreateDevice FAILED hr=0x%08X", hr);
        return hr;
    }

    VddLog("GetOrCreateD3DDevice: D3D11 device created successfully");

    /* Cache in device context */
    ctx->pCachedDevice = pDevice;
    ctx->pCachedCtx = pDevCtx;
    ctx->cachedDeviceLuid = adapterLuid;

    *ppDevice = pDevice;
    *ppCtx = pDevCtx;
    return S_OK;
}

static HRESULT VddCreateStagingTexture(VDD_SWAP_PROC* proc, UINT width, UINT height)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width              = width;
    desc.Height             = height;
    desc.MipLevels          = 1;
    desc.ArraySize          = 1;
    desc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count   = 1;
    desc.Usage              = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;

    return proc->pDevice->CreateTexture2D(&desc, nullptr, &proc->pStagingTex);
}

/* ========================================================================= */
/*  Swap chain processor thread                                              */
/* ========================================================================= */

static void VddSwapChainRunCore(VDD_SWAP_PROC* proc);

static DWORD WINAPI VddSwapChainThread(LPVOID lpParameter)
{
    VDD_SWAP_PROC* proc = (VDD_SWAP_PROC*)lpParameter;
    DWORD taskIndex = 0;
    HANDLE hAvrt;
    IDXGIDevice* pDxgiDevice = NULL;
    HRESULT hr;

    hAvrt = AvSetMmThreadCharacteristicsW(L"Distribution", &taskIndex);

    VddLog("SwapChain thread started");

    /* Raise GPU priority (IddCx 1.9+) */
#pragma warning(suppress: 4127)
    if (IDD_IS_FUNCTION_AVAILABLE(IddCxSetRealtimeGPUPriority) && proc->pDevice) {
        hr = proc->pDevice->QueryInterface(IID_PPV_ARGS(&pDxgiDevice));
        if (SUCCEEDED(hr)) {
            IDARG_IN_SETREALTIMEGPUPRIORITY priorityArgs = { 0 };
            priorityArgs.pDevice = pDxgiDevice;
            IddCxSetRealtimeGPUPriority(proc->hSwapChain, &priorityArgs);
            pDxgiDevice->Release();
            pDxgiDevice = NULL;
        }
    }

    /* COM for the performance-mode encoder (Media Foundation). */
    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    VddSwapChainRunCore(proc);

    if (SUCCEEDED(hrCom))
        CoUninitialize();

    /* Always delete the swap chain when the processing loop terminates, to
       kick the system to provide a new swap chain if necessary.  The D3D
       device is NOT released here — it's cached in VDD_DEVICE_CONTEXT to
       survive IddCx teardown. */
    if (proc->hSwapChain) {
        VddLog("SwapChain thread: deleting swap chain (WdfObjectDelete)");
        WdfObjectDelete((WDFOBJECT)proc->hSwapChain);
        proc->hSwapChain = NULL;
    }

    if (hAvrt)
        AvRevertMmThreadCharacteristics(hAvrt);

    VddLog("SwapChain thread exiting");
    return 0;
}

/* ========================================================================= */
/*  Sending to the host                                                      */
/* ========================================================================= */

/* One whole message (optionally in two parts) on the client connection,
   never interleaved with another thread's message. 0 on success. */
static int VddSendMsg(VDD_SWAP_PROC* proc, const void* a, int alen, const void* b, int blen)
{
    int r = -1;
    EnterCriticalSection(&proc->sendLock);
    AsbConn* c = proc->hClientConn;
    if (c) {
        r = VddSendAll(c, (const char*)a, alen);
        if (r == 0 && b && blen > 0) r = VddSendAll(c, (const char*)b, blen);
    }
    LeaveCriticalSection(&proc->sendLock);
    return r;
}

/* Close the client connection (waits for a message in flight). */
static void VddCloseClient(VDD_SWAP_PROC* proc)
{
    EnterCriticalSection(&proc->sendLock);
    VddCloseConn(&proc->hClientConn);
    proc->connGen++;
    LeaveCriticalSection(&proc->sendLock);
}

/* TRUE while a packed frame waits for the sender. A frame packed now would
   replace it, so it must be a full frame (a dirty-rect delta would lose the
   waiting frame's changes). */
static BOOL VddFrameWaiting(VDD_SWAP_PROC* proc)
{
    BOOL waiting;
    EnterCriticalSection(&proc->queueLock);
    waiting = proc->pendingSlot != -1;
    LeaveCriticalSection(&proc->queueLock);
    return waiting;
}

/* Reserve the slot the sender is not using, with room for `need` bytes.
   Returns the slot or -1. Only the capture thread calls this. */
static int VddQueueBegin(VDD_SWAP_PROC* proc, UINT need)
{
    int slot;
    EnterCriticalSection(&proc->queueLock);
    slot = (proc->sendingSlot == 0) ? 1 : 0;
    if (proc->pendingSlot == slot) proc->pendingSlot = -1;   /* replaced by this frame */
    LeaveCriticalSection(&proc->queueLock);

    if (proc->slotCap[slot] < need) {
        BYTE* nb = (BYTE*)realloc(proc->slotBuf[slot], need);
        if (!nb) {
            VddLog("Frame: send buffer allocation of %u bytes FAILED", need);
            return -1;
        }
        proc->slotBuf[slot] = nb;
        proc->slotCap[slot] = need;
    }
    return slot;
}

static void VddQueueCommit(VDD_SWAP_PROC* proc, int slot, UINT len)
{
    EnterCriticalSection(&proc->queueLock);
    proc->slotLen[slot] = len;
    proc->slotGen[slot] = proc->connGen;
    proc->pendingSlot = slot;
    LeaveCriticalSection(&proc->queueLock);
    SetEvent(proc->hSendEvent);
}

static DWORD WINAPI VddSenderThread(LPVOID param)
{
    VDD_SWAP_PROC* proc = (VDD_SWAP_PROC*)param;
    for (;;) {
        WaitForSingleObject(proc->hSendEvent, INFINITE);
        if (proc->bStopSend) break;
        for (;;) {
            int slot;
            UINT len = 0;
            LONG gen = 0;
            EnterCriticalSection(&proc->queueLock);
            slot = proc->pendingSlot;
            if (slot != -1) {
                proc->pendingSlot = -1;
                proc->sendingSlot = slot;
                len = proc->slotLen[slot];
                gen = proc->slotGen[slot];
            }
            LeaveCriticalSection(&proc->queueLock);
            if (slot == -1 || proc->bStopSend) break;

            EnterCriticalSection(&proc->sendLock);
            if (proc->hClientConn && gen == proc->connGen &&
                VddSendAll(proc->hClientConn, (const char*)proc->slotBuf[slot], (int)len) != 0)
                InterlockedExchange(&proc->sendFailed, 1);
            LeaveCriticalSection(&proc->sendLock);

            EnterCriticalSection(&proc->queueLock);
            proc->sendingSlot = -1;
            LeaveCriticalSection(&proc->queueLock);
        }
    }
    return 0;
}

/* Query hardware cursor and send update to host if connected */
static void VddSendCursorUpdate(VDD_SWAP_PROC* proc)
{
    IDARG_IN_QUERY_HWCURSOR inArgs = {};
    IDARG_OUT_QUERY_HWCURSOR3 outArgs = {};
    VDD_WIRE_CURSOR_HEADER chdr;
    AsbConn* s;

    if (!proc->hCursorEvent || !proc->pCursorShapeBuffer || !proc->hMonitor)
        return;

    s = proc->hClientConn;
    if (s == NULL)
        return;

    inArgs.LastShapeId = proc->lastShapeId;
    inArgs.ShapeBufferSizeInBytes = proc->cursorBufSize;
    inArgs.pShapeBuffer = proc->pCursorShapeBuffer;

    HRESULT hr = IddCxMonitorQueryHardwareCursor3(proc->hMonitor, &inArgs, &outArgs);
    if (FAILED(hr)) {
        VddLog("Cursor: QueryHardwareCursor3 FAILED 0x%08X", hr);
        return;
    }

    chdr.magic          = VDD_CURSOR_MAGIC;
    chdr.x              = outArgs.X;
    chdr.y              = outArgs.Y;
    chdr.visible        = outArgs.IsCursorVisible ? 1 : 0;
    chdr.shape_updated  = outArgs.IsCursorShapeUpdated ? 1 : 0;
    chdr.shape_id       = outArgs.CursorShapeInfo.ShapeId;
    chdr.width          = outArgs.CursorShapeInfo.Width;
    chdr.height         = outArgs.CursorShapeInfo.Height;
    chdr.pitch          = outArgs.CursorShapeInfo.Pitch;
    chdr.xhot           = outArgs.CursorShapeInfo.XHot;
    chdr.yhot           = outArgs.CursorShapeInfo.YHot;
    chdr.cursor_type    = (UINT32)outArgs.CursorShapeInfo.CursorType;

    if (outArgs.IsCursorShapeUpdated) {
        UINT32 data_size = outArgs.CursorShapeInfo.Pitch * outArgs.CursorShapeInfo.Height;
        if (data_size > proc->cursorBufSize) {
            VddLog("Cursor: shape data %u exceeds buffer %u, skipping",
                   data_size, proc->cursorBufSize);
            chdr.shape_updated = 0;
            chdr.shape_data_size = 0;
        } else {
            chdr.shape_data_size = data_size;
            proc->lastShapeId = outArgs.CursorShapeInfo.ShapeId;
        }
    } else {
        chdr.shape_data_size = 0;
    }

    /* Header and shape go out as one message, between frames. */
    (void)s;
    if (VddSendMsg(proc, &chdr, (int)sizeof(chdr),
                   chdr.shape_data_size > 0 ? proc->pCursorShapeBuffer : NULL,
                   (int)chdr.shape_data_size) != 0) {
        VddLog("Cursor: send failed, disconnecting");
        VddCloseClient(proc);
    }
}

/* Pack one frame message (header [+ rects] + data_size + pixels) into a free
   sender slot and queue it; the sender thread transmits it in one call
   (one transport call per frame, not per scanline). rectCount == 0 sends
   the whole frame. Returns 0 when queued. */
static int VddSendFrame(VDD_SWAP_PROC* proc, const D3D11_MAPPED_SUBRESOURCE* mapped,
                        const RECT* rects, UINT32 rectCount)
{
    const UINT w = proc->frameWidth, h = proc->frameHeight;
    const UINT stride = w * VDD_BPP;
    VDD_WIRE_FRAME_HEADER whdr;
    UINT32 data_size = 0;
    UINT total;
    BYTE* p;

    if (rectCount == 0) {
        data_size = stride * h;
    } else {
        for (UINT32 i = 0; i < rectCount; i++)
            data_size += (UINT)(rects[i].right - rects[i].left) * VDD_BPP *
                         (UINT)(rects[i].bottom - rects[i].top);
    }

    total = (UINT)sizeof(whdr) + rectCount * (UINT)sizeof(RECT) + 4 + data_size;
    if (!proc->hClientConn) return -1;
    int slot = VddQueueBegin(proc, total);
    if (slot < 0) return -1;

    proc->frameSeq++;
    whdr.magic            = VDD_FRAME_MAGIC;
    whdr.width            = w;
    whdr.height           = h;
    whdr.stride           = stride;
    whdr.frame_seq        = proc->frameSeq;
    whdr.dirty_rect_count = rectCount;

    p = proc->slotBuf[slot];
    memcpy(p, &whdr, sizeof(whdr)); p += sizeof(whdr);
    if (rectCount) { memcpy(p, rects, rectCount * sizeof(RECT)); p += rectCount * sizeof(RECT); }
    memcpy(p, &data_size, 4); p += 4;

    if (rectCount == 0) {
        if (mapped->RowPitch == stride) {
            memcpy(p, mapped->pData, (size_t)stride * h);
        } else {
            for (UINT row = 0; row < h; row++)
                memcpy(p + (size_t)row * stride,
                       (const BYTE*)mapped->pData + (size_t)row * mapped->RowPitch, stride);
        }
    } else {
        for (UINT32 i = 0; i < rectCount; i++) {
            UINT left = (UINT)rects[i].left, top = (UINT)rects[i].top;
            UINT rowBytes = (UINT)(rects[i].right - rects[i].left) * VDD_BPP;
            UINT rh = (UINT)(rects[i].bottom - rects[i].top);
            for (UINT row = 0; row < rh; row++) {
                memcpy(p, (const BYTE*)mapped->pData + (size_t)(top + row) * mapped->RowPitch
                          + (size_t)left * VDD_BPP, rowBytes);
                p += rowBytes;
            }
        }
    }

    VddQueueCommit(proc, slot, total);
    return 0;
}

/* ========================================================================= */
/*  Performance mode (GPU-encoded video)                                     */
/* ========================================================================= */

/* Queue one encoded access unit. Unlike raw frames, a waiting video message
   is never replaced (that would break the stream until the next key frame):
   the new one is appended to it, and the capture loop skips encoding while
   anything waits, so the backlog stays at about one frame. Called from the
   encoder's event thread. */
static void VddEncOut(void* user, const BYTE* data, UINT len, BOOL key, UINT width, UINT height)
{
    VDD_SWAP_PROC* proc = (VDD_SWAP_PROC*)user;
    VDD_WIRE_VIDEO_HEADER h;
    UINT need = (UINT)sizeof(h) + len;
    int slot;
    UINT off;

    if (!proc->hClientConn || len == 0) return;
    h.magic  = VDD_VIDEO_MAGIC;
    h.width  = width;
    h.height = height;
    h.codec  = VDD_CODEC_H264;
    h.flags  = key ? VDD_VIDEO_FLAG_KEY : 0;
    h.size   = len;
    h.seq    = ++proc->videoSeq;

    EnterCriticalSection(&proc->queueLock);
    if (proc->pendingSlot != -1 && proc->slotGen[proc->pendingSlot] == proc->connGen) {
        slot = proc->pendingSlot;
        off = proc->slotLen[slot];
    } else {
        slot = (proc->sendingSlot == 0) ? 1 : 0;
        off = 0;
    }
    if (off + need > 64u * 1024 * 1024) {
        /* The host stopped reading; start over from a key frame. */
        off = 0;
        InterlockedExchange(&proc->perfKeyReq, 1);
    }
    if (proc->slotCap[slot] < off + need) {
        BYTE* nb = (BYTE*)realloc(proc->slotBuf[slot], off + need);
        if (!nb) {
            LeaveCriticalSection(&proc->queueLock);
            InterlockedExchange(&proc->perfKeyReq, 1);
            return;
        }
        proc->slotBuf[slot] = nb;
        proc->slotCap[slot] = off + need;
    }
    memcpy(proc->slotBuf[slot] + off, &h, sizeof(h));
    memcpy(proc->slotBuf[slot] + off + sizeof(h), data, len);
    proc->slotLen[slot] = off + need;
    proc->slotGen[slot] = proc->connGen;
    proc->pendingSlot = slot;
    LeaveCriticalSection(&proc->queueLock);
    SetEvent(proc->hSendEvent);
}

static void VddEncLog(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    VddLog("%s", buf);
}

static void VddSendPerfStatus(VDD_SWAP_PROC* proc, UINT mode, HRESULT hr)
{
    VDD_WIRE_PERF_STATUS st;
    st.magic = VDD_PERF_STATUS_MAGIC;
    st.mode = mode;
    st.codec = mode ? VDD_CODEC_H264 : 0;
    st.hresult = (INT32)hr;
    if (proc->hClientConn)
        VddSendMsg(proc, &st, (int)sizeof(st), NULL, 0);
}

static void VddPerfStop(VDD_SWAP_PROC* proc)
{
    if (proc->pEncoder) {
        VddEncoderDestroy(proc->pEncoder);
        proc->pEncoder = nullptr;
    }
}

/* Make sure an encoder for width x height exists when the host wants video.
   Returns TRUE when frames should go through the encoder. On failure the
   host is told and raw frames continue. */
static BOOL VddPerfEnsure(VDD_SWAP_PROC* proc, UINT width, UINT height)
{
    if (!proc->perfWanted || proc->perfFailed || !proc->hClientConn) {
        if (proc->pEncoder) {
            VddPerfStop(proc);
            VddLog("Perf: video off, sending raw frames");
            VddSendPerfStatus(proc, 0, S_OK);
        }
        return FALSE;
    }
    if (proc->pEncoder) {
        if (VddEncoderFailed(proc->pEncoder)) {
            VddPerfStop(proc);
            proc->perfFailed = TRUE;
            VddLog("Perf: encoder failed, back to raw frames");
            VddSendPerfStatus(proc, 0, E_FAIL);
            return FALSE;
        }
        if (VddEncoderWidth(proc->pEncoder) == width && VddEncoderHeight(proc->pEncoder) == height)
            return TRUE;
        VddPerfStop(proc);   /* new size: new encoder (starts with a key frame) */
    }

    UINT fps = proc->pContext && proc->pContext->lastRefresh ? proc->pContext->lastRefresh : 60;
    VddEncoder* enc = nullptr;
    HRESULT hr = VddEncoderCreate(proc->pDevice, proc->pDeviceContext, width, height, fps,
                                  (UINT)proc->perfKbps, VddEncOut, proc, VddEncLog, &enc);
    if (FAILED(hr) || !enc) {
        proc->perfFailed = TRUE;
        VddLog("Perf: no GPU video encoder (0x%08X), staying with raw frames", hr);
        VddSendPerfStatus(proc, 0, FAILED(hr) ? hr : E_FAIL);
        return FALSE;
    }
    proc->pEncoder = enc;
    VddLog("Perf: GPU video on (%s, %ux%u)", VddEncoderName(enc), width, height);
    VddSendPerfStatus(proc, 1, S_OK);
    return TRUE;
}

/* Read a pending mode request from the host, if any. The host only ever
   sends VDD_WIRE_MODE_REQUEST messages on this channel. Returns FALSE when
   the connection is gone. */
static BOOL VddPollHostRequests(VDD_SWAP_PROC* proc)
{
    AsbConn* c = proc->hClientConn;
    if (!c) return TRUE;
    for (int guard = 0; guard < 8; guard++) {
        int r = asb_poll(c, 0);
        if (r == 0) return TRUE;
        if (r < 0) return FALSE;

        VDD_WIRE_MODE_REQUEST req;
        int got = 0;
        while (got < (int)sizeof(req)) {
            int n = asb_recv(c, (char*)&req + got, (int)sizeof(req) - got);
            if (n <= 0) return FALSE;
            got += n;
        }
        if (req.magic == VDD_HELLO_QUERY_MAGIC) {
            /* Host asks what this driver supports. */
            VDD_WIRE_HELLO hello = {};
            hello.magic = VDD_HELLO_MAGIC;
            hello.version = VDD_PROTOCOL_VERSION;
            VddLog("HostRequest: hello query, answering protocol %u", hello.version);
            if (VddSendMsg(proc, &hello, (int)sizeof(hello), NULL, 0) != 0) return FALSE;
            continue;
        }
        if (req.magic == VDD_PERF_REQUEST_MAGIC) {
            /* width = mode, height = kbps, refresh = flags */
            LONG mode = req.width == 1 ? 1 : 0;
            if (mode != proc->perfWanted || req.height != (UINT32)proc->perfKbps) {
                VddLog("HostRequest: performance mode %s (%u kbit/s)",
                       mode ? "video" : "raw", req.height);
                if (proc->pEncoder && req.height != (UINT32)proc->perfKbps)
                    VddPerfStop(proc);   /* new bit rate: reopen */
            }
            InterlockedExchange(&proc->perfKbps, (LONG)req.height);
            InterlockedExchange(&proc->perfWanted, mode);
            proc->perfFailed = FALSE;    /* asking again retries a failed encoder */
            if (req.refresh & VDD_PERF_FLAG_KEYFRAME)
                InterlockedExchange(&proc->perfKeyReq, 1);
            continue;
        }
        if (req.magic != VDD_MODE_REQUEST_MAGIC) {
            VddLog("HostRequest: bad magic 0x%08X, dropping connection", req.magic);
            return FALSE;
        }
        VddRequestMode(proc->pContext, req.width, req.height, req.refresh);
    }
    return TRUE;
}

/* (Re)create the staging texture when the swap chain surface size changes. */
static BOOL VddEnsureStaging(VDD_SWAP_PROC* proc, UINT width, UINT height)
{
    if (proc->pStagingTex && proc->frameWidth == width && proc->frameHeight == height)
        return TRUE;
    if (proc->pStagingTex) {
        proc->pStagingTex->Release();
        proc->pStagingTex = nullptr;
    }
    HRESULT hr = VddCreateStagingTexture(proc, width, height);
    if (FAILED(hr)) {
        VddLog("Frame: staging texture %ux%u FAILED hr=0x%08X", width, height, hr);
        return FALSE;
    }
    VddLog("Frame: surface size %ux%u", width, height);
    proc->frameWidth = width;
    proc->frameHeight = height;
    return TRUE;
}

static void VddSwapChainRunCore(VDD_SWAP_PROC* proc)
{
    IDXGIDevice* pDxgiDevice = NULL;
    IDARG_IN_SWAPCHAINSETDEVICE setDevice;
    HRESULT hr;
    HANDLE waitHandles[3];  /* swap chain, terminate, cursor */
    DWORD waitCount;

    /* Retry limits — on persistent failure, exit RunCore so the caller can
       WdfObjectDelete the swap chain, which tells IddCx to reassign a fresh one. */
    const int MAX_RETRIES = 5;
    const DWORD RETRY_DELAY_INIT = 1;
    const DWORD RETRY_DELAY_MAX = 100;
    DWORD retryDelay = RETRY_DELAY_INIT;
    int retryCount = 0;

    VddLog("SwapChainRunCore: entered");

    hr = proc->pDevice->QueryInterface(IID_PPV_ARGS(&pDxgiDevice));
    if (FAILED(hr)) {
        VddLog("SwapChainRunCore: QueryInterface for IDXGIDevice FAILED hr=0x%08X", hr);
        return;
    }

    memset(&setDevice, 0, sizeof(setDevice));
    setDevice.pDevice = pDxgiDevice;

    hr = IddCxSwapChainSetDevice(proc->hSwapChain, &setDevice);
    pDxgiDevice->Release();
    if (FAILED(hr)) {
        VddLog("SwapChainRunCore: IddCxSwapChainSetDevice FAILED hr=0x%08X, exiting for reassignment", hr);
        return;
    }
    VddLog("SwapChainRunCore: swap chain device set, entering frame loop");

    waitHandles[0] = proc->hAvailableEvent;
    waitHandles[1] = proc->hTerminateEvent;
    waitCount = 2;
    if (proc->hCursorEvent) {
        waitHandles[2] = proc->hCursorEvent;
        waitCount = 3;
    }

    /* Track staging texture row pitch (set on first capture) and whether
       we have a valid frame in the staging texture for resend on idle. */
    BOOL bHasFrame = FALSE;
    BOOL bSentFullFrame = FALSE;   /* TRUE after first full frame sent to current client */


    /* Main frame acquisition loop */
    for (;;)
    {
        IDXGIResource* pSurface = NULL;

        /* Check for pending new connection from network thread */
        {
            AsbConn* pending = (AsbConn*)InterlockedExchangePointer(
                (PVOID volatile*)&proc->hPendingConn, NULL);
            if (pending) {
                EnterCriticalSection(&proc->sendLock);   /* no frame in flight while swapping */
                /* Drop the previous client. On PC the old socket is distinct -> close it. On ivshmem
                   the pending connection is the SAME single slot the host just re-armed, so closing
                   the old wrapper would mark the shared slot CLOSING and kill the new one — abandon
                   the stale wrapper instead. */
                if (proc->hClientConn) {
                    if (asb_conn_socket_u64(proc->hClientConn) != ~0ull)
                        VddCloseConn(&proc->hClientConn);          /* PC: real close */
                    else { asb_abandon(proc->hClientConn); proc->hClientConn = NULL; }  /* ivshmem */
                    VddLog("Frame: replaced previous client connection");
                }
                asb_stream_reset(pending);   /* ivshmem: align the fresh full frame at ring offset 0 (no-op on PC) */
                /* A stalled host must not pin the sender forever. */
                asb_set_timeout(pending, 0, 5000);
                proc->hClientConn = pending;
                proc->connGen++;
                LeaveCriticalSection(&proc->sendLock);
                proc->frameSeq = 0;
                bSentFullFrame = FALSE;   /* new connection -> first frame is full (the reconnect trigger) */
                /* Each connection starts with raw frames; the host asks for
                   video again after the hello. */
                VddPerfStop(proc);
                InterlockedExchange(&proc->perfWanted, 0);
                proc->perfFailed = FALSE;
                VddLog("Frame: new client connection active");
            }
        }

        /* The sender thread could not deliver a frame. */
        if (InterlockedExchange(&proc->sendFailed, 0)) {
            VddLog("Frame: send failed, disconnecting (seq=%llu)", proc->frameSeq);
            VddCloseClient(proc);
            bSentFullFrame = FALSE;
        }

        /* Mode requests from the host arrive on the same channel. */
        if (proc->hClientConn && !VddPollHostRequests(proc)) {
            VddLog("Frame: host channel closed while reading requests");
            VddCloseClient(proc);
            bSentFullFrame = FALSE;
        }


        /* Use v2 acquire when available */
#pragma warning(suppress: 4127)
        if (IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2)) {
            IDARG_IN_RELEASEANDACQUIREBUFFER2 bufferInArgs = { 0 };
            IDARG_OUT_RELEASEANDACQUIREBUFFER2 buffer = { 0 };
            bufferInArgs.Size = sizeof(bufferInArgs);
            hr = IddCxSwapChainReleaseAndAcquireBuffer2(proc->hSwapChain, &bufferInArgs, &buffer);
            pSurface = buffer.MetaData.pSurface;
        } else {
            IDARG_OUT_RELEASEANDACQUIREBUFFER buffer = { 0 };
            hr = IddCxSwapChainReleaseAndAcquireBuffer(proc->hSwapChain, &buffer);
            pSurface = buffer.MetaData.pSurface;
        }

        if (hr == E_PENDING) {
            DWORD waitResult = WaitForMultipleObjects(waitCount, waitHandles, FALSE, 100);
            if (waitResult == WAIT_OBJECT_0 + 1) {
                break;  /* Terminate */
            }
            if (waitResult == WAIT_OBJECT_0 + 2) {
                /* Cursor event — query and send cursor update over the transport */
                VddSendCursorUpdate(proc);
                continue;
            }
            if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_TIMEOUT) {
                VddLog("SwapChainRunCore: unexpected wait result 0x%lX, exiting for reassignment", waitResult);
                break;
            }

            /* Video: encode the last picture again (keeps the stream and the
               host's frame counter alive; an unchanged picture is tiny). */
            if (waitResult == WAIT_TIMEOUT && proc->pEncoder && proc->hClientConn &&
                !VddFrameWaiting(proc)) {
                BOOL key = InterlockedExchange(&proc->perfKeyReq, 0) != 0;
                HRESULT ehr = VddEncoderRepeat(proc->pEncoder, key);
                if (ehr != S_OK && key) InterlockedExchange(&proc->perfKeyReq, 1);
                continue;
            }

            /* On timeout with a connected client and a cached frame, resend
               the staging texture contents to keep the connection alive. */
            if (waitResult == WAIT_TIMEOUT && bHasFrame &&
                proc->hClientConn != NULL && proc->pStagingTex)
            {
                D3D11_MAPPED_SUBRESOURCE mapped = {};
                hr = proc->pDeviceContext->Map(proc->pStagingTex, 0, D3D11_MAP_READ, 0, &mapped);
                if (SUCCEEDED(hr))
                {
                    BOOL send_ok = VddSendFrame(proc, &mapped, nullptr, 0) == 0;
                    proc->pDeviceContext->Unmap(proc->pStagingTex, 0);

                    if (!send_ok) bSentFullFrame = FALSE;
                }
            }

            continue;
        } else if (SUCCEEDED(hr)) {
            /* Reset retry state on successful acquisition */
            retryDelay = RETRY_DELAY_INIT;
            retryCount = 0;

            /* New frame from IddCx — capture and send to host if connected */
            if (pSurface)
            {
                ID3D11Texture2D* pTexture = nullptr;
                hr = pSurface->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&pTexture);
                if (SUCCEEDED(hr) && pTexture)
                {
                    D3D11_TEXTURE2D_DESC sdesc = {};
                    pTexture->GetDesc(&sdesc);

                    /* The surface follows the active mode; a new size means the
                       host must get a full frame first. */
                    if (proc->frameWidth != sdesc.Width || proc->frameHeight != sdesc.Height) {
                        bSentFullFrame = FALSE;
                        bHasFrame = FALSE;
                    }

                    if (VddPerfEnsure(proc, sdesc.Width, sdesc.Height)) {
                        /* Performance mode: the frame stays on the GPU. While
                           the sender still has a message waiting, skip this
                           frame; the stream stays valid. */
                        if (proc->pStagingTex) {    /* stale now; raw fallback recreates it */
                            proc->pStagingTex->Release();
                            proc->pStagingTex = nullptr;
                        }
                        proc->frameWidth = sdesc.Width;
                        proc->frameHeight = sdesc.Height;
                        bHasFrame = FALSE;
                        bSentFullFrame = FALSE;     /* a raw fallback starts with a full frame */
                        if (!VddFrameWaiting(proc)) {
                            BOOL key = InterlockedExchange(&proc->perfKeyReq, 0) != 0;
                            HRESULT ehr = VddEncoderEncode(proc->pEncoder, pTexture, key);
                            if (ehr != S_OK && key) InterlockedExchange(&proc->perfKeyReq, 1);
                        }
                    } else if (VddEnsureStaging(proc, sdesc.Width, sdesc.Height)) {
                        proc->pDeviceContext->CopyResource(proc->pStagingTex, pTexture);

                        D3D11_MAPPED_SUBRESOURCE mapped = {};
                        hr = proc->pDeviceContext->Map(proc->pStagingTex, 0, D3D11_MAP_READ, 0, &mapped);
                        if (SUCCEEDED(hr))
                        {
                            bHasFrame = TRUE;

                            if (proc->hClientConn != NULL)
                            {
                                RECT dirtyRects[VDD_MAX_DIRTY_RECTS];
                                UINT32 rectCount = 0;
                                BOOL sendFull = !bSentFullFrame || VddFrameWaiting(proc);  /* first frame, or replacing a waiting one */

                                /* Query dirty rects from IddCx.  If GetDirtyRects returns
                                   0 rects OR fails, fall back to sending a full frame.
                                   0 rects does NOT reliably mean "identical" — during early
                                   boot the presenter may not supply dirty rect metadata,
                                   and skipping would stall updates until the E_PENDING
                                   timeout resend path fires ~100ms later. */
                                if (!sendFull) {
                                    IDARG_IN_GETDIRTYRECTS drIn = {};
                                    IDARG_OUT_GETDIRTYRECTS drOut = {};
                                    drIn.DirtyRectInCount = VDD_MAX_DIRTY_RECTS;
                                    drIn.pDirtyRects = dirtyRects;
                                    HRESULT drHr = IddCxSwapChainGetDirtyRects(proc->hSwapChain, &drIn, &drOut);
                                    if (SUCCEEDED(drHr) && drOut.DirtyRectOutCount > 0 &&
                                        drOut.DirtyRectOutCount <= VDD_MAX_DIRTY_RECTS) {
                                        rectCount = drOut.DirtyRectOutCount;
                                    } else {
                                        sendFull = TRUE;
                                    }
                                }

                                if (!sendFull) {
                                    /* Clamp rects to the frame and drop empty ones. */
                                    UINT32 kept = 0;
                                    for (UINT32 i = 0; i < rectCount; i++) {
                                        RECT r = dirtyRects[i];
                                        if (r.left < 0) r.left = 0;
                                        if (r.top  < 0) r.top  = 0;
                                        if (r.right  > (LONG)proc->frameWidth)  r.right  = (LONG)proc->frameWidth;
                                        if (r.bottom > (LONG)proc->frameHeight) r.bottom = (LONG)proc->frameHeight;
                                        if (r.left < r.right && r.top < r.bottom)
                                            dirtyRects[kept++] = r;
                                    }
                                    rectCount = kept;
                                }

                                if (sendFull || rectCount > 0) {
                                    BOOL send_ok = VddSendFrame(proc, &mapped, dirtyRects,
                                                                sendFull ? 0 : rectCount) == 0;
                                    if (!send_ok) {
                                        bSentFullFrame = FALSE;
                                    } else {
                                        if (sendFull) bSentFullFrame = TRUE;
                                        if (proc->frameSeq == 1)
                                            VddLog("Frame: first frame queued (%ux%u)",
                                                   proc->frameWidth, proc->frameHeight);
                                    }
                                }
                            }

                            proc->pDeviceContext->Unmap(proc->pStagingTex, 0);
                        }
                    }

                    pTexture->Release();
                }
            }

            if (pSurface) {
                pSurface->Release();
            }

            /* Check for cursor update alongside frame */
            if (proc->hCursorEvent &&
                WaitForSingleObject(proc->hCursorEvent, 0) == WAIT_OBJECT_0) {
                VddSendCursorUpdate(proc);
            }


            hr = IddCxSwapChainFinishedProcessingFrame(proc->hSwapChain);
            if (FAILED(hr)) {
                VddLog("SwapChainRunCore: FinishedProcessingFrame FAILED hr=0x%08X, exiting for reassignment", hr);
                break;
            }
        } else {
            /* ACCESS_LOST can be transient (display mode change) — retry with
               exponential backoff up to MAX_RETRIES.  All other errors: exit
               immediately so WdfObjectDelete triggers IddCx reassignment. */
            if (hr == (HRESULT)0x887A0026 /* DXGI_ERROR_ACCESS_LOST */ && retryCount < MAX_RETRIES) {
                retryCount++;
                VddLog("SwapChainRunCore: DXGI_ERROR_ACCESS_LOST, retry %d/%d after %lums",
                       retryCount, MAX_RETRIES, retryDelay);
                if (WaitForSingleObject(proc->hTerminateEvent, retryDelay) == WAIT_OBJECT_0)
                    break;
                retryDelay = min(retryDelay * 2, RETRY_DELAY_MAX);
                continue;
            }
            VddLog("SwapChainRunCore: AcquireBuffer FAILED hr=0x%08X (retries=%d), exiting for reassignment",
                   hr, retryCount);
            break;
        }
    }

    /* The encoder lives on this thread (COM) and calls back into proc. */
    VddPerfStop(proc);
}

/* ========================================================================= */
/*  Swap chain cleanup helper                                                */
/* ========================================================================= */

static void VddDestroySwapProc(VDD_SWAP_PROC* proc)
{
    if (!proc) return;

    /* Stop network first */
    proc->bStopNetwork = TRUE;

    VddLog("DestroySwapProc: signaling terminate event");
    if (proc->hTerminateEvent)
        SetEvent(proc->hTerminateEvent);

    if (proc->hThread)
    {
        VddLog("DestroySwapProc: waiting for swap chain thread...");
        WaitForSingleObject(proc->hThread, 5000);
        VddLog("DestroySwapProc: swap chain thread exited");
        CloseHandle(proc->hThread);
    }

    /* Wait for network thread to exit */
    if (proc->hNetworkThread)
    {
        VddLog("DestroySwapProc: waiting for network thread...");
        WaitForSingleObject(proc->hNetworkThread, 5000);
        VddLog("DestroySwapProc: network thread exited");
        CloseHandle(proc->hNetworkThread);
    }

    /* Stop the sender before its buffers and connection go away. */
    if (proc->hSendThread) {
        proc->bStopSend = TRUE;
        SetEvent(proc->hSendEvent);
        WaitForSingleObject(proc->hSendThread, 10000);
        CloseHandle(proc->hSendThread);
    }

    /* Close any remaining client connection */
    VddLog("DestroySwapProc: closing client connection (%p)", (void*)proc->hClientConn);
    VddCloseConn(&proc->hClientConn);
    VddLog("DestroySwapProc: closing pending connection");
    {
        AsbConn* pending = (AsbConn*)InterlockedExchangePointer(
            (PVOID volatile*)&proc->hPendingConn, NULL);
        if (pending) VddCloseConn(&pending);
    }

    VddLog("DestroySwapProc: closing terminate event");
    if (proc->hTerminateEvent)
        CloseHandle(proc->hTerminateEvent);

    VddLog("DestroySwapProc: closing cursor resources");
    if (proc->hCursorEvent)
        CloseHandle(proc->hCursorEvent);
    if (proc->pCursorShapeBuffer)
        free(proc->pCursorShapeBuffer);

    /* Only release the per-swap-chain staging texture.  The D3D device and
       context are cached in VDD_DEVICE_CONTEXT and intentionally NOT released
       here — this is the fix for the pDevice->Release() crash after session
       logoff.  Destroying the D3D device after WdfObjectDelete triggers IddCx
       teardown corrupts DXGI internal state. */
    VddLog("DestroySwapProc: releasing staging texture (staging=%p, device=%p cached)",
           (void*)proc->pStagingTex, (void*)proc->pDevice);
    if (proc->pStagingTex)
        proc->pStagingTex->Release();
    VddLog("DestroySwapProc: staging texture released");
    free(proc->slotBuf[0]);
    free(proc->slotBuf[1]);
    if (proc->hSendEvent) CloseHandle(proc->hSendEvent);
    DeleteCriticalSection(&proc->queueLock);
    DeleteCriticalSection(&proc->sendLock);

    free(proc);
    VddLog("DestroySwapProc: COMPLETE");
}

/* ========================================================================= */
/*  Device context init/cleanup                                              */
/* ========================================================================= */

static void VddContextInit(VDD_DEVICE_CONTEXT* ctx, WDFDEVICE device)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->wdfDevice = device;

    InitializeSRWLock(&ctx->modeLock);

    /* A macOS host reads frames through a fixed-size ivshmem region, so keep
       the single 1920x1080@60 mode there. The transport probe is safe here
       (not under the loader lock). */
    VddEnsureTransportInit();
    ctx->legacySingleMode = asb_transport_is_ivshmem() ? TRUE : FALSE;
    VddRebuildModeListLocked(ctx, nullptr);
    VddLog("ContextInit: %u modes offered%s", ctx->modeCount,
           ctx->legacySingleMode ? " (macOS host: single mode)" : "");

    g_pVddContext = ctx;
}

static void VddContextCleanup(VDD_DEVICE_CONTEXT* ctx)
{
    VddLog("Context cleanup starting");

    if (g_pVddContext == ctx) g_pVddContext = nullptr;
    if (ctx->hModeTimer) {
        WdfTimerStop(ctx->hModeTimer, TRUE);
    }
    if (ctx->hRenderTimer) {
        WdfTimerStop(ctx->hRenderTimer, TRUE);
    }

    /* Stop recovery timer (wait for any in-progress callback to finish) */
    if (ctx->hRecoveryTimer) {
        WdfTimerStop(ctx->hRecoveryTimer, TRUE);
    }

    if (ctx->pSwapProc)
    {
        VddDestroySwapProc(ctx->pSwapProc);
        ctx->pSwapProc = nullptr;
    }

    /* Release cached D3D device (safe here — no IddCx teardown in progress) */
    if (ctx->pCachedCtx) {
        ctx->pCachedCtx->Release();
        ctx->pCachedCtx = nullptr;
    }
    if (ctx->pCachedDevice) {
        VddLog("Context cleanup: releasing cached D3D device");
        ctx->pCachedDevice->Release();
        ctx->pCachedDevice = nullptr;
    }

    VddLog("Context cleanup done");
}

/* ========================================================================= */
/*  InitAdapter (called from D0Entry)                                        */
/* ========================================================================= */

static void VddInitAdapter(VDD_DEVICE_CONTEXT* ctx)
{
    VddLog("InitAdapter: entered (ctx=%p, wdfDevice=%p)", (void*)ctx, (void*)ctx->wdfDevice);

    IDDCX_ADAPTER_CAPS adapterCaps = {};
    adapterCaps.Size = sizeof(adapterCaps);
    VddLog("InitAdapter: IDDCX_ADAPTER_CAPS.Size=%u", (unsigned)adapterCaps.Size);

    /* Set FP16 processing flag when supported */
#pragma warning(suppress: 4127)
    if (IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2)) {
        adapterCaps.Flags = IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16;
        VddLog("InitAdapter: FP16 flag set");
    } else {
        VddLog("InitAdapter: FP16 not available");
    }

    adapterCaps.MaxMonitorsSupported = 1;

    adapterCaps.EndPointDiagnostics.Size = sizeof(adapterCaps.EndPointDiagnostics);
    adapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    adapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;

    adapterCaps.EndPointDiagnostics.pEndPointFriendlyName      = L"AppSandbox Virtual Display";
    adapterCaps.EndPointDiagnostics.pEndPointManufacturerName   = L"AppSandbox";
    adapterCaps.EndPointDiagnostics.pEndPointModelName          = L"VDD";

    IDDCX_ENDPOINT_VERSION version = {};
    version.Size = sizeof(version);
    version.MajorVer = 1;
    adapterCaps.EndPointDiagnostics.pFirmwareVersion = &version;
    adapterCaps.EndPointDiagnostics.pHardwareVersion = &version;

    VddLog("InitAdapter: adapter caps configured, calling IddCxAdapterInitAsync...");

    /* Put IndirectDeviceContextWrapper on the adapter object so we can
       retrieve our context in AdapterInitFinished */
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, IndirectDeviceContextWrapper);

    IDARG_IN_ADAPTER_INIT adapterInit = {};
    adapterInit.WdfDevice = ctx->wdfDevice;
    adapterInit.pCaps     = &adapterCaps;
    adapterInit.ObjectAttributes = &attr;

    IDARG_OUT_ADAPTER_INIT adapterInitOut = {};
    NTSTATUS st = IddCxAdapterInitAsync(&adapterInit, &adapterInitOut);

    if (NT_SUCCESS(st))
    {
        ctx->hAdapter = adapterInitOut.AdapterObject;

        /* Link adapter wrapper back to our context */
        IndirectDeviceContextWrapper* pWrapper =
            WdfObjectGet_IndirectDeviceContextWrapper(adapterInitOut.AdapterObject);
        pWrapper->pContext = ctx;

        VddLog("InitAdapter: IddCxAdapterInitAsync succeeded (adapter=%p)", (void*)adapterInitOut.AdapterObject);

        /* The render adapter is chosen in FinishInit (VddApplyRenderAdapter). */
    }
    else
    {
        VddLog("InitAdapter: IddCxAdapterInitAsync FAILED st=0x%08X", st);
    }
}

/* ========================================================================= */
/*  FinishInit (called from AdapterInitFinished)                             */
/* ========================================================================= */

/* ========================================================================= */
/*  Render adapter                                                           */
/* ========================================================================= */

/* The hardware GPU to compose on: the non-software adapter with the most
   dedicated memory (the GPU-PV adapter of the GPU given to the VM). FALSE
   when only WARP / basic adapters exist. HKLM\SOFTWARE\AppSandbox
   DisplayRenderAdapter = "default" keeps the OS choice. */
static BOOL VddFindRenderAdapter(LUID* out, char* name, size_t name_cap)
{
    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    SIZE_T best_mem = 0;
    BOOL found = FALSE;

    {
        HKEY k;
        char val[32] = "";
        DWORD len = sizeof(val);
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\AppSandbox", 0, KEY_READ, &k) == ERROR_SUCCESS) {
            RegQueryValueExA(k, "DisplayRenderAdapter", NULL, NULL, (BYTE*)val, &len);
            RegCloseKey(k);
            if (_stricmp(val, "default") == 0) return FALSE;
        }
    }

    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return FALSE;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d = {};
        if (SUCCEEDED(adapter->GetDesc1(&d)) &&
            !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            d.VendorId != 0x1414 /* Microsoft: WARP, Basic Render, Hyper-V video */ &&
            (!found || d.DedicatedVideoMemory > best_mem)) {
            found = TRUE;
            best_mem = d.DedicatedVideoMemory;
            *out = d.AdapterLuid;
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, (int)name_cap, NULL, NULL);
        }
        adapter->Release();
        adapter = nullptr;
    }
    factory->Release();
    return found;
}

/* Ask IddCx to compose on the hardware GPU. Returns TRUE once that is done
   (or nothing more can be done). Runs on a WDF thread. */
static BOOL VddApplyRenderAdapter(VDD_DEVICE_CONTEXT* ctx)
{
    LUID luid = {};
    char name[128] = "";
    if (!ctx->hAdapter) return FALSE;
#pragma warning(suppress: 4127)
    if (!IDD_IS_FUNCTION_AVAILABLE(IddCxAdapterSetRenderAdapter)) {
        VddLog("RenderAdapter: IddCxAdapterSetRenderAdapter not available, OS default kept");
        return TRUE;
    }
    if (!VddFindRenderAdapter(&luid, name, sizeof(name))) {
        VddLog("RenderAdapter: no hardware GPU yet (attempt %u)", ctx->renderAttempts);
        return FALSE;
    }
    if (luid.LowPart == ctx->renderLuid.LowPart && luid.HighPart == ctx->renderLuid.HighPart)
        return TRUE;
    {
        IDARG_IN_ADAPTERSETRENDERADAPTER in = {};
        in.PreferredRenderAdapter = luid;
        IddCxAdapterSetRenderAdapter(ctx->hAdapter, &in);
    }
    ctx->renderLuid = luid;
    VddLog("RenderAdapter: composing on \"%s\" (LUID=%08X:%08X)", name, luid.HighPart, luid.LowPart);
    return TRUE;
}

static VOID VddRenderTimerCallback(WDFTIMER Timer)
{
    RenderTimerContext* t = WdfObjectGet_RenderTimerContext(Timer);
    VDD_DEVICE_CONTEXT* ctx = t->pContext;
    if (!ctx) return;
    ctx->renderAttempts++;
    /* GPU-PV drivers are copied in and started after boot: keep looking for
       a few minutes. */
    if (!VddApplyRenderAdapter(ctx) && ctx->renderAttempts < 60)
        WdfTimerStart(ctx->hRenderTimer, WDF_REL_TIMEOUT_IN_SEC(5));
}

static void VddFinishInit(VDD_DEVICE_CONTEXT* ctx)
{
    VddLog("FinishInit: adapter ready (ctx=%p)", (void*)ctx);
    if (ctx->hRenderTimer)
        WdfTimerStart(ctx->hRenderTimer, WDF_REL_TIMEOUT_IN_MS(100));
}

/* ========================================================================= */
/*  CreateMonitor (called after adapter init finished)                       */
/* ========================================================================= */

static NTSTATUS VddCreateMonitor(VDD_DEVICE_CONTEXT* ctx)
{
    VddLog("CreateMonitor: entered (ctx=%p, adapter=%p)", (void*)ctx, (void*)ctx->hAdapter);

    IDDCX_MONITOR_INFO monitorInfo = {};
    monitorInfo.Size = sizeof(monitorInfo);
    monitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    monitorInfo.ConnectorIndex = 0;

    monitorInfo.MonitorDescription.Size     = sizeof(monitorInfo.MonitorDescription);
    monitorInfo.MonitorDescription.Type     = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    monitorInfo.MonitorDescription.DataSize = 128;
    monitorInfo.MonitorDescription.pData    = (void*)VddGetEdid();

    VddLog("CreateMonitor: EDID checksum byte=0x%02X, IDDCX_MONITOR_INFO.Size=%u",
           ((const BYTE*)monitorInfo.MonitorDescription.pData)[127],
           (unsigned)monitorInfo.Size);

    /* Generate a container GUID */
    HRESULT hrGuid = CoCreateGuid(&monitorInfo.MonitorContainerId);
    VddLog("CreateMonitor: CoCreateGuid hr=0x%08X", hrGuid);

    WDF_OBJECT_ATTRIBUTES monAttrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&monAttrs, MonitorContextWrapper);

    IDARG_IN_MONITORCREATE createIn = {};
    createIn.ObjectAttributes = &monAttrs;
    createIn.pMonitorInfo     = &monitorInfo;

    IDARG_OUT_MONITORCREATE createOut = {};

    VddLog("CreateMonitor: calling IddCxMonitorCreate...");
    NTSTATUS st = IddCxMonitorCreate(ctx->hAdapter, &createIn, &createOut);
    if (!NT_SUCCESS(st))
    {
        VddLog("CreateMonitor: IddCxMonitorCreate FAILED st=0x%08X", st);
        return st;
    }
    VddLog("CreateMonitor: IddCxMonitorCreate succeeded (monitor=%p)", (void*)createOut.MonitorObject);

    ctx->hMonitor = createOut.MonitorObject;

    /* Store context on the monitor object */
    MonitorContextWrapper* pMonCtx = WdfObjectGet_MonitorContextWrapper(createOut.MonitorObject);
    pMonCtx->pContext = ctx;

    VddLog("CreateMonitor: calling IddCxMonitorArrival...");
    IDARG_OUT_MONITORARRIVAL arrivalOut = {};
    st = IddCxMonitorArrival(ctx->hMonitor, &arrivalOut);
    if (!NT_SUCCESS(st))
    {
        VddLog("CreateMonitor: IddCxMonitorArrival FAILED st=0x%08X", st);
        return st;
    }

    VddLog("CreateMonitor: monitor created and arrived successfully");

    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  Monitor recovery: depart + re-arrive if no AssignSwapChain after Unassign*/
/*  Uses WDF timer so callback runs on a proper WDF thread — IddCx APIs     */
/*  crash when called from raw CreateThread threads in UMDF.                 */
/* ========================================================================= */

static VOID VddRecoveryTimerCallback(WDFTIMER Timer)
{
    RecoveryTimerContext* timerCtx = WdfObjectGet_RecoveryTimerContext(Timer);
    VDD_DEVICE_CONTEXT* ctx = timerCtx->pContext;

    /* If a swap chain was assigned in the meantime, nothing to do */
    if (ctx->pSwapProc) {
        VddLog("Recovery: swap chain already reassigned, skipping");
        return;
    }

    /* No swap chain arrived yet.  This is normal during session transitions
       (logoff → login screen) — DWM may take longer than 5s to start
       compositing.  Do NOT call WdfDeviceSetFailed here; that permanently
       bricks the device and requires a full PnP restart.  The monitor stays
       arrived and IddCx will call AssignSwapChain when DWM is ready. */
    VddLog("Recovery: no AssignSwapChain after timeout — waiting (device remains available)");
}

/* ========================================================================= */
/*  Monitor re-plug after a new custom mode was added                        */
/*  The OS reads monitor modes at arrival only, so a mode that was not in    */
/*  the list needs a departure + arrival. Runs on a WDF timer thread.        */
/* ========================================================================= */

/* Offer the current list as the monitor's target modes without a re-plug.
   Returns TRUE when the OS accepted the update. */
static BOOL VddUpdateTargetModes(VDD_DEVICE_CONTEXT* ctx)
{
    VDD_MODE modes[VDD_MAX_MODES];
    UINT n = VddSnapshotModes(ctx, modes, nullptr);
    NTSTATUS st = STATUS_NOT_SUPPORTED;

#pragma warning(suppress: 4127)
    if (IDD_IS_FUNCTION_AVAILABLE(IddCxMonitorUpdateModes2)) {
        IDDCX_TARGET_MODE2 tm[VDD_MAX_MODES];
        IDARG_IN_UPDATEMODES2 in = {};
        memset(tm, 0, sizeof(tm));
        for (UINT i = 0; i < n; i++) {
            tm[i].Size = sizeof(IDDCX_TARGET_MODE2);
            tm[i].BitsPerComponent.Rgb = IDDCX_BITS_PER_COMPONENT_8;
            VddCreateMonitorMode(&tm[i].TargetVideoSignalInfo.targetVideoSignalInfo, &modes[i], 1);
        }
        in.Reason = IDDCX_UPDATE_REASON_OTHER;
        in.TargetModeCount = n;
        in.pTargetModes = tm;
        st = IddCxMonitorUpdateModes2(ctx->hMonitor, &in);
    } else
#pragma warning(suppress: 4127)
    if (IDD_IS_FUNCTION_AVAILABLE(IddCxMonitorUpdateModes)) {
        IDDCX_TARGET_MODE tm[VDD_MAX_MODES];
        IDARG_IN_UPDATEMODES in = {};
        memset(tm, 0, sizeof(tm));
        for (UINT i = 0; i < n; i++) {
            tm[i].Size = sizeof(IDDCX_TARGET_MODE);
            VddCreateMonitorMode(&tm[i].TargetVideoSignalInfo.targetVideoSignalInfo, &modes[i], 1);
        }
        in.Reason = IDDCX_UPDATE_REASON_OTHER;
        in.TargetModeCount = n;
        in.pTargetModes = tm;
        st = IddCxMonitorUpdateModes(ctx->hMonitor, &in);
    }
    VddLog("ModeUpdate: IddCxMonitorUpdateModes (%u modes) st=0x%08X", n, st);
    return NT_SUCCESS(st);
}

static VOID VddModeTimerCallback(WDFTIMER Timer)
{
    ModeTimerContext* timerCtx = WdfObjectGet_ModeTimerContext(Timer);
    VDD_DEVICE_CONTEXT* ctx = timerCtx->pContext;
    NTSTATUS st;

    if (!ctx || !ctx->hMonitor || !ctx->hAdapter) {
        VddLog("ModeReplug: no monitor yet, the new list applies at the next arrival");
        InterlockedExchange(&ctx->modeStage, 0);
        return;
    }

    if (ctx->modeStage == 1) {
        /* Seamless first: no monitor re-plug, so windows stay where they are.
           The guest helper switches as soon as the mode is offered. */
        if (VddUpdateTargetModes(ctx)) {
            InterlockedExchange(&ctx->modeStage, 2);
            WdfTimerStart(ctx->hModeTimer, WDF_REL_TIMEOUT_IN_MS(2500));
            return;
        }
    } else if (ctx->modeStage == 2) {
        VDD_SWAP_PROC* proc = ctx->pSwapProc;
        UINT w = proc ? proc->frameWidth : 0, h = proc ? proc->frameHeight : 0;
        if (w == ctx->pendingMode.width && h == ctx->pendingMode.height) {
            VddLog("ModeUpdate: %ux%u active without a re-plug", w, h);
            InterlockedExchange(&ctx->modeStage, 0);
            return;
        }
        VddLog("ModeUpdate: still %ux%u after the update, re-plugging for %ux%u",
               w, h, ctx->pendingMode.width, ctx->pendingMode.height);
    }
    InterlockedExchange(&ctx->modeStage, 0);

    VddLog("ModeReplug: departing monitor %p", (void*)ctx->hMonitor);
    st = IddCxMonitorDeparture(ctx->hMonitor);
    if (!NT_SUCCESS(st)) {
        VddLog("ModeReplug: IddCxMonitorDeparture FAILED st=0x%08X", st);
        return;
    }
    ctx->hMonitor = NULL;

    st = VddCreateMonitor(ctx);
    VddLog("ModeReplug: monitor re-created st=0x%08X", st);
}

/* ========================================================================= */
/*  IddCx: Assign / Unassign Swap Chain                                     */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddMonitorAssignSwapChain(
    IDDCX_MONITOR MonitorObject,
    const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
    VddLog("AssignSwapChain: entered (MonitorObject=%p)", (void*)MonitorObject);

    MonitorContextWrapper* pMonCtx = WdfObjectGet_MonitorContextWrapper(MonitorObject);
    VDD_DEVICE_CONTEXT* ctx = pMonCtx->pContext;

    /* Cancel any pending recovery timer — a new swap chain is being assigned */
    if (ctx->hRecoveryTimer) {
        WdfTimerStop(ctx->hRecoveryTimer, FALSE);
        VddLog("AssignSwapChain: recovery timer cancelled");
    }

    /* Destroy existing swap proc if any */
    if (ctx->pSwapProc)
    {
        VddLog("AssignSwapChain: destroying existing swap proc");
        VddDestroySwapProc(ctx->pSwapProc);
        ctx->pSwapProc = nullptr;
        VddLog("AssignSwapChain: existing swap proc destroyed");
    }

    VddLog("AssignSwapChain: allocating new swap proc (%u bytes)", (unsigned)sizeof(VDD_SWAP_PROC));
    VDD_SWAP_PROC* proc = (VDD_SWAP_PROC*)malloc(sizeof(VDD_SWAP_PROC));
    if (!proc)
    {
        VddLog("AssignSwapChain: malloc FAILED");
        return STATUS_NO_MEMORY;
    }
    memset(proc, 0, sizeof(*proc));
    VddLog("AssignSwapChain: swap proc allocated at %p", (void*)proc);
    InitializeCriticalSection(&proc->sendLock);
    InitializeCriticalSection(&proc->queueLock);
    proc->sendingSlot = -1;
    proc->pendingSlot = -1;

    proc->hSwapChain      = pInArgs->hSwapChain;
    proc->hAvailableEvent = pInArgs->hNextSurfaceAvailable;
    proc->hTerminateEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    VddLog("AssignSwapChain: hSwapChain=%p hAvailableEvent=%p hTerminateEvent=%p",
           (void*)proc->hSwapChain, (void*)proc->hAvailableEvent, (void*)proc->hTerminateEvent);

    VddLog("AssignSwapChain: getting D3D11 device (LUID=%08X:%08X)...",
           pInArgs->RenderAdapterLuid.HighPart, pInArgs->RenderAdapterLuid.LowPart);

    /* Get or reuse cached D3D11 device (cached in ctx to survive swap chain
       transitions) */
    HRESULT hr = VddGetOrCreateD3DDevice(ctx, pInArgs->RenderAdapterLuid,
                                          &proc->pDevice, &proc->pDeviceContext);
    if (FAILED(hr))
    {
        VddLog("AssignSwapChain: D3D11 device creation FAILED hr=0x%08X", hr);
        if (proc->hTerminateEvent)
            CloseHandle(proc->hTerminateEvent);
        DeleteCriticalSection(&proc->queueLock);
        DeleteCriticalSection(&proc->sendLock);
        free(proc);
        return (NTSTATUS)0xC01E0012L; /* STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN */
    }
    VddLog("AssignSwapChain: D3D11 device ready (cached=%p)", (void*)proc->pDevice);

    /* Initialize network state */
    VddLog("AssignSwapChain: initializing network state");
    proc->hClientConn    = NULL;
    proc->hPendingConn   = NULL;
    proc->bStopNetwork   = FALSE;
    proc->frameSeq       = 0;

    proc->pContext       = ctx;

    /* Create staging texture for CPU readback at the preferred mode size; the
       frame loop re-creates it if the surface turns out to be another size. */
    {
        VDD_MODE modes[VDD_MAX_MODES];
        UINT pref = 0;
        UINT n = VddCurrentModes(ctx, modes, &pref);
        proc->frameWidth  = (n && pref < n) ? modes[pref].width  : VDD_WIDTH;
        proc->frameHeight = (n && pref < n) ? modes[pref].height : VDD_HEIGHT;
    }
    VddLog("AssignSwapChain: creating staging texture (%ux%u)...", proc->frameWidth, proc->frameHeight);
    hr = VddCreateStagingTexture(proc, proc->frameWidth, proc->frameHeight);
    if (FAILED(hr))
    {
        VddLog("AssignSwapChain: staging texture creation FAILED hr=0x%08X", hr);
        /* Don't release pDevice/pDeviceContext — they're cached in ctx */
        if (proc->hTerminateEvent)
            CloseHandle(proc->hTerminateEvent);
        DeleteCriticalSection(&proc->queueLock);
        DeleteCriticalSection(&proc->sendLock);
        free(proc);
        return (NTSTATUS)0xC01E0012L; /* STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN */
    }
    VddLog("AssignSwapChain: staging texture created");

    /* Setup hardware cursor BEFORE starting swap chain thread — the thread
       reads proc->hCursorEvent at startup to build its wait handle array */
    VddLog("AssignSwapChain: setting up hardware cursor...");
    proc->hMonitor = MonitorObject;
    proc->hCursorEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    proc->cursorBufSize = 256 * 256 * 4 * 2;  /* max 256x256, 2x for MASKED_COLOR double-height */
    proc->pCursorShapeBuffer = (PBYTE)malloc(proc->cursorBufSize);
    proc->lastShapeId = 0;

    if (proc->hCursorEvent && proc->pCursorShapeBuffer) {
        IDDCX_CURSOR_CAPS cursorCaps = {};
        IDARG_IN_SETUP_HWCURSOR hwCursorArgs = {};

        cursorCaps.Size = sizeof(cursorCaps);
        cursorCaps.ColorXorCursorSupport = IDDCX_XOR_CURSOR_SUPPORT_FULL;
        cursorCaps.AlphaCursorSupport = TRUE;
        cursorCaps.MaxX = 256;
        cursorCaps.MaxY = 256;

        hwCursorArgs.CursorInfo = cursorCaps;
        hwCursorArgs.hNewCursorDataAvailable = proc->hCursorEvent;

        VddLog("AssignSwapChain: calling IddCxMonitorSetupHardwareCursor...");
        NTSTATUS status = IddCxMonitorSetupHardwareCursor(MonitorObject, &hwCursorArgs);
        if (NT_SUCCESS(status)) {
            VddLog("AssignSwapChain: hardware cursor enabled (max 256x256)");
        } else {
            VddLog("AssignSwapChain: hardware cursor setup FAILED 0x%08X", status);
            CloseHandle(proc->hCursorEvent);
            proc->hCursorEvent = NULL;
        }
    } else {
        VddLog("AssignSwapChain: failed to allocate cursor resources (event=%p buf=%p)",
               (void*)proc->hCursorEvent, (void*)proc->pCursorShapeBuffer);
    }

    /* Frame sender: transmits while the swap chain thread captures. */
    proc->hSendEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    proc->hSendThread = proc->hSendEvent
        ? CreateThread(NULL, 0, VddSenderThread, proc, 0, NULL) : NULL;
    if (!proc->hSendThread) {
        VddLog("AssignSwapChain: sender thread FAILED err=%u", GetLastError());
        if (proc->hSendEvent) CloseHandle(proc->hSendEvent);
        proc->pStagingTex->Release();
        if (proc->hTerminateEvent) CloseHandle(proc->hTerminateEvent);
        if (proc->hCursorEvent) CloseHandle(proc->hCursorEvent);
        free(proc->pCursorShapeBuffer);
        DeleteCriticalSection(&proc->queueLock);
        DeleteCriticalSection(&proc->sendLock);
        free(proc);
        return (NTSTATUS)0xC01E0012L; /* STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN */
    }

    /* Start swap chain processing thread (cursor must be set up first) */
    VddLog("AssignSwapChain: creating swap chain thread...");
    proc->hThread = CreateThread(NULL, 0, VddSwapChainThread, proc, 0, NULL);
    if (!proc->hThread)
    {
        VddLog("AssignSwapChain: CreateThread FAILED err=%u", GetLastError());
        proc->bStopSend = TRUE;
        SetEvent(proc->hSendEvent);
        WaitForSingleObject(proc->hSendThread, 5000);
        CloseHandle(proc->hSendThread);
        CloseHandle(proc->hSendEvent);
        DeleteCriticalSection(&proc->queueLock);
        DeleteCriticalSection(&proc->sendLock);
        proc->pStagingTex->Release();
        /* Don't release pDevice/pDeviceContext — they're cached in ctx */
        if (proc->hTerminateEvent)
            CloseHandle(proc->hTerminateEvent);
        if (proc->hCursorEvent)
            CloseHandle(proc->hCursorEvent);
        free(proc->pCursorShapeBuffer);
        free(proc);
        return (NTSTATUS)0xC01E0012L; /* STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN */
    }
    VddLog("AssignSwapChain: swap chain thread created (handle=%p)", (void*)proc->hThread);

    /* Start network listener thread for direct host connection */
    VddLog("AssignSwapChain: creating network thread...");
    proc->hNetworkThread = CreateThread(NULL, 0, VddNetworkThread, proc, 0, NULL);
    if (!proc->hNetworkThread)
    {
        VddLog("AssignSwapChain: network thread CreateThread FAILED err=%u", GetLastError());
        /* Non-fatal: swap chain still works, just no remote display */
    }
    else
    {
        VddLog("AssignSwapChain: network thread created (handle=%p)", (void*)proc->hNetworkThread);
    }

    ctx->pSwapProc = proc;

    VddLog("AssignSwapChain: COMPLETE — swap chain assigned, all threads started");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VddMonitorUnassignSwapChain(
    IDDCX_MONITOR MonitorObject)
{
    MonitorContextWrapper* pMonCtx = WdfObjectGet_MonitorContextWrapper(MonitorObject);
    VDD_DEVICE_CONTEXT* ctx = pMonCtx->pContext;

    if (ctx->pSwapProc)
    {
        VddDestroySwapProc(ctx->pSwapProc);
        ctx->pSwapProc = nullptr;
    }

    VddLog("SwapChain unassigned — starting recovery timer (5s)");

    /* Start recovery timer: if IddCx doesn't call AssignSwapChain within
       5 seconds, the timer callback departs and re-arrives the monitor. */
    if (ctx->hRecoveryTimer) {
        WdfTimerStart(ctx->hRecoveryTimer, WDF_REL_TIMEOUT_IN_SEC(5));
    } else {
        VddLog("Recovery: no timer available (not created during DeviceAdd)");
    }

    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx: Parse Monitor Description (EDID)                                 */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddParseMonitorDescription(
    const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs,
    IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
    VDD_MODE modes[VDD_MAX_MODES];
    UINT preferred = 0;
    UINT n = VddCurrentModes(nullptr, modes, &preferred);

    VddLog("ParseMonitorDescription: InputCount=%u, modes=%u",
           pInArgs->MonitorModeBufferInputCount, n);

    pOutArgs->MonitorModeBufferOutputCount = n;
    if (pInArgs->MonitorModeBufferInputCount == 0)
        return STATUS_SUCCESS;
    if (pInArgs->MonitorModeBufferInputCount < n)
        return STATUS_BUFFER_TOO_SMALL;

    for (UINT i = 0; i < n; i++) {
        IDDCX_MONITOR_MODE* pMode = &pInArgs->pMonitorModes[i];
        pMode->Size = sizeof(IDDCX_MONITOR_MODE);
        pMode->Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
        VddCreateMonitorMode(&pMode->MonitorVideoSignalInfo, &modes[i], 0); /* vSyncDivider=0 for monitor modes */
    }

    pOutArgs->PreferredMonitorModeIdx = preferred;
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx: Get Default Modes                                                */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddMonitorGetDefaultModes(
    IDDCX_MONITOR MonitorObject,
    const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs,
    IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
    MonitorContextWrapper* pMonCtx = WdfObjectGet_MonitorContextWrapper(MonitorObject);
    VDD_MODE modes[VDD_MAX_MODES];
    UINT preferred = 0;
    UINT n = VddCurrentModes(pMonCtx ? pMonCtx->pContext : nullptr, modes, &preferred);

    VddLog("GetDefaultModes: InputCount=%u, modes=%u",
           pInArgs->DefaultMonitorModeBufferInputCount, n);

    pOutArgs->DefaultMonitorModeBufferOutputCount = n;
    pOutArgs->PreferredMonitorModeIdx = preferred;

    if (pInArgs->DefaultMonitorModeBufferInputCount == 0)
        return STATUS_SUCCESS;
    if (pInArgs->DefaultMonitorModeBufferInputCount < n)
        return STATUS_BUFFER_TOO_SMALL;

    for (UINT i = 0; i < n; i++) {
        IDDCX_MONITOR_MODE* pMode = &pInArgs->pDefaultMonitorModes[i];
        pMode->Size = sizeof(IDDCX_MONITOR_MODE);
        pMode->Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
        VddCreateMonitorMode(&pMode->MonitorVideoSignalInfo, &modes[i], 0); /* vSyncDivider=0 for monitor modes */
    }
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx: Query Target Modes (WAS MISSING - required by IddCx)             */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddMonitorQueryTargetModes(
    IDDCX_MONITOR MonitorObject,
    const IDARG_IN_QUERYTARGETMODES* pInArgs,
    IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
    MonitorContextWrapper* pMonCtx = WdfObjectGet_MonitorContextWrapper(MonitorObject);
    VDD_MODE modes[VDD_MAX_MODES];
    UINT n = VddCurrentModes(pMonCtx ? pMonCtx->pContext : nullptr, modes, nullptr);

    VddLog("QueryTargetModes: InputCount=%u, modes=%u", pInArgs->TargetModeBufferInputCount, n);

    pOutArgs->TargetModeBufferOutputCount = n;
    if (pInArgs->TargetModeBufferInputCount == 0)
        return STATUS_SUCCESS;
    if (pInArgs->TargetModeBufferInputCount < n)
        return STATUS_BUFFER_TOO_SMALL;

    for (UINT i = 0; i < n; i++) {
        IDDCX_TARGET_MODE* pMode = &pInArgs->pTargetModes[i];
        pMode->Size = sizeof(IDDCX_TARGET_MODE);
        VddCreateMonitorMode(&pMode->TargetVideoSignalInfo.targetVideoSignalInfo, &modes[i], 1); /* vSyncDivider=1 for TARGET modes */
    }
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx: Commit Modes (v1 fallback)                                       */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddAdapterCommitModes(
    IDDCX_ADAPTER AdapterObject,
    const IDARG_IN_COMMITMODES* pInArgs)
{
    UNREFERENCED_PARAMETER(AdapterObject);
    UNREFERENCED_PARAMETER(pInArgs);
    VddLog("CommitModes (v1): called");
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx v2: Commit Modes 2                                                */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddAdapterCommitModes2(
    IDDCX_ADAPTER AdapterObject,
    const IDARG_IN_COMMITMODES2* pInArgs)
{
    UNREFERENCED_PARAMETER(AdapterObject);

    VddLog("CommitModes2 (v2): called, PathCount=%u", pInArgs->PathCount);
    for (UINT i = 0; i < pInArgs->PathCount; i++) {
        UINT flags = (UINT)pInArgs->pPaths[i].Flags;
        BOOL active = (flags & IDDCX_PATH_FLAGS_ACTIVE) != 0;
        BOOL changed = (flags & IDDCX_PATH_FLAGS_CHANGED) != 0;
        VddLog("CommitModes2: path[%u] monitor=%p flags=0x%X (active=%d changed=%d)",
               i, (void*)pInArgs->pPaths[i].MonitorObject, flags, active, changed);
    }

    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx v2: Parse Monitor Description 2                                   */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddParseMonitorDescription2(
    const IDARG_IN_PARSEMONITORDESCRIPTION2* pInArgs,
    IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
    VDD_MODE modes[VDD_MAX_MODES];
    UINT preferred = 0;
    UINT n = VddCurrentModes(nullptr, modes, &preferred);

    VddLog("ParseMonitorDescription2: InputCount=%u, modes=%u",
           pInArgs->MonitorModeBufferInputCount, n);

    pOutArgs->MonitorModeBufferOutputCount = n;
    if (pInArgs->MonitorModeBufferInputCount == 0)
        return STATUS_SUCCESS;
    if (pInArgs->MonitorModeBufferInputCount < n)
        return STATUS_BUFFER_TOO_SMALL;

    for (UINT i = 0; i < n; i++) {
        IDDCX_MONITOR_MODE2* pMode = &pInArgs->pMonitorModes[i];
        pMode->Size = sizeof(IDDCX_MONITOR_MODE2);
        pMode->Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
        VddCreateMonitorMode(&pMode->MonitorVideoSignalInfo, &modes[i], 0);
        pMode->BitsPerComponent.Rgb = IDDCX_BITS_PER_COMPONENT_8;
    }

    pOutArgs->PreferredMonitorModeIdx = preferred;
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx v2: Query Target Modes 2                                          */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddMonitorQueryTargetModes2(
    IDDCX_MONITOR MonitorObject,
    const IDARG_IN_QUERYTARGETMODES2* pInArgs,
    IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
    MonitorContextWrapper* pMonCtx = WdfObjectGet_MonitorContextWrapper(MonitorObject);
    VDD_MODE modes[VDD_MAX_MODES];
    UINT n = VddCurrentModes(pMonCtx ? pMonCtx->pContext : nullptr, modes, nullptr);

    VddLog("QueryTargetModes2: InputCount=%u, modes=%u", pInArgs->TargetModeBufferInputCount, n);

    pOutArgs->TargetModeBufferOutputCount = n;
    if (pInArgs->TargetModeBufferInputCount == 0)
        return STATUS_SUCCESS;
    if (pInArgs->TargetModeBufferInputCount < n)
        return STATUS_BUFFER_TOO_SMALL;

    for (UINT i = 0; i < n; i++) {
        IDDCX_TARGET_MODE2* pMode = &pInArgs->pTargetModes[i];
        pMode->Size = sizeof(IDDCX_TARGET_MODE2);
        pMode->BitsPerComponent.Rgb = IDDCX_BITS_PER_COMPONENT_8;
        VddCreateMonitorMode(&pMode->TargetVideoSignalInfo.targetVideoSignalInfo, &modes[i], 1);
    }
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx v2: Adapter Query Target Info                                     */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddAdapterQueryTargetInfo(
    IDDCX_ADAPTER AdapterObject,
    IDARG_IN_QUERYTARGET_INFO* pInArgs,
    IDARG_OUT_QUERYTARGET_INFO* pOutArgs)
{
    UNREFERENCED_PARAMETER(AdapterObject);
    UNREFERENCED_PARAMETER(pInArgs);

    VddLog("QueryTargetInfo: called (ConnectorIndex=%u)", pInArgs->ConnectorIndex);

    pOutArgs->TargetCaps = (IDDCX_TARGET_CAPS)0;
    pOutArgs->DitheringSupport.Rgb = IDDCX_BITS_PER_COMPONENT_8;

    VddLog("QueryTargetInfo: returning caps=0, dithering=8bpc");
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx v2: Set Default HDR Metadata (no-op, SDR only)                    */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddMonitorSetDefaultHdrMetadata(
    IDDCX_MONITOR MonitorObject,
    const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA* pInArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);
    UNREFERENCED_PARAMETER(pInArgs);
    VddLog("SetDefaultHdrMetadata: called (no-op, SDR only)");
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx v2: Set Gamma Ramp (no-op)                                        */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddMonitorSetGammaRamp(
    IDDCX_MONITOR MonitorObject,
    const IDARG_IN_SET_GAMMARAMP* pInArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);
    UNREFERENCED_PARAMETER(pInArgs);
    VddLog("SetGammaRamp: called (no-op)");
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx: Adapter Init Finished                                            */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddAdapterInitFinished(
    IDDCX_ADAPTER AdapterObject,
    const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
    IndirectDeviceContextWrapper* pWrapper =
        WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject);

    VddLog("AdapterInitFinished: AdapterInitStatus=0x%08X", pInArgs->AdapterInitStatus);

    if (NT_SUCCESS(pInArgs->AdapterInitStatus))
    {
        VddLog("AdapterInitFinished: adapter init succeeded, creating monitor...");
        VddFinishInit(pWrapper->pContext);
        NTSTATUS st = VddCreateMonitor(pWrapper->pContext);
        VddLog("AdapterInitFinished: VddCreateMonitor returned 0x%08X", st);
        return st;
    }

    VddLog("AdapterInitFinished: adapter init FAILED, not creating monitor");
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  WDF: D0 Entry (device power-up)                                          */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddDeviceD0Entry(
    WDFDEVICE Device,
    WDF_POWER_DEVICE_STATE PreviousState)
{
    VddLog("D0Entry: entered (Device=%p, PreviousState=%d)", (void*)Device, (int)PreviousState);

    /* Set GPU scheduling priority to realtime for this process (WUDFHost.exe) */
    {
        NTSTATUS st = D3DKMTSetProcessSchedulingPriorityClass(
            GetCurrentProcess(), D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME);
        VddLog("D0Entry: D3DKMTSetProcessSchedulingPriorityClass(REALTIME) = 0x%08X", st);
    }

    IndirectDeviceContextWrapper* pWrapper =
        WdfObjectGet_IndirectDeviceContextWrapper(Device);

    if (pWrapper && pWrapper->pContext)
    {
        VddLog("D0Entry: have context, calling VddInitAdapter");
        VddInitAdapter(pWrapper->pContext);
    }
    else
    {
        VddLog("D0Entry: WARNING - no context (pWrapper=%p)", (void*)pWrapper);
    }

    VddLog("D0Entry: returning STATUS_SUCCESS");
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  WDF: EvtCleanupCallback                                                  */
/* ========================================================================= */

static void EvtCleanupCallback(WDFOBJECT Object)
{
    VddLog("EvtCleanupCallback: entered (Object=%p)", (void*)Object);
    IndirectDeviceContextWrapper* pWrapper =
        WdfObjectGet_IndirectDeviceContextWrapper(Object);
    if (pWrapper && pWrapper->pContext)
    {
        VddLog("EvtCleanupCallback: cleaning up context %p", (void*)pWrapper->pContext);
        VddContextCleanup(pWrapper->pContext);
        free(pWrapper->pContext);
        pWrapper->pContext = NULL;
    }
    else
    {
        VddLog("EvtCleanupCallback: no context to clean up");
    }
}

/* ========================================================================= */
/*  WDF: Device Add                                                          */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddDeviceAdd(
    WDFDRIVER Driver,
    PWDFDEVICE_INIT pDeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    IDD_CX_CLIENT_CONFIG iddConfig;
    WDF_OBJECT_ATTRIBUTES attr;
    WDFDEVICE hDevice = NULL;
    IndirectDeviceContextWrapper* pWrapper;
    VDD_DEVICE_CONTEXT* ctx;
    NTSTATUS st;

    UNREFERENCED_PARAMETER(Driver);

    VddLog("DeviceAdd: entered");

    /* PnP power callbacks FIRST */
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDeviceD0Entry = VddDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &pnpCallbacks);
    VddLog("DeviceAdd: PnP callbacks set");

    /* Configure IddCx callbacks */
    IDD_CX_CLIENT_CONFIG_INIT(&iddConfig);

    iddConfig.EvtIddCxAdapterInitFinished               = VddAdapterInitFinished;
    iddConfig.EvtIddCxMonitorGetDefaultDescriptionModes  = VddMonitorGetDefaultModes;
    iddConfig.EvtIddCxMonitorAssignSwapChain             = VddMonitorAssignSwapChain;
    iddConfig.EvtIddCxMonitorUnassignSwapChain           = VddMonitorUnassignSwapChain;

    /* Use v2 callbacks when compiled with IddCx 1.10+ */
    if (IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterQueryTargetInfo)) {
        VddLog("DeviceAdd: registering v2 callbacks (IddCx 1.10+)");
        iddConfig.EvtIddCxAdapterQueryTargetInfo            = VddAdapterQueryTargetInfo;
        iddConfig.EvtIddCxMonitorSetDefaultHdrMetaData      = VddMonitorSetDefaultHdrMetadata;
        iddConfig.EvtIddCxParseMonitorDescription2          = VddParseMonitorDescription2;
        iddConfig.EvtIddCxMonitorQueryTargetModes2          = VddMonitorQueryTargetModes2;
        iddConfig.EvtIddCxAdapterCommitModes2               = VddAdapterCommitModes2;
        iddConfig.EvtIddCxMonitorSetGammaRamp               = VddMonitorSetGammaRamp;
    } else {
        VddLog("DeviceAdd: registering v1 callbacks");
        iddConfig.EvtIddCxParseMonitorDescription           = VddParseMonitorDescription;
        iddConfig.EvtIddCxMonitorQueryTargetModes            = VddMonitorQueryTargetModes;
        iddConfig.EvtIddCxAdapterCommitModes                = VddAdapterCommitModes;
    }

    st = IddCxDeviceInitConfig(pDeviceInit, &iddConfig);
    if (!NT_SUCCESS(st))
    {
        VddLog("DeviceAdd: IddCxDeviceInitConfig FAILED st=0x%08X", st);
        return st;
    }
    VddLog("DeviceAdd: IddCxDeviceInitConfig succeeded");

    /* Create WDFDEVICE with IndirectDeviceContextWrapper + EvtCleanupCallback */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, IndirectDeviceContextWrapper);
    attr.EvtCleanupCallback = EvtCleanupCallback;

    st = WdfDeviceCreate(&pDeviceInit, &attr, &hDevice);
    if (!NT_SUCCESS(st))
    {
        VddLog("DeviceAdd: WdfDeviceCreate FAILED st=0x%08X", st);
        return st;
    }
    VddLog("DeviceAdd: WdfDeviceCreate succeeded (hDevice=%p)", (void*)hDevice);

    st = IddCxDeviceInitialize(hDevice);
    if (!NT_SUCCESS(st))
    {
        VddLog("DeviceAdd: IddCxDeviceInitialize FAILED st=0x%08X", st);
        return st;
    }
    VddLog("DeviceAdd: IddCxDeviceInitialize succeeded");

    /* Heap-allocate device context */
    ctx = (VDD_DEVICE_CONTEXT*)malloc(sizeof(VDD_DEVICE_CONTEXT));
    if (!ctx)
    {
        VddLog("DeviceAdd: malloc for VDD_DEVICE_CONTEXT FAILED");
        return STATUS_NO_MEMORY;
    }

    VddContextInit(ctx, hDevice);

    pWrapper = WdfObjectGet_IndirectDeviceContextWrapper(hDevice);
    pWrapper->pContext = ctx;

    /* Create WDF timer for monitor recovery — fires on a proper WDF thread
       so IddCx APIs (MonitorDeparture, MonitorCreate, MonitorArrival) are safe. */
    {
        WDF_TIMER_CONFIG timerConfig;
        WDF_OBJECT_ATTRIBUTES timerAttrs;
        WDFTIMER hTimer = NULL;

        WDF_TIMER_CONFIG_INIT(&timerConfig, VddRecoveryTimerCallback);
        timerConfig.AutomaticSerialization = FALSE;

        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&timerAttrs, RecoveryTimerContext);
        timerAttrs.ParentObject = hDevice;

        st = WdfTimerCreate(&timerConfig, &timerAttrs, &hTimer);
        if (NT_SUCCESS(st)) {
            RecoveryTimerContext* timerCtx = WdfObjectGet_RecoveryTimerContext(hTimer);
            timerCtx->pContext = ctx;
            ctx->hRecoveryTimer = hTimer;
            VddLog("DeviceAdd: recovery timer created");
        } else {
            VddLog("DeviceAdd: WdfTimerCreate FAILED st=0x%08X (recovery disabled)", st);
        }
    }

    /* Timer that moves composition onto the hardware GPU (see VddApplyRenderAdapter). */
    {
        WDF_TIMER_CONFIG timerConfig;
        WDF_OBJECT_ATTRIBUTES timerAttrs;
        WDFTIMER hTimer = NULL;

        WDF_TIMER_CONFIG_INIT(&timerConfig, VddRenderTimerCallback);
        timerConfig.AutomaticSerialization = FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&timerAttrs, RenderTimerContext);
        timerAttrs.ParentObject = hDevice;
        st = WdfTimerCreate(&timerConfig, &timerAttrs, &hTimer);
        if (NT_SUCCESS(st)) {
            WdfObjectGet_RenderTimerContext(hTimer)->pContext = ctx;
            ctx->hRenderTimer = hTimer;
        } else {
            VddLog("DeviceAdd: render timer WdfTimerCreate FAILED st=0x%08X (OS default adapter)", st);
        }
    }

    /* Timer that re-plugs the monitor after a new custom mode was added. */
    {
        WDF_TIMER_CONFIG timerConfig;
        WDF_OBJECT_ATTRIBUTES timerAttrs;
        WDFTIMER hTimer = NULL;

        WDF_TIMER_CONFIG_INIT(&timerConfig, VddModeTimerCallback);
        timerConfig.AutomaticSerialization = FALSE;

        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&timerAttrs, ModeTimerContext);
        timerAttrs.ParentObject = hDevice;

        st = WdfTimerCreate(&timerConfig, &timerAttrs, &hTimer);
        if (NT_SUCCESS(st)) {
            ModeTimerContext* timerCtx = WdfObjectGet_ModeTimerContext(hTimer);
            timerCtx->pContext = ctx;
            ctx->hModeTimer = hTimer;
            VddLog("DeviceAdd: mode timer created");
        } else {
            VddLog("DeviceAdd: mode timer WdfTimerCreate FAILED st=0x%08X (custom modes need a reboot)", st);
        }
    }

    VddLog("DeviceAdd: complete, context=%p", (void*)ctx);
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  WDF: Driver Unload                                                       */
/* ========================================================================= */

_Use_decl_annotations_
VOID VddDriverUnload(WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    VddLog("DriverUnload: called");
}

/* ========================================================================= */
/*  DllMain                                                                  */
/* ========================================================================= */

extern "C" BOOL WINAPI DllMain(
    _In_ HINSTANCE hInstance,
    _In_ UINT dwReason,
    _In_opt_ LPVOID lpReserved)
{
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(lpReserved);
    if (dwReason == DLL_PROCESS_ATTACH) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        VddLog("DllMain: DLL_PROCESS_ATTACH (WSAStartup done)");
    } else if (dwReason == DLL_PROCESS_DETACH) {
        VddLog("DllMain: DLL_PROCESS_DETACH");
        WSACleanup();
    }
    return TRUE;
}

/* ========================================================================= */
/*  DriverEntry                                                              */
/* ========================================================================= */

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(
    PDRIVER_OBJECT pDriverObject,
    PUNICODE_STRING pRegistryPath)
{
    VddLog("DriverEntry: entered (pDriverObject=%p)", pDriverObject);

    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    WDF_DRIVER_CONFIG_INIT(&config, VddDeviceAdd);
    config.EvtDriverUnload = VddDriverUnload;

    NTSTATUS st = WdfDriverCreate(pDriverObject, pRegistryPath, &attributes, &config, WDF_NO_HANDLE);

    if (!NT_SUCCESS(st))
    {
        VddLog("DriverEntry: WdfDriverCreate FAILED st=0x%08X", st);
    }
    else
    {
        VddLog("DriverEntry: WdfDriverCreate succeeded");
    }

    return st;
}
