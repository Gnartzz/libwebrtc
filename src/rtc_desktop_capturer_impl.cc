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

#include <algorithm>

#include "api/sequence_checker.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv.h"
#ifdef WEBRTC_WIN
#include "modules/desktop_capture/win/window_capture_utils.h"
#endif
#if defined(WEBRTC_LINUX) && defined(USE_LINUX_HWENC)
#include "src/linux/hwenc/ffmpeg/hw_nullkopie.h"
#include "src/linux/hwenc/hw_gemeinsam.h"
#endif
#if defined(WEBRTC_LINUX)
#include <cstdlib>
#endif

namespace libwebrtc {

enum { kCaptureDelay = 33, kCaptureMessageId = 1000 };

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
  // honeycord: PipeWire fuer BEIDE Typen. Unter Wayland ist die X11-Aufzaehlung
  // nicht erlaubt; fuer Fenster kam deshalb nullptr zurueck -> Absturz weiter
  // unten. Die Auswahl trifft dort ohnehin der Portal-Dialog des Systems.
  (void)type;
  options_.set_allow_pipewire(true);
#if defined(USE_LINUX_HWENC)
  // honeycord (Null-Kopie, 08.11.2026): Bleiben die Bilder auf der
  // Grafikkarte, malt niemand mehr den Zeiger ins ausgelesene Bild
  // (DesktopAndCursorComposer arbeitet auf den Pixeln). Also soll ihn der
  // Compositor selbst in den Strom legen. Das gilt dann auch fuer den alten
  // Weg, falls ein Bild doch ueber die CPU geht.
  if (libwebrtc::nullkopie::Einrichten()) {
    if (showCursor) options_.set_prefer_cursor_embedded(true);
    // ★ Prüfbefund M1: Der Differ-Wrapper vergliche je Bild zwei Pixelpuffer,
    // die bei GPU-Bildern gar nicht gefuellt sind (2x ~20 MB Lesen fuer
    // nichts). Unter Wayland meldet PipeWire den Schaden ohnehin selbst.
    options_.set_detect_updated_region(false);
    nullkopie_ = true;
  }
#endif
#endif
  thread_->BlockingCall([this, type, showCursor] {
    if (type == kScreen) {
      std::unique_ptr<webrtc::DesktopCapturer> roh;
#if defined(WEBRTC_LINUX) && defined(WEBRTC_USE_PIPEWIRE)
      // honeycord (22.11.2026, Fenster unter Wayland): Die Sammelquelle
      // „Bildschirm“ der Medienliste fragt das Portal nach Bildschirm ODER
      // Fenster (CaptureType::kAnyScreenContent, wie Chromium/Firefox fuer
      // getDisplayMedia). Vorher kam nur CreateScreenCapturer = kScreen, der
      // Dialog bot nur Bildschirme an. Ein Fenster ueber kWindow ginge nicht:
      // Start() ruft fuer kWindow FocusOnSelectedSource(), das
      // BaseCapturerPipeWire nicht kennt (Vorgabe DesktopCapturer: false) —
      // die Aufnahme scheiterte sofort mit CS_FAILED. Welche Typen der Desktop
      // wirklich anbietet, prueft screencast_portal.cc (AvailableSourceTypes,
      // patches/honeycord_portal_fenster.patch). Rueckweg fuer die Diagnose:
      // HONEYCORD_PORTAL_NUR_BILDSCHIRM=1.
      const char* nur = getenv("HONEYCORD_PORTAL_NUR_BILDSCHIRM");
      if (!(nur && nur[0] == '1')) {
        roh = webrtc::DesktopCapturer::CreateGenericCapturer(options_);
      }
#if defined(USE_LINUX_HWENC)
      if (webrtc::DesktopCapturer::IsRunningUnderWayland()) {
        libwebrtc::Protokoll(
            "Freigabe: Portal fragt nach %s",
            roh ? "Bildschirm oder Fenster"
                : (nur && nur[0] == '1'
                       ? "Bildschirm (HONEYCORD_PORTAL_NUR_BILDSCHIRM=1)"
                       : "Bildschirm"));
      }
#endif
#endif
      if (!roh) roh = webrtc::DesktopCapturer::CreateScreenCapturer(options_);
      if (showCursor) {
        capturer_ = std::make_unique<webrtc::DesktopAndCursorComposer>(
            std::move(roh), options_);
      } else {
        capturer_ = webrtc::DesktopAndCursorComposer::CreateWithoutMouseCursorMonitor(
                std::move(roh));
      }
    } else {
      capturer_ = std::make_unique<webrtc::DesktopAndCursorComposer>(
          webrtc::DesktopCapturer::CreateWindowCapturer(options_), options_);
    }
  });
}

RTCDesktopCapturerImpl::~RTCDesktopCapturerImpl() {
  thread_->Stop();
  capturer_.reset();
#if defined(WEBRTC_LINUX) && defined(USE_LINUX_HWENC)
  letztes_aus_ = nullptr;
  letztes_nativ_ = nullptr;
  if (aufnahme_gezaehlt_) {
    libwebrtc::nullkopie::AufnahmeEndet();
  } else if (nullkopie_) {
    // Nach Stop() kann der Strom bis eben noch Bilder geliefert haben.
    libwebrtc::nullkopie::AufraeumenWennRuhig();
  }
#endif
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

  // Poll the desktop capturer at roughly 2× the requested rate. The
  // underlying capturer returns ERROR_TEMPORARY when no new frame is ready,
  // so over-polling is cheap but ensures we never miss a vsync edge — the
  // previous "delay == interval" scheduling combined with the capturer's
  // own ~16ms vsync wait effectively halved the framerate (60→30).
  uint32_t interval_ms = uint32_t(1000.0 / fps);
  capture_delay_ = interval_ms > 4 ? interval_ms / 2 : 1;

  if (source_id_ != -1) {
    if (!capturer_ || !capturer_->SelectSource(source_id_)) {
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

  // honeycord: ohne Aufnehmer sauber scheitern statt Nullzugriff.
  if (!capturer_) {
    RTC_LOG(LS_ERROR) << "RTCDesktopCapturer: kein Aufnehmer verfuegbar";
    capture_state_ = CS_FAILED;
    return capture_state_;
  }
#if defined(WEBRTC_LINUX) && defined(USE_LINUX_HWENC)
  // Vor dem Start zaehlen: Der Wandler erzeugt nur bei laufender Aufnahme.
  if (nullkopie_ && !aufnahme_gezaehlt_) {
    aufnahme_gezaehlt_ = true;
    libwebrtc::nullkopie::AufnahmeBeginnt();
  }
#endif
  thread_->BlockingCall([this] { capturer_->Start(this); });
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
#if defined(WEBRTC_LINUX) && defined(USE_LINUX_HWENC)
  // Pruefbefund M5: die letzte Aufnahme gibt die Bildvorraete frei.
  thread_->BlockingCall([this] {
    letztes_nativ_ = nullptr;
    letztes_aus_ = nullptr;
  });
  if (aufnahme_gezaehlt_) {
    aufnahme_gezaehlt_ = false;
    libwebrtc::nullkopie::AufnahmeEndet();
  }
#endif
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

#if defined(WEBRTC_LINUX) && defined(USE_LINUX_HWENC)
  // honeycord (Null-Kopie): Das Bild liegt schon als NV12 auf der Grafikkarte.
  // Zuschnitt und Deckel laufen dort (VPP), die Pixel in frame->data() sind
  // NICHT gefuellt und werden nicht angefasst.
  if (frame->honeycord_nativ()) {
    if (frame->honeycord_nativ() == letztes_nativ_ && letztes_aus_) {
      // honeycord (22.11.2026): Kein neues PipeWire-Bild seit dem letzten
      // Abholen. Abgefragt wird mit doppelter Bildrate (s. Start), vorher ging
      // jedes Mal derselbe Puffer erneut hinaus — gemessen am Encoder 120
      // statt 60 Bilder je Sekunde, wenn kein Bildraten-Deckel griff, und mit
      // Deckel entschied der Zufall, ob das neue oder das wiederholte Bild
      // durchkam. Jetzt nur noch als Lebenszeichen bei ruhigem Bild (fuer
      // Schluesselbilder neuer Zuschauer), hoechstens alle 200 ms.
      const int64_t jetzt = webrtc::TimeMillis();
      if (jetzt - letztes_gesendet_ms_ < 200) return;
      letztes_gesendet_ms_ = jetzt;
      ++wiederholt_;
      OnFrame(webrtc::VideoFrame(letztes_aus_, 0, jetzt,
                                 webrtc::kVideoRotation_0));
      return;
    }
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> gpu(
        static_cast<webrtc::VideoFrameBuffer*>(frame->honeycord_nativ().get()));
    // Region ins Bild begrenzen (Pruefbefund N1), gerade Masse fuer NV12.
    const int bx = std::min(static_cast<int>(x_), gpu->width() - 2) & ~1;
    const int by = std::min(static_cast<int>(y_), gpu->height() - 2) & ~1;
    const int bw = std::min(static_cast<int>(w_), gpu->width() - bx) & ~1;
    const int bh = std::min(static_cast<int>(h_), gpu->height() - by) & ~1;
    if (w_ > 0 && h_ > 0 && bw >= 2 && bh >= 2 &&
        (bw != gpu->width() || bh != gpu->height())) {
      gpu = gpu->CropAndScale(bx, by, bw, bh, bw, bh);
      if (!gpu) return;  // Pruefbefund S2: verwerfen statt abstuerzen
    }
    if (max_width_ > 0 && max_height_ > 0 &&
        (gpu->width() > static_cast<int>(max_width_) ||
         gpu->height() > static_cast<int>(max_height_))) {
      double sx = static_cast<double>(max_width_) / gpu->width();
      double sy = static_cast<double>(max_height_) / gpu->height();
      double scale = sx < sy ? sx : sy;
      int dst_w = static_cast<int>(gpu->width() * scale) & ~1;
      int dst_h = static_cast<int>(gpu->height() * scale) & ~1;
      gpu = gpu->Scale(dst_w, dst_h);
      if (!gpu) return;
    }
    letztes_nativ_ = frame->honeycord_nativ();
    letztes_aus_ = gpu;
    letztes_gesendet_ms_ = webrtc::TimeMillis();
    ++neu_;
    if (letztes_gesendet_ms_ - zaehler_seit_ms_ >= 30000) {
      if (zaehler_seit_ms_ > 0)
        libwebrtc::Protokoll(
            "Aufnehmer (30 s): %d neue Bilder, %d Lebenszeichen weitergegeben",
            neu_, wiederholt_);
      zaehler_seit_ms_ = letztes_gesendet_ms_;
      neu_ = 0;
      wiederholt_ = 0;
    }
    OnFrame(webrtc::VideoFrame(gpu, 0, letztes_gesendet_ms_,
                               webrtc::kVideoRotation_0));
    return;
  }
#endif

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

    OnFrame(webrtc::VideoFrame(out_buffer, 0, webrtc::TimeMillis(),
                               webrtc::kVideoRotation_0));
  }
#ifdef WEBRTC_WIN
  __except (filterException(GetExceptionCode(), GetExceptionInformation())) {
  }
#endif
}

void RTCDesktopCapturerImpl::CaptureFrame() {
  RTC_DCHECK_RUN_ON(thread_.get());
  if (capture_state_ == CS_RUNNING) {
    if (!capturer_) return;
    capturer_->CaptureFrame();
    thread_->PostDelayedHighPrecisionTask(
        [this]() { CaptureFrame(); },
        webrtc::TimeDelta::Millis(capture_delay_));
  }
}

}  // namespace libwebrtc
