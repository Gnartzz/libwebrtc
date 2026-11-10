// Prüfprogramm Empfang ohne Umweg (10.11.2026): der ECHTE Decoder, der ECHTE
// Export (`honeycord_gpu_bild`) und der GL-Baustein des Plugins
// (`hc_gpu_textur.h`, Kopie aus flutter-webrtc linux/) gegen den alten Weg.
//
//   alter Weg: VA-API dekodieren + zurückkopieren → I420ToABGR → glTexImage2D
//   GPU-Weg:   VA-API dekodieren + GPU-Kopie → DMA-BUF → EGLImage → Shader
//
// Bauen:   ninja -C out/Release-x64 libwebrtc:empfang_test
// Aufruf:  empfang_test strom.h264 [runden]
// Rückgabe 0 = bestanden: alle Bilder über die Grafikkarte, Farben gleich
// (mittlere Abweichung ≤ 2 je Kanal), GPU-Weg billiger.
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "api/environment/environment_factory.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/video_decoder.h"
#include "libyuv/convert_argb.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "src/rtc_video_frame_impl.h"
#include "src/linux/hwenc/ffmpeg/hw_h264_decoder.h"
#include "src/linux/hwenc/ffmpeg/hw_nullkopie.h"
#include "test/hc_gpu_textur.h"

extern "C" int honeycord_gpu_bild(void* rahmen, libwebrtc::nullkopie::HcGpuBild* aus);

namespace {

double CpuMs() {
  rusage r{};
  getrusage(RUSAGE_SELF, &r);
  return (r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1000.0 +
         (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1000.0;
}

std::vector<std::vector<uint8_t>> Zerlegen(const std::vector<uint8_t>& s, std::vector<bool>* key) {
  std::vector<size_t> starts;
  std::vector<int> typen;
  for (size_t i = 0; i + 3 < s.size(); ++i) {
    if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) {
      starts.push_back((i > 0 && s[i - 1] == 0) ? i - 1 : i);
      typen.push_back(s[i + 3] & 0x1F);
      i += 2;
    }
  }
  std::vector<std::vector<uint8_t>> aus;
  for (size_t k = 0; k < starts.size(); ++k) {
    if (typen[k] == 9 || aus.empty()) { aus.emplace_back(); key->push_back(false); }
    const size_t ende = k + 1 < starts.size() ? starts[k + 1] : s.size();
    aus.back().insert(aus.back().end(), s.begin() + starts[k], s.begin() + ende);
    if (typen[k] == 5) key->back() = true;
  }
  return aus;
}

// GL auf dem Render-Knoten ohne Fenster (GBM + EGL, OpenGL ES 2).
struct Gl {
  void* egl = nullptr;
  void* anzeige = nullptr;
  hcgpu::GetProc gp = nullptr;
  void (*glReadPixels)(int, int, int, int, unsigned, unsigned, void*) = nullptr;
  bool Auf() {
    void* gbm = dlopen("libgbm.so.1", RTLD_NOW);
    egl = dlopen("libEGL.so.1", RTLD_NOW);
    if (!gbm || !egl) return false;
    auto create_device = reinterpret_cast<void* (*)(int)>(dlsym(gbm, "gbm_create_device"));
    auto get_platform = reinterpret_cast<void* (*)(unsigned, void*, const intptr_t*)>(
        dlsym(egl, "eglGetPlatformDisplay"));
    auto init = reinterpret_cast<unsigned (*)(void*, int*, int*)>(dlsym(egl, "eglInitialize"));
    auto bind = reinterpret_cast<unsigned (*)(unsigned)>(dlsym(egl, "eglBindAPI"));
    auto create = reinterpret_cast<void* (*)(void*, void*, void*, const int*)>(
        dlsym(egl, "eglCreateContext"));
    auto make = reinterpret_cast<unsigned (*)(void*, void*, void*, void*)>(dlsym(egl, "eglMakeCurrent"));
    gp = reinterpret_cast<hcgpu::GetProc>(dlsym(egl, "eglGetProcAddress"));
    int drm = open("/dev/dri/renderD128", O_RDWR);
    void* dev = create_device(drm);
    anzeige = get_platform(0x31D7, dev, nullptr);  // EGL_PLATFORM_GBM_KHR
    if (!anzeige || !init(anzeige, nullptr, nullptr)) return false;
    bind(0x30A0);  // EGL_OPENGL_ES_API
    const int ca[] = {0x3098, 2, 0x3038};  // EGL_CONTEXT_CLIENT_VERSION 2
    void* ctx = create(anzeige, nullptr, nullptr, ca);
    if (!ctx || !make(anzeige, nullptr, nullptr, ctx)) return false;
    glReadPixels = reinterpret_cast<decltype(glReadPixels)>(gp("glReadPixels"));
    return glReadPixels != nullptr;
  }
};

struct Sammler : webrtc::DecodedImageCallback {
  hcgpu::Zeichner* z = nullptr;
  Gl* gl = nullptr;
  bool gpu_erwartet = false;
  int bilder = 0, gpu_bilder = 0, fehler = 0, vergleiche = 0;
  double cpu_anzeige = 0, abw[3] = {0, 0, 0};
  hcgpu::GLuint tex = 0;
  int tb = 0, th = 0;
  std::vector<uint8_t> rgba, gelesen;

  int32_t Decoded(webrtc::VideoFrame& f) override {
    ++bilder;
    const double c0 = CpuMs();
    // So wie die Anzeige es tut: über das Wrapper-Bild.
    auto rahmen = libwebrtc::scoped_refptr<libwebrtc::VideoFrameBufferImpl>(
        new libwebrtc::RefCountedObject<libwebrtc::VideoFrameBufferImpl>(f.video_frame_buffer()));
    libwebrtc::nullkopie::HcGpuBild g{};
    g.version = 1;
    bool gezeichnet = false;
    if (honeycord_gpu_bild(static_cast<libwebrtc::RTCVideoFrame*>(rahmen.get()), &g)) {
      hcgpu::Bild b;
      std::memcpy(&b, &g, sizeof b);
      gezeichnet = z->Zeichnen(b, &tex, &tb, &th);
      if (gezeichnet) ++gpu_bilder;
      else ++fehler;
    }
    if (!gezeichnet) {
      // Alter Weg (mit dem Sofortgewinn: keine Dreh-/Skalierkopie).
      auto i4 = f.video_frame_buffer()->ToI420();
      rgba.resize(static_cast<size_t>(i4->width()) * i4->height() * 4);
      libyuv::I420ToABGR(i4->DataY(), i4->StrideY(), i4->DataU(), i4->StrideU(), i4->DataV(),
                         i4->StrideV(), rgba.data(), i4->width() * 4, i4->width(), i4->height());
      z->Hochladen(rgba.data(), i4->width(), i4->height(), &tex, &tb, &th);
    }
    cpu_anzeige += CpuMs() - c0;
    // Farbvergleich alle 50 Bilder: GPU-Ergebnis gegen libyuv.
    if (gezeichnet && bilder % 50 == 1) {
      auto i4 = f.video_frame_buffer()->ToI420();
      const int w = std::min(i4->width(), tb), h = std::min(i4->height(), th);
      rgba.resize(static_cast<size_t>(i4->width()) * i4->height() * 4);
      libyuv::I420ToABGR(i4->DataY(), i4->StrideY(), i4->DataU(), i4->StrideU(), i4->DataV(),
                         i4->StrideV(), rgba.data(), i4->width() * 4, i4->width(), i4->height());
      gelesen.resize(static_cast<size_t>(tb) * th * 4);
      z->glBindFramebuffer(hcgpu::kFramebuffer, z->fbo());
      z->glFramebufferTexture2D(hcgpu::kFramebuffer, hcgpu::kColorAttachment0, hcgpu::kTexture2D, tex, 0);
      gl->glReadPixels(0, 0, tb, th, hcgpu::kRGBA, hcgpu::kUByte, gelesen.data());
      z->glBindFramebuffer(hcgpu::kFramebuffer, 0);
      double s[3] = {0, 0, 0};
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          for (int c = 0; c < 3; ++c)
            s[c] += std::abs(int(gelesen[(static_cast<size_t>(y) * tb + x) * 4 + c]) -
                             int(rgba[(static_cast<size_t>(y) * i4->width() + x) * 4 + c]));
      for (int c = 0; c < 3; ++c) abw[c] += s[c] / (static_cast<double>(w) * h);
      ++vergleiche;
    }
    return 0;
  }
};

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) { std::printf("Aufruf: empfang_test strom.h264 [runden]\n"); return 2; }
  const int runden = argc > 2 ? std::atoi(argv[2]) : 2;
  FILE* f = std::fopen(argv[1], "rb");
  if (!f) return 2;
  std::vector<uint8_t> roh;
  uint8_t puffer[65536];
  size_t n;
  while ((n = std::fread(puffer, 1, sizeof puffer, f)) > 0) roh.insert(roh.end(), puffer, puffer + n);
  std::fclose(f);
  std::vector<bool> key;
  const auto einheiten = Zerlegen(roh, &key);

  if (!libwebrtc::nullkopie::Einrichten()) { std::printf("FEHL: Null-Kopie nicht aktiv\n"); return 1; }
  Gl gl;
  if (!gl.Auf()) { std::printf("FEHL: GL\n"); return 1; }
  hcgpu::Zeichner z;
  if (!z.Einrichten(gl.gp, gl.anzeige, hcgpu::Dialekt::kES)) { std::printf("FEHL: Zeichner\n"); return 1; }

  const webrtc::Environment env = webrtc::CreateEnvironment();
  webrtc::VideoDecoder::Settings einst;
  einst.set_codec_type(webrtc::kVideoCodecH264);
  einst.set_number_of_cores(1);

  double cpu[2] = {0, 0}, anz[2] = {0, 0};
  int bilder[2] = {0, 0};
  Sammler ergebnis[2];
  for (int weg = 0; weg < 2; ++weg) {
    libwebrtc::nullkopie::EmpfangMelden(weg == 1);
    libwebrtc::HwH264Decoder hw;
    Sammler& s = ergebnis[weg];
    s.z = &z;
    s.gl = &gl;
    if (!hw.Configure(einst)) { std::printf("FEHL: Configure\n"); return 1; }
    hw.RegisterDecodeCompleteCallback(&s);
    const double c0 = CpuMs();
    for (int r = 0; r < runden; ++r) {
      for (size_t i = 0; i < einheiten.size(); ++i) {
        webrtc::EncodedImage img;
        img.SetEncodedData(webrtc::EncodedImageBuffer::Create(einheiten[i].data(), einheiten[i].size()));
        img.SetRtpTimestamp(static_cast<uint32_t>((r * einheiten.size() + i) * 3000));
        img._frameType = key[i] ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta;
        hw.Decode(img, /*missing_frames=*/false, /*render_time_ms=*/0);
      }
    }
    cpu[weg] = CpuMs() - c0;
    anz[weg] = s.cpu_anzeige;
    bilder[weg] = s.bilder;
    hw.Release();
    z.TexturWeg(&s.tex);
  }
  const Sammler& g = ergebnis[1];
  std::printf("Alter Weg: %d Bilder, CPU %.2f ms/Bild gesamt (davon Anzeige %.2f)\n", bilder[0],
              cpu[0] / bilder[0], anz[0] / bilder[0]);
  std::printf("GPU-Weg:   %d Bilder, %d über die Grafikkarte, %d Fehler, CPU %.2f ms/Bild gesamt (davon Anzeige %.2f)\n",
              bilder[1], g.gpu_bilder, g.fehler, cpu[1] / std::max(1, bilder[1]), anz[1] / std::max(1, bilder[1]));
  const double ar = g.vergleiche ? g.abw[0] / g.vergleiche : 99, ag = g.vergleiche ? g.abw[1] / g.vergleiche : 99,
               ab = g.vergleiche ? g.abw[2] / g.vergleiche : 99;
  std::printf("Farbvergleich (%d Bilder): R %.2f · G %.2f · B %.2f (mittlere Abweichung)\n", g.vergleiche, ar, ag, ab);
  const bool gut = g.gpu_bilder == bilder[1] && bilder[1] > 0 && g.fehler == 0 && ar <= 2 && ag <= 2 && ab <= 2 &&
                   cpu[1] < cpu[0];
  std::printf("%s\n", gut ? "BESTANDEN" : "FEHL");
  return gut ? 0 : 1;
}
