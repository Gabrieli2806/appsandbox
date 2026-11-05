/*
 * vm_video_decode.cpp -- H.264 decoding for the viewer's performance mode.
 * See vm_video_decode.h.
 *
 *   access unit --> MF H.264 decoder (DXVA on the host GPU) --> NV12 texture
 *   NV12 --ID3D11VideoProcessor--> BGRA texture --> drawn by the viewer
 *
 * Without DXVA the decoder hands back NV12 in system memory; it is uploaded
 * to a texture and converted the same way.
 */

#include <windows.h>
#include <initguid.h>
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

#include "vm_video_decode.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "ole32.lib")

#define SAFE_RELEASE(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)

struct VmVideoDecoder {
    VmVideoLogFn            log;
    void                   *log_user;
    BOOL                    com_init;
    BOOL                    mf_started;
    BOOL                    hw;

    UINT                    codec;                  /* VM_VIDEO_CODEC_* */
    UINT                    width, height;          /* picture size */
    UINT                    coded_w, coded_h;       /* decoder output size (may be padded) */

    ID3D11Device           *device;
    ID3D11DeviceContext    *ctx;
    ID3D11VideoDevice      *vdev;
    ID3D11VideoContext     *vctx;

    IMFDXGIDeviceManager   *dev_mgr;
    UINT                    reset_token;
    IMFTransform           *mft;
    MFT_OUTPUT_STREAM_INFO  out_info;
    LONGLONG                pts;

    /* NV12 -> BGRA */
    ID3D11VideoProcessorEnumerator *vp_enum;
    ID3D11VideoProcessor   *vp;
    UINT                    vp_w, vp_h;             /* input size vp was made for */
    ID3D11Texture2D        *out_tex[2];
    ID3D11ShaderResourceView *out_srv[2];
    ID3D11VideoProcessorOutputView *out_view[2];
    int                     cur;                    /* out_* holding the latest picture */

    /* Software decode path: NV12 written through a staging texture into a
       GPU texture the video processor reads. */
    ID3D11Texture2D        *upload;
    ID3D11Texture2D        *upload_gpu;
    UINT                    upload_w, upload_h;

    UINT64                  pictures;
};

static void dlog(VmVideoDecoder *d, const wchar_t *fmt, ...)
{
    wchar_t buf[512];
    va_list ap;
    if (!d || !d->log) return;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    d->log(d->log_user, buf);
}

/* Pick the NV12 output type and read the decoder's output size. */
static HRESULT set_output_nv12(VmVideoDecoder *d)
{
    for (DWORD i = 0; ; i++) {
        IMFMediaType *t = nullptr;
        GUID sub = GUID_NULL;
        HRESULT hr = d->mft->GetOutputAvailableType(0, i, &t);
        if (FAILED(hr)) return hr;
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub == MFVideoFormat_NV12) {
            hr = d->mft->SetOutputType(0, t, 0);
            if (SUCCEEDED(hr)) {
                UINT32 w = 0, h = 0;
                if (SUCCEEDED(MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h)) && w && h) {
                    d->coded_w = w;
                    d->coded_h = h;
                }
                d->mft->GetOutputStreamInfo(0, &d->out_info);
            }
            t->Release();
            return hr;
        }
        t->Release();
    }
}

static HRESULT ensure_output_textures(VmVideoDecoder *d)
{
    if (d->out_tex[0]) return S_OK;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = d->width;
    td.Height = d->height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (int i = 0; i < 2; i++) {
        HRESULT hr = d->device->CreateTexture2D(&td, nullptr, &d->out_tex[i]);
        if (FAILED(hr)) return hr;
        hr = d->device->CreateShaderResourceView(d->out_tex[i], nullptr, &d->out_srv[i]);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

/* Video processor for coded-size NV12 input -> picture-size BGRA output. */
static HRESULT ensure_converter(VmVideoDecoder *d, UINT in_w, UINT in_h)
{
    HRESULT hr;
    if (d->vp && d->vp_w == in_w && d->vp_h == in_h) return S_OK;
    SAFE_RELEASE(d->vp);
    SAFE_RELEASE(d->vp_enum);
    for (int i = 0; i < 2; i++) SAFE_RELEASE(d->out_view[i]);

    hr = ensure_output_textures(d);
    if (FAILED(hr)) return hr;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate.Numerator = 60;
    cd.InputFrameRate.Denominator = 1;
    cd.InputWidth = in_w;
    cd.InputHeight = in_h;
    cd.OutputFrameRate = cd.InputFrameRate;
    cd.OutputWidth = d->width;
    cd.OutputHeight = d->height;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    hr = d->vdev->CreateVideoProcessorEnumerator(&cd, &d->vp_enum);
    if (FAILED(hr)) return hr;
    hr = d->vdev->CreateVideoProcessor(d->vp_enum, 0, &d->vp);
    if (FAILED(hr)) return hr;

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    for (int i = 0; i < 2; i++) {
        hr = d->vdev->CreateVideoProcessorOutputView(d->out_tex[i], d->vp_enum, &ovd, &d->out_view[i]);
        if (FAILED(hr)) return hr;
    }

    /* The guest encodes BT.709 limited range. */
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in = {};
    in.YCbCr_Matrix = 1;
    in.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out = {};
    out.RGB_Range = 0;
    d->vctx->VideoProcessorSetStreamColorSpace(d->vp, 0, &in);
    d->vctx->VideoProcessorSetOutputColorSpace(d->vp, &out);
    d->vctx->VideoProcessorSetStreamFrameFormat(d->vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    d->vctx->VideoProcessorSetStreamAutoProcessingMode(d->vp, 0, FALSE);

    RECT src = { 0, 0, (LONG)(d->width < in_w ? d->width : in_w),
                       (LONG)(d->height < in_h ? d->height : in_h) };
    RECT dst = { 0, 0, (LONG)d->width, (LONG)d->height };
    d->vctx->VideoProcessorSetStreamSourceRect(d->vp, 0, TRUE, &src);
    d->vctx->VideoProcessorSetStreamDestRect(d->vp, 0, TRUE, &dst);
    d->vctx->VideoProcessorSetOutputTargetRect(d->vp, TRUE, &dst);

    d->vp_w = in_w;
    d->vp_h = in_h;
    return S_OK;
}

/* Convert one NV12 texture (array slice) into the next BGRA output. */
static HRESULT convert(VmVideoDecoder *d, ID3D11Texture2D *nv12, UINT slice)
{
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;
    nv12->GetDesc(&td);
    hr = ensure_converter(d, td.Width, td.Height);
    if (FAILED(hr)) {
        dlog(d, L"Video: converter for %ux%u FAILED (0x%08X)", td.Width, td.Height, hr);
        return hr;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.ArraySlice = slice;
    ID3D11VideoProcessorInputView *iv = nullptr;
    hr = d->vdev->CreateVideoProcessorInputView(nv12, d->vp_enum, &ivd, &iv);
    if (FAILED(hr)) {
        dlog(d, L"Video: input view FAILED (0x%08X)", hr);
        return hr;
    }
    D3D11_VIDEO_PROCESSOR_STREAM st = {};
    st.Enable = TRUE;
    st.pInputSurface = iv;
    int next = d->cur ^ 1;
    hr = d->vctx->VideoProcessorBlt(d->vp, d->out_view[next], 0, 1, &st);
    iv->Release();
    if (FAILED(hr)) {
        dlog(d, L"Video: VideoProcessorBlt FAILED (0x%08X)", hr);
        return hr;
    }
    d->cur = next;
    return S_OK;
}

/* Software decode: copy the NV12 picture into a texture, then convert. */
static HRESULT upload_and_convert(VmVideoDecoder *d, IMFMediaBuffer *buf)
{
    UINT w = d->coded_w ? d->coded_w : d->width;
    UINT h = d->coded_h ? d->coded_h : d->height;
    HRESULT hr;
    BYTE *src = nullptr;
    LONG pitch = 0;
    IMF2DBuffer *b2 = nullptr;
    DWORD len = 0;

    if (!d->upload || d->upload_w != w || d->upload_h != h) {
        SAFE_RELEASE(d->upload);
        SAFE_RELEASE(d->upload_gpu);
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = d->device->CreateTexture2D(&td, nullptr, &d->upload);
        if (FAILED(hr)) return hr;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.CPUAccessFlags = 0;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr = d->device->CreateTexture2D(&td, nullptr, &d->upload_gpu);
        if (FAILED(hr)) return hr;
        d->upload_w = w;
        d->upload_h = h;
    }

    if (SUCCEEDED(buf->QueryInterface(__uuidof(IMF2DBuffer), (void **)&b2)) && b2) {
        hr = b2->Lock2D(&src, &pitch);
        if (FAILED(hr)) { b2->Release(); return hr; }
    } else {
        hr = buf->Lock(&src, nullptr, &len);
        if (FAILED(hr)) return hr;
        pitch = (LONG)w;
        if (len < (DWORD)w * h * 3 / 2) { buf->Unlock(); return E_UNEXPECTED; }
    }

    D3D11_MAPPED_SUBRESOURCE m;
    hr = d->ctx->Map(d->upload, 0, D3D11_MAP_WRITE, 0, &m);
    if (SUCCEEDED(hr)) {
        BYTE *dst = (BYTE *)m.pData;
        UINT row_bytes = w < (UINT)pitch ? w : (UINT)pitch;
        if (row_bytes > m.RowPitch) row_bytes = m.RowPitch;
        for (UINT y = 0; y < h; y++)                 /* Y plane */
            memcpy(dst + (size_t)y * m.RowPitch, src + (size_t)y * pitch, row_bytes);
        for (UINT y = 0; y < h / 2; y++)             /* interleaved UV plane */
            memcpy(dst + (size_t)(h + y) * m.RowPitch, src + (size_t)(h + y) * pitch, row_bytes);
        d->ctx->Unmap(d->upload, 0);
    }
    if (b2) { b2->Unlock2D(); b2->Release(); }
    else buf->Unlock();
    if (FAILED(hr)) return hr;
    d->ctx->CopyResource(d->upload_gpu, d->upload);
    return convert(d, d->upload_gpu, 0);
}

/* Fetch every decoded picture. Returns 1 if one was produced, 0 if none,
   -1 on error. */
static int drain(VmVideoDecoder *d)
{
    int got = 0;
    for (int guard = 0; guard < 8; guard++) {
        MFT_OUTPUT_DATA_BUFFER ob = {};
        IMFSample *own = nullptr;
        DWORD status = 0;
        HRESULT hr;

        if (!(d->out_info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                     MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
            IMFMediaBuffer *mb = nullptr;
            DWORD cb = d->out_info.cbSize ? d->out_info.cbSize
                                          : (d->coded_w ? d->coded_w : d->width) *
                                            (d->coded_h ? d->coded_h : d->height) * 3 / 2;
            if (FAILED(MFCreateSample(&own))) return -1;
            if (FAILED(MFCreateMemoryBuffer(cb, &mb))) { own->Release(); return -1; }
            own->AddBuffer(mb);
            mb->Release();
            ob.pSample = own;
        }

        hr = d->mft->ProcessOutput(0, 1, &ob, &status);
        if (ob.pEvents) ob.pEvents->Release();

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            SAFE_RELEASE(own);
            return got;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            SAFE_RELEASE(own);
            if (FAILED(set_output_nv12(d))) return -1;
            continue;
        }
        if (FAILED(hr)) {
            SAFE_RELEASE(own);
            dlog(d, L"Video: ProcessOutput FAILED (0x%08X)", hr);
            return -1;
        }

        IMFSample *s = ob.pSample;
        if (s) {
            IMFMediaBuffer *buf = nullptr;
            if (SUCCEEDED(s->GetBufferByIndex(0, &buf)) && buf) {
                IMFDXGIBuffer *dx = nullptr;
                if (SUCCEEDED(buf->QueryInterface(__uuidof(IMFDXGIBuffer), (void **)&dx)) && dx) {
                    ID3D11Texture2D *tex = nullptr;
                    UINT slice = 0;
                    if (SUCCEEDED(dx->GetResource(__uuidof(ID3D11Texture2D), (void **)&tex)) && tex) {
                        dx->GetSubresourceIndex(&slice);
                        if (SUCCEEDED(convert(d, tex, slice))) got = 1;
                        tex->Release();
                    }
                    dx->Release();
                } else if (SUCCEEDED(upload_and_convert(d, buf))) {
                    got = 1;
                }
                buf->Release();
            }
            if (s != own) s->Release();
        }
        SAFE_RELEASE(own);
        if (got && d->pictures++ == 0)
            dlog(d, L"Video: first picture decoded (%ux%u, %s).", d->width, d->height,
                 d->hw ? L"GPU decoding" : L"CPU decoding");
    }
    return got;
}

extern "C" BOOL vm_video_hevc_supported(void)
{
    static LONG cached = -1;
    if (cached < 0) {
        MFT_REGISTER_TYPE_INFO in_info = { MFMediaType_Video, MFVideoFormat_HEVC };
        IMFActivate **list = nullptr;
        UINT32 count = 0;
        BOOL com = SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED));
        BOOL mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
        if (mf && SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                                      MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT |
                                      MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_LOCALMFT |
                                      MFT_ENUM_FLAG_SORTANDFILTER,
                                      &in_info, NULL, &list, &count))) {
            for (UINT32 i = 0; i < count; i++) list[i]->Release();
            CoTaskMemFree(list);
        }
        if (mf) MFShutdown();
        if (com) CoUninitialize();
        InterlockedExchange(&cached, count > 0 ? 1 : 0);
    }
    return cached == 1;
}

extern "C" VmVideoDecoder *vm_video_decoder_create(ID3D11Device *device, ID3D11DeviceContext *ctx,
                                                   UINT codec, UINT width, UINT height,
                                                   VmVideoLogFn log, void *log_user)
{
    VmVideoDecoder *d;
    HRESULT hr;
    IMFActivate **list = nullptr;
    UINT32 count = 0;
    MFT_REGISTER_TYPE_INFO in_info = { MFMediaType_Video,
                                       codec == VM_VIDEO_CODEC_HEVC ? MFVideoFormat_HEVC : MFVideoFormat_H264 };
    const wchar_t *cname = codec == VM_VIDEO_CODEC_HEVC ? L"HEVC" : L"H.264";

    if (!device || !ctx || width == 0 || height == 0) return NULL;
    d = (VmVideoDecoder *)calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->log = log;
    d->log_user = log_user;
    d->codec = codec == VM_VIDEO_CODEC_HEVC ? VM_VIDEO_CODEC_HEVC : VM_VIDEO_CODEC_H264;
    d->width = width;
    d->height = height;
    d->device = device;
    d->ctx = ctx;
    device->AddRef();
    ctx->AddRef();

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    d->com_init = SUCCEEDED(hr);

    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) { dlog(d, L"Video: MFStartup FAILED (0x%08X)", hr); goto fail; }
    d->mf_started = TRUE;

    hr = device->QueryInterface(__uuidof(ID3D11VideoDevice), (void **)&d->vdev);
    if (FAILED(hr)) { dlog(d, L"Video: the viewer's device has no video support (0x%08X)", hr); goto fail; }
    hr = ctx->QueryInterface(__uuidof(ID3D11VideoContext), (void **)&d->vctx);
    if (FAILED(hr)) goto fail;

    {
        ID3D11Multithread *mt = nullptr;
        if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Multithread), (void **)&mt)) && mt) {
            mt->SetMultithreadProtected(TRUE);
            mt->Release();
        }
    }

    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                   MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                   &in_info, NULL, &list, &count);
    if (FAILED(hr) || count == 0) {
        dlog(d, L"Video: no %s decoder on this PC (0x%08X).", cname, hr);
        if (list) CoTaskMemFree(list);
        goto fail;
    }
    for (UINT32 i = 0; i < count && !d->mft; i++) {
        IMFTransform *mft = nullptr;
        if (FAILED(list[i]->ActivateObject(__uuidof(IMFTransform), (void **)&mft))) continue;
        d->mft = mft;
    }
    for (UINT32 i = 0; i < count; i++) list[i]->Release();
    CoTaskMemFree(list);
    if (!d->mft) { dlog(d, L"Video: could not open an %s decoder.", cname); goto fail; }

    {
        IMFAttributes *attrs = nullptr;
        if (SUCCEEDED(d->mft->GetAttributes(&attrs)) && attrs) {
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
            if (MFGetAttributeUINT32(attrs, MF_SA_D3D11_AWARE, FALSE)) {
                if (SUCCEEDED(MFCreateDXGIDeviceManager(&d->reset_token, &d->dev_mgr)) &&
                    SUCCEEDED(d->dev_mgr->ResetDevice(device, d->reset_token)) &&
                    SUCCEEDED(d->mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                                     (ULONG_PTR)d->dev_mgr)))
                    d->hw = TRUE;
            }
            attrs->Release();
        }
        ICodecAPI *codec = nullptr;
        if (SUCCEEDED(d->mft->QueryInterface(__uuidof(ICodecAPI), (void **)&codec)) && codec) {
            VARIANT v;
            ZeroMemory(&v, sizeof(v));
            v.vt = VT_BOOL;
            v.boolVal = VARIANT_TRUE;
            codec->SetValue(&CODECAPI_AVLowLatencyMode, &v);
            codec->Release();
        }
    }

    {
        IMFMediaType *t = nullptr;
        hr = MFCreateMediaType(&t);
        if (FAILED(hr)) goto fail;
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, d->codec == VM_VIDEO_CODEC_HEVC ? MFVideoFormat_HEVC : MFVideoFormat_H264);
        MFSetAttributeSize(t, MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        hr = d->mft->SetInputType(0, t, 0);
        t->Release();
        if (FAILED(hr)) { dlog(d, L"Video: decoder refused %ux%u %s (0x%08X).", width, height, cname, hr); goto fail; }
    }
    d->coded_w = width;
    d->coded_h = height;
    hr = set_output_nv12(d);
    if (FAILED(hr)) { dlog(d, L"Video: decoder has no NV12 output (0x%08X).", hr); goto fail; }

    d->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    d->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    hr = ensure_output_textures(d);
    if (FAILED(hr)) goto fail;

    dlog(d, L"Video: %s decoder ready (%ux%u, %s).", cname, width, height,
         d->hw ? L"GPU" : L"CPU");
    return d;

fail:
    vm_video_decoder_destroy(d);
    return NULL;
}

extern "C" int vm_video_decoder_decode(VmVideoDecoder *d, const BYTE *data, UINT len)
{
    IMFSample *s = nullptr;
    IMFMediaBuffer *mb = nullptr;
    BYTE *p = nullptr;
    HRESULT hr;
    int got = 0, r;

    if (!d || !d->mft || !data || !len) return -1;
    if (FAILED(MFCreateSample(&s))) return -1;
    if (FAILED(MFCreateMemoryBuffer(len, &mb))) { s->Release(); return -1; }
    if (SUCCEEDED(mb->Lock(&p, nullptr, nullptr))) {
        memcpy(p, data, len);
        mb->Unlock();
        mb->SetCurrentLength(len);
    }
    s->AddBuffer(mb);
    mb->Release();
    s->SetSampleTime(d->pts);
    s->SetSampleDuration(166667);
    d->pts += 166667;

    hr = d->mft->ProcessInput(0, s, 0);
    if (hr == MF_E_NOTACCEPTING) {
        r = drain(d);
        if (r < 0) { s->Release(); return -1; }
        got |= r;
        hr = d->mft->ProcessInput(0, s, 0);
    }
    s->Release();
    if (FAILED(hr)) {
        dlog(d, L"Video: ProcessInput FAILED (0x%08X)", hr);
        return -1;
    }
    r = drain(d);
    if (r < 0) return -1;
    return got | r;
}

extern "C" ID3D11ShaderResourceView *vm_video_decoder_srv(VmVideoDecoder *d)
{
    return d ? d->out_srv[d->cur] : NULL;
}

extern "C" UINT vm_video_decoder_width(VmVideoDecoder *d) { return d ? d->width : 0; }
extern "C" UINT vm_video_decoder_codec(VmVideoDecoder *d) { return d ? d->codec : 0; }
extern "C" UINT vm_video_decoder_height(VmVideoDecoder *d) { return d ? d->height : 0; }
extern "C" BOOL vm_video_decoder_hardware(VmVideoDecoder *d) { return d ? d->hw : FALSE; }

extern "C" void vm_video_decoder_destroy(VmVideoDecoder *d)
{
    if (!d) return;
    if (d->mft) {
        d->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        if (d->dev_mgr) d->mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
    }
    SAFE_RELEASE(d->mft);
    SAFE_RELEASE(d->dev_mgr);
    for (int i = 0; i < 2; i++) {
        SAFE_RELEASE(d->out_view[i]);
        SAFE_RELEASE(d->out_srv[i]);
        SAFE_RELEASE(d->out_tex[i]);
    }
    SAFE_RELEASE(d->upload);
    SAFE_RELEASE(d->upload_gpu);
    SAFE_RELEASE(d->vp);
    SAFE_RELEASE(d->vp_enum);
    SAFE_RELEASE(d->vctx);
    SAFE_RELEASE(d->vdev);
    if (d->mf_started) MFShutdown();
    SAFE_RELEASE(d->ctx);
    SAFE_RELEASE(d->device);
    if (d->com_init) CoUninitialize();
    free(d);
}
