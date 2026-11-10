// Empfang ohne Umweg (10.11.2026): die Schnittstelle zum flutter-webrtc-Plugin.
//
// ★ BEWUSST C-FUNKTIONEN statt einer neuen virtuellen Methode an
// `RTCVideoFrame`: Das Plugin baut gegen die Header aus dem Release-Zip; eine
// geänderte vtable bräche jedes Plugin, das gegen die alten Header gebaut ist.
// Das Plugin sucht beide Funktionen per dlsym — fehlen sie (ältere
// Bibliothek), bleibt es beim Pixelweg.
#include "src/rtc_video_frame_impl.h"
#include "src/linux/hwenc/ffmpeg/hw_nullkopie.h"

extern "C" {

// GPU-Bild des Rahmens als DMA-BUF. 1 = ja (Deskriptoren gehören dem
// Aufrufer), 0 = kein GPU-Bild oder nicht exportierbar.
__attribute__((visibility("default"))) int honeycord_gpu_bild(
    void* rahmen, libwebrtc::nullkopie::HcGpuBild* aus) {
  if (rahmen == nullptr || aus == nullptr || aus->version != 1) return 0;
  auto* f = static_cast<libwebrtc::RTCVideoFrame*>(rahmen);
  auto* impl = dynamic_cast<libwebrtc::VideoFrameBufferImpl*>(f);
  if (impl == nullptr || impl->rotation() != libwebrtc::RTCVideoFrame::kVideoRotation_0) return 0;
  webrtc::scoped_refptr<webrtc::VideoFrameBuffer> puffer = impl->buffer();
  if (!puffer) return 0;
  return libwebrtc::nullkopie::Exportieren(*puffer, aus) ? 1 : 0;
}

// Die Anzeige kann GPU-Bilder übernehmen (1) oder nicht (0); -1 = sie ist
// am GPU-Weg gescheitert — dann bleibt er für den Prozess aus, auch wenn
// eine andere Engine später 1 meldet.
__attribute__((visibility("default"))) void honeycord_empfang_gpu_bereit(int an) {
  if (an < 0) libwebrtc::nullkopie::EmpfangSperren("Anzeige meldet Fehlschlag");
  else libwebrtc::nullkopie::EmpfangMelden(an != 0);
}

}  // extern "C"
