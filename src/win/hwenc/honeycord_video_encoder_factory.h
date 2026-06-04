#ifndef LIBWEBRTC_WIN_HWENC_HONEYCORD_VIDEO_ENCODER_FACTORY_H_
#define LIBWEBRTC_WIN_HWENC_HONEYCORD_VIDEO_ENCODER_FACTORY_H_

// HoneyCord multi-vendor encoder factory.
//
// Picks the encoder backend at runtime based on the local GPU vendor:
//   NVIDIA  -> Momo-ported NvCodecVideoEncoder (NVENC via D3D11)
//   AMD/Intel/unknown -> WebRTC's builtin software encoders (OpenH264 / VP8 / VP9)
//
// Constructed once at PeerConnectionFactory init; vendor detection runs once
// (DXGI EnumAdapters[0].GetDesc) and is cached for every Create() call.

#include <memory>
#include <vector>

#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"

namespace libwebrtc {

class HoneycordVideoEncoderFactory : public webrtc::VideoEncoderFactory {
 public:
  HoneycordVideoEncoderFactory();
  ~HoneycordVideoEncoderFactory() override;

  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;

  std::unique_ptr<webrtc::VideoEncoder> Create(
      const webrtc::Environment& env,
      const webrtc::SdpVideoFormat& format) override;

  // For logging / Sidebar-Footer-Variant display.
  enum class Backend { kNvenc, kAmf, kBuiltin };
  Backend selected_backend() const { return backend_; }
  const char* selected_backend_name() const;

 private:
  std::unique_ptr<webrtc::VideoEncoderFactory> builtin_factory_;
  Backend backend_ = Backend::kBuiltin;
  uint32_t gpu_vendor_id_ = 0;
};

}  // namespace libwebrtc

#endif  // LIBWEBRTC_WIN_HWENC_HONEYCORD_VIDEO_ENCODER_FACTORY_H_
