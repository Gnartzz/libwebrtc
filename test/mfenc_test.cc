// Prüfprogramm #94: der echte MfH264Encoder (Intel Quick Sync über Media
// Foundation) ohne Client — Bilder hinein, Ratenwechsel, Schlüsselbild auf
// Anforderung, Ausgabe als Annex-B-Datei zum Gegenprüfen.
//
// Bauen (im WebRTC-Baum):  ninja -C out\win-x64-alpha libwebrtc:mfenc_test
// Aufruf:  mfenc_test <breite> <hoehe> <fps> <kbit/s> <sekunden> [ausgabe.h264]
// Ablauf: erste Hälfte mit <kbit/s>, zweite mit der Hälfte (SetRates),
// Schlüsselbild nach einem Viertel angefordert. Rückgabe 0 = bestanden.

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_codec.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "src/win/hwenc/mf/mf_h264_encoder.h"

namespace {

struct Sammler : webrtc::EncodedImageCallback {
  FILE* datei = nullptr;
  size_t bytes[2] = {0, 0};
  int bilder = 0, schluessel = 0;
  int haelfte = 0;
  int erstes_schluessel_nach_anforderung = -1;
  int anforderung_bei = -1;
  Result OnEncodedImage(const webrtc::EncodedImage& img, const webrtc::CodecSpecificInfo*) override {
    if (datei) std::fwrite(img.data(), 1, img.size(), datei);
    bytes[haelfte] += img.size();
    if (img._frameType == webrtc::VideoFrameType::kVideoFrameKey) {
      ++schluessel;
      if (anforderung_bei >= 0 && erstes_schluessel_nach_anforderung < 0 && bilder >= anforderung_bei)
        erstes_schluessel_nach_anforderung = bilder;
    }
    ++bilder;
    return Result(Result::OK);
  }
};

double Jetzt() {
  static LARGE_INTEGER f{};
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart * 1000.0 / f.QuadPart;
}

double CpuMs() {
  FILETIME a, b, k, u;
  GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u);
  auto z = [](FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32 | f.dwLowDateTime) / 10000.0; };
  return z(k) + z(u);
}

void Bild(webrtc::I420Buffer* b, int n) {
  const int w = b->width(), h = b->height();
  unsigned r = 1234567u + n * 7919u;
  for (int j = 0; j < h; ++j) {
    uint8_t* y = b->MutableDataY() + j * b->StrideY();
    for (int i = 0; i < w; ++i) {
      r = r * 1103515245u + 12345u;
      const int v = ((i + n * 4) & 255) / 2 + ((j * 2 + n) & 127) / 2 + (int)((r >> 24) & 15);
      const bool block = (i - (n * 7) % w + w) % w < w / 6 && (j - (n * 3) % h + h) % h < h / 5;
      y[i] = (uint8_t)(block ? 235 : (v > 235 ? 235 : v));
    }
  }
  for (int j = 0; j < (h + 1) / 2; ++j)
    for (int i = 0; i < (w + 1) / 2; ++i) {
      b->MutableDataU()[j * b->StrideU() + i] = (uint8_t)(128 + ((i + n) & 63) - 32);
      b->MutableDataV()[j * b->StrideV() + i] = (uint8_t)(128 + ((j - n) & 63) - 32);
    }
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 6) {
    std::printf("Aufruf: mfenc_test <breite> <hoehe> <fps> <kbit/s> <sekunden> [ausgabe.h264]\n");
    return 2;
  }
  const int w = std::atoi(argv[1]), h = std::atoi(argv[2]), fps = std::atoi(argv[3]);
  const int kbit = std::atoi(argv[4]), sek = std::atoi(argv[5]);
  // MF_EINSPEISUNG_FPS: weniger Bilder einspeisen als angemeldet (Bildschirm
  // mit 5–15 fps bei maxFramerate 30) — prüft, ob die Rate dann noch stimmt.
  const char* ef = std::getenv("MF_EINSPEISUNG_FPS");
  const int feed = ef && std::atoi(ef) > 0 ? std::atoi(ef) : fps;
  if (!libwebrtc::MfH264Encoder::IsSupported()) {
    std::printf("FEHL: Intel Quick Sync nicht verfuegbar (siehe %%LOCALAPPDATA%%\\HoneyCord\\mfenc.log)\n");
    return 1;
  }
  libwebrtc::MfH264Encoder enc(webrtc::SdpVideoFormat("H264", {{"profile-level-id", "42e01f"},
                                                              {"packetization-mode", "1"}}));
  webrtc::VideoCodec vc;
  vc.codecType = webrtc::kVideoCodecH264;
  vc.width = w;
  vc.height = h;
  vc.maxFramerate = fps;
  vc.startBitrate = kbit;
  vc.maxBitrate = kbit * 2;
  vc.mode = webrtc::VideoCodecMode::kRealtimeVideo;
  if (enc.InitEncode(&vc, 1, 1200) != WEBRTC_VIDEO_CODEC_OK) {
    std::printf("FEHL: InitEncode\n");
    return 1;
  }
  Sammler s;
  if (argc > 6) fopen_s(&s.datei, argv[6], "wb");
  enc.RegisterEncodeCompleteCallback(&s);
  auto rate = [&](int k) {
    webrtc::VideoBitrateAllocation a;
    a.SetBitrate(0, 0, k * 1000);
    // WebRTC meldet in SetRates die EINGANGSbildrate — hier die Einspeisung.
    enc.SetRates(webrtc::VideoEncoder::RateControlParameters(a, feed));
  };
  rate(kbit);

  const int n = feed * sek;
  std::vector<webrtc::scoped_refptr<webrtc::I420Buffer>> vorrat;
  for (int i = 0; i < 30; ++i) {
    vorrat.push_back(webrtc::I420Buffer::Create(w, h));
    Bild(vorrat.back().get(), i * 3);
  }
  const int anforderung = n / 4, wechsel = n / 2;
  s.anforderung_bei = anforderung;
  const double t0 = Jetzt(), c0 = CpuMs();
  double encode_ms = 0, encode_max = 0;
  int fehler = 0;
  for (int i = 0; i < n; ++i) {
    const double soll = t0 + i * 1000.0 / feed;
    while (Jetzt() < soll) Sleep(1);
    if (i == wechsel) {
      rate(kbit / 2);
      s.haelfte = 1;
    }
    webrtc::VideoFrame f = webrtc::VideoFrame::Builder()
                               .set_video_frame_buffer(vorrat[i % 30])
                               .set_rtp_timestamp(static_cast<uint32_t>(i * 90000 / feed))
                               .set_timestamp_us(1000000 + static_cast<int64_t>(i) * 1000000 / feed)
                               .build();
    std::vector<webrtc::VideoFrameType> typ{i == anforderung ? webrtc::VideoFrameType::kVideoFrameKey
                                                             : webrtc::VideoFrameType::kVideoFrameDelta};
    const double e0 = Jetzt();
    if (enc.Encode(f, &typ) != WEBRTC_VIDEO_CODEC_OK) ++fehler;
    const double e = Jetzt() - e0;
    encode_ms += e;
    if (e > encode_max) encode_max = e;
  }
  const double dt = Jetzt() - t0, dc = CpuMs() - c0;
  enc.Release();
  if (s.datei) std::fclose(s.datei);
  const double s1 = (double)wechsel / feed, s2 = (double)(n - wechsel) / feed;
  const double r1 = s.bytes[0] * 8 / s1 / 1000.0, r2 = s.bytes[1] * 8 / s2 / 1000.0;
  std::printf("Einspeisung: %d fps (angemeldet %d)\n", feed, fps);
  std::printf("Bilder: %d hinein, %d heraus, %d Fehler, %d Schluesselbilder\n", n, s.bilder, fehler, s.schluessel);
  std::printf("Schluesselbild angefordert bei %d -> erstes danach bei %d\n", anforderung, s.erstes_schluessel_nach_anforderung);
  std::printf("Encode: Mittel %.2f ms, max %.2f ms; CPU %.2f ms je Bild; Wand %.1f s\n", encode_ms / n, encode_max,
              dc / n, dt / 1000.0);
  std::printf("Rate 1. Haelfte %.0f kbit/s (Ziel %d), 2. Haelfte %.0f kbit/s (Ziel %d)\n", r1, kbit, r2, kbit / 2);
  const bool gut = s.bilder >= n * 95 / 100 && fehler == 0 &&
                   s.erstes_schluessel_nach_anforderung >= anforderung &&
                   s.erstes_schluessel_nach_anforderung <= anforderung + 2 && r2 < r1 * 0.8 &&
                   r1 > kbit * 0.7 && r1 < kbit * 1.3 && r2 > kbit * 0.5 * 0.7 && r2 < kbit * 0.5 * 1.3;
  std::printf("%s\n", gut ? "BESTANDEN" : "FEHL");
  return gut ? 0 : 1;
}
