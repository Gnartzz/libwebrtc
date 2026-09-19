#include "src/linux/hwenc/vaapi/vaapi_h264_encoder.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "api/video/i420_buffer.h"
#include "api/video_codecs/h264_profile_level_id.h"
#include "modules/video_coding/codecs/h264/include/h264_globals.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"

namespace libwebrtc {
namespace {

bool RenderKnotenDa() {
  // ★ Ohne Render-Knoten keine VA-API. Das kommt öfter vor, als man denkt: in
  // Containern ohne `--device /dev/dri`, auf Servern ohne GPU und in Flatpaks
  // ohne `--device=dri`.
  for (const char* pfad : {"/dev/dri/renderD128", "/dev/dri/renderD129"}) {
    int fd = ::open(pfad, O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
      ::close(fd);
      return true;
    }
  }
  return false;
}

// ★ WebRTC liefert I420, die Hardware will NV12. Die Wandlung kostet einen
// Durchlauf über das Bild — sie ist der Preis dafür, dass dieser Encoder mit
// JEDEM Aufnehmer zusammenarbeitet (Kamera, PipeWire, Testbild). Ein echter
// Null-Kopie-Weg ginge nur über DMA-BUF vom Aufnehmer bis in den Encoder, und
// das ist eine eigene Runde.
void I420NachNv12(const webrtc::I420BufferInterface& src, AVFrame* dst) {
  const int w = src.width(), h = src.height();
  for (int y = 0; y < h; ++y) {
    memcpy(dst->data[0] + y * dst->linesize[0], src.DataY() + y * src.StrideY(), w);
  }
  const int cw = (w + 1) / 2, ch = (h + 1) / 2;
  for (int y = 0; y < ch; ++y) {
    uint8_t* zeile = dst->data[1] + y * dst->linesize[1];
    const uint8_t* u = src.DataU() + y * src.StrideU();
    const uint8_t* v = src.DataV() + y * src.StrideV();
    for (int x = 0; x < cw; ++x) {
      zeile[2 * x] = u[x];
      zeile[2 * x + 1] = v[x];
    }
  }
}

// Das im SDP ausgehandelte Profil in FFmpegs Zahl übersetzen.
//
// ★ NICHT EINFACH „constrained baseline" NEHMEN. Der Empfänger hat sich auf
// ein bestimmtes profile-level-id eingelassen; wer etwas anderes sendet,
// riskiert einen Decoder, der aussteigt oder Artefakte zeigt. Was hier
// herauskommt, ist genau das Format, das die Gegenstelle bestellt hat.
int ProfilAus(const webrtc::SdpVideoFormat& format) {
  const auto pl = webrtc::ParseSdpForH264ProfileLevelId(format.parameters);
  if (!pl) return AV_PROFILE_H264_CONSTRAINED_BASELINE;
  switch (pl->profile) {
    case webrtc::H264Profile::kProfileConstrainedBaseline:
      return AV_PROFILE_H264_CONSTRAINED_BASELINE;
    case webrtc::H264Profile::kProfileBaseline:
      return AV_PROFILE_H264_BASELINE;
    case webrtc::H264Profile::kProfileMain:
      return AV_PROFILE_H264_MAIN;
    case webrtc::H264Profile::kProfileConstrainedHigh:
    case webrtc::H264Profile::kProfileHigh:
    case webrtc::H264Profile::kProfilePredictiveHigh444:
      return AV_PROFILE_H264_HIGH;
  }
  return AV_PROFILE_H264_CONSTRAINED_BASELINE;
}

}  // namespace

// ── Verfügbar? ──────────────────────────────────────────────────────────────
bool VaapiH264Encoder::IstVerfuegbar() {
  static const bool ergebnis = [] {
    if (!RenderKnotenDa()) {
      RTC_LOG(LS_INFO) << "[vaapi] kein /dev/dri/renderD* — Software-Encoder";
      return false;
    }
    if (!ffmpeg::Laden()) return false;
    const auto& api = ffmpeg::Zugriff();
    if (!api.avcodec_find_encoder_by_name("h264_vaapi")) {
      RTC_LOG(LS_INFO) << "[vaapi] FFmpeg ohne h264_vaapi — Software-Encoder";
      return false;
    }
    // ★ Eine echte Probe, kein „sieht gut aus": Erst wenn sich ein
    // VA-API-Gerät wirklich öffnen lässt, ist ein brauchbarer Treiber da. Auf
    // einem System mit /dev/dri, aber ohne passenden Mesa-Treiber scheitert
    // genau hier, was sonst erst mitten im Gespräch aufgefallen wäre.
    AVBufferRef* geraet = nullptr;
    const int r = api.av_hwdevice_ctx_create(&geraet, AV_HWDEVICE_TYPE_VAAPI,
                                             nullptr, nullptr, 0);
    if (r < 0 || !geraet) {
      RTC_LOG(LS_INFO) << "[vaapi] VA-API-Gerät nicht zu öffnen ("
                       << ffmpeg::FehlerText(r) << ") — Software-Encoder";
      return false;
    }
    api.av_buffer_unref(&geraet);
    RTC_LOG(LS_INFO) << "[vaapi] Hardware-Encoder verfügbar ("
                     << ffmpeg::GeladeneBibliothek() << ")";
    return true;
  }();
  return ergebnis;
}

std::string VaapiH264Encoder::BackendName() {
  if (!IstVerfuegbar()) return std::string();
  return "VA-API (" + ffmpeg::GeladeneBibliothek() + ")";
}

// ── Leben und Sterben ───────────────────────────────────────────────────────
VaapiH264Encoder::VaapiH264Encoder(const webrtc::SdpVideoFormat& format)
    // 0,5 … 0,95: derselbe Rahmen wie im AMF-Encoder unter Windows. Hardware
    // trifft die Zielrate selten genau; der Ausgleicher zieht nach, was
    // wirklich herauskommt.
    : bitrate_adjuster_(0.5, 0.95), profil_(ProfilAus(format)) {
  const auto it = format.parameters.find(webrtc::kH264FmtpPacketizationMode);
  if (it != format.parameters.end() && it->second == "0") {
    packetization_mode_ = webrtc::H264PacketizationMode::SingleNalUnit;
  }
}

VaapiH264Encoder::~VaapiH264Encoder() {
  std::lock_guard<std::mutex> sperre(mutex_);
  CodecSchliessen();
}

int VaapiH264Encoder::InitEncode(
    const webrtc::VideoCodec* codec_settings,
    const webrtc::VideoEncoder::Settings& /*settings*/) {
  if (!codec_settings || codec_settings->codecType != webrtc::kVideoCodecH264) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  if (codec_settings->width < 1 || codec_settings->height < 1) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  if (!IstVerfuegbar()) return WEBRTC_VIDEO_CODEC_ERROR;

  std::lock_guard<std::mutex> sperre(mutex_);
  width_ = codec_settings->width;
  height_ = codec_settings->height;
  framerate_ = codec_settings->maxFramerate ? codec_settings->maxFramerate : 30;
  target_bitrate_bps_ = codec_settings->startBitrate * 1000;
  max_bitrate_bps_ = codec_settings->maxBitrate * 1000;
  if (target_bitrate_bps_ == 0) target_bitrate_bps_ = 1000000;
  if (max_bitrate_bps_ < target_bitrate_bps_) {
    max_bitrate_bps_ = target_bitrate_bps_ * 2;
  }
  bildschirm_ = codec_settings->mode == webrtc::VideoCodecMode::kScreensharing;
  bitrate_adjuster_.SetTargetBitrateBps(target_bitrate_bps_);
  frame_index_ = 0;

  return CodecOeffnen();
}

int32_t VaapiH264Encoder::CodecOeffnen() {
  const auto& api = ffmpeg::Zugriff();
  CodecSchliessen();

  const AVCodec* codec = api.avcodec_find_encoder_by_name("h264_vaapi");
  if (!codec) return WEBRTC_VIDEO_CODEC_ERROR;

  int r = api.av_hwdevice_ctx_create(&hw_device_, AV_HWDEVICE_TYPE_VAAPI,
                                     nullptr, nullptr, 0);
  if (r < 0) {
    RTC_LOG(LS_ERROR) << "[vaapi] VA-API-Gerät: " << ffmpeg::FehlerText(r);
    CodecSchliessen();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  // Bildspeicher der Hardware — der Encoder nimmt nur Bilder an, die aus
  // seinem eigenen Vorrat stammen; deshalb muss der Vorrat vor dem Öffnen
  // stehen.
  hw_frames_ = api.av_hwframe_ctx_alloc(hw_device_);
  if (!hw_frames_) {
    CodecSchliessen();
    return WEBRTC_VIDEO_CODEC_MEMORY;
  }
  auto* rahmen = reinterpret_cast<AVHWFramesContext*>(hw_frames_->data);
  rahmen->format = AV_PIX_FMT_VAAPI;
  rahmen->sw_format = AV_PIX_FMT_NV12;
  rahmen->width = width_;
  rahmen->height = height_;
  // 8 Bilder: genug, dass Hochladen und Kodieren einander nicht ausbremsen,
  // klein genug, dass bei großen Auflösungen kein Videospeicher brachliegt.
  rahmen->initial_pool_size = 8;
  r = api.av_hwframe_ctx_init(hw_frames_);
  if (r < 0) {
    RTC_LOG(LS_ERROR) << "[vaapi] Bildspeicher: " << ffmpeg::FehlerText(r);
    CodecSchliessen();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  ctx_ = api.avcodec_alloc_context3(codec);
  if (!ctx_) {
    CodecSchliessen();
    return WEBRTC_VIDEO_CODEC_MEMORY;
  }
  ctx_->width = width_;
  ctx_->height = height_;
  ctx_->pix_fmt = AV_PIX_FMT_VAAPI;
  ctx_->sw_pix_fmt = AV_PIX_FMT_NV12;
  // ★ Zeitbasis in Millisekunden. WebRTC rechnet in 90 kHz, FFmpeg braucht nur
  // eine monoton steigende Zahl — den RTP-Stempel setzen wir beim
  // Weiterreichen ohnehin selbst.
  ctx_->time_base = AVRational{1, 1000};
  ctx_->framerate = AVRational{static_cast<int>(framerate_), 1};
  ctx_->bit_rate = target_bitrate_bps_;
  ctx_->rc_max_rate = max_bitrate_bps_;
  // Puffer so groß wie eine halbe Sekunde: größer glättet stärker, kostet aber
  // Verzögerung — und Verzögerung ist in einem Gespräch teurer als eine
  // schwankende Rate.
  ctx_->rc_buffer_size = static_cast<int>(max_bitrate_bps_ / 2);
  // ★ KEINE periodischen Keyframes. WebRTC fordert sie an, wenn ein Empfänger
  // eines braucht; von selbst gesendete kosten nur Bandbreite. `gop_size`
  // lässt sich nicht abschalten, also setzen wir ihn außer Reichweite.
  ctx_->gop_size = 1 << 20;
  // ★ KEINE B-Bilder. Sie brauchen Umordnung und kosten mindestens ein Bild
  // Verzögerung — in einer Konferenz spürbar, und WebRTCs Paketierung erwartet
  // sie ohnehin nicht.
  ctx_->max_b_frames = 0;
  ctx_->refs = 1;
  ctx_->profile = profil_;
  ctx_->hw_frames_ctx = api.av_buffer_ref(hw_frames_);
  if (!ctx_->hw_frames_ctx) {
    CodecSchliessen();
    return WEBRTC_VIDEO_CODEC_MEMORY;
  }

  // ★ Private Optionen von h264_vaapi. `async_depth=1` heißt: ein Bild hinein,
  // ein Bild heraus — mehr Tiefe brächte Durchsatz, den wir nicht brauchen,
  // und Verzögerung, die wir nicht wollen. Die Ratensteuerung lassen wir
  // bewusst auf „auto": nicht jeder Treiber kann CBR, und ein abgelehnter
  // Modus kostet uns den ganzen Hardware-Weg.
  api.av_opt_set(ctx_->priv_data, "async_depth", "1", 0);
  // `idr_interval=0`: jedes angeforderte Keyframe wird ein echtes IDR, damit
  // ein neu hinzugekommener Empfänger wirklich einsteigen kann.
  api.av_opt_set(ctx_->priv_data, "idr_interval", "0", 0);

  r = api.avcodec_open2(ctx_, codec, nullptr);
  if (r < 0) {
    RTC_LOG(LS_ERROR) << "[vaapi] h264_vaapi öffnen: " << ffmpeg::FehlerText(r);
    CodecSchliessen();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  sw_frame_ = api.av_frame_alloc();
  hw_frame_ = api.av_frame_alloc();
  packet_ = api.av_packet_alloc();
  if (!sw_frame_ || !hw_frame_ || !packet_) {
    CodecSchliessen();
    return WEBRTC_VIDEO_CODEC_MEMORY;
  }
  sw_frame_->format = AV_PIX_FMT_NV12;
  sw_frame_->width = width_;
  sw_frame_->height = height_;
  r = api.av_frame_get_buffer(sw_frame_, 32);
  if (r < 0) {
    RTC_LOG(LS_ERROR) << "[vaapi] Zwischenbild: " << ffmpeg::FehlerText(r);
    CodecSchliessen();
    return WEBRTC_VIDEO_CODEC_MEMORY;
  }

  // ★ Frischer Kontext, frische Zählung (Prüfbefund 19.09.2026): Ohne das
  // greift die Regel „das erste Bild ist ein Keyframe" nach einem Neu-Aufsetzen
  // aus `Encode` heraus nicht mehr — und genau dann braucht der Empfänger
  // eines.
  frame_index_ = 0;
  stempel_.clear();
  oeffnen_scheiterte_ = false;
  misslungene_versuche_ = 0;
  letztes_aufsetzen_ms_ = webrtc::TimeMillis();

  RTC_LOG(LS_INFO) << "[vaapi] Encoder offen: " << width_ << "x" << height_
                   << " @" << framerate_ << ", " << (target_bitrate_bps_ / 1000)
                   << " kbit/s, Profil " << profil_
                   << (bildschirm_ ? ", Bildschirm" : "");
  return WEBRTC_VIDEO_CODEC_OK;
}

void VaapiH264Encoder::CodecSchliessen() {
  if (!ffmpeg::Laden()) return;
  const auto& api = ffmpeg::Zugriff();
  if (packet_) api.av_packet_free(&packet_);
  if (sw_frame_) api.av_frame_free(&sw_frame_);
  if (hw_frame_) api.av_frame_free(&hw_frame_);
  // Der Kontext hält eine eigene Referenz auf hw_frames_; free_context gibt
  // sie zurück, unsere eigene lösen wir danach.
  if (ctx_) api.avcodec_free_context(&ctx_);
  if (hw_frames_) api.av_buffer_unref(&hw_frames_);
  if (hw_device_) api.av_buffer_unref(&hw_device_);
  packet_ = nullptr;
  sw_frame_ = nullptr;
  hw_frame_ = nullptr;
  ctx_ = nullptr;
  hw_frames_ = nullptr;
  hw_device_ = nullptr;
}

int32_t VaapiH264Encoder::Release() {
  std::lock_guard<std::mutex> sperre(mutex_);
  CodecSchliessen();
  callback_ = nullptr;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t VaapiH264Encoder::RegisterEncodeCompleteCallback(
    webrtc::EncodedImageCallback* callback) {
  std::lock_guard<std::mutex> sperre(mutex_);
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

// ── Kodieren ────────────────────────────────────────────────────────────────
int32_t VaapiH264Encoder::Encode(
    const webrtc::VideoFrame& frame,
    const std::vector<webrtc::VideoFrameType>* frame_types) {
  std::lock_guard<std::mutex> sperre(mutex_);
  if (!callback_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;

  // ★ NICHT ENDGÜLTIG AUFGEBEN (Prüfbefund 19.09.2026). Ein gescheitertes
  // Öffnen kann vorübergehend sein — Gerät belegt, Bildspeicher erschöpft,
  // VA-Kontext nach Suspend verloren. Ohne erneuten Versuch bliebe das Bild
  // für den Rest des Gesprächs schwarz. Also alle 30 Bilder (rund eine
  // Sekunde) ein neuer Anlauf; nach zehn vergeblichen geben wir an WebRTC
  // ab, damit der umhüllende Adapter in Software weitermacht.
  if (!ctx_ && oeffnen_scheiterte_) {
    if (misslungene_versuche_ >= 10) return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    if (++wiederanlauf_zaehler_ % 30 != 0) return WEBRTC_VIDEO_CODEC_ERROR;
    width_ = frame.width();
    height_ = frame.height();
    if (CodecOeffnen() != WEBRTC_VIDEO_CODEC_OK) {
      oeffnen_scheiterte_ = true;
      ++misslungene_versuche_;
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
  }
  if (!ctx_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  const auto& api = ffmpeg::Zugriff();

  // Größenwechsel: neu aufsetzen. Das kommt bei jeder Stufenänderung vor, und
  // der Bildspeicher der Hardware liegt auf eine feste Größe fest.
  if (frame.width() != width_ || frame.height() != height_) {
    width_ = frame.width();
    height_ = frame.height();
    const int32_t r = CodecOeffnen();
    if (r != WEBRTC_VIDEO_CODEC_OK) {
      oeffnen_scheiterte_ = true;
      ++misslungene_versuche_;
      return r;
    }
  }

  bool keyframe = false;
  if (frame_types) {
    for (const auto& t : *frame_types) {
      if (t == webrtc::VideoFrameType::kVideoFrameKey) keyframe = true;
    }
  }
  // Das erste Bild nach dem Öffnen ist immer ein Keyframe — sonst hat der
  // Empfänger nichts, worauf er aufsetzen kann.
  if (frame_index_ == 0) keyframe = true;

  auto puffer = frame.video_frame_buffer()->ToI420();
  if (!puffer) return WEBRTC_VIDEO_CODEC_ERROR;

  int r = api.av_frame_make_writable(sw_frame_);
  if (r < 0) {
    RTC_LOG(LS_ERROR) << "[vaapi] make_writable: " << ffmpeg::FehlerText(r);
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  I420NachNv12(*puffer, sw_frame_);
  const int64_t pts = frame_index_++;
  sw_frame_->pts = pts;
  sw_frame_->pict_type = keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
  if (keyframe) keyframe_angefordert_ = true;

  // Zeitangaben unter der pts merken — siehe `Stempel` im Kopf.
  Stempel st;
  st.rtp = frame.rtp_timestamp();
  st.ntp = frame.ntp_time_ms();
  st.render = frame.render_time_ms();
  st.rotation = frame.rotation();
  stempel_[pts] = st;
  // Die Tabelle darf nicht wachsen, wenn ein Bild einmal nicht zurückkommt.
  while (stempel_.size() > 32) stempel_.erase(stempel_.begin());

  // In den Hardware-Speicher hochladen. Das Zielbild wird jedes Mal frisch
  // geholt; ohne das vorherige Freigeben hielte der Encoder Puffer fest, die
  // er selbst noch braucht.
  api.av_frame_unref(hw_frame_);
  r = api.av_hwframe_get_buffer(hw_frames_, hw_frame_, 0);
  if (r < 0) {
    RTC_LOG(LS_ERROR) << "[vaapi] Hardware-Bildpuffer: " << ffmpeg::FehlerText(r);
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  r = api.av_hwframe_transfer_data(hw_frame_, sw_frame_, 0);
  if (r < 0) {
    RTC_LOG(LS_ERROR) << "[vaapi] Hochladen: " << ffmpeg::FehlerText(r);
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  hw_frame_->pts = sw_frame_->pts;
  hw_frame_->pict_type = sw_frame_->pict_type;

  r = api.avcodec_send_frame(ctx_, hw_frame_);
  if (r == AVERROR(EAGAIN)) {
    // ★ EAGAIN heißt „nimm erst die Ausgabe ab und schick DASSELBE Bild
    // nochmal" (Prüfbefund 19.09.2026). Es als Erfolg zu behandeln, hätte das
    // Bild stillschweigend verworfen — und mit ihm womöglich ein angefordertes
    // Keyframe.
    PaketeAbholen();
    r = api.avcodec_send_frame(ctx_, hw_frame_);
  }
  if (r < 0) {
    RTC_LOG(LS_ERROR) << "[vaapi] send_frame: " << ffmpeg::FehlerText(r);
    stempel_.erase(pts);
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  PaketeAbholen();
  return WEBRTC_VIDEO_CODEC_OK;
}

void VaapiH264Encoder::PaketeAbholen() {
  const auto& api = ffmpeg::Zugriff();
  while (true) {
    const int r = api.avcodec_receive_packet(ctx_, packet_);
    if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return;
    if (r < 0) {
      RTC_LOG(LS_ERROR) << "[vaapi] receive_packet: " << ffmpeg::FehlerText(r);
      return;
    }
    if (packet_->size > 0 && packet_->data) {
      encoded_image_.SetEncodedData(webrtc::EncodedImageBuffer::Create(
          packet_->data, static_cast<size_t>(packet_->size)));
      encoded_image_._encodedWidth = width_;
      encoded_image_._encodedHeight = height_;
      // Die Zeitangaben des Bildes, das DIESES Paket erzeugt hat.
      const auto es = stempel_.find(packet_->pts);
      const Stempel st = (es != stempel_.end()) ? es->second : Stempel();
      if (es != stempel_.end()) stempel_.erase(es);
      encoded_image_.SetRtpTimestamp(st.rtp);
      encoded_image_.ntp_time_ms_ = st.ntp;
      encoded_image_.capture_time_ms_ = st.render;
      encoded_image_.rotation_ = st.rotation;
      encoded_image_.content_type_ = bildschirm_
                                         ? webrtc::VideoContentType::SCREENSHARE
                                         : webrtc::VideoContentType::UNSPECIFIED;
      const bool ist_key = (packet_->flags & AV_PKT_FLAG_KEY) != 0;
      encoded_image_.SetFrameType(ist_key
                                      ? webrtc::VideoFrameType::kVideoFrameKey
                                      : webrtc::VideoFrameType::kVideoFrameDelta);
      // ★ EINMALIGE MESSUNG statt Annahme: Dass `pict_type = I` bei
      // h264_vaapi wirklich ein IDR erzwingt, ist dokumentiertes Verhalten,
      // aber vom Treiber abhängig. Bleibt die Marke aus, steht es im
      // Protokoll — sonst suchte man später an der falschen Stelle, warum ein
      // neu Hinzugekommener kein Bild bekommt.
      if (keyframe_angefordert_) {
        keyframe_angefordert_ = false;
        if (!ist_key && !keyframe_warnung_) {
          keyframe_warnung_ = true;
          RTC_LOG(LS_WARNING) << "[vaapi] Keyframe angefordert, aber ohne "
                                 "IDR-Marke zurueck — Treiber ignoriert pict_type";
        }
      }

      // ★ Der Parser liefert QP — ohne ihn fehlt der Gegenseite die Grundlage
      // für ihre Qualitätsmeldung, und unsere eigene Statistik zeigt Nullen.
      h264_parser_.ParseBitstream(webrtc::ArrayView<const uint8_t>(
          packet_->data, static_cast<size_t>(packet_->size)));
      encoded_image_.qp_ = h264_parser_.GetLastSliceQp().value_or(-1);

      webrtc::CodecSpecificInfo spezifisch;
      spezifisch.codecType = webrtc::kVideoCodecH264;
      spezifisch.codecSpecific.H264.packetization_mode = packetization_mode_;

      callback_->OnEncodedImage(encoded_image_, &spezifisch);
      bitrate_adjuster_.Update(static_cast<size_t>(packet_->size));
    }
    api.av_packet_unref(packet_);
  }
}

void VaapiH264Encoder::SetRates(
    const webrtc::VideoEncoder::RateControlParameters& parameters) {
  std::lock_guard<std::mutex> sperre(mutex_);
  if (parameters.bitrate.get_sum_bps() == 0) return;
  if (parameters.framerate_fps > 0) {
    framerate_ = static_cast<uint32_t>(parameters.framerate_fps);
  }
  bitrate_adjuster_.SetTargetBitrateBps(parameters.bitrate.get_sum_bps());
  const uint32_t neu = bitrate_adjuster_.GetAdjustedBitrateBps();
  if (!ctx_ || neu == 0 || target_bitrate_bps_ == 0) return;

  // ★ `ctx_->bit_rate` NACH dem Öffnen zu beschreiben, bleibt wirkungslos
  // (Prüfbefund 19.09.2026): h264_vaapi baut seine Ratensteuerung beim Öffnen
  // einmal auf, und FFmpeg hat für Encoder keine Nachkonfiguration. Die
  // Staukontrolle würde also auf 400 kbit/s drosseln, während weiter die alte
  // Rate herausliefe — Warteschlange voll, Verluste, Schätzung sinkt weiter.
  //
  // Also wirklich neu aufsetzen. Das kostet ein Keyframe, deshalb nur bei
  // echten Sprüngen: mehr als ein Viertel Abweichung UND höchstens alle drei
  // Sekunden. Kleine Wellen fängt der Ausgleicher ab.
  const uint32_t gross = std::max(neu, target_bitrate_bps_);
  const uint32_t klein = std::min(neu, target_bitrate_bps_);
  const bool sprung = (gross - klein) * 4 > gross;
  const int64_t jetzt = webrtc::TimeMillis();
  if (!sprung || jetzt - letztes_aufsetzen_ms_ < 3000) return;

  target_bitrate_bps_ = neu;
  if (max_bitrate_bps_ < target_bitrate_bps_) {
    max_bitrate_bps_ = target_bitrate_bps_ * 2;
  }
  if (CodecOeffnen() != WEBRTC_VIDEO_CODEC_OK) {
    // Nicht schlimmer machen als nötig: Der Wiederanlauf in `Encode` fängt es
    // auf, und scheitert auch der, übernimmt der Software-Rückfall.
    oeffnen_scheiterte_ = true;
    ++misslungene_versuche_;
    RTC_LOG(LS_ERROR) << "[vaapi] Neu-Aufsetzen nach Ratenwechsel misslungen";
  }
}

webrtc::VideoEncoder::EncoderInfo VaapiH264Encoder::GetEncoderInfo() const {
  webrtc::VideoEncoder::EncoderInfo info;
  info.implementation_name = "HoneyCord VA-API";
  info.is_hardware_accelerated = true;
  // ★ Die Hardware braucht gerade Maße — 16 ist die sichere Zahl für H.264
  // (Makroblock). Ohne diese Angabe schickt WebRTC ungerade Größen, und der
  // Treiber schneidet stillschweigend ab.
  info.requested_resolution_alignment = 16;
  info.supports_native_handle = false;
  info.has_trusted_rate_controller = false;
  info.scaling_settings = webrtc::VideoEncoder::ScalingSettings(24, 37);
  return info;
}

}  // namespace libwebrtc
