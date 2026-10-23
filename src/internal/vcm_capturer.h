/*
 *  Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#ifndef INTERNAL_VCM_CAPTURER_H_
#define INTERNAL_VCM_CAPTURER_H_

#include <stdint.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "api/scoped_refptr.h"
#include "modules/video_capture/video_capture.h"
#include "rtc_base/thread.h"
#include "src/internal/video_capturer.h"

namespace webrtc {
namespace internal {

class VcmCapturer : public VideoCapturer,
                    public webrtc::VideoSinkInterface<VideoFrame> {
 public:
  static std::shared_ptr<VcmCapturer> Create(webrtc::Thread* worker_thread,
                                             size_t width, size_t height,
                                             size_t target_fps,
                                             size_t capture_device_index);
  VcmCapturer(webrtc::Thread* worker_thread);

  virtual ~VcmCapturer();

  bool StartCapture() override;

  bool CaptureStarted() override;

  void StopCapture() override;

  void OnFrame(const VideoFrame& frame) override;

 protected:
  // Takt-Messpunkt „aus" (nach dem VideoAdapter), siehe VideoCapturer.
  void NachAdapterBild() override;

 private:
  bool Init(size_t width, size_t height, size_t target_fps,
            size_t capture_device_index);
  void Destroy();
  // Raten-Pruefung nach gesenkter Rate (nur Windows): plant auf dem
  // worker_thread_ eine Messung ein; liefert die Kamera zu wenig, einmal
  // Rueckfall auf die unveraenderte Faehigkeit. Nur auf worker_thread_.
  void PlaneRatenPruefung(std::shared_ptr<std::atomic<bool>> lebt, int runde,
                          uint32_t n_vorher, int64_t qpc_vorher, int unter);
  void RateRueckfall(const char* grund);
  // Takt-Ringfeld auswerten und (ueber den worker_thread_) ins cam.log geben.
  void TaktZusammenfassen(int64_t jetzt_qpc);

  webrtc::scoped_refptr<VideoCaptureModule> vcm_;
  webrtc::Thread* worker_thread_ = nullptr;
  VideoCaptureCapability capability_;
  // DirectShow-DevicePath des gewaehlten Geraets (Init) — id34: erlaubt es,
  // die Kamera in StartCapture separat via Moniker zu oeffnen und die
  // Belichtung zu deckeln (Framerate-Halten bei wenig Licht).
  std::string unique_name_;
  // Kamera-Telemetrie (cam.log, nur Windows genutzt): geliefert-fps 1x/min.
  int64_t cam_dbg_start_ms_ = 0;
  unsigned cam_dbg_frames_ = 0;
  // Dauer des DirectShow-Capability-Map-Baus (Init) — fuer #5 „Open langsam".
  int64_t cap_map_ms_ = 0;

  // --- Bildrate senken (2026-10-23, jimtonics-Mikroruckler) ---------------
  // DirectShow meldet je Medientyp nur die HOECHSTE Rate (z. B. 720p MJPEG
  // „@90"). Ohne Senkung stellt webrtc die Kamera auf 90 und der
  // FramerateController wirft jedes dritte Bild weg (11/22-ms-Takt).
  int32_t wunsch_fps_ = 0;
  // Unveraenderte Faehigkeit (Rate wie von der Kamera gemeldet) fuer den
  // einmaligen Rueckfall.
  VideoCaptureCapability rueckfall_cap_;
  // true, solange capability_.maxFPS gegenueber rueckfall_cap_ gesenkt ist
  // und der Rueckfall noch nicht gelaufen ist. Nur worker_thread_ / Init.
  bool rate_gesenkt_ = false;
  // Lebt-Merker fuer verzoegerte Aufgaben auf worker_thread_: wird in
  // StopCapture (auf worker_thread_) auf false gesetzt; verzoegerte Aufgaben
  // laufen auf demselben Faden und pruefen ihn vor jedem Zugriff auf this.
  std::shared_ptr<std::atomic<bool>> pruef_lebt_;
  // Vom Aufnahme-Faden geschrieben, vom worker_thread_ gelesen.
  std::atomic<uint32_t> bilder_gesamt_{0};
  std::atomic<int64_t> erstes_bild_qpc_{0};
  // N1: Kamera-Rate fuer die Takt-Zeile (Streaming-Faden liest; Init,
  // StartCapture und RateRueckfall schreiben) — statt capability_.maxFPS.
  std::atomic<int> kamera_fps_log_{0};

  // --- Takt-Messung (nur Aufnahme-Faden) ----------------------------------
  // Annahme Einzelfaden: OnFrame und NachAdapterBild laufen beide auf dem
  // DirectShow-Streaming-Faden (ein Ausgangs-Pin = ein Faden; NachAdapterBild
  // wird synchron aus VideoCapturer::OnFrame gerufen). Ein Neustart (Rueckfall)
  // stoppt den alten Faden, bevor der neue liefert. Daher keine Atomics/Sperren.
  static constexpr int kTaktRing = 600;
  int64_t qpc_freq_ = 0;
  int64_t takt_fenster_qpc_ = 0;
  int64_t ein_letzt_qpc_ = 0;
  int64_t aus_letzt_qpc_ = 0;
  int ein_n_ = 0;
  int aus_n_ = 0;
  int ein_pos_ = 0;
  int aus_pos_ = 0;
  int32_t ein_us_[kTaktRing] = {};
  int32_t aus_us_[kTaktRing] = {};
  int32_t takt_sortier_[kTaktRing] = {};
};

class CapturerTrackSource : public webrtc::VideoTrackSource {
 public:
  static webrtc::scoped_refptr<CapturerTrackSource> Create(
      webrtc::Thread* worker_thread);

 public:
  explicit CapturerTrackSource(std::shared_ptr<VideoCapturer> capturer)
      : VideoTrackSource(/*remote=*/false), capturer_(capturer) {}

 private:
  webrtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
    return capturer_.get();
  }
  std::shared_ptr<VideoCapturer> capturer_;
};
}  // namespace internal
}  // namespace webrtc

#endif  // INTERNAL_VCM_CAPTURER_H_
