/*
 * vm_video_decode.h -- H.264 decoding for the viewer's performance mode.
 *
 * Decodes the guest's GPU-encoded access units with the Media Foundation
 * H.264 decoder (DXVA on the host GPU) and converts each picture to a BGRA
 * texture the viewer draws like a raw frame. Everything runs on the thread
 * that owns the D3D11 immediate context (the viewer's window thread).
 */
#ifndef VM_VIDEO_DECODE_H
#define VM_VIDEO_DECODE_H

#include <windows.h>
#include <d3d11.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VmVideoDecoder VmVideoDecoder;

typedef void (*VmVideoLogFn)(void *user, const wchar_t *msg);

#define VM_VIDEO_CODEC_H264 1
#define VM_VIDEO_CODEC_HEVC 2

/* TRUE when this PC has an HEVC decoder (Windows needs the "HEVC Video
   Extensions" from the Microsoft Store for it). Checked once. */
BOOL vm_video_hevc_supported(void);

/* NULL when Media Foundation or the device cannot decode video. */
VmVideoDecoder *vm_video_decoder_create(ID3D11Device *device, ID3D11DeviceContext *ctx,
                                        UINT codec, UINT width, UINT height,
                                        VmVideoLogFn log, void *log_user);

/* Decodes one access unit. Returns 1 when a new picture is in the output
   texture, 0 when the decoder needs more data, -1 on a decoding error (the
   caller asks the guest for a key frame and recreates the decoder). */
int vm_video_decoder_decode(VmVideoDecoder *dec, const BYTE *data, UINT len);

/* BGRA picture (width x height). Valid until the next decode. */
ID3D11ShaderResourceView *vm_video_decoder_srv(VmVideoDecoder *dec);

UINT vm_video_decoder_width(VmVideoDecoder *dec);
UINT vm_video_decoder_codec(VmVideoDecoder *dec);
UINT vm_video_decoder_height(VmVideoDecoder *dec);
BOOL vm_video_decoder_hardware(VmVideoDecoder *dec);

void vm_video_decoder_destroy(VmVideoDecoder *dec);

#ifdef __cplusplus
}
#endif

#endif
