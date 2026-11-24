// Prüfprogramm Neuaufsetzen des VA-API-Encoders unter GPU-Last (23.11.2026).
//
// Anlass: V3 Linux 2.6.364 (hwenc.18) mit echter Kamera (CPU-Bilder, 720p,
// 15–21 Bilder/s) setzt den VA-API-Encoder ~2 s nach dem Start neu auf
// (2000 -> 4000 kbit/s, @60 -> @21). Unter GPU-Last (sichtbares GL-Fenster +
// HEVC-Encode daneben) kam genau dann ein VCN-Seitenfehler, Ring-Timeout,
// Mesa-Abbruch („context is lost") — und ein zweiter Timeout traf einen
// fremden Prozess (Sunshine).
//
// Das Programm bildet die Lage nach: ein (oder mehrere) Kamera-Encoder mit
// CPU-Bildern im Takt einer Kamera, erzwungenes Neuaufsetzen alle N Bilder,
// wahlweise GL-Last IM SELBEN PROZESS (wie Qt Quick in honeycord-nativ, über
// eine per dlopen geladene libgllast.so — gleiche Mesa-Instanz wie VA-API).
//
// Bauen:   ninja -C out/Release-x64 libwebrtc:neuaufsetzen_test
// Aufruf:  neuaufsetzen_test [--weg init|groesse|rate|keiner] [--alle N]
//            [--takt 20] [--dauer 60] [--breite 1280] [--hoehe 720]
//            [--fps 60] [--kbit 2000] [--encoder 1] [--gllast pfad.so]
//            [--glstufe 3] [--geteilt] [--aus datei.h264] [--bilder N]
//
//   --weg init     InitEncode erneut (Rate/Bildrate wechseln 2000@60 <-> 4000@21),
//                  derselbe Pfad wie das Neuaufsetzen nach Ratenwechsel
//                  (CodecOeffnen -> CodecSchliessen -> neu öffnen).
//   --weg groesse  Bildgröße wechselt (1280x720 <-> 1280x704) -> Encode setzt neu auf.
//   --weg rate     nur SetRates-Sprünge; ob/wann neu aufgesetzt wird, entscheidet
//                  die echte Ratenlogik (steht im hwenc.log).
//   --weg release  Release() + InitEncode() + Rückruf neu (Kamera aus/an).
//   --weg instanz  Encoder-Objekt zerstören und neu anlegen.
//   --weg keiner   kein Neuaufsetzen (Vergleich, Bitgleichheit).
//   --bilder N     statt --dauer genau N Bilder je Encoder (Bitgleichheit).
//   --bildschirm   Bilder tragen die Kennung des Bildschirm-Aufnehmers
//                  (nullkopie::kBildschirmBildId) — auf AMD sonst Kamera → Software
//                  (hwenc.19-Übergang: Encode liefert FALLBACK_SOFTWARE).
//   --erwarte-software  bestanden, wenn JEDER Encoder beim ersten Bild auf
//                  Software zurückfällt und keine VA-Sitzung öffnet.
//
// Rückgabe 0 = bestanden: jedes Bild angenommen, nach jedem Neuaufsetzen zuerst
// ein Schlüsselbild, kein Hänger (> 5 s ohne Ausgabe). „ohne Paket" zählt
// Bilder, deren Ausgabe beim Schließen verworfen wurde (nur Angabe).

#include <dlfcn.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "api/environment/environment_factory.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_encoder.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "src/linux/hwenc/ffmpeg/hw_h264_encoder.h"
#include "src/linux/hwenc/ffmpeg/hw_nullkopie.h"

namespace {

double JetztMs() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

struct Optionen {
  std::string weg = "init";
  int alle = 40;
  int takt = 20;
  int dauer = 60;
  int breite = 1280, hoehe = 720, fps = 60, kbit = 2000;
  int encoder = 1;
  std::string gllast;
  int glstufe = 3;
  bool geteilt = false;
  std::string aus;
  int bilder = 0;
  bool bildschirm = false;
  bool erwarte_software = false;
};

std::atomic<bool> g_halt{false};
std::atomic<double> g_letzte_ausgabe_ms{0};

struct Sammler : webrtc::EncodedImageCallback {
  FILE* datei = nullptr;
  std::atomic<int> pakete{0}, schluessel{0};
  std::atomic<uint64_t> bytes{0};
  // Nach einem Neuaufsetzen muss das nächste Paket ein Schlüsselbild sein.
  std::atomic<bool> warte_schluessel{true};
  std::atomic<int> ohne_schluessel{0};
  Result OnEncodedImage(const webrtc::EncodedImage& img, const webrtc::CodecSpecificInfo*) override {
    if (datei) std::fwrite(img.data(), 1, img.size(), datei);
    bytes += img.size();
    const bool key = img._frameType == webrtc::VideoFrameType::kVideoFrameKey;
    if (key) ++schluessel;
    if (warte_schluessel.exchange(false) && !key) ++ohne_schluessel;
    ++pakete;
    g_letzte_ausgabe_ms = JetztMs();
    return Result(Result::OK);
  }
};

// Kamera-ähnliches Bild: Verlauf, der wandert, plus Rauschen (nicht trivial
// zu kodieren). Deterministisch aus der Bildnummer.
void Malen(webrtc::I420Buffer& b, int n) {
  const int w = b.width(), h = b.height();
  uint32_t zufall = 0x9e3779b9u * static_cast<uint32_t>(n + 1);
  for (int y = 0; y < h; ++y) {
    uint8_t* z = b.MutableDataY() + y * b.StrideY();
    for (int x = 0; x < w; ++x) {
      zufall = zufall * 1664525u + 1013904223u;
      z[x] = static_cast<uint8_t>(((x + n * 3) ^ (y + n)) + (zufall >> 29));
    }
  }
  for (int y = 0; y < (h + 1) / 2; ++y) {
    uint8_t* u = b.MutableDataU() + y * b.StrideU();
    uint8_t* v = b.MutableDataV() + y * b.StrideV();
    for (int x = 0; x < (w + 1) / 2; ++x) {
      u[x] = static_cast<uint8_t>(128 + ((x + n) % 64) - 32);
      v[x] = static_cast<uint8_t>(128 + ((y - n) % 64) - 32);
    }
  }
}

struct Ergebnis {
  int bilder = 0, fehler = 0, neu = 0, neu_fehler = 0;
  double neu_ms_summe = 0, neu_ms_max = 0;
  int pakete = 0, schluessel = 0, ohne_schluessel = 0;
  uint64_t bytes = 0;
  int software_bei = -1;   // Bildnummer des Software-Rückfalls, -1 = keiner
};

Ergebnis EncoderLauf(const Optionen& o, int nummer) {
  Ergebnis e;
  auto Neu = [] {
    return std::make_unique<libwebrtc::HwH264Encoder>(webrtc::SdpVideoFormat(
        "H264", {{"profile-level-id", "42e01f"}, {"packetization-mode", "1"}}));
  };
  std::unique_ptr<libwebrtc::HwH264Encoder> enc_ptr = Neu();
  webrtc::VideoCodec vc;
  vc.codecType = webrtc::kVideoCodecH264;
  vc.width = o.breite;
  vc.height = o.hoehe;
  vc.maxFramerate = o.fps;
  vc.startBitrate = o.kbit;
  vc.maxBitrate = o.kbit * 3;
  vc.minBitrate = 100;
  vc.mode = webrtc::VideoCodecMode::kRealtimeVideo;
  const webrtc::VideoEncoder::Settings einst(webrtc::VideoEncoder::Capabilities(false), 2, 1200);
  if (enc_ptr->InitEncode(&vc, einst) != WEBRTC_VIDEO_CODEC_OK) {
    std::printf("FEHL[%d]: InitEncode\n", nummer);
    std::exit(1);
  }
  Sammler s;
  if (nummer == 0 && !o.aus.empty()) s.datei = std::fopen(o.aus.c_str(), "wb");
  enc_ptr->RegisterEncodeCompleteCallback(&s);
  auto Rate = [&](int kbit, int fps) {
    webrtc::VideoBitrateAllocation a;
    a.SetBitrate(0, 0, kbit * 1000);
    enc_ptr->SetRates(webrtc::VideoEncoder::RateControlParameters(a, fps));
  };
  Rate(o.kbit, o.fps);

  auto gross = webrtc::I420Buffer::Create(o.breite, o.hoehe);
  auto klein = webrtc::I420Buffer::Create(o.breite, o.hoehe - 16);
  const double start = JetztMs();
  bool wechsel = false;
  for (int n = 0;; ++n) {
    if (g_halt) break;
    if (o.bilder > 0 ? n >= o.bilder : JetztMs() - start >= o.dauer * 1000.0) break;
    // Neuaufsetzen erzwingen (nicht beim ersten Bild).
    if (n > 0 && o.alle > 0 && n % o.alle == 0 && o.weg != "keiner") {
      wechsel = !wechsel;
      if (o.weg == "init") {
        vc.startBitrate = wechsel ? o.kbit * 2 : o.kbit;
        vc.maxFramerate = wechsel ? 21 : o.fps;
        const double t0 = JetztMs();
        const int r = enc_ptr->InitEncode(&vc, einst);
        const double d = JetztMs() - t0;
        ++e.neu;
        e.neu_ms_summe += d;
        if (d > e.neu_ms_max) e.neu_ms_max = d;
        if (r != WEBRTC_VIDEO_CODEC_OK) ++e.neu_fehler;
        s.warte_schluessel = true;
        Rate(vc.startBitrate, vc.maxFramerate);
      } else if (o.weg == "release" || o.weg == "instanz") {
        const double t0 = JetztMs();
        if (o.weg == "release") {
          enc_ptr->Release();
        } else {
          enc_ptr.reset();
          enc_ptr = Neu();
        }
        const int r = enc_ptr->InitEncode(&vc, einst);
        enc_ptr->RegisterEncodeCompleteCallback(&s);
        const double d = JetztMs() - t0;
        ++e.neu;
        e.neu_ms_summe += d;
        if (d > e.neu_ms_max) e.neu_ms_max = d;
        if (r != WEBRTC_VIDEO_CODEC_OK) ++e.neu_fehler;
        s.warte_schluessel = true;
        Rate(vc.startBitrate, vc.maxFramerate);
      } else if (o.weg == "rate") {
        Rate(wechsel ? o.kbit * 2 : o.kbit, wechsel ? 21 : o.fps);
      } else if (o.weg == "groesse") {
        ++e.neu;
        s.warte_schluessel = true;
      }
    }
    webrtc::scoped_refptr<webrtc::I420Buffer> b =
        (o.weg == "groesse" && wechsel) ? klein : gross;
    Malen(*b, n);
    const webrtc::VideoFrame f = webrtc::VideoFrame::Builder()
                                     .set_video_frame_buffer(b)
                                     .set_rtp_timestamp(static_cast<uint32_t>(n * 3000))
                                     .set_timestamp_us(static_cast<int64_t>(n) * 33333)
                                     .set_id(o.bildschirm ? libwebrtc::nullkopie::kBildschirmBildId : 0)
                                     .build();
    const double t0 = JetztMs();
    std::vector<webrtc::VideoFrameType> arten{n == 0 ? webrtc::VideoFrameType::kVideoFrameKey
                                                     : webrtc::VideoFrameType::kVideoFrameDelta};
    const int r = enc_ptr->Encode(f, &arten);
    const double d = JetztMs() - t0;
    if (o.weg == "groesse" && n > 0 && o.alle > 0 && n % o.alle == 0) {
      e.neu_ms_summe += d;
      if (d > e.neu_ms_max) e.neu_ms_max = d;
    }
    ++e.bilder;
    if (r == WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE) {
      // Hier übernähme die Hülle des SimulcastEncoderAdapter (OpenH264).
      e.software_bei = n;
      break;
    }
    if (r != WEBRTC_VIDEO_CODEC_OK) ++e.fehler;
    if (o.takt > 0) {
      const double soll = start + (n + 1) * 1000.0 / o.takt;
      const double rest = soll - JetztMs();
      if (rest > 0) usleep(static_cast<useconds_t>(rest * 1000));
    }
    if (nummer == 0 && n > 0 && n % 200 == 0) {
      std::printf("  [%5.1f s] Bilder %d, Pakete %d, Neuaufsetzen %d, Fehler %d\n",
                  (JetztMs() - start) / 1000.0, n, s.pakete.load(), e.neu, e.fehler);
      std::fflush(stdout);
    }
  }
  enc_ptr->Release();
  enc_ptr.reset();
  if (s.datei) std::fclose(s.datei);
  e.pakete = s.pakete;
  e.schluessel = s.schluessel;
  e.ohne_schluessel = s.ohne_schluessel;
  e.bytes = s.bytes;
  return e;
}

}  // namespace

int main(int argc, char** argv) {
  Optionen o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto Wert = [&]() -> const char* {
      if (i + 1 >= argc) {
        std::printf("FEHL: %s ohne Wert\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--weg") o.weg = Wert();
    else if (a == "--alle") o.alle = std::atoi(Wert());
    else if (a == "--takt") o.takt = std::atoi(Wert());
    else if (a == "--dauer") o.dauer = std::atoi(Wert());
    else if (a == "--breite") o.breite = std::atoi(Wert());
    else if (a == "--hoehe") o.hoehe = std::atoi(Wert());
    else if (a == "--fps") o.fps = std::atoi(Wert());
    else if (a == "--kbit") o.kbit = std::atoi(Wert());
    else if (a == "--encoder") o.encoder = std::atoi(Wert());
    else if (a == "--gllast") o.gllast = Wert();
    else if (a == "--glstufe") o.glstufe = std::atoi(Wert());
    else if (a == "--geteilt") o.geteilt = true;
    else if (a == "--aus") o.aus = Wert();
    else if (a == "--bilder") o.bilder = std::atoi(Wert());
    else if (a == "--bildschirm") o.bildschirm = true;
    else if (a == "--erwarte-software") o.erwarte_software = true;
    else {
      std::printf("FEHL: unbekannt %s\n", a.c_str());
      return 2;
    }
  }
  const webrtc::Environment env = webrtc::CreateEnvironment();
  (void)env;
  if (!libwebrtc::HwH264Encoder::IstVerfuegbar()) {
    std::printf("FEHL: kein Hardware-Encoder\n");
    return 1;
  }
  std::printf("Encoder: %s\n", libwebrtc::HwH264Encoder::BackendName().c_str());
  if (o.geteilt) {
    std::printf("Gemeinsames VA-Gerät (Null-Kopie): %s\n",
                libwebrtc::nullkopie::Einrichten() ? "an" : "AUS");
  }

  void (*gl_stop)() = nullptr;
  int (*gl_bilder)() = nullptr;
  if (!o.gllast.empty()) {
    void* h = dlopen(o.gllast.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
      std::printf("FEHL: %s\n", dlerror());
      return 2;
    }
    auto start = reinterpret_cast<int (*)(int)>(dlsym(h, "gllast_start"));
    gl_stop = reinterpret_cast<void (*)()>(dlsym(h, "gllast_stop"));
    gl_bilder = reinterpret_cast<int (*)()>(dlsym(h, "gllast_bilder"));
    if (!start || !gl_stop || !gl_bilder || start(o.glstufe) != 0) {
      std::printf("FEHL: GL-Last startet nicht\n");
      return 2;
    }
    std::printf("GL-Last im Prozess: Stufe %d\n", o.glstufe);
  }

  std::printf("Lauf: weg=%s alle=%d takt=%d %s=%d %dx%d @%d %d kbit/s, %d Encoder\n",
              o.weg.c_str(), o.alle, o.takt, o.bilder > 0 ? "bilder" : "dauer",
              o.bilder > 0 ? o.bilder : o.dauer, o.breite, o.hoehe, o.fps, o.kbit, o.encoder);
  std::fflush(stdout);

  // Hängewächter: > 5 s ohne ein einziges Paket = Encoder steht.
  g_letzte_ausgabe_ms = JetztMs();
  std::atomic<bool> fertig{false};
  std::atomic<bool> hing{false};
  std::thread waechter([&] {
    while (!fertig) {
      usleep(200 * 1000);
      if (JetztMs() - g_letzte_ausgabe_ms.load() > 5000) {
        hing = true;
        std::printf("HAENGT: seit %.1f s kein Paket\n",
                    (JetztMs() - g_letzte_ausgabe_ms.load()) / 1000.0);
        std::fflush(stdout);
        g_halt = true;
        return;
      }
    }
  });

  std::vector<Ergebnis> erg(o.encoder);
  std::vector<std::thread> faeden;
  for (int k = 0; k < o.encoder; ++k)
    faeden.emplace_back([&, k] { erg[k] = EncoderLauf(o, k); });
  for (auto& t : faeden) t.join();
  fertig = true;
  waechter.join();
  int gl = gl_bilder ? gl_bilder() : 0;
  if (gl_stop) gl_stop();

  bool ok = !hing;
  for (int k = 0; k < o.encoder; ++k) {
    const Ergebnis& e = erg[k];
    std::printf("Encoder %d: Bilder %d, Pakete %d, Schlüssel %d, Bytes %llu, Fehler %d, "
                "Neuaufsetzen %d (gescheitert %d, Mittel %.1f ms, max %.1f ms), "
                "ohne Schlüssel danach %d, ohne Paket %d\n",
                k, e.bilder, e.pakete, e.schluessel, static_cast<unsigned long long>(e.bytes),
                e.fehler, e.neu, e.neu_fehler, e.neu ? e.neu_ms_summe / e.neu : 0.0, e.neu_ms_max,
                e.ohne_schluessel, e.bilder - e.pakete);
    if (e.software_bei >= 0)
      std::printf("Encoder %d: Software-Rückfall bei Bild %d\n", k, e.software_bei);
    if (o.erwarte_software) {
      if (e.software_bei != 0 || e.pakete != 0) ok = false;
      continue;
    }
    if (e.software_bei >= 0) ok = false;
    if (e.fehler || e.neu_fehler || e.ohne_schluessel) ok = false;
  }
  if (gl_bilder) std::printf("GL-Last: %d Bilder gezeichnet\n", gl);
  std::printf("%s\n", ok ? "BESTANDEN" : "DURCHGEFALLEN");
  return ok ? 0 : 1;
}
