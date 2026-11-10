#ifndef LIBWEBRTC_LINUX_HWENC_FFMPEG_HW_NULLKOPIE_H_
#define LIBWEBRTC_LINUX_HWENC_FFMPEG_HW_NULLKOPIE_H_

// Null-Kopie für die Bildschirmfreigabe unter Linux (08.11.2026).
//
// ★ GEMESSEN (CT 107, Intel UHD 630, Messprogramm pwweg.c): Der alte Weg —
// PipeWire-DMA-BUF per glReadPixels in den Arbeitsspeicher, libyuv
// ARGBToI420, I420Scale, I420→NV12, Hochladen, h264_vaapi — kostet 5–10 ms
// CPU je Bild (2880×1800 → 1152×720 bei 60 fps: 9,7 ms, mehr als die Hälfte
// eines Kerns). Den DMA-BUF als VA-Oberfläche zu übernehmen und die
// Grafikkarte umrechnen zu lassen (VPP, BT.601 begrenzt wie libyuv) kostet
// 0,6–0,8 ms, Farben gleich (Y mittel 0,06, UV 0,86).
//
// Aufbau:
//  - Der PipeWire-Aufnehmer (WebRTC-Baum, Patch honeycord_pipewire_nullkopie)
//    fragt vor dem Auslesen `webrtc::HcDmaBufWandler`. Der hier liefert ein
//    `HcVaBild` (VideoFrameBuffer kNative, NV12 auf der Grafikkarte).
//  - `HcVaBild` kann sich auf der Grafikkarte zuschneiden und skalieren
//    (CropAndScale) und notfalls nach I420 zurücklesen (ToI420) — damit bleibt
//    jeder Verbraucher korrekt, der kein GPU-Bild versteht (Software-Encoder,
//    VP8, Vorschau).
//  - Der VA-API-Encoder nimmt `HcVaBild` direkt (gleiches VA-Gerät).
//
// Abschalten: HONEYCORD_NULLKOPIE=0. Nur VA-API, nie NVIDIA.

#include <cstdint>

extern "C" {
#include "libavutil/buffer.h"
#include "libavutil/frame.h"
}

#include "api/video/video_frame_buffer.h"

namespace libwebrtc {
namespace nullkopie {

// Einmal je Prozess: VA-Gerät öffnen, Umrechnung prüfen, den Wandler beim
// Aufnehmer eintragen. true = Null-Kopie ist aktiv.
bool Einrichten();

// Wahr nach erfolgreichem `Einrichten()`.
bool Aktiv();

// Neue Referenz auf das gemeinsame VA-Gerät (Encoder und Wandler müssen
// dasselbe benutzen), nullptr wenn Null-Kopie aus ist.
AVBufferRef* GeraetRef();

// Ist `puffer` ein GPU-Bild auf `geraet` in genau `breite`×`hoehe`, kommt
// dessen Rahmen zurück (geborgt, lebt so lange wie `puffer`), sonst nullptr.
const AVFrame* AlsVaRahmen(const webrtc::VideoFrameBuffer& puffer,
                           const AVBufferRef* geraet, int breite, int hoehe);

// ── Empfang ohne Umweg (10.11.2026) ──
// Ein GPU-Bild für die Anzeige, ABI-fest (das Plugin fragt es per dlsym ab,
// `honeycord_gpu_bild`). NV12 als zwei Ebenen: [0] Y (DRM R8), [1] UV (GR88).
// Die Deskriptoren gehören danach dem Aufrufer (schließen!).
struct HcGpuBild {
  uint32_t version;  // 1
  int32_t breite, hoehe;
  struct {
    int32_t fd;
    uint32_t drm_format, offset, pitch;
    uint64_t modifier;
  } ebene[2];
};

// Das Plugin meldet, ob seine Anzeige GPU-Bilder übernehmen kann.
void EmpfangMelden(bool an);
// Den GPU-Weg beim Empfang für den Prozess beenden (Fehlschläge).
void EmpfangSperren(const char* grund);
// Sollen Decoder GPU-Bilder ausgeben? (gemeldet + Null-Kopie aktiv +
// HONEYCORD_EMPFANG_GPU != 0)
bool EmpfangAktiv();
// Darf der Decoder das gemeinsame Gerät nehmen? (Null-Kopie aktiv und
// HONEYCORD_EMPFANG_GPU != 0 — unabhängig davon, ob die Anzeige schon bereit ist)
bool EmpfangErlaubt();
// Benutzt `geraet` das gemeinsame VA-Gerät?
bool GeraetIstGemeinsam(const AVBufferRef* geraet);
// Dekodiertes VA-Bild (gemeinsames Gerät) → eigenes GPU-Bild, oder nullptr.
webrtc::scoped_refptr<webrtc::VideoFrameBuffer> AusDecoder(const AVFrame* hw, int breite, int hoehe);
// GPU-Bild → DMA-BUF-Deskriptoren für die Anzeige. false = kein GPU-Bild.
bool Exportieren(const webrtc::VideoFrameBuffer& puffer, HcGpuBild* aus);

// Zählt ein Bild, das ohne Umweg in den Encoder ging (für die 30-s-Zeile).
void EncoderDirektGezaehlt();

// Eine Bildschirmaufnahme beginnt / endet. Endet die letzte, gibt der Wandler
// seine Bildvorräte und VPP-Kontexte frei.
void AufnahmeBeginnt();
void AufnahmeEndet();
// Leert die Vorräte, wenn keine Aufnahme mehr zählt (nach dem Abbau).
void AufraeumenWennRuhig();

}  // namespace nullkopie
}  // namespace libwebrtc

#endif  // LIBWEBRTC_LINUX_HWENC_FFMPEG_HW_NULLKOPIE_H_
