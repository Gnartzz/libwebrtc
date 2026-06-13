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
// konvertieren will (Adaptation unter Last / Stats) — dann lieber langsam (ein
// GPU->CPU-Readback) als ein nullptr-Crash.

#ifndef HONEYCORD_D3D11_FRAME_H_
#define HONEYCORD_D3D11_FRAME_H_

#ifdef _WIN32
#include <d3d11.h>
#include <wrl/client.h>

#include "api/scoped_refptr.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame_buffer.h"
#include "api/make_ref_counted.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv.h"

namespace honeycord {

class D3D11FrameBuffer : public webrtc::VideoFrameBuffer {
 public:
  static webrtc::scoped_refptr<D3D11FrameBuffer> Create(
      ID3D11Device* device, ID3D11Texture2D* texture, int width, int height,
      HANDLE shared_handle = nullptr) {
    return webrtc::make_ref_counted<D3D11FrameBuffer>(device, texture, width,
                                                      height, shared_handle);
  }

  D3D11FrameBuffer(ID3D11Device* device, ID3D11Texture2D* texture, int w, int h,
                   HANDLE shared_handle = nullptr)
      : device_(device),
        texture_(texture),
        width_(w),
        height_(h),
        shared_handle_(shared_handle) {}

  Type type() const override { return Type::kNative; }
  int width() const override { return width_; }
  int height() const override { return height_; }

  ID3D11Device* device() const { return device_.Get(); }
  ID3D11Texture2D* texture() const { return texture_.Get(); }

  // DXGI-Legacy-Shared-Handle (IDXGIResource::GetSharedHandle) einer
  // KEYED_MUTEX-Shared-Textur, in die der Capturer pro Frame das fertige
  // BGRA-Bild kopiert. Fuer die GPU-Vorschau: der flutter_webrtc-Renderer gibt
  // diesen Handle an Flutters GpuSurfaceTexture (ANGLE oeffnet ihn cross-device),
  // statt den Frame per ToI420()-Readback auf die CPU zu holen. nullptr, wenn
  // keine Shared-Textur verfuegbar (dann CPU-Fallback). Konstant ueber alle
  // Frames einer Capture-Session.
  HANDLE shared_handle() const { return shared_handle_; }

  // CPU-Fallback (Readback). Wird im Zero-Copy-Pfad NIE gerufen; nur wenn die
  // webrtc-Pipeline den Frame doch konvertieren muss (z.B. Resolution-Adaptation
  // unter Bandbreiten-Last oder Stats). Strategie: BGRA-Textur in eine
  // CPU-lesbare Staging-Textur kopieren, mappen, mit libyuv nach I420 wandeln.
  // Der Immediate-Context steht unter Multithread-Schutz (im Capturer via
  // SetMultithreadProtected(TRUE) gesetzt), daher ist CopyResource/Map auch vom
  // Encoder-/Adaptation-Thread aus sicher.
  webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override {
    using Microsoft::WRL::ComPtr;
    if (!device_ || !texture_) return nullptr;

    D3D11_TEXTURE2D_DESC desc = {};
    texture_->GetDesc(&desc);

    ComPtr<ID3D11DeviceContext> ctx;
    device_->GetImmediateContext(&ctx);
    if (!ctx) return nullptr;

    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device_->CreateTexture2D(&sd, nullptr, &staging))) {
      RTC_LOG(LS_ERROR) << "D3D11FrameBuffer::ToI420: CreateTexture2D(staging) "
                           "fehlgeschlagen";
      return nullptr;
    }
    ctx->CopyResource(staging.Get(), texture_.Get());

    D3D11_MAPPED_SUBRESOURCE map = {};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
      RTC_LOG(LS_ERROR) << "D3D11FrameBuffer::ToI420: Map(staging) "
                           "fehlgeschlagen";
      return nullptr;
    }

    webrtc::scoped_refptr<webrtc::I420Buffer> i420 =
        webrtc::I420Buffer::Create(width_, height_);
    // BGRA (DXGI_FORMAT_B8G8R8A8_UNORM) liegt im Speicher byteweise als
    // B,G,R,A — in libyuv-Nomenklatur "ARGB" (little-endian) — also ARGBToI420.
    libyuv::ARGBToI420(static_cast<const uint8_t*>(map.pData),
                       static_cast<int>(map.RowPitch),
                       i420->MutableDataY(), i420->StrideY(),
                       i420->MutableDataU(), i420->StrideU(),
                       i420->MutableDataV(), i420->StrideV(),
                       width_, height_);

    ctx->Unmap(staging.Get(), 0);
    return i420;
  }

 private:
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
  const int width_;
  const int height_;
  HANDLE shared_handle_ = nullptr;  // gehoert dem Capturer, nicht schliessen
};

}  // namespace honeycord

#endif  // _WIN32
#endif  // HONEYCORD_D3D11_FRAME_H_
