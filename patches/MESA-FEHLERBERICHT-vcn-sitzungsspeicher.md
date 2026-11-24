# ENTWURF (nicht abgeschickt) — Mesa-Fehlerbericht

Ziel: https://gitlab.freedesktop.org/mesa/mesa/-/issues (Komponente radeonsi / VA-API / VCN)

---

## Title

radeonsi/vcn: encoder session info buffer is not cleared — stale pool memory makes the VCN firmware fault on the first OP_ENCODE (ring vcn_unified_0 timeout, kills other VCN users)

## Summary

`radeon_enc_begin_frame()` allocates the 128 KiB VCN encoder session info buffer
(`enc->si`) with `si_vid_create_buffer(..., PIPE_USAGE_DEFAULT, PIPE_RESOURCE_FLAG_UNMAPPABLE, 128 * 1024)`
and never clears it. The buffer comes from the winsys reuse pool / slabs, so it can
carry stale data: the session info of a previously closed encoder, or anything else
that was freed in the same process (for example GL texture data when an OpenGL
context lives in the same process — both share the `amdgpu_winsys`).

With stale content, the VCN firmware faults on the **first** `OP_ENCODE` of the new
session: `[mmhub] page fault ... Faulty UTCL2 client ID: VCN`, write access at
`0x0 + offset` (0x0, 0xfff000, 0xf54000, 0xfbd000) or at "addresses" that look like
pixel data (0x59593f3f3000, 0xbfbf9ca5a000, 0x73738d8d8000), then
`ring vcn_unified_0 timeout`, ring reset, the context is marked guilty and Mesa
aborts the process ("The CS has cancelled because the context is lost"). A second
timeout usually hits an unrelated process that was encoding at the same time
(Sunshine, ffmpeg), which aborts as well.

Clearing only this one buffer fixes it. Clearing only the DPB buffer does not.

## System

- AMD Ryzen 9 8945HS, Radeon 780M (Phoenix, PCI 1002:1900), VCN 4.0.2, VCN firmware 0x09118037
- Kernel 7.3.5 (CachyOS), linux-firmware 20261019
- Mesa 26.2.7 (Flatpak org.freedesktop.Platform.GL.default 25.08-extra and own build) and 26.3.1 (Arch);
  the code is unchanged on main (`src/gallium/drivers/radeonsi/mm/radeon_vcn_enc.c`, `radeon_enc_begin_frame`)
- libva 2.24, FFmpeg 7.1.5 `h264_vaapi` (CBR, low delay, no B-frames, 1280x720 and 1920x1080)

## How to reproduce

Two ways, both without any other GPU load:

1. Two `h264_vaapi` encoders on **one** VADisplay in one process, repeatedly closed and
   reopened (close encoder, open encoder, encode a few frames; both encoders interleaved).
   Hangs after 10–26 session opens. (With one VADisplay per encoder, or with a single
   encoder, 180–360 opens pass.)
2. An application that renders with OpenGL (Qt Quick, new texture per video frame) and
   opens an `h264_vaapi` encoder in the same process: hangs on the first encoded frame
   in 3 of 4 runs. With Qt on the software or Vulkan backend: no hang.

## Evidence

All 9 `devcoredump`s show the same hung job: `OP_SET_SPEED_MODE` (or `…_BALANCE_MODE`)
+ `OP_ENCODE`, `TASK_INFO` task id 2 — the first IDR picture of a freshly initialised
session. All addresses in the IB are valid GPU addresses.

Mesa 26.2.7 built from source with switchable test patches, reproduction 1
(sessions opened until the hang, 360 = run completed):

| Variant | Result |
| --- | --- |
| unmodified | hang after 26 / 18 opens |
| video surfaces `PIPE_BIND_SHARED` (as in 26.3) | hang after 10 opens |
| `si_vid_create_buffer`: `PIPE_BIND_SHARED` (no reuse, no suballocation) | 360, 360 |
| `si_vid_create_buffer`: `SI_RESOURCE_FLAG_CLEAR` for all buffers | 360 |
| `SI_RESOURCE_FLAG_CLEAR` only for buffers <= 128 KiB (session info) | 360 |
| `SI_RESOURCE_FLAG_CLEAR` only for buffers > 128 KiB (DPB) | hang after 10 opens |

Reproduction 2 with the VA driver loaded as a separate build (own `amdgpu_winsys`
instance, so no buffers shared with GL): 12 of 12 runs without a hang, patched or not.

## Proposed fix

```diff
--- a/src/gallium/drivers/radeonsi/mm/radeon_vcn_enc.c
+++ b/src/gallium/drivers/radeonsi/mm/radeon_vcn_enc.c
@@ static void radeon_enc_begin_frame(struct pipe_video_codec *encoder,
    if (!enc->si) {
       enc->si = si_vid_create_buffer(enc->screen, PIPE_USAGE_DEFAULT,
-                                     PIPE_RESOURCE_FLAG_UNMAPPABLE, 128 * 1024);
+                                     PIPE_RESOURCE_FLAG_UNMAPPABLE | SI_RESOURCE_FLAG_CLEAR,
+                                     128 * 1024);
```

(The decoder already clears its context buffers with `si_vid_clear_buffer()`.)

## Possibly related reports

- drm/amd #3201 — "ERROR: ring vcn_unified_0 timeout - when using VAAPI encoder"
- mesa #10541 — "amdgpu page fault when running wivrn to vaapi encode on 6800xt"
- drm/amd #2563 — "Navi 22 - Occasional vcn timeouts when VAAPI encoding and decoding simultaneously"

## Attachments to add when filing

- devcoredumps (devcd5 … devcd13), kernel log excerpts
- the test program (`test/neuaufsetzen_test.cc` of the libwebrtc fork) or a reduced FFmpeg-only reproducer
