#include "src/win/hwdec/honeycord_video_decoder_factory.h"

#include "absl/strings/match.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "rtc_base/logging.h"

#ifdef _WIN32
#include "src/win/hwdec/honeycord_d3d11va_h264_decoder.h"
#endif

namespace libwebrtc {

HoneycordVideoDecoderFactory::HoneycordVideoDecoderFactory()
    : builtin_factory_(webrtc::CreateBuiltinVideoDecoderFactory()) {
#ifdef _WIN32
  hw_h264_ = D3D11VAH264Decoder::IsSupported();
#endif
  RTC_LOG(LS_INFO) << "[hwdec] HoneycordVideoDecoderFactory hw_h264=" << hw_h264_;
}

HoneycordVideoDecoderFactory::~HoneycordVideoDecoderFactory() = default;

std::vector<webrtc::SdpVideoFormat>
HoneycordVideoDecoderFactory::GetSupportedFormats() const {
  // Kein neues Codec-Set; H.264 wird nur HW-beschleunigt, falls verfuegbar.
  // Unterstuetzte Formate = die des Builtins (H264/VP8/VP9/AV1).
  return builtin_factory_->GetSupportedFormats();
}

std::unique_ptr<webrtc::VideoDecoder> HoneycordVideoDecoderFactory::Create(
    const webrtc::Environment& env, const webrtc::SdpVideoFormat& format) {
#ifdef _WIN32
  if (hw_h264_ && absl::EqualsIgnoreCase(format.name, "H264")) {
    RTC_LOG(LS_INFO) << "[hwdec] using D3D11VA H264 hardware decoder";
    return std::make_unique<D3D11VAH264Decoder>();
  }
#endif
  return builtin_factory_->Create(env, format);
}

}  // namespace libwebrtc
