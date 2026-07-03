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

  device_info->GetCapability(vcm_->CurrentDeviceName(), 0, capability_);

  capability_.width = static_cast<int32_t>(width);
  capability_.height = static_cast<int32_t>(height);
  capability_.maxFPS = static_cast<int32_t>(target_fps);
  capability_.videoType = VideoType::kI420;

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
  // Verhandeltes Format loggen — RAW (I420/YUY2) bei 720p+ = USB-Bandbreiten-
  // Falle (liefert real weniger fps als nominell); MJPEG/NV12 = ok.
  VideoCaptureCapability settings;
  if (vcm_->CaptureSettings(settings) == 0) {
    CamLog("Start: angefordert %dx%d@%d -> verhandelt %dx%d@%d Format=%s",
           capability_.width, capability_.height, capability_.maxFPS,
           settings.width, settings.height, settings.maxFPS,
           CamVideoTypeName(settings.videoType));
  } else {
    CamLog("Start: angefordert %dx%d@%d (CaptureSettings n/a)",
           capability_.width, capability_.height, capability_.maxFPS);
  }
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
