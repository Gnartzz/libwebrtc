#include "src/linux/hwenc/ffmpeg/va_treiber.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "src/linux/hwenc/ffmpeg/ffmpeg_lader.h"
#include "src/linux/hwenc/hw_gemeinsam.h"

// ── Mitgelieferter VA-Treiber für AMD (hwenc.19, 24.11.2026) ────────────────
//
// ★★ URSACHE der VCN-Hänger, GEMESSEN auf dem UM890 (Radeon 780M, VCN 4.0.2,
// Firmware 0x09118037, Mesa 26.2.7 / 26.3.1 / main): Mesa legt den 128 KB
// großen Sitzungsspeicher des VCN-Encoders (radeon_vcn_enc.c, `enc->si`) aus
// seinem Wiederverwendungs-Vorrat an, OHNE ihn zu löschen. Trägt er alten
// Inhalt — eine geschlossene Sitzung oder irgendetwas, das im selben Prozess
// freigegeben wurde, z. B. Texturdaten von Qt/Flutter über OpenGL —, greift
// die VCN-Firmware beim ERSTEN Bild der neuen Sitzung daneben (Seitenfehler an
// 0x0 + Versatz bzw. an „Adressen" aus Bilddaten), der Ring läuft in den
// Timeout, Mesa bricht den Prozess ab, und fremde VCN-Nutzer (Sunshine)
// stürzen mit. Belegt mit einem eigenen Mesa-Bau: nur diesen Puffer löschen
// → 360 Öffnungen ohne Befund statt Absturz nach 10–26; nur das DPB löschen
// hilft nicht.
//
// Von außen ist das nicht zu umgehen (der Vorrat gehört Mesa). Deshalb liefern
// die Pakete einen radeonsi-VA-Treiber mit genau dieser Korrektur mit
// (patches/honeycord_mesa_vcn_sitzungsspeicher.patch, gebaut von
// build/mesa_va_amd_bauen.sh): `hc-va/radeonsi_drv_video.so` neben dieser
// Bibliothek. libva kennt keinen Treiberpfad je Anzeige, nur die
// Umgebungsvariable LIBVA_DRIVERS_PATH, die es bei JEDEM vaInitialize liest.
//
// ★ REGELN (Prüfbefunde M1–M3, M7, N1, N2 vom 24.11.2026):
//  • Gesetzt wird im KONSTRUKTOR dieser Bibliothek (N1): Er läuft beim Laden
//    des Programms, bevor main() Fäden startet — `setenv` neben einem
//    `getenv` in einem anderen Faden ist sonst ein Wettlauf. Nur der
//    Rückfall (M1) ändert die Variable später noch einmal, und zwar durch
//    Überschreiben (glibc tauscht dabei nur den Zeiger im bestehenden
//    Eintrag) bzw. einmaliges `unsetenv`.
//  • Vorgeschaltet wird nur, wenn der Knoten, den FFmpeg für VA wirklich
//    öffnet (`VaKnoten`, gleiche Reihenfolge wie hwcontext_vaapi.c), eine
//    AMD-Karte ist (M7, N2) und ihre VCN-Fassung (sysfs ip_discovery) in
//    Mesa 26.2.7 bekannt ist (M2). Ältere Karten ohne ip_discovery (VCE/UVD,
//    Polaris, GCN 1/2) und unbekannte neuere bekommen den Systemtreiber. Auf
//    Hybrid-Rechnern mit Intel/NVIDIA als VA-Gerät greift nichts ein.
//  • Erbschaft (M3): Kindprozesse erben die Variable. Der Ursprungswert steht
//    deshalb in HC_VA_PFAD_VORHER; beim Start entfernt jede Fassung eigene
//    `…/hc-va`-Einträge aus einem geerbten Wert und stellt den Ursprung her,
//    bevor sie selbst entscheidet (HC_VA_TREIBER=0 wirkt so auch nach einem
//    Neustart aus der laufenden Fassung). Programme, die die Clients starten
//    (Browser, Nachläufer), bekommen den Ursprung von der Produktseite.
//  • War die Variable vorher nicht gesetzt (M7), folgen auf unseren Ordner
//    die Treiberordner neben der geladenen libva (<libdir>/dri,
//    <libdir>/va/drivers) und die bekannten Orte der Verteilungen (NixOS,
//    Gentoo, Flatpak-GL-Erweiterungen) — mit gesetzter Variable sucht libva
//    seinen eingebauten Ort nicht mehr.
//  • Rückfall (M1): libva bricht nach einem gefundenen Init-Symbol ab, auch
//    wenn die Initialisierung scheitert — ein Treiber, der lädt, aber diese
//    Karte nicht kann, ließe VA ganz ausfallen. Scheitert deshalb das ERSTE
//    VA-Gerät des Prozesses, wird auf den Ursprung zurückgestellt und einmal
//    neu versucht (prozessweit gemerkt, steht in hwenc.log).
//  • Welcher Treiber wirklich geladen ist, steht nach dem ersten Gerät aus
//    /proc/self/maps im Protokoll (M4): Übergeht libva unseren Treiber (z. B.
//    fehlende Bibliothek oder eine ältere libva ohne passendes Init-Symbol),
//    sagt das Protokoll es.
//  • Andere Hersteller berührt das nicht (dort liegt nur radeonsi).
//    HC_VA_TREIBER=0 schaltet es ab.

namespace libwebrtc {
namespace vatreiber {
namespace {

constexpr char kVar[] = "LIBVA_DRIVERS_PATH";
constexpr char kVorher[] = "HC_VA_PFAD_VORHER";
constexpr char kNichtGesetzt[] = "(nicht gesetzt)";

struct Fassung {
  int mj, mn, rv;
};
bool operator<(const Fassung& a, const Fassung& b) {
  if (a.mj != b.mj) return a.mj < b.mj;
  if (a.mn != b.mn) return a.mn < b.mn;
  return a.rv < b.rv;
}

// Jede VCN-Fassung, die Mesa 26.2.7 kennt (src/amd/common/ac_gpu_info.c,
// Zuordnung `VCN_IP_VERSION` → `vcn_ip_version`). Eine Fassung außerhalb
// dieser Liste fiele dort auf VCN_UNKNOWN — der Treiber könnte die Karte
// nicht kodieren, also bleibt sie beim Systemtreiber.
constexpr Fassung kVcnBekannt[] = {
    {1, 0, 0}, {1, 0, 1}, {2, 0, 0},  {2, 0, 2},  {2, 0, 3}, {2, 2, 0}, {2, 5, 0}, {2, 6, 0},
    {3, 0, 0}, {3, 0, 2}, {3, 0, 16}, {3, 0, 33}, {3, 1, 1}, {3, 1, 2}, {4, 0, 0}, {4, 0, 2},
    {4, 0, 3}, {4, 0, 4}, {4, 0, 5},  {4, 0, 6},  {5, 0, 0}, {5, 0, 1}, {5, 0, 2}, {5, 3, 0},
};

// Erste Mesa-Fassung, die den VCN-Sitzungsspeicher selbst löscht. Stand
// 24.11.2026: KEINE ({0,0,0}; der Fehlerbericht ist ein Entwurf,
// patches/MESA-FEHLERBERICHT-vcn-sitzungsspeicher.md). Steht hier eine
// Fassung und hat das System-Mesa sie erreicht, wird nicht vorgeschaltet.
constexpr Fassung kMesaMitFix = {0, 0, 0};

// Testschalter HC_TEST_MESA_MIT_FIX=x.y.z ersetzt kMesaMitFix (Prüffall M2).
Fassung MesaMitFix() {
  Fassung f = kMesaMitFix;
  const char* t = std::getenv("HC_TEST_MESA_MIT_FIX");
  if (t && *t && std::sscanf(t, "%d.%d.%d", &f.mj, &f.mn, &f.rv) < 2) f = kMesaMitFix;
  return f;
}

struct Wahl {
  bool vorgeschaltet = false;
  bool ursprung_gesetzt = false;
  std::string ursprung;
  std::string eigen;     // …/hc-va
  std::string knoten;    // VA-Knoten beim Start
  std::string text = "System";
  std::string geladen;   // aus /proc/self/maps, nach dem ersten Gerät
  bool rueckfall = false;
  bool erstes_geraet = true;
};
std::mutex g_sperre;   // konstant initialisiert, im Konstruktor schon nutzbar
Wahl& W() {
  static Wahl* w = new Wahl();   // nie zerstört
  return *w;
}

std::vector<std::string> Teile(const std::string& s) {
  std::vector<std::string> v;
  size_t a = 0;
  while (a <= s.size()) {
    const size_t e = s.find(':', a);
    const std::string t = s.substr(a, e == std::string::npos ? std::string::npos : e - a);
    if (!t.empty()) v.push_back(t);
    if (e == std::string::npos) break;
    a = e + 1;
  }
  return v;
}

std::string Verbinden(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& t : v) {
    if (!s.empty()) s += ':';
    s += t;
  }
  return s;
}

// Ein von uns vorgeschalteter Eintrag endet auf „/hc-va".
bool IstEigenerEintrag(std::string t) {
  while (t.size() > 1 && t.back() == '/') t.pop_back();
  return t == "hc-va" || (t.size() > 6 && t.compare(t.size() - 6, 6, "/hc-va") == 0);
}

bool IstOrdner(const std::string& p) {
  struct stat st;
  return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string Ordnername(const std::string& pfad) {
  const size_t s = pfad.rfind('/');
  return s == std::string::npos ? "" : pfad.substr(0, s);
}

// Ordner dieser Bibliothek (bzw. des Programms, in das sie gebunden ist), mit
// Schrägstrich am Ende.
std::string EigenerOrdner() {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<void*>(&EigenerOrdner), &info) || !info.dli_fname) return "";
  const std::string o = Ordnername(info.dli_fname);
  return o.empty() ? "" : o + "/";
}

// Wie FFmpeg 7.1 (hwcontext_vaapi.c, vaapi_device_create ohne Gerät):
// renderD128 … renderD135 der Reihe nach, fehlende überspringen, vgem
// überspringen, bei einem anderen Öffnungsfehler aufhören.
std::string VaKnotenErmitteln() {
  for (int n = 0; n < 8; ++n) {
    const std::string name = "renderD" + std::to_string(128 + n);
    const int fd = ::open(("/dev/dri/" + name).c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      if (errno == ENOENT) continue;
      return "";
    }
    ::close(fd);
    char ziel[256] = {0};
    const std::string link = "/sys/class/drm/" + name + "/device/driver";
    const ssize_t l = ::readlink(link.c_str(), ziel, sizeof(ziel) - 1);
    if (l > 0) {
      const std::string t(ziel, static_cast<size_t>(l));
      if (t.size() >= 5 && t.compare(t.size() - 5, 5, "/vgem") == 0) continue;
    }
    return name;
  }
  return "";
}

// VCN-Fassung aus sysfs (amdgpu, ip_discovery: der Block heißt dort UVD).
// Testschalter HC_TEST_VCN=x.y.z ersetzt den gelesenen Wert (Prüffall M2).
bool VcnFassung(const std::string& knoten, Fassung* f, std::string* text) {
  const char* test = std::getenv("HC_TEST_VCN");
  if (test && *test) {
    if (std::sscanf(test, "%d.%d.%d", &f->mj, &f->mn, &f->rv) != 3) return false;
    *text = std::string(test) + " (HC_TEST_VCN)";
    return true;
  }
  const std::string basis = "/sys/class/drm/" + knoten + "/device/ip_discovery/die";
  DIR* dies = ::opendir(basis.c_str());
  if (!dies) return false;
  bool gefunden = false;
  while (dirent* d = ::readdir(dies)) {
    if (d->d_name[0] == '.') continue;
    const std::string uvd = basis + "/" + d->d_name + "/UVD";
    DIR* inst = ::opendir(uvd.c_str());
    if (!inst) continue;
    while (dirent* i = ::readdir(inst)) {
      if (i->d_name[0] == '.') continue;
      const std::string p = uvd + "/" + i->d_name + "/";
      const std::string mj = SysfsZeile(p + "major"), mn = SysfsZeile(p + "minor"),
                        rv = SysfsZeile(p + "revision");
      if (mj == "?" || mn == "?" || rv == "?") continue;
      f->mj = std::atoi(mj.c_str());
      f->mn = std::atoi(mn.c_str());
      f->rv = std::atoi(rv.c_str());
      *text = mj + "." + mn + "." + rv;
      gefunden = true;
      break;
    }
    ::closedir(inst);
    if (gefunden) break;
  }
  ::closedir(dies);
  return gefunden;
}

bool VcnBekannt(const Fassung& f) {
  for (const auto& b : kVcnBekannt)
    if (b.mj == f.mj && b.mn == f.mn && b.rv == f.rv) return true;
  return false;
}

// Ordner der libva, die der Prozess lädt. Lädt sie (nur RTLD_LOCAL) und hält
// sie — sie wird für VA ohnehin gebraucht.
std::string LibvaOrdner() {
  void* h = dlopen("libva.so.2", RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
  if (!h) h = dlopen("libva.so.2", RTLD_LAZY | RTLD_LOCAL);
  if (!h) return "";
  link_map* lm = nullptr;
  if (dlinfo(h, RTLD_DI_LINKMAP, &lm) != 0 || !lm || !lm->l_name) return "";
  char echt[4096];
  if (::realpath(lm->l_name, echt)) return Ordnername(echt);
  return Ordnername(lm->l_name);
}

// Orte, an denen libva ohne gesetzte Variable sucht bzw. die Verteilungen
// ihre Treiber ablegen; nur vorhandene, ohne Doppel.
std::vector<std::string> Standardorte() {
  std::vector<std::string> kandidaten;
  const std::string libva = LibvaOrdner();
  if (!libva.empty()) {
    kandidaten.push_back(libva + "/dri");
    kandidaten.push_back(libva + "/va/drivers");
  }
  for (const char* p :
       {"/usr/lib/dri", "/usr/lib64/dri", "/usr/lib/x86_64-linux-gnu/dri", "/usr/lib64/va/drivers",
        "/usr/lib/va/drivers", "/usr/local/lib/dri", "/run/opengl-driver/lib/dri",
        "/usr/lib/x86_64-linux-gnu/GL/default/lib/dri", "/usr/lib/x86_64-linux-gnu/GL/lib/dri",
        "/usr/lib/x86_64-linux-gnu/dri/intel-vaapi-driver"}) {
    kandidaten.push_back(p);
  }
  std::vector<std::string> v;
  for (const auto& k : kandidaten) {
    if (!IstOrdner(k)) continue;
    char echt[4096];
    const std::string e = ::realpath(k.c_str(), echt) ? std::string(echt) : k;
    bool doppelt = false;
    for (const auto& x : v) {
      char ex[4096];
      if ((::realpath(x.c_str(), ex) ? std::string(ex) : x) == e) doppelt = true;
    }
    if (!doppelt) v.push_back(k);
  }
  return v;
}

// Mesa-Fassung des Systemtreibers, „?" wenn nicht lesbar. Seit Mesa 24.3 ist
// radeonsi_drv_video.so eine kleine Hülle mit NEEDED libgallium-<Fassung>.so;
// ältere tragen die Herstellerzeichenkette „Mesa Gallium driver <Fassung>".
std::string SystemMesa(const std::vector<std::string>& orte, std::string* fundort) {
  for (const auto& o : orte) {
    const std::string p = o + "/radeonsi_drv_video.so";
    const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) continue;
    struct stat st;
    std::string fassung;
    if (::fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size < (256 << 20)) {
      void* m = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
      if (m != MAP_FAILED) {
        const char* a = static_cast<const char*>(m);
        const size_t n = static_cast<size_t>(st.st_size);
        for (const char* marke : {"libgallium-", "Mesa Gallium driver "}) {
          const size_t lm = std::strlen(marke);
          const void* t = memmem(a, n, marke, lm);
          if (!t) continue;
          const char* z = static_cast<const char*>(t) + lm;
          while (z < a + n && (std::isdigit(static_cast<unsigned char>(*z)) || *z == '.'))
            fassung += *z++;
          while (!fassung.empty() && fassung.back() == '.') fassung.pop_back();   // „….so"
          if (!fassung.empty()) break;
        }
        ::munmap(m, static_cast<size_t>(st.st_size));
      }
    }
    ::close(fd);
    *fundort = p;
    return fassung.empty() ? "?" : fassung;
  }
  return "?";
}

void UrsprungHerstellen(const Wahl& w) {
  if (w.ursprung_gesetzt)
    ::setenv(kVar, w.ursprung.c_str(), 1);
  else
    ::unsetenv(kVar);
}

// Gibt "" zurück, wenn vorgeschaltet werden soll, sonst den Grund dagegen.
std::string Pruefen(Wahl& w, std::string* angabe) {
  const char* aus = std::getenv("HC_VA_TREIBER");
  if (aus && std::strcmp(aus, "0") == 0) return "abgeschaltet (HC_VA_TREIBER=0)";
  const std::string ordner = EigenerOrdner();
  if (ordner.empty()) return "eigener Ordner unbekannt";
  w.eigen = ordner + "hc-va";
  if (::access((w.eigen + "/radeonsi_drv_video.so").c_str(), R_OK) != 0)
    return "kein mitgelieferter Treiber";
  if (w.eigen.find(':') != std::string::npos)
    return "Programmpfad enthält „:“ (" + w.eigen + ") — in LIBVA_DRIVERS_PATH nicht darstellbar";
  w.knoten = VaKnotenErmitteln();
  if (w.knoten.empty()) return "kein Render-Knoten";
  const std::string hersteller = SysfsZeile("/sys/class/drm/" + w.knoten + "/device/vendor");
  if (hersteller != "0x1002") return "VA-Gerät " + w.knoten + " ist " + hersteller + ", nicht AMD";
  Fassung vcn{};
  std::string vcn_text;
  if (!VcnFassung(w.knoten, &vcn, &vcn_text))
    return "VCN-Fassung von " + w.knoten + " nicht lesbar (kein ip_discovery: VCE/UVD oder ältere Karte)";
  if (!VcnBekannt(vcn)) return "VCN " + vcn_text + " ist Mesa 26.2.7 nicht bekannt";
  std::string fundort;
  const std::string mesa = SystemMesa(
      w.ursprung_gesetzt ? Teile(w.ursprung) : Standardorte(), &fundort);
  *angabe = w.knoten + ", VCN " + vcn_text + ", System-Mesa " + mesa +
            (fundort.empty() ? "" : " (" + fundort + ")");
  const Fassung mit_fix = MesaMitFix();
  if (mit_fix.mj > 0 && mesa != "?") {
    Fassung m{};
    if (std::sscanf(mesa.c_str(), "%d.%d.%d", &m.mj, &m.mn, &m.rv) >= 2 && !(m < mit_fix))
      return "System-Mesa " + mesa + " enthält den Fix (" + *angabe + ")";
  }
  return "";
}

// ★ Läuft beim Laden der Bibliothek (N1), s. Kopf.
__attribute__((constructor)) void BeimLaden() {
  std::lock_guard<std::mutex> halt(g_sperre);
  Wahl& w = W();
  // 1. Geerbtes bereinigen (M3).
  const char* jetzt = std::getenv(kVar);
  const char* vorher = std::getenv(kVorher);
  bool geerbt = false;
  std::vector<std::string> rest;
  if (jetzt) {
    for (const auto& t : Teile(jetzt)) {
      if (IstEigenerEintrag(t))
        geerbt = true;
      else
        rest.push_back(t);
    }
  }
  std::string erbschaft;
  if (geerbt && vorher) {
    w.ursprung_gesetzt = std::strcmp(vorher, kNichtGesetzt) != 0;
    w.ursprung = w.ursprung_gesetzt ? vorher : "";
    erbschaft = "; geerbten hc-va-Eintrag entfernt, Ursprung aus HC_VA_PFAD_VORHER";
  } else if (geerbt) {
    w.ursprung_gesetzt = !rest.empty();
    w.ursprung = Verbinden(rest);
    erbschaft = "; geerbten hc-va-Eintrag entfernt";
  } else {
    w.ursprung_gesetzt = jetzt != nullptr;
    w.ursprung = jetzt ? jetzt : "";
  }
  UrsprungHerstellen(w);
  ::unsetenv(kVorher);
  // 2. Entscheiden.
  std::string angabe;
  const std::string grund = Pruefen(w, &angabe);
  if (!grund.empty()) {
    w.text = "System — " + grund + erbschaft;
    return;
  }
  // 3. Vorschalten.
  const std::string folge = w.ursprung_gesetzt ? w.ursprung : Verbinden(Standardorte());
  const std::string pfad = folge.empty() ? w.eigen : w.eigen + ":" + folge;
  ::setenv(kVar, pfad.c_str(), 1);
  ::setenv(kVorher, w.ursprung_gesetzt ? w.ursprung.c_str() : kNichtGesetzt, 1);
  w.vorgeschaltet = true;
  w.text = "mitgeliefert vorgeschaltet (" + w.eigen + "; " + angabe + "; " + kVar + "=" + pfad +
           "; vorher " + (w.ursprung_gesetzt ? w.ursprung : std::string(kNichtGesetzt)) + ")" +
           erbschaft;
}

// Welche VA-Treiber laut /proc/self/maps geladen sind. Die Systemtreiber sind
// seit Mesa 24.3 Verweise auf libgallium-<Fassung>.so (dieselbe Bibliothek wie
// OpenGL) — in maps steht das Ziel. Deshalb je Suchort jede *_drv_video.so
// über realpath abgleichen; übrig gebliebene *_drv_video.so kommen dazu.
std::string GeladeneTreiber(const Wahl& w) {
  std::vector<std::string> gemappt;
  if (FILE* f = std::fopen("/proc/self/maps", "r")) {
    char zeile[4096];
    while (std::fgets(zeile, sizeof(zeile), f)) {
      const char* p = std::strchr(zeile, '/');
      if (!p) continue;
      std::string s(p);
      while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
      if (std::find(gemappt.begin(), gemappt.end(), s) == gemappt.end()) gemappt.push_back(s);
    }
    std::fclose(f);
  }
  std::vector<std::string> orte;
  if (!w.eigen.empty()) orte.push_back(w.eigen);
  for (const auto& o : w.ursprung_gesetzt ? Teile(w.ursprung) : Standardorte()) orte.push_back(o);
  std::vector<std::string> treffer, erklaert;
  for (const auto& o : orte) {
    DIR* d = ::opendir(o.c_str());
    if (!d) continue;
    // radeonsi zuerst: Die Verweise einer libgallium (d3d12, nouveau, …) zeigen
    // alle auf dieselbe Datei.
    std::vector<std::string> namen{"radeonsi_drv_video.so"};
    while (dirent* e = ::readdir(d)) namen.push_back(e->d_name);
    ::closedir(d);
    for (const std::string& n : namen) {
      if (n.size() < 13 || n.compare(n.size() - 13, 13, "_drv_video.so") != 0) continue;
      char echt[4096];
      const std::string p = o + "/" + n;
      if (!::realpath(p.c_str(), echt)) continue;
      if (std::find(gemappt.begin(), gemappt.end(), echt) == gemappt.end()) continue;
      if (std::find(erklaert.begin(), erklaert.end(), echt) != erklaert.end()) continue;
      erklaert.push_back(echt);
      treffer.push_back(p == echt ? p : p + " (→ " + echt + ")");
    }
  }
  for (const auto& g : gemappt) {
    if (g.find("_drv_video.so") == std::string::npos) continue;
    if (std::find(erklaert.begin(), erklaert.end(), g) == erklaert.end()) treffer.push_back(g);
  }
  std::string text;
  for (const auto& t : treffer) text += (text.empty() ? "" : ", ") + t;
  return text.empty() ? "keiner" : text;
}

// Nach jedem gelungenen Gerät: beim ersten (und nach einem Rückfall) melden,
// welcher Treiber geladen ist.
void Gelungen() {
  std::lock_guard<std::mutex> halt(g_sperre);
  Wahl& w = W();
  if (!w.erstes_geraet) return;
  w.erstes_geraet = false;
  w.geladen = GeladeneTreiber(w);
  const bool eigener = !w.eigen.empty() && w.geladen.find(w.eigen + "/") != std::string::npos;
  Protokoll("VA-Treiber geladen: %s%s", w.geladen.c_str(),
            w.vorgeschaltet && !eigener
                ? " — ★ der vorgeschaltete mitgelieferte Treiber wurde ÜBERGANGEN "
                  "(libva fand ihn nicht ladbar oder ohne passendes Init-Symbol)"
                : "");
}

bool Zurueckstellen(const char* wer, int fehler) {
  std::lock_guard<std::mutex> halt(g_sperre);
  Wahl& w = W();
  if (!w.vorgeschaltet || !w.erstes_geraet || w.rueckfall) return false;
  w.rueckfall = true;
  w.vorgeschaltet = false;
  UrsprungHerstellen(w);
  ::unsetenv(kVorher);
  w.text = "System — Rückfall: erstes VA-Gerät (" + std::string(wer) +
           ") scheiterte mit dem mitgelieferten Treiber (" + ffmpeg::FehlerText(fehler) + ")";
  Protokoll("VA-Treiber: %s — zweiter Versuch mit dem Systemtreiber", w.text.c_str());
  return true;
}

}  // namespace

std::string Lage() {
  std::lock_guard<std::mutex> halt(g_sperre);
  const Wahl& w = W();
  return w.geladen.empty() ? w.text : w.text + "; geladen: " + w.geladen;
}

std::string VaKnoten() { return VaKnotenErmitteln(); }

int GeraetErzeugen(AVBufferRef** ziel, const char* wer) {
  const auto& api = ffmpeg::Zugriff();
  int r = api.av_hwdevice_ctx_create(ziel, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
  bool ok = r >= 0 && *ziel;
  if (!ok && Zurueckstellen(wer, r)) {
    if (*ziel) api.av_buffer_unref(ziel);
    r = api.av_hwdevice_ctx_create(ziel, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
    ok = r >= 0 && *ziel;
    Protokoll("VA-Treiber: zweiter Versuch (%s): %s", wer,
              ok ? "geöffnet" : ffmpeg::FehlerText(r).c_str());
  }
  if (!ok) return r < 0 ? r : AVERROR_UNKNOWN;
  Gelungen();
  return 0;
}

}  // namespace vatreiber
}  // namespace libwebrtc
