/*
 * vm_display_d3d12.h -- optional Direct3D 12 presenter for the VM window.
 *
 * Settings > VM windows > Renderer = D3D12. Decoding (Media Foundation) and
 * raw-frame upload stay on the D3D11 device; each frame is copied into
 * textures shared with D3D12 (synchronised with a shared fence), and D3D12
 * draws the window: flip-model swap chain, the plain picture or the
 * adaptive composite (blurred backdrop, rounded picture with a shadow),
 * Present. If anything here fails the viewer falls back to its D3D11 swap
 * chain.
 */
#ifndef VM_DISPLAY_D3D12_H
#define VM_DISPLAY_D3D12_H

#include <windows.h>
#include <d3d11.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Dx12Presenter Dx12Presenter;
typedef void (*Dx12LogFn)(void *user, const wchar_t *msg);

/* vs/ps sources are the viewer's HLSL (compiled here for shader model 5).
   NULL on failure (logged). */
Dx12Presenter *dx12_create(HWND hwnd, ID3D11Device *dev11, ID3D11DeviceContext *ctx11,
                           const char *vs_hlsl, const char *ps_hlsl, const char *ps_blend_hlsl,
                           Dx12LogFn log, void *log_user);

/* Window client size changed. */
void dx12_resize(Dx12Presenter *p, UINT width, UINT height);

/* Draw one frame and present.
   pic: the picture (BGRA, frame size) as a D3D11 SRV.
   viewport: x, y, w, h of the picture in client pixels.
   blend: draw the adaptive composite over the whole window instead (bg =
   mip-mapped copy of the picture, consts = the 16 floats of the blend
   shader's constant buffer, passed as root constants b0; same layout as
   the D3D11 path, see g_ps_blur_hlsl in vm_display_idd.c).
   Returns FALSE if the frame could not be drawn (a failing Present is
   logged once). */
BOOL dx12_present(Dx12Presenter *p, ID3D11ShaderResourceView *pic,
                  const float viewport[4], BOOL blend,
                  ID3D11ShaderResourceView *bg, const float consts[16]);

/* S_OK while the D3D12 device works; otherwise why it was removed
   (driver update or reset, TDR). The viewer then rebuilds its renderer. */
HRESULT dx12_device_removed_reason(Dx12Presenter *p);

/* Adapter name, for the log/title. */
const wchar_t *dx12_adapter_name(Dx12Presenter *p);

void dx12_destroy(Dx12Presenter *p);

#ifdef __cplusplus
}
#endif

#endif
