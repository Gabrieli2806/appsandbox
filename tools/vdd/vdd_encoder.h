/*
 * vdd_encoder.h -- GPU video encoder for the performance (gaming) display mode.
 *
 * The raw display path copies every frame from the GPU to the CPU and sends
 * it uncompressed (about 33 MB per frame at 3840x2160). In performance mode
 * the frame stays on the GPU instead: a D3D11 video processor converts the
 * desktop surface to NV12 and the GPU's hardware H.264 encoder (NVENC, AMF,
 * Quick Sync, exposed through Media Foundation) compresses it, so only a few
 * hundred kilobytes per frame cross the VM boundary.
 *
 * Media Foundation is loaded at run time, so a guest without it (or without
 * a hardware encoder) simply keeps the raw path.
 */
#pragma once

#include <windows.h>
#include <d3d11.h>

struct VddEncoder;

/* One encoded access unit (Annex-B H.264). key = IDR picture. Called from the
   encoder's event thread. */
typedef void (*VddEncOutputFn)(void* user, const BYTE* data, UINT len, BOOL key,
                               UINT width, UINT height);
typedef void (*VddEncLogFn)(const char* fmt, ...);

/* Creates an encoder for width x height frames (BGRA surfaces of the given
   device). fps is the display refresh, kbps the target bit rate (0 = auto).
   The device must have been created with D3D11_CREATE_DEVICE_VIDEO_SUPPORT.
   Call from a thread that has COM initialised. */
/* codec: 1 H.264, 2 HEVC (HEVC needs NVENC; other encoders fall back to
   H.264, see VddEncoderCodec). */
HRESULT VddEncoderCreate(ID3D11Device* device, ID3D11DeviceContext* ctx,
                         UINT width, UINT height, UINT fps, UINT kbps, UINT codec,
                         VddEncOutputFn out, void* user, VddEncLogFn log,
                         VddEncoder** result);

/* Encodes one desktop surface (B8G8R8A8, encoder size). The surface is copied
   on the GPU before this returns, so the caller may release it right away.
   Returns S_FALSE when the frame was skipped because the encoder is busy. */
HRESULT VddEncoderEncode(VddEncoder* enc, ID3D11Texture2D* surface, BOOL forceKey);

/* Encodes the last frame again (keeps the stream alive while the desktop is
   idle; an unchanged picture costs only a few bytes). */
HRESULT VddEncoderRepeat(VddEncoder* enc, BOOL forceKey);

/* Frames the encoder failed on (the caller falls back to raw frames). */
BOOL VddEncoderFailed(VddEncoder* enc);

UINT VddEncoderWidth(VddEncoder* enc);
UINT VddEncoderHeight(VddEncoder* enc);
const char* VddEncoderName(VddEncoder* enc);
UINT VddEncoderCodec(VddEncoder* enc);   /* codec actually produced */

void VddEncoderDestroy(VddEncoder* enc);
