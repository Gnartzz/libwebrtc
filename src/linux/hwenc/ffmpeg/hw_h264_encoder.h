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
  // VA-API mit neuer Rate/Bildrate öffnen (unter `mutex_`). `wie` erscheint
  // im hwenc.log („" oder „nachgeholt ").
  // `unterschuss_gesendet` > 0: Auslöser ist der Unterschuss-Ausgleich
  // (hwenc.11), der Wert ist die gemessene Ausgaberate fürs Protokoll.
  void VaapiNeuAufsetzen(uint32_t neu, uint32_t gefordert, bool hoch, bool runter,
                         bool bildrate, bool folge, uint32_t fz, int64_t jetzt,
                         const char* wie, uint32_t unterschuss_gesendet);
  // ★★ 23.10.2026 (hwenc.10): wertet die zeitbasierten Folgen aus — aus
  // `SetRates` UND aus `Encode` (unter `mutex_`, nur VA-API). Rechnet nur mit
  // Zahlen, die schon da sind; keine Allokation, kein Treiberaufruf. Schreibt
  // höchstens alle 30 s eine Diagnosezeile.
  // ★★ hwenc.11: zusätzlich das Ausgabefenster fortschreiben und den
  // Unterschuss werten (`unterschuss`).
  void LageAuswerten(int64_t jetzt, bool* hoch_folge, bool* bildrate,
                     bool* unterschuss);
  // hwenc.11: Ziel für den Unterschuss-Ausgleich —
  // min(Forderung, Ziel × Ziel / gesendet), mindestens 1,1× Ziel, nie über
  // der rohen Forderung. Nur sinnvoll, wenn `LageAuswerten` Unterschuss meldet.
  uint32_t UnterschussZiel() const;
  // hwenc.11: Ausgabefenster leeren (nach jedem Öffnen — die alten Bytes
  // gehören zu einem anderen Ziel).
  void AusgabeFensterLeeren();
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
  // ★ 25.09.2026: die Bildrate, mit der der Encoder GEÖFFNET wurde. VA-API
  // verteilt die CBR-Rate auf genau diese Bildrate — kommen weniger Bilder,
  // geht nur ein Bruchteil der Rate raus (Haltoan: 947 kbit/s bei @60
  // geöffnet, ~22 fps geliefert → 300–400 kbit/s gesendet).
  uint32_t framerate_offen_ = 30;
  // maxFramerate aus InitEncode — Obergrenze der Klemme, damit eine 15-fps-
  // Kamera nicht auf 20 hochgezogen wird (Prüfbefund M3).
  uint32_t max_fps_ = 30;
  // Die ROHE Forderung von WebRTC beim letzten Öffnen. Der Ausgleicher
  // schwankt von selbst zwischen 0,5× und 0,95× — auf ihn bezogen wäre eine
  // 1,5×-Schwelle ein neuer Kreis (Prüfbefund M1).
  uint32_t gefordert_offen_ = 0;
  // In welche Richtung die gemeldete Bildrate von der beim Öffnen abweicht
  // (+1 höher, −1 niedriger, 0 nicht) und seit wann durchgehend. Bis
  // hwenc.9 zählte hier „drei SetRates in Folge" (Prüfbefund M2) — seit
  // hwenc.10 gilt die ZEIT (3 s), s. `LageAuswerten`.
  int bildrate_richtung_ = 0;
  int64_t bildrate_seit_ms_ = 0;
  // ★★ 23.10.2026 (hwenc.10, jimtonics): Die letzte ROHE Forderung und die
  // letzte (geklemmte) Bildrate aus `SetRates` — damit `Encode` die Folgen
  // auch dann auswerten kann, wenn WebRTC eine Weile nichts Neues meldet.
  uint32_t forderung_roh_ = 0;
  uint32_t fz_zuletzt_ = 30;
  // Seit wann die rohe Forderung durchgehend ≥ 1,2× `gefordert_offen_` liegt
  // (0 = nicht).
  int64_t hoch_seit_ms_ = 0;
  // Diagnose „Encoder haengt": seit wann Forderung und Encoder-Ziel um mehr
  // als 15 % auseinanderliegen, und wann zuletzt gemeldet.
  int64_t haengt_seit_ms_ = 0;
  int64_t haengt_gemeldet_ms_ = 0;
  // So lange muss eine Abweichung durchgehend bestehen, bevor sie zählt.
  static constexpr int64_t kFolgeMs = 3000;
  // ★★ 23.10.2026 (hwenc.11, jimtonics nach dem Netzstillstand 22:51): Was
  // der Encoder WIRKLICH ausgibt — fünf Sekundenfächer (Bytes und Bilder),
  // gefüllt in `PaketeAbholen`, fortgeschrieben in `LageAuswerten`. Feste
  // Felder, keine Allokation. `gesendet_bps_` = 0 heißt „Fenster noch nicht
  // voll" (nach jedem Öffnen fünf Sekunden).
  static constexpr int kAusFaecher = 5;
  static constexpr int64_t kUnterschussMs = 5000;
  uint64_t aus_bytes_[kAusFaecher] = {};
  uint32_t aus_bilder_[kAusFaecher] = {};
  uint64_t aus_bytes_jetzt_ = 0;
  uint32_t aus_bilder_jetzt_ = 0;
  int aus_index_ = 0;
  int aus_volle_ = 0;
  int64_t aus_beginn_ms_ = 0;
  uint32_t gesendet_bps_ = 0;
  uint32_t gesendet_bilder_5s_ = 0;
  // Seit wann die Unterschuss-Lage durchgehend besteht (0 = nicht).
  int64_t unterschuss_seit_ms_ = 0;
  // Wann zuletzt „runter" neu aufgesetzt wurde (0 = nie). Danach ist die
  // Unterschuss-Hebung kNachRunterMs gesperrt — die Schätzung springt nach
  // einem Stau noch (Prüfbefund M2).
  static constexpr int64_t kNachRunterMs = 20000;
  int64_t runter_ms_ = 0;
  // Zeitmessung je Phase (Mikrosekunden), alle fünf Sekunden ins hwenc.log.
  int64_t zeit_i420_us_ = 0;
  int64_t zeit_nv12_us_ = 0;
  int64_t zeit_hoch_us_ = 0;
  int64_t zeit_kod_us_ = 0;
  int zeit_bilder_ = 0;
  int64_t zeit_start_ms_ = 0;
  int width_ = 0;
  int height_ = 0;
  bool bildschirm_ = false;
  int64_t frame_index_ = 0;
  std::map<int64_t, Stempel> stempel_;

  // Neu aufsetzen bei Ratenwechsel kostet ein Keyframe — deshalb nur bei
  // echten Sprüngen und nicht öfter als alle paar Sekunden.
  int64_t letztes_aufsetzen_ms_ = 0;
  // ★ 17.10.2026: Anlaufphase nach InitEncode — aufwärts schneller neu aufsetzen.
  static constexpr int64_t kAnlaufMs = 8000;
  int64_t anlauf_bis_ms_ = 0;
  // Intel-iHD erkannt (Qualitätsstufe 1).
  bool intel_ = false;
  // ★★ 09.10.2026 (Haltoan, „720p mit 0,1 Mbit", hwenc.log Bündel 85): Ein
  // Sprung, der in die Sperrzeit fiel, wurde VERWORFEN. Fragt WebRTC danach
  // nicht mehr nach (Rate konstant: kein weiteres SetRates), blieb der
  // Encoder für immer auf der Startrate — geöffnet mit 290 kbit/s, gefordert
  // 4000. Jetzt wird der Sprung gemerkt und in `Encode` nachgeholt, sobald
  // die Sperrzeit um ist.
  bool aufsetzen_ausstehend_ = false;
  uint32_t ausstehend_gefordert_ = 0;
  uint32_t ausstehend_fz_ = 30;
  bool ausstehend_hoch_ = false;
  bool ausstehend_runter_ = false;
  bool ausstehend_bildrate_ = false;
  bool ausstehend_folge_ = false;
  // Wahr, wenn der gemerkte Sprung NUR aus den zeitbasierten Folgen stammt
  // (kein 1,5×-Sprung, kein Abwärts; seit hwenc.11 zählt der Unterschuss
  // dazu) — dann verfällt er auch in `Encode`, sobald die Folge abreißt.
  bool ausstehend_nur_folge_ = false;
  int64_t ausstehend_sperre_ms_ = 10000;
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
