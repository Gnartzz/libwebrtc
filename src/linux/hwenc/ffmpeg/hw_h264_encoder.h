#ifndef LIBWEBRTC_LINUX_HWENC_HW_H264_ENCODER_H_
#define LIBWEBRTC_LINUX_HWENC_HW_H264_ENCODER_H_

// HoneyCords H.264-Encoder für Linux auf Basis von VA-API — über FFmpegs
// `h264_vaapi`.
//
// ★ WARUM FFMPEG UND NICHT libva DIREKT. Anders als AMF oder NVENC ist VA-API
// keine Encoder-Schnittstelle, sondern eine dünne Befehlsschicht. Wer sie
// direkt bedient, baut SPS/PPS von Hand, füllt Slice-Parameter und schreibt
// die Ratensteuerung selbst — rund zweitausend Zeilen, deren Fehler sich beim
// Gegenüber als zerfallendes Bild zeigen. FFmpegs `h264_vaapi` erledigt genau
// das, liegt auf jeder Desktop-Distribution und wird von um Größenordnungen
// mehr Leuten benutzt, als wir je erreichen. Gemessen am 19.09.2026: Der UM890
// (Radeon 780M, Mesa 25.1.9) bietet `VAEntrypointEncSlice` für H.264
// Baseline/Main/High, und libavcodec.so.61 trägt `h264_vaapi`. Dasselbe gilt
// auf jedem Intel- und (mit dem offenen Treiber) auch auf NVIDIA-System — EIN
// Encoder deckt damit alle drei Hersteller ab.
//
// ★ ZUR LAUFZEIT GELADEN, nie gebunden: siehe `ffmpeg_lader.h`. Fehlende
// Bibliothek, fehlender Encoder, fehlendes /dev/dri, falsche Hauptversion —
// jeder dieser Wege meldet schlicht „nicht unterstützt", und die Fabrik fällt
// auf OpenH264 zurück. Ein Client, der nicht startet, weil eine Codec-
// Bibliothek eine Fassung zurückliegt, wäre schlimmer als einer, der in
// Software kodiert.

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "modules/video_coding/codecs/h264/include/h264_globals.h"
#include "common_video/h264/h264_bitstream_parser.h"
#include "common_video/include/bitrate_adjuster.h"
#include "src/linux/hwenc/ffmpeg/ffmpeg_lader.h"

namespace libwebrtc {

class HwH264Encoder : public webrtc::VideoEncoder {
 public:
  // Welcher Weg in die Hardware führt.
  //
  // ★ EIN ENCODER FÜR ALLE DREI HERSTELLER, weil FFmpeg die Unterschiede
  // schon gekapselt hat: AMD und Intel über `h264_vaapi`, NVIDIA über
  // `h264_nvenc`. Der Unterschied im Code beschränkt sich auf den
  // Encoder-Namen, ein paar Optionen und die Frage, ob wir das Bild selbst in
  // den Grafikspeicher legen müssen (VA-API: ja; NVENC lädt selbst hoch).
  //
  // ★ WARUM NVENC NICHT ÜBER VA-API MITLÄUFT: NVIDIAs VA-API-Treiber kann nur
  // DEKODIEREN. Wer auf einer NVIDIA-Karte kodieren will, muss NVENC nehmen —
  // über VA-API käme dort gar nichts.
  enum class Backend { kVaapi, kNvenc };

  // Wahr, wenn FFmpeg in der passenden Fassung lädt, einer der beiden Encoder
  // existiert UND sich das zugehörige Gerät wirklich öffnen lässt. Wird einmal
  // geprüft und gemerkt — die Antwort kann sich im laufenden Prozess nicht
  // ändern.
  static bool IstVerfuegbar();

  // Der gefundene Weg. Nur gültig, wenn IstVerfuegbar() wahr ist.
  static Backend GewaehlterWeg();

  // Was im Fuß des Clients steht, z. B. „NVENC (libavcodec.so.61)".
  // Leer, wenn nicht verfügbar.
  static std::string BackendName();

  explicit HwH264Encoder(const webrtc::SdpVideoFormat& format);
  ~HwH264Encoder() override;

  int InitEncode(const webrtc::VideoCodec* codec_settings,
                 const webrtc::VideoEncoder::Settings& settings) override;
  int32_t RegisterEncodeCompleteCallback(
      webrtc::EncodedImageCallback* callback) override;
  int32_t Release() override;
  int32_t Encode(
      const webrtc::VideoFrame& frame,
      const std::vector<webrtc::VideoFrameType>* frame_types) override;
  void SetRates(
      const webrtc::VideoEncoder::RateControlParameters& parameters) override;
  webrtc::VideoEncoder::EncoderInfo GetEncoderInfo() const override;

 private:
  int32_t CodecOeffnen();
  void CodecSchliessen();
  // Holt jedes fertige Paket ab und reicht es an den Rückruf weiter. Die
  // Zeitangaben kommen aus `stempel_`, NICHT vom gerade übergebenen Bild —
  // siehe dort.
  void PaketeAbholen();

  // Was ein ausgehendes Paket an Zeitangaben braucht. Wird beim Hineingeben
  // unter der `pts` abgelegt und beim Herauskommen wieder herausgesucht.
  //
  // ★ WARUM NICHT EINFACH DAS AKTUELLE BILD NEHMEN (Prüfbefund 19.09.2026):
  // Solange der Encoder jedes Bild sofort zurückgibt, wäre das dasselbe. Gibt
  // er aber eines später zurück — bei `EAGAIN`, bei einer Treiber-Warteschlange
  // oder wenn jemand `async_depth` erhöht —, klebte am Paket der Zeitstempel
  // eines FREMDEN Bildes. Das verschiebt Ton gegen Bild und die
  // Jitter-Schätzung, und zwar unauffällig.
  struct Stempel {
    uint32_t rtp = 0;
    int64_t ntp = 0;
    int64_t render = 0;
    webrtc::VideoRotation rotation = webrtc::kVideoRotation_0;
  };

  std::mutex mutex_;
  webrtc::EncodedImageCallback* callback_ = nullptr;
  webrtc::H264BitstreamParser h264_parser_;
  // ★ Derselbe Grund wie unter Windows: Hardware-Encoder treffen die
  // gewünschte Rate nicht genau, und WebRTCs Staukontrolle glaubt der Zahl,
  // die sie bestellt hat. Der Ausgleicher meldet zurück, was wirklich kam.
  webrtc::BitrateAdjuster bitrate_adjuster_;
  webrtc::EncodedImage encoded_image_;

  Backend weg_ = Backend::kVaapi;
  int profil_ = 0;  // AV_PROFILE_H264_*, aus dem ausgehandelten SDP-Format
  webrtc::H264PacketizationMode packetization_mode_ =
      webrtc::H264PacketizationMode::NonInterleaved;

  uint32_t target_bitrate_bps_ = 0;
  uint32_t max_bitrate_bps_ = 0;
  uint32_t framerate_ = 30;
  int width_ = 0;
  int height_ = 0;
  bool bildschirm_ = false;
  int64_t frame_index_ = 0;
  std::map<int64_t, Stempel> stempel_;

  // Neu aufsetzen bei Ratenwechsel kostet ein Keyframe — deshalb nur bei
  // echten Sprüngen und nicht öfter als alle paar Sekunden.
  int64_t letztes_aufsetzen_ms_ = 0;
  // Nach einem gescheiterten Öffnen nicht endgültig aufgeben, sondern es
  // gelegentlich erneut versuchen (der Grund kann vorübergehend sein:
  // Gerät belegt, Suspend, VT-Wechsel).
  bool oeffnen_scheiterte_ = false;
  int misslungene_versuche_ = 0;
  int64_t wiederanlauf_zaehler_ = 0;
  // Einmalige Warnung, wenn ein angefordertes Keyframe nicht als solches
  // zurückkam — die billigste Messung für „greift pict_type wirklich?".
  bool keyframe_angefordert_ = false;
  bool keyframe_warnung_ = false;

  AVCodecContext* ctx_ = nullptr;
  AVBufferRef* hw_device_ = nullptr;
  AVBufferRef* hw_frames_ = nullptr;
  AVFrame* sw_frame_ = nullptr;
  AVFrame* hw_frame_ = nullptr;
  AVPacket* packet_ = nullptr;
};

}  // namespace libwebrtc

#endif  // LIBWEBRTC_LINUX_HWENC_HW_H264_ENCODER_H_
