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

#include "rtc_desktop_capturer_impl.h"

#include <cstdarg>
#include <cstdio>
#ifdef WEBRTC_WIN
#include <windows.h>
#endif

#include "api/sequence_checker.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv.h"
#ifdef WEBRTC_WIN
#include "modules/desktop_capture/win/window_capture_utils.h"
#include "modules/desktop_capture/win/screen_capturer_win_directx.h"
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include "win/honeycord_d3d11_frame.h"
// WGC (Windows.Graphics.Capture) fuer GPU-Zero-Copy-Fenster-Capture. Die WinRT-
// Header + webrtc-Helfer (gleiche, die webrtcs eigener WGC-Capturer nutzt -> im
// Build bewiesen funktionsfaehig).
#include <windows.graphics.h>
#include <windows.graphics.directx.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include "rtc_base/win/create_direct3d_device.h"
#include "rtc_base/win/get_activation_factory.h"
#endif

namespace libwebrtc {

enum { kCaptureDelay = 33, kCaptureMessageId = 1000 };

// honeycord diagnostics: append to the same file NvProbeLog uses
// (%LOCALAPPDATA%\HoneyCord\nvenc-probe.log) — RTC_LOG does not reach that
// file in release builds, and that's the file the user already pulls.
static int g_hc_src_w = 0, g_hc_src_h = 0, g_hc_dst_w = 0, g_hc_dst_h = 0;
static int g_hc_conv_ms = 0;
#ifdef WEBRTC_WIN
static void HcCapLog(const char* fmt, ...) {
  wchar_t dir[MAX_PATH];
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return;
  wchar_t path[MAX_PATH];
  swprintf_s(path, MAX_PATH, L"%s\\HoneyCord\\nvenc-probe.log", dir);
  FILE* f = nullptr;
  if (_wfopen_s(&f, path, L"a") != 0 || !f) return;
  SYSTEMTIME st;
  GetLocalTime(&st);
  fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond,
          st.wMilliseconds);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fprintf(f, "\n");
  fclose(f);
}
#else
static void HcCapLog(const char*, ...) {}
#endif

#ifdef _WIN32
namespace WGC = ABI::Windows::Graphics::Capture;
namespace WGDX = ABI::Windows::Graphics::DirectX;

// PIMPL-Definition: haelt die WinRT-Objekte der WGC-Fenster-Capture (die Typen
// bleiben so aus dem von mehreren TUs inkludierten Header heraus).
struct RTCDesktopCapturerImpl::WgcState {
  Microsoft::WRL::ComPtr<WGDX::Direct3D11::IDirect3DDevice> device;
  Microsoft::WRL::ComPtr<WGC::IDirect3D11CaptureFramePool> pool;
  Microsoft::WRL::ComPtr<WGC::IGraphicsCaptureSession> session;
  Microsoft::WRL::ComPtr<WGC::IGraphicsCaptureItem> item;
};
#endif

RTCDesktopCapturerImpl::RTCDesktopCapturerImpl(
    DesktopType type, webrtc::DesktopCapturer::SourceId source_id,
    webrtc::Thread* signaling_thread, scoped_refptr<MediaSource> source,
    bool showCursor)
    : thread_(webrtc::Thread::Create()),
      source_id_(source_id),
      signaling_thread_(signaling_thread),
      source_(source) {
  RTC_DCHECK(thread_);
  type_ = type;
  thread_->Start();
  options_ = webrtc::DesktopCaptureOptions::CreateDefault();
  options_.set_detect_updated_region(true);
#ifdef WEBRTC_WIN
  options_.set_allow_directx_capturer(true);
#endif
#ifdef WEBRTC_LINUX
  if (type == kScreen) {
    options_.set_allow_pipewire(true);
  }
#endif
  show_cursor_ = showCursor;
  thread_->BlockingCall([this, type, showCursor] {
    if (type != kScreen) {
      capturer_ = std::make_unique<webrtc::DesktopAndCursorComposer>(
          webrtc::DesktopCapturer::CreateWindowCapturer(options_), options_);
      return;
    }
#ifdef _WIN32
    // honeycord: Bildschirm-Capturer auf Windows LAZY erzeugen (erst in Start()
    // nach InitGpu(), nur bei CPU-Fallback). CreateScreenCapturer(allow_directx)
    // ruft intern ScreenCapturerWinDirectx::IsSupported() -> initialisiert
    // webrtcs DxgiDuplicatorController, der ALLE Monitor-Outputs dupliziert und
    // haelt. webrtc erlaubt nur EINE Duplication pro Monitor/Prozess, also
    // wuerde unser Zero-Copy-InitGpu::DuplicateOutput sonst mit E_INVALIDARG
    // (0x80070057) scheitern. Fenster sind davon nicht betroffen.
    (void)showCursor;
#else
    if (showCursor) {
      capturer_ = std::make_unique<webrtc::DesktopAndCursorComposer>(
          webrtc::DesktopCapturer::CreateScreenCapturer(options_), options_);
    } else {
      capturer_ = webrtc::DesktopAndCursorComposer::CreateWithoutMouseCursorMonitor(
              webrtc::DesktopCapturer::CreateScreenCapturer(options_));
    }
#endif
  });
#ifdef WEBRTC_WIN
  // KEIN ScreenCapturerWinDirectx::IsSupported() hier aufrufen -> initialisiert
  // sonst den DxgiDuplicatorController und greift den Monitor-Output (s.o.).
  HcCapLog("capturer init: type=%d showCursor=%d", (int)type, (int)showCursor);
#endif
}

RTCDesktopCapturerImpl::~RTCDesktopCapturerImpl() {
  thread_->Stop();
#ifdef _WIN32
  ReleaseGpu();
#endif
  capturer_.reset();
}

RTCDesktopCapturerImpl::CaptureState RTCDesktopCapturerImpl::Start(
    uint32_t fps, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  x_ = x;
  y_ = y;
  w_ = w;
  h_ = h;
  if (!w_ || !h) {
    x_ = 0;
    y_ = 0;
  }
  return Start(fps);
}

RTCDesktopCapturerImpl::CaptureState RTCDesktopCapturerImpl::Start(
    uint32_t fps) {
  if (capture_state_ == CS_RUNNING) {
    return capture_state_;
  }

  if (fps == 0) {
    capture_state_ = CS_FAILED;
    return capture_state_;
  }

  if (fps >= 60) {
    capture_delay_ = uint32_t(1000.0 / 60.0);
  } else {
    capture_delay_ = uint32_t(1000.0 / fps);
  }

#ifdef _WIN32
  // Zero-Copy-GPU-Pfad nur fuer Bildschirm (kScreen). InitGpu() faellt bei
  // Monitor-an-iGPU / kein-NVIDIA sauber durch -> CPU-Capturer uebernimmt.
  if (type_ == kScreen) {
    thread_->BlockingCall([this] { gpu_mode_ = InitGpu(); });
    if (!gpu_mode_ && !capturer_) {
      // GPU-Pfad inaktiv -> jetzt erst den webrtc-Bildschirm-Capturer bauen
      // (lazy, siehe Konstruktor). Ab hier darf DirectX/DxgiDuplicator
      // initialisieren, da kein GPU-Pfad mehr um die eine erlaubte Monitor-
      // Duplication konkurriert.
      thread_->BlockingCall([this] {
        if (show_cursor_) {
          capturer_ = std::make_unique<webrtc::DesktopAndCursorComposer>(
              webrtc::DesktopCapturer::CreateScreenCapturer(options_), options_);
        } else {
          capturer_ =
              webrtc::DesktopAndCursorComposer::CreateWithoutMouseCursorMonitor(
                  webrtc::DesktopCapturer::CreateScreenCapturer(options_));
        }
      });
    }
  } else if (type_ == kWindow && source_id_ != -1) {
    // WGC-Zero-Copy fuer Fenster auf der NVIDIA. Scheitert es (kein NVIDIA / WGC
    // nicht verfuegbar / geschuetztes Fenster), uebernimmt der im Konstruktor
    // erzeugte CPU-Window-Capturer ueber den !gpu_mode_-Pfad unten.
    thread_->BlockingCall([this] { gpu_mode_ = InitGpuWindow(source_id_); });
    HcCapLog(gpu_mode_ ? "Start: WGC-Fenster-Zero-Copy aktiv"
                       : "Start: WGC fehlgeschlagen -> CPU-Window-Capturer");
  }
#endif

  if (!gpu_mode_ && source_id_ != -1) {
    if (!capturer_->SelectSource(source_id_)) {
      capture_state_ = CS_FAILED;
      return capture_state_;
    }
    if (type_ == kWindow) {
      if (!capturer_->FocusOnSelectedSource()) {
        capture_state_ = CS_FAILED;
        return capture_state_;
      }
    }
  }

  if (!gpu_mode_) {
    thread_->BlockingCall([this] { capturer_->Start(this); });
  }
  capture_state_ = CS_RUNNING;
  thread_->PostTask([this] { CaptureFrame(); });
  if (observer_) {
    signaling_thread_->BlockingCall([&, this]() { observer_->OnStart(this); });
  }
  return capture_state_;
}

void RTCDesktopCapturerImpl::Stop() {
  if (observer_) {
    if (!signaling_thread_->IsCurrent()) {
      signaling_thread_->BlockingCall([&, this]() { observer_->OnStop(this); });
    } else {
      observer_->OnStop(this);
    }
  }
  capture_state_ = CS_STOPPED;
}

bool RTCDesktopCapturerImpl::IsRunning() {
  return capture_state_ == CS_RUNNING;
}

#ifdef WEBRTC_WIN
int filterException(int code, PEXCEPTION_POINTERS ex) {
  return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void RTCDesktopCapturerImpl::OnCaptureResult(
    webrtc::DesktopCapturer::Result result,
    std::unique_ptr<webrtc::DesktopFrame> frame) {
  if (result != result_) {
    if (result == webrtc::DesktopCapturer::Result::ERROR_PERMANENT) {
      if (observer_) {
        signaling_thread_->BlockingCall(
            [&, this]() { observer_->OnError(this); });
      }
      capture_state_ = CS_FAILED;
      return;
    }

    if (result == webrtc::DesktopCapturer::Result::ERROR_TEMPORARY) {
      result_ = result;
      if (observer_) {
        signaling_thread_->BlockingCall(
            [&, this]() { observer_->OnPaused(this); });
      }
      return;
    }

    if (result == webrtc::DesktopCapturer::Result::SUCCESS) {
      result_ = result;
      if (observer_) {
        signaling_thread_->BlockingCall(
            [&, this]() { observer_->OnStart(this); });
      }
    }
  }

  if (result == webrtc::DesktopCapturer::Result::ERROR_TEMPORARY) {
    return;
  }

  int width = frame->size().width();
  int height = frame->size().height();
#ifdef WEBRTC_WIN
  webrtc::DesktopRect rect_ = webrtc::DesktopRect::MakeWH(width, height);

  if (type_ != kScreen) {
    webrtc::GetWindowRect(reinterpret_cast<HWND>(source_id_), &rect_);
  }

  __try
#endif
  {
    width = w_ > 0 ? w_ : width;
    height = h_ > 0 ? h_ : height;
    if (!i420_buffer_ || !i420_buffer_.get() ||
        i420_buffer_->width() * i420_buffer_->height() != width * height) {
      i420_buffer_ = webrtc::I420Buffer::Create(width, height);
    }

    int64_t hc_tc = webrtc::TimeMillis();
    libyuv::ConvertToI420(frame->data(), 0, i420_buffer_->MutableDataY(),
                          i420_buffer_->StrideY(), i420_buffer_->MutableDataU(),
                          i420_buffer_->StrideU(), i420_buffer_->MutableDataV(),
                          i420_buffer_->StrideV(), x_, y_,
#ifdef WEBRTC_WIN
                          rect_.width(), rect_.height(),
#else
                          width, height,
#endif
                          width, height, libyuv::kRotate0, libyuv::FOURCC_ARGB);

    // Optional max-resolution clamp (preserves aspect ratio). Required on
    // Windows where the native DesktopCapturer always emits at monitor
    // resolution and runtime scaleResolutionDownBy on RTPSender is not
    // honored for desktop sources. Downscale here via libyuv before the
    // frame reaches the encoder.
    webrtc::scoped_refptr<webrtc::I420BufferInterface> out_buffer = i420_buffer_;
    if (max_width_ > 0 && max_height_ > 0 &&
        (i420_buffer_->width() > static_cast<int>(max_width_) ||
         i420_buffer_->height() > static_cast<int>(max_height_))) {
      double sx = static_cast<double>(max_width_) / i420_buffer_->width();
      double sy = static_cast<double>(max_height_) / i420_buffer_->height();
      double scale = sx < sy ? sx : sy;
      int dst_w = static_cast<int>(i420_buffer_->width() * scale) & ~1;
      int dst_h = static_cast<int>(i420_buffer_->height() * scale) & ~1;
      if (!scaled_buffer_ || scaled_buffer_->width() != dst_w ||
          scaled_buffer_->height() != dst_h) {
        scaled_buffer_ = webrtc::I420Buffer::Create(dst_w, dst_h);
      }
      libyuv::I420Scale(
          i420_buffer_->DataY(), i420_buffer_->StrideY(),
          i420_buffer_->DataU(), i420_buffer_->StrideU(),
          i420_buffer_->DataV(), i420_buffer_->StrideV(),
          i420_buffer_->width(), i420_buffer_->height(),
          scaled_buffer_->MutableDataY(), scaled_buffer_->StrideY(),
          scaled_buffer_->MutableDataU(), scaled_buffer_->StrideU(),
          scaled_buffer_->MutableDataV(), scaled_buffer_->StrideV(),
          dst_w, dst_h, libyuv::kFilterBilinear);
      out_buffer = scaled_buffer_;
    }

    g_hc_conv_ms = static_cast<int>(webrtc::TimeMillis() - hc_tc);
    g_hc_src_w = i420_buffer_->width();
    g_hc_src_h = i420_buffer_->height();
    g_hc_dst_w = out_buffer->width();
    g_hc_dst_h = out_buffer->height();

    OnFrame(webrtc::VideoFrame(out_buffer, 0, webrtc::TimeMillis(),
                               webrtc::kVideoRotation_0));
  }
#ifdef WEBRTC_WIN
  __except (filterException(GetExceptionCode(), GetExceptionInformation())) {
  }
#endif
}

#ifdef _WIN32
// Fullscreen-Triangle-Shader: Bilinear-Downscale Desktop-SRV -> Pool-RTV.
static const char* kHcShaderHLSL =
    "Texture2D tex:register(t0); SamplerState smp:register(s0);"
    "struct VO{float4 p:SV_POSITION;float2 uv:TEXCOORD0;};"
    "VO VSMain(uint id:SV_VertexID){VO o;o.uv=float2((id<<1)&2,id&2);"
    "o.p=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;}"
    "float4 PSMain(VO i):SV_TARGET{return tex.Sample(smp,i.uv);}";

// id12: die im Picker gewaehlte Screen-ID (GDI-Index der MediaList, d. h.
// EnumDisplayDevices-Reihenfolge) in den Windows-Geraetenamen (\\.\DISPLAYn)
// aufloesen. NUR ueber den Namen darf der DXGI-Output gesucht werden — die
// DXGI-Output-Reihenfolge (Port-Reihenfolge am Adapter) ist NICHT die
// GDI-Reihenfolge; Index-Gleichsetzung streamt den falschen Monitor.
// id < 0 (kFullDesktop) -> primaerer Monitor.
static bool ScreenIdToDeviceName(intptr_t screen_id, wchar_t* name, size_t cch) {
  DISPLAY_DEVICEW dd = {};
  dd.cb = sizeof(dd);
  if (screen_id < 0) {
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
      if ((dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) &&
          (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)) {
        wcsncpy_s(name, cch, dd.DeviceName, _TRUNCATE);
        return true;
      }
      dd = {};
      dd.cb = sizeof(dd);
    }
    return false;
  }
  if (!EnumDisplayDevicesW(nullptr, static_cast<DWORD>(screen_id), &dd, 0))
    return false;
  if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) return false;
  wcsncpy_s(name, cch, dd.DeviceName, _TRUNCATE);
  return true;
}

bool RTCDesktopCapturerImpl::InitGpu() {
  using Microsoft::WRL::ComPtr;
  // Deckel-Box aus dem Quality-Setting (max_width/max_height). g_target wird
  // erst NACH der Duplication berechnet: Desktop seitenverhaeltnis-korrekt in
  // die Box eingepasst, NIE hochskaliert (Monitor <= Box => nativ).
  uint32_t box_w = max_width_ ? max_width_ : 1920;
  uint32_t box_h = max_height_ ? max_height_ : 1080;
  if (box_w > 4096) box_w = 4096;  // H.264-Grenze
  if (box_h > 4096) box_h = 4096;
  HcCapLog("InitGpu Versuch: Deckel-Box=%ux%u", box_w, box_h);

  HRESULT hr;
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    HcCapLog("InitGpu FAIL @ CreateDXGIFactory1: hr=0x%08X", (unsigned)hr);
    return false;
  }
  ComPtr<IDXGIAdapter1> adapter, nvidia;
  int adapter_count = 0;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
    DXGI_ADAPTER_DESC1 d;
    adapter->GetDesc1(&d);
    // Outputs dieses Adapters zaehlen -> zeigt, ob der Desktop-Output im
    // honeycord-Prozess ueberhaupt an der NVIDIA haengt (Hybrid-Diagnose).
    UINT nout = 0;
    ComPtr<IDXGIOutput> o;
    for (UINT k = 0; adapter->EnumOutputs(k, &o) == S_OK; ++k) { nout++; o.Reset(); }
    HcCapLog("InitGpu adapter[%u]: vendor=0x%04X device=0x%04X flags=0x%X outputs=%u",
             i, d.VendorId, d.DeviceId, (unsigned)d.Flags, nout);
    if (d.VendorId == 0x10DE && !nvidia) nvidia = adapter;
    adapter.Reset();
    adapter_count++;
  }
  if (!nvidia) {
    HcCapLog("InitGpu FAIL @ kein-NVIDIA-Adapter (adapter_count=%d)", adapter_count);
    return false;
  }
  D3D_FEATURE_LEVEL fl;
  if (FAILED(hr = D3D11CreateDevice(nvidia.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                               nullptr, 0, D3D11_SDK_VERSION, &g_dev_, &fl,
                               &g_ctx_))) {
    HcCapLog("InitGpu FAIL @ D3D11CreateDevice(nvidia): hr=0x%08X", (unsigned)hr);
    return false;
  }
  // KRITISCH: Capture-Thread (Render) + Encoder-Thread (CopyResource) nutzen
  // denselben Immediate-Context -> Multithread-Schutz an, sonst Race/Crash.
  ComPtr<ID3D11Multithread> mt;
  if (SUCCEEDED(g_ctx_.As(&mt))) mt->SetMultithreadProtected(TRUE);

  // id12-Fix: den DXGI-Output des GEWAEHLTEN Monitors duplizieren (vorher stur
  // Output 0 -> "Screen 2 gewaehlt, Screen 1 gestreamt"). Mapping ueber den
  // Geraetenamen; haengt der gewaehlte Monitor nicht am NVIDIA-Adapter ->
  // false -> CPU-Fallback (SelectSource loest die ID dort korrekt auf).
  wchar_t want[32] = L"";
  const bool have_name =
      ScreenIdToDeviceName(static_cast<intptr_t>(source_id_), want,
                           sizeof(want) / sizeof(want[0]));
  HcCapLog("InitGpu: source_id=%lld -> device='%ls' (aufgeloest=%d)",
           (long long)source_id_, want, have_name ? 1 : 0);
  ComPtr<IDXGIOutput> out;
  if (have_name) {
    ComPtr<IDXGIOutput> cand;
    for (UINT k = 0; nvidia->EnumOutputs(k, &cand) == S_OK; ++k) {
      DXGI_OUTPUT_DESC od = {};
      if (SUCCEEDED(cand->GetDesc(&od)) && wcscmp(od.DeviceName, want) == 0) {
        HcCapLog("InitGpu: Output[%u] = '%ls' MATCH", k, od.DeviceName);
        out = cand;
        break;
      }
      cand.Reset();
    }
    if (!out) {
      HcCapLog("InitGpu FAIL @ Output '%ls' haengt nicht am NVIDIA-Adapter "
               "-> CPU-Fallback (der die ID korrekt aufloest)", want);
      ReleaseGpu();
      return false;
    }
  } else {
    // ID nicht aufloesbar (sollte nicht vorkommen): altes Verhalten als
    // letzte Rettung — Output 0.
    if (FAILED(hr = nvidia->EnumOutputs(0, &out))) {
      HcCapLog("InitGpu FAIL @ EnumOutputs(0) nvidia: hr=0x%08X", (unsigned)hr);
      ReleaseGpu(); return false;
    }
  }
  // Hybrid-GPU (NVIDIA dGPU + AMD iGPU) + Per-Monitor-DPI-aware Prozess (Flutter
  // ist das): das aeltere IDXGIOutput1::DuplicateOutput wirft hier E_INVALIDARG
  // (0x80070057). IDXGIOutput5::DuplicateOutput1 mit expliziter Formatliste ist
  // der dokumentierte, DPI-aware-/Hybrid-taugliche Weg -> zuerst versuchen, nur
  // BGRA anbieten (damit der nachgelagerte BGRA-Cache + Shader stimmen; DXGI
  // konvertiert noetigenfalls aus einem HDR-Desktop-Format). Fallback aufs alte
  // API fuer Nicht-Hybrid-/Aeltere-Systeme.
  ComPtr<IDXGIOutput5> out5;
  if (SUCCEEDED(out.As(&out5))) {
    const DXGI_FORMAT fmts[] = { DXGI_FORMAT_B8G8R8A8_UNORM };
    hr = out5->DuplicateOutput1(g_dev_.Get(), 0,
                                (UINT)(sizeof(fmts) / sizeof(fmts[0])), fmts,
                                &g_dup_);
    if (SUCCEEDED(hr)) {
      HcCapLog("InitGpu: DuplicateOutput1 OK (Hybrid/DPI-Pfad)");
    } else {
      HcCapLog("InitGpu: DuplicateOutput1 hr=0x%08X -> Fallback auf DuplicateOutput",
               (unsigned)hr);
      g_dup_.Reset();
    }
  } else {
    HcCapLog("InitGpu: kein IDXGIOutput5 -> DuplicateOutput");
  }
  if (!g_dup_) {
    ComPtr<IDXGIOutput1> out1;
    if (FAILED(hr = out.As(&out1))) {
      HcCapLog("InitGpu FAIL @ IDXGIOutput1-QueryInterface: hr=0x%08X", (unsigned)hr);
      ReleaseGpu(); return false;
    }
    if (FAILED(hr = out1->DuplicateOutput(g_dev_.Get(), &g_dup_))) {
      HcCapLog("InitGpu FAIL @ DuplicateOutput (auch +1 ging nicht): hr=0x%08X "
               "(80070057=E_INVALIDARG 887A0004=UNSUPPORTED "
               "887A0022=NOT_CURRENTLY_AVAILABLE)", (unsigned)hr);
      ReleaseGpu();
      return false;
    }
  }
  DXGI_OUTDUPL_DESC dd;
  g_dup_->GetDesc(&dd);
  g_desk_w_ = dd.ModeDesc.Width;
  g_desk_h_ = dd.ModeDesc.Height;
  // g_target: Desktop seitenverhaeltnis-korrekt in die Deckel-Box einpassen.
  // s<=1 => nie hochskalieren (Monitor <= Box => nativ encoden, kein Downscale
  // + keine Verzerrung; 5120x1440 @ Box 1920x1080 -> 1920x540). Gerade Maße.
  {
    double sx = static_cast<double>(box_w) / static_cast<double>(g_desk_w_);
    double sy = static_cast<double>(box_h) / static_cast<double>(g_desk_h_);
    double s = sx < sy ? sx : sy;
    if (s > 1.0) s = 1.0;
    g_target_w_ = static_cast<uint32_t>(static_cast<double>(g_desk_w_) * s) & ~1u;
    g_target_h_ = static_cast<uint32_t>(static_cast<double>(g_desk_h_) * s) & ~1u;
    if (g_target_w_ < 2) g_target_w_ = 2;
    if (g_target_h_ < 2) g_target_h_ = 2;
  }

  ComPtr<ID3DBlob> vsb, psb, err;
  if (FAILED(D3DCompile(kHcShaderHLSL, strlen(kHcShaderHLSL), nullptr, nullptr,
                        nullptr, "VSMain", "vs_5_0", 0, 0, &vsb, &err)) ||
      FAILED(D3DCompile(kHcShaderHLSL, strlen(kHcShaderHLSL), nullptr, nullptr,
                        nullptr, "PSMain", "ps_5_0", 0, 0, &psb, &err))) {
    HcCapLog("InitGpu FAIL @ D3DCompile (D3DCompiler_47.dll fehlt?)");
    ReleaseGpu();
    return false;
  }
  if (FAILED(g_dev_->CreateVertexShader(vsb->GetBufferPointer(),
                                        vsb->GetBufferSize(), nullptr, &g_vs_)) ||
      FAILED(g_dev_->CreatePixelShader(psb->GetBufferPointer(),
                                       psb->GetBufferSize(), nullptr, &g_ps_))) {
    HcCapLog("InitGpu FAIL @ Create*Shader");
    ReleaseGpu();
    return false;
  }
  D3D11_SAMPLER_DESC sd = {};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(g_dev_->CreateSamplerState(&sd, &g_smp_))) {
    HcCapLog("InitGpu FAIL @ CreateSamplerState");
    ReleaseGpu(); return false;
  }

  D3D11_TEXTURE2D_DESC cd = {};
  cd.Width = g_desk_w_; cd.Height = g_desk_h_; cd.MipLevels = 1; cd.ArraySize = 1;
  cd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; cd.SampleDesc.Count = 1;
  cd.Usage = D3D11_USAGE_DEFAULT; cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(g_dev_->CreateTexture2D(&cd, nullptr, &g_cached_)) ||
      FAILED(g_dev_->CreateShaderResourceView(g_cached_.Get(), nullptr, &g_srv_))) {
    HcCapLog("InitGpu FAIL @ Cached-Texture/SRV (%ux%u)", g_desk_w_, g_desk_h_);
    ReleaseGpu();
    return false;
  }
  D3D11_TEXTURE2D_DESC od = {};
  od.Width = g_target_w_; od.Height = g_target_h_; od.MipLevels = 1; od.ArraySize = 1;
  od.Format = DXGI_FORMAT_B8G8R8A8_UNORM; od.SampleDesc.Count = 1;
  od.Usage = D3D11_USAGE_DEFAULT; od.BindFlags = D3D11_BIND_RENDER_TARGET;
  for (int i = 0; i < kGpuPool; ++i) {
    if (FAILED(g_dev_->CreateTexture2D(&od, nullptr, &g_out_[i])) ||
        FAILED(g_dev_->CreateRenderTargetView(g_out_[i].Get(), nullptr,
                                              &g_rtv_[i]))) {
      HcCapLog("InitGpu FAIL @ Pool-Texture[%d] (%ux%u)", i, g_target_w_, g_target_h_);
      ReleaseGpu();
      return false;
    }
  }
  // GPU-Vorschau: zusaetzliche KEYED_MUTEX-Shared-Textur. Pro Frame kopieren wir
  // das fertige BGRA hier rein; ihr Legacy-Shared-Handle (IDXGIResource::
  // GetSharedHandle, wie von Flutters GpuSurfaceTexture erwartet) geht an den
  // Renderer, der ihn auf ANGLEs Device oeffnet. NICHT fatal: scheitert das,
  // bleibt nur die Vorschau auf dem CPU-Pfad, der Sende-/Encode-Pfad laeuft
  // unveraendert.
  {
    D3D11_TEXTURE2D_DESC shd = {};
    shd.Width = g_target_w_; shd.Height = g_target_h_;
    shd.MipLevels = 1; shd.ArraySize = 1;
    shd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; shd.SampleDesc.Count = 1;
    shd.Usage = D3D11_USAGE_DEFAULT;
    shd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    // Legacy-Shared (KEIN Keyed-Mutex!): Flutters ANGLE oeffnet das via
    // EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE und fasst laut Engine-Quelle KEINEN
    // Keyed-Mutex an. Eine KEYEDMUTEX-Textur ohne AcquireSync zu benutzen ist ein
    // D3D-Device-Error -> korrumpiert ANGLEs (von der ganzen App geteilten)
    // Kontext -> komplette UI kaputt (Versuch 1). Also plain SHARED + Flush.
    shd.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    // RING: kShareRing Texturen mit je eigenem Legacy-Handle (s. Header) -> das
    // pro Frame rotierte Handle erzwingt Flutters eglBindTexImage-Re-Bind.
    bool ring_ok = true;
    for (int i = 0; i < kShareRing; ++i) {
      ComPtr<IDXGIResource> shres;
      if (!(SUCCEEDED(g_dev_->CreateTexture2D(&shd, nullptr, &g_shared_tex_[i])) &&
            SUCCEEDED(g_shared_tex_[i].As(&shres)) &&
            SUCCEEDED(shres->GetSharedHandle(&g_shared_handle_[i])) &&
            g_shared_handle_[i])) {
        ring_ok = false;
        break;
      }
    }
    if (ring_ok) {
      HcCapLog("InitGpu: Vorschau-Shared-Ring ok (%d Tex, SHARED, kein KeyedMutex) handle[0]=%p",
               kShareRing, g_shared_handle_[0]);
    } else {
      for (auto& t : g_shared_tex_) t.Reset();
      g_shared_handle_.fill(nullptr);
      HcCapLog("InitGpu: Vorschau-Shared-Ring FEHLGESCHLAGEN (Vorschau bleibt CPU)");
    }
  }

  HcCapLog("InitGpu OK: Desktop %ux%u -> %ux%u (Zero-Copy GPU-Pfad aktiv)",
           g_desk_w_, g_desk_h_, g_target_w_, g_target_h_);
  return true;
}

// Geteilte GPU-Pipeline (Shader/Sampler/g_cached_+SRV/Output-Pool/Vorschau-Ring)
// fuer die WGC-Fenster-Quelle, parametrisiert ueber die Quellgroesse. Spiegelt die
// Pipeline aus InitGpu (DXGI-Bildschirm), damit der gesamte Downscale/Ring/NVENC/
// Steady-Cadence-Pfad in GpuCaptureFrame unveraendert wiederverwendet wird.
bool RTCDesktopCapturerImpl::InitGpuPipeline(uint32_t src_w, uint32_t src_h) {
  using Microsoft::WRL::ComPtr;
  if (src_w < 2 || src_h < 2) {
    HcCapLog("InitGpuPipeline FAIL @ ungueltige Quellgroesse %ux%u", src_w, src_h);
    return false;
  }
  g_desk_w_ = src_w;
  g_desk_h_ = src_h;
  uint32_t box_w = max_width_ ? max_width_ : 1920;
  uint32_t box_h = max_height_ ? max_height_ : 1080;
  if (box_w > 4096) box_w = 4096;
  if (box_h > 4096) box_h = 4096;
  {
    double sx = static_cast<double>(box_w) / static_cast<double>(g_desk_w_);
    double sy = static_cast<double>(box_h) / static_cast<double>(g_desk_h_);
    double s = sx < sy ? sx : sy;
    if (s > 1.0) s = 1.0;
    g_target_w_ = static_cast<uint32_t>(static_cast<double>(g_desk_w_) * s) & ~1u;
    g_target_h_ = static_cast<uint32_t>(static_cast<double>(g_desk_h_) * s) & ~1u;
    if (g_target_w_ < 2) g_target_w_ = 2;
    if (g_target_h_ < 2) g_target_h_ = 2;
  }
  ComPtr<ID3DBlob> vsb, psb, err;
  if (FAILED(D3DCompile(kHcShaderHLSL, strlen(kHcShaderHLSL), nullptr, nullptr,
                        nullptr, "VSMain", "vs_5_0", 0, 0, &vsb, &err)) ||
      FAILED(D3DCompile(kHcShaderHLSL, strlen(kHcShaderHLSL), nullptr, nullptr,
                        nullptr, "PSMain", "ps_5_0", 0, 0, &psb, &err))) {
    HcCapLog("InitGpuPipeline FAIL @ D3DCompile");
    ReleaseGpu(); return false;
  }
  if (FAILED(g_dev_->CreateVertexShader(vsb->GetBufferPointer(),
                                        vsb->GetBufferSize(), nullptr, &g_vs_)) ||
      FAILED(g_dev_->CreatePixelShader(psb->GetBufferPointer(),
                                       psb->GetBufferSize(), nullptr, &g_ps_))) {
    HcCapLog("InitGpuPipeline FAIL @ Create*Shader");
    ReleaseGpu(); return false;
  }
  D3D11_SAMPLER_DESC sd = {};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(g_dev_->CreateSamplerState(&sd, &g_smp_))) {
    HcCapLog("InitGpuPipeline FAIL @ CreateSamplerState");
    ReleaseGpu(); return false;
  }
  D3D11_TEXTURE2D_DESC cd = {};
  cd.Width = g_desk_w_; cd.Height = g_desk_h_; cd.MipLevels = 1; cd.ArraySize = 1;
  cd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; cd.SampleDesc.Count = 1;
  cd.Usage = D3D11_USAGE_DEFAULT; cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(g_dev_->CreateTexture2D(&cd, nullptr, &g_cached_)) ||
      FAILED(g_dev_->CreateShaderResourceView(g_cached_.Get(), nullptr, &g_srv_))) {
    HcCapLog("InitGpuPipeline FAIL @ Cached-Texture/SRV (%ux%u)", g_desk_w_, g_desk_h_);
    ReleaseGpu(); return false;
  }
  D3D11_TEXTURE2D_DESC od = {};
  od.Width = g_target_w_; od.Height = g_target_h_; od.MipLevels = 1; od.ArraySize = 1;
  od.Format = DXGI_FORMAT_B8G8R8A8_UNORM; od.SampleDesc.Count = 1;
  od.Usage = D3D11_USAGE_DEFAULT; od.BindFlags = D3D11_BIND_RENDER_TARGET;
  for (int i = 0; i < kGpuPool; ++i) {
    if (FAILED(g_dev_->CreateTexture2D(&od, nullptr, &g_out_[i])) ||
        FAILED(g_dev_->CreateRenderTargetView(g_out_[i].Get(), nullptr, &g_rtv_[i]))) {
      HcCapLog("InitGpuPipeline FAIL @ Pool-Texture[%d]", i);
      ReleaseGpu(); return false;
    }
  }
  {
    D3D11_TEXTURE2D_DESC shd = {};
    shd.Width = g_target_w_; shd.Height = g_target_h_;
    shd.MipLevels = 1; shd.ArraySize = 1;
    shd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; shd.SampleDesc.Count = 1;
    shd.Usage = D3D11_USAGE_DEFAULT;
    shd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    shd.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    bool ring_ok = true;
    for (int i = 0; i < kShareRing; ++i) {
      ComPtr<IDXGIResource> shres;
      if (!(SUCCEEDED(g_dev_->CreateTexture2D(&shd, nullptr, &g_shared_tex_[i])) &&
            SUCCEEDED(g_shared_tex_[i].As(&shres)) &&
            SUCCEEDED(shres->GetSharedHandle(&g_shared_handle_[i])) &&
            g_shared_handle_[i])) {
        ring_ok = false; break;
      }
    }
    if (!ring_ok) {
      for (auto& t : g_shared_tex_) t.Reset();
      g_shared_handle_.fill(nullptr);
      HcCapLog("InitGpuPipeline: Vorschau-Shared-Ring FEHLGESCHLAGEN");
    }
  }
  g_last_out_idx_ = -1;  // nach (Re-)Init: Steady-Cadence wartet auf echten Frame
  HcCapLog("InitGpuPipeline OK: Quelle %ux%u -> %ux%u", g_desk_w_, g_desk_h_,
           g_target_w_, g_target_h_);
  return true;
}

// WGC-Zero-Copy-Capture eines FENSTERS: NVIDIA-Device + WGC-Item(HWND) +
// FreeThreaded-Frame-Pool/Session auf g_dev_ + geteilte Downscale-Pipeline. Pro
// Frame liefert WgcAcquire die GPU-Textur direkt (kein CPU-Readback wie GDI).
bool RTCDesktopCapturerImpl::InitGpuWindow(intptr_t hwnd) {
  using Microsoft::WRL::ComPtr;
  HcCapLog("InitGpuWindow Versuch (WGC) hwnd=%p", (void*)hwnd);
  if (!hwnd) return false;

  // --- NVIDIA-Device (wie InitGpu) ---
  HRESULT hr;
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    HcCapLog("InitGpuWindow FAIL @ CreateDXGIFactory1: 0x%08X", (unsigned)hr);
    return false;
  }
  ComPtr<IDXGIAdapter1> adapter, nvidia;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
    DXGI_ADAPTER_DESC1 d; adapter->GetDesc1(&d);
    if (d.VendorId == 0x10DE && !nvidia) nvidia = adapter;
    adapter.Reset();
  }
  if (!nvidia) { HcCapLog("InitGpuWindow FAIL @ kein NVIDIA-Adapter"); return false; }
  D3D_FEATURE_LEVEL fl;
  if (FAILED(hr = D3D11CreateDevice(nvidia.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                    0, nullptr, 0, D3D11_SDK_VERSION, &g_dev_, &fl,
                                    &g_ctx_))) {
    HcCapLog("InitGpuWindow FAIL @ D3D11CreateDevice: 0x%08X", (unsigned)hr);
    return false;
  }
  ComPtr<ID3D11Multithread> mt;
  if (SUCCEEDED(g_ctx_.As(&mt))) mt->SetMultithreadProtected(TRUE);

  wgc_.reset(new WgcState());

  // --- WGC-Item aus dem Fenster ---
  ComPtr<IGraphicsCaptureItemInterop> interop;
  hr = webrtc::GetActivationFactory<
      IGraphicsCaptureItemInterop,
      RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem>(&interop);
  if (FAILED(hr)) { HcCapLog("InitGpuWindow FAIL @ ItemInterop: 0x%08X", (unsigned)hr); ReleaseGpu(); return false; }
  hr = interop->CreateForWindow(reinterpret_cast<HWND>(hwnd), IID_PPV_ARGS(&wgc_->item));
  if (FAILED(hr) || !wgc_->item) { HcCapLog("InitGpuWindow FAIL @ CreateForWindow: 0x%08X", (unsigned)hr); ReleaseGpu(); return false; }
  ABI::Windows::Graphics::SizeInt32 isize = {};
  wgc_->item->get_Size(&isize);
  if (isize.Width < 2 || isize.Height < 2) {
    HcCapLog("InitGpuWindow FAIL @ Item-Groesse %dx%d", isize.Width, isize.Height);
    ReleaseGpu(); return false;
  }

  // --- IDirect3DDevice aus unserem g_dev_ ---
  ComPtr<IDXGIDevice> dxgi_device;
  if (FAILED(g_dev_.As(&dxgi_device)) || !webrtc::ResolveCoreWinRTDirect3DDelayload()) {
    HcCapLog("InitGpuWindow FAIL @ DXGIDevice/Delayload"); ReleaseGpu(); return false;
  }
  hr = webrtc::CreateDirect3DDeviceFromDXGIDevice(dxgi_device.Get(), &wgc_->device);
  if (FAILED(hr)) { HcCapLog("InitGpuWindow FAIL @ CreateDirect3DDevice: 0x%08X", (unsigned)hr); ReleaseGpu(); return false; }

  // --- Frame-Pool (FreeThreaded, von unserem Capture-Thread pollbar) + Session ---
  ComPtr<WGC::IDirect3D11CaptureFramePoolStatics> fps;
  hr = webrtc::GetActivationFactory<
      WGC::IDirect3D11CaptureFramePoolStatics,
      RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool>(&fps);
  ComPtr<WGC::IDirect3D11CaptureFramePoolStatics2> fps2;
  if (FAILED(hr) || FAILED(fps.As(&fps2))) { HcCapLog("InitGpuWindow FAIL @ FramePoolStatics"); ReleaseGpu(); return false; }
  hr = fps2->CreateFreeThreaded(wgc_->device.Get(),
       WGDX::DirectXPixelFormat_B8G8R8A8UIntNormalized, 2, isize, &wgc_->pool);
  if (FAILED(hr)) { HcCapLog("InitGpuWindow FAIL @ CreateFreeThreaded: 0x%08X", (unsigned)hr); ReleaseGpu(); return false; }
  hr = wgc_->pool->CreateCaptureSession(wgc_->item.Get(), &wgc_->session);
  if (FAILED(hr)) { HcCapLog("InitGpuWindow FAIL @ CreateCaptureSession: 0x%08X", (unsigned)hr); ReleaseGpu(); return false; }
  // Gelbe WGC-Border weg (Win11) + Cursor wie gewuenscht.
  ComPtr<WGC::IGraphicsCaptureSession3> s3;
  if (SUCCEEDED(wgc_->session.As(&s3))) s3->put_IsBorderRequired(false);
  ComPtr<WGC::IGraphicsCaptureSession2> s2;
  if (SUCCEEDED(wgc_->session.As(&s2))) s2->put_IsCursorCaptureEnabled(show_cursor_ ? TRUE : FALSE);
  hr = wgc_->session->StartCapture();
  if (FAILED(hr)) { HcCapLog("InitGpuWindow FAIL @ StartCapture: 0x%08X", (unsigned)hr); ReleaseGpu(); return false; }

  wgc_w_ = static_cast<uint32_t>(isize.Width);
  wgc_h_ = static_cast<uint32_t>(isize.Height);
  wgc_mode_ = true;
  if (!InitGpuPipeline(wgc_w_, wgc_h_)) return false;  // ReleaseGpu schon drin
  HcCapLog("InitGpuWindow OK: Fenster %ux%u (WGC Zero-Copy aktiv)", wgc_w_, wgc_h_);
  return true;
}

// Naechsten WGC-Frame -> g_cached_ (Zero-Copy, kein CPU-Readback). *changed=false
// bei keinem neuen Frame (Stillstand -> Steady-Cadence) oder nach Resize (Frame
// uebersprungen). false = fataler Fehler -> Aufrufer faellt auf CPU-Capturer.
bool RTCDesktopCapturerImpl::WgcAcquire(bool* changed) {
  using Microsoft::WRL::ComPtr;
  *changed = false;
  if (!wgc_ || !wgc_->pool) return false;
  ComPtr<WGC::IDirect3D11CaptureFrame> frame;
  if (FAILED(wgc_->pool->TryGetNextFrame(&frame))) return true;  // transient
  if (!frame) return true;  // kein neuer Frame -> Stillstand

  ABI::Windows::Graphics::SizeInt32 csize = {};
  frame->get_ContentSize(&csize);
  if (csize.Width >= 2 && csize.Height >= 2 &&
      (static_cast<uint32_t>(csize.Width) != wgc_w_ ||
       static_cast<uint32_t>(csize.Height) != wgc_h_)) {
    // Fenster-Resize -> Frame-Pool + geteilte Pipeline auf neue Groesse.
    ComPtr<ABI::Windows::Foundation::IClosable> cl;
    if (SUCCEEDED(frame.As(&cl))) cl->Close();
    frame.Reset();
    if (FAILED(wgc_->pool->Recreate(wgc_->device.Get(),
            WGDX::DirectXPixelFormat_B8G8R8A8UIntNormalized, 2, csize))) {
      ReleaseGpu(); wgc_mode_ = false; return false;
    }
    wgc_w_ = static_cast<uint32_t>(csize.Width);
    wgc_h_ = static_cast<uint32_t>(csize.Height);
    if (!InitGpuPipeline(wgc_w_, wgc_h_)) { wgc_mode_ = false; return false; }
    return true;  // diesen Frame ueberspringen, naechster hat die neue Groesse
  }

  ComPtr<WGDX::Direct3D11::IDirect3DSurface> surf;
  if (FAILED(frame->get_Surface(&surf)) || !surf) return true;
  ComPtr<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> acc;
  if (FAILED(surf.As(&acc))) return true;
  ComPtr<ID3D11Texture2D> tex;
  if (FAILED(acc->GetInterface(IID_PPV_ARGS(&tex))) || !tex) return true;
  // Zero-Copy: Fenster-Textur (auf g_dev_) -> g_cached_ (gleiche Groesse). Danach
  // laeuft die geteilte Downscale/Ring/NVENC-Pipeline wie beim Bildschirm.
  g_ctx_->CopyResource(g_cached_.Get(), tex.Get());
  g_have_frame_ = true;
  *changed = true;
  ComPtr<ABI::Windows::Foundation::IClosable> cl;
  if (SUCCEEDED(frame.As(&cl))) cl->Close();
  return true;
}

void RTCDesktopCapturerImpl::ReleaseGpu() {
  g_dup_.Reset();
  for (auto& r : g_rtv_) r.Reset();
  for (auto& t : g_out_) t.Reset();
  g_srv_.Reset();
  g_cached_.Reset();
  g_smp_.Reset();
  g_ps_.Reset();
  g_vs_.Reset();
  // Vorschau-Shared-Ring. Die Legacy-Shared-Handles (GetSharedHandle) gehoeren
  // den Ressourcen und werden mit ihnen frei -> NICHT CloseHandle (nur NT-Handles).
  for (auto& t : g_shared_tex_) t.Reset();
  g_shared_handle_.fill(nullptr);
  g_share_idx_ = 0;
  g_last_out_idx_ = -1;
  // WGC-Fenster-Capture beenden (haelt Refs auf g_dev_).
  if (wgc_) {
    if (wgc_->session) {
      Microsoft::WRL::ComPtr<ABI::Windows::Foundation::IClosable> cl;
      if (SUCCEEDED(wgc_->session.As(&cl))) cl->Close();
    }
    if (wgc_->pool) {
      Microsoft::WRL::ComPtr<ABI::Windows::Foundation::IClosable> cl;
      if (SUCCEEDED(wgc_->pool.As(&cl))) cl->Close();
    }
    wgc_.reset();
  }
  wgc_mode_ = false;
  wgc_w_ = wgc_h_ = 0;
  g_ctx_.Reset();
  g_dev_.Reset();
  g_have_frame_ = false;
}

void RTCDesktopCapturerImpl::GpuCaptureFrame() {
  using Microsoft::WRL::ComPtr;
  bool changed = false;
  if (wgc_mode_) {
    // WGC-Fenster: naechste GPU-Textur -> g_cached_ (Zero-Copy). changed=false bei
    // Stillstand (-> Steady-Cadence-Wiederholung). false = diesen Frame skippen.
    if (!WgcAcquire(&changed)) return;
    if (!changed && !g_have_frame_) return;  // noch kein Frame
  } else if (g_dup_) {
    ComPtr<IDXGIResource> res;
    DXGI_OUTDUPL_FRAME_INFO fi;
    HRESULT a = g_dup_->AcquireNextFrame(15, &fi, &res);
    if (a == S_OK) {
      ComPtr<ID3D11Texture2D> desk;
      if (SUCCEEDED(res.As(&desk)))
        g_ctx_->CopyResource(g_cached_.Get(), desk.Get());
      g_dup_->ReleaseFrame();
      g_have_frame_ = true;
      changed = true;  // neuer Bildinhalt
    } else if (a == DXGI_ERROR_WAIT_TIMEOUT) {
      if (!g_have_frame_) return;  // noch kein Frame -> nichts senden
      // statisch: kein neuer Inhalt (changed bleibt false)
    } else if (a == DXGI_ERROR_ACCESS_LOST) {
      // Modus-/Aufloesungswechsel -> GPU-Pfad fallenlassen (CPU uebernimmt).
      ReleaseGpu();
      gpu_mode_ = false;
      return;
    } else {
      return;
    }
  } else {
    return;  // GPU-Pfad abgebaut (z.B. WGC-Teardown) -> nichts tun (kein Crash)
  }

  // Stetiger Sende-Takt statt DXGI-gekoppelter Rate: bei UNVERAENDERTEM Bild
  // (AcquireNextFrame-Timeout) NICHT skippen, sondern den letzten fertigen
  // Downscale-Frame im festen Loop-Takt erneut senden. Das ergibt billige
  // H.264-Skip-Frames (NVENC ~0,3ms, ~0 Bytes) und verhindert sowohl den
  // 4-fps-Einbruch bei Stillstand als auch das FPS-Flackern bei Video/Bewegung
  // (Luecken werden mit dem letzten Frame gefuellt). KEIN Re-Grab/Re-Downscale
  // im Stillstand -> die g_out_-Textur wird wiederverwendet (Last bleibt gering).
  // Self-View bleibt separat auf ~25fps gedrosselt.
  int out_idx;
  if (changed) {
    // GPU-Downscale: g_cached_(SRV) -> Pool-Textur(RTV). Round-Robin, damit der
    // Encoder-Thread die gerade gelesene Textur nicht ueberschrieben bekommt.
    out_idx = g_pool_idx_;
    g_pool_idx_ = (g_pool_idx_ + 1) % kGpuPool;
    ID3D11RenderTargetView* rtv = g_rtv_[out_idx].Get();
    ID3D11ShaderResourceView* srv = g_srv_.Get();
    ID3D11SamplerState* smp = g_smp_.Get();
    D3D11_VIEWPORT vp = {0, 0, (FLOAT)g_target_w_, (FLOAT)g_target_h_, 0, 1};
    g_ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx_->RSSetViewports(1, &vp);
    g_ctx_->VSSetShader(g_vs_.Get(), nullptr, 0);
    g_ctx_->PSSetShader(g_ps_.Get(), nullptr, 0);
    g_ctx_->PSSetShaderResources(0, 1, &srv);
    g_ctx_->PSSetSamplers(0, 1, &smp);
    g_ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx_->IASetInputLayout(nullptr);
    g_ctx_->Draw(3, 0);
    ID3D11ShaderResourceView* nullsrv = nullptr;
    g_ctx_->PSSetShaderResources(0, 1, &nullsrv);
    g_last_out_idx_ = out_idx;
  } else {
    // Stillstand: letzten fertigen Frame wiederholen (kein Re-Grab/Re-Downscale).
    if (g_last_out_idx_ < 0) return;  // noch nichts zu wiederholen
    out_idx = g_last_out_idx_;
  }

  // GPU-Vorschau: fertiges Bild reihum in die naechste Shared-Textur des Rings
  // kopieren und DEREN Handle durchreichen. So sieht Flutter pro Frame ein neues
  // Handle und re-bindet die EGL-Surface (eglBindTexImage) jedes Frame statt nur
  // 1x -> kein Einfrieren bei In-place-Updates (beige Vorschau). Der Ring gibt
  // zugleich Producer/Consumer-Trennung. KEIN Keyed-Mutex (ANGLE bedient keinen).
  HANDLE share_handle = nullptr;
  if (g_shared_tex_[0]) {
    int sidx = g_share_idx_;
    g_share_idx_ = (g_share_idx_ + 1) % kShareRing;
    g_ctx_->CopyResource(g_shared_tex_[sidx].Get(), g_out_[out_idx].Get());
    share_handle = g_shared_handle_[sidx];
  }

  g_ctx_->Flush();  // sicherstellen, dass der Render fertig ist, bevor der
                    // Encoder-Thread aus der Pool-Textur kopiert.

  auto buf = honeycord::D3D11FrameBuffer::Create(
      g_dev_.Get(), g_out_[out_idx].Get(), (int)g_target_w_, (int)g_target_h_,
      share_handle);
  OnFrame(webrtc::VideoFrame(buf, 0, webrtc::TimeMillis(),
                             webrtc::kVideoRotation_0));
}
#endif  // _WIN32

void RTCDesktopCapturerImpl::CaptureFrame() {
  RTC_DCHECK_RUN_ON(thread_.get());
  if (capture_state_ == CS_RUNNING) {
#ifdef _WIN32
    if (gpu_mode_) {
      int64_t gt0 = webrtc::TimeMillis();
      GpuCaptureFrame();
      int64_t gel = webrtc::TimeMillis() - gt0;
      int64_t gn = static_cast<int64_t>(capture_delay_) - gel;
      if (gn < 0) gn = 0;
      static int s_gpu_dbg = 0;
      if ((s_gpu_dbg++ % 120) == 0)
        HcCapLog("gpu cap: total=%lldms next=%lldms %ux%u->%ux%u",
                 (long long)gel, (long long)gn, g_desk_w_, g_desk_h_,
                 g_target_w_, g_target_h_);
      thread_->PostDelayedHighPrecisionTask(
          [this]() { CaptureFrame(); }, webrtc::TimeDelta::Millis(gn));
      return;
    }
#endif
    // honeycord: schedule the NEXT grab at a steady cadence measured from the
    // START of this one. The old code added capture_delay_ AFTER the
    // synchronous grab + ARGB->I420 convert + scale had finished, so the real
    // period was (delay + processing). At 1080p the convert/scale roughly
    // matches the delay, which halved the effective rate (33ms delay -> ~66ms
    // -> 15fps). Subtract the elapsed work so we hit capture_delay_, not
    // delay+work.
    int64_t t0 = webrtc::TimeMillis();
    capturer_->CaptureFrame();
    int64_t elapsed = webrtc::TimeMillis() - t0;
    int64_t next_ms = static_cast<int64_t>(capture_delay_) - elapsed;
    if (next_ms < 0) next_ms = 0;
    static int s_cap_dbg = 0;
    if ((s_cap_dbg++ % 120) == 0) {
      HcCapLog("cap loop: total=%lldms grab=%lldms convert+scale=%dms "
               "src=%dx%d out=%dx%d delay=%ums next=%lldms",
               (long long)elapsed, (long long)(elapsed - g_hc_conv_ms),
               g_hc_conv_ms, g_hc_src_w, g_hc_src_h, g_hc_dst_w, g_hc_dst_h,
               capture_delay_, (long long)next_ms);
    }
    thread_->PostDelayedHighPrecisionTask(
        [this]() { CaptureFrame(); },
        webrtc::TimeDelta::Millis(next_ms));
  }
}

}  // namespace libwebrtc
