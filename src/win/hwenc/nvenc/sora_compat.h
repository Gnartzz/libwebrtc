#ifndef LIBWEBRTC_WIN_HWENC_NVENC_SORA_COMPAT_H_
#define LIBWEBRTC_WIN_HWENC_NVENC_SORA_COMPAT_H_

// Minimal compat layer so Shiguredo Momo / sora-cpp-sdk's NvCodec encoder
// builds inside libwebrtc-honeycord WITHOUT pulling in the full sora-cpp-sdk
// (which depends on Sora's CUDA loader, dynamic-module facility, etc.).
//
// On Windows the encoder uses D3D11 only — CudaContext is a passthrough
// parameter that's never dereferenced. We keep the type just to preserve
// the upstream signatures.

#include <memory>

namespace sora {

enum class CudaVideoCodec {
  H264,
  H265,
  AV1,
};

// Opaque placeholder so std::shared_ptr<CudaContext> compiles on Windows.
// Real CUDA bookkeeping lives in the upstream sora::CudaContext that we
// don't link against; the Win path in nvcodec_video_encoder.cpp never
// touches members of this class.
class CudaContext {
 public:
  virtual ~CudaContext() = default;
};

}  // namespace sora

#endif  // LIBWEBRTC_WIN_HWENC_NVENC_SORA_COMPAT_H_
