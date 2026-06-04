// Adapted from Shiguredo Momo / sora-cpp-sdk (Apache License 2.0).
// Original: sora-cpp-sdk/include/sora/hwenc_nvcodec/nvcodec_video_encoder.h
#ifndef SORA_HWENC_NVCODEC_NVCODEC_VIDEO_ENCODER_H_
#define SORA_HWENC_NVCODEC_NVCODEC_VIDEO_ENCODER_H_

#include <memory>

// WebRTC
#include <api/video_codecs/video_encoder.h>

#include "sora_compat.h"

namespace sora {

class NvCodecVideoEncoder : public webrtc::VideoEncoder {
 public:
  static bool IsSupported(std::shared_ptr<CudaContext> cuda_context,
                          CudaVideoCodec codec);
  static std::unique_ptr<NvCodecVideoEncoder> Create(
      std::shared_ptr<CudaContext> cuda_context,
      CudaVideoCodec codec);
};

}  // namespace sora

#endif
