#include "src/linux/hwenc/hw_decoder_factory.h"

#include "absl/strings/match.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/video_decoder_software_fallback_wrapper.h"
#include "media/base/media_constants.h"
#include "rtc_base/logging.h"
#include "src/linux/hwenc/ffmpeg/hw_h264_decoder.h"

namespace libwebrtc {

HwVideoDecoderFactory::HwVideoDecoderFactory()
    : intern_(webrtc::CreateBuiltinVideoDecoderFactory()) {}

std::vector<webrtc::SdpVideoFormat> HwVideoDecoderFactory::GetSupportedFormats()
    const {
  return intern_->GetSupportedFormats();
}

webrtc::VideoDecoderFactory::CodecSupport HwVideoDecoderFactory::QueryCodecSupport(
    const webrtc::SdpVideoFormat& format, bool reference_scaling) const {
  return intern_->QueryCodecSupport(format, reference_scaling);
}

std::unique_ptr<webrtc::VideoDecoder> HwVideoDecoderFactory::Create(
    const webrtc::Environment& env, const webrtc::SdpVideoFormat& format) {
  std::unique_ptr<webrtc::VideoDecoder> software = intern_->Create(env, format);
  if (!software) return nullptr;
  if (!absl::EqualsIgnoreCase(format.name, webrtc::kH264CodecName) ||
      !HwH264Decoder::IstVerfuegbar()) {
    return software;
  }
  // ★ Reihenfolge der Argumente: erst der Rückfall, dann die Hardware.
  return webrtc::CreateVideoDecoderSoftwareFallbackWrapper(
      env, std::move(software), std::make_unique<HwH264Decoder>());
}

std::unique_ptr<webrtc::VideoDecoderFactory> CreateLinuxVideoDecoderFactory() {
  // Die Probe beim Aufbau, nicht beim ersten Anruf — wie beim Encoder steht
  // so VOR dem ersten Gespräch im hwenc.log, ob dekodiert wird und womit.
  const bool hw = HwH264Decoder::IstVerfuegbar();
  RTC_LOG(LS_INFO) << "[hwenc] Video-Decoder H.264: "
                   << (hw ? "VA-API mit Software-Rueckfall" : "Software");
  return std::make_unique<HwVideoDecoderFactory>();
}

}  // namespace libwebrtc
