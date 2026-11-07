#ifndef LIBWEBRTC_LINUX_HWENC_HW_DECODER_FACTORY_H_
#define LIBWEBRTC_LINUX_HWENC_HW_DECODER_FACTORY_H_

// Die Decoder-Fabrik für Linux (07.11.2026): H.264 über VA-API, alles andere
// wie bisher.
//
// ★ EINE HÜLLE, KEIN ERSATZ — dieselbe Regel wie bei der Encoder-Fabrik. Sie
// meldet exakt die Formate der eingebauten Fabrik; an der Aushandlung ändert
// sich nichts. Für H.264 legt sie den Rückfall-Wrapper von WebRTC um zwei
// Decoder: vorn VA-API, dahinter der eingebaute Software-Decoder. Scheitert
// VA-API beim Öffnen, bei einem Strom, den der Treiber nicht kann, oder
// wiederholt beim Dekodieren, übernimmt Software — für diese Spur bis zu
// ihrem Ende.

#include <memory>
#include <vector>

#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_decoder_factory.h"

namespace libwebrtc {

class HwVideoDecoderFactory : public webrtc::VideoDecoderFactory {
 public:
  HwVideoDecoderFactory();
  ~HwVideoDecoderFactory() override = default;

  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;
  CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat& format,
                                 bool reference_scaling) const override;
  std::unique_ptr<webrtc::VideoDecoder> Create(
      const webrtc::Environment& env,
      const webrtc::SdpVideoFormat& format) override;

 private:
  std::unique_ptr<webrtc::VideoDecoderFactory> intern_;
};

// Liefert die Linux-Decoder-Fabrik. Ohne VA-API verhält sie sich exakt wie
// die eingebaute.
std::unique_ptr<webrtc::VideoDecoderFactory> CreateLinuxVideoDecoderFactory();

}  // namespace libwebrtc

#endif  // LIBWEBRTC_LINUX_HWENC_HW_DECODER_FACTORY_H_
