// honeycord_d3d11_frame.h — Zero-Copy-Frame-Traeger fuer den GPU-Bildschirm-Pfad.
//
// Ein kNative-VideoFrameBuffer, der eine FERTIGE (bereits auf Zielaufloesung
// herunterskalierte) D3D11-BGRA-Textur + ihr Device traegt. Der Bildschirm-
// Capturer (rtc_desktop_capturer_impl) erzeugt das; der NVENC-Encoder
// (nvcodec_video_encoder) erkennt type()==kNative, holt device()+texture() und
// CopyResource't direkt in den NVENC-Input — gleiches Device => echtes
// Zero-Copy (im Spike 0,64 ms/Frame vs ~15-20 ms CPU).
//
// ToI420() ist der CPU-Fallback (Readback): wird im Zero-Copy-Pfad NIE gerufen,
// existiert nur als Sicherheitsnetz, falls die webrtc-Pipeline doch mal
// konvertieren will (Adaptation/Stats) — dann lieber langsam als Crash.

#ifndef HONEYCORD_D3D11_FRAME_H_
#define HONEYCORD_D3D11_FRAME_H_

#ifdef _WIN32
#include <d3d11.h>
#include <wrl/client.h>

#include "api/scoped_refptr.h"
#include "api/video/video_frame_buffer.h"
#include "api/make_ref_counted.h"
#include "rtc_base/logging.h"

namespace honeycord {

class D3D11FrameBuffer : public webrtc::VideoFrameBuffer {
 public:
  static webrtc::scoped_refptr<D3D11FrameBuffer> Create(ID3D11Device* device,
                                                        ID3D11Texture2D* texture,
                                                        int width, int height) {
    return webrtc::make_ref_counted<D3D11FrameBuffer>(device, texture, width, height);
  }

  D3D11FrameBuffer(ID3D11Device* device, ID3D11Texture2D* texture, int w, int h)
      : device_(device), texture_(texture), width_(w), height_(h) {}

  Type type() const override { return Type::kNative; }
  int width() const override { return width_; }
  int height() const override { return height_; }

  ID3D11Device* device() const { return device_.Get(); }
  ID3D11Texture2D* texture() const { return texture_.Get(); }

  // CPU-Fallback (noch nicht implementiert — im Zero-Copy-Pfad nie gerufen).
  // TODO Phase 2: echtes Readback (Map staging + libyuv ARGB->I420), damit
  // Adaptation/Stats nicht crasht. Bis dahin: laut loggen + nullptr.
  webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override {
    RTC_LOG(LS_ERROR) << "honeycord::D3D11FrameBuffer::ToI420() gerufen — "
                         "Zero-Copy-Pfad sollte das NIE; Pipeline konvertiert?";
    return nullptr;
  }

 private:
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
  const int width_;
  const int height_;
};

}  // namespace honeycord

#endif  // _WIN32
#endif  // HONEYCORD_D3D11_FRAME_H_
