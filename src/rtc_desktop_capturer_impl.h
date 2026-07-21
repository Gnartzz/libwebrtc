/*
 * Copyright 2022 LiveKit
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef LIBWEBRTC_RTC_DESKTOP_CAPTURER_IMPL_HXX
#define LIBWEBRTC_RTC_DESKTOP_CAPTURER_IMPL_HXX

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "include/rtc_desktop_capturer.h"
#include "include/rtc_types.h"
#include "modules/desktop_capture/desktop_and_cursor_composer.h"
#include "modules/desktop_capture/desktop_capture_options.h"
#include "modules/desktop_capture/desktop_capturer.h"
#include "modules/desktop_capture/desktop_frame.h"
#include "rtc_base/thread.h"
#include "src/internal/vcm_capturer.h"
#include "src/internal/video_capturer.h"

#ifdef _WIN32
#include <d3d11.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <memory>
#endif

namespace libwebrtc {

class RTCDesktopCapturerImpl : public RTCDesktopCapturer,
                               public webrtc::DesktopCapturer::Callback,
                               public webrtc::internal::VideoCapturer {
 public:
  RTCDesktopCapturerImpl(DesktopType type,
                         webrtc::DesktopCapturer::SourceId source_id,
                         webrtc::Thread* signaling_thread,
                         scoped_refptr<MediaSource> source, bool showCursor = true);
  ~RTCDesktopCapturerImpl();

  void RegisterDesktopCapturerObserver(
      DesktopCapturerObserver* observer) override {
    observer_ = observer;
  }

  void DeRegisterDesktopCapturerObserver() override { observer_ = nullptr; }
  CaptureState Start(uint32_t fps) override;

  CaptureState Start(uint32_t fps, uint32_t x, uint32_t y, uint32_t w,
                     uint32_t h) override;

  void Stop() override;

  bool IsRunning() override;

  scoped_refptr<MediaSource> source() override { return source_; }

  void SetMaxResolution(uint32_t max_width, uint32_t max_height) override {
    max_width_ = max_width;
    max_height_ = max_height;
  }

 protected:
  virtual void OnCaptureResult(
      webrtc::DesktopCapturer::Result result,
      std::unique_ptr<webrtc::DesktopFrame> frame) override;

 private:
  void CaptureFrame();
  webrtc::DesktopCaptureOptions options_;
  std::unique_ptr<webrtc::DesktopCapturer> capturer_;
  std::unique_ptr<webrtc::Thread> thread_;
  webrtc::scoped_refptr<webrtc::I420Buffer> i420_buffer_;
  CaptureState capture_state_ = CS_STOPPED;
  DesktopType type_;
  webrtc::DesktopCapturer::SourceId source_id_;
  DesktopCapturerObserver* observer_ = nullptr;
  uint32_t capture_delay_ = 1000;  // 1s
  webrtc::DesktopCapturer::Result result_ =
      webrtc::DesktopCapturer::Result::SUCCESS;
  webrtc::Thread* signaling_thread_ = nullptr;
  scoped_refptr<MediaSource> source_;
  uint32_t x_ = 0;
  uint32_t y_ = 0;
  uint32_t w_ = 0;
  uint32_t h_ = 0;
  uint32_t max_width_ = 0;
  uint32_t max_height_ = 0;
  webrtc::scoped_refptr<webrtc::I420Buffer> scaled_buffer_;
  bool gpu_mode_ = false;  // Zero-Copy-GPU-Pfad aktiv (Screen+NVIDIA); sonst CPU
  bool show_cursor_ = true;  // fuer die LAZY-Erzeugung des Screen-Capturers in
                             // Start() (Cursor-Variante), siehe .cc-Konstruktor

#ifdef _WIN32
  // Zero-Copy-GPU-Pfad: DXGI-dup + Shader-Downscale -> kNative D3D11-Textur,
  // alles auf einem NVIDIA-Device. Aktiv, wenn InitGpu() erfolgreich (Screen +
  // Monitor an NVIDIA); sonst Fallback auf den webrtc-CPU-Capturer oben.
  bool InitGpu();
  // WGC (Windows.Graphics.Capture): GPU-Zero-Copy-Capture eines FENSTERS auf
  // g_dev_ (statt CPU/GDI). Liefert wie InitGpu einen nativen Frame -> Vorschau
  // ueber den GPU-Ring (keine gelbe Kachel) + zero-copy Encode.
  bool InitGpuWindow(intptr_t hwnd);
  // Geteilte Pipeline-Einrichtung (Shader/Sampler/g_cached_/Pool/Ring) fuer beide
  // Quellen (DXGI-Bildschirm + WGC-Fenster), parametrisiert ueber die Quellgroesse.
  bool InitGpuPipeline(uint32_t src_w, uint32_t src_h);
  // Naechsten WGC-Frame holen -> g_cached_. *changed=false bei keinem neuen Frame
  // (Stillstand -> Steady-Cadence-Wiederholung). false = fataler Fehler (Fallback).
  bool WgcAcquire(bool* changed);
  void ReleaseGpu();
  void GpuCaptureFrame();
  static constexpr int kGpuPool = 4;
  Microsoft::WRL::ComPtr<ID3D11Device> g_dev_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> g_ctx_;
  Microsoft::WRL::ComPtr<IDXGIOutputDuplication> g_dup_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> g_vs_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> g_ps_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> g_smp_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> g_cached_;       // voller Desktop (SRV)
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> g_srv_;
  std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, kGpuPool> g_out_;   // Pool
  std::array<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>, kGpuPool> g_rtv_;
  int g_pool_idx_ = 0;
  int g_last_out_idx_ = -1;  // letzter fertiger Downscale-Frame; bei Stillstand
                             // (DXGI-Timeout) im festen Takt wiederholt

  bool g_have_frame_ = false;
  uint32_t g_target_w_ = 0, g_target_h_ = 0, g_desk_w_ = 0, g_desk_h_ = 0;
  // GPU-Vorschau: RING aus kShareRing plain-SHARED-Texturen, in die MIT
  // KADENZ (~30 fps, s. kShareCadenceMs) reihum das fertige BGRA-Bild kopiert
  // wird; ihr Legacy-Shared-Handle geht an den Renderer (Flutter
  // GpuSurfaceTexture). Bei jedem Ring-Update rotiert das durchgereichte
  // Handle, damit Flutters ExternalTextureD3d ein NEUES Handle sieht und
  // eglBindTexImage erneut aufruft — sonst bindet es nur 1x pro Handle und
  // friert bei In-place-Updates ein (beige Vorschau). ZWISCHEN Updates traegt
  // der Frame dasselbe Handle -> die Engine bindet nicht neu (billig).
  // KADENZ + RING=6 (2026-07-21, AIX1-Messreihe): das Zeichnen eines Ring-
  // Slots synchronisiert cross-device gegen die Producer-Queue; wurde der Ring
  // mit 60 fps beschrieben, blieb Flutters Fenster-Compositor bei ~18,5 fps —
  // unabhaengig von der Re-Bind-Rate (Drossel-Probe: Re-Binds halbiert, 0 fps
  // Gewinn; statische Texturen im selben Szenario: 31-85 fps). Mit ~30-fps-
  // Kadenz + 6er-Ring ist der angezeigte Slot laengst fertig geschrieben und
  // wird erst ~200 ms spaeter wiederverwendet -> Draws warten nicht mehr.
  // Sende-/Encode-Pfad (g_out_) voellig unberuehrt. Optional — scheitert die
  // Erzeugung, laeuft der Sende-Pfad unveraendert weiter (Vorschau bleibt CPU).
  static constexpr int kShareRing = 6;
  static constexpr int64_t kShareCadenceMs = 33;  // ~30 fps Vorschau-Kadenz
  std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, kShareRing> g_shared_tex_;
  std::array<HANDLE, kShareRing> g_shared_handle_ = {};
  int g_share_idx_ = 0;
  HANDLE g_share_cur_handle_ = nullptr;  // zuletzt beschriebener Slot
  int64_t g_share_last_ms_ = 0;          // letztes Ring-Update (Kadenz-Gate)

  // WGC-Fenster-Capture-State (PIMPL: WinRT-Typen bleiben in der .cc, da dieser
  // Header von mehreren TUs inkludiert wird). wgc_mode_ aktiv => GpuCaptureFrame
  // nimmt den WGC-Pfad; wgc_w_/h_ = aktuelle Frame-Pool-Groesse (Resize-Erkennung).
  struct WgcState;
  std::unique_ptr<WgcState> wgc_;
  bool wgc_mode_ = false;
  uint32_t wgc_w_ = 0, wgc_h_ = 0;
#endif
};

}  // namespace libwebrtc

#endif  // LIBWEBRTC_RTC_DESKTOP_CAPTURER_IMPL_HXX
