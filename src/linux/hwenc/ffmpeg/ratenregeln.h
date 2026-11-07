#ifndef LIBWEBRTC_LINUX_HWENC_RATENREGELN_H_
#define LIBWEBRTC_LINUX_HWENC_RATENREGELN_H_

// Reine Entscheidungsregeln des Linux-Hardware-Encoders (VA-API) — nur
// Ganzzahlen hinein, Wahrheitswert oder Zahl heraus. Kein WebRTC, kein FFmpeg.
//
// ★ WARUM EIGENE DATEI: Die Regeln in `hw_h264_encoder.cc` sind nur im
// GN-Baum des Forks übersetzbar (Stunden) und nur im Gespräch messbar. Was
// hier liegt, prüft `test/ratenregeln_test.cc` mit einem gewöhnlichen
// Compiler in einer Sekunde — der Nachweis für O34 (Anlauf-Rampe) steht dort.
// Alles, was Treiber oder Zeit braucht, bleibt im Encoder.

#include <algorithm>
#include <cstdint>

namespace libwebrtc {
namespace ratenregeln {

// ★★ O34 (Bündel 120, Jim, Intel Meteor Lake, 06.11.2026): Bis 8 Mbit/s
// dauerte der Anlauf 100 s. WebRTC hebt die Forderung alle ~2 s um ~15 %;
// der Encoder folgte nur, wenn die Forderung 1,5× der beim Öffnen erreichte
// (sofort) oder 1,2× drei Sekunden lang hielt (dann 5 s Sperre) — also immer
// einen Schritt hinterher, und die app-limitierte Schätzung wächst nur, wenn
// auch gesendet wird. Deshalb: in den ersten 60 s nach `InitEncode` einer
// HÖHEREN Forderung sofort folgen (Schwelle 1,1×, keine Haltezeit),
// Neuaufsetzen höchstens alle 2 s (jedes kostet ein Vollbild). Abwärts
// unverändert (Lehre 21.09.: nie hastig).
constexpr int64_t kRampeMs = 60000;
constexpr int64_t kRampeSperreMs = 2000;

// Wahr, wenn in der Rampe neu aufgesetzt werden soll: die ROHE Forderung
// liegt ≥ 1,1× über der Forderung beim letzten Öffnen, UND der ausgeglichene
// Wert liegt über dem Encoder-Ziel — sonst ergäbe `max(neu, Ziel)` dieselbe
// Rate, ein Vollbild für nichts.
//
// ★ WARUM NICHT „neu ≥ 1,1 × Ziel" wie in der Folge nach dem Anlauf: Der
// Ausgleicher deckelt bei 0,95× der Forderung (gemessen am Code,
// `BitrateAdjuster`); 0,95 × 1,15 = 1,09 < 1,1 — ein 15-%-Schritt der
// Schätzung hätte zwischen zwei `SetRates` NIE gezählt, und der in der Sperre
// gemerkte Sprung wäre in `Encode` verfallen. „Über dem Ziel" genügt: der
// Gewinn ist dann mindestens 0,95 × 1,1 − 1 = 4,5 %, im Regelfall 10–15 %.
inline bool RampeHoch(uint32_t forderung_roh, uint32_t gefordert_offen,
                      uint32_t neu, uint32_t ziel) {
  if (gefordert_offen == 0 || ziel == 0) return false;
  const bool ueber = static_cast<uint64_t>(forderung_roh) * 10 >=
                     static_cast<uint64_t>(gefordert_offen) * 11;
  return ueber && neu > ziel;
}

// ★★ O34 (2): Startrate. WebRTC öffnet mit 290 kbit/s (sein Start der
// Schätzung), die ersten Sekunden sind matschig. Ab 720p mit 1300 kbit/s
// beginnen, darunter anteilig zur Fläche (640×360 = ein Viertel → 325), nie
// unter der Vorgabe, nie über dem Deckel (`maxBitrate`, falls gesetzt).
// Nur VA-API — NVENC stellt sich in `SetRates` ohne Vollbild um.
constexpr uint32_t kStartrateBps = 1300000;
constexpr int64_t kStartratePixel = 1280 * 720;

inline uint32_t Startrate(uint32_t vorgabe_bps, uint32_t max_bps, int breite,
                          int hoehe) {
  if (breite <= 0 || hoehe <= 0) return vorgabe_bps;
  const int64_t pixel = static_cast<int64_t>(breite) * hoehe;
  uint64_t start = kStartrateBps;
  if (pixel < kStartratePixel) {
    start = static_cast<uint64_t>(kStartrateBps) * pixel / kStartratePixel;
  }
  start = std::max<uint64_t>(start, vorgabe_bps);
  if (max_bps > 0) start = std::min<uint64_t>(start, max_bps);
  return static_cast<uint32_t>(start);
}

// ★★ O34 (3): Diagnose „Encoder haengt". Bis hwenc.11 reichte ein Abstand
// von 30 % zwischen Forderung und Encoder-Ziel — Jim (23:01:19): „Forderung
// 908, Encoder 1306, gesendet 1030": der Encoder sendete MEHR als gefordert,
// kein Hänger. Jetzt zusätzlich: gesendet bekannt (Fenster voll) und
// < 0,8× der Forderung. Wer mehr liefert als gefordert, hängt nicht.
// Der 30-%-Abstand bleibt als Vorbedingung (Prüfbefund N3, hwenc.10): ohne
// ihn meldete ein ruhender Bildschirm mit Ziel = Forderung laufend.
inline bool HaengtLage(uint32_t forderung_roh, uint32_t ziel,
                       uint32_t gesendet) {
  if (forderung_roh == 0 || ziel == 0 || gesendet == 0) return false;
  const uint64_t g = std::max(forderung_roh, ziel);
  const uint64_t k = std::min(forderung_roh, ziel);
  const bool klafft = g * 100 > k * 130;
  const bool zu_wenig = static_cast<uint64_t>(gesendet) * 10 <
                        static_cast<uint64_t>(forderung_roh) * 8;
  return klafft && zu_wenig;
}

}  // namespace ratenregeln
}  // namespace libwebrtc

#endif  // LIBWEBRTC_LINUX_HWENC_RATENREGELN_H_
