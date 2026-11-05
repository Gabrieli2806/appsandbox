/*
 * vdd_encoder.cpp -- hardware H.264 encoder for the performance display mode.
 * See vdd_encoder.h.
 *
 * Pipeline (all on the GPU):
 *   IddCx surface (BGRA) --CopyResource--> srcTex (BGRA, ours)
 *   srcTex --ID3D11VideoProcessor--> NV12 texture (ring)
 *   NV12 texture --IMFDXGIBuffer sample--> hardware encoder MFT --> Annex-B bytes
 *
 * Hardware encoder MFTs are asynchronous: they announce with events when they
 * want input (METransformNeedInput) and when output is ready
 * (METransformHaveOutput). A dedicated thread waits on those events and hands
 * the compressed access units to the caller's callback.
 */

#include <windows.h>
#include <initguid.h>   /* defines the MF / CODECAPI GUIDs in this file */
#include <d3d11_4.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <strmif.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vdd_encoder.h"
#include "ThirdParty/nvenc/nvEncodeAPI.h"   /* NVIDIA Video Codec SDK header (MIT) */

/* ---- NVENC, loaded at run time ----
   In GPU-PV guests the NVIDIA driver is not installed the normal way, so its
   Media Foundation encoder is never registered; the NVENC API itself
   (nvEncodeAPI64.dll, the path Sunshine and OBS use) works. */
typedef NVENCSTATUS (NVENCAPI *PFN_NvEncodeAPICreateInstance)(NV_ENCODE_API_FUNCTION_LIST*);
typedef NVENCSTATUS (NVENCAPI *PFN_NvEncodeAPIGetMaxSupportedVersion)(uint32_t*);

/* ---- Media Foundation, loaded at run time ---- */

typedef HRESULT (WINAPI *PFN_MFStartup)(ULONG, DWORD);
typedef HRESULT (WINAPI *PFN_MFShutdown)(void);
typedef HRESULT (WINAPI *PFN_MFTEnumEx)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO*,
                                        const MFT_REGISTER_TYPE_INFO*, IMFActivate***, UINT32*);
typedef HRESULT (WINAPI *PFN_MFCreateMediaType)(IMFMediaType**);
typedef HRESULT (WINAPI *PFN_MFCreateSample)(IMFSample**);
typedef HRESULT (WINAPI *PFN_MFCreateMemoryBuffer)(DWORD, IMFMediaBuffer**);
typedef HRESULT (WINAPI *PFN_MFCreateDXGISurfaceBuffer)(REFIID, IUnknown*, UINT, BOOL, IMFMediaBuffer**);
typedef HRESULT (WINAPI *PFN_MFCreateDXGIDeviceManager)(UINT*, IMFDXGIDeviceManager**);
typedef HRESULT (WINAPI *PFN_MFTEnum2)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO*,
                                       const MFT_REGISTER_TYPE_INFO*, IMFAttributes*,
                                       IMFActivate***, UINT32*);
typedef HRESULT (WINAPI *PFN_MFCreateAttributes)(IMFAttributes**, UINT32);

static struct {
    HMODULE                       dll;
    PFN_MFStartup                 Startup;
    PFN_MFShutdown                Shutdown;
    PFN_MFTEnumEx                 TEnumEx;
    PFN_MFCreateMediaType         CreateMediaType;
    PFN_MFCreateSample            CreateSample;
    PFN_MFCreateMemoryBuffer      CreateMemoryBuffer;
    PFN_MFCreateDXGISurfaceBuffer CreateDXGISurfaceBuffer;
    PFN_MFCreateDXGIDeviceManager CreateDXGIDeviceManager;
    PFN_MFTEnum2                  TEnum2;            /* optional (Windows 10 1703+) */
    PFN_MFCreateAttributes        CreateAttributes;  /* optional */
} g_mf;

static BOOL VddMfLoad(void)
{
    if (g_mf.dll) return TRUE;
    HMODULE dll = LoadLibraryExW(L"mfplat.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) return FALSE;
    g_mf.Startup  = (PFN_MFStartup)GetProcAddress(dll, "MFStartup");
    g_mf.Shutdown = (PFN_MFShutdown)GetProcAddress(dll, "MFShutdown");
    g_mf.TEnumEx  = (PFN_MFTEnumEx)GetProcAddress(dll, "MFTEnumEx");
    g_mf.CreateMediaType = (PFN_MFCreateMediaType)GetProcAddress(dll, "MFCreateMediaType");
    g_mf.CreateSample = (PFN_MFCreateSample)GetProcAddress(dll, "MFCreateSample");
    g_mf.CreateMemoryBuffer = (PFN_MFCreateMemoryBuffer)GetProcAddress(dll, "MFCreateMemoryBuffer");
    g_mf.CreateDXGISurfaceBuffer =
        (PFN_MFCreateDXGISurfaceBuffer)GetProcAddress(dll, "MFCreateDXGISurfaceBuffer");
    g_mf.CreateDXGIDeviceManager =
        (PFN_MFCreateDXGIDeviceManager)GetProcAddress(dll, "MFCreateDXGIDeviceManager");
    if (!g_mf.Startup || !g_mf.Shutdown || !g_mf.TEnumEx || !g_mf.CreateMediaType ||
        !g_mf.CreateSample || !g_mf.CreateMemoryBuffer || !g_mf.CreateDXGISurfaceBuffer ||
        !g_mf.CreateDXGIDeviceManager) {
        FreeLibrary(dll);
        memset(&g_mf, 0, sizeof(g_mf));
        return FALSE;
    }
    g_mf.TEnum2 = (PFN_MFTEnum2)GetProcAddress(dll, "MFTEnum2");
    g_mf.CreateAttributes = (PFN_MFCreateAttributes)GetProcAddress(dll, "MFCreateAttributes");
    g_mf.dll = dll;
    return TRUE;
}

/* MFT_ENUM_ADAPTER_LUID (mfapi.h, Windows 10 1703+), defined here because the
   driver build targets an older NTDDI. */
static const GUID VDD_MFT_ENUM_ADAPTER_LUID =
    { 0x1d39518c, 0xe220, 0x4da8, { 0xa0, 0x7f, 0xba, 0x17, 0x25, 0x52, 0xd6, 0xb1 } };

#define SAFE_RELEASE(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)

#define VDD_ENC_RING 6

struct VddEncoder {
    VddEncLogFn             log;
    VddEncOutputFn          out;
    void*                   user;
    char                    name[128];

    UINT                    width, height, fps, kbps;
    BOOL                    mfStarted;

    /* D3D (non-owning device/context, owned resources) */
    ID3D11Device*           device;
    ID3D11DeviceContext*    ctx;
    ID3D11VideoDevice*      vdev;
    ID3D11VideoContext*     vctx;
    ID3D11VideoProcessorEnumerator* vpEnum;
    ID3D11VideoProcessor*   vp;
    ID3D11Texture2D*        srcTex;
    ID3D11VideoProcessorInputView* srcView;
    ID3D11Texture2D*        nv12[VDD_ENC_RING];
    ID3D11VideoProcessorOutputView* nv12View[VDD_ENC_RING];
    UINT                    ringNext;
    BOOL                    haveFrame;

    /* Media Foundation */
    IMFDXGIDeviceManager*   devMgr;
    UINT                    resetToken;
    IMFTransform*           mft;
    IMFMediaEventGenerator* events;
    ICodecAPI*              codec;
    DWORD                   inId, outId;
    BOOL                    async;
    MFT_OUTPUT_STREAM_INFO  outInfo;
    BYTE*                   seqHeader;      /* SPS/PPS from the output type, if any */
    UINT                    seqHeaderLen;
    BYTE*                   scratch;        /* key frame with the sequence header prepended */
    UINT                    scratchCap;

    /* Event thread */
    HANDLE                  hThread;
    HANDLE                  hNeedInput;     /* auto-reset, signalled per NeedInput */
    volatile LONG           needInput;
    volatile LONG           stop;
    volatile LONG           failed;
    CRITICAL_SECTION        outLock;        /* ProcessOutput vs. stream change */

    LONGLONG                startQpc;
    LONGLONG                qpcFreq;
    UINT64                  framesIn, framesOut;
    BOOL                    pendingKey;

    /* NVENC backend (preferred; Media Foundation is the fallback) */
    BOOL                    nvenc;
    HMODULE                 nvDll;
    NV_ENCODE_API_FUNCTION_LIST nv;
    void*                   nvEnc;
    NV_ENC_OUTPUT_PTR       nvBits;
    NV_ENC_REGISTERED_PTR   nvReg[VDD_ENC_RING];
};

static void EncLog(VddEncoder* e, const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    if (!e || !e->log) return;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    e->log("%s", buf);
}

static void SetCodecU32(VddEncoder* e, const GUID* api, UINT32 value, const char* what)
{
    VARIANT v;
    if (!e->codec) return;
    ZeroMemory(&v, sizeof(v));
    v.vt = VT_UI4;
    v.ulVal = value;
    HRESULT hr = e->codec->SetValue(api, &v);
    if (FAILED(hr) && what)
        EncLog(e, "Encoder: %s=%u not accepted (0x%08X)", what, value, hr);
}

static void SetCodecBool(VddEncoder* e, const GUID* api, BOOL value, const char* what)
{
    VARIANT v;
    if (!e->codec) return;
    ZeroMemory(&v, sizeof(v));
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    HRESULT hr = e->codec->SetValue(api, &v);
    if (FAILED(hr) && what)
        EncLog(e, "Encoder: %s not accepted (0x%08X)", what, hr);
}

/* ---- Annex-B helpers ---- */

/* TRUE when the access unit already carries an SPS (NAL type 7). */
static BOOL HasSps(const BYTE* p, UINT len)
{
    for (UINT i = 0; i + 4 < len && i < 4096; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            if ((p[i + 3] & 0x1F) == 7) return TRUE;
            i += 2;
        }
    }
    return FALSE;
}

/* ---- Video processor (BGRA -> NV12) ---- */

static HRESULT CreateConverter(VddEncoder* e)
{
    HRESULT hr;
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate.Numerator = e->fps;
    cd.InputFrameRate.Denominator = 1;
    cd.InputWidth = e->width;
    cd.InputHeight = e->height;
    cd.OutputFrameRate = cd.InputFrameRate;
    cd.OutputWidth = e->width;
    cd.OutputHeight = e->height;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;

    hr = e->device->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&e->vdev);
    if (FAILED(hr)) { EncLog(e, "Encoder: no D3D11 video device (0x%08X)", hr); return hr; }
    hr = e->ctx->QueryInterface(__uuidof(ID3D11VideoContext), (void**)&e->vctx);
    if (FAILED(hr)) { EncLog(e, "Encoder: no D3D11 video context (0x%08X)", hr); return hr; }
    hr = e->vdev->CreateVideoProcessorEnumerator(&cd, &e->vpEnum);
    if (FAILED(hr)) { EncLog(e, "Encoder: video processor enumerator FAILED 0x%08X", hr); return hr; }

    UINT fmtFlags = 0;
    if (FAILED(e->vpEnum->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &fmtFlags)) ||
        !(fmtFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        EncLog(e, "Encoder: GPU cannot convert to NV12");
        return E_NOTIMPL;
    }
    hr = e->vdev->CreateVideoProcessor(e->vpEnum, 0, &e->vp);
    if (FAILED(hr)) { EncLog(e, "Encoder: CreateVideoProcessor FAILED 0x%08X", hr); return hr; }

    /* Full-range RGB in, BT.709 limited-range YUV out (what the host decoder
       assumes). */
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in = {};
    in.RGB_Range = 0;               /* 0-255 */
    in.YCbCr_Matrix = 1;            /* BT.709 */
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs = {};
    outCs.RGB_Range = 0;
    outCs.YCbCr_Matrix = 1;
    outCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    e->vctx->VideoProcessorSetStreamColorSpace(e->vp, 0, &in);
    e->vctx->VideoProcessorSetOutputColorSpace(e->vp, &outCs);
    e->vctx->VideoProcessorSetStreamFrameFormat(e->vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    e->vctx->VideoProcessorSetStreamAutoProcessingMode(e->vp, 0, FALSE);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = e->width;
    td.Height = e->height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = e->device->CreateTexture2D(&td, nullptr, &e->srcTex);
    if (FAILED(hr)) { EncLog(e, "Encoder: source texture FAILED 0x%08X", hr); return hr; }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    hr = e->vdev->CreateVideoProcessorInputView(e->srcTex, e->vpEnum, &ivd, &e->srcView);
    if (FAILED(hr)) { EncLog(e, "Encoder: input view FAILED 0x%08X", hr); return hr; }

    td.Format = DXGI_FORMAT_NV12;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    for (UINT i = 0; i < VDD_ENC_RING; i++) {
        hr = e->device->CreateTexture2D(&td, nullptr, &e->nv12[i]);
        if (FAILED(hr)) { EncLog(e, "Encoder: NV12 texture FAILED 0x%08X", hr); return hr; }
        hr = e->vdev->CreateVideoProcessorOutputView(e->nv12[i], e->vpEnum, &ovd, &e->nv12View[i]);
        if (FAILED(hr)) { EncLog(e, "Encoder: output view FAILED 0x%08X", hr); return hr; }
    }
    return S_OK;
}

/* ---- Encoder MFT ---- */

static HRESULT ReadSequenceHeader(VddEncoder* e)
{
    IMFMediaType* t = nullptr;
    UINT32 size = 0;
    free(e->seqHeader);
    e->seqHeader = nullptr;
    e->seqHeaderLen = 0;
    if (FAILED(e->mft->GetOutputCurrentType(e->outId, &t)) || !t) return S_OK;
    if (SUCCEEDED(t->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size > 0 && size < 4096) {
        e->seqHeader = (BYTE*)malloc(size);
        if (e->seqHeader && SUCCEEDED(t->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, e->seqHeader, size, &size)))
            e->seqHeaderLen = size;
    }
    t->Release();
    return S_OK;
}

static HRESULT SetTypes(VddEncoder* e, BOOL withProfile)
{
    HRESULT hr;
    IMFMediaType* t = nullptr;

    /* The declared rate stays within what H.264 levels describe (higher
       refresh rates simply produce more frames); the bit rate is scaled to
       match, so the per-frame budget is what was asked for. */
    UINT typeFps = e->fps > 60 ? 60 : e->fps;
    UINT typeKbps = (UINT)((UINT64)e->kbps * typeFps / (e->fps ? e->fps : 60));

    hr = g_mf.CreateMediaType(&t);
    if (FAILED(hr)) return hr;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    t->SetUINT32(MF_MT_AVG_BITRATE, typeKbps * 1000);
    MFSetAttributeSize(t, MF_MT_FRAME_SIZE, e->width, e->height);
    MFSetAttributeRatio(t, MF_MT_FRAME_RATE, typeFps, 1);
    MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    if (withProfile)
        t->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    hr = e->mft->SetOutputType(e->outId, t, 0);
    t->Release();
    if (FAILED(hr)) {
        EncLog(e, "Encoder: SetOutputType%s FAILED 0x%08X", withProfile ? " (High)" : "", hr);
        return hr;
    }

    hr = g_mf.CreateMediaType(&t);
    if (FAILED(hr)) return hr;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(t, MF_MT_FRAME_SIZE, e->width, e->height);
    MFSetAttributeRatio(t, MF_MT_FRAME_RATE, typeFps, 1);
    MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    hr = e->mft->SetInputType(e->inId, t, 0);
    t->Release();
    if (FAILED(hr)) {
        EncLog(e, "Encoder: SetInputType NV12 FAILED 0x%08X", hr);
        return hr;
    }
    return S_OK;
}

static HRESULT ConfigureCodec(VddEncoder* e)
{
    if (FAILED(e->mft->QueryInterface(__uuidof(ICodecAPI), (void**)&e->codec)))
        e->codec = nullptr;
    if (!e->codec) {
        EncLog(e, "Encoder: no ICodecAPI, using encoder defaults");
        return S_OK;
    }
    /* Interactive streaming: one frame in, one frame out, no B-frames. */
    SetCodecBool(e, &CODECAPI_AVLowLatencyMode, TRUE, "low latency");
    SetCodecU32(e, &CODECAPI_AVEncCommonRateControlMode,
                eAVEncCommonRateControlMode_CBR, "CBR");
    SetCodecU32(e, &CODECAPI_AVEncCommonMeanBitRate, e->kbps * 1000, "bit rate");
    SetCodecU32(e, &CODECAPI_AVEncMPVDefaultBPictureCount, 0, "no B-frames");
    /* Long GOP: the transport is lossless, so key frames are only needed when
       the host (re)starts decoding; it asks for one then. */
    SetCodecU32(e, &CODECAPI_AVEncMPVGOPSize, e->fps * 10, "GOP");
    SetCodecU32(e, &CODECAPI_AVEncCommonQualityVsSpeed, 30, nullptr);
    return S_OK;
}

static void ForceKey(VddEncoder* e)
{
    SetCodecU32(e, &CODECAPI_AVEncVideoForceKeyFrame, 1, nullptr);
}

/* Pull every ready output from the MFT. Returns FALSE on a fatal error. */
static BOOL DrainOutput(VddEncoder* e)
{
    for (int guard = 0; guard < 16; guard++) {
        MFT_OUTPUT_DATA_BUFFER ob = {};
        IMFSample* own = nullptr;
        DWORD status = 0;
        HRESULT hr;

        ob.dwStreamID = e->outId;
        if (!(e->outInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                    MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
            IMFMediaBuffer* mb = nullptr;
            DWORD cb = e->outInfo.cbSize ? e->outInfo.cbSize : e->width * e->height * 2;
            if (FAILED(g_mf.CreateSample(&own))) return FALSE;
            if (FAILED(g_mf.CreateMemoryBuffer(cb, &mb))) { own->Release(); return FALSE; }
            own->AddBuffer(mb);
            mb->Release();
            ob.pSample = own;
        }

        EnterCriticalSection(&e->outLock);
        hr = e->mft->ProcessOutput(0, 1, &ob, &status);
        LeaveCriticalSection(&e->outLock);
        if (ob.pEvents) { ob.pEvents->Release(); ob.pEvents = nullptr; }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            SAFE_RELEASE(own);
            return TRUE;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            IMFMediaType* t = nullptr;
            SAFE_RELEASE(own);
            if (SUCCEEDED(e->mft->GetOutputAvailableType(e->outId, 0, &t)) && t) {
                e->mft->SetOutputType(e->outId, t, 0);
                t->Release();
            }
            e->mft->GetOutputStreamInfo(e->outId, &e->outInfo);
            ReadSequenceHeader(e);
            EncLog(e, "Encoder: output format changed");
            continue;
        }
        if (FAILED(hr)) {
            SAFE_RELEASE(own);
            if (ob.pSample && ob.pSample != own) ob.pSample->Release();
            EncLog(e, "Encoder: ProcessOutput FAILED 0x%08X", hr);
            return FALSE;
        }

        IMFSample* s = ob.pSample;
        if (s) {
            IMFMediaBuffer* buf = nullptr;
            if (SUCCEEDED(s->ConvertToContiguousBuffer(&buf)) && buf) {
                BYTE* p = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(buf->Lock(&p, nullptr, &len)) && p && len) {
                    BOOL key = MFGetAttributeUINT32(s, MFSampleExtension_CleanPoint, FALSE) != 0;
                    if (key && e->seqHeaderLen && !HasSps(p, len)) {
                        UINT need = e->seqHeaderLen + len;
                        if (e->scratchCap < need) {
                            BYTE* nb = (BYTE*)realloc(e->scratch, need);
                            if (nb) { e->scratch = nb; e->scratchCap = need; }
                        }
                        if (e->scratchCap >= need) {
                            memcpy(e->scratch, e->seqHeader, e->seqHeaderLen);
                            memcpy(e->scratch + e->seqHeaderLen, p, len);
                            e->out(e->user, e->scratch, need, TRUE, e->width, e->height);
                        }
                    } else {
                        e->out(e->user, p, len, key, e->width, e->height);
                    }
                    if (e->framesOut++ == 0)
                        EncLog(e, "Encoder: first frame encoded (%u bytes, key=%d)", len, key);
                    buf->Unlock();
                }
                buf->Release();
            }
            if (s != own) s->Release();
        }
        SAFE_RELEASE(own);
        if (e->async) return TRUE;   /* one HaveOutput event = one sample */
    }
    return TRUE;
}

static DWORD WINAPI EventThread(LPVOID param)
{
    VddEncoder* e = (VddEncoder*)param;
    while (!e->stop) {
        IMFMediaEvent* ev = nullptr;
        HRESULT hr = e->events->GetEvent(0, &ev);     /* blocks; IMFShutdown wakes it */
        if (FAILED(hr) || !ev) {
            if (!e->stop) {
                EncLog(e, "Encoder: GetEvent FAILED 0x%08X", hr);
                InterlockedExchange(&e->failed, 1);
            }
            break;
        }
        MediaEventType type = MEUnknown;
        ev->GetType(&type);
        HRESULT status = S_OK;
        ev->GetStatus(&status);
        ev->Release();

        if (FAILED(status)) {
            EncLog(e, "Encoder: event %u failed 0x%08X", (UINT)type, status);
            InterlockedExchange(&e->failed, 1);
            break;
        }
        if (type == METransformNeedInput) {
            InterlockedIncrement(&e->needInput);
            SetEvent(e->hNeedInput);
        } else if (type == METransformHaveOutput) {
            if (!DrainOutput(e)) { InterlockedExchange(&e->failed, 1); break; }
        }
    }
    SetEvent(e->hNeedInput);   /* never leave a waiter hanging */
    return 0;
}

static HRESULT OpenEncoder(VddEncoder* e)
{
    HRESULT hr;
    IMFActivate** list = nullptr;
    UINT32 count = 0;
    MFT_REGISTER_TYPE_INFO inInfo = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO outInfo = { MFMediaType_Video, MFVideoFormat_H264 };

    /* Only encoders on the GPU this device lives on: with two GPUs in the
       guest (e.g. NVIDIA and Intel) the first hardware encoder can belong to
       the other one, which cannot read our textures. */
    hr = E_NOTIMPL;
    if (g_mf.TEnum2 && g_mf.CreateAttributes) {
        IDXGIDevice* dxgi = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IMFAttributes* filter = nullptr;
        DXGI_ADAPTER_DESC ad = {};
        if (SUCCEEDED(e->device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi)) &&
            SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&ad)) &&
            SUCCEEDED(g_mf.CreateAttributes(&filter, 1))) {
            char an[128];
            WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, an, sizeof(an), NULL, NULL);
            EncLog(e, "Encoder: looking for a hardware H.264 encoder on \"%s\"", an);
            filter->SetBlob(VDD_MFT_ENUM_ADAPTER_LUID, (const UINT8*)&ad.AdapterLuid, sizeof(LUID));
            hr = g_mf.TEnum2(MFT_CATEGORY_VIDEO_ENCODER,
                             MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                             &inInfo, &outInfo, filter, &list, &count);
            if (SUCCEEDED(hr) && count == 0) { if (list) CoTaskMemFree(list); list = nullptr; hr = MF_E_NOT_FOUND; }
        }
        SAFE_RELEASE(filter);
        SAFE_RELEASE(adapter);
        SAFE_RELEASE(dxgi);
    }
    if (FAILED(hr))
        hr = g_mf.TEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                          MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                          &inInfo, &outInfo, &list, &count);
    if (FAILED(hr) || count == 0) {
        EncLog(e, "Encoder: no hardware H.264 encoder found (hr=0x%08X, count=%u)", hr, count);
        if (list) CoTaskMemFree(list);
        return FAILED(hr) ? hr : MF_E_NOT_FOUND;
    }

    hr = MF_E_NOT_FOUND;
    for (UINT32 i = 0; i < count && !e->mft; i++) {
        IMFTransform* mft = nullptr;
        IMFAttributes* attrs = nullptr;
        WCHAR* wname = nullptr;
        UINT32 nameLen = 0;
        char name[128] = "hardware encoder";

        if (SUCCEEDED(list[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &wname, &nameLen)) && wname) {
            WideCharToMultiByte(CP_UTF8, 0, wname, -1, name, sizeof(name), NULL, NULL);
            CoTaskMemFree(wname);
        }
        hr = list[i]->ActivateObject(__uuidof(IMFTransform), (void**)&mft);
        if (FAILED(hr)) {
            EncLog(e, "Encoder: activating \"%s\" FAILED 0x%08X", name, hr);
            continue;
        }

        e->async = FALSE;
        if (SUCCEEDED(mft->GetAttributes(&attrs)) && attrs) {
            if (MFGetAttributeUINT32(attrs, MF_TRANSFORM_ASYNC, FALSE)) {
                attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                e->async = TRUE;
            }
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
            attrs->Release();
        }

        hr = mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)e->devMgr);
        if (FAILED(hr)) {
            EncLog(e, "Encoder: \"%s\" does not take D3D11 input (0x%08X)", name, hr);
            mft->Release();
            continue;
        }

        e->mft = mft;
        e->inId = 0;
        e->outId = 0;
        {
            DWORD in = 0, out = 0;
            if (mft->GetStreamIDs(1, &in, 1, &out) == S_OK) { e->inId = in; e->outId = out; }
        }
        ConfigureCodec(e);
        hr = SetTypes(e, TRUE);
        if (FAILED(hr)) hr = SetTypes(e, FALSE);
        if (FAILED(hr)) {
            SAFE_RELEASE(e->codec);
            mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
            mft->Release();
            e->mft = nullptr;
            continue;
        }
        strcpy_s(e->name, sizeof(e->name), name);
    }
    for (UINT32 i = 0; i < count; i++) list[i]->Release();
    CoTaskMemFree(list);
    if (!e->mft) return FAILED(hr) ? hr : MF_E_NOT_FOUND;

    e->mft->GetOutputStreamInfo(e->outId, &e->outInfo);
    ReadSequenceHeader(e);

    if (e->async) {
        hr = e->mft->QueryInterface(__uuidof(IMFMediaEventGenerator), (void**)&e->events);
        if (FAILED(hr)) { EncLog(e, "Encoder: async MFT without events (0x%08X)", hr); return hr; }
    }
    e->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    e->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    e->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return S_OK;
}

/* ---- NVENC backend ---- */

static HMODULE LoadNvEncode(VddEncoder* e)
{
    HMODULE h = LoadLibraryExW(L"nvEncodeAPI64.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (h) return h;
    /* GPU-PV guests: the host's driver folder is mapped under HostDriverStore. */
    wchar_t sys[MAX_PATH], pat[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    if (!GetSystemDirectoryW(sys, MAX_PATH)) return NULL;
    swprintf_s(pat, MAX_PATH, L"%s\\HostDriverStore\\FileRepository\\nv*", sys);
    HANDLE f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        swprintf_s(path, MAX_PATH, L"%s\\HostDriverStore\\FileRepository\\%s\\nvEncodeAPI64.dll",
                   sys, fd.cFileName);
        h = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (h) {
            char a[MAX_PATH];
            WideCharToMultiByte(CP_UTF8, 0, path, -1, a, sizeof(a), NULL, NULL);
            EncLog(e, "Encoder: NVENC from %s", a);
            break;
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return h;
}

static void CloseNvenc(VddEncoder* e)
{
    if (e->nvEnc) {
        for (UINT i = 0; i < VDD_ENC_RING; i++)
            if (e->nvReg[i]) { e->nv.nvEncUnregisterResource(e->nvEnc, e->nvReg[i]); e->nvReg[i] = nullptr; }
        if (e->nvBits) { e->nv.nvEncDestroyBitstreamBuffer(e->nvEnc, e->nvBits); e->nvBits = nullptr; }
        e->nv.nvEncDestroyEncoder(e->nvEnc);
        e->nvEnc = nullptr;
    }
    if (e->nvDll) { FreeLibrary(e->nvDll); e->nvDll = nullptr; }
    e->nvenc = FALSE;
}

static HRESULT OpenNvenc(VddEncoder* e)
{
    NVENCSTATUS st;
    e->nvDll = LoadNvEncode(e);
    if (!e->nvDll) {
        EncLog(e, "Encoder: NVENC not present (not an NVIDIA GPU, or its driver files are missing)");
        return E_NOTIMPL;
    }
    PFN_NvEncodeAPICreateInstance create =
        (PFN_NvEncodeAPICreateInstance)(void*)GetProcAddress(e->nvDll, "NvEncodeAPICreateInstance");
    PFN_NvEncodeAPIGetMaxSupportedVersion maxver =
        (PFN_NvEncodeAPIGetMaxSupportedVersion)(void*)GetProcAddress(e->nvDll, "NvEncodeAPIGetMaxSupportedVersion");
    if (!create || !maxver) { CloseNvenc(e); return E_NOTIMPL; }

    uint32_t have = 0;
    maxver(&have);
    uint32_t need = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    if (have < need) {
        EncLog(e, "Encoder: NVIDIA driver too old for NVENC API %u.%u (has %u.%u)",
               NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION, have >> 4, have & 0xF);
        CloseNvenc(e);
        return E_NOTIMPL;
    }
    memset(&e->nv, 0, sizeof(e->nv));
    e->nv.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    st = create(&e->nv);
    if (st != NV_ENC_SUCCESS) { EncLog(e, "Encoder: NvEncodeAPICreateInstance failed (%d)", (int)st); CloseNvenc(e); return E_FAIL; }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS op = {};
    op.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    op.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    op.device = e->device;
    op.apiVersion = NVENCAPI_VERSION;
    st = e->nv.nvEncOpenEncodeSessionEx(&op, &e->nvEnc);
    if (st != NV_ENC_SUCCESS) {
        EncLog(e, "Encoder: NVENC session on this GPU failed (%d)", (int)st);
        e->nvEnc = nullptr;
        CloseNvenc(e);
        return E_FAIL;
    }

    NV_ENC_PRESET_CONFIG pc = {};
    pc.version = NV_ENC_PRESET_CONFIG_VER;
    pc.presetCfg.version = NV_ENC_CONFIG_VER;
    st = e->nv.nvEncGetEncodePresetConfigEx(e->nvEnc, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P3_GUID,
                                            NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &pc);
    if (st != NV_ENC_SUCCESS) { EncLog(e, "Encoder: NVENC preset failed (%d)", (int)st); CloseNvenc(e); return E_FAIL; }

    NV_ENC_CONFIG cfg = pc.presetCfg;
    cfg.version = NV_ENC_CONFIG_VER;
    cfg.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
    cfg.gopLength = NVENC_INFINITE_GOPLENGTH;          /* key frames only on request */
    cfg.frameIntervalP = 1;                            /* no B-frames */
    cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    cfg.rcParams.averageBitRate = e->kbps * 1000;
    cfg.rcParams.maxBitRate = e->kbps * 1000;
    /* About two frames of buffer: low latency without starving big frames. */
    cfg.rcParams.vbvBufferSize = (uint32_t)((UINT64)e->kbps * 1000 * 2 / e->fps);
    cfg.rcParams.vbvInitialDelay = cfg.rcParams.vbvBufferSize;
    NV_ENC_CONFIG_H264* h = &cfg.encodeCodecConfig.h264Config;
    h->idrPeriod = NVENC_INFINITE_GOPLENGTH;
    h->repeatSPSPPS = 1;
    h->h264VUIParameters.videoSignalTypePresentFlag = 1;
    h->h264VUIParameters.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
    h->h264VUIParameters.videoFullRangeFlag = 0;
    h->h264VUIParameters.colourDescriptionPresentFlag = 1;
    h->h264VUIParameters.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
    h->h264VUIParameters.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
    h->h264VUIParameters.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;

    NV_ENC_INITIALIZE_PARAMS ip = {};
    ip.version = NV_ENC_INITIALIZE_PARAMS_VER;
    ip.encodeGUID = NV_ENC_CODEC_H264_GUID;
    ip.presetGUID = NV_ENC_PRESET_P3_GUID;
    ip.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    ip.encodeWidth = e->width;
    ip.encodeHeight = e->height;
    ip.darWidth = e->width;
    ip.darHeight = e->height;
    ip.maxEncodeWidth = e->width;
    ip.maxEncodeHeight = e->height;
    ip.frameRateNum = e->fps;
    ip.frameRateDen = 1;
    ip.enablePTD = 1;
    ip.encodeConfig = &cfg;
    st = e->nv.nvEncInitializeEncoder(e->nvEnc, &ip);
    if (st != NV_ENC_SUCCESS) {
        EncLog(e, "Encoder: NVENC initialize %ux%u failed (%d)", e->width, e->height, (int)st);
        CloseNvenc(e);
        return E_FAIL;
    }

    NV_ENC_CREATE_BITSTREAM_BUFFER bb = {};
    bb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    st = e->nv.nvEncCreateBitstreamBuffer(e->nvEnc, &bb);
    if (st != NV_ENC_SUCCESS) { EncLog(e, "Encoder: NVENC bitstream buffer failed (%d)", (int)st); CloseNvenc(e); return E_FAIL; }
    e->nvBits = bb.bitstreamBuffer;

    for (UINT i = 0; i < VDD_ENC_RING; i++) {
        NV_ENC_REGISTER_RESOURCE rr = {};
        rr.version = NV_ENC_REGISTER_RESOURCE_VER;
        rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        rr.width = e->width;
        rr.height = e->height;
        rr.resourceToRegister = e->nv12[i];
        rr.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
        rr.bufferUsage = NV_ENC_INPUT_IMAGE;
        st = e->nv.nvEncRegisterResource(e->nvEnc, &rr);
        if (st != NV_ENC_SUCCESS) { EncLog(e, "Encoder: NVENC register texture failed (%d)", (int)st); CloseNvenc(e); return E_FAIL; }
        e->nvReg[i] = rr.registeredResource;
    }
    e->nvenc = TRUE;
    strcpy_s(e->name, sizeof(e->name), "NVIDIA NVENC H.264");
    return S_OK;
}

/* Encode the NV12 texture in ring slot `slot` and hand the result to out().
   Synchronous: a few milliseconds even at 4K. */
static HRESULT NvencEncode(VddEncoder* e, UINT slot, BOOL key)
{
    NVENCSTATUS st;
    NV_ENC_MAP_INPUT_RESOURCE mi = {};
    mi.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mi.registeredResource = e->nvReg[slot];
    st = e->nv.nvEncMapInputResource(e->nvEnc, &mi);
    if (st != NV_ENC_SUCCESS) { EncLog(e, "Encoder: NVENC map failed (%d)", (int)st); return E_FAIL; }

    NV_ENC_PIC_PARAMS pp = {};
    pp.version = NV_ENC_PIC_PARAMS_VER;
    pp.inputBuffer = mi.mappedResource;
    pp.bufferFmt = mi.mappedBufferFmt;
    pp.inputWidth = e->width;
    pp.inputHeight = e->height;
    pp.outputBitstream = e->nvBits;
    pp.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pp.inputTimeStamp = e->framesIn;
    if (key) pp.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    st = e->nv.nvEncEncodePicture(e->nvEnc, &pp);
    if (st != NV_ENC_SUCCESS) {
        e->nv.nvEncUnmapInputResource(e->nvEnc, mi.mappedResource);
        EncLog(e, "Encoder: NVENC encode failed (%d)", (int)st);
        return E_FAIL;
    }

    NV_ENC_LOCK_BITSTREAM lb = {};
    lb.version = NV_ENC_LOCK_BITSTREAM_VER;
    lb.outputBitstream = e->nvBits;
    st = e->nv.nvEncLockBitstream(e->nvEnc, &lb);
    if (st == NV_ENC_SUCCESS) {
        BOOL idr = lb.pictureType == NV_ENC_PIC_TYPE_IDR;
        if (lb.bitstreamSizeInBytes)
            e->out(e->user, (const BYTE*)lb.bitstreamBufferPtr, lb.bitstreamSizeInBytes, idr,
                   e->width, e->height);
        if (e->framesOut++ == 0)
            EncLog(e, "Encoder: first frame encoded (%u bytes, key=%d)", lb.bitstreamSizeInBytes, idr);
        e->nv.nvEncUnlockBitstream(e->nvEnc, e->nvBits);
    } else {
        EncLog(e, "Encoder: NVENC lock bitstream failed (%d)", (int)st);
    }
    e->nv.nvEncUnmapInputResource(e->nvEnc, mi.mappedResource);
    return st == NV_ENC_SUCCESS ? S_OK : E_FAIL;
}

/* ---- Public API ---- */

HRESULT VddEncoderCreate(ID3D11Device* device, ID3D11DeviceContext* ctx,
                         UINT width, UINT height, UINT fps, UINT kbps,
                         VddEncOutputFn out, void* user, VddEncLogFn log,
                         VddEncoder** result)
{
    HRESULT hr;
    *result = nullptr;
    if (!device || !ctx || !out || width < 64 || height < 64 || (width & 1) || (height & 1))
        return E_INVALIDARG;
    VddEncoder* e = (VddEncoder*)calloc(1, sizeof(VddEncoder));
    if (!e) return E_OUTOFMEMORY;
    e->log = log;
    e->out = out;
    e->user = user;
    e->width = width;
    e->height = height;
    e->fps = fps ? fps : 60;
    if (!kbps) {
        /* About 0.15 bit per pixel per frame: ~19 Mbit/s at 1080p60,
           ~75 Mbit/s at 4K60. The link is local, so err on the side of
           quality. */
        UINT64 auto_kbps = (UINT64)width * height * e->fps * 15 / 100 / 1000;
        if (auto_kbps < 8000) auto_kbps = 8000;
        if (auto_kbps > 200000) auto_kbps = 200000;
        kbps = (UINT)auto_kbps;
    }
    e->kbps = kbps;
    e->device = device;
    e->ctx = ctx;
    InitializeCriticalSection(&e->outLock);
    e->hNeedInput = CreateEventW(NULL, FALSE, FALSE, NULL);
    {
        LARGE_INTEGER f, c;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        e->qpcFreq = f.QuadPart ? f.QuadPart : 1;
        e->startQpc = c.QuadPart;
    }

    /* The encoder uses the device from its own threads. */
    {
        ID3D11Multithread* mt = nullptr;
        if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Multithread), (void**)&mt)) && mt) {
            mt->SetMultithreadProtected(TRUE);
            mt->Release();
        }
    }

    hr = CreateConverter(e);
    if (FAILED(hr)) goto fail;

    /* NVIDIA: the NVENC API directly. */
    if (SUCCEEDED(OpenNvenc(e))) {
        EncLog(e, "Encoder: \"%s\" ready, %ux%u @ %u Hz, %u kbit/s",
               e->name, width, height, e->fps, e->kbps);
        e->pendingKey = TRUE;
        *result = e;
        return S_OK;
    }

    /* Other GPUs: a hardware encoder through Media Foundation. */
    if (!VddMfLoad()) {
        EncLog(e, "Encoder: Media Foundation is not available in this guest");
        hr = E_NOTIMPL;
        goto fail;
    }
    hr = g_mf.Startup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) { EncLog(e, "Encoder: MFStartup FAILED 0x%08X", hr); goto fail; }
    e->mfStarted = TRUE;

    hr = g_mf.CreateDXGIDeviceManager(&e->resetToken, &e->devMgr);
    if (FAILED(hr)) { EncLog(e, "Encoder: MFCreateDXGIDeviceManager FAILED 0x%08X", hr); goto fail; }
    hr = e->devMgr->ResetDevice(device, e->resetToken);
    if (FAILED(hr)) { EncLog(e, "Encoder: ResetDevice FAILED 0x%08X", hr); goto fail; }

    hr = OpenEncoder(e);
    if (FAILED(hr)) goto fail;

    if (e->async) {
        e->hThread = CreateThread(NULL, 0, EventThread, e, 0, NULL);
        if (!e->hThread) { hr = HRESULT_FROM_WIN32(GetLastError()); goto fail; }
    }

    EncLog(e, "Encoder: \"%s\" ready, %ux%u @ %u Hz, %u kbit/s, %s",
           e->name, width, height, e->fps, e->kbps, e->async ? "async" : "sync");
    e->pendingKey = TRUE;
    *result = e;
    return S_OK;

fail:
    VddEncoderDestroy(e);
    return FAILED(hr) ? hr : E_FAIL;
}

/* Convert srcTex into the next NV12 ring texture and feed it to the MFT. */
static HRESULT FeedFrame(VddEncoder* e, BOOL forceKey)
{
    HRESULT hr;
    if (e->failed) return E_FAIL;
    if (forceKey) e->pendingKey = TRUE;

    if (e->async) {
        /* Wait briefly for the encoder to ask for input; when it is still
           busy with the previous frame, skip this one (the stream stays
           valid, the host just sees one frame less). */
        if (e->needInput <= 0 && WaitForSingleObject(e->hNeedInput, 4) != 0 /* WAIT_OBJECT_0 */)
            return S_FALSE;
        if (e->failed) return E_FAIL;
        if (e->needInput <= 0) return S_FALSE;
    }

    UINT slot = e->ringNext;
    e->ringNext = (e->ringNext + 1) % VDD_ENC_RING;

    D3D11_VIDEO_PROCESSOR_STREAM st = {};
    st.Enable = TRUE;
    st.pInputSurface = e->srcView;
    hr = e->vctx->VideoProcessorBlt(e->vp, e->nv12View[slot], 0, 1, &st);
    if (FAILED(hr)) { EncLog(e, "Encoder: VideoProcessorBlt FAILED 0x%08X", hr); return hr; }

    if (e->nvenc) {
        BOOL key = e->pendingKey;
        e->pendingKey = FALSE;
        hr = NvencEncode(e, slot, key);
        if (FAILED(hr)) return hr;
        e->framesIn++;
        return S_OK;
    }

    IMFMediaBuffer* buf = nullptr;
    IMFSample* sample = nullptr;
    hr = g_mf.CreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), e->nv12[slot], 0, FALSE, &buf);
    if (FAILED(hr)) { EncLog(e, "Encoder: MFCreateDXGISurfaceBuffer FAILED 0x%08X", hr); return hr; }
    {
        IMF2DBuffer* b2 = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(buf->QueryInterface(__uuidof(IMF2DBuffer), (void**)&b2)) && b2) {
            if (SUCCEEDED(b2->GetContiguousLength(&len))) buf->SetCurrentLength(len);
            b2->Release();
        }
    }
    hr = g_mf.CreateSample(&sample);
    if (FAILED(hr)) { buf->Release(); return hr; }
    sample->AddBuffer(buf);
    buf->Release();

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    LONGLONG t = (LONGLONG)((double)(now.QuadPart - e->startQpc) * 10000000.0 / (double)e->qpcFreq);
    sample->SetSampleTime(t);
    sample->SetSampleDuration(10000000LL / e->fps);

    if (e->pendingKey) {
        ForceKey(e);
        e->pendingKey = FALSE;
    }

    hr = e->mft->ProcessInput(e->inId, sample, 0);
    sample->Release();
    if (hr == MF_E_NOTACCEPTING) {
        if (!e->async && !DrainOutput(e)) return E_FAIL;
        return S_FALSE;
    }
    if (FAILED(hr)) {
        EncLog(e, "Encoder: ProcessInput FAILED 0x%08X", hr);
        return hr;
    }
    if (e->async) InterlockedDecrement(&e->needInput);
    else if (!DrainOutput(e)) return E_FAIL;
    e->framesIn++;
    return S_OK;
}

HRESULT VddEncoderEncode(VddEncoder* e, ID3D11Texture2D* surface, BOOL forceKey)
{
    if (!e || !surface) return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC d;
    surface->GetDesc(&d);
    if (d.Width != e->width || d.Height != e->height) return E_INVALIDARG;
    if (d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
        d.Format != DXGI_FORMAT_B8G8R8X8_UNORM) {
        EncLog(e, "Encoder: unsupported surface format %u", (UINT)d.Format);
        InterlockedExchange(&e->failed, 1);
        return E_NOTIMPL;
    }
    e->ctx->CopyResource(e->srcTex, surface);
    e->haveFrame = TRUE;
    HRESULT hr = FeedFrame(e, forceKey);
    if (FAILED(hr)) InterlockedExchange(&e->failed, 1);
    return hr;
}

HRESULT VddEncoderRepeat(VddEncoder* e, BOOL forceKey)
{
    if (!e || !e->haveFrame) return S_FALSE;
    HRESULT hr = FeedFrame(e, forceKey);
    if (FAILED(hr)) InterlockedExchange(&e->failed, 1);
    return hr;
}

BOOL VddEncoderFailed(VddEncoder* e) { return !e || e->failed != 0; }
UINT VddEncoderWidth(VddEncoder* e) { return e ? e->width : 0; }
UINT VddEncoderHeight(VddEncoder* e) { return e ? e->height : 0; }
const char* VddEncoderName(VddEncoder* e) { return e ? e->name : ""; }

void VddEncoderDestroy(VddEncoder* e)
{
    if (!e) return;
    InterlockedExchange(&e->stop, 1);
    CloseNvenc(e);
    if (e->mft) {
        IMFShutdown* sd = nullptr;
        e->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        if (SUCCEEDED(e->mft->QueryInterface(__uuidof(IMFShutdown), (void**)&sd)) && sd) {
            sd->Shutdown();            /* wakes the event thread's GetEvent */
            sd->Release();
        }
    }
    if (e->hThread) {
        if (WaitForSingleObject(e->hThread, 3000) != 0 /* WAIT_OBJECT_0 */)
            EncLog(e, "Encoder: event thread did not stop");
        CloseHandle(e->hThread);
    }
    SAFE_RELEASE(e->events);
    SAFE_RELEASE(e->codec);
    if (e->mft) e->mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
    SAFE_RELEASE(e->mft);
    SAFE_RELEASE(e->devMgr);
    for (UINT i = 0; i < VDD_ENC_RING; i++) {
        SAFE_RELEASE(e->nv12View[i]);
        SAFE_RELEASE(e->nv12[i]);
    }
    SAFE_RELEASE(e->srcView);
    SAFE_RELEASE(e->srcTex);
    SAFE_RELEASE(e->vp);
    SAFE_RELEASE(e->vpEnum);
    SAFE_RELEASE(e->vctx);
    SAFE_RELEASE(e->vdev);
    if (e->mfStarted) g_mf.Shutdown();
    if (e->hNeedInput) CloseHandle(e->hNeedInput);
    DeleteCriticalSection(&e->outLock);
    free(e->seqHeader);
    free(e->scratch);
    if (e->log && (e->framesIn || e->framesOut))
        e->log("Encoder: closed after %llu frames in, %llu out",
               (unsigned long long)e->framesIn, (unsigned long long)e->framesOut);
    free(e);
}
