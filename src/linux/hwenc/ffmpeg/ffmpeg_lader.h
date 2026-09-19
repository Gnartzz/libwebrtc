#ifndef LIBWEBRTC_LINUX_HWENC_FFMPEG_LADER_H_
#define LIBWEBRTC_LINUX_HWENC_FFMPEG_LADER_H_

// Lädt zur Laufzeit die Handvoll FFmpeg-Funktionen, die der VA-API-Encoder
// braucht — über den Soname, niemals beim Binden.
//
// ★ WARUM DLOPEN UND NICHT LINKEN. Gegen libavcodec zu binden hieße: Ein
// Client, der gegen .so.61 gebaut wurde, startet auf einem Rechner mit .so.60
// überhaupt nicht — der Nutzer sieht eine Meldung über eine fehlende
// Bibliothek für eine Funktion, nach der er nie gefragt hat. Zur Laufzeit
// geladen wird daraus „heute kein Hardware-Encoder, wir kodieren in Software".
// Genau so hat ein Codec auszufallen.
//
// ★ WARUM DIE HEADER TROTZDEM MITLIEGEN (und zwar GENAU EINE FASSUNG).
// Gemessen am 19.09.2026: Die Optionen von AVCodecContext heißen `b`, `g`,
// `bf`, `maxrate`, `bufsize`, `refs`, `profile` — für `pixel_format`,
// `video_size` und `time_base` gibt es GAR KEINE Option, und AVHWFramesContext
// hat überhaupt keine Optionsschnittstelle. Ohne Header ginge es also nicht
// ohne von Hand nachgebaute Strukturen, und die tragen nicht: `AVFrame.key_frame`
// ist in FFmpeg 7 entfallen, womit sich alle folgenden Felder verschieben. Ein
// solcher Fehler zeigt sich nicht als Absturz, sondern als zerfallendes Bild
// beim Gegenüber. Also: die öffentlichen Header von FFmpeg 7.1.1 liegen unter
// `ffmpeg/include/`, und `Laden()` besteht darauf, dass die gefundene
// Bibliothek DIESELBE Hauptversion hat (61/59). Passt sie nicht, gibt es
// Software — kein Raten an Strukturen vorbei.
//
// ★ ZIELE, GEMESSEN: UM890/SteamOS und Fedora 42 tragen libavcodec.so.61,
// die Flatpak-Laufzeit org.freedesktop.Platform 25.08 ebenfalls (FFmpeg 7.1).
// Das sind genau unsere beiden Auslieferungswege. Wer eine andere Fassung
// fährt (z. B. Ubuntu 24.04 mit .so.60), kodiert in Software; eine zweite
// Header-Fassung ließe sich später danebenlegen, ohne diesen Encoder anzufassen.
//
// ★ LIZENZ: FFmpeg ist LGPL. Zur Laufzeit zu laden hält uns von den Pflichten
// des statischen Bindens frei, und wir liefern selbst keinen FFmpeg-Code aus.

#include <cstdint>
#include <string>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/avutil.h"
#include "libavutil/hwcontext.h"
#include "libavutil/opt.h"
#include "libavutil/pixfmt.h"
}

// ★ MERKE, WESSEN HEADER DAS SIND (19.09.2026 gemessen): Als der Include-Pfad
// nach einer Umbenennung ins Leere zeigte, fand der Compiler klaglos die
// FFmpeg-Header des Bau-Containers. Hier fiel es nur auf, weil die aelteren
// Header ein paar Namen nicht kannten — waeren sie zufaellig uebersetzbar
// gewesen, haetten wir gegen fremde Strukturlagen gebaut und es erst am
// zerfallenden Bild beim Gegenueber gemerkt. Diese Zusicherung sagt es sofort.
static_assert(LIBAVCODEC_VERSION_MAJOR == 61 && LIBAVUTIL_VERSION_MAJOR == 59,
              "Es wurden andere FFmpeg-Header gefunden als die mitgelieferten "
              "(erwartet avcodec 61 / avutil 59 aus ffmpeg/include). Pruefe "
              "include_dirs in BUILD.gn.");

namespace libwebrtc {
namespace ffmpeg {

// Alles, was wir aufrufen, an einer Stelle. Null, bis Laden() geglückt ist.
struct Api {
  // libavcodec
  const AVCodec* (*avcodec_find_encoder_by_name)(const char*) = nullptr;
  AVCodecContext* (*avcodec_alloc_context3)(const AVCodec*) = nullptr;
  void (*avcodec_free_context)(AVCodecContext**) = nullptr;
  int (*avcodec_open2)(AVCodecContext*, const AVCodec*, AVDictionary**) = nullptr;
  int (*avcodec_send_frame)(AVCodecContext*, const AVFrame*) = nullptr;
  int (*avcodec_receive_packet)(AVCodecContext*, AVPacket*) = nullptr;
  AVPacket* (*av_packet_alloc)() = nullptr;
  void (*av_packet_free)(AVPacket**) = nullptr;
  void (*av_packet_unref)(AVPacket*) = nullptr;
  unsigned (*avcodec_version)() = nullptr;

  // libavutil
  AVFrame* (*av_frame_alloc)() = nullptr;
  void (*av_frame_free)(AVFrame**) = nullptr;
  void (*av_frame_unref)(AVFrame*) = nullptr;
  int (*av_frame_get_buffer)(AVFrame*, int) = nullptr;
  int (*av_frame_make_writable)(AVFrame*) = nullptr;
  int (*av_hwdevice_ctx_create)(AVBufferRef**, enum AVHWDeviceType, const char*,
                                AVDictionary*, int) = nullptr;
  AVBufferRef* (*av_hwframe_ctx_alloc)(AVBufferRef*) = nullptr;
  int (*av_hwframe_ctx_init)(AVBufferRef*) = nullptr;
  int (*av_hwframe_get_buffer)(AVBufferRef*, AVFrame*, int) = nullptr;
  int (*av_hwframe_transfer_data)(AVFrame*, const AVFrame*, int) = nullptr;
  AVBufferRef* (*av_buffer_ref)(const AVBufferRef*) = nullptr;
  void (*av_buffer_unref)(AVBufferRef**) = nullptr;
  int (*av_opt_set)(void*, const char*, const char*, int) = nullptr;
  int (*av_opt_set_int)(void*, const char*, int64_t, int) = nullptr;
  int (*av_strerror)(int, char*, size_t) = nullptr;
  unsigned (*avutil_version)() = nullptr;
};

// Lädt libavcodec/libavutil genau einmal. Mehrfach aufrufbar, fadensicher,
// wirft nie. Liefert false, sobald irgendetwas fehlt oder die Hauptversion
// nicht zu den mitgelieferten Headern passt — der Aufrufer meldet dann
// „nicht unterstützt", und die Fabrik behält den Software-Encoder.
bool Laden();

// Nur nach erfolgreichem Laden() gültig.
const Api& Zugriff();

// Für die Protokollzeile und den Fuß im Client, z. B. „libavcodec.so.61".
const std::string& GeladeneBibliothek();

// Fehlernummer als Text — für Protokollzeilen, die man auch in einem Jahr
// noch lesen kann. Ohne geladene Bibliothek nur die nackte Zahl.
std::string FehlerText(int fehler);

}  // namespace ffmpeg
}  // namespace libwebrtc

#endif  // LIBWEBRTC_LINUX_HWENC_FFMPEG_LADER_H_
