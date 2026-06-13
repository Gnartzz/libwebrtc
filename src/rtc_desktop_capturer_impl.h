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
  bool g_have_frame_ = false;
  uint32_t g_target_w_ = 0, g_target_h_ = 0, g_desk_w_ = 0, g_desk_h_ = 0;
  // GPU-Vorschau: eine KEYED_MUTEX-Shared-Textur, in die pro Frame das fertige
  // BGRA-Bild kopiert wird; ihr Legacy-Shared-Handle geht an den Renderer
  // (Flutter GpuSurfaceTexture). Optional — wenn die Erzeugung scheitert, laeuft
  // der Sende-/Encode-Pfad unveraendert weiter (nur die Vorschau bleibt CPU).
  Microsoft::WRL::ComPtr<ID3D11Texture2D> g_shared_tex_;
  HANDLE g_shared_handle_ = nullptr;
#endif
};

}  // namespace libwebrtc

#endif  // LIBWEBRTC_RTC_DESKTOP_CAPTURER_IMPL_HXX
