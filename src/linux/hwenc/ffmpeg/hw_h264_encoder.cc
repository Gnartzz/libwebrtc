#include "src/linux/hwenc/ffmpeg/hw_h264_encoder.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>

#include "api/video/i420_buffer.h"
#include "api/video_codecs/h264_profile_level_id.h"
#include "media/base/media_constants.h"
#include "modules/video_coding/codecs/h264/include/h264_globals.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"

namespace libwebrtc {

namespace {

// ★★ EIGENE PROTOKOLLDATEI `hwenc.log` (21.09.2026): `RTC_LOG` aus diesem
// Encoder erreicht das Protokoll des Clients NICHT — gemessen an Tims
// Bündeln, null Treffer für „[hwenc]". Damit war die Frage „was gibt WebRTC
// dem Encoder vor, und was macht er daraus?" nicht zu beantworten, obwohl
// genau sie zwischen Bandbreitenschätzung und Encoder entscheidet. Die Datei
// liegt neben `send.log` (dieselbe Regel wie `log_ort.dart` im Client) und
// geht mit dem Diagnose-Bündel automatisch mit hoch.
void Protokoll(const char* format, ...) {
  static std::string pfad;
  if (pfad.empty()) {
    const char* xdg = std::getenv("XDG_STATE_HOME");
    const char* home = std::getenv("HOME");
    std::string dir = (xdg && *xdg) ? std::string(xdg) + "/honeycord"
                    : (home && *home) ? std::string(home) + "/.local/state/honeycord"
                    : std::string("/tmp/honeycord");
    ::mkdir(dir.c_str(), 0700);
    pfad = dir + "/hwenc.log";
  }
  FILE* f = std::fopen(pfad.c_str(), "a");
  if (!f) return;
  // Gedeckelt wie diag.log: ab 512 KB von vorn (die letzte Sitzung zählt).
  struct stat st;
  if (::stat(pfad.c_str(), &st) == 0 && st.st_size > 512 * 1024) {
    std::fclose(f);
    f = std::fopen(pfad.c_str(), "w");
    if (!f) return;
  }
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  std::fprintf(f, "[%02d-%02d %02d:%02d:%02d] ", tm.tm_mon + 1, tm.tm_mday,
               tm.tm_hour, tm.tm_min, tm.tm_sec);
  va_list args;
  va_start(args, format);
  std::vfprintf(f, format, args);
  va_end(args);
  std::fputc('\n', f);
  std::fclose(f);
}

}  // namespace

namespace {

bool NvidiaKnotenDa() {
  // ★ Erst nachsehen, ob ueberhaupt eine NVIDIA-Karte da ist. `h264_nvenc`
  // existiert in jedem FFmpeg-Bau, auch auf Rechnern ohne NVIDIA — ein blinder
  // Oeffnungsversuch kostet dort nur Zeit und schreibt Fehler ins Protokoll.
  for (const char* pfad : {"/dev/nvidiactl", "/dev/nvidia0"}) {
    if (::access(pfad, F_OK) == 0) return true;
  }
  return false;
}

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
namespace {

// Probiert EINEN Weg wirklich aus: Encoder vorhanden und Gerät zu öffnen.
//
// ★ Keine Vermutung, sondern eine Probe. Ein System kann /dev/dri haben und
// trotzdem keinen brauchbaren Treiber; `h264_nvenc` liegt in jedem FFmpeg,
// auch ohne NVIDIA-Karte. Was hier nicht aufgeht, fällt sofort auf Software
// zurück — statt mitten im ersten Gespräch.
bool WegGeht(const char* encoder, enum AVHWDeviceType typ, const char* name) {
  const auto& api = ffmpeg::Zugriff();
  if (!api.avcodec_find_encoder_by_name(encoder)) {
    RTC_LOG(LS_INFO) << "[hwenc] FFmpeg ohne " << encoder;
    return false;
  }
  AVBufferRef* geraet = nullptr;
  const int r = api.av_hwdevice_ctx_create(&geraet, typ, nullptr, nullptr, 0);
  if (r < 0 || !geraet) {
    RTC_LOG(LS_INFO) << "[hwenc] " << name << "-Gerät nicht zu öffnen ("
                     << ffmpeg::FehlerText(r) << ")";
    return false;
  }
  api.av_buffer_unref(&geraet);
  return true;
}

// Einmal ermittelt, dann gemerkt: -1 = keiner, sonst der Backend-Wert.
int WegErmitteln() {
  if (!ffmpeg::Laden()) return -1;
  // ★ NVIDIA zuerst: Steckt eine NVIDIA-Karte im Rechner, ist sie fast immer
  // die kräftigere, und ihr VA-API-Treiber kann ohnehin nur dekodieren.
  if (NvidiaKnotenDa() &&
      WegGeht("h264_nvenc", AV_HWDEVICE_TYPE_CUDA, "CUDA")) {
    RTC_LOG(LS_INFO) << "[hwenc] NVENC verfügbar (" << ffmpeg::GeladeneBibliothek() << ")";
    return static_cast<int>(HwH264Encoder::Backend::kNvenc);
  }
  if (RenderKnotenDa() && WegGeht("h264_vaapi", AV_HWDEVICE_TYPE_VAAPI, "VA-API")) {
    RTC_LOG(LS_INFO) << "[hwenc] VA-API verfügbar (" << ffmpeg::GeladeneBibliothek() << ")";
    return static_cast<int>(HwH264Encoder::Backend::kVaapi);
  }
  RTC_LOG(LS_INFO) << "[hwenc] kein Hardware-Encoder — Software";
  return -1;
}

int GemerkterWeg() {
  static const int weg = WegErmitteln();
  return weg;
}

}  // namespace

bool HwH264Encoder::IstVerfuegbar() { return GemerkterWeg() >= 0; }

HwH264Encoder::Backend HwH264Encoder::GewaehlterWeg() {
  return static_cast<Backend>(GemerkterWeg() < 0 ? 0 : GemerkterWeg());
}

std::string HwH264Encoder::BackendName() {
  if (!IstVerfuegbar()) return std::string();
  const char* n = GewaehlterWeg() == Backend::kNvenc ? "NVENC" : "VA-API";
  return std::string(n) + " (" + ffmpeg::GeladeneBibliothek() + ")";
}

// ── Leben und Sterben ───────────────────────────────────────────────────────
HwH264Encoder::HwH264Encoder(const webrtc::SdpVideoFormat& format)
    // 0,5 … 0,95: derselbe Rahmen wie im AMF-Encoder unter Windows. Hardware
    // trifft die Zielrate selten genau; der Ausgleicher zieht nach, was
    // wirklich herauskommt.
    : bitrate_adjuster_(0.5, 0.95), profil_(ProfilAus(format)) {
  if (IstVerfuegbar()) weg_ = GewaehlterWeg();
  const auto it = format.parameters.find(webrtc::kH264FmtpPacketizationMode);
  if (it != format.parameters.end() && it->second == "0") {
    packetization_mode_ = webrtc::H264PacketizationMode::SingleNalUnit;
  }
}

HwH264Encoder::~HwH264Encoder() {
  std::lock_guard<std::mutex> sperre(mutex_);
  CodecSchliessen();
}

int HwH264Encoder::InitEncode(
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

int32_t HwH264Encoder::CodecOeffnen() {
  const auto& api = ffmpeg::Zugriff();
  CodecSchliessen();

  const bool nvenc = weg_ == Backend::kNvenc;
  const AVCodec* codec =
      api.avcodec_find_encoder_by_name(nvenc ? "h264_nvenc" : "h264_vaapi");
  if (!codec) return WEBRTC_VIDEO_CODEC_ERROR;

  int r = 0;
  if (!nvenc) {
    // ★ NUR VA-API braucht diesen Vorbau. NVENC nimmt Bilder aus dem
    // Arbeitsspeicher entgegen und lädt sie selbst hoch — ein eigener
    // Bildspeicher waere dort nur Ballast.
    r = api.av_hwdevice_ctx_create(&hw_device_, AV_HWDEVICE_TYPE_VAAPI,
                                   nullptr, nullptr, 0);
    if (r < 0) {
      RTC_LOG(LS_ERROR) << "[hwenc] VA-API-Gerät: " << ffmpeg::FehlerText(r);
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
      RTC_LOG(LS_ERROR) << "[hwenc] Bildspeicher: " << ffmpeg::FehlerText(r);
      CodecSchliessen();
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
  }

  ctx_ = api.avcodec_alloc_context3(codec);
  if (!ctx_) {
    CodecSchliessen();
    return WEBRTC_VIDEO_CODEC_MEMORY;
  }
  ctx_->width = width_;
  ctx_->height = height_;
  // NVENC bekommt NV12 direkt; VA-API bekommt Oberflächen mit NV12 darin.
  ctx_->pix_fmt = nvenc ? AV_PIX_FMT_NV12 : AV_PIX_FMT_VAAPI;
  ctx_->sw_pix_fmt = AV_PIX_FMT_NV12;
  // ★ Zeitbasis in Millisekunden. WebRTC rechnet in 90 kHz, FFmpeg braucht nur
  // eine monoton steigende Zahl — den RTP-Stempel setzen wir beim
  // Weiterreichen ohnehin selbst.
  ctx_->time_base = AVRational{1, 1000};
  ctx_->framerate = AVRational{static_cast<int>(framerate_), 1};
  ctx_->bit_rate = target_bitrate_bps_;
  // ★★ GEMESSEN 21.09.2026 (Tim: „alle 2–5 Sekunden scheint das Bild kurz zu
  // stehen", bei 7,8 Mbit/s Ziel und 60 Bildern/s):
  //
  // Die Spitzenrate hing am DECKEL der Qualitätsstufe (24 Mbit/s), nicht am
  // tatsächlichen Ziel. Der Encoder durfte also jederzeit auf das Dreifache
  // ausbrechen, und der Puffer (halber Deckel = 12 Mbit) reichte für rund
  // ANDERTHALB SEKUNDEN dieser Spitze. Genau das kam in den Zahlen an: 9677
  // kbit/s gemessen, wo 7825 vorgegeben waren. Ein solcher Ausbruch muss über
  // die Leitung geschoben werden, und solange das läuft, wartet der Empfänger
  // — das Bild steht kurz und holt dann auf.
  //
  // Für ein Gespräch ist eine gleichmäßige Rate mehr wert als eine hohe
  // Spitze. Also: Spitzenrate = Ziel (knapp darüber, damit die Steuerung Luft
  // hat), Puffer eine halbe Sekunde AM ZIEL statt am Deckel.
  ctx_->rc_max_rate = static_cast<int64_t>(target_bitrate_bps_) * 11 / 10;
  ctx_->rc_buffer_size = static_cast<int>(target_bitrate_bps_ / 2);
  // ★ KEINE periodischen Keyframes. WebRTC fordert sie an, wenn ein Empfänger
  // eines braucht; von selbst gesendete kosten nur Bandbreite. `gop_size`
  // lässt sich nicht abschalten, also setzen wir ihn außer Reichweite.
  ctx_->gop_size = 1 << 20;
  // ★ KEINE B-Bilder. Sie brauchen Umordnung und kosten mindestens ein Bild
  // Verzögerung — in einer Konferenz spürbar, und WebRTCs Paketierung erwartet
  // sie ohnehin nicht.
  ctx_->max_b_frames = 0;
  // ★ `refs = 1` NUR fuer VA-API (Pruefbefund 19.09.2026). NVENC liest alles
  // ausser 0 als „mehrere Referenzbilder" und verlangt dafuer eine Faehigkeit,
  // die es erst ab Turing gibt — auf einer GTX 9xx/10xx schlaegt avcodec_open2
  // damit IMMER fehl. Das Protokoll haette „NVENC verfuegbar" gemeldet und der
  // Client dauerhaft in Software kodiert.
  if (!nvenc) ctx_->refs = 1;
  ctx_->profile = profil_;
  if (!nvenc) {
    ctx_->hw_frames_ctx = api.av_buffer_ref(hw_frames_);
    if (!ctx_->hw_frames_ctx) {
      CodecSchliessen();
      return WEBRTC_VIDEO_CODEC_MEMORY;
    }
  }

  if (nvenc) {
    // ★ Private Optionen von h264_nvenc, alle auf Gespräch statt Aufnahme
    // ausgelegt: `p1` ist die schnellste Voreinstellung, `ull` (ultra low
    // latency) verzichtet auf Blick nach vorn, `cbr` hält die Rate, und
    // `delay=0` gibt jedes Bild sofort heraus statt es zu puffern.
    // Vorausschau abschalten ist Pflicht — sie kostet Bilder Verzögerung.
    api.av_opt_set(ctx_->priv_data, "preset", "p1", 0);
    api.av_opt_set(ctx_->priv_data, "tune", "ull", 0);
    api.av_opt_set(ctx_->priv_data, "rc", "cbr", 0);
    api.av_opt_set_int(ctx_->priv_data, "delay", 0, 0);
    api.av_opt_set_int(ctx_->priv_data, "rc-lookahead", 0, 0);
    api.av_opt_set_int(ctx_->priv_data, "zerolatency", 1, 0);
    // ★ OHNE DIES IST EIN ANGEFORDERTES KEYFRAME KEIN IDR (Pruefbefund
    // 19.09.2026): NVENC macht aus `pict_type = I` ohne `forced-idr` nur ein
    // I-Bild ohne Wiedereinstiegspunkt, und `AV_PKT_FLAG_KEY` bleibt aus. Mit
    // unserem sehr grossen gop_size waere nur das allererste Bild ein IDR —
    // jeder spaeter Hinzukommende saehe nie ein Bild. Das ist das Gegenstueck
    // zum `idr_interval=0` auf der VA-API-Seite.
    api.av_opt_set_int(ctx_->priv_data, "forced-idr", 1, 0);
    // ★ `ctx_->profile` ueberschreibt NVENC mit seiner eigenen Option (Vorgabe
    // `main`) — das ausgehandelte SDP-Profil ginge also verloren. NVENC kennt
    // kein Constrained Baseline; `baseline` ist die naechstliegende Abbildung.
    api.av_opt_set(ctx_->priv_data, "profile",
                   profil_ == AV_PROFILE_H264_HIGH   ? "high"
                   : profil_ == AV_PROFILE_H264_MAIN ? "main"
                                                     : "baseline",
                   0);
  } else {
    // ★ Private Optionen von h264_vaapi. `async_depth=1` heißt: ein Bild
    // hinein, ein Bild heraus — mehr Tiefe brächte Durchsatz, den wir nicht
    // brauchen, und Verzögerung, die wir nicht wollen. Die Ratensteuerung
    // lassen wir bewusst auf „auto": nicht jeder Treiber kann CBR, und ein
    // abgelehnter Modus kostet uns den ganzen Hardware-Weg.
    api.av_opt_set(ctx_->priv_data, "async_depth", "1", 0);
    // `idr_interval=0`: jedes angeforderte Keyframe wird ein echtes IDR, damit
    // ein neu hinzugekommener Empfänger wirklich einsteigen kann.
    api.av_opt_set(ctx_->priv_data, "idr_interval", "0", 0);
  }

  r = api.avcodec_open2(ctx_, codec, nullptr);
  if (r < 0) {
    RTC_LOG(LS_ERROR) << "[hwenc] Encoder öffnen: " << ffmpeg::FehlerText(r);
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
    RTC_LOG(LS_ERROR) << "[hwenc] Zwischenbild: " << ffmpeg::FehlerText(r);
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

  RTC_LOG(LS_INFO) << "[hwenc] " << (nvenc ? "NVENC" : "VA-API")
                   << " offen: " << width_ << "x" << height_
                   << " @" << framerate_ << ", " << (target_bitrate_bps_ / 1000)
                   << " kbit/s, Profil " << profil_
                   << (bildschirm_ ? ", Bildschirm" : "");
  Protokoll("offen: %s %ux%u @%u  ziel %u kbit/s  max %u kbit/s  Profil %d%s",
            nvenc ? "NVENC" : "VA-API", width_, height_, framerate_,
            target_bitrate_bps_ / 1000, max_bitrate_bps_ / 1000, profil_,
            bildschirm_ ? "  Bildschirm" : "");
  return WEBRTC_VIDEO_CODEC_OK;
}

void HwH264Encoder::CodecSchliessen() {
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

int32_t HwH264Encoder::Release() {
  std::lock_guard<std::mutex> sperre(mutex_);
  CodecSchliessen();
  callback_ = nullptr;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t HwH264Encoder::RegisterEncodeCompleteCallback(
    webrtc::EncodedImageCallback* callback) {
  std::lock_guard<std::mutex> sperre(mutex_);
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

// ── Kodieren ────────────────────────────────────────────────────────────────
int32_t HwH264Encoder::Encode(
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
    RTC_LOG(LS_ERROR) << "[hwenc] make_writable: " << ffmpeg::FehlerText(r);
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

  // VA-API will das Bild im Grafikspeicher; NVENC nimmt es aus dem
  // Arbeitsspeicher und lädt selbst hoch.
  AVFrame* hinein = sw_frame_;
  if (weg_ == Backend::kVaapi) {
    // Das Zielbild wird jedes Mal frisch geholt; ohne das vorherige Freigeben
    // hielte der Encoder Puffer fest, die er selbst noch braucht.
    api.av_frame_unref(hw_frame_);
    r = api.av_hwframe_get_buffer(hw_frames_, hw_frame_, 0);
    if (r < 0) {
      RTC_LOG(LS_ERROR) << "[hwenc] Hardware-Bildpuffer: " << ffmpeg::FehlerText(r);
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    r = api.av_hwframe_transfer_data(hw_frame_, sw_frame_, 0);
    if (r < 0) {
      RTC_LOG(LS_ERROR) << "[hwenc] Hochladen: " << ffmpeg::FehlerText(r);
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    hw_frame_->pts = sw_frame_->pts;
    hw_frame_->pict_type = sw_frame_->pict_type;
    hinein = hw_frame_;
  }

  r = api.avcodec_send_frame(ctx_, hinein);
  if (r == AVERROR(EAGAIN)) {
    // ★ EAGAIN heißt „nimm erst die Ausgabe ab und schick DASSELBE Bild
    // nochmal" (Prüfbefund 19.09.2026). Es als Erfolg zu behandeln, hätte das
    // Bild stillschweigend verworfen — und mit ihm womöglich ein angefordertes
    // Keyframe.
    PaketeAbholen();
    r = api.avcodec_send_frame(ctx_, hinein);
  }
  if (r < 0) {
    RTC_LOG(LS_ERROR) << "[hwenc] send_frame: " << ffmpeg::FehlerText(r);
    stempel_.erase(pts);
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  PaketeAbholen();
  return WEBRTC_VIDEO_CODEC_OK;
}

void HwH264Encoder::PaketeAbholen() {
  const auto& api = ffmpeg::Zugriff();
  while (true) {
    const int r = api.avcodec_receive_packet(ctx_, packet_);
    if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return;
    if (r < 0) {
      RTC_LOG(LS_ERROR) << "[hwenc] receive_packet: " << ffmpeg::FehlerText(r);
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
          RTC_LOG(LS_WARNING) << "[hwenc] Keyframe angefordert, aber ohne "
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

void HwH264Encoder::SetRates(
    const webrtc::VideoEncoder::RateControlParameters& parameters) {
  std::lock_guard<std::mutex> sperre(mutex_);
  if (parameters.bitrate.get_sum_bps() == 0) return;
  if (parameters.framerate_fps > 0) {
    framerate_ = static_cast<uint32_t>(parameters.framerate_fps);
  }
  bitrate_adjuster_.SetTargetBitrateBps(parameters.bitrate.get_sum_bps());
  const uint32_t neu = bitrate_adjuster_.GetAdjustedBitrateBps();
  // ★ Messung (21.09.2026): jede Vorgabe, die sich um mehr als ein Zehntel
  // vom letzten Eintrag unterscheidet, sonst höchstens alle fünf Sekunden.
  {
    static uint32_t zuletzt_gefordert = 0;
    static int64_t zuletzt_ms = 0;
    const uint32_t gefordert = parameters.bitrate.get_sum_bps();
    const int64_t jetzt_ms = webrtc::TimeMillis();
    const uint32_t g = std::max(gefordert, zuletzt_gefordert);
    const uint32_t k = std::min(gefordert, zuletzt_gefordert);
    if ((g - k) * 10 > g || jetzt_ms - zuletzt_ms >= 5000) {
      Protokoll("SetRates: WebRTC fordert %u kbit/s @%.1f fps  -> nach Ausgleich %u  "
                "(Encoder steht auf %u, max %u)%s",
                gefordert / 1000, parameters.framerate_fps, neu / 1000,
                target_bitrate_bps_ / 1000, max_bitrate_bps_ / 1000,
                ctx_ ? "" : "  [kein Kontext]");
      zuletzt_gefordert = gefordert;
      zuletzt_ms = jetzt_ms;
    }
  }
  if (!ctx_ || neu == 0 || target_bitrate_bps_ == 0) return;

  // ★ Die beiden Wege verhalten sich hier UNTERSCHIEDLICH (Prüfbefund
  // 19.09.2026):
  //
  //  * NVENC liest `bit_rate` vor jedem Bild neu und stellt sich selbst um.
  //    Ein Schreiben genügt — den Encoder neu aufzusetzen wäre teurer,
  //    riskanter (die Sitzung kann beim Öffnen scheitern) und brächte nichts.
  //  * h264_vaapi baut seine Ratensteuerung beim Öffnen EINMAL auf; ein
  //    späteres Schreiben bleibt wirkungslos. Die Staukontrolle würde also auf
  //    400 kbit/s drosseln, während weiter die alte Rate herausliefe —
  //    Warteschlange voll, Verluste, Schätzung sinkt weiter.
  const uint32_t gross = std::max(neu, target_bitrate_bps_);
  const uint32_t klein = std::min(neu, target_bitrate_bps_);
  // ★★ GEMESSEN 21.09.2026 (Tims Freigabe am Kabel, `hwenc.log` + `send.log`):
  // Mit „ein Viertel Abweichung, alle drei Sekunden" setzte sich der Encoder in
  // 80 Sekunden FÜNFMAL neu auf (1244 → 1859 → 930 → 495 → 285 → 703 kbit/s).
  // Jedes Neu-Aufsetzen kostet ein Vollbild, und bei 3840×1072 ist das bei
  // einem Budget von 200 kbit/s die Arbeit mehrerer Sekunden. Das frisst genau
  // die Bandbreite, die die Schätzung bräuchte, um wieder zu steigen — der
  // Strom kam mit 17–40 kbit/s an, obwohl die Leitung (2,5 Gbit, 11 ms, 0 %
  // Verlust) nichts hergab, woran es liegen könnte. Ein Kreis, der sich selbst
  // am Leben hält.
  //
  // Darum jetzt: erst bei DOPPELTER Abweichung und höchstens alle zehn
  // Sekunden. Zwischen den Vollbildern hat die Ratensteuerung damit Zeit, sich
  // einzuschwingen, und die Schätzung bekommt Luft zum Wachsen. Kleine Wellen
  // fängt weiterhin der Ausgleicher ab; was er nicht auffängt, kostet
  // Bildqualität — aber Bildqualität, die überhaupt ankommt.
  const bool sprung = gross > klein * 2;
  const int64_t jetzt = webrtc::TimeMillis();

  if (weg_ == Backend::kNvenc) {
    target_bitrate_bps_ = neu;
    if (max_bitrate_bps_ < target_bitrate_bps_) {
      max_bitrate_bps_ = target_bitrate_bps_ * 2;
    }
    // Dieselbe Rechnung wie beim Öffnen: Spitze am Ziel, nicht am Deckel.
    ctx_->bit_rate = target_bitrate_bps_;
    ctx_->rc_max_rate = static_cast<int64_t>(target_bitrate_bps_) * 11 / 10;
    ctx_->rc_buffer_size = static_cast<int>(target_bitrate_bps_ / 2);
    return;
  }

  // VA-API: wirklich neu aufsetzen. Das kostet ein Keyframe, deshalb nur bei
  // echten Sprüngen — mehr als ein Viertel Abweichung UND höchstens alle drei
  // Sekunden. Kleine Wellen fängt der Ausgleicher ab.
  if (!sprung || jetzt - letztes_aufsetzen_ms_ < 10000) return;

  Protokoll("VA-API neu aufsetzen: %u -> %u kbit/s (doppelte Abweichung, %lld ms seit dem letzten)",
            target_bitrate_bps_ / 1000, neu / 1000,
            static_cast<long long>(jetzt - letztes_aufsetzen_ms_));
  target_bitrate_bps_ = neu;
  if (max_bitrate_bps_ < target_bitrate_bps_) {
    max_bitrate_bps_ = target_bitrate_bps_ * 2;
  }
  if (CodecOeffnen() != WEBRTC_VIDEO_CODEC_OK) {
    // Nicht schlimmer machen als nötig: Der Wiederanlauf in `Encode` fängt es
    // auf, und scheitert auch der, übernimmt der Software-Rückfall.
    oeffnen_scheiterte_ = true;
    ++misslungene_versuche_;
    RTC_LOG(LS_ERROR) << "[hwenc] Neu-Aufsetzen nach Ratenwechsel misslungen";
    Protokoll("FEHLER: Neu-Aufsetzen nach Ratenwechsel misslungen (Versuch %d)",
              misslungene_versuche_);
  }
}

webrtc::VideoEncoder::EncoderInfo HwH264Encoder::GetEncoderInfo() const {
  webrtc::VideoEncoder::EncoderInfo info;
  info.implementation_name = weg_ == Backend::kNvenc ? "HoneyCord NVENC"
                                                     : "HoneyCord VA-API";
  info.is_hardware_accelerated = true;
  // ★ Die Hardware braucht gerade Maße — 16 ist die sichere Zahl für H.264
  // (Makroblock). Ohne diese Angabe schickt WebRTC ungerade Größen, und der
  // Treiber schneidet stillschweigend ab.
  info.requested_resolution_alignment = 16;
  info.supports_native_handle = false;
  // ★★ GEMESSEN 21.09.2026 auf dem UM890 (drei Läufe, Netz gut wie schlecht):
  // Der Bildschirm-Capturer lieferte 54–57 Bilder/s, dieser Encoder brauchte
  // 6–9 ms je Bild — und kodiert wurden 7 bis 9. Die Bilder verwarf WebRTC
  // SELBST, zwischen Capturer und Encoder: Mit `has_trusted_rate_controller =
  // false` schaltet es seinen eigenen Frame-Dropper vor, der Bilder wegwirft,
  // sobald der Encoder mehr Bytes liefert, als die geschätzte Bandbreite
  // hergibt. Die Schätzung hing bei 16–336 kbit/s bei erlaubten 24 000 — das
  // Henne-Ei aus #102: Wer nichts sendet, dem wird nichts zugestanden.
  //
  // Der Windows-Encoder des Forks (`src/win/msdkvideoencoder.cc`) meldet
  // `true` und `ScalingSettings::kOff` — dort gehen 99 % der Bilder durch.
  // Beim Bau am 19.09. hatte ich die vorsichtige Einstellung gewählt, ohne
  // das Vorbild anzusehen. `true` ist hier auch WAHR: VA-API und NVENC regeln
  // die Rate selbst (CBR/VBR im Treiber), und `SetRates` reicht Bitrate und
  // Bildrate an sie durch. Den Qualitäts-Skalierer braucht es ebenso nicht —
  // die Auflösungsstufen wählt der Client (Stufe 1080p60 usw.), nicht WebRTC.
  info.has_trusted_rate_controller = true;
  info.scaling_settings = webrtc::VideoEncoder::ScalingSettings::kOff;
  return info;
}

}  // namespace libwebrtc
