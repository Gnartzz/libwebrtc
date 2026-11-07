// Vergleich VA-API-Decoder gegen den eingebauten Software-Decoder (07.11.2026).
//
// ★ H.264-DEKODIERUNG IST BITGENAU FESTGELEGT. Jeder normgerechte Decoder muss
// aus demselben Strom exakt dieselben Bildpunkte erzeugen. Deshalb ist der
// schärfste Test kein „sieht gut aus", sondern: Hardware und Software liefern
// Bild für Bild identische Ebenen. Dazu die CPU-Zeit beider Wege — das ist die
// Zahl, um die es geht.
//
// Bauen (im WebRTC-Baum):  ninja -C out/Release-x64 libwebrtc:hwdec_vergleich
// Eingabe: Annex-B-H.264 mit Zugriffseinheits-Trennern (AUD), z. B.
//   ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=30 -t 10 -c:v libx264 \
//     -profile:v high -bf 0 -g 60 -x264-params repeat-headers=1:slices=1 \
//     -bsf:v h264_metadata=aud=insert -f h264 strom.h264
// Aufruf:  hwdec_vergleich strom.h264 [--nur-wrapper]
//   Standard: SW und HW getrennt, Bildvergleich, CPU-Zeiten.
//   --nur-wrapper: nur über die Linux-Fabrik (Rückfall-Wrapper); prüft, dass
//   alle Bilder herauskommen, und nennt den am Ende aktiven Decoder — für
//   Ströme über der Treibergrenze.
// Rückgabe 0 = bestanden.

#include <sys/resource.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "api/environment/environment_factory.h"
#include "api/video/encoded_image.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "src/linux/hwenc/ffmpeg/hw_h264_decoder.h"
#include "src/linux/hwenc/hw_decoder_factory.h"

namespace {

struct Einheit {
  std::vector<uint8_t> daten;
  bool schluessel = false;
};

// Zerlegt Annex-B an den AUD-NALs (Typ 9).
std::vector<Einheit> Zerlegen(const std::vector<uint8_t>& s) {
  std::vector<size_t> starts;  // Position des Startcodes
  std::vector<int> typen;
  for (size_t i = 0; i + 3 < s.size(); ++i) {
    if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) {
      size_t beginn = (i > 0 && s[i - 1] == 0) ? i - 1 : i;
      starts.push_back(beginn);
      typen.push_back(s[i + 3] & 0x1F);
      i += 2;
    }
  }
  std::vector<Einheit> aus;
  for (size_t k = 0; k < starts.size(); ++k) {
    if (typen[k] == 9 || aus.empty()) aus.emplace_back();
    const size_t ende = k + 1 < starts.size() ? starts[k + 1] : s.size();
    aus.back().daten.insert(aus.back().daten.end(), s.begin() + starts[k],
                            s.begin() + ende);
    if (typen[k] == 5) aus.back().schluessel = true;
  }
  return aus;
}

struct Sammler : webrtc::DecodedImageCallback {
  std::map<uint32_t, webrtc::scoped_refptr<webrtc::I420BufferInterface>> bilder;
  bool behalten = true;  // Dauerlauf: nur zählen, sonst läuft der Speicher voll
  size_t gezaehlt = 0;
  int32_t Decoded(webrtc::VideoFrame& f) override {
    ++gezaehlt;
    if (behalten) bilder[f.rtp_timestamp()] = f.video_frame_buffer()->ToI420();
    return 0;
  }
};

double CpuMs() {
  rusage r{};
  getrusage(RUSAGE_SELF, &r);
  return (r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1000.0 +
         (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1000.0;
}

struct Lauf {
  int ok = 0, fehler = 0, rueckfall = 0;
  double cpu_ms = 0;
};

Lauf Dekodieren(webrtc::VideoDecoder* d, const std::vector<Einheit>& e, int w,
                int h, size_t runden = 1) {
  Lauf l;
  const double c0 = CpuMs();
  for (size_t k = 0; k < e.size() * runden; ++k) {
    const size_t i = k % e.size();
    webrtc::EncodedImage img;
    img.SetEncodedData(
        webrtc::EncodedImageBuffer::Create(e[i].daten.data(), e[i].daten.size()));
    img.SetRtpTimestamp(static_cast<uint32_t>(k * 3000));
    img._frameType = e[i].schluessel ? webrtc::VideoFrameType::kVideoFrameKey
                                     : webrtc::VideoFrameType::kVideoFrameDelta;
    img._encodedWidth = w;
    img._encodedHeight = h;
    const int32_t r = d->Decode(img, /*render_time_ms=*/0);
    if (r == WEBRTC_VIDEO_CODEC_OK) ++l.ok;
    else if (r == WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE) ++l.rueckfall;
    else ++l.fehler;
  }
  l.cpu_ms = CpuMs() - c0;
  return l;
}

// Größte Abweichung je Ebene; -1, wenn die Maße nicht passen.
int Abweichung(const webrtc::I420BufferInterface& a,
               const webrtc::I420BufferInterface& b) {
  if (a.width() != b.width() || a.height() != b.height()) return -1;
  int m = 0;
  auto ebene = [&m](const uint8_t* p, int sp, const uint8_t* q, int sq, int w,
                    int h) {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const int d = std::abs(int(p[y * sp + x]) - int(q[y * sq + x]));
        if (d > m) m = d;
      }
  };
  ebene(a.DataY(), a.StrideY(), b.DataY(), b.StrideY(), a.width(), a.height());
  ebene(a.DataU(), a.StrideU(), b.DataU(), b.StrideU(), a.ChromaWidth(),
        a.ChromaHeight());
  ebene(a.DataV(), a.StrideV(), b.DataV(), b.StrideV(), a.ChromaWidth(),
        a.ChromaHeight());
  return m;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "Aufruf: %s strom.h264 [--nur-wrapper]\n", argv[0]);
    return 2;
  }
  const bool nur_wrapper = argc > 2 && std::strcmp(argv[2], "--nur-wrapper") == 0;
  // --dauer N: nur VA-API, den Strom N-mal hintereinander (für die
  // 30-s-Zeitenzeile im hwenc.log), CPU-Zeit je Bild.
  const size_t dauer = argc > 3 && std::strcmp(argv[2], "--dauer") == 0
                           ? static_cast<size_t>(std::atoi(argv[3])) : 0;
  FILE* f = std::fopen(argv[1], "rb");
  if (!f) {
    std::perror("Datei");
    return 2;
  }
  std::vector<uint8_t> roh;
  uint8_t puf[65536];
  size_t n;
  while ((n = std::fread(puf, 1, sizeof(puf), f)) > 0) roh.insert(roh.end(), puf, puf + n);
  std::fclose(f);
  const std::vector<Einheit> einheiten = Zerlegen(roh);
  std::printf("Strom: %zu Bytes, %zu Zugriffseinheiten\n", roh.size(), einheiten.size());
  if (einheiten.empty()) return 2;

  const webrtc::Environment env = webrtc::CreateEnvironment();
  const webrtc::SdpVideoFormat format = webrtc::SdpVideoFormat::H264();
  webrtc::VideoDecoder::Settings einst;
  einst.set_codec_type(webrtc::kVideoCodecH264);
  einst.set_number_of_cores(1);

  if (nur_wrapper) {
    auto fabrik = libwebrtc::CreateLinuxVideoDecoderFactory();
    auto d = fabrik->Create(env, format);
    Sammler s;
    if (!d || !d->Configure(einst)) {
      std::printf("FEHL: Configure\n");
      return 1;
    }
    d->RegisterDecodeCompleteCallback(&s);
    const Lauf l = Dekodieren(d.get(), einheiten, 0, 0);
    const auto info = d->GetDecoderInfo();
    std::printf("Wrapper: %d ok, %d Fehler, %zu Bilder, CPU %.0f ms, am Ende: %s (hw=%d)\n",
                l.ok, l.fehler, s.bilder.size(), l.cpu_ms,
                info.implementation_name.c_str(), info.is_hardware_accelerated);
    d->Release();
    const bool gut = s.bilder.size() == einheiten.size();
    std::printf("%s\n", gut ? "BESTANDEN" : "FEHL: nicht alle Bilder");
    return gut ? 0 : 1;
  }

  if (!libwebrtc::HwH264Decoder::IstVerfuegbar()) {
    std::printf("FEHL: VA-API-Decoder nicht verfügbar (siehe hwenc.log)\n");
    return 1;
  }

  if (dauer > 0) {
    libwebrtc::HwH264Decoder hw;
    Sammler s;
    s.behalten = false;
    if (!hw.Configure(einst)) return 1;
    hw.RegisterDecodeCompleteCallback(&s);
    const Lauf l = Dekodieren(&hw, einheiten, 0, 0, dauer);
    hw.Release();
    std::printf("Dauer VA-API: %d ok, %d Fehler, %zu Bilder, CPU %.2f ms/Bild\n", l.ok,
                l.fehler, s.gezaehlt, l.cpu_ms / (einheiten.size() * dauer));
    return 0;
  }

  // Software (eingebaut).
  auto sw = webrtc::CreateBuiltinVideoDecoderFactory()->Create(env, format);
  Sammler sw_s;
  if (!sw || !sw->Configure(einst)) {
    std::printf("FEHL: Software-Configure\n");
    return 1;
  }
  sw->RegisterDecodeCompleteCallback(&sw_s);
  const Lauf ls = Dekodieren(sw.get(), einheiten, 0, 0);
  sw->Release();

  // Hardware (direkt, ohne Wrapper — ein Rückfall soll hier auffallen).
  libwebrtc::HwH264Decoder hw;
  Sammler hw_s;
  if (!hw.Configure(einst)) {
    std::printf("FEHL: VA-API-Configure\n");
    return 1;
  }
  hw.RegisterDecodeCompleteCallback(&hw_s);
  const Lauf lh = Dekodieren(&hw, einheiten, 0, 0);
  hw.Release();

  int gleich = 0, verschieden = 0, fehlt = 0, max_abw = 0;
  for (const auto& [ts, b] : sw_s.bilder) {
    auto it = hw_s.bilder.find(ts);
    if (it == hw_s.bilder.end()) {
      ++fehlt;
      continue;
    }
    const int a = Abweichung(*b, *it->second);
    if (a == 0) ++gleich;
    else {
      ++verschieden;
      if (a < 0 || a > max_abw) max_abw = a;
    }
  }
  const int nb = static_cast<int>(einheiten.size());
  std::printf("Software: %d ok, %d Fehler, %zu Bilder, CPU %.0f ms (%.2f ms/Bild)\n",
              ls.ok, ls.fehler, sw_s.bilder.size(), ls.cpu_ms, ls.cpu_ms / nb);
  std::printf("VA-API:   %d ok, %d Fehler, %d Rückfall, %zu Bilder, CPU %.0f ms (%.2f ms/Bild)\n",
              lh.ok, lh.fehler, lh.rueckfall, hw_s.bilder.size(), lh.cpu_ms,
              lh.cpu_ms / nb);
  if (!sw_s.bilder.empty()) {
    const auto& b = *sw_s.bilder.begin()->second;
    std::printf("Maße: %dx%d\n", b.width(), b.height());
  }
  std::printf("Vergleich: %d bitgleich, %d verschieden (max. Abweichung %d), %d fehlen\n",
              gleich, verschieden, max_abw, fehlt);
  const bool gut = gleich == nb && verschieden == 0 && fehlt == 0 && lh.rueckfall == 0;
  std::printf("%s\n", gut ? "BESTANDEN" : "FEHL");
  return gut ? 0 : 1;
}
