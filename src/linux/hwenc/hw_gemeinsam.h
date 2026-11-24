#ifndef LIBWEBRTC_LINUX_HWENC_HW_GEMEINSAM_H_
#define LIBWEBRTC_LINUX_HWENC_HW_GEMEINSAM_H_

// Was Encoder und Decoder unter Linux gemeinsam brauchen: die eigene
// Protokolldatei `hwenc.log` und die Angaben zu Grafikkarte, Treiber und
// Systemlage. Bis hwenc.12 lag das alles im Encoder.

#include <mutex>
#include <string>

extern "C" {
#include "libavutil/buffer.h"
}

namespace libwebrtc {

// Eine Zeile mit Zeitstempel nach `hwenc.log` (neben send.log, gedeckelt
// 512 KB). Fadensicher.
void Protokoll(const char* format, ...) __attribute__((format(printf, 1, 2)));

// Wahr, wenn sich irgendein /dev/dri/renderD* öffnen lässt.
bool RenderKnotenDa();

// Eine Zeile aus sysfs/proc, ohne Zeilenende; „?" wenn nicht lesbar.
std::string SysfsZeile(const std::string& pfad);

// Hersteller:Gerät (Treiber) je Render-Knoten.
std::string GpuBeschreibung();

// CPU-Takt, Regler, GPU-Takt, Netzteil, Last.
std::string SystemLage();

// Der geladene VA-Treiber samt Version (vaQueryVendorString), „?" sonst.
std::string VaTreiber(AVBufferRef* geraet);

// ★★ hwenc.19 (23.11.2026): prozessweite Umbau-Sperre für VA-API.
//
// GEMESSEN (UM890, VCN4, Mesa 26.2.7/26.3.1, test/neuaufsetzen_test.cc):
// Mesa schickt beim Abbau eines VA-Encoders den Sitzungsabschluss nur
// asynchron an VCN und gibt Puffer samt GPU-Adressen sofort frei. Legt ein
// ANDERER VA-Nutzer im selben Prozess in diesem Moment neue Oberflächen an
// und füllt sie, kommt bei belegtem VCN ein Ring-Timeout — der Prozess bricht
// ab, fremde VCN-Nutzer (Sunshine) stürzen mit. Ein einzelner Encoder allein
// überstand dieselbe Lage (179 Größenwechsel), zwei gleichzeitig nicht.
//
// Regel: Abbau eines VA-Kontexts und Anlegen von VA-Gerät oder Bildvorrat
// laufen unter dieser Sperre; nach einem Abbau wartet jedes Anlegen, bis er
// `kVaRuheMs` zurückliegt. Reihenfolge: zuerst die eigene Sperre des
// Encoders/Decoders, dann diese (nie umgekehrt).
constexpr int kVaRuheMs = 150;
std::recursive_mutex& VaUmbauSperre();
// Unter der Sperre: eben wurde ein VA-Kontext (Encoder/Decoder) abgebaut.
void VaAbbauVermerken();
// Unter der Sperre, vor dem Anlegen von Gerät oder Bildvorrat: wartet, bis
// der letzte Abbau `kVaRuheMs` zurückliegt. Rückgabe: gewartete ms.
int VaRuheAbwarten();

// ★★ hwenc.19: EIN VA-Gerät je Prozess. GEMESSEN mit V3 Linux 2.6.364 (BRIO,
// Mesa 26.2.7, kopflos, HEVC-Last daneben): Jeder Absturz fiel auf das
// ANLEGEN eines neuen VA-Geräts im laufenden Prozess (erstes Öffnen beim
// Kamerastart, Kamera wieder an, Neuaufsetzen mit neuem Gerät); Neuaufsetzen
// auf einem behaltenen Gerät nie. Die Verfügbarkeitsprüfung des Encoders legt
// beim Start (bevor Oberfläche und Medien laufen) ein Gerät an und gibt es
// hierher; es wird nie freigegeben. Encoder und Null-Kopie nehmen es.
// ★ SEIT 24.11. abends AUS (nur noch mit HC_VA_DAUERGERAET=1): zwei Encoder
// auf einer VA-Anzeige hängen VCN im Release-Weg (s. hw_gemeinsam.cc).
// `VaDauerGeraetSetzen` übernimmt die Referenz (nur der erste Aufruf zählt).
void VaDauerGeraetSetzen(AVBufferRef* geraet);
// Neue Referenz auf das Dauergerät oder nullptr.
AVBufferRef* VaDauerGeraetRef();


}  // namespace libwebrtc

#endif  // LIBWEBRTC_LINUX_HWENC_HW_GEMEINSAM_H_
