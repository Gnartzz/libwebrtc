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

#include "rtc_desktop_media_list_impl.h"

#include "internal/jpeg_util.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv.h"

#ifdef WEBRTC_WIN
#include "modules/desktop_capture/win/window_capture_utils.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
namespace {
// Picker-Diagnose (2026-07-04): Vorschaubilder werden ab dem 2. Öffnen schwarz,
// nachdem einmal ein echter Screen-/Game-Share lief. Loggt SelectSource-Erfolg
// + Capture-Ergebnis-Code nach %LOCALAPPDATA%\HoneyCord\picker.log, um zu sehen
// WO es bricht (Source verloren / Capture-Fehler / Frame schwarz).
void PickLog(const char* fmt, ...) {
  char path[MAX_PATH];
  DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return;
  std::strncat(path, "\\HoneyCord", MAX_PATH - n - 1);
  CreateDirectoryA(path, nullptr);
  std::strncat(path, "\\picker.log", MAX_PATH - std::strlen(path) - 1);
  // grobe Rotation: > 512 KB -> neu anfangen (Diagnose, kurzlebig).
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) {
    unsigned long long sz =
        (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    if (sz > 512ULL * 1024ULL) DeleteFileA(path);
  }
  if (FILE* f = std::fopen(path, "a")) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::fprintf(f, "[pick %02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fclose(f);
  }
}
}  // namespace
#else
namespace {
void PickLog(const char*, ...) {}
}  // namespace
#endif

#include <fstream>
#include <iostream>

namespace libwebrtc {

RTCDesktopMediaListImpl::RTCDesktopMediaListImpl(DesktopType type,
                                                 webrtc::Thread* signaling_thread)
    : thread_(webrtc::Thread::Create()),
      type_(type),
      signaling_thread_(signaling_thread) {
  RTC_DCHECK(thread_);
  thread_->Start();
  options_ = webrtc::DesktopCaptureOptions::CreateDefault();
  options_.set_detect_updated_region(true);
#ifdef WEBRTC_WIN
  // honeycord: BEWUSST GDI (kein DirectX) fuer die Quellen-Vorschaubilder.
  // webrtc erlaubt nur EINE IDXGIOutputDuplication pro Monitor pro Prozess
  // (siehe dxgi_duplicator_controller.h). Wuerde der Thumbnail-Picker den
  // DXGI-Duplicator initialisieren, haelt webrtcs Singleton den Monitor-Output
  // -> unser Zero-Copy-GPU-Pfad (RTCDesktopCapturerImpl::InitGpu) kann denselben
  // Output dann nicht mehr duplizieren (DuplicateOutput -> E_INVALIDARG). Fuer
  // 320x180-Standbilder reicht GDI vollkommen; so bleibt die eine erlaubte
  // Duplication fuer den eigentlichen Bildschirm-Stream frei.
  options_.set_allow_directx_capturer(false);
#endif
#ifdef WEBRTC_LINUX
  // honeycord: PipeWire fuer BEIDE Typen. Unter Wayland ist die X11-Aufzaehlung
  // nicht erlaubt; ohne diese Zeile liefert CreateWindowCapturer dort nullptr und
  // der ungepruefte Start() unten stuerzt die App ab (GEMESSEN auf Fedora 42:
  // SIGSEGV beim Bildschirm-Teilen). Die Auswahl trifft dann der Portal-Dialog.
  options_.set_allow_pipewire(true);
#endif
  callback_ = std::make_unique<CallbackProxy>();
  thread_->BlockingCall([this, type] {
    if (type == kScreen) {
      capturer_ = webrtc::DesktopCapturer::CreateScreenCapturer(options_);
    } else {
      capturer_ = webrtc::DesktopCapturer::CreateWindowCapturer(options_);
    }
    // honeycord: Create*Capturer DARF nullptr liefern (kein Aufnehmer verfuegbar,
    // z.B. Wayland ohne Portal oder Kopfloser Betrieb). Vorher lief das ungeprueft
    // in einen Nullzugriff -> SIGSEGV, den kein try/catch der Anwendung faengt.
    if (!capturer_) {
      RTC_LOG(LS_ERROR) << "RTCDesktopMediaList: kein Aufnehmer verfuegbar (Typ "
                        << (type == kScreen ? "Bildschirm" : "Fenster")
                        << ") - Liste bleibt leer";
      return;
    }
    capturer_->Start(callback_.get());
  });
}

RTCDesktopMediaListImpl::~RTCDesktopMediaListImpl() { thread_->Stop(); }

int32_t RTCDesktopMediaListImpl::UpdateSourceList(bool force_reload,
                                                  bool get_thumbnail) {
  if (force_reload) {
    for (auto source : sources_) {
      if (observer_) {
        auto source_ptr = source.get();
        signaling_thread_->BlockingCall(
            [&, source_ptr]() { observer_->OnMediaSourceRemoved(source_ptr); });
      }
    }
    sources_.clear();
  }

  webrtc::DesktopCapturer::SourceList new_sources;
  thread_->BlockingCall(
      [this, &new_sources] {
        if (capturer_) capturer_->GetSourceList(&new_sources);
      });

  typedef std::set<webrtc::DesktopCapturer::SourceId> SourceSet;
  SourceSet new_source_set;
  for (size_t i = 0; i < new_sources.size(); ++i) {
    if (type_ == kScreen && new_sources[i].title.length() == 0) {
      new_sources[i].title = std::string("Screen " + std::to_string(i + 1));
    }
    new_source_set.insert(new_sources[i].id);
  }
  // Iterate through the old sources to find the removed sources.
  for (size_t i = 0; i < sources_.size(); ++i) {
    if (new_source_set.find(sources_[i]->source_id()) == new_source_set.end()) {
      if (observer_) {
        auto source = (*(sources_.begin() + i)).get();
        signaling_thread_->BlockingCall(
            [&, source]() { observer_->OnMediaSourceRemoved(source); });
      }
      sources_.erase(sources_.begin() + i);
      --i;
    }
  }
  // Iterate through the new sources to find the added sources.
  if (new_sources.size() > sources_.size()) {
    SourceSet old_source_set;
    for (size_t i = 0; i < sources_.size(); ++i) {
      old_source_set.insert(sources_[i]->source_id());
    }
    for (size_t i = 0; i < new_sources.size(); ++i) {
      if (old_source_set.find(new_sources[i].id) == old_source_set.end()) {
        auto source =
            new RefCountedObject<MediaSourceImpl>(this, new_sources[i], type_);
        sources_.insert(sources_.begin() + i, source);
        GetThumbnail(source, true);
        if (observer_) {
          signaling_thread_->BlockingCall(
              [&, source]() { observer_->OnMediaSourceAdded(source); });
        }
      }
    }
  }

  RTC_DCHECK_EQ(new_sources.size(), sources_.size());

  // Find the moved/changed sources.
  size_t pos = 0;
  while (pos < sources_.size()) {
    if (!(sources_[pos]->source_id() == new_sources[pos].id)) {
      // Find the source that should be moved to |pos|, starting from |pos + 1|
      // of |sources_|, because entries before |pos| should have been sorted.
      size_t old_pos = pos + 1;
      for (; old_pos < sources_.size(); ++old_pos) {
        if (sources_[old_pos]->source_id() == new_sources[pos].id) break;
      }
      RTC_DCHECK(sources_[old_pos]->source_id() == new_sources[pos].id);

      // Move the source from |old_pos| to |pos|.
      auto temp = sources_[old_pos];
      sources_.erase(sources_.begin() + old_pos);
      sources_.insert(sources_.begin() + pos, temp);
      // if(observer_) observer_->OnMediaSourceMoved:old_pos newIndex:pos];
    }

    if (sources_[pos]->source.title != new_sources[pos].title) {
      sources_[pos]->source.title = new_sources[pos].title;
      if (observer_) {
        auto source = sources_[pos].get();
        signaling_thread_->BlockingCall(
            [&, source]() { observer_->OnMediaSourceNameChanged(source); });
      }
    }
    ++pos;
  }

  if (get_thumbnail) {
    for (auto source : sources_) {
      GetThumbnail(source.get(), true);
    }
  }
  return sources_.size();
}

bool RTCDesktopMediaListImpl::GetThumbnail(scoped_refptr<MediaSource> source,
                                           bool notify) {
  thread_->PostTask([this, source, notify] {
    MediaSourceImpl* source_impl = static_cast<MediaSourceImpl*>(source.get());
    if (!capturer_) return false;
    const bool sel = capturer_->SelectSource(source_impl->source_id());
    if (sel) {
      callback_->SetCallback([&](webrtc::DesktopCapturer::Result result,
                                 std::unique_ptr<webrtc::DesktopFrame> frame) {
        // Schwarz-Check: mittlere Helligkeit ueber ein grobes Raster. Sagt uns,
        // ob der Capture erfolgreich ist ABER schwarze Pixel liefert (GDI ueber
        // RDP / nach DXGI-Share) — dann liegt der Bug am Frame, nicht am UI.
        long long avg = -1;
        int fw = 0, fh = 0;
        if (frame && frame->data()) {
          fw = frame->size().width();
          fh = frame->size().height();
          const int stride = frame->stride();  // Bytes/Zeile (BGRA)
          unsigned long long sum = 0;
          int n = 0;
          for (int y = 0; y < fh; y += (fh / 16 > 0 ? fh / 16 : 1)) {
            const uint8_t* row = frame->data() + (size_t)y * stride;
            for (int x = 0; x < fw; x += (fw / 16 > 0 ? fw / 16 : 1)) {
              const uint8_t* px = row + (size_t)x * 4;  // BGRA
              sum += px[0] + px[1] + px[2];
              ++n;
            }
          }
          if (n > 0) avg = (long long)(sum / (3ULL * n));  // 0..255
        }
        PickLog("thumb id=%lld SelectSource=1 Capture=%d frame=%s %dx%d avgLum=%lld%s",
                (long long)source_impl->source_id(), (int)result,
                frame ? "ja" : "null", fw, fh, avg,
                (avg >= 0 && avg < 4) ? " [SCHWARZ]" : "");
        auto old_thumbnail = source_impl->thumbnail();
        source_impl->SaveCaptureResult(result, std::move(frame));
        if (observer_ && notify) {
          signaling_thread_->BlockingCall([&, source_impl]() {
            observer_->OnMediaSourceThumbnailChanged(source_impl);
          });
        }
      });
      if (capturer_) capturer_->CaptureFrame();
    } else {
      PickLog("thumb id=%lld SelectSource=0 (Quelle verloren)",
              (long long)source_impl->source_id());
    }
  });
  return true;
}

int RTCDesktopMediaListImpl::GetSourceCount() const { return sources_.size(); }

scoped_refptr<MediaSource> RTCDesktopMediaListImpl::GetSource(int index) {
  return sources_[index];
}

bool MediaSourceImpl::UpdateThumbnail() {
  return mediaList_->GetThumbnail(this);
}

#ifdef WEBRTC_WIN
extern int filterException(int code, PEXCEPTION_POINTERS ex);
#endif

void MediaSourceImpl::SaveCaptureResult(
    webrtc::DesktopCapturer::Result result,
    std::unique_ptr<webrtc::DesktopFrame> frame) {
  if (result != webrtc::DesktopCapturer::Result::SUCCESS) {
    return;
  }

  int width = frame->size().width();
  int height = frame->size().height();
#ifdef WEBRTC_WIN
  webrtc::DesktopRect rect_ = webrtc::DesktopRect::MakeWH(width, height);

  if (type_ != kScreen) {
    webrtc::GetWindowRect(reinterpret_cast<HWND>(source_id()), &rect_);
  }

  __try
#endif
  {

    if (!i420_buffer_ || !i420_buffer_.get() ||
        i420_buffer_->width() * i420_buffer_->height() != width * height) {
      i420_buffer_ = webrtc::I420Buffer::Create(width, height);
    }

    libyuv::ConvertToI420(frame->data(), 0, i420_buffer_->MutableDataY(),
                          i420_buffer_->StrideY(), i420_buffer_->MutableDataU(),
                          i420_buffer_->StrideU(), i420_buffer_->MutableDataV(),
                          i420_buffer_->StrideV(), 0, 0,
#ifdef WEBRTC_WIN
                          rect_.width(), rect_.height(),
#else
                          width, height,
#endif
                          width, height, libyuv::kRotate0, libyuv::FOURCC_ARGB);

    webrtc::VideoFrame input_frame(i420_buffer_, 0, 0,
                                   webrtc::kVideoRotation_0);

    const int kColorPlanes = 3;  // R, G and B.
    size_t rgb_len = input_frame.height() * input_frame.width() * kColorPlanes;
    std::unique_ptr<uint8_t[]> rgb_buf(new uint8_t[rgb_len]);

    // kRGB24 actually corresponds to FourCC 24BG which is 24-bit BGR.
    if (ConvertFromI420(input_frame, webrtc::VideoType::kRGB24, 0,
                        rgb_buf.get()) < 0) {
      RTC_LOG(LS_ERROR) << "Could not convert input frame to RGB.";
      return;
    }

    // Create a thumbnail image from the captured frame.
    thumbnail_ = EncodeRGBToJpeg((const unsigned char*)rgb_buf.get(),
                                 input_frame.width(), input_frame.height(),
                                 kColorPlanes, 75);
  }
#ifdef WEBRTC_WIN
  __except (filterException(GetExceptionCode(), GetExceptionInformation())) {
  }
#endif
}

}  // namespace libwebrtc
