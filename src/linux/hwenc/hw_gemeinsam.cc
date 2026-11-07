#include "src/linux/hwenc/hw_gemeinsam.h"

#include "src/linux/hwenc/ffmpeg/ffmpeg_lader.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>

// Aus hw_h264_encoder.cc ausgelagert (07.11.2026), damit Encoder und Decoder
// dieselbe Protokolldatei und dieselben Systemangaben benutzen. Inhalt
// unverändert bis auf die Sperre in `Protokoll` (zwei Fäden schreiben jetzt).

namespace libwebrtc {
// ★★ EIGENE PROTOKOLLDATEI `hwenc.log` (21.09.2026): `RTC_LOG` aus diesem
// Encoder erreicht das Protokoll des Clients NICHT — gemessen an Tims
// Bündeln, null Treffer für „[hwenc]". Damit war die Frage „was gibt WebRTC
// dem Encoder vor, und was macht er daraus?" nicht zu beantworten, obwohl
// genau sie zwischen Bandbreitenschätzung und Encoder entscheidet. Die Datei
// liegt neben `send.log` (dieselbe Regel wie `log_ort.dart` im Client) und
// geht mit dem Diagnose-Bündel automatisch mit hoch.
void Protokoll(const char* format, ...) {
  // Seit dem Decoder (07.11.2026) schreiben mehrere Fäden — Encode-Faden,
  // je ein Decode-Faden pro Empfangsspur. Die Sperre hält Zeilen ganz und
  // schützt das einmalige Setzen des Pfads.
  static std::mutex sperre;
  std::lock_guard<std::mutex> halt(sperre);
  static std::string pfad;
  if (pfad.empty()) {
    const char* xdg = std::getenv("XDG_STATE_HOME");
    const char* home = std::getenv("HOME");
    std::string dir = (xdg && *xdg) ? std::string(xdg) + "/honeycord"
                    : (home && *home) ? std::string(home) + "/.local/state/honeycord"
                    : std::string("/tmp/honeycord");
    ::mkdir(dir.c_str(), 0700);
    pfad = dir + "/hwenc.log";
  }
  FILE* f = std::fopen(pfad.c_str(), "a");
  if (!f) return;
  // Gedeckelt wie diag.log: ab 512 KB von vorn (die letzte Sitzung zählt).
  struct stat st;
  if (::stat(pfad.c_str(), &st) == 0 && st.st_size > 512 * 1024) {
    std::fclose(f);
    f = std::fopen(pfad.c_str(), "w");
    if (!f) return;
  }
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  std::fprintf(f, "[%02d-%02d %02d:%02d:%02d] ", tm.tm_mon + 1, tm.tm_mday,
               tm.tm_hour, tm.tm_min, tm.tm_sec);
  va_list args;
  va_start(args, format);
  std::vfprintf(f, format, args);
  va_end(args);
  std::fputc('\n', f);
  std::fclose(f);
}


bool RenderKnotenDa() {
  // ★ Ohne Render-Knoten keine VA-API. Das kommt öfter vor, als man denkt: in
  // Containern ohne `--device /dev/dri`, auf Servern ohne GPU und in Flatpaks
  // ohne `--device=dri`.
  for (const char* pfad : {"/dev/dri/renderD128", "/dev/dri/renderD129"}) {
    int fd = ::open(pfad, O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
      ::close(fd);
      return true;
    }
  }
  return false;
}

// ★ 25.09.2026: Welche Grafikkarte steckt hinter dem Render-Knoten? Ohne
// diese Zeile war bei Haltoans 40–55 ms je Bild nicht zu sagen, ob eine
// schwache iGPU, ein fremder Treiber oder unser Weg die Zeit kostet.
std::string SysfsZeile(const std::string& pfad) {
  FILE* f = std::fopen(pfad.c_str(), "r");
  if (!f) return "?";
  char puffer[128] = {0};
  if (!std::fgets(puffer, sizeof(puffer), f)) puffer[0] = 0;
  std::fclose(f);
  std::string s(puffer);
  while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
  return s.empty() ? "?" : s;
}

std::string GpuBeschreibung() {
  std::string text;
  for (const char* knoten : {"renderD128", "renderD129"}) {
    const std::string basis = std::string("/sys/class/drm/") + knoten + "/device/";
    const std::string hersteller = SysfsZeile(basis + "vendor");
    if (hersteller == "?") continue;
    char ziel[256] = {0};
    const ssize_t n = ::readlink((basis + "driver").c_str(), ziel, sizeof(ziel) - 1);
    std::string treiber = "?";
    if (n > 0) {
      treiber = std::string(ziel, static_cast<size_t>(n));
      const size_t pos = treiber.rfind('/');
      if (pos != std::string::npos) treiber = treiber.substr(pos + 1);
    }
    if (!text.empty()) text += ", ";
    text += std::string(knoten) + "=" + hersteller + ":" + SysfsZeile(basis + "device") +
            " (" + treiber + ")";
  }
  return text.empty() ? "keine Angabe" : text;
}

// ★ 17.10.2026 (Haltoan, UHD 620): Derselbe Bildweg kostet bei ihm ~15 ms je
// Bild, auf einer UHD 630 im Messstand 1,7 ms — gleiche Grafik-Generation,
// gleicher iHD-Treiber aus der Flatpak-Erweiterung (gemessen, auch mit den
// Flatpak-Bibliotheken). Schon das reine Umformen auf der CPU ist bei ihm
// viermal langsamer. Also mitschreiben, wie schnell CPU und GPU gerade
// laufen, ob das Netzteil steckt und wie ausgelastet das System ist.
std::string SystemLage() {
  // CPU: Mittel der aktuellen Taktraten aller Kerne (sysfs, auch im Flatpak lesbar).
  long summe_khz = 0;
  int kerne = 0;
  // ★ Höchstens vier Kerne (Prüfbefund M2): Auf Kerneln vor 5.18 kann das
  // Lesen von scaling_cur_freq je Kern ~10 ms schlafen — und das hier läuft
  // alle fünf Sekunden auf dem Encode-Faden.
  for (int i = 0; i < 4; ++i) {
    const std::string z = SysfsZeile("/sys/devices/system/cpu/cpu" + std::to_string(i) +
                                     "/cpufreq/scaling_cur_freq");
    if (z == "?") break;
    summe_khz += std::atol(z.c_str());
    ++kerne;
  }
  const std::string regler = SysfsZeile("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
  const std::string epp =
      SysfsZeile("/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference");
  // GPU (i915: gt_act_freq_mhz / gt_max_freq_mhz; andere Treiber: „?").
  std::string gpu = "?";
  for (const char* karte : {"card0", "card1", "card2"}) {
    const std::string basis = std::string("/sys/class/drm/") + karte + "/";
    const std::string akt = SysfsZeile(basis + "gt_act_freq_mhz");
    if (akt == "?") continue;
    gpu = akt + "/" + SysfsZeile(basis + "gt_max_freq_mhz") + " MHz";
    break;
  }
  // Netzteil: irgendeine Stromquelle vom Typ „Mains" mit online=1.
  std::string netz = "?";
  for (const char* q : {"AC", "AC0", "ACAD", "ADP0", "ADP1", "AC1"}) {
    const std::string z = SysfsZeile(std::string("/sys/class/power_supply/") + q + "/online");
    if (z == "?") continue;
    netz = z == "1" ? "ja" : "nein (Akku)";
    break;
  }
  const std::string last = SysfsZeile("/proc/loadavg");
  char text[256];
  std::snprintf(text, sizeof(text), "CPU %ld MHz (%d Kerne, %s/%s) · GPU %s · Netzteil %s · Last %s",
                kerne ? summe_khz / kerne / 1000 : 0, kerne, regler.c_str(), epp.c_str(),
                gpu.c_str(), netz.c_str(), last.substr(0, 14).c_str());
  return text;
}

// Welcher VA-Treiber wirklich geladen ist (iHD/i965/radeonsi samt Version).
// libva ist zu diesem Zeitpunkt schon durch libavutil geladen; die Anzeige
// steckt im ersten Feld von AVVAAPIDeviceContext (`VADisplay display`).
std::string VaTreiber(AVBufferRef* geraet) {
  if (!geraet) return "?";
  using Fn = const char* (*)(void*);
  // ★ NICHT RTLD_DEFAULT: FFmpeg wird mit RTLD_LOCAL geladen (ffmpeg_lader.cc),
  // damit landet auch libva nicht im globalen Namensraum — gemessen 17.10.:
  // RTLD_DEFAULT liefert nil. RTLD_NOLOAD gibt den Griff auf die schon
  // geladene libva, ohne etwas nachzuladen.
  void* va = ::dlopen("libva.so.2", RTLD_NOW | RTLD_NOLOAD);
  if (!va) return "? (libva nicht geladen)";
  auto fn = reinterpret_cast<Fn>(::dlsym(va, "vaQueryVendorString"));
  ::dlclose(va);  // nur der Referenzzähler; libavutil hält sie weiter
  if (!fn) return "? (vaQueryVendorString fehlt)";
  auto* hw = reinterpret_cast<AVHWDeviceContext*>(geraet->data);
  if (!hw || !hw->hwctx) return "?";
  void* anzeige = *reinterpret_cast<void**>(hw->hwctx);
  const char* t = anzeige ? fn(anzeige) : nullptr;
  return t ? t : "?";
}

}  // namespace libwebrtc
