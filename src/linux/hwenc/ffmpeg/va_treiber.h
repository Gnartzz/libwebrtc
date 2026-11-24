#ifndef LIBWEBRTC_LINUX_HWENC_FFMPEG_VA_TREIBER_H_
#define LIBWEBRTC_LINUX_HWENC_FFMPEG_VA_TREIBER_H_

// Wahl des VA-Treibers (hwenc.19): der mitgelieferte radeonsi-VA-Treiber
// (`hc-va/` neben dieser Bibliothek) gegen den Systemtreiber. Hintergrund,
// Regeln und Belege stehen in va_treiber.cc.

#include <string>

extern "C" {
#include "libavutil/buffer.h"
}

namespace libwebrtc {
namespace vatreiber {

// Eine Zeile für hwenc.log: was gewählt wurde und warum, nach dem ersten
// VA-Gerät auch, welcher Treiber laut /proc/self/maps wirklich geladen ist.
std::string Lage();

// Der Render-Knoten, den FFmpeg für ein VA-Gerät ohne Gerätevorgabe öffnet
// (z. B. "renderD128"), "" wenn keiner.
std::string VaKnoten();

// Wie `av_hwdevice_ctx_create(ziel, AV_HWDEVICE_TYPE_VAAPI, nullptr,
// nullptr, 0)` — ALLE VA-Geräte dieser Bibliothek gehen hier durch. Scheitert
// das ERSTE Gerät des Prozesses mit vorgeschaltetem mitgeliefertem Treiber,
// wird prozessweit auf den Systemtreiber zurückgestellt und einmal neu
// versucht. `wer` steht im Protokoll. Rückgabe wie FFmpeg (< 0 = Fehler).
int GeraetErzeugen(AVBufferRef** ziel, const char* wer);

}  // namespace vatreiber
}  // namespace libwebrtc

#endif  // LIBWEBRTC_LINUX_HWENC_FFMPEG_VA_TREIBER_H_
