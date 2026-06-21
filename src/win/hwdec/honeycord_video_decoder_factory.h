#ifndef LIBWEBRTC_WIN_HWDEC_HONEYCORD_VIDEO_DECODER_FACTORY_H_
#define LIBWEBRTC_WIN_HWDEC_HONEYCORD_VIDEO_DECODER_FACTORY_H_

// HoneyCord HW-Decoder-Factory.
//
// H.264 wird ueber den vendor-neutralen D3D11VA/Media-Foundation-HW-Decoder
// dekodiert (DXVA -> GPU-Decode-Einheit von NVIDIA/AMD/Intel), Ausgabe als
// honeycord::D3D11FrameBuffer (Zero-Copy zum Renderer). Ist HW-Decode auf der
// Maschine nicht verfuegbar (Probe schlaegt fehl) ODER ist der Codec nicht
// H.264, faellt die Factory transparent auf WebRTCs Builtin-SW-Decoder zurueck.
//
// Einmal bei PeerConnectionFactory-Init konstruiert; die HW-Verfuegbarkeit wird
// einmalig geprobt und fuer jeden Create()-Aufruf gecacht (analog zur
// HoneycordVideoEncoderFactory).

#include <memory>
#include <vector>

#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_decoder_factory.h"

namespace libwebrtc {

class HoneycordVideoDecoderFactory : public webrtc::VideoDecoderFactory {
 public:
  HoneycordVideoDecoderFactory();
  ~HoneycordVideoDecoderFactory() override;

  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;

  std::unique_ptr<webrtc::VideoDecoder> Create(
      const webrtc::Environment& env,
      const webrtc::SdpVideoFormat& format) override;

 private:
  std::unique_ptr<webrtc::VideoDecoderFactory> builtin_factory_;
  bool hw_h264_ = false;  // D3D11VA-H.264-HW-Decode auf dieser Maschine moeglich?
};

}  // namespace libwebrtc

#endif  // LIBWEBRTC_WIN_HWDEC_HONEYCORD_VIDEO_DECODER_FACTORY_H_
