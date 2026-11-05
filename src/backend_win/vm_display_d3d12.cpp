/*
 * vm_display_d3d12.cpp -- optional Direct3D 12 presenter for the VM window.
 * See vm_display_d3d12.h.
 */

#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>

#include "vm_display_d3d12.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

#define SAFE_RELEASE(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)

/* A D3D11 texture shared with D3D12 (copy target on the D3D11 side). */
struct SharedTex {
    ID3D11Texture2D *tex11;
    ID3D12Resource  *res12;
    UINT             w, h, mips;
};

struct Dx12Presenter {
    Dx12LogFn                 log;
    void                     *log_user;
    wchar_t                   adapter[128];
    HWND                      hwnd;

    ID3D11Device             *dev11;
    ID3D11DeviceContext      *ctx11;
    ID3D11DeviceContext4     *ctx11_4;
    ID3D11Fence              *fence11;       /* shared: D3D11 signals copies done */
    ID3D12Fence              *fence11in12;
    UINT64                    copy_value;

    ID3D12Device             *dev;
    ID3D12CommandQueue       *queue;
    ID3D12CommandAllocator   *alloc;
    ID3D12GraphicsCommandList *list;
    ID3D12Fence              *fence;         /* frame completion */
    UINT64                    fence_value;
    HANDLE                    fence_event;

    IDXGISwapChain3          *swap;
    HANDLE                    latency_wait;
    ID3D12Resource           *back[2];
    ID3D12DescriptorHeap     *rtv_heap;
    UINT                      rtv_inc;
    ID3D12DescriptorHeap     *srv_heap;      /* t0 picture, t1 backdrop */
    UINT                      srv_inc;
    UINT                      width, height;

    ID3D12RootSignature      *root;
    ID3D12PipelineState      *pso_plain;
    ID3D12PipelineState      *pso_blend;

    SharedTex                 pic, bg;
};

static void plog(Dx12Presenter *p, const wchar_t *fmt, ...)
{
    wchar_t buf[512];
    va_list ap;
    if (!p || !p->log) return;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    p->log(p->log_user, buf);
}

static void wait_idle(Dx12Presenter *p)
{
    if (!p->queue || !p->fence) return;
    UINT64 v = ++p->fence_value;
    if (SUCCEEDED(p->queue->Signal(p->fence, v)) && p->fence->GetCompletedValue() < v) {
        p->fence->SetEventOnCompletion(v, p->fence_event);
        WaitForSingleObject(p->fence_event, 2000);
    }
}

static ID3DBlob *compile(Dx12Presenter *p, const char *src, const char *target)
{
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "main", target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        plog(p, L"D3D12: shader (%S) failed to compile: %S", target,
             err ? (const char *)err->GetBufferPointer() : "");
        SAFE_RELEASE(code);
    }
    SAFE_RELEASE(err);
    return code;
}

static BOOL create_targets(Dx12Presenter *p)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = p->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < 2; i++) {
        if (FAILED(p->swap->GetBuffer(i, __uuidof(ID3D12Resource), (void **)&p->back[i]))) return FALSE;
        p->dev->CreateRenderTargetView(p->back[i], nullptr, h);
        h.ptr += p->rtv_inc;
    }
    return TRUE;
}

static void release_targets(Dx12Presenter *p)
{
    for (UINT i = 0; i < 2; i++) SAFE_RELEASE(p->back[i]);
}

static void release_shared(SharedTex *t)
{
    SAFE_RELEASE(t->res12);
    SAFE_RELEASE(t->tex11);
    t->w = t->h = t->mips = 0;
}

/* Make t match `src` (size, mips) and write its SRV into heap slot `slot`. */
static BOOL ensure_shared(Dx12Presenter *p, SharedTex *t, ID3D11Texture2D *src, UINT slot)
{
    D3D11_TEXTURE2D_DESC sd;
    src->GetDesc(&sd);
    if (t->tex11 && t->w == sd.Width && t->h == sd.Height && t->mips == sd.MipLevels)
        return TRUE;

    wait_idle(p);                  /* the GPU may still read the old one */
    release_shared(t);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = sd.Width;
    td.Height = sd.Height;
    td.MipLevels = sd.MipLevels;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    HRESULT hr = p->dev11->CreateTexture2D(&td, nullptr, &t->tex11);
    if (FAILED(hr)) { plog(p, L"D3D12: shared texture %ux%u failed (0x%08X)", sd.Width, sd.Height, hr); return FALSE; }

    IDXGIResource1 *r1 = nullptr;
    HANDLE h = nullptr;
    hr = t->tex11->QueryInterface(__uuidof(IDXGIResource1), (void **)&r1);
    if (SUCCEEDED(hr)) hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                    nullptr, &h);
    SAFE_RELEASE(r1);
    if (SUCCEEDED(hr)) {
        hr = p->dev->OpenSharedHandle(h, __uuidof(ID3D12Resource), (void **)&t->res12);
        CloseHandle(h);
    }
    if (FAILED(hr)) { plog(p, L"D3D12: sharing a texture failed (0x%08X)", hr); release_shared(t); return FALSE; }

    D3D12_SHADER_RESOURCE_VIEW_DESC v = {};
    v.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.Texture2D.MipLevels = sd.MipLevels;
    D3D12_CPU_DESCRIPTOR_HANDLE dst = p->srv_heap->GetCPUDescriptorHandleForHeapStart();
    dst.ptr += (SIZE_T)slot * p->srv_inc;
    p->dev->CreateShaderResourceView(t->res12, &v, dst);

    t->w = sd.Width;
    t->h = sd.Height;
    t->mips = sd.MipLevels;
    return TRUE;
}

/* Copy a D3D11 SRV's texture into a shared texture. */
static BOOL copy_in(Dx12Presenter *p, SharedTex *t, ID3D11ShaderResourceView *srv, UINT slot)
{
    ID3D11Resource *res = nullptr;
    ID3D11Texture2D *tex = nullptr;
    BOOL ok = FALSE;
    srv->GetResource(&res);
    if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&tex)) &&
        ensure_shared(p, t, tex, slot)) {
        p->ctx11->CopyResource(t->tex11, tex);
        ok = TRUE;
    }
    SAFE_RELEASE(tex);
    SAFE_RELEASE(res);
    return ok;
}

static ID3D12PipelineState *make_pso(Dx12Presenter *p, ID3DBlob *vs, ID3DBlob *ps)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = p->root;
    d.VS.pShaderBytecode = vs->GetBufferPointer();
    d.VS.BytecodeLength = vs->GetBufferSize();
    d.PS.pShaderBytecode = ps->GetBufferPointer();
    d.PS.BytecodeLength = ps->GetBufferSize();
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = 0xFFFFFFFFu;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    ID3D12PipelineState *pso = nullptr;
    HRESULT hr = p->dev->CreateGraphicsPipelineState(&d, __uuidof(ID3D12PipelineState), (void **)&pso);
    if (FAILED(hr)) plog(p, L"D3D12: pipeline state failed (0x%08X)", hr);
    return pso;
}

extern "C" Dx12Presenter *dx12_create(HWND hwnd, ID3D11Device *dev11, ID3D11DeviceContext *ctx11,
                                      const char *vs_hlsl, const char *ps_hlsl, const char *ps_blend_hlsl,
                                      Dx12LogFn log, void *log_user)
{
    Dx12Presenter *p = (Dx12Presenter *)calloc(1, sizeof(Dx12Presenter));
    if (!p) return nullptr;
    HRESULT hr;
    IDXGIDevice *dxgi_dev = nullptr;
    IDXGIAdapter *adapter11 = nullptr;
    IDXGIFactory4 *factory = nullptr;
    IDXGIAdapter1 *adapter = nullptr;
    ID3D11Device5 *dev11_5 = nullptr;
    ID3DBlob *vs = nullptr, *ps = nullptr, *psb = nullptr;
    DXGI_ADAPTER_DESC ad = {};

    p->log = log;
    p->log_user = log_user;
    p->hwnd = hwnd;
    p->dev11 = dev11; dev11->AddRef();
    p->ctx11 = ctx11; ctx11->AddRef();

    /* Same GPU as the D3D11 device (textures are shared between them). */
    hr = dev11->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi_dev);
    if (SUCCEEDED(hr)) hr = dxgi_dev->GetAdapter(&adapter11);
    if (SUCCEEDED(hr)) hr = adapter11->GetDesc(&ad);
    if (SUCCEEDED(hr)) hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), (void **)&factory);
    if (SUCCEEDED(hr)) hr = factory->EnumAdapterByLuid(ad.AdapterLuid, __uuidof(IDXGIAdapter1), (void **)&adapter);
    if (SUCCEEDED(hr)) hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void **)&p->dev);
    SAFE_RELEASE(adapter);
    SAFE_RELEASE(adapter11);
    SAFE_RELEASE(dxgi_dev);
    if (FAILED(hr)) { plog(p, L"D3D12: no D3D12 device on this GPU (0x%08X)", hr); goto fail; }
    wcsncpy_s(p->adapter, ARRAYSIZE(p->adapter), ad.Description, _TRUNCATE);

    {
        D3D12_COMMAND_QUEUE_DESC q = {};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = p->dev->CreateCommandQueue(&q, __uuidof(ID3D12CommandQueue), (void **)&p->queue);
        if (SUCCEEDED(hr)) hr = p->dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                    __uuidof(ID3D12CommandAllocator), (void **)&p->alloc);
        if (SUCCEEDED(hr)) hr = p->dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, p->alloc, nullptr,
                                    __uuidof(ID3D12GraphicsCommandList), (void **)&p->list);
        if (SUCCEEDED(hr)) hr = p->list->Close();
        if (SUCCEEDED(hr)) hr = p->dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void **)&p->fence);
        p->fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (FAILED(hr) || !p->fence_event) { plog(p, L"D3D12: command objects failed (0x%08X)", hr); goto fail; }
    }

    /* Fence shared with D3D11: D3D12 waits until the frame copies land. */
    hr = dev11->QueryInterface(__uuidof(ID3D11Device5), (void **)&dev11_5);
    if (SUCCEEDED(hr)) hr = ctx11->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&p->ctx11_4);
    if (SUCCEEDED(hr)) hr = dev11_5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void **)&p->fence11);
    if (SUCCEEDED(hr)) {
        HANDLE h = nullptr;
        hr = p->fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h);
        if (SUCCEEDED(hr)) {
            hr = p->dev->OpenSharedHandle(h, __uuidof(ID3D12Fence), (void **)&p->fence11in12);
            CloseHandle(h);
        }
    }
    SAFE_RELEASE(dev11_5);
    if (FAILED(hr)) { plog(p, L"D3D12: shared fence failed (0x%08X)", hr); goto fail; }

    /* Flip-model swap chain, one frame of latency. */
    {
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        IDXGISwapChain1 *sc1 = nullptr;
        RECT rc;
        GetClientRect(hwnd, &rc);
        p->width = rc.right > 0 ? (UINT)rc.right : 1;
        p->height = rc.bottom > 0 ? (UINT)rc.bottom : 1;
        sd.Width = p->width;
        sd.Height = p->height;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.Scaling = DXGI_SCALING_NONE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        hr = factory->CreateSwapChainForHwnd(p->queue, hwnd, &sd, nullptr, nullptr, &sc1);
        if (SUCCEEDED(hr)) hr = sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void **)&p->swap);
        SAFE_RELEASE(sc1);
        if (FAILED(hr)) { plog(p, L"D3D12: swap chain failed (0x%08X)", hr); goto fail; }
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        p->swap->SetMaximumFrameLatency(1);
        p->latency_wait = p->swap->GetFrameLatencyWaitableObject();
    }

    {
        D3D12_DESCRIPTOR_HEAP_DESC h = {};
        h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        h.NumDescriptors = 2;
        hr = p->dev->CreateDescriptorHeap(&h, __uuidof(ID3D12DescriptorHeap), (void **)&p->rtv_heap);
        if (SUCCEEDED(hr)) {
            h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            h.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            hr = p->dev->CreateDescriptorHeap(&h, __uuidof(ID3D12DescriptorHeap), (void **)&p->srv_heap);
        }
        if (FAILED(hr)) { plog(p, L"D3D12: descriptor heaps failed (0x%08X)", hr); goto fail; }
        p->rtv_inc = p->dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        p->srv_inc = p->dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if (!create_targets(p)) { plog(p, L"D3D12: back buffers failed"); goto fail; }
        /* Null SRVs until real textures arrive. */
        D3D12_SHADER_RESOURCE_VIEW_DESC v = {};
        v.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE c = p->srv_heap->GetCPUDescriptorHandleForHeapStart();
        p->dev->CreateShaderResourceView(nullptr, &v, c);
        c.ptr += p->srv_inc;
        p->dev->CreateShaderResourceView(nullptr, &v, c);
    }

    /* Root signature: b0 = 16 constants, t0-t1 table, s0 linear clamp. */
    {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 2;
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 16;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &range;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC samp = {};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.MaxLOD = D3D12_FLOAT32_MAX;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd = {};
        rd.NumParameters = 2;
        rd.pParameters = params;
        rd.NumStaticSamplers = 1;
        rd.pStaticSamplers = &samp;
        ID3DBlob *blob = nullptr, *err = nullptr;
        hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        if (SUCCEEDED(hr))
            hr = p->dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             __uuidof(ID3D12RootSignature), (void **)&p->root);
        SAFE_RELEASE(blob);
        SAFE_RELEASE(err);
        if (FAILED(hr)) { plog(p, L"D3D12: root signature failed (0x%08X)", hr); goto fail; }
    }

    vs = compile(p, vs_hlsl, "vs_5_0");
    ps = compile(p, ps_hlsl, "ps_5_0");
    psb = ps_blend_hlsl ? compile(p, ps_blend_hlsl, "ps_5_0") : nullptr;
    if (!vs || !ps) goto fail;
    p->pso_plain = make_pso(p, vs, ps);
    if (psb) p->pso_blend = make_pso(p, vs, psb);    /* optional: no backdrop without it */
    SAFE_RELEASE(vs); SAFE_RELEASE(ps); SAFE_RELEASE(psb);
    if (!p->pso_plain) goto fail;

    SAFE_RELEASE(factory);
    plog(p, L"Renderer: Direct3D 12 on \"%s\".", p->adapter);
    return p;

fail:
    SAFE_RELEASE(vs); SAFE_RELEASE(ps); SAFE_RELEASE(psb);
    SAFE_RELEASE(factory);
    dx12_destroy(p);
    return nullptr;
}

extern "C" void dx12_resize(Dx12Presenter *p, UINT width, UINT height)
{
    if (!p || !p->swap || !width || !height || (width == p->width && height == p->height)) return;
    wait_idle(p);
    release_targets(p);
    HRESULT hr = p->swap->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN,
                                        DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    if (FAILED(hr)) plog(p, L"D3D12: ResizeBuffers failed (0x%08X)", hr);
    p->width = width;
    p->height = height;
    create_targets(p);
}

extern "C" BOOL dx12_present(Dx12Presenter *p, ID3D11ShaderResourceView *pic,
                             const float viewport[4], BOOL blend,
                             ID3D11ShaderResourceView *bg, const float consts[16])
{
    if (!p || !p->swap || !pic) return FALSE;
    if (blend && (!p->pso_blend || !bg)) blend = FALSE;

    /* D3D11 side: copy the picture (and backdrop) into the shared textures,
       then signal the shared fence. */
    if (!copy_in(p, &p->pic, pic, 0)) return FALSE;
    if (blend && !copy_in(p, &p->bg, bg, 1)) blend = FALSE;
    UINT64 copied = ++p->copy_value;
    p->ctx11_4->Signal(p->fence11, copied);
    p->ctx11->Flush();

    if (p->latency_wait) WaitForSingleObjectEx(p->latency_wait, 100, FALSE);

    UINT bi = p->swap->GetCurrentBackBufferIndex();
    if (!p->back[bi]) return FALSE;
    p->alloc->Reset();
    p->list->Reset(p->alloc, blend ? p->pso_blend : p->pso_plain);

    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = p->back[bi];
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    p->list->ResourceBarrier(1, &b);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = p->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)bi * p->rtv_inc;
    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    p->list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    p->list->ClearRenderTargetView(rtv, black, 0, nullptr);

    p->list->SetGraphicsRootSignature(p->root);
    ID3D12DescriptorHeap *heaps[] = { p->srv_heap };
    p->list->SetDescriptorHeaps(1, heaps);
    p->list->SetGraphicsRootDescriptorTable(1, p->srv_heap->GetGPUDescriptorHandleForHeapStart());
    if (blend && consts) p->list->SetGraphicsRoot32BitConstants(0, 16, consts, 0);

    D3D12_VIEWPORT vp = {};
    D3D12_RECT sc = { 0, 0, (LONG)p->width, (LONG)p->height };
    if (blend) {
        vp.Width = (float)p->width;
        vp.Height = (float)p->height;
    } else {
        vp.TopLeftX = viewport[0];
        vp.TopLeftY = viewport[1];
        vp.Width = viewport[2];
        vp.Height = viewport[3];
    }
    vp.MaxDepth = 1.0f;
    p->list->RSSetViewports(1, &vp);
    p->list->RSSetScissorRects(1, &sc);
    p->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    p->list->DrawInstanced(3, 1, 0, 0);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    p->list->ResourceBarrier(1, &b);
    if (FAILED(p->list->Close())) return FALSE;

    p->queue->Wait(p->fence11in12, copied);           /* copies done on the D3D11 side */
    ID3D12CommandList *lists[] = { p->list };
    p->queue->ExecuteCommandLists(1, lists);
    HRESULT hr = p->swap->Present(0, 0);
    wait_idle(p);                                      /* one frame in flight; allocator reuse */
    if (FAILED(hr)) {
        plog(p, L"D3D12: Present failed (0x%08X)", hr);
        return FALSE;
    }
    return TRUE;
}

extern "C" const wchar_t *dx12_adapter_name(Dx12Presenter *p)
{
    return p ? p->adapter : L"";
}

extern "C" void dx12_destroy(Dx12Presenter *p)
{
    if (!p) return;
    wait_idle(p);
    release_shared(&p->pic);
    release_shared(&p->bg);
    SAFE_RELEASE(p->pso_blend);
    SAFE_RELEASE(p->pso_plain);
    SAFE_RELEASE(p->root);
    release_targets(p);
    SAFE_RELEASE(p->srv_heap);
    SAFE_RELEASE(p->rtv_heap);
    if (p->latency_wait) CloseHandle(p->latency_wait);
    SAFE_RELEASE(p->swap);
    SAFE_RELEASE(p->list);
    SAFE_RELEASE(p->alloc);
    SAFE_RELEASE(p->fence);
    SAFE_RELEASE(p->queue);
    SAFE_RELEASE(p->fence11in12);
    SAFE_RELEASE(p->fence11);
    SAFE_RELEASE(p->ctx11_4);
    SAFE_RELEASE(p->dev);
    if (p->fence_event) CloseHandle(p->fence_event);
    SAFE_RELEASE(p->ctx11);
    SAFE_RELEASE(p->dev11);
    free(p);
}
