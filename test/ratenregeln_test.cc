// Prüfung der reinen Ratenregeln des Linux-Hardware-Encoders (O34) — ohne
// WebRTC, ohne FFmpeg, ohne GN. Übersetzen und laufen lassen:
//
//   c++ -std=c++17 -I. test/ratenregeln_test.cc -o /tmp/ratenregeln_test && /tmp/ratenregeln_test
//
// Teil 1: Einzelfälle, mit Jims Zahlen aus Bündel 120 (06.11.2026).
// Teil 2: Modell der Anlauf-Rampe, alte Regel gegen neue. KEINE Messung —
// die Schätzung ist nachgebaut (alle 2 s +15 %, gedeckelt auf 1,5× des
// Gesendeten + 10 kbit/s wie `AimdRateControl`), der Treiber als Faktor
// „gesendet/Ziel" (1,0 = trifft; 0,8 = Intel-Unterschuss wie bei Jim).

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "src/linux/hwenc/ffmpeg/ratenregeln.h"

using namespace libwebrtc::ratenregeln;

namespace {

int fehler = 0;

void Pruefe(bool ok, const char* was) {
  std::printf("  %s  %s\n", ok ? "ok  " : "FEHL", was);
  if (!ok) ++fehler;
}

// ── Teil 2: Rampenmodell ────────────────────────────────────────────────────
// Rückgabe: Sekunde, in der das Encoder-Ziel zuerst ≥ 7200 kbit/s (90 % des
// 8000er-Deckels) liegt (oder -1), dazu die Zahl der Neuaufsetzer. `neu` = Regel O34, sonst hwenc.11.
struct Lauf {
  int sekunde_8000 = -1;   // erste Sekunde mit Ziel >= 90 % des Deckels (der Ausgleicher hält 0,95x)
  int aufsetzer = 0;
  uint32_t ziel_nach_30s = 0;
  uint32_t ziel_nach_60s = 0;
  uint32_t ziel_nach_100s = 0;
};

Lauf Rampe(bool neu, double treiber) {
  const uint32_t kStart = 290000;         // WebRTCs Startschätzung
  const uint32_t kSprung = 1306000;        // Jims Log: nach 2 s auf 1306 (Sonden)
  const uint32_t kDeckel = 8000000;
  // Encoder-Zustand wie im Code: Ziel, Forderung beim Öffnen, Sperre.
  uint32_t ziel = neu ? Startrate(kStart, 24000000, 1728, 1072) : kStart;
  uint32_t gefordert_offen = ziel;
  int letztes_aufsetzen = 0;
  int hoch_seit = -1;        // 1,2×-Folge (nur alte Regel)
  int unterschuss_seit = -1; // hwenc.11
  int fenster_voll_ab = 5;   // 5 s nach dem Öffnen gibt es eine gesendete Rate
  uint32_t forderung = kStart;
  Lauf lauf;
  for (int t = 1; t <= 180; ++t) {
    // Schätzung: bei t = 2 s der Sondensprung, danach alle 2 s +15 %, gedeckelt
    // app-limitiert auf 1,5× gesendet + 10 k.
    const uint32_t gesendet = static_cast<uint32_t>(ziel * treiber);
    if (t == 2) {
      forderung = kSprung;
    } else if (t % 2 == 0) {
      uint64_t f = static_cast<uint64_t>(forderung) * 115 / 100;
      const uint64_t app = static_cast<uint64_t>(gesendet) * 3 / 2 + 10000;
      if (f > app) f = app;
      if (f < forderung) f = forderung;   // die Rampe fällt im Modell nie
      forderung = static_cast<uint32_t>(f > kDeckel ? kDeckel : f);
    }
    // Ausgleicher: bei > 10 % Änderung = Forderung, sonst 0,95× (Unterschuss).
    const uint32_t ausgeglichen = static_cast<uint32_t>(forderung * 0.95);
    const bool fenster = t - fenster_voll_ab >= 0;
    const int seit = t - letztes_aufsetzen;

    bool aufsetzen = false;
    uint32_t soll = ziel;
    // 1,5×-Sprung (beide Regeln), Sperre 2 s im Anlauf (8 s) bzw. Rampe (60 s), sonst 10 s.
    const bool sprung = static_cast<uint64_t>(forderung) * 2 >= static_cast<uint64_t>(gefordert_offen) * 3;
    const bool rampe = neu && t < 60;
    if (neu && rampe && RampeHoch(forderung, gefordert_offen, forderung, ziel) && seit >= 2) {
      aufsetzen = true;
      soll = forderung;   // SetRates mit > 10 % Änderung: Ausgleicher = Forderung
    }
    if (!aufsetzen && sprung && seit >= ((neu ? rampe : t < 8) ? 2 : 10)) {
      aufsetzen = true;
      soll = forderung > ziel ? forderung : ziel;
    }
    // 1,2× drei Sekunden (nach 8 s), Sperre 5 s — hwenc.10, in beiden Regeln.
    const bool ueber = static_cast<uint64_t>(forderung) * 5 >= static_cast<uint64_t>(gefordert_offen) * 6;
    if (!ueber) hoch_seit = -1; else if (hoch_seit < 0) hoch_seit = t;
    if (!aufsetzen && ueber && t >= 8 && hoch_seit >= 0 && t - hoch_seit >= 3 && seit >= 5 &&
        static_cast<uint64_t>(ausgeglichen) * 10 >= static_cast<uint64_t>(ziel) * 11) {
      aufsetzen = true;
      soll = ausgeglichen;
    }
    // Unterschuss (hwenc.11): F ≥ 1,1 Z, gesendet < 0,85 Z, ≥ 0,5 Z, 5 s, Sperre 5 s.
    const bool lage = t >= 8 && fenster && static_cast<uint64_t>(forderung) * 10 >= static_cast<uint64_t>(ziel) * 11 &&
                      static_cast<uint64_t>(gesendet) * 100 < static_cast<uint64_t>(ziel) * 85 &&
                      static_cast<uint64_t>(gesendet) * 2 >= ziel;
    if (!lage) unterschuss_seit = -1; else if (unterschuss_seit < 0) unterschuss_seit = t;
    if (!aufsetzen && lage && t - unterschuss_seit >= 5 && seit >= 5) {
      uint64_t s = static_cast<uint64_t>(ziel) * ziel / gesendet;
      if (s > forderung) s = forderung;
      if (s < static_cast<uint64_t>(ziel) * 11 / 10) s = static_cast<uint64_t>(ziel) * 11 / 10;
      if (s > forderung) s = forderung;
      if (s > ziel) {
        aufsetzen = true;
        soll = static_cast<uint32_t>(s);
      }
    }
    if (aufsetzen) {
      ziel = soll;
      gefordert_offen = forderung;
      letztes_aufsetzen = t;
      hoch_seit = -1;
      unterschuss_seit = -1;
      fenster_voll_ab = t + 5;
      ++lauf.aufsetzer;
    }
    if (t == 30) lauf.ziel_nach_30s = ziel;
    if (t == 60) lauf.ziel_nach_60s = ziel;
    if (t == 100) lauf.ziel_nach_100s = ziel;
    if (lauf.sekunde_8000 < 0 && static_cast<uint64_t>(ziel) * 10 >= static_cast<uint64_t>(kDeckel) * 9) {
      lauf.sekunde_8000 = t;
    }
  }
  return lauf;
}

}  // namespace

int main() {
  std::printf("Teil 1 — Einzelfaelle\n");
  // Startrate (O34 2)
  Pruefe(Startrate(290000, 24000000, 1920, 1080) == 1300000, "Startrate 1080p = 1300 kbit/s");
  Pruefe(Startrate(290000, 24000000, 1280, 720) == 1300000, "Startrate 720p = 1300 kbit/s");
  Pruefe(Startrate(290000, 0, 640, 360) == 325000, "Startrate 360p anteilig = 325 kbit/s");
  Pruefe(Startrate(290000, 0, 320, 180) == 290000, "Startrate 180p nie unter der Vorgabe");
  Pruefe(Startrate(290000, 1000000, 1920, 1080) == 1000000, "Startrate nie ueber maxBitrate");
  Pruefe(Startrate(2000000, 0, 1920, 1080) == 2000000, "Startrate nie unter einer hoeheren Vorgabe");
  Pruefe(Startrate(290000, 0, 0, 0) == 290000, "Startrate ohne Masse = Vorgabe");

  // Rampe (O34 1), Jims Zahlen: Start 1300, Forderung 1306 → kein Vollbild fuer +0,5 %
  Pruefe(!RampeHoch(1306000, 1300000, 1306000, 1300000), "Rampe: 1306 bei Start 1300 setzt NICHT neu auf");
  Pruefe(RampeHoch(1544000, 1306000, 1544000, 1306000), "Rampe: 1306 -> 1544 (1,18x) setzt sofort neu auf");
  Pruefe(RampeHoch(1500000, 1306000, 1425000, 1306000),
         "Rampe: 15-%-Schritt zaehlt auch mit Ausgleicher bei 0,95x (1,1x auf den Ausgleicher haette NICHT gezaehlt)");
  Pruefe(!RampeHoch(1400000, 1306000, 1400000, 1306000), "Rampe: 1,07x reicht nicht");
  Pruefe(!RampeHoch(1544000, 1306000, 1200000, 1306000), "Rampe: Ausgleicher unter dem Ziel -> kein Vollbild fuer nichts");
  Pruefe(!RampeHoch(908000, 1306000, 908000, 1306000), "Rampe: abwaerts nie (Jim 23:01:19)");
  Pruefe(!RampeHoch(1544000, 0, 1544000, 1306000), "Rampe: ohne Bezug nichts");

  // Haengt (O34 3)
  Pruefe(!HaengtLage(908000, 1306000, 1030000), "Haengt: Jim 23:01:19 (gesendet 1030 > Forderung 908) meldet NICHT mehr");
  Pruefe(HaengtLage(1306000, 908000, 900000), "Haengt: Encoder 908 hinter Forderung 1306, gesendet 900 (69 %) meldet");
  Pruefe(!HaengtLage(1306000, 908000, 0), "Haengt: ohne volles Fenster keine Meldung");
  Pruefe(!HaengtLage(1000000, 1000000, 500000), "Haengt: Ziel = Forderung (ruhiger Bildschirm) meldet nicht");
  Pruefe(HaengtLage(4000000, 1300000, 1200000), "Haengt: 1300 hinter 4000, gesendet 1200 (30 %) meldet");
  Pruefe(!HaengtLage(1000000, 1400000, 810000), "Haengt: gesendet 81 % der Forderung meldet nicht");
  Pruefe(HaengtLage(1000000, 1400000, 790000), "Haengt: gesendet 79 % der Forderung meldet (Ziel 1,4x)");

  std::printf("\nTeil 2 — Rampenmodell (Sekunde, in der das Ziel 90 %% von 8000 kbit/s erreicht; Neuaufsetzer; Ziel nach 30/60/100 s)\n");
  for (double treiber : {1.0, 0.8}) {
    const Lauf alt = Rampe(false, treiber);
    const Lauf neu = Rampe(true, treiber);
    std::printf("  Treiber %.2f: hwenc.11  %3d s  %2d Aufsetzer  %5u / %5u / %5u kbit/s\n", treiber,
                alt.sekunde_8000, alt.aufsetzer, alt.ziel_nach_30s / 1000, alt.ziel_nach_60s / 1000,
                alt.ziel_nach_100s / 1000);
    std::printf("  Treiber %.2f: O34       %3d s  %2d Aufsetzer  %5u / %5u / %5u kbit/s\n", treiber,
                neu.sekunde_8000, neu.aufsetzer, neu.ziel_nach_30s / 1000, neu.ziel_nach_60s / 1000,
                neu.ziel_nach_100s / 1000);
    Pruefe(neu.sekunde_8000 > 0 && (alt.sekunde_8000 < 0 || neu.sekunde_8000 < alt.sekunde_8000),
           "O34 erreicht 90 % von 8000 kbit/s frueher als hwenc.11");
    Pruefe(neu.aufsetzer <= 30, "O34: hoechstens ein Neuaufsetzen je 2 s in der Rampe");
  }

  std::printf("\n%s (%d Fehler)\n", fehler ? "FEHLGESCHLAGEN" : "ALLES OK", fehler);
  return fehler ? 1 : 0;
}
