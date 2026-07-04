/*
 *  Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "src/internal/vcm_capturer.h"

#include <stdint.h>

#include <memory>

#include "modules/video_capture/video_capture_factory.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"

#ifdef _WIN32
// Kamera-Telemetrie (2026-07-03, Abendtest "Win-Kamera 30->24->15 fps"):
// loggt verhandeltes Format + tatsaechlich gelieferte fps 1x/min nach
// %LOCALAPPDATA%\HoneyCord\cam.log (rotiert). Beantwortet im Feld sofort:
// WAS wurde mit der Kamera verhandelt (Format! MJPEG vs RAW = USB-Bandbreite)
// und WAS liefert sie wirklich.
#include <windows.h>
#include <cstdio>
#include <cstring>
namespace {
void CamRotateIfNeeded(const char* path) {
  char old_path[MAX_PATH + 8];
  std::snprintf(old_path, sizeof(old_path), "%s.old", path);
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (GetFileAttributesExA(old_path, GetFileExInfoStandard, &fad)) {
    FILETIME now_ft;
    GetSystemTimeAsFileTime(&now_ft);
    ULARGE_INTEGER now_u{}, old_u{};
    now_u.LowPart = now_ft.dwLowDateTime;
    now_u.HighPart = now_ft.dwHighDateTime;
    old_u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
    old_u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    if (now_u.QuadPart > old_u.QuadPart &&
        now_u.QuadPart - old_u.QuadPart > 14ULL * 24 * 3600 * 10000000ULL) {
      DeleteFileA(old_path);
    }
  }
  if (GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) {
    unsigned long long size =
        (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) |
        fad.nFileSizeLow;
    if (size > 1024ULL * 1024ULL)
      MoveFileExA(path, old_path, MOVEFILE_REPLACE_EXISTING);
  }
}
void CamLog(const char* fmt, ...) {
  char path[MAX_PATH];
  DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return;
  std::strncat(path, "\\HoneyCord", MAX_PATH - n - 1);
  CreateDirectoryA(path, nullptr);
  std::strncat(path, "\\cam.log", MAX_PATH - std::strlen(path) - 1);
  static bool rotated = false;
  if (!rotated) {
    CamRotateIfNeeded(path);
    rotated = true;
  }
  if (FILE* f = std::fopen(path, "a")) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::fprintf(f, "[cam %02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fclose(f);
  }
}
const char* CamVideoTypeName(webrtc::VideoType t) {
  switch (t) {
    case webrtc::VideoType::kI420: return "I420";
    case webrtc::VideoType::kMJPEG: return "MJPEG";
    case webrtc::VideoType::kYUY2: return "YUY2";
    case webrtc::VideoType::kNV12: return "NV12";
    case webrtc::VideoType::kRGB24: return "RGB24";
    case webrtc::VideoType::kARGB: return "ARGB";
    case webrtc::VideoType::kUYVY: return "UYVY";
    default: return "andere";
  }
}
}  // namespace
#endif  // _WIN32

namespace webrtc {
namespace internal {

VcmCapturer::VcmCapturer(webrtc::Thread* worker_thread)
    : vcm_(nullptr), worker_thread_(worker_thread) {}

bool VcmCapturer::Init(size_t width, size_t height, size_t target_fps,
                       size_t capture_device_index) {
  std::unique_ptr<VideoCaptureModule::DeviceInfo> device_info(
      VideoCaptureFactory::CreateDeviceInfo());

  char device_name[256];
  char unique_name[256];
  if (device_info->GetDeviceName(static_cast<uint32_t>(capture_device_index),
                                 device_name, sizeof(device_name), unique_name,
                                 sizeof(unique_name)) != 0) {
    Destroy();
    return false;
  }

  vcm_ = webrtc::VideoCaptureFactory::Create(unique_name);

  if (!vcm_) {
    return false;
  }

  vcm_->RegisterCaptureDataCallback(this);

  const char* uid = vcm_->CurrentDeviceName();
  const int32_t wantW = static_cast<int32_t>(width);
  const int32_t wantH = static_cast<int32_t>(height);
  const int32_t wantF = static_cast<int32_t>(target_fps);

  // Format-Wahl (2026-07-04): FRUEHER wurde stur Capability[0] genommen und
  // videoType hart auf kI420 gezwungen. Fuer USB-Webcams ist das die
  // Bandbreiten-Falle: 720p60 als RAW (I420/YUY2) = ~660 Mbps -> Kamera oeffnet
  // langsam (#5) und faellt unter Last auf 24/15 fps (Abendtest). Native
  // Webcams liefern hohe Aufloesung/fps als MJPEG (~10x kleiner, von libwebrtc
  // intern billig nach I420 dekodiert) — genau das nutzt auch die Windows-
  // Kamera-App (die deshalb fluessig lief). Also: alle Geraete-Capabilities
  // durchsuchen und die beste fuer (Wunsch-Aufloesung, -fps, Format) waehlen.
  const bool prefer_mjpeg = (wantW * wantH) >= (1280 * 720);
  // Timing (#5 „Kamera oeffnet langsam"): NumberOfCapabilities baut auf Windows
  // die DirectShow-Capability-Map (verbindet sich mit dem Geraet, enumeriert
  // Medientypen) — klassischer Zeitfresser. Getrennt messen, um Enumeration vs
  // Geraete-Open vs erster Frame im Feld zu unterscheiden.
#ifdef _WIN32
  const int64_t t_caps0 = static_cast<int64_t>(GetTickCount64());
#endif
  const int32_t ncaps = device_info->NumberOfCapabilities(uid);
#ifdef _WIN32
  cap_map_ms_ = static_cast<int64_t>(GetTickCount64()) - t_caps0;
#endif
  VideoCaptureCapability best;
  bool have_best = false;
  int64_t best_score = 0;
  for (int32_t i = 0; i < ncaps; ++i) {
    VideoCaptureCapability c;
    if (device_info->GetCapability(uid, i, c) != 0) continue;
    if (c.width <= 0 || c.height <= 0) continue;
    const int32_t dw = c.width > wantW ? c.width - wantW : wantW - c.width;
    const int32_t dh = c.height > wantH ? c.height - wantH : wantH - c.height;
    // Aufloesung dominiert; darunter fps (unter Ziel STARK bestrafen, ueber Ziel
    // nur leicht); Format entscheidet nur noch Gleichstaende.
    int64_t score = (static_cast<int64_t>(dw) + dh) * 100000;
    if (c.maxFPS >= wantF)
      score += (c.maxFPS - wantF);
    else
      score += static_cast<int64_t>(wantF - c.maxFPS) * 1000;
    int type_pref;
    if (prefer_mjpeg) {
      type_pref = (c.videoType == VideoType::kMJPEG)  ? 0
                  : (c.videoType == VideoType::kNV12)  ? 20
                  : (c.videoType == VideoType::kYUY2 ||
                     c.videoType == VideoType::kUYVY)  ? 30
                  : (c.videoType == VideoType::kI420)  ? 40
                                                       : 60;
    } else {
      // Niedrige Aufloesung: RAW bevorzugen (kein MJPEG-Decode noetig).
      type_pref = (c.videoType == VideoType::kNV12)   ? 0
                  : (c.videoType == VideoType::kI420)  ? 10
                  : (c.videoType == VideoType::kYUY2 ||
                     c.videoType == VideoType::kUYVY)  ? 20
                  : (c.videoType == VideoType::kMJPEG) ? 40
                                                       : 60;
    }
    score += type_pref;
    if (!have_best || score < best_score) {
      best = c;
      best_score = score;
      have_best = true;
    }
  }

  if (have_best) {
    // Echte Geraete-Capability uebernehmen (inkl. korrektem videoType) — NICHT
    // mehr Aufloesung/fps ueberschreiben, sonst verhandelt DirectShow neu.
    capability_ = best;
#ifdef _WIN32
    CamLog("Init: Wunsch %dx%d@%d prefMJPEG=%d -> gewaehlt %dx%d@%d Format=%s (aus %d Caps, CapMap=%lldms)",
           wantW, wantH, wantF, prefer_mjpeg ? 1 : 0, capability_.width,
           capability_.height, capability_.maxFPS,
           CamVideoTypeName(capability_.videoType), ncaps, (long long)cap_map_ms_);
#endif
  } else {
    // Fallback: alte Heuristik (Index 0 + Wunschwerte, I420-Konvertierung).
    device_info->GetCapability(uid, 0, capability_);
    capability_.width = wantW;
    capability_.height = wantH;
    capability_.maxFPS = wantF;
    capability_.videoType = VideoType::kI420;
#ifdef _WIN32
    CamLog("Init: keine Caps gefunden (%d) -> Fallback %dx%d@%d I420", ncaps,
           wantW, wantH, wantF);
#endif
  }

  return true;
}

std::shared_ptr<VcmCapturer> VcmCapturer::Create(webrtc::Thread* worker_thread,
                                                 size_t width, size_t height,
                                                 size_t target_fps,
                                                 size_t capture_device_index) {
  std::shared_ptr<VcmCapturer> vcm_capturer(
      std::make_shared<VcmCapturer>(worker_thread));
  if (!vcm_capturer->Init(width, height, target_fps, capture_device_index)) {
    RTC_LOG(LS_WARNING) << "Failed to create VcmCapturer(w = " << width
                        << ", h = " << height << ", fps = " << target_fps
                        << ")";
    return nullptr;
  }
  return vcm_capturer;
}

bool VcmCapturer::StartCapture() {
#ifdef _WIN32
  const int64_t t_start0 = static_cast<int64_t>(GetTickCount64());
#endif
  int32_t result = worker_thread_->BlockingCall(
      [&] { return vcm_->StartCapture(capability_); });

  if (result != 0) {
#ifdef _WIN32
    CamLog("StartCapture FEHLGESCHLAGEN (angefordert %dx%d@%d)",
           capability_.width, capability_.height, capability_.maxFPS);
#endif
    Destroy();
    return false;
  }

#ifdef _WIN32
  // capability_ ist jetzt die ECHTE, in Init() gewaehlte Geraete-Capability
  // (inkl. wahrem videoType) — kein CaptureSettings-Echo mehr noetig.
  // StartCapture-Dauer = DirectShow-Graph bauen + Geraet hochfahren (der zweite
  // grosse Zeitblock beim Kamera-Oeffnen). Erster Frame folgt separat (OnFrame).
  const int64_t start_ms = static_cast<int64_t>(GetTickCount64()) - t_start0;
  CamLog("Start OK: %dx%d@%d Format=%s (StartCapture=%lldms, CapMap war %lldms)",
         capability_.width, capability_.height, capability_.maxFPS,
         CamVideoTypeName(capability_.videoType), (long long)start_ms,
         (long long)cap_map_ms_);
#endif
  return true;
}

bool VcmCapturer::CaptureStarted() {
  return vcm_ != nullptr &&
         worker_thread_->BlockingCall([&] { return vcm_->CaptureStarted(); });
}

void VcmCapturer::StopCapture() {
  worker_thread_->BlockingCall([&] {
    vcm_->StopCapture();
    // Release reference to VCM.
    vcm_ = nullptr;
  });
}

void VcmCapturer::Destroy() {
  if (!vcm_) return;

  vcm_->DeRegisterCaptureDataCallback();

  StopCapture();
}

VcmCapturer::~VcmCapturer() { Destroy(); }

void VcmCapturer::OnFrame(const VideoFrame& frame) {
#ifdef _WIN32
  // Geliefert-fps 1x/min: DAS ist die Zahl, die im Abendtest heimlich auf
  // 24/15 fiel. Ab jetzt objektiv im Log statt am Badge abgelesen.
  const int64_t now = static_cast<int64_t>(GetTickCount64());
  if (cam_dbg_start_ms_ == 0) cam_dbg_start_ms_ = now;
  ++cam_dbg_frames_;
  if (now - cam_dbg_start_ms_ >= 60000) {
    CamLog("liefert %.1f fps (%dx%d)",
           cam_dbg_frames_ * 1000.0 / (now - cam_dbg_start_ms_), frame.width(),
           frame.height());
    cam_dbg_frames_ = 0;
    cam_dbg_start_ms_ = now;
  }
#endif
  VideoCapturer::OnFrame(frame);
}

webrtc::scoped_refptr<CapturerTrackSource> CapturerTrackSource::Create(
    webrtc::Thread* worker_thread) {
  const size_t kWidth = 640;
  const size_t kHeight = 480;
  const size_t kFps = 30;
  std::shared_ptr<VcmCapturer> capturer;
  std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(
      webrtc::VideoCaptureFactory::CreateDeviceInfo());
  if (!info) {
    return nullptr;
  }
  int num_devices = info->NumberOfDevices();
  for (int i = 0; i < num_devices; ++i) {
    capturer = VcmCapturer::Create(worker_thread, kWidth, kHeight, kFps, i);
    if (capturer) {
      return webrtc::scoped_refptr<CapturerTrackSource>(
          new webrtc::RefCountedObject<CapturerTrackSource>(capturer));
    }
  }

  return nullptr;
}

}  // namespace internal
}  // namespace webrtc
