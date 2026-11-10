#include "src/linux/hwenc/ffmpeg/hw_h264_decoder.h"

#include <strings.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "api/video/color_space.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "libyuv/convert.h"
#include "libyuv/planar_functions.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"
#include "src/linux/hwenc/ffmpeg/hw_nullkopie.h"
#include "src/linux/hwenc/hw_gemeinsam.h"

namespace libwebrtc {

namespace {

constexpr const char kName[] = "HoneyCord VA-API";
std::atomic<int> g_naechste_nummer{1};

bool AbgeschaltetPerUmgebung() {
  const char* w = std::getenv("HONEYCORD_HWDEC");
  if (!w) return false;
  return std::strcmp(w, "0") == 0 || ::strcasecmp(w, "aus") == 0 ||
         ::strcasecmp(w, "off") == 0 || ::strcasecmp(w, "nein") == 0;
}

// Kennt FFmpegs H.264-Decoder einen VA-API-Weg über einen Gerätekontext?
// ★ Nicht selbstverständlich: Fedoras „ffmpeg-free" und manche Flatpak-
// Laufzeiten bauen H.264 ohne VA-API-Beschleuniger. Dann gibt es hier nichts
// zu tun, und es bleibt beim eingebauten Software-Decoder.
bool H264MitVaapi(const ffmpeg::Api& api) {
  const AVCodec* codec = api.avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!codec) {
    Protokoll("Decoder: FFmpeg ohne H.264-Decoder — Software wie bisher");
    return false;
  }
  for (int i = 0;; ++i) {
    const AVCodecHWConfig* cfg = api.avcodec_get_hw_config(codec, i);
    if (!cfg) break;
    if (cfg->device_type == AV_HWDEVICE_TYPE_VAAPI &&
        (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
      return true;
    }
  }
  Protokoll("Decoder: FFmpeg-H.264 (%s) ohne VA-API-Beschleuniger — Software wie bisher",
            codec->name ? codec->name : "?");
  return false;
}

bool HerunterladenGeht(const ffmpeg::Api& api, AVBufferRef* geraet) {
  AVBufferRef* vorrat = api.av_hwframe_ctx_alloc(geraet);
  if (!vorrat) return false;
  auto* fc = reinterpret_cast<AVHWFramesContext*>(vorrat->data);
  fc->format = AV_PIX_FMT_VAAPI;
  fc->sw_format = AV_PIX_FMT_NV12;
  fc->width = 64;
  fc->height = 64;
  fc->initial_pool_size = 1;
  bool ok = api.av_hwframe_ctx_init(vorrat) >= 0;
  AVFrame* hw = ok ? api.av_frame_alloc() : nullptr;
  AVFrame* sw = ok ? api.av_frame_alloc() : nullptr;
  ok = ok && hw && sw && api.av_hwframe_get_buffer(vorrat, hw, 0) >= 0;
  if (ok) {
    sw->format = AV_PIX_FMT_NV12;
    ok = api.av_hwframe_transfer_data(sw, hw, 0) >= 0 && sw->format == AV_PIX_FMT_NV12;
  }
  if (hw) api.av_frame_free(&hw);
  if (sw) api.av_frame_free(&sw);
  api.av_buffer_unref(&vorrat);
  return ok;
}

bool Pruefen() {
  if (AbgeschaltetPerUmgebung()) {
    Protokoll("Decoder: HONEYCORD_HWDEC=%s — Hardware-Dekodierung abgeschaltet",
              std::getenv("HONEYCORD_HWDEC"));
    return false;
  }
  if (!ffmpeg::Laden()) return false;
  const auto& api = ffmpeg::Zugriff();
  if (!api.dekoder_ok) {
    Protokoll("Decoder: FFmpeg ohne Dekoder-Funktionen — Software wie bisher");
    return false;
  }
  if (!H264MitVaapi(api)) return false;
  if (!RenderKnotenDa()) {
    Protokoll("Decoder: kein Render-Knoten (/dev/dri) — Software wie bisher");
    return false;
  }
  AVBufferRef* geraet = nullptr;
  const int r = api.av_hwdevice_ctx_create(&geraet, AV_HWDEVICE_TYPE_VAAPI,
                                           nullptr, nullptr, 0);
  if (r < 0 || !geraet) {
    Protokoll("Decoder: VA-API-Gerät nicht zu öffnen (%s) — Software wie bisher",
              ffmpeg::FehlerText(r).c_str());
    return false;
  }
  const std::string treiber = VaTreiber(geraet);
  // ★ NVIDIA bleibt draußen (Prüfbefund M2): Dort kodiert NVENC, und der
  // VA-Treiber (nvidia-vaapi-driver) ist ein Aufsatz auf NVDEC, den wir nie
  // gemessen haben. Software ist dort der bekannte Weg.
  if (treiber.find("NVDEC") != std::string::npos ||
      treiber.find("nvidia") != std::string::npos ||
      treiber.find("NVIDIA") != std::string::npos) {
    api.av_buffer_unref(&geraet);
    Protokoll("Decoder: VA-Treiber %s ist NVIDIA — Software wie bisher", treiber.c_str());
    return false;
  }
  // ★ PROBE MIT ECHTEM HERUNTERLADEN (Prüfbefund M1): Ein Treiber, der zwar
  // dekodiert, aber kein Bild zurückgibt, fiele sonst erst im Gespräch auf —
  // und WebRTC reicht nach einem Fehler nur noch Schlüsselbilder durch, also
  // Sekunden ohne Bild je Spur. Hier kostet es einmal ein 64×64-Bild.
  if (!HerunterladenGeht(api, geraet)) {
    api.av_buffer_unref(&geraet);
    Protokoll("Decoder: VA-API-Bild lässt sich nicht herunterladen (Treiber %s) — "
              "Software wie bisher", treiber.c_str());
    return false;
  }
  api.av_buffer_unref(&geraet);
  Protokoll("Decoder: H.264 über VA-API verfügbar (%s, Treiber %s, %s)",
            ffmpeg::GeladeneBibliothek().c_str(), treiber.c_str(),
            GpuBeschreibung().c_str());
  return true;
}

}  // namespace

bool HwH264Decoder::IstVerfuegbar() {
  static const bool ja = Pruefen();
  return ja;
}

HwH264Decoder::HwH264Decoder()
    : nummer_(g_naechste_nummer.fetch_add(1)),
      // Kein Nullen: jedes Bild wird vollständig überschrieben.
      pool_(/*zero_initialize=*/false) {}

HwH264Decoder::~HwH264Decoder() { Schliessen(); }

void HwH264Decoder::Schliessen() {
  if (!ffmpeg::Laden()) return;
  const auto& api = ffmpeg::Zugriff();
  if (ctx_) api.avcodec_free_context(&ctx_);  // gibt auch hw_device_ctx frei
  if (hw_device_) api.av_buffer_unref(&hw_device_);
  if (packet_) api.av_packet_free(&packet_);
  if (hw_frame_) api.av_frame_free(&hw_frame_);
  if (sw_frame_) api.av_frame_free(&sw_frame_);
  if (map_frame_) api.av_frame_free(&map_frame_);
  ctx_ = nullptr;
  rueckfall_ = false;
  vaapi_aktiv_ = false;
  gemeldet_ = false;
  rueckfall_grund_.clear();
  fehler_in_folge_ = 0;
}

bool HwH264Decoder::Configure(const Settings& settings) {
  Schliessen();
  if (settings.codec_type() != webrtc::kVideoCodecH264) return false;
  if (!IstVerfuegbar()) return false;
  const auto& api = ffmpeg::Zugriff();

  const AVCodec* codec = api.avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!codec) return false;
  ctx_ = api.avcodec_alloc_context3(codec);
  if (!ctx_) return false;

  // ★ Empfang ohne Umweg (10.11.2026): dasselbe VA-Gerät wie Wandler und
  // Encoder — nur dann kann die Grafikkarte das Bild für die Anzeige
  // weiterreichen, ohne dass es den Speicher verlässt.
  int r = 0;
  if (nullkopie::EmpfangErlaubt()) hw_device_ = nullkopie::GeraetRef();
  if (!hw_device_)
    r = api.av_hwdevice_ctx_create(&hw_device_, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
  if (r < 0 || !hw_device_) {
    Protokoll("Decoder #%d: VA-API-Gerät: %s — Rückfall auf Software", nummer_,
              ffmpeg::FehlerText(r).c_str());
    Schliessen();
    return false;
  }
  ctx_->hw_device_ctx = api.av_buffer_ref(hw_device_);
  ctx_->get_format = &HwH264Decoder::FormatWaehlen;
  ctx_->opaque = this;
  // ★ EIN FADEN. Mit VA-API dekodiert die Grafikkarte; FFmpegs Rahmen-Fäden
  // brächten nur Verzögerung (je Faden ein Bild), keine Leistung. Der
  // eingebaute Decoder fährt aus demselben Grund mit einem Faden.
  ctx_->thread_count = 1;
  // ★ Sofort ausgeben: WebRTC-H.264 hat keine B-Bilder, ein Umsortierpuffer
  // hielte nur Bilder zurück, wenn der SPS nichts dazu sagt.
  ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
  // ★ Reines Baseline (Profil 66 ohne constraint_set1) kennt FFmpegs
  // VA-API-Abbildung sonst nicht und ginge still in Software (Prüfbefund N4).
  // WebRTC-Ströme benutzen weder FMO noch ASO — der Unterschied zu
  // Constrained Baseline existiert für uns nicht. Der Vergleichstest prüft
  // bitgleich gegen Software.
  ctx_->hwaccel_flags |= AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH;
  const webrtc::RenderResolution& groesse = settings.max_render_resolution();
  if (groesse.Valid()) {
    ctx_->coded_width = groesse.Width();
    ctx_->coded_height = groesse.Height();
  }

  r = api.avcodec_open2(ctx_, codec, nullptr);
  if (r < 0) {
    Protokoll("Decoder #%d: avcodec_open2: %s — Rückfall auf Software", nummer_,
              ffmpeg::FehlerText(r).c_str());
    Schliessen();
    return false;
  }
  packet_ = api.av_packet_alloc();
  hw_frame_ = api.av_frame_alloc();
  sw_frame_ = api.av_frame_alloc();
  map_frame_ = api.av_frame_alloc();
  {
    const char* weg = std::getenv("HONEYCORD_HWDEC_WEG");
    abbilden_ = !(weg && ::strcasecmp(weg, "holen") == 0);
  }
  if (!packet_ || !hw_frame_ || !sw_frame_ || !map_frame_) {
    Schliessen();
    return false;
  }
  if (std::optional<int> n = settings.buffer_pool_size()) {
    if (!pool_.Resize(*n)) {
      Schliessen();
      return false;
    }
  }
  zeit_start_ms_ = webrtc::TimeMillis();
  Protokoll("Decoder #%d offen (VA-API, höchstens %dx%d, Weg %s)", nummer_,
            groesse.Valid() ? groesse.Width() : 0,
            groesse.Valid() ? groesse.Height() : 0, abbilden_ ? "abbilden" : "holen");
  return true;
}

// static
enum AVPixelFormat HwH264Decoder::FormatWaehlen(
    AVCodecContext* ctx, const enum AVPixelFormat* formate) {
  auto* self = static_cast<HwH264Decoder*>(ctx->opaque);
  for (const enum AVPixelFormat* p = formate; *p != AV_PIX_FMT_NONE; ++p) {
    if (*p == AV_PIX_FMT_VAAPI) {
      self->vaapi_aktiv_ = true;
      return *p;
    }
  }
  // ★ Kein VA-API im Angebot: Entweder kann der Treiber diesen Strom nicht
  // (FFmpeg hat den Beschleuniger beim Aufsetzen verworfen und fragt erneut),
  // oder das Profil ist keins für Hardware. Dann gibt dieser Decoder ab.
  // Ein Format müssen wir trotzdem nennen — das Software-Format, das FFmpeg
  // immer als LETZTES anbietet (davor stehen alle eingebauten
  // Hardware-Formate, die ohne Gerät nur erneut scheitern würden). Das Bild
  // wird nie ausgegeben, weil Decode jetzt FALLBACK_SOFTWARE meldet.
  self->rueckfall_ = true;
  char grund[160];
  std::snprintf(grund, sizeof(grund),
                "kein VA-API-Weg für diesen Strom (%dx%d, Profil %d)",
                ctx->coded_width, ctx->coded_height, ctx->profile);
  self->rueckfall_grund_ = grund;
  const enum AVPixelFormat* letztes = formate;
  while (letztes[0] != AV_PIX_FMT_NONE && letztes[1] != AV_PIX_FMT_NONE) ++letztes;
  return *letztes;
}

int32_t HwH264Decoder::RegisterDecodeCompleteCallback(
    webrtc::DecodedImageCallback* callback) {
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t HwH264Decoder::Release() {
  if (ctx_) {
    Protokoll("Decoder #%d geschlossen", nummer_);
  }
  Schliessen();
  return WEBRTC_VIDEO_CODEC_OK;
}

webrtc::VideoDecoder::DecoderInfo HwH264Decoder::GetDecoderInfo() const {
  DecoderInfo info;
  info.implementation_name = kName;
  info.is_hardware_accelerated = true;
  return info;
}

const char* HwH264Decoder::ImplementationName() const { return kName; }

int32_t HwH264Decoder::Abgeben(const char* grund, int r) {
  rueckfall_ = true;
  rueckfall_grund_ = std::string(grund) + " (" + ffmpeg::FehlerText(r) + ")";
  Protokoll("Decoder #%d: Rückfall auf Software — %s", nummer_, rueckfall_grund_.c_str());
  return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
}

int32_t HwH264Decoder::Fehler(const char* wo, int r) {
  ++fehler_in_folge_;
  ++zeit_fehler_;
  if (fehler_in_folge_ == 1 || fehler_in_folge_ % 10 == 0) {
    Protokoll("Decoder #%d: %s scheitert (%s), %d in Folge", nummer_, wo,
              ffmpeg::FehlerText(r).c_str(), fehler_in_folge_);
  }
  // ★ Einzelne Fehler sind normal (Verlust, fehlende Referenz) — WebRTC
  // fordert dann ein Schlüsselbild an und reicht bis dahin NUR Schlüsselbilder
  // durch. Der Rückfall-Wrapper wechselt selbst nach vier gescheiterten
  // Schlüsselbildern. Diese Grenze ist nur der Gurt dahinter; systematische
  // Fehler (Herunterladen, Format, Maße) geben sofort ab, s. `Abgeben`.
  if (fehler_in_folge_ >= kMaxFehlerInFolge) {
    rueckfall_ = true;
    rueckfall_grund_ = std::string(wo) + " scheitert dauerhaft";
    Protokoll("Decoder #%d: Rückfall auf Software — %s", nummer_,
              rueckfall_grund_.c_str());
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }
  return WEBRTC_VIDEO_CODEC_ERROR;
}

int32_t HwH264Decoder::Decode(const webrtc::EncodedImage& input_image,
                              bool /*missing_frames*/,
                              int64_t /*render_time_ms*/) {
  if (!ctx_ || !callback_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  if (rueckfall_) return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  if (!input_image.data() || input_image.size() == 0 ||
      input_image.size() > static_cast<size_t>(1 << 30)) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  const auto& api = ffmpeg::Zugriff();
  const int64_t t0 = webrtc::TimeMicros();

  packet_->data = const_cast<uint8_t*>(input_image.data());
  packet_->size = static_cast<int>(input_image.size());
  // Die RTP-Zeit reist als pts mit und kommt am Bild wieder heraus — so
  // klebt nie die Zeit eines fremden Bildes daran, falls FFmpeg doch einmal
  // eines zurückhält.
  packet_->pts = input_image.RtpTimestamp();
  int r = api.avcodec_send_packet(ctx_, packet_);
  packet_->data = nullptr;
  packet_->size = 0;
  if (rueckfall_) {
    Protokoll("Decoder #%d: Rückfall auf Software — %s", nummer_,
              rueckfall_grund_.c_str());
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }
  if (r < 0) return Fehler("Dekodieren", r);

  h264_parser_.ParseBitstream(input_image);
  const std::optional<int> qp_roh = h264_parser_.GetLastSliceQp();
  std::optional<uint8_t> qp;
  if (qp_roh && *qp_roh >= 0 && *qp_roh <= 255) qp = static_cast<uint8_t>(*qp_roh);

  bool erstes = true;
  for (;;) {
    r = api.avcodec_receive_frame(ctx_, hw_frame_);
    if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
    if (rueckfall_) {
      api.av_frame_unref(hw_frame_);
      Protokoll("Decoder #%d: Rückfall auf Software — %s", nummer_,
                rueckfall_grund_.c_str());
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }
    if (r < 0) return Fehler("Dekodieren", r);
    if (erstes) {
      zeit_dek_us_ += webrtc::TimeMicros() - t0;
      erstes = false;
    }
    const int32_t e = BildAusliefern(input_image, qp);
    api.av_frame_unref(hw_frame_);
    if (e != WEBRTC_VIDEO_CODEC_OK) return e;
  }
  ZeitenMelden(webrtc::TimeMillis());
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t HwH264Decoder::BildAusliefern(const webrtc::EncodedImage& input_image,
                                      std::optional<uint8_t> qp) {
  const auto& api = ffmpeg::Zugriff();
  if (hw_frame_->format != AV_PIX_FMT_VAAPI) {
    // Sollte nach FormatWaehlen nicht vorkommen — aber ein Software-Bild
    // geben wir grundsätzlich nicht aus (s. Kopfdatei).
    rueckfall_ = true;
    const char* n = api.av_get_pix_fmt_name(static_cast<AVPixelFormat>(hw_frame_->format));
    rueckfall_grund_ = std::string("Bild im Format ") + (n ? n : "?") + " statt VA-API";
    Protokoll("Decoder #%d: Rückfall auf Software — %s", nummer_, rueckfall_grund_.c_str());
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }
  const int w = hw_frame_->width;
  const int h = hw_frame_->height;
  if (w <= 0 || h <= 0) return Abgeben("Bild ohne Maße", 0);

  // ★ Empfang ohne Umweg (10.11.2026): Kann die Anzeige GPU-Bilder
  // übernehmen, bleibt das Bild auf der Grafikkarte (eine GPU-Kopie in einen
  // eigenen Vorrat, ~0,1 ms CPU) — statt 2,3 ms Zurückkopieren plus Umrechnen.
  // Scheitert es, für dieses Bild der heutige Weg.
  if (nullkopie::EmpfangAktiv() && nullkopie::GeraetIstGemeinsam(hw_device_)) {
    const int64_t g0 = webrtc::TimeMicros();
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> gpu = nullkopie::AusDecoder(hw_frame_, w, h);
    if (gpu) {
      webrtc::ColorSpace fr;
      if (input_image.ColorSpace()) {
        fr = *input_image.ColorSpace();
      } else {
        fr.set_primaries_from_uint8(static_cast<uint8_t>(hw_frame_->color_primaries));
        fr.set_transfer_from_uint8(static_cast<uint8_t>(hw_frame_->color_trc));
        fr.set_matrix_from_uint8(static_cast<uint8_t>(hw_frame_->colorspace));
        fr.set_range_from_uint8(static_cast<uint8_t>(hw_frame_->color_range));
      }
      const uint32_t rtp = hw_frame_->pts != AV_NOPTS_VALUE ? static_cast<uint32_t>(hw_frame_->pts)
                                                            : input_image.RtpTimestamp();
      webrtc::VideoFrame bild = webrtc::VideoFrame::Builder()
                                    .set_video_frame_buffer(gpu)
                                    .set_rtp_timestamp(rtp)
                                    .set_color_space(fr)
                                    .build();
      callback_->Decoded(bild, std::nullopt, qp);
      if (!gpu_gemeldet_) {
        Protokoll("Decoder #%d: Bilder bleiben auf der Grafikkarte (%dx%d)", nummer_, w, h);
        gpu_gemeldet_ = true;
      }
      if (!gemeldet_) {
        Protokoll("Decoder #%d: VA-API dekodiert (%dx%d, Profil %d)", nummer_, w, h, ctx_->profile);
        gemeldet_ = true;
      }
      fehler_in_folge_ = 0;
      zeit_holen_us_ += webrtc::TimeMicros() - g0;
      ++zeit_bilder_;
      ++gpu_bilder_;
      breite_ = w;
      hoehe_ = h;
      ZeitenMelden(webrtc::TimeMillis());
      return WEBRTC_VIDEO_CODEC_OK;
    }
  }

  // ★ Den Zwischenpuffer behalten, solange die Maße gleich bleiben: Mit
  // gefülltem buf[0] lädt FFmpeg direkt hinein, statt je Bild ~3 MB (1080p)
  // neu anzufordern.
  webrtc::scoped_refptr<webrtc::I420Buffer> puffer = pool_.CreateI420Buffer(w, h);
  if (!puffer) return Fehler("Bildvorrat erschöpft", 0);

  const int64_t t1 = webrtc::TimeMicros();
  int64_t t2 = t1;
  bool fertig = false;
  if (abbilden_) {
    fertig = Abbilden(puffer.get(), w, h);
    t2 = webrtc::TimeMicros();
    if (!fertig) {
      abbilden_ = false;
      Protokoll("Decoder #%d: Abbilden geht hier nicht — Weg holen", nummer_);
    }
  }
  if (!fertig) {
    // ★ Weg „holen": den Zwischenpuffer behalten, solange die Maße gleich
    // bleiben — mit gefülltem buf[0] lädt FFmpeg direkt hinein, statt je
    // Bild ~3 MB (1080p) neu anzufordern.
    if (sw_frame_->buf[0] &&
        (sw_frame_->width != w || sw_frame_->height != h ||
         sw_frame_->format != AV_PIX_FMT_NV12)) {
      api.av_frame_unref(sw_frame_);
    }
    if (!sw_frame_->buf[0]) {
      sw_frame_->format = AV_PIX_FMT_NV12;
      sw_frame_->width = w;
      sw_frame_->height = h;
    }
    int r = api.av_hwframe_transfer_data(sw_frame_, hw_frame_, 0);
    if (r < 0 || sw_frame_->format != AV_PIX_FMT_NV12) {
      api.av_frame_unref(sw_frame_);
      // Systematisch: Gelingt das einmal nicht, gelingt es beim nächsten Bild
      // auch nicht — sofort abgeben statt Sekunden ohne Bild.
      return Abgeben("Herunterladen scheitert", r);
    }
    t2 = webrtc::TimeMicros();
    libyuv::NV12ToI420(sw_frame_->data[0], sw_frame_->linesize[0],
                       sw_frame_->data[1], sw_frame_->linesize[1],
                       puffer->MutableDataY(), puffer->StrideY(),
                       puffer->MutableDataU(), puffer->StrideU(),
                       puffer->MutableDataV(), puffer->StrideV(), w, h);
  }
  const int64_t t3 = webrtc::TimeMicros();

  // ★ Farbraum NICHT über `ExtractH264ColorSpace` (h264_color_space.h): Die
  // Funktion ist gegen das in WebRTC eingebaute FFmpeg gebaut und liest
  // dessen Strukturlage — unser Kontext stammt aus dem System-FFmpeg 7.1.
  // Die Kennzahlen selbst sind in beiden die aus H.273, also direkt übertragbar.
  webrtc::ColorSpace farbraum;
  if (input_image.ColorSpace()) {
    farbraum = *input_image.ColorSpace();
  } else {
    farbraum.set_primaries_from_uint8(static_cast<uint8_t>(hw_frame_->color_primaries));
    farbraum.set_transfer_from_uint8(static_cast<uint8_t>(hw_frame_->color_trc));
    farbraum.set_matrix_from_uint8(static_cast<uint8_t>(hw_frame_->colorspace));
    farbraum.set_range_from_uint8(static_cast<uint8_t>(hw_frame_->color_range));
  }

  const uint32_t rtp = hw_frame_->pts != AV_NOPTS_VALUE
                           ? static_cast<uint32_t>(hw_frame_->pts)
                           : input_image.RtpTimestamp();
  webrtc::VideoFrame bild = webrtc::VideoFrame::Builder()
                                .set_video_frame_buffer(puffer)
                                .set_rtp_timestamp(rtp)
                                .set_color_space(farbraum)
                                .build();
  callback_->Decoded(bild, std::nullopt, qp);

  if (!gemeldet_) {
    Protokoll("Decoder #%d: VA-API dekodiert (%dx%d, Profil %d)", nummer_, w, h,
              ctx_->profile);
    gemeldet_ = true;
  }
  fehler_in_folge_ = 0;
  zeit_holen_us_ += t2 - t1;
  zeit_umformen_us_ += t3 - t2;
  ++zeit_bilder_;
  breite_ = w;
  hoehe_ = h;
  return WEBRTC_VIDEO_CODEC_OK;
}

// ★ WARUM ABBILDEN (07.11.2026, gemessen): Mit `av_hwframe_transfer_data`
// kostete ein 1080p-Bild 3,2 ms (AMD 780M) bzw. ~4 ms CPU (UHD 630) — fast
// alles im Kopieren. FFmpeg kopiert dort mit gewöhnlichem memcpy aus der
// abgebildeten Oberfläche; die liegt aber in nicht zwischengespeichertem
// (write-combined) Speicher, aus dem gewöhnliche Lesezugriffe sehr langsam
// sind. `av_image_copy_plane_uc_from` liest mit Streaming-Loads (SSE4.1
// MOVNTDQA) — genau für diesen Speicher gebaut. Die Helligkeitsebene geht
// so direkt in den I420-Puffer, die Farbebene über einen kleinen
// zwischengespeicherten Puffer und libyuv in U und V.
bool HwH264Decoder::Abbilden(webrtc::I420Buffer* puffer, int w, int h) {
  const auto& api = ffmpeg::Zugriff();
  api.av_frame_unref(map_frame_);
  map_frame_->format = AV_PIX_FMT_NV12;
  int r = api.av_hwframe_map(map_frame_, hw_frame_, AV_HWFRAME_MAP_READ);
  if (r < 0 || map_frame_->format != AV_PIX_FMT_NV12 || !map_frame_->data[0] ||
      !map_frame_->data[1]) {
    if (r < 0) {
      Protokoll("Decoder #%d: av_hwframe_map: %s", nummer_, ffmpeg::FehlerText(r).c_str());
    }
    api.av_frame_unref(map_frame_);
    return false;
  }
  api.av_image_copy_plane_uc_from(puffer->MutableDataY(), puffer->StrideY(),
                                  map_frame_->data[0], map_frame_->linesize[0], w, h);
  const int cw = (w + 1) / 2;
  const int ch = (h + 1) / 2;
  const size_t uv_zeile = static_cast<size_t>(cw) * 2;
  uv_zwischen_.resize(uv_zeile * ch);
  api.av_image_copy_plane_uc_from(uv_zwischen_.data(), uv_zeile, map_frame_->data[1],
                                  map_frame_->linesize[1], uv_zeile, ch);
  api.av_frame_unref(map_frame_);
  libyuv::SplitUVPlane(uv_zwischen_.data(), static_cast<int>(uv_zeile),
                       puffer->MutableDataU(), puffer->StrideU(),
                       puffer->MutableDataV(), puffer->StrideV(), cw, ch);
  return true;
}

void HwH264Decoder::ZeitenMelden(int64_t jetzt_ms) {
  if (jetzt_ms - zeit_start_ms_ < 30000) return;
  if (zeit_bilder_ > 0 || zeit_fehler_ > 0) {
    const double n = zeit_bilder_ > 0 ? zeit_bilder_ : 1;
    Protokoll("Decoder #%d %dx%d: %d Bilder/%lld s (davon %d auf der Grafikkarte) · "
              "dekodieren %.2f ms · herunterladen %.2f ms · umformen %.2f ms · Fehler %d",
              nummer_, breite_, hoehe_, zeit_bilder_,
              static_cast<long long>((jetzt_ms - zeit_start_ms_) / 1000), gpu_bilder_,
              zeit_dek_us_ / n / 1000.0, zeit_holen_us_ / n / 1000.0,
              zeit_umformen_us_ / n / 1000.0, zeit_fehler_);
  }
  zeit_dek_us_ = zeit_holen_us_ = zeit_umformen_us_ = 0;
  zeit_bilder_ = zeit_fehler_ = 0;
  gpu_bilder_ = 0;
  zeit_start_ms_ = jetzt_ms;
}

}  // namespace libwebrtc
