#ifndef LIBWEBRTC_LINUX_HWENC_HW_H264_DECODER_H_
#define LIBWEBRTC_LINUX_HWENC_HW_H264_DECODER_H_

// HoneyCords H.264-Decoder für Linux: FFmpegs eingebauter H.264-Decoder mit
// VA-API als Beschleuniger (hwaccel), zur Laufzeit aus dem System-FFmpeg
// geladen wie der Encoder.
//
// ★ WARUM (07.11.2026, Jim, Bündel 119/120, Meteor Lake): Linux dekodierte
// bis hierhin ALLES in Software (`decoder=FFmpeg`, Last 3,1 bei vier Kernen).
// Jede fremde Kamera und jede Freigabe kostet so CPU, die auf einem Laptop
// oder dem Deck Akku und Takt frisst. Die Grafikkarte hat dafür eigene
// Schaltkreise.
//
// ★ ERSTER SCHRITT, BEWUSST MIT KOPIE: Das fertige Bild liegt im
// Grafikspeicher (NV12). Der Renderer des Clients braucht I420 im
// Arbeitsspeicher (`ConvertToARGB` → `I420Buffer::Rotate`), also wird es
// heruntergeladen und umgeformt. Das kostet zwei Durchläufe über das Bild —
// bei 1080p deutlich weniger als das Dekodieren selbst. Null-Kopie bis in die
// Textur des Clients ist eine eigene Runde und braucht erst Messwerte.
//
// ★ NUR VA-API-BILDER VERLASSEN DIESEN DECODER. Bietet FFmpeg für einen Strom
// keinen VA-API-Weg an (Auflösung über der Grenze des Treibers, z. B. 5120
// Pixel breit; Profil, das der Treiber nicht kann), meldet der Decoder
// `FALLBACK_SOFTWARE`, und der Rückfall-Wrapper von WebRTC übernimmt mit dem
// eingebauten Software-Decoder — demselben Weg wie bisher. Wir dekodieren also
// nie selbst in Software, sondern geben ab.
//
// ★ ABSCHALTBAR: Umgebungsvariable `HONEYCORD_HWDEC=0` (oder `aus`) — dann
// bleibt alles wie vor diesem Decoder. Für Vergleichsmessungen gedacht.

#include <cstdint>
#include <string>

#include "api/video/encoded_image.h"
#include "api/video_codecs/video_decoder.h"
#include "common_video/h264/h264_bitstream_parser.h"
#include "common_video/include/video_frame_buffer_pool.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "src/linux/hwenc/ffmpeg/ffmpeg_lader.h"

namespace libwebrtc {

class HwH264Decoder : public webrtc::VideoDecoder {
 public:
  // Wahr, wenn FFmpeg lädt, der H.264-Decoder einen VA-API-Weg kennt, sich
  // das VA-API-Gerät öffnen lässt und niemand `HONEYCORD_HWDEC=0` gesetzt
  // hat. Einmal geprüft und gemerkt. Ob der Treiber den konkreten Strom
  // kann, zeigt sich erst beim ersten Bild — dafür gibt es den Rückfall.
  static bool IstVerfuegbar();

  HwH264Decoder();
  ~HwH264Decoder() override;

  bool Configure(const Settings& settings) override;
  int32_t Decode(const webrtc::EncodedImage& input_image,
                 bool missing_frames,
                 int64_t render_time_ms) override;
  int32_t RegisterDecodeCompleteCallback(
      webrtc::DecodedImageCallback* callback) override;
  int32_t Release() override;
  DecoderInfo GetDecoderInfo() const override;
  const char* ImplementationName() const override;

 private:
  // FFmpegs Formatwahl. Nimmt VA-API, sobald es angeboten wird; sonst merkt
  // sie sich den Rückfall und nimmt das erste Software-Format (das Bild wird
  // nie ausgegeben, Decode meldet FALLBACK_SOFTWARE).
  static enum AVPixelFormat FormatWaehlen(AVCodecContext* ctx,
                                          const enum AVPixelFormat* formate);
  // Ein fertiges VA-API-Bild herunterladen, umformen, ausliefern.
  int32_t BildAusliefern(const webrtc::EncodedImage& input_image,
                         std::optional<uint8_t> qp);
  // Zählt einen Fehler; liefert FALLBACK_SOFTWARE, wenn es zu viele in Folge
  // waren, sonst ERROR (WebRTC fordert dann ein Schlüsselbild an).
  int32_t Fehler(const char* wo, int r);
  void ZeitenMelden(int64_t jetzt_ms);
  void Schliessen();

  const int nummer_;  // für das Protokoll: welche Empfangsspur
  webrtc::DecodedImageCallback* callback_ = nullptr;
  webrtc::H264BitstreamParser h264_parser_;
  webrtc::VideoFrameBufferPool pool_;

  AVCodecContext* ctx_ = nullptr;
  AVBufferRef* hw_device_ = nullptr;
  AVPacket* packet_ = nullptr;
  AVFrame* hw_frame_ = nullptr;
  AVFrame* sw_frame_ = nullptr;

  // Von FormatWaehlen gesetzt (läuft innerhalb von send_packet/receive_frame
  // auf demselben Faden wie Decode — keine Sperre nötig).
  bool rueckfall_ = false;
  bool vaapi_aktiv_ = false;
  std::string rueckfall_grund_;

  int fehler_in_folge_ = 0;
  static constexpr int kMaxFehlerInFolge = 30;

  // Zeitmessung je Phase (µs), alle 30 s ins hwenc.log.
  int64_t zeit_dek_us_ = 0;
  int64_t zeit_holen_us_ = 0;
  int64_t zeit_umformen_us_ = 0;
  int zeit_bilder_ = 0;
  int zeit_fehler_ = 0;
  int64_t zeit_start_ms_ = 0;
  int breite_ = 0;
  int hoehe_ = 0;
};

}  // namespace libwebrtc

#endif  // LIBWEBRTC_LINUX_HWENC_HW_H264_DECODER_H_
