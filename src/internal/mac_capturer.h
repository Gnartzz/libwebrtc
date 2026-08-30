/*
 * Kameraaufnahme auf macOS (HoneyCord, 30.08.2026).
 *
 * WARUM ES DIESE DATEI GIBT: `VideoCaptureFactory::Create()` in WebRTC gibt auf
 * Apple-Plattformen `nullptr` zurueck —
 *
 *     #if defined(WEBRTC_ANDROID) || defined(WEBRTC_MAC)
 *       return nullptr;
 *
 * (modules/video_capture/video_capture_factory.cc). Kameras erreicht man dort
 * ausschliesslich ueber das ObjC-SDK (`RTCCameraVideoCapturer`, AVFoundation).
 * Ohne diese Bruecke meldet `RTCVideoDevice::NumberOfDevices()` auf dem Mac
 * dauerhaft 0, und zwar OHNE Fehler — man sucht den Fehler dann in der
 * Berechtigung oder im eigenen Code.
 *
 * VORLAGE: `test/mac_capturer.mm` aus WebRTC selbst, also derselbe Rechenweg,
 * den Google testet. Unterschiede zum Original, alle beabsichtigt:
 *   - haengt an `webrtc::internal::VideoCapturer` (die Basis dieses Wrappers)
 *     statt an `TestVideoCapturer`;
 *   - die Aufnahme startet in `StartCapture()`, nicht im Konstruktor — der
 *     Wrapper trennt Erzeugen und Starten (hc_media: `Create()` dann
 *     `StartCapture()`), und ein Konstruktor, der schon die Kamera-Lampe
 *     anschaltet, waere eine Ueberraschung;
 *   - dazu die Geraeteliste, die `RTCVideoDeviceImpl` braucht.
 */
#ifndef INTERNAL_MAC_CAPTURER_H_
#define INTERNAL_MAC_CAPTURER_H_

#include <cstddef>
#include <cstdint>
#include <memory>

#include "src/internal/video_capturer.h"

namespace webrtc {
namespace internal {

class MacCapturer : public VideoCapturer {
 public:
  /// Legt den Aufnehmer an, WAEHLT das Geraet und das passende Format — startet
  /// aber noch nichts. `nullptr`, wenn es die Kamera nicht gibt.
  static std::shared_ptr<MacCapturer> Create(size_t width, size_t height,
                                             size_t target_fps,
                                             size_t capture_device_index);

  ~MacCapturer() override;

  bool StartCapture() override;
  bool CaptureStarted() override;
  void StopCapture() override;

  // ── Geraeteliste (fuer RTCVideoDeviceImpl) ─────────────────────────────────
  static uint32_t NumberOfDevices();
  /// Wie `VideoCaptureModule::DeviceInfo::GetDeviceName`: 0 = ok, -1 = nicht da.
  static int32_t GetDeviceName(uint32_t index, char* name_utf8,
                               uint32_t name_len, char* unique_id_utf8,
                               uint32_t unique_id_len);

  /// Vom ObjC-Delegierten gerufen — leitet an die Basis weiter.
  void OnCapturedFrame(const VideoFrame& frame);

 private:
  MacCapturer(size_t width, size_t height, size_t target_fps,
              size_t capture_device_index);
  bool Gueltig() const;
  void Destroy();

  size_t width_ = 0;
  size_t height_ = 0;
  size_t target_fps_ = 30;
  bool laeuft_ = false;
  // Undurchsichtig, damit dieser Kopf reines C++ bleibt (er wird auch aus
  // .cc-Dateien eingebunden): RTCCameraVideoCapturer, der Delegierte, das
  // AVCaptureDevice und das gewaehlte Format.
  void* capturer_ = nullptr;
  void* adapter_ = nullptr;
  void* device_ = nullptr;
  void* format_ = nullptr;
};

}  // namespace internal
}  // namespace webrtc

#endif  // INTERNAL_MAC_CAPTURER_H_
