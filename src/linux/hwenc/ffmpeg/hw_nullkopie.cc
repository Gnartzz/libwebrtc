#include "src/linux/hwenc/ffmpeg/hw_nullkopie.h"

#include <dlfcn.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <utility>

extern "C" {
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_vaapi.h"
}

#include "api/make_ref_counted.h"
#include "api/video/i420_buffer.h"
#include "libyuv/convert.h"
#include "modules/desktop_capture/linux/wayland/honeycord_dmabuf.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"
#include "src/linux/hwenc/ffmpeg/ffmpeg_lader.h"
#include "src/linux/hwenc/ffmpeg/hw_h264_encoder.h"
#include "src/linux/hwenc/hw_gemeinsam.h"

namespace libwebrtc {
namespace nullkopie {
namespace {

// ── libva über dlsym ────────────────────────────────────────────────────────
// ★ FFmpeg ist RTLD_LOCAL geladen, libva damit nicht im globalen Namensraum
// (gemessen 17.10.). RTLD_NOLOAD gibt den Griff auf die libva, die libavutil
// schon geladen hat — wir binden nichts eigenes.
struct VaFn {
  VAStatus (*vaCreateSurfaces)(VADisplay, unsigned int, unsigned int, unsigned int,
                               VASurfaceID*, unsigned int, VASurfaceAttrib*, unsigned int);
  VAStatus (*vaDestroySurfaces)(VADisplay, VASurfaceID*, int);
  VAStatus (*vaCreateConfig)(VADisplay, VAProfile, VAEntrypoint, VAConfigAttrib*, int,
                             VAConfigID*);
  VAStatus (*vaDestroyConfig)(VADisplay, VAConfigID);
  VAStatus (*vaCreateContext)(VADisplay, VAConfigID, int, int, int, VASurfaceID*, int,
                              VAContextID*);
  VAStatus (*vaDestroyContext)(VADisplay, VAContextID);
  VAStatus (*vaCreateBuffer)(VADisplay, VAContextID, VABufferType, unsigned int,
                             unsigned int, void*, VABufferID*);
  VAStatus (*vaDestroyBuffer)(VADisplay, VABufferID);
  VAStatus (*vaBeginPicture)(VADisplay, VAContextID, VASurfaceID);
  VAStatus (*vaRenderPicture)(VADisplay, VAContextID, VABufferID*, int);
  VAStatus (*vaEndPicture)(VADisplay, VAContextID);
  VAStatus (*vaSyncSurface)(VADisplay, VASurfaceID);
  const char* (*vaQueryVendorString)(VADisplay);
  const char* (*vaErrorStr)(VAStatus);
  VAStatus (*vaExportSurfaceHandle)(VADisplay, VASurfaceID, uint32_t, uint32_t, void*);
};

template <typename T>
bool Hole(void* h, const char* name, T* ziel) {
  *ziel = reinterpret_cast<T>(::dlsym(h, name));
  return *ziel != nullptr;
}

double CpuMs() {
  timespec t;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

// DRM-Formatkennungen (drm_fourcc.h), hier fest, um keine libdrm-Header
// vorauszusetzen.
constexpr uint32_t Fourcc(char a, char b, char c, char d) {
  return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |
         (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
}
constexpr uint64_t kModInvalid = 0x00ffffffffffffffULL;

// SPA_VIDEO_FORMAT_* (spa/param/video/raw.h): RGBx=7, BGRx=8, RGBA=11, BGRA=12.
// Speicherreihenfolge → DRM-Format → VA-Format.
bool FormatAus(uint32_t spa, uint32_t* drm, uint32_t* va) {
  switch (spa) {
    case 8:  *drm = Fourcc('X', 'R', '2', '4'); *va = VA_FOURCC_BGRX; return true;
    case 12: *drm = Fourcc('A', 'R', '2', '4'); *va = VA_FOURCC_BGRA; return true;
    case 7:  *drm = Fourcc('X', 'B', '2', '4'); *va = VA_FOURCC_RGBX; return true;
    case 11: *drm = Fourcc('A', 'B', '2', '4'); *va = VA_FOURCC_RGBA; return true;
  }
  return false;
}

class Va;
Va* g_va = nullptr;          // lebt bis zum Prozessende
std::atomic<bool> g_aktiv{false};
// Laufende Bildschirmaufnahmen (Start..Stop). Ohne eine erzeugt der Wandler
// nichts — nach Stop() läuft der PipeWire-Strom bis zum Abbau weiter, und
// jedes Bild legte die eben freigegebenen Vorräte neu an (Nachprüfung M5).
std::atomic<int> g_aufnahmen{0};

// Zahlen für die 30-s-Zeile in hwenc.log.
std::atomic<uint64_t> z_bilder{0}, z_fehl{0}, z_skal{0}, z_i420{0}, z_direkt{0};
std::atomic<uint64_t> z_wandel_us{0}, z_i420_us{0};
// Für die Aussetz-Regel (Prüfbefund M3): erzeugte und zurückgelesene Pixel.
std::atomic<uint64_t> p_erzeugt{0}, p_gelesen{0};

class Va {
 public:
  VaFn fn{};
  AVBufferRef* geraet = nullptr;
  VADisplay dpy = nullptr;
  VAConfigID vpp_cfg = VA_INVALID_ID;

  // Umrechnung (Farbe + Zuschnitt + Skalierung) in eine frische NV12-Oberfläche
  // aus dem Vorrat für `zb`×`zh`. Wartet, bis die Grafikkarte fertig ist.
  // nullptr bei Fehler.
  AVFrame* Vpp(VASurfaceID quelle, const VARectangle& bereich, int zb, int zh, bool rgb) {
    const auto& api = ffmpeg::Zugriff();
    std::lock_guard<std::mutex> sperre(m_);
    Ziel* z = ZielFuer(zb, zh);
    if (!z) return nullptr;
    AVFrame* f = api.av_frame_alloc();
    if (!f) return nullptr;
    if (api.av_hwframe_get_buffer(z->vorrat, f, 0) < 0) {
      api.av_frame_free(&f);
      return nullptr;
    }
    VASurfaceID ziel = static_cast<VASurfaceID>(reinterpret_cast<uintptr_t>(f->data[3]));
    VAProcPipelineParameterBuffer pp;
    std::memset(&pp, 0, sizeof pp);
    pp.surface = quelle;
    pp.surface_region = &bereich;
    pp.output_region = nullptr;
    pp.output_background_color = 0xff000000;
    pp.filter_flags = VA_FILTER_SCALING_DEFAULT;
    // ★ BT.601 begrenzt — genau das, was libyuv ARGBToI420 im alten Weg
    // erzeugt (gemessen: Y mittel 0,06 Abweichung). Für NV12→NV12 ebenso, so
    // bleibt ein Zuschnitt farbneutral.
    pp.surface_color_standard = VAProcColorStandardBT601;
    pp.output_color_standard = VAProcColorStandardBT601;
    (void)rgb;
    VABufferID buf = VA_INVALID_ID;
    VAStatus s = fn.vaCreateBuffer(dpy, z->ctx, VAProcPipelineParameterBufferType, sizeof pp, 1,
                                   &pp, &buf);
    if (s == VA_STATUS_SUCCESS) s = fn.vaBeginPicture(dpy, z->ctx, ziel);
    if (s == VA_STATUS_SUCCESS) s = fn.vaRenderPicture(dpy, z->ctx, &buf, 1);
    if (s == VA_STATUS_SUCCESS) s = fn.vaEndPicture(dpy, z->ctx);
    if (buf != VA_INVALID_ID) fn.vaDestroyBuffer(dpy, buf);
    // ★ Warten ist Pflicht: Der PipeWire-Puffer geht gleich zurück an den
    // Compositor, und der Encoder liest die Oberfläche aus einem anderen
    // VA-Kontext.
    if (s == VA_STATUS_SUCCESS) s = fn.vaSyncSurface(dpy, ziel);
    if (s != VA_STATUS_SUCCESS) {
      FehlerEinmal("VPP", s);
      api.av_frame_free(&f);
      return nullptr;
    }
    return f;
  }

  // Alle Kontexte und Vorräte abgeben (Prüfbefund M5). Bilder, die noch
  // leben, halten ihren Vorrat selbst; ein neuer Bedarf legt neu an.
  void Leeren() {
    const auto& api = ffmpeg::Zugriff();
    std::lock_guard<std::mutex> sperre(m_);
    for (auto& [groesse, z] : ziele_) {
      fn.vaDestroyContext(dpy, z.ctx);
      api.av_buffer_unref(&z.vorrat);
    }
    ziele_.clear();
  }

  void FehlerEinmal(const char* wo, VAStatus s) {
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 5) Protokoll("Null-Kopie: %s scheitert (%s)", wo, fn.vaErrorStr(s));
  }

 private:
  struct Ziel {
    VAContextID ctx = VA_INVALID_ID;
    AVBufferRef* vorrat = nullptr;
    uint64_t benutzt = 0;
  };

  // Je Ausgabegröße ein VPP-Kontext und ein Vorrat (dynamisch: initial 0 —
  // die Bilder werden gehalten von Warteschlange, Encoder, Vorschau und
  // WebRTCs Taktgeber, ein fester Vorrat liefe leer). Höchstens 6 Größen.
  Ziel* ZielFuer(int zb, int zh) {
    const auto& api = ffmpeg::Zugriff();
    auto it = ziele_.find({zb, zh});
    if (it != ziele_.end()) {
      it->second.benutzt = ++uhr_;
      return &it->second;
    }
    if (ziele_.size() >= 6) {
      auto alt = ziele_.begin();
      for (auto i = ziele_.begin(); i != ziele_.end(); ++i)
        if (i->second.benutzt < alt->second.benutzt) alt = i;
      fn.vaDestroyContext(dpy, alt->second.ctx);
      // Bilder halten ihren Vorrat selbst am Leben.
      api.av_buffer_unref(&alt->second.vorrat);
      ziele_.erase(alt);
    }
    Ziel z;
    z.vorrat = api.av_hwframe_ctx_alloc(geraet);
    if (!z.vorrat) return nullptr;
    auto* fc = reinterpret_cast<AVHWFramesContext*>(z.vorrat->data);
    fc->format = AV_PIX_FMT_VAAPI;
    fc->sw_format = AV_PIX_FMT_NV12;
    fc->width = zb;
    fc->height = zh;
    fc->initial_pool_size = 0;
    if (api.av_hwframe_ctx_init(z.vorrat) < 0) {
      api.av_buffer_unref(&z.vorrat);
      Protokoll("Null-Kopie: Bildvorrat %dx%d nicht anzulegen", zb, zh);
      return nullptr;
    }
    VAStatus s = fn.vaCreateContext(dpy, vpp_cfg, zb, zh, VA_PROGRESSIVE, nullptr, 0, &z.ctx);
    if (s != VA_STATUS_SUCCESS) {
      api.av_buffer_unref(&z.vorrat);
      FehlerEinmal("VPP-Kontext", s);
      return nullptr;
    }
    z.benutzt = ++uhr_;
    return &(ziele_[{zb, zh}] = z);
  }

  std::mutex m_;
  std::map<std::pair<int, int>, Ziel> ziele_;
  uint64_t uhr_ = 0;
};

// ── Das GPU-Bild ────────────────────────────────────────────────────────────
class HcVaBild : public webrtc::VideoFrameBuffer {
 public:
  // `aus_aufnahme`: Bild des Bildschirm-Aufnehmers (zählt für die
  // Aussetz-Regel, Prüfbefund M3). Empfangene Bilder (Decoder) zählen nicht —
  // sonst schaltete ein Zurücklesen beim Empfang die Null-Kopie beim Senden ab.
  HcVaBild(AVFrame* rahmen, int breite, int hoehe, bool aus_aufnahme = true)
      : rahmen_(rahmen), breite_(breite), hoehe_(hoehe), aus_aufnahme_(aus_aufnahme) {}
  bool aus_aufnahme() const { return aus_aufnahme_; }
  ~HcVaBild() override { ffmpeg::Zugriff().av_frame_free(&rahmen_); }

  Type type() const override { return Type::kNative; }
  int width() const override { return breite_; }
  int height() const override { return hoehe_; }
  const AVFrame* rahmen() const { return rahmen_; }

  // Notweg für alle, die kein GPU-Bild verstehen. Einmal je Bild, danach aus
  // dem Zwischenspeicher (Vorschau und Software-Encoder teilen sich das).
  webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override {
    std::lock_guard<std::mutex> sperre(m_);
    if (i420_) return i420_;
    const double c0 = CpuMs();
    const auto& api = ffmpeg::Zugriff();
    AVFrame* sw = api.av_frame_alloc();
    if (!sw) return nullptr;
    sw->format = AV_PIX_FMT_NV12;
    if (api.av_hwframe_transfer_data(sw, rahmen_, 0) < 0) {
      api.av_frame_free(&sw);
      static std::atomic<int> n{0};
      if (n.fetch_add(1) < 5) Protokoll("Null-Kopie: Zurücklesen scheitert");
      return nullptr;
    }
    auto puffer = webrtc::I420Buffer::Create(breite_, hoehe_);
    libyuv::NV12ToI420(sw->data[0], sw->linesize[0], sw->data[1], sw->linesize[1],
                       puffer->MutableDataY(), puffer->StrideY(), puffer->MutableDataU(),
                       puffer->StrideU(), puffer->MutableDataV(), puffer->StrideV(), breite_,
                       hoehe_);
    api.av_frame_free(&sw);
    i420_ = puffer;
    z_i420++;
    if (aus_aufnahme_) p_gelesen += static_cast<uint64_t>(breite_) * hoehe_;
    z_i420_us += static_cast<uint64_t>((CpuMs() - c0) * 1000);
    return i420_;
  }

  // Zuschneiden und Skalieren auf der Grafikkarte (WebRTCs Ausrichtung auf
  // 16, Auflösungsstufen, Vorschau in Kachelgröße).
  webrtc::scoped_refptr<webrtc::VideoFrameBuffer> CropAndScale(int ox, int oy, int cb, int ch,
                                                               int sb, int sh) override {
    if (ox == 0 && oy == 0 && cb == breite_ && ch == hoehe_ && sb == breite_ && sh == hoehe_)
      return webrtc::scoped_refptr<webrtc::VideoFrameBuffer>(this);
    if (g_va && sb >= 2 && sh >= 2 && (sb % 2) == 0 && (sh % 2) == 0 && ox >= 0 && oy >= 0 &&
        ox + cb <= breite_ && oy + ch <= hoehe_) {
      VARectangle r;
      r.x = static_cast<int16_t>(ox);
      r.y = static_cast<int16_t>(oy);
      r.width = static_cast<uint16_t>(cb);
      r.height = static_cast<uint16_t>(ch);
      VASurfaceID quelle =
          static_cast<VASurfaceID>(reinterpret_cast<uintptr_t>(rahmen_->data[3]));
      if (AVFrame* f = g_va->Vpp(quelle, r, sb, sh, false)) {
        z_skal++;
        return webrtc::make_ref_counted<HcVaBild>(f, sb, sh, aus_aufnahme_);
      }
    }
    // Rückfall: über I420 (langsam, aber richtig). ★ Prüfbefund S2: Der
    // allgemeine Weg in WebRTC dereferenziert ToI420() ungeprüft — scheitert
    // auch das Zurücklesen (GPU-Reset, Suspend), wird das Bild verworfen.
    webrtc::scoped_refptr<webrtc::I420BufferInterface> i420 = ToI420();
    if (!i420) return nullptr;
    return i420->CropAndScale(ox, oy, cb, ch, sb, sh);
  }

 private:
  AVFrame* rahmen_;
  const int breite_;
  const int hoehe_;
  const bool aus_aufnahme_;
  std::mutex m_;
  webrtc::scoped_refptr<webrtc::I420BufferInterface> i420_;
};

// ── Der Wandler am PipeWire-Aufnehmer ───────────────────────────────────────
class Wandler : public webrtc::HcDmaBufWandler {
 public:
  webrtc::scoped_refptr<webrtc::RefCountInterface> Wandeln(
      const webrtc::HcDmaBufBild& b) override {
    const double c0 = CpuMs();
    auto ergebnis = Machen(b);
    if (ergebnis) {
      z_bilder++;
      z_wandel_us += static_cast<uint64_t>((CpuMs() - c0) * 1000);
    } else {
      z_fehl++;
    }
    Bericht();
    return ergebnis;
  }

  bool ModifierUnbrauchbar(const webrtc::HcDmaBufBild& b) override {
    std::lock_guard<std::mutex> sperre(m_);
    return gescheitert_.count(Schluessel(b)) > 0;
  }

 private:
  // ★ Prüfbefund M3, GEMESSEN 08.11. (Intel UHD 630): Nimmt kein VA-Encoder
  // das GPU-Bild (VP8, Software-Rückfall), liest jeder Verbraucher es zurück.
  // Bei 1080p ohne Verkleinerung kostet das 6,0 statt 5,0 ms je Bild — also
  // mehr als der alte Weg; mit Verkleinerung (2880×1800 → 1152×720) 3,1
  // statt 12,4 ms — weniger. Die Kosten wachsen mit den zurückgelesenen
  // Pixeln. Regel: Wird in 5 s mehr als 30 % der erzeugten Fläche
  // zurückgelesen, setzt der GPU-Weg 30 s aus (alter Weg), dann neuer Versuch.
  // Die Vorschau in Kachelgröße (15 fps) liegt weit darunter.
  bool Ausgesetzt() {
    const int64_t jetzt = webrtc::TimeMillis();
    if (jetzt < pause_bis_ms_) return true;
    if (pause_bis_ms_ != 0) {
      pause_bis_ms_ = 0;
      fenster_ms_ = jetzt;
      p_erzeugt = 0;
      p_gelesen = 0;
      Protokoll("Null-Kopie: neuer Versuch über die Grafikkarte");
    }
    if (fenster_ms_ == 0) fenster_ms_ = jetzt;
    if (jetzt - fenster_ms_ >= 5000) {
      const uint64_t e = p_erzeugt.exchange(0), g = p_gelesen.exchange(0);
      fenster_ms_ = jetzt;
      if (e > 0 && g * 10 > e * 3) {
        pause_bis_ms_ = jetzt + 30000;
        Protokoll("Null-Kopie: %.0f %% der Bildfläche zurückgelesen (kein VA-Encoder nimmt die "
                  "GPU-Bilder) — 30 s alter Weg",
                  100.0 * static_cast<double>(g) / static_cast<double>(e));
        return true;
      }
    }
    return false;
  }

  webrtc::scoped_refptr<webrtc::RefCountInterface> Machen(const webrtc::HcDmaBufBild& b) {
    Va* va = g_va;
    if (!va || g_aufnahmen.load() == 0) return nullptr;
    {
      std::lock_guard<std::mutex> sperre(m_);
      if (Ausgesetzt()) return nullptr;
    }
    uint32_t drm = 0, vaf = 0;
    if (!FormatAus(b.spa_format, &drm, &vaf)) {
      Einmal(&warn_format_, "Null-Kopie: Format %u nicht unterstützt — alter Weg", b.spa_format);
      return nullptr;
    }
    // ★ Ein impliziter Modifier sagt nicht, wie der Puffer gekachelt ist —
    // VA würde raten. Dann lieber der bewährte Weg über EGL.
    if (b.modifier == kModInvalid) {
      Einmal(&warn_mod_, "Null-Kopie: Puffer ohne Modifier — alter Weg");
      return nullptr;
    }
    {
      std::lock_guard<std::mutex> sperre(m_);
      if (gescheitert_.count(Schluessel(b))) return nullptr;
    }
    const int ab = b.breite & ~1, ah = b.hoehe & ~1;
    if (ab < 2 || ah < 2 || b.x < 0 || b.y < 0 || b.x + b.breite > b.strom_breite ||
        b.y + b.hoehe > b.strom_hoehe)
      return nullptr;

    VADRMPRIMESurfaceDescriptor d;
    std::memset(&d, 0, sizeof d);
    d.fourcc = vaf;
    d.width = static_cast<uint32_t>(b.strom_breite);
    d.height = static_cast<uint32_t>(b.strom_hoehe);
    d.num_layers = 1;
    d.layers[0].drm_format = drm;
    d.layers[0].num_planes = static_cast<uint32_t>(b.ebenen);
    for (int i = 0; i < b.ebenen; ++i) {
      // Gleiche Dateien teilen sich ein Objekt (mehrere Ebenen in einem Puffer).
      int obj = -1;
      for (uint32_t k = 0; k < d.num_objects; ++k) {
        if (Gleich(d.objects[k].fd, b.ebene[i].fd)) obj = static_cast<int>(k);
      }
      if (obj < 0) {
        obj = static_cast<int>(d.num_objects++);
        d.objects[obj].fd = b.ebene[i].fd;
        const off_t ende = ::lseek(b.ebene[i].fd, 0, SEEK_END);
        ::lseek(b.ebene[i].fd, 0, SEEK_SET);
        d.objects[obj].size = ende > 0 ? static_cast<uint32_t>(ende) : 0;
        d.objects[obj].drm_format_modifier = b.modifier;
      }
      d.layers[0].object_index[i] = static_cast<uint32_t>(obj);
      d.layers[0].offset[i] = b.ebene[i].offset;
      d.layers[0].pitch[i] = b.ebene[i].stride;
    }
    VASurfaceAttrib at[2];
    std::memset(at, 0, sizeof at);
    at[0].type = VASurfaceAttribMemoryType;
    at[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    at[0].value.type = VAGenericValueTypeInteger;
    at[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    at[1].type = VASurfaceAttribExternalBufferDescriptor;
    at[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    at[1].value.type = VAGenericValueTypePointer;
    at[1].value.value.p = &d;
    VASurfaceID quelle = VA_INVALID_SURFACE;
    VAStatus s = va->fn.vaCreateSurfaces(va->dpy, VA_RT_FORMAT_RGB32, d.width, d.height,
                                         &quelle, 1, at, 2);
    if (s != VA_STATUS_SUCCESS) {
      // Dreimal in Folge mit demselben Modifier, Format und derselben Größe:
      // aufgeben (Prüfbefund M4: nicht für jeden Strom und jede Größe).
      std::lock_guard<std::mutex> sperre(m_);
      if (++fehl_folge_[Schluessel(b)] >= 3) {
        gescheitert_.insert(Schluessel(b));
        Protokoll("Null-Kopie: Modifier 0x%llx lässt sich nicht übernehmen (%s) — "
                  "neu verhandeln, sonst alter Weg",
                  static_cast<unsigned long long>(b.modifier), va->fn.vaErrorStr(s));
      }
      return nullptr;
    }
    VARectangle r;
    r.x = static_cast<int16_t>(b.x);
    r.y = static_cast<int16_t>(b.y);
    r.width = static_cast<uint16_t>(ab);
    r.height = static_cast<uint16_t>(ah);
    AVFrame* f = va->Vpp(quelle, r, ab, ah, true);
    va->fn.vaDestroySurfaces(va->dpy, &quelle, 1);
    if (!f) return nullptr;
    {
      std::lock_guard<std::mutex> sperre(m_);
      fehl_folge_[Schluessel(b)] = 0;
      if (!erstes_) {
        erstes_ = true;
        Protokoll("Null-Kopie: erstes Bild %dx%d (Strom %dx%d, Format %u, Modifier 0x%llx, "
                  "%d Ebene(n)) direkt auf der Grafikkarte",
                  ab, ah, b.strom_breite, b.strom_hoehe, b.spa_format,
                  static_cast<unsigned long long>(b.modifier), b.ebenen);
      }
    }
    p_erzeugt += static_cast<uint64_t>(ab) * ah;
    return webrtc::scoped_refptr<webrtc::RefCountInterface>(
        webrtc::make_ref_counted<HcVaBild>(f, ab, ah));
  }

  using Key = std::tuple<uint64_t, uint32_t, int, int>;
  static Key Schluessel(const webrtc::HcDmaBufBild& b) {
    return Key(b.modifier, b.spa_format, b.strom_breite, b.strom_hoehe);
  }

  // Zwei Deskriptoren, dieselbe Datei? (PipeWire reicht je Ebene einen eigenen.)
  static bool Gleich(int a, int b) {
    if (a == b) return true;
    struct stat sa, sb;
    if (::fstat(a, &sa) != 0 || ::fstat(b, &sb) != 0) return false;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
  }

  template <typename... A>
  void Einmal(bool* schon, const char* f, A... a) {
    std::lock_guard<std::mutex> sperre(m_);
    if (*schon) return;
    *schon = true;
    Protokoll(f, a...);
  }

  // Alle 30 s: wie viel lief über die Grafikkarte, und was kostete es.
  void Bericht() {
    const int64_t jetzt = webrtc::TimeMillis();
    int64_t start = bericht_ms_.load();
    if (start == 0) {
      bericht_ms_.compare_exchange_strong(start, jetzt);
      return;
    }
    if (jetzt - start < 30000 || !bericht_ms_.compare_exchange_strong(start, jetzt)) return;
    const uint64_t n = z_bilder.exchange(0), fehl = z_fehl.exchange(0);
    const uint64_t wus = z_wandel_us.exchange(0), i4 = z_i420.exchange(0),
                   i4us = z_i420_us.exchange(0), sk = z_skal.exchange(0),
                   di = z_direkt.exchange(0);
    Protokoll("Null-Kopie (30 s): %llu Bilder über die Grafikkarte (%.2f ms CPU je Bild), "
              "%llu alter Weg, %llu direkt in den Encoder, %llu skaliert, "
              "%llu zurückgelesen (%.2f ms je Bild)",
              static_cast<unsigned long long>(n), n ? wus / 1000.0 / n : 0.0,
              static_cast<unsigned long long>(fehl), static_cast<unsigned long long>(di),
              static_cast<unsigned long long>(sk), static_cast<unsigned long long>(i4),
              i4 ? i4us / 1000.0 / i4 : 0.0);
  }

  std::mutex m_;
  std::set<Key> gescheitert_;
  std::map<Key, int> fehl_folge_;
  bool warn_format_ = false, warn_mod_ = false, erstes_ = false;
  int64_t pause_bis_ms_ = 0, fenster_ms_ = 0;   // unter m_
  std::atomic<int64_t> bericht_ms_{0};
};

bool EinrichtenIntern() {
  const char* aus = std::getenv("HONEYCORD_NULLKOPIE");
  if (aus && std::strcmp(aus, "0") == 0) {
    Protokoll("Null-Kopie: HONEYCORD_NULLKOPIE=0 — abgeschaltet");
    return false;
  }
  if (!HwH264Encoder::IstVerfuegbar() ||
      HwH264Encoder::GewaehlterWeg() != HwH264Encoder::Backend::kVaapi) {
    Protokoll("Null-Kopie: kein VA-API-Encoder — alter Weg");
    return false;
  }
  void* lib = ::dlopen("libva.so.2", RTLD_NOW | RTLD_NOLOAD);
  if (!lib) {
    Protokoll("Null-Kopie: libva nicht geladen — alter Weg");
    return false;
  }
  auto* va = new Va();
  bool ok = true;
  ok &= Hole(lib, "vaCreateSurfaces", &va->fn.vaCreateSurfaces);
  ok &= Hole(lib, "vaDestroySurfaces", &va->fn.vaDestroySurfaces);
  ok &= Hole(lib, "vaCreateConfig", &va->fn.vaCreateConfig);
  ok &= Hole(lib, "vaDestroyConfig", &va->fn.vaDestroyConfig);
  ok &= Hole(lib, "vaCreateContext", &va->fn.vaCreateContext);
  ok &= Hole(lib, "vaDestroyContext", &va->fn.vaDestroyContext);
  ok &= Hole(lib, "vaCreateBuffer", &va->fn.vaCreateBuffer);
  ok &= Hole(lib, "vaDestroyBuffer", &va->fn.vaDestroyBuffer);
  ok &= Hole(lib, "vaBeginPicture", &va->fn.vaBeginPicture);
  ok &= Hole(lib, "vaRenderPicture", &va->fn.vaRenderPicture);
  ok &= Hole(lib, "vaEndPicture", &va->fn.vaEndPicture);
  ok &= Hole(lib, "vaSyncSurface", &va->fn.vaSyncSurface);
  ok &= Hole(lib, "vaQueryVendorString", &va->fn.vaQueryVendorString);
  ok &= Hole(lib, "vaErrorStr", &va->fn.vaErrorStr);
  ok &= Hole(lib, "vaExportSurfaceHandle", &va->fn.vaExportSurfaceHandle);
  // `lib` bleibt offen (nur ein Referenzzähler; libavutil hält sie ohnehin).
  if (!ok) {
    Protokoll("Null-Kopie: libva ohne alle Funktionen — alter Weg");
    delete va;
    return false;
  }
  const auto& api = ffmpeg::Zugriff();
  int r = api.av_hwdevice_ctx_create(&va->geraet, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
  if (r < 0 || !va->geraet) {
    Protokoll("Null-Kopie: VA-Gerät nicht zu öffnen (%s) — alter Weg",
              ffmpeg::FehlerText(r).c_str());
    delete va;
    return false;
  }
  auto* hw = reinterpret_cast<AVHWDeviceContext*>(va->geraet->data);
  va->dpy = reinterpret_cast<AVVAAPIDeviceContext*>(hw->hwctx)->display;
  const char* hersteller = va->fn.vaQueryVendorString(va->dpy);
  const std::string h = hersteller ? hersteller : "?";
  // nvidia-vaapi-driver kann nur dekodieren; Gallium-VDPAU-Brücken ebenso.
  if (h.find("NVIDIA") != std::string::npos || h.find("VDPAU") != std::string::npos) {
    Protokoll("Null-Kopie: Treiber %s — alter Weg", h.c_str());
    api.av_buffer_unref(&va->geraet);
    delete va;
    return false;
  }
  VAStatus s = va->fn.vaCreateConfig(va->dpy, VAProfileNone, VAEntrypointVideoProc, nullptr, 0,
                                     &va->vpp_cfg);
  if (s != VA_STATUS_SUCCESS) {
    Protokoll("Null-Kopie: Treiber %s ohne Bildumrechnung (VPP: %s) — alter Weg", h.c_str(),
              va->fn.vaErrorStr(s));
    api.av_buffer_unref(&va->geraet);
    delete va;
    return false;
  }
  g_va = va;
  static Wandler* wandler = new Wandler();
  webrtc::HcDmaBufWandlerPlatz().store(wandler, std::memory_order_release);
  Protokoll("Null-Kopie: an (%s) — Bildschirmbilder bleiben auf der Grafikkarte", h.c_str());
  return true;
}

}  // namespace

bool Einrichten() {
  static const bool an = [] {
    const bool a = EinrichtenIntern();
    g_aktiv.store(a);
    return a;
  }();
  return an;
}

bool Aktiv() { return g_aktiv.load(); }

AVBufferRef* GeraetRef() {
  if (!Aktiv() || !g_va) return nullptr;
  return ffmpeg::Zugriff().av_buffer_ref(g_va->geraet);
}

const AVFrame* AlsVaRahmen(const webrtc::VideoFrameBuffer& puffer, const AVBufferRef* geraet,
                           int breite, int hoehe) {
  if (puffer.type() != webrtc::VideoFrameBuffer::Type::kNative || !g_va || !geraet) return nullptr;
  const auto* bild = dynamic_cast<const HcVaBild*>(&puffer);
  if (!bild || bild->width() != breite || bild->height() != hoehe) return nullptr;
  if (geraet->data != g_va->geraet->data) return nullptr;
  return bild->rahmen();
}

void EncoderDirektGezaehlt() { z_direkt++; }

// ── Empfang ohne Umweg (10.11.2026) ─────────────────────────────────────────
//
// ★ GEMESSEN (CT 107, UHD 630, 1080p): Dekodieren 0,2 ms, aber Zurückkopieren
// 2,3 ms, Wrapper-Kopien + I420ToABGR 2,1 ms, glTexImage2D 0,9 ms — rund
// 5,9 ms CPU je fremdem Bild. Bleibt das Bild auf der Grafikkarte und
// übernimmt die Anzeige es als DMA-BUF, bleibt ~0,5 ms.
//
// Der Decoder gibt GPU-Bilder NUR aus, wenn die Anzeige sie übernehmen kann:
// Das Plugin meldet sich (`honeycord_empfang_gpu_bereit`), sobald sein
// GL-Teil steht. Sonst (X11, abgelöstes Fenster ohne GL, Fehler) bleibt
// alles beim heutigen Weg — ein GPU-Bild, das doch zurückgelesen werden
// müsste, wäre nur ein Umweg mehr.

namespace {
std::atomic<bool> g_empfang_gemeldet{false};
}

void EmpfangMelden(bool an) {
  const bool vorher = g_empfang_gemeldet.exchange(an);
  if (vorher != an) Protokoll("Null-Kopie Empfang: Anzeige %s", an ? "übernimmt GPU-Bilder" : "ohne GPU-Weg");
}

bool EmpfangErlaubt() {
  static const bool erlaubt = [] {
    const char* aus = std::getenv("HONEYCORD_EMPFANG_GPU");
    if (aus && std::strcmp(aus, "0") == 0) {
      Protokoll("Null-Kopie Empfang: HONEYCORD_EMPFANG_GPU=0 — abgeschaltet");
      return false;
    }
    return true;
  }();
  return erlaubt && Einrichten();
}

bool EmpfangAktiv() { return EmpfangErlaubt() && g_empfang_gemeldet.load(); }

bool GeraetIstGemeinsam(const AVBufferRef* geraet) {
  return geraet && g_va && g_va->geraet && geraet->data == g_va->geraet->data;
}

webrtc::scoped_refptr<webrtc::VideoFrameBuffer> AusDecoder(const AVFrame* hw, int breite, int hoehe) {
  if (!g_va || !hw || breite < 2 || hoehe < 2) return nullptr;
  // ★ Eine Kopie auf der GRAFIKKARTE in einen eigenen Vorrat: Der Vorrat des
  // Decoders ist fest; hielte die Anzeige seine Oberflächen fest, liefe er leer.
  VASurfaceID quelle = static_cast<VASurfaceID>(reinterpret_cast<uintptr_t>(hw->data[3]));
  VARectangle r;
  r.x = 0;
  r.y = 0;
  r.width = static_cast<uint16_t>(breite);
  r.height = static_cast<uint16_t>(hoehe);
  // Gerade Maße für NV12; ungerade Ränder (selten) gehen eine Zeile verloren.
  const int zb = breite & ~1, zh = hoehe & ~1;
  AVFrame* f = g_va->Vpp(quelle, r, zb, zh, false);
  if (!f) return nullptr;
  return webrtc::make_ref_counted<HcVaBild>(f, zb, zh, false);
}

bool Exportieren(const webrtc::VideoFrameBuffer& puffer, HcGpuBild* aus) {
  if (!aus || !g_va || puffer.type() != webrtc::VideoFrameBuffer::Type::kNative) return false;
  const auto* bild = dynamic_cast<const HcVaBild*>(&puffer);
  if (!bild) return false;
  VASurfaceID s = static_cast<VASurfaceID>(reinterpret_cast<uintptr_t>(bild->rahmen()->data[3]));
  VADRMPRIMESurfaceDescriptor d;
  std::memset(&d, 0, sizeof d);
  const VAStatus st = g_va->fn.vaExportSurfaceHandle(
      g_va->dpy, s, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
      VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &d);
  if (st != VA_STATUS_SUCCESS) {
    g_va->FehlerEinmal("Export", st);
    return false;
  }
  bool ok = d.num_layers == 2;
  for (uint32_t i = 0; ok && i < 2; ++i) ok = d.layers[i].num_planes == 1;
  if (ok) {
    aus->breite = bild->width();
    aus->hoehe = bild->height();
    for (int i = 0; i < 2; ++i) {
      const uint32_t o = d.layers[i].object_index[0];
      aus->ebene[i].fd = ::dup(d.objects[o].fd);
      aus->ebene[i].drm_format = d.layers[i].drm_format;
      aus->ebene[i].offset = d.layers[i].offset[0];
      aus->ebene[i].pitch = d.layers[i].pitch[0];
      aus->ebene[i].modifier = d.objects[o].drm_format_modifier;
      if (aus->ebene[i].fd < 0) ok = false;
    }
    if (!ok) {
      for (int i = 0; i < 2; ++i) if (aus->ebene[i].fd >= 0) ::close(aus->ebene[i].fd);
    }
  }
  for (uint32_t k = 0; k < d.num_objects; ++k) ::close(d.objects[k].fd);
  return ok;
}

void AufnahmeBeginnt() { g_aufnahmen++; }

void AufnahmeEndet() {
  int n = g_aufnahmen.load();
  while (n > 0 && !g_aufnahmen.compare_exchange_weak(n, n - 1)) {
  }
  AufraeumenWennRuhig();
}

void AufraeumenWennRuhig() {
  if (g_aufnahmen.load() == 0 && g_va) {
    g_va->Leeren();
    Protokoll("Null-Kopie: keine Aufnahme mehr — Bildvorräte freigegeben");
  }
}

}  // namespace nullkopie
}  // namespace libwebrtc
