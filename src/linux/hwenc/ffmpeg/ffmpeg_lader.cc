#include "src/linux/hwenc/ffmpeg/ffmpeg_lader.h"

#include <dlfcn.h>

#include <mutex>

#include "rtc_base/logging.h"
#include "src/linux/hwenc/hw_gemeinsam.h"

namespace libwebrtc {
namespace ffmpeg {
namespace {

Api g_api;
bool g_geladen = false;
std::string g_name;
std::once_flag g_einmal;

// ★ Nur die EINE Hauptversion, deren Header hier mitliegen. Die Strukturen
// (AVCodecContext, AVFrame, AVHWFramesContext) fassen wir direkt an; ihre
// Feldlagen ändern sich zwischen Hauptversionen. Eine andere Fassung zu
// bedienen hieße, eine zweite Header-Fassung danebenzulegen — nicht, die
// Schranke aufzuweichen.
const char* const kAvcodecName = "libavcodec.so." AV_STRINGIFY(LIBAVCODEC_VERSION_MAJOR);
const char* const kAvutilName = "libavutil.so." AV_STRINGIFY(LIBAVUTIL_VERSION_MAJOR);

template <typename T>
bool Hole(void* handle, const char* name, T* ziel) {
  void* s = dlsym(handle, name);
  if (!s) {
    RTC_LOG(LS_WARNING) << "[hwenc] FFmpeg-Symbol fehlt: " << name;
    return false;
  }
  *ziel = reinterpret_cast<T>(s);
  return true;
}

// Ordner, in dem diese libwebrtc.so liegt (mit Schrägstrich am Ende).
std::string EigenerOrdner() {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<void*>(&EigenerOrdner), &info) || !info.dli_fname) return "";
  std::string pfad(info.dli_fname);
  const size_t s = pfad.rfind('/');
  return s == std::string::npos ? "" : pfad.substr(0, s + 1);
}

void LadenEinmal() {
  // RTLD_LOCAL: Die Symbole sollen NICHT in den globalen Namensraum — sonst
  // könnte eine zweite FFmpeg-Fassung, die irgendein Plugin lädt, unsere
  // Aufrufe umbiegen.
  //
  // ★ 10.11.2026: ZUERST das mitgelieferte FFmpeg neben dieser Bibliothek
  // (tar.gz und Flatpak legen ein schlankes 7.1 in denselben lib/-Ordner,
  // infra/ffmpeg-bauen.sh). GEMESSEN: Auf dem UM890-Wirt (CachyOS, FFmpeg
  // 9.0.2 = avcodec .63) fand der Lader kein .61 — VA-API, NVENC, Dekodieren
  // auf der Grafikkarte und beide Null-Kopie-Wege waren im Archiv AUS. Über
  // den eigenen Pfad statt über Suchpfade: dlopen sieht die RUNPATH der
  // ausführbaren Datei nicht. Erst avutil, damit avcodec seine Abhängigkeit
  // schon geladen vorfindet. Fehlt es, wie bisher das System-FFmpeg.
  const std::string ordner = EigenerOrdner();
  void* avutil = nullptr;
  void* avcodec = nullptr;
  bool mitgeliefert = false;
  if (!ordner.empty()) {
    avutil = dlopen((ordner + kAvutilName).c_str(), RTLD_NOW | RTLD_LOCAL);
    if (avutil) avcodec = dlopen((ordner + kAvcodecName).c_str(), RTLD_NOW | RTLD_LOCAL);
    if (avutil && avcodec) {
      mitgeliefert = true;
    } else {
      if (avutil) {
        Protokoll("FFmpeg: mitgeliefertes %s nicht zu laden (%s) — System-FFmpeg", kAvcodecName,
                  dlerror());
        dlclose(avutil);
      }
      avutil = nullptr;
    }
  }
  if (!mitgeliefert) {
    avcodec = dlopen(kAvcodecName, RTLD_NOW | RTLD_LOCAL);
    if (!avcodec) {
      RTC_LOG(LS_INFO) << "[hwenc] " << kAvcodecName
                       << " nicht gefunden — Software-Encoder";
      Protokoll("FFmpeg: weder mitgeliefert noch im System ein %s — Software-Encoder, "
                "kein Dekodieren auf der Grafikkarte", kAvcodecName);
      return;
    }
    avutil = dlopen(kAvutilName, RTLD_NOW | RTLD_LOCAL);
    if (!avutil) {
      RTC_LOG(LS_INFO) << "[hwenc] " << kAvutilName
                       << " nicht gefunden — Software-Encoder";
      Protokoll("FFmpeg: %s ohne %s — Software-Encoder", kAvcodecName, kAvutilName);
      dlclose(avcodec);
      return;
    }
  }

  bool ok = true;
  ok &= Hole(avcodec, "avcodec_find_encoder_by_name", &g_api.avcodec_find_encoder_by_name);
  ok &= Hole(avcodec, "avcodec_alloc_context3", &g_api.avcodec_alloc_context3);
  ok &= Hole(avcodec, "avcodec_free_context", &g_api.avcodec_free_context);
  ok &= Hole(avcodec, "avcodec_open2", &g_api.avcodec_open2);
  ok &= Hole(avcodec, "avcodec_send_frame", &g_api.avcodec_send_frame);
  ok &= Hole(avcodec, "avcodec_receive_packet", &g_api.avcodec_receive_packet);
  ok &= Hole(avcodec, "av_packet_alloc", &g_api.av_packet_alloc);
  ok &= Hole(avcodec, "av_packet_free", &g_api.av_packet_free);
  ok &= Hole(avcodec, "av_packet_unref", &g_api.av_packet_unref);
  ok &= Hole(avcodec, "avcodec_version", &g_api.avcodec_version);

  ok &= Hole(avutil, "av_frame_alloc", &g_api.av_frame_alloc);
  ok &= Hole(avutil, "av_frame_free", &g_api.av_frame_free);
  ok &= Hole(avutil, "av_frame_unref", &g_api.av_frame_unref);
  ok &= Hole(avutil, "av_frame_ref", &g_api.av_frame_ref);
  ok &= Hole(avutil, "av_frame_get_buffer", &g_api.av_frame_get_buffer);
  ok &= Hole(avutil, "av_frame_make_writable", &g_api.av_frame_make_writable);
  ok &= Hole(avutil, "av_hwdevice_ctx_create", &g_api.av_hwdevice_ctx_create);
  ok &= Hole(avutil, "av_hwframe_ctx_alloc", &g_api.av_hwframe_ctx_alloc);
  ok &= Hole(avutil, "av_hwframe_ctx_init", &g_api.av_hwframe_ctx_init);
  ok &= Hole(avutil, "av_hwframe_get_buffer", &g_api.av_hwframe_get_buffer);
  ok &= Hole(avutil, "av_hwframe_transfer_data", &g_api.av_hwframe_transfer_data);
  ok &= Hole(avutil, "av_buffer_ref", &g_api.av_buffer_ref);
  ok &= Hole(avutil, "av_buffer_unref", &g_api.av_buffer_unref);
  ok &= Hole(avutil, "av_opt_set", &g_api.av_opt_set);
  ok &= Hole(avutil, "av_opt_set_int", &g_api.av_opt_set_int);
  ok &= Hole(avutil, "av_strerror", &g_api.av_strerror);
  ok &= Hole(avutil, "avutil_version", &g_api.avutil_version);

  if (!ok) {
    RTC_LOG(LS_WARNING) << "[hwenc] FFmpeg gefunden, aber unvollständig — Software-Encoder";
    Protokoll("FFmpeg: %s unvollständig (%s) — Software-Encoder", kAvcodecName,
              mitgeliefert ? "mitgeliefert" : "System");
    g_api = Api();
    return;
  }

  // ★ Gürtel und Hosenträger: Der Soname sagt schon, welche Hauptversion wir
  // geöffnet haben — aber eine falsch gesetzte Verknüpfung oder ein
  // untergeschobenes LD_LIBRARY_PATH kann etwas anderes liefern. Die Zahl aus
  // der Bibliothek selbst lügt nicht.
  const unsigned codec_haupt = g_api.avcodec_version() >> 16;
  const unsigned util_haupt = g_api.avutil_version() >> 16;
  if (codec_haupt != LIBAVCODEC_VERSION_MAJOR ||
      util_haupt != LIBAVUTIL_VERSION_MAJOR) {
    RTC_LOG(LS_WARNING) << "[hwenc] FFmpeg meldet avcodec " << codec_haupt
                        << "/avutil " << util_haupt << ", gebaut gegen "
                        << LIBAVCODEC_VERSION_MAJOR << "/"
                        << LIBAVUTIL_VERSION_MAJOR << " — Software-Encoder";
    Protokoll("FFmpeg: meldet avcodec %u/avutil %u statt %d/%d — Software-Encoder", codec_haupt,
              util_haupt, LIBAVCODEC_VERSION_MAJOR, LIBAVUTIL_VERSION_MAJOR);
    g_api = Api();
    return;
  }

  // Dekoder-Gruppe: getrennt geprüft, ein Fehlen schaltet nur sie ab.
  bool dek = true;
  dek &= Hole(avcodec, "avcodec_find_decoder", &g_api.avcodec_find_decoder);
  dek &= Hole(avcodec, "avcodec_get_hw_config", &g_api.avcodec_get_hw_config);
  dek &= Hole(avcodec, "avcodec_send_packet", &g_api.avcodec_send_packet);
  dek &= Hole(avcodec, "avcodec_receive_frame", &g_api.avcodec_receive_frame);
  dek &= Hole(avutil, "av_get_pix_fmt_name", &g_api.av_get_pix_fmt_name);
  dek &= Hole(avutil, "av_hwframe_map", &g_api.av_hwframe_map);
  dek &= Hole(avutil, "av_image_copy_plane_uc_from", &g_api.av_image_copy_plane_uc_from);
  g_api.dekoder_ok = dek;
  if (!dek) {
    RTC_LOG(LS_WARNING) << "[hwenc] FFmpeg ohne vollständige Dekoder-Funktionen — "
                           "Hardware-Dekodierung aus, Encoder unberührt";
  }

  g_name = kAvcodecName;
  g_geladen = true;
  RTC_LOG(LS_INFO) << "[hwenc] " << g_name << " geladen";
  Protokoll("FFmpeg: %s %u.%u.%u (%s)", kAvcodecName, g_api.avcodec_version() >> 16,
            (g_api.avcodec_version() >> 8) & 0xff, g_api.avcodec_version() & 0xff,
            mitgeliefert ? "mitgeliefert" : "System");
  // Die Handles bleiben absichtlich offen: Die Bibliothek lebt, solange der
  // Prozess lebt. Ein dlclose zur Laufzeit schüfe nur Fehlerquellen.
}

}  // namespace

bool Laden() {
  std::call_once(g_einmal, LadenEinmal);
  return g_geladen;
}

const Api& Zugriff() { return g_api; }

const std::string& GeladeneBibliothek() { return g_name; }

std::string FehlerText(int fehler) {
  char puffer[128] = {0};
  if (g_geladen && g_api.av_strerror &&
      g_api.av_strerror(fehler, puffer, sizeof(puffer)) == 0) {
    return std::string(puffer) + " (" + std::to_string(fehler) + ")";
  }
  return std::to_string(fehler);
}

}  // namespace ffmpeg
}  // namespace libwebrtc
