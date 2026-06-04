#ifndef LIBWEBRTC_WIN_HWENC_AMF_H264_ENCODER_H_
#define LIBWEBRTC_WIN_HWENC_AMF_H264_ENCODER_H_

// HoneyCord H.264 encoder built on AMD's Advanced Media Framework (AMF).
//
// Targets Radeon RX 5000+ (RDNA, VCN), RX 7000+ (RDNA3, VCN 4.0 — also the
// Phoenix-APU iGPUs like Radeon 780M) and older Polaris/Vega/RX 400 (VCE 4.x).
// The encoder block is a dedicated ASIC — iGPU vs dGPU does not affect
// throughput.
//
// Loads amfrt64.dll at runtime from the AMD driver (analogous to how Momo's
// NVENC encoder loads nvEncodeAPI64.dll). No link-time dependency on the
// AMF SDK runtime library.

#include <memory>
#include <mutex>

#include <d3d11.h>
#include <wrl.h>

#include "amf_sdk/common/AMFFactory.h"
#include "amf_sdk/include/components/VideoEncoderVCE.h"
#include "amf_sdk/include/core/Context.h"
#include "amf_sdk/include/core/Surface.h"
#include "api/video_codecs/video_encoder.h"
#include "common_video/h264/h264_bitstream_parser.h"
#include "common_video/include/bitrate_adjuster.h"

namespace libwebrtc {

class AmfH264Encoder : public webrtc::VideoEncoder {
 public:
  // Returns true if AMF is loadable AND the local AMD driver exposes a
  // working H.264 VCE/VCN encoder (does a tiny probe-init / terminate).
  static bool IsSupported();

  AmfH264Encoder();
  ~AmfH264Encoder() override;

  int32_t InitEncode(const webrtc::VideoCodec* codec_settings,
                     int32_t number_of_cores,
                     size_t max_payload_size) override;
  int32_t RegisterEncodeCompleteCallback(
      webrtc::EncodedImageCallback* callback) override;
  int32_t Release() override;
  int32_t Encode(
      const webrtc::VideoFrame& frame,
      const std::vector<webrtc::VideoFrameType>* frame_types) override;
  void SetRates(
      const webrtc::VideoEncoder::RateControlParameters& parameters) override;
  webrtc::VideoEncoder::EncoderInfo GetEncoderInfo() const override;

 private:
  int32_t InitAmfPipeline();
  void ReleaseAmfPipeline();
  void DrainEncoderTo(webrtc::VideoFrameType frame_type_hint,
                      uint32_t rtp_timestamp,
                      int64_t ntp_time_ms,
                      int64_t render_time_ms,
                      webrtc::VideoRotation rotation,
                      bool screensharing);

  std::mutex mutex_;
  webrtc::EncodedImageCallback* callback_ = nullptr;
  webrtc::H264BitstreamParser h264_parser_;
  webrtc::BitrateAdjuster bitrate_adjuster_;
  webrtc::EncodedImage encoded_image_;
  uint32_t target_bitrate_bps_ = 0;
  uint32_t max_bitrate_bps_ = 0;
  uint32_t framerate_ = 30;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  bool reconfigure_needed_ = false;
  bool force_keyframe_ = false;
  bool screensharing_mode_ = false;

  Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d11_context_;
  amf::AMFContextPtr amf_context_;
  amf::AMFComponentPtr amf_encoder_;
};

}  // namespace libwebrtc

#endif  // LIBWEBRTC_WIN_HWENC_AMF_H264_ENCODER_H_
