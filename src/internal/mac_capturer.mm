/*
 * Umsetzung von mac_capturer.h — siehe dort, warum es die Datei gibt.
 * Vorlage: test/mac_capturer.mm aus WebRTC. Diese Datei wird MIT ARC uebersetzt
 * (BUILD.gn: enable_arc), deshalb die __bridge-Ueberfuehrungen.
 */
#include "src/internal/mac_capturer.h"

#import "sdk/objc/base/RTCVideoCapturer.h"
#import "sdk/objc/base/RTCVideoFrame.h"
#import "sdk/objc/components/capturer/RTCCameraVideoCapturer.h"
#import "sdk/objc/native/src/objc_frame_buffer.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "api/video/video_frame.h"
#include "api/video/video_rotation.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"

// ── Der Delegierte: nimmt ObjC-Bilder entgegen und reicht sie nach C++ ───────
@interface HCCameraAdapter : NSObject <RTC_OBJC_TYPE (RTCVideoCapturerDelegate)>
@property(nonatomic, assign) webrtc::internal::MacCapturer* capturer;
@end

@implementation HCCameraAdapter
@synthesize capturer = _capturer;

- (void)capturer:(RTC_OBJC_TYPE(RTCVideoCapturer) *)capturer
    didCaptureVideoFrame:(RTC_OBJC_TYPE(RTCVideoFrame) *)frame {
  // ★ Der Zeiger kann null sein, wenn der Aufnehmer schon abgeraeumt wurde und
  // AVFoundation noch ein Bild in der Warteschlange hatte. Ohne diese Pruefung
  // waere das ein Use-after-free auf dem Aufnahme-Faden — genau die Sorte
  // Fehler, die erst beim Auflegen zuschlaegt.
  if (!_capturer) return;
  static int erste = 0;
  if (erste++ == 0) RTC_LOG(LS_INFO) << "MacCapturer: erstes Bild vom Delegierten";
  const int64_t timestamp_us = frame.timeStampNs / webrtc::kNumNanosecsPerMicrosec;
  webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer =
      webrtc::make_ref_counted<webrtc::ObjCFrameBuffer>(frame.buffer);
  _capturer->OnCapturedFrame(webrtc::VideoFrame::Builder()
                                 .set_video_frame_buffer(buffer)
                                 .set_rotation(webrtc::kVideoRotation_0)
                                 .set_timestamp_us(timestamp_us)
                                 .build());
}
@end

namespace {

/// Das Format mit der kleinsten Abweichung zur Wunschgroesse. Bei gleichem
/// Abstand gewinnt das Format, das die gewuenschte Bildrate noch schafft —
/// sonst bekaeme man 1080p30 statt 720p60, obwohl 60 bestellt war.
AVCaptureDeviceFormat* PassendesFormat(AVCaptureDevice* device, size_t width,
                                       size_t height, size_t fps) {
  NSArray<AVCaptureDeviceFormat*>* formats =
      [RTC_OBJC_TYPE(RTCCameraVideoCapturer) supportedFormatsForDevice:device];
  AVCaptureDeviceFormat* gewaehlt = nil;
  int64_t bester = INT64_MAX;
  bool bester_schafft_fps = false;
  for (AVCaptureDeviceFormat* format in formats) {
    CMVideoDimensions dim =
        CMVideoFormatDescriptionGetDimensions(format.formatDescription);
    const int64_t diff = std::abs((int64_t)width - dim.width) +
                         std::abs((int64_t)height - dim.height);
    bool schafft_fps = false;
    for (AVFrameRateRange* r in format.videoSupportedFrameRateRanges) {
      if (r.maxFrameRate + 0.5 >= (double)fps) { schafft_fps = true; break; }
    }
    if (diff < bester || (diff == bester && schafft_fps && !bester_schafft_fps)) {
      gewaehlt = format;
      bester = diff;
      bester_schafft_fps = schafft_fps;
    }
  }
  return gewaehlt;
}

void KopiereText(NSString* text, char* ziel, uint32_t laenge) {
  if (!ziel || laenge == 0) return;
  // ★ `strncpy` fuellt nicht zwingend mit 0 ab — deshalb von Hand abschliessen.
  // Ein Kameraname mit Emoji (es gibt sie) darf hier nichts ueberschreiben.
  const char* quelle = text.UTF8String ?: "";
  std::snprintf(ziel, laenge, "%s", quelle);
}

}  // namespace

namespace webrtc {
namespace internal {

MacCapturer::MacCapturer(size_t width, size_t height, size_t target_fps,
                         size_t capture_device_index)
    : width_(width), height_(height), target_fps_(target_fps) {
  NSArray<AVCaptureDevice*>* geraete =
      [RTC_OBJC_TYPE(RTCCameraVideoCapturer) captureDevices];
  if (capture_device_index >= geraete.count) {
    RTC_LOG(LS_ERROR) << "MacCapturer: Kamera " << capture_device_index
                      << " gibt es nicht (" << geraete.count << " vorhanden)";
    return;
  }
  AVCaptureDevice* device = geraete[capture_device_index];
  AVCaptureDeviceFormat* format = PassendesFormat(device, width, height, target_fps);
  if (!format) {
    RTC_LOG(LS_ERROR) << "MacCapturer: kein passendes Format";
    return;
  }

  HCCameraAdapter* adapter = [[HCCameraAdapter alloc] init];
  adapter.capturer = this;
  RTC_OBJC_TYPE(RTCCameraVideoCapturer)* capturer =
      [[RTC_OBJC_TYPE(RTCCameraVideoCapturer) alloc] initWithDelegate:adapter];

  adapter_ = (__bridge_retained void*)adapter;
  capturer_ = (__bridge_retained void*)capturer;
  device_ = (__bridge_retained void*)device;
  format_ = (__bridge_retained void*)format;
}

std::shared_ptr<MacCapturer> MacCapturer::Create(size_t width, size_t height,
                                                 size_t target_fps,
                                                 size_t capture_device_index) {
  auto c = std::shared_ptr<MacCapturer>(
      new MacCapturer(width, height, target_fps, capture_device_index));
  return c->Gueltig() ? c : nullptr;
}

bool MacCapturer::Gueltig() const {
  return capturer_ && adapter_ && device_ && format_;
}

bool MacCapturer::StartCapture() {
  if (!Gueltig()) return false;
  if (laeuft_) return true;
  RTC_OBJC_TYPE(RTCCameraVideoCapturer)* capturer =
      (__bridge RTC_OBJC_TYPE(RTCCameraVideoCapturer)*)capturer_;
  AVCaptureDevice* device = (__bridge AVCaptureDevice*)device_;
  AVCaptureDeviceFormat* format = (__bridge AVCaptureDeviceFormat*)format_;
  // Berechtigung MESSEN, nicht annehmen: ohne Kamerarecht liefert AVFoundation
  // stillschweigend keine Bilder — man sucht den Fehler dann im eigenen Code.
  const AVAuthorizationStatus stand =
      [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
  RTC_LOG(LS_INFO) << "MacCapturer: Kamera-Berechtigung = " << (int)stand
                   << " (0=unbestimmt 1=eingeschraenkt 2=verweigert 3=erteilt)";
  if (stand == AVAuthorizationStatusNotDetermined) {
    dispatch_semaphore_t warte = dispatch_semaphore_create(0);
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                             completionHandler:^(BOOL erteilt) {
                               RTC_LOG(LS_INFO) << "MacCapturer: Nachfrage beantwortet, erteilt="
                                                << (erteilt ? 1 : 0);
                               dispatch_semaphore_signal(warte);
                             }];
    dispatch_semaphore_wait(warte, dispatch_time(DISPATCH_TIME_NOW, (int64_t)30 * NSEC_PER_SEC));
  }
  [capturer startCaptureWithDevice:device
                            format:format
                               fps:(NSInteger)target_fps_
                 completionHandler:^(NSError* fehler) {
                   if (fehler) {
                     RTC_LOG(LS_ERROR) << "MacCapturer: startCapture meldet Fehler: "
                                       << fehler.localizedDescription.UTF8String;
                   } else {
                     RTC_LOG(LS_INFO) << "MacCapturer: Aufnahme gestartet";
                   }
                 }];
  laeuft_ = true;
  CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
  RTC_LOG(LS_INFO) << "MacCapturer: " << device.localizedName.UTF8String << " "
                   << dim.width << "x" << dim.height << " @" << target_fps_;
  return true;
}

bool MacCapturer::CaptureStarted() { return laeuft_; }

void MacCapturer::StopCapture() {
  if (!laeuft_) return;
  RTC_OBJC_TYPE(RTCCameraVideoCapturer)* capturer =
      (__bridge RTC_OBJC_TYPE(RTCCameraVideoCapturer)*)capturer_;
  [capturer stopCapture];
  laeuft_ = false;
}

void MacCapturer::Destroy() {
  // ★ Reihenfolge: erst die Aufnahme anhalten, DANN den Rueckzeiger loeschen,
  // dann freigeben. Andersherum koennte ein bereits gepuffertes Bild in ein
  // halb abgeraeumtes Objekt laufen.
  StopCapture();
  if (adapter_) {
    HCCameraAdapter* adapter = (__bridge_transfer HCCameraAdapter*)adapter_;
    adapter.capturer = nullptr;
    adapter_ = nullptr;
  }
  if (capturer_) {
    RTC_OBJC_TYPE(RTCCameraVideoCapturer)* c =
        (__bridge_transfer RTC_OBJC_TYPE(RTCCameraVideoCapturer)*)capturer_;
    (void)c;
    capturer_ = nullptr;
  }
  if (device_) { AVCaptureDevice* d = (__bridge_transfer AVCaptureDevice*)device_; (void)d; device_ = nullptr; }
  if (format_) { AVCaptureDeviceFormat* f = (__bridge_transfer AVCaptureDeviceFormat*)format_; (void)f; format_ = nullptr; }
}

MacCapturer::~MacCapturer() { Destroy(); }

void MacCapturer::OnCapturedFrame(const VideoFrame& frame) { OnFrame(frame); }

uint32_t MacCapturer::NumberOfDevices() {
  return (uint32_t)[RTC_OBJC_TYPE(RTCCameraVideoCapturer) captureDevices].count;
}

int32_t MacCapturer::GetDeviceName(uint32_t index, char* name_utf8,
                                   uint32_t name_len, char* unique_id_utf8,
                                   uint32_t unique_id_len) {
  NSArray<AVCaptureDevice*>* geraete =
      [RTC_OBJC_TYPE(RTCCameraVideoCapturer) captureDevices];
  if (index >= geraete.count) return -1;
  AVCaptureDevice* device = geraete[index];
  KopiereText(device.localizedName, name_utf8, name_len);
  KopiereText(device.uniqueID, unique_id_utf8, unique_id_len);
  return 0;
}

}  // namespace internal
}  // namespace webrtc
