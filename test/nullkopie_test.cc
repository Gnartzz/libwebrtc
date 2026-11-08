// Prüfprogramm Null-Kopie Linux (08.11.2026): der ECHTE Wandler
// (hw_nullkopie.cc) und der ECHTE VA-API-Encoder mit einem gekachelten
// GBM-Puffer, wie ihn ein Compositor über PipeWire reicht — gegen den alten
// Weg (BGRA im Arbeitsspeicher → ARGBToI420 → I420Scale → Encoder).
//
// Bauen:   ninja -C out/Release-x64 libwebrtc:nullkopie_test
// Aufruf:  nullkopie_test <quell_b> <quell_h> <ziel_b> <ziel_h> [bilder] [fps] [kbit]
//          NK_MOD=y (Intel Y-Kachelung) | x | linear | auto (Treiber wählt)
// Rückgabe 0 = bestanden: alle Bilder kodiert, Farben gleich (Y ≤ 2, UV ≤ 3
// im Mittel), Schlüsselbild auf Anforderung, Rate wie der alte Weg (±15 %),
// GPU-Weg billiger.

#include <dlfcn.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "api/environment/environment_factory.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_encoder.h"
#include "libyuv/convert.h"
#include "libyuv/scale.h"
#include "modules/desktop_capture/linux/wayland/honeycord_dmabuf.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "src/linux/hwenc/ffmpeg/hw_h264_encoder.h"
#include "src/linux/hwenc/ffmpeg/hw_nullkopie.h"

namespace {

// ── GBM per dlopen (keine Mesa-Header im Bau nötig) ──
struct gbm_device;
struct gbm_bo;
struct Gbm {
  gbm_device* (*create_device)(int);
  gbm_bo* (*bo_create)(gbm_device*, uint32_t, uint32_t, uint32_t, uint32_t);
  gbm_bo* (*bo_create_with_modifiers)(gbm_device*, uint32_t, uint32_t, uint32_t,
                                      const uint64_t*, unsigned);
  int (*bo_get_fd)(gbm_bo*);
  uint32_t (*bo_get_stride)(gbm_bo*);
  uint32_t (*bo_get_offset)(gbm_bo*, int);
  uint64_t (*bo_get_modifier)(gbm_bo*);
  int (*bo_get_plane_count)(gbm_bo*);
  int (*bo_get_fd_for_plane)(gbm_bo*, int);
  uint32_t (*bo_get_stride_for_plane)(gbm_bo*, int);
  void* (*bo_map)(gbm_bo*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t*, void**);
  void (*bo_unmap)(gbm_bo*, void*);
};
constexpr uint32_t kXrgb = 'X' | ('R' << 8) | ('2' << 16) | ('4' << 24);
constexpr uint32_t kUseRendering = 1u << 2, kUseLinear = 1u << 4, kTransferWrite = 1u << 1;

template <typename T>
void Fn(void* h, const char* n, T* z) {
  *z = reinterpret_cast<T>(dlsym(h, n));
  if (!*z) { std::printf("FEHL: %s fehlt\n", n); std::exit(2); }
}

double Cpu() { timespec t; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
double Wand() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

// Muster wie pwweg.c: vier Farbflächen, eine wandernde weiße.
void Malen(uint8_t* bgra, int stride, int w, int h, int n) {
  const uint8_t grund[3] = {77, 51, 38};  // B G R
  const uint8_t farbe[4][3] = {{25, 25, 230}, {51, 204, 25}, {230, 77, 51}, {51, 217, 242}};
  for (int y = 0; y < h; ++y) {
    uint8_t* z = bgra + y * stride;
    for (int x = 0; x < w; ++x) {
      const uint8_t* c = grund;
      for (int k = 0; k < 4; ++k)
        if (x >= w / 8 + k * w / 5 && x < w / 8 + k * w / 5 + w / 6 && y >= h / 4 && y < h / 4 + h / 3)
          c = farbe[k];
      static const uint8_t weiss[3] = {255, 255, 255};
      const int wx = (n * 13) % (w - w / 6);
      if (x >= wx && x < wx + w / 6 && y >= h * 2 / 3 && y < h * 2 / 3 + h / 5) c = weiss;
      z[4 * x] = c[0]; z[4 * x + 1] = c[1]; z[4 * x + 2] = c[2]; z[4 * x + 3] = 255;
    }
  }
}

struct Sammler : webrtc::EncodedImageCallback {
  FILE* datei = nullptr;
  size_t bytes[2] = {0, 0};
  int haelfte = 0, bilder = 0, schluessel = 0, anf_bei = -1, schluessel_nach = -1;
  Result OnEncodedImage(const webrtc::EncodedImage& img, const webrtc::CodecSpecificInfo*) override {
    if (datei) std::fwrite(img.data(), 1, img.size(), datei);
    bytes[haelfte] += img.size();
    if (img._frameType == webrtc::VideoFrameType::kVideoFrameKey) {
      ++schluessel;
      if (anf_bei >= 0 && schluessel_nach < 0 && bilder >= anf_bei) schluessel_nach = bilder;
    }
    ++bilder;
    return Result(Result::OK);
  }
};

struct Lauf {
  double cpu_ms = 0, wand_ms = 0;
  double r1 = 0, r2 = 0;
  int bilder = 0, fehler = 0, schluessel_nach = -1, anf = 0;
};

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 5) {
    std::printf("Aufruf: nullkopie_test <quell_b> <quell_h> <ziel_b> <ziel_h> [bilder] [fps] [kbit]\n");
    return 2;
  }
  const int qb = std::atoi(argv[1]), qh = std::atoi(argv[2]);
  const int zb = std::atoi(argv[3]), zh = std::atoi(argv[4]);
  const int n = argc > 5 ? std::atoi(argv[5]) : 300;
  const int fps = argc > 6 ? std::atoi(argv[6]) : 60;
  const int kbit = argc > 7 ? std::atoi(argv[7]) : 4000;

  if (!libwebrtc::nullkopie::Einrichten()) {
    std::printf("FEHL: Null-Kopie nicht aktiv (siehe hwenc.log)\n");
    return 1;
  }
  webrtc::HcDmaBufWandler* wandler = webrtc::HcDmaBufWandlerPlatz().load();
  if (!wandler) { std::printf("FEHL: kein Wandler eingetragen\n"); return 1; }

  void* lib = dlopen("libgbm.so.1", RTLD_NOW);
  if (!lib) { std::printf("FEHL: libgbm.so.1 fehlt\n"); return 2; }
  Gbm g;
  Fn(lib, "gbm_create_device", &g.create_device);
  Fn(lib, "gbm_bo_create", &g.bo_create);
  Fn(lib, "gbm_bo_create_with_modifiers", &g.bo_create_with_modifiers);
  Fn(lib, "gbm_bo_get_fd", &g.bo_get_fd);
  Fn(lib, "gbm_bo_get_stride", &g.bo_get_stride);
  Fn(lib, "gbm_bo_get_offset", &g.bo_get_offset);
  Fn(lib, "gbm_bo_get_modifier", &g.bo_get_modifier);
  Fn(lib, "gbm_bo_get_plane_count", &g.bo_get_plane_count);
  Fn(lib, "gbm_bo_get_fd_for_plane", &g.bo_get_fd_for_plane);
  Fn(lib, "gbm_bo_get_stride_for_plane", &g.bo_get_stride_for_plane);
  Fn(lib, "gbm_bo_map", &g.bo_map);
  Fn(lib, "gbm_bo_unmap", &g.bo_unmap);
  const int drm = open("/dev/dri/renderD128", O_RDWR);
  gbm_device* dev = drm >= 0 ? g.create_device(drm) : nullptr;
  if (!dev) { std::printf("FEHL: GBM-Gerät\n"); return 2; }
  const char* m = std::getenv("NK_MOD");
  gbm_bo* bo = nullptr;
  if (!m || !std::strcmp(m, "auto")) {
    bo = g.bo_create(dev, qb, qh, kXrgb, kUseRendering);
  } else if (!std::strcmp(m, "linear")) {
    bo = g.bo_create(dev, qb, qh, kXrgb, kUseRendering | kUseLinear);
  } else {
    // y / x = Intel-Kachelung, sonst eine Zahl (0x…, aus eglQueryDmaBufModifiersEXT).
    const uint64_t mod = !std::strcmp(m, "x")   ? 0x0100000000000001ULL
                         : !std::strcmp(m, "y") ? 0x0100000000000002ULL
                                                : std::strtoull(m, nullptr, 0);
    bo = g.bo_create_with_modifiers(dev, qb, qh, kXrgb, &mod, 1);
  }
  if (!bo) { std::printf("FEHL: GBM-Puffer\n"); return 2; }
  webrtc::HcDmaBufBild bild;
  bild.strom_breite = qb; bild.strom_hoehe = qh;
  bild.breite = qb; bild.hoehe = qh;
  bild.spa_format = 8;  // SPA_VIDEO_FORMAT_BGRx
  bild.modifier = g.bo_get_modifier(bo);
  bild.ebenen = g.bo_get_plane_count(bo);
  if (bild.ebenen < 1 || bild.ebenen > 4) { std::printf("FEHL: %d Ebenen\n", bild.ebenen); return 2; }
  // Je Ebene ein eigener Deskriptor — so reicht PipeWire sie auch.
  for (int i = 0; i < bild.ebenen; ++i) {
    bild.ebene[i].fd = g.bo_get_fd_for_plane(bo, i);
    bild.ebene[i].offset = g.bo_get_offset(bo, i);
    bild.ebene[i].stride = g.bo_get_stride_for_plane(bo, i);
  }
  std::printf("Puffer %dx%d stride %u Modifier 0x%llx, %d Ebene(n) → Ziel %dx%d @%d, %d Bilder, %d kbit/s\n", qb,
              qh, bild.ebene[0].stride, static_cast<unsigned long long>(bild.modifier), bild.ebenen, zb, zh, fps,
              n, kbit);

  std::vector<uint8_t> ref(static_cast<size_t>(qb) * qh * 4);
  auto PufferMalen = [&](int i) {
    Malen(ref.data(), qb * 4, qb, qh, i);
    uint32_t st = 0; void* md = nullptr;
    auto* p = static_cast<uint8_t*>(g.bo_map(bo, 0, 0, qb, qh, kTransferWrite, &st, &md));
    if (!p) { std::printf("FEHL: gbm_bo_map\n"); std::exit(2); }
    for (int y = 0; y < qh; ++y) std::memcpy(p + y * st, ref.data() + y * qb * 4, qb * 4);
    g.bo_unmap(bo, md);
  };

  const webrtc::Environment env = webrtc::CreateEnvironment();
  (void)env;
  auto Laufen = [&](bool gpu, const char* datei, double* farbe_y, double* farbe_uv) {
    Lauf l;
    libwebrtc::HwH264Encoder enc(webrtc::SdpVideoFormat(
        "H264", {{"profile-level-id", "42e01f"}, {"packetization-mode", "1"}}));
    webrtc::VideoCodec vc;
    vc.codecType = webrtc::kVideoCodecH264;
    vc.width = zb; vc.height = zh; vc.maxFramerate = fps;
    vc.startBitrate = kbit; vc.maxBitrate = kbit * 2; vc.minBitrate = 100;
    vc.mode = webrtc::VideoCodecMode::kScreensharing;
    if (enc.InitEncode(&vc, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 2, 1200)) !=
        WEBRTC_VIDEO_CODEC_OK) { std::printf("FEHL: InitEncode\n"); std::exit(1); }
    Sammler s;
    s.datei = std::fopen(datei, "wb");
    enc.RegisterEncodeCompleteCallback(&s);
    auto Rate = [&](int k) {
      webrtc::VideoBitrateAllocation a;
      a.SetBitrate(0, 0, k * 1000);
      enc.SetRates(webrtc::VideoEncoder::RateControlParameters(a, fps));
    };
    Rate(kbit);
    if (gpu) std::printf("   Encoder: %s, nimmt GPU-Bilder: %s\n", enc.GetEncoderInfo().implementation_name.c_str(),
                         enc.GetEncoderInfo().supports_native_handle ? "ja" : "NEIN");
    l.anf = n / 4;
    s.anf_bei = l.anf;
    const int wechsel = n / 2;
    const double t0 = Wand();
    for (int i = 0; i < n; ++i) {
      PufferMalen(i);   // gehört nicht zur Messung (simuliert den Compositor)
      while (Wand() < t0 + i * 1000.0 / fps) usleep(500);
      if (i == wechsel) { Rate(kbit / 2); s.haelfte = 1; }
      const double c0 = Cpu(), w0 = Wand();
      webrtc::scoped_refptr<webrtc::VideoFrameBuffer> puffer;
      if (gpu) {
        auto nativ = wandler->Wandeln(bild);
        if (!nativ) { ++l.fehler; continue; }
        puffer = webrtc::scoped_refptr<webrtc::VideoFrameBuffer>(
            static_cast<webrtc::VideoFrameBuffer*>(nativ.get()));
        if (zb != qb || zh != qh) puffer = puffer->Scale(zb, zh);
      } else {
        auto i420 = webrtc::I420Buffer::Create(qb, qh);
        libyuv::ARGBToI420(ref.data(), qb * 4, i420->MutableDataY(), i420->StrideY(), i420->MutableDataU(),
                           i420->StrideU(), i420->MutableDataV(), i420->StrideV(), qb, qh);
        if (zb != qb || zh != qh) {
          auto k = webrtc::I420Buffer::Create(zb, zh);
          k->ScaleFrom(*i420);
          puffer = k;
        } else {
          puffer = i420;
        }
      }
      webrtc::VideoFrame f = webrtc::VideoFrame::Builder()
                                 .set_video_frame_buffer(puffer)
                                 .set_rtp_timestamp(static_cast<uint32_t>(i * 90000 / fps))
                                 .set_timestamp_us(1000000 + static_cast<int64_t>(i) * 1000000 / fps)
                                 .build();
      std::vector<webrtc::VideoFrameType> typ{i == l.anf ? webrtc::VideoFrameType::kVideoFrameKey
                                                         : webrtc::VideoFrameType::kVideoFrameDelta};
      if (enc.Encode(f, &typ) != WEBRTC_VIDEO_CODEC_OK) ++l.fehler;
      if (i >= 10) { l.cpu_ms += Cpu() - c0; l.wand_ms += Wand() - w0; }
      // Farbvergleich am letzten Bild: GPU-Bild zurücklesen gegen den alten Weg.
      if (gpu && i == n - 1 && farbe_y) {
        auto a = puffer->ToI420();
        auto b0 = webrtc::I420Buffer::Create(qb, qh);
        libyuv::ARGBToI420(ref.data(), qb * 4, b0->MutableDataY(), b0->StrideY(), b0->MutableDataU(),
                           b0->StrideU(), b0->MutableDataV(), b0->StrideV(), qb, qh);
        webrtc::scoped_refptr<webrtc::I420BufferInterface> b = b0;
        if (zb != qb || zh != qh) { auto k = webrtc::I420Buffer::Create(zb, zh); k->ScaleFrom(*b0); b = k; }
        double dy = 0, duv = 0;
        for (int y = 0; y < zh; ++y)
          for (int x = 0; x < zb; ++x) dy += std::abs(a->DataY()[y * a->StrideY() + x] - b->DataY()[y * b->StrideY() + x]);
        for (int y = 0; y < (zh + 1) / 2; ++y)
          for (int x = 0; x < (zb + 1) / 2; ++x)
            duv += std::abs(a->DataU()[y * a->StrideU() + x] - b->DataU()[y * b->StrideU() + x]) +
                   std::abs(a->DataV()[y * a->StrideV() + x] - b->DataV()[y * b->StrideV() + x]);
        *farbe_y = dy / (zb * zh);
        *farbe_uv = duv / (2.0 * ((zb + 1) / 2) * ((zh + 1) / 2));
      }
    }
    enc.Release();
    if (s.datei) std::fclose(s.datei);
    const double s1 = static_cast<double>(wechsel) / fps, s2 = static_cast<double>(n - wechsel) / fps;
    l.r1 = s.bytes[0] * 8 / s1 / 1000.0;
    l.r2 = s.bytes[1] * 8 / s2 / 1000.0;
    l.bilder = s.bilder;
    l.schluessel_nach = s.schluessel_nach;
    const int m = n - 10;
    std::printf("%s: CPU %.2f ms/Bild, Wand %.2f ms/Bild, %d/%d Bilder, %d Fehler, Schlüsselbild bei %d → %d, "
                "Rate %.0f / %.0f kbit/s (Ziel %d / %d)\n",
                gpu ? "GPU-Weg (Null-Kopie)" : "Alter Weg (ohne glReadPixels)", l.cpu_ms / m, l.wand_ms / m,
                l.bilder, n, l.fehler, l.anf, l.schluessel_nach, l.r1, l.r2, kbit, kbit / 2);
    return l;
  };

  double fy = 99, fuv = 99;
  const Lauf a = Laufen(false, "/tmp/nk_alt.h264", nullptr, nullptr);
  const Lauf b = Laufen(true, "/tmp/nk_gpu.h264", &fy, &fuv);
  std::printf("Farbvergleich (letztes Bild, GPU zurückgelesen gegen alten Weg): Y %.2f, UV %.2f\n", fy, fuv);
  // Die Ratenregeln des Encoders sind dieselben wie vorher (eine Absenkung
  // wartet ihre Sperrzeit ab) — geprüft wird, dass der GPU-Weg die Rate
  // genauso trifft wie der alte, und die erste Hälfte das Ziel.
  auto RateOk = [&](const Lauf& l) {
    return l.r1 > kbit * 0.7 && l.r1 < kbit * 1.3 && std::fabs(l.r1 - a.r1) < a.r1 * 0.15 &&
           std::fabs(l.r2 - a.r2) < a.r2 * 0.15;
  };
  const bool gut = b.fehler == 0 && b.bilder >= n * 95 / 100 && fy <= 2.0 && fuv <= 3.0 &&
                   b.schluessel_nach >= b.anf && b.schluessel_nach <= b.anf + 2 && RateOk(b) &&
                   b.cpu_ms < a.cpu_ms;
  std::printf("%s\n", gut ? "BESTANDEN" : "FEHL");
  return gut ? 0 : 1;
}
