#ifndef LIBWEBRTC_LINUX_HWENC_HW_GEMEINSAM_H_
#define LIBWEBRTC_LINUX_HWENC_HW_GEMEINSAM_H_

// Was Encoder und Decoder unter Linux gemeinsam brauchen: die eigene
// Protokolldatei `hwenc.log` und die Angaben zu Grafikkarte, Treiber und
// Systemlage. Bis hwenc.12 lag das alles im Encoder.

#include <string>

extern "C" {
#include "libavutil/buffer.h"
}

namespace libwebrtc {

// Eine Zeile mit Zeitstempel nach `hwenc.log` (neben send.log, gedeckelt
// 512 KB). Fadensicher.
void Protokoll(const char* format, ...) __attribute__((format(printf, 1, 2)));

// Wahr, wenn sich /dev/dri/renderD128 oder renderD129 öffnen lässt.
bool RenderKnotenDa();

// Eine Zeile aus sysfs/proc, ohne Zeilenende; „?" wenn nicht lesbar.
std::string SysfsZeile(const std::string& pfad);

// Hersteller:Gerät (Treiber) je Render-Knoten.
std::string GpuBeschreibung();

// CPU-Takt, Regler, GPU-Takt, Netzteil, Last.
std::string SystemLage();

// Der geladene VA-Treiber samt Version (vaQueryVendorString), „?" sonst.
std::string VaTreiber(AVBufferRef* geraet);

}  // namespace libwebrtc

#endif  // LIBWEBRTC_LINUX_HWENC_HW_GEMEINSAM_H_
