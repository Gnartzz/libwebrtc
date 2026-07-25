#include "amf_h264_encoder.h"

#include <cstring>
#include <utility>

#include "api/video/i420_buffer.h"
#include "api/video/video_frame_buffer.h"
#include "libyuv/convert_argb.h"
#include "libyuv/convert_from.h"

#include "../../honeycord_d3d11_frame.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"

namespace libwebrtc {

using Microsoft::WRL::ComPtr;

namespace {

constexpr const wchar_t* kAmfH264Encoder = AMFVideoEncoderVCE_AVC;

// Maps WebRTC bitrate adjuster output -> AMF rate-control properties.
// AMF uses an explicit bitrate; we let WebRTC's BitrateAdjuster smooth out
// short-term over/under-shoots before we forward.
constexpr double kBitrateLowSmooth = 0.5;
constexpr double kBitrateHighSmooth = 0.95;

}  // namespace

// SEH-guarded probe: AMF can SEH-crash inside the driver-supplied amfrt64.dll
// (old/mismatched driver, missing kernel feature, etc.). We wrap each AMF
// entry-point in __try/__except so a 0xc0000005 inside AMFInit() never kills
// the app — we fall through to the software encoder transparently.
//
// __try/__except + C++ objects with non-trivial dtors conflict under clang-cl,
// so this helper uses raw AMFFactory/AMFContext/AMFComponent pointers with
// explicit Release(). On SEH exit we leak (one-shot probe, never a hot path).
namespace {
bool DoIsSupportedSEH() {
  __try {
    if (g_AMFFactory.Init() != AMF_OK) return false;
    amf::AMFFactory* factory = g_AMFFactory.GetFactory();
    if (!factory) { g_AMFFactory.Terminate(); return false; }

    amf::AMFContext* ctx = nullptr;
    if (factory->CreateContext(&ctx) != AMF_OK || !ctx) {
      g_AMFFactory.Terminate();
      return false;
    }
    if (ctx->InitDX11(nullptr) != AMF_OK) {
      ctx->Terminate();
      ctx->Release();
      g_AMFFactory.Terminate();
      return false;
    }

    amf::AMFComponent* enc = nullptr;
    AMF_RESULT r = factory->CreateComponent(ctx, kAmfH264Encoder, &enc);
    if (enc) {
      enc->Terminate();
      enc->Release();
    }
    ctx->Terminate();
    ctx->Release();
    g_AMFFactory.Terminate();
    return r == AMF_OK;
  } __except(EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
}  // namespace

// static
bool AmfH264Encoder::IsSupported() {
  bool ok = DoIsSupportedSEH();
  if (!ok) {
    RTC_LOG(LS_WARNING) << "AmfH264Encoder::IsSupported(): AMF probe failed "
                           "(SEH or error result), falling back to software";
  }
  return ok;
}

AmfH264Encoder::AmfH264Encoder()
    : bitrate_adjuster_(kBitrateLowSmooth, kBitrateHighSmooth) {}

AmfH264Encoder::~AmfH264Encoder() {
  Release();
}

int32_t AmfH264Encoder::InitEncode(const webrtc::VideoCodec* codec_settings,
                                    int32_t /*number_of_cores*/,
                                    size_t /*max_payload_size*/) {
  if (!codec_settings) return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  Release();

  width_ = codec_settings->width;
  height_ = codec_settings->height;
  target_bitrate_bps_ = codec_settings->startBitrate * 1000;
  max_bitrate_bps_ = codec_settings->maxBitrate * 1000;
  framerate_ = codec_settings->maxFramerate ? codec_settings->maxFramerate : 30;
  screensharing_mode_ =
      codec_settings->mode == webrtc::VideoCodecMode::kScreensharing;
  bitrate_adjuster_.SetTargetBitrateBps(target_bitrate_bps_);

  return InitAmfPipeline();
}

void AmfH264Encoder::ApplyEncoderProperties(amf::AMFComponent* enc) {
  if (!enc) return;
  enc->SetProperty(AMF_VIDEO_ENCODER_USAGE,
                   AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY);
  enc->SetProperty(AMF_VIDEO_ENCODER_QUALITY_PRESET,
                   AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED);
  enc->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
                   AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR);
  enc->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE,
                   static_cast<amf_int64>(target_bitrate_bps_));
  enc->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE,
                   static_cast<amf_int64>(max_bitrate_bps_));
  enc->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE,
                   ::AMFConstructRate(framerate_, 1));
  enc->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE,
                   ::AMFConstructSize(width_, height_));
  enc->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, static_cast<amf_int64>(0));
  enc->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, static_cast<amf_int64>(0));
  // Output style: in-stream SPS/PPS so the bitstream parser can fish them
  // out of the first slices (same model as Momo's NVENC path).
  enc->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, true);
  enc->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, true);
}

// Stufe 3 (#77): Encoder auf DEM D3D11-Device aufbauen, auf dem der Capturer
// seine Textur haelt. Dann kann der fertige Frame GPU->GPU in die Encoder-
// Oberflaeche wandern, statt per ToI420()-Readback ueber den Hauptspeicher zu
// laufen (2560x720 = 7,4 MB je Bild, GEMESSEN ~8 ms Encoder-Zeit).
//
// Vorsichtsprinzip: die neue Kette wird komplett in lokalen Variablen aufgebaut
// und erst bei vollem Erfolg uebernommen. Nimmt der Treiber BGRA nicht als
// Encoder-Eingabe an (aeltere AMF-Versionen wollen NV12), bleibt der bestehende
// Host-Pfad unveraendert in Betrieb und wir versuchen es nie wieder.
bool AmfH264Encoder::EnsureNativeAmf(ID3D11Device* dev) {
  if (dx11_failed_ || !dev) return false;
  if (dx11_mode_ && d3d11_device_.Get() == dev) return true;

  amf::AMFContextPtr ctx;
  amf::AMFComponentPtr enc;
  amf::AMFFactory* factory = g_AMFFactory.GetFactory();
  if (!factory || factory->CreateContext(&ctx) != AMF_OK || !ctx) {
    RTC_LOG(LS_WARNING) << "AmfH264Encoder: DX11 CreateContext fehlgeschlagen";
    dx11_failed_ = true;
    return false;
  }
  if (ctx->InitDX11(dev) != AMF_OK) {
    RTC_LOG(LS_WARNING) << "AmfH264Encoder: InitDX11(Capturer-Device) fehlgeschlagen";
    ctx->Terminate();
    dx11_failed_ = true;
    return false;
  }
  if (factory->CreateComponent(ctx, kAmfH264Encoder, &enc) != AMF_OK || !enc) {
    RTC_LOG(LS_WARNING) << "AmfH264Encoder: DX11 CreateComponent fehlgeschlagen";
    ctx->Terminate();
    dx11_failed_ = true;
    return false;
  }
  ApplyEncoderProperties(enc);
  if (enc->Init(amf::AMF_SURFACE_BGRA, width_, height_) != AMF_OK) {
    RTC_LOG(LS_WARNING) << "AmfH264Encoder: BGRA-Eingabe nicht unterstuetzt "
                           "-> bleibe beim Host-Pfad";
    enc->Terminate();
    ctx->Terminate();
    dx11_failed_ = true;
    return false;
  }

  // Umschalten: alte Kette abbauen, neue uebernehmen.
  if (amf_encoder_) amf_encoder_->Terminate();
  if (amf_context_) amf_context_->Terminate();
  amf_encoder_ = enc;
  amf_context_ = ctx;
  d3d11_device_ = dev;
  d3d11_context_.Reset();
  dev->GetImmediateContext(&d3d11_context_);
  dx11_mode_ = true;
  force_keyframe_ = true;  // neuer Encoder -> Empfaenger brauchen ein Keyframe
  RTC_LOG(LS_INFO) << "AmfH264Encoder: GPU-Zero-Copy aktiv (BGRA auf dem "
                      "Capturer-Device, kein Readback mehr)";
  return true;
}

int32_t AmfH264Encoder::InitAmfPipeline() {
  if (g_AMFFactory.Init() != AMF_OK) {
    RTC_LOG(LS_ERROR) << "AmfH264Encoder: AMF runtime not loadable";
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }

  // D3D11 device — we let AMF pick the default adapter via InitDX11(nullptr).
  // (Vendor selection happens upstream in HoneycordVideoEncoderFactory.)
  if (g_AMFFactory.GetFactory()->CreateContext(&amf_context_) != AMF_OK) {
    RTC_LOG(LS_ERROR) << "AmfH264Encoder: CreateContext failed";
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }
  if (amf_context_->InitDX11(nullptr) != AMF_OK) {
    RTC_LOG(LS_ERROR) << "AmfH264Encoder: InitDX11 failed";
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }

  if (g_AMFFactory.GetFactory()->CreateComponent(
          amf_context_, kAmfH264Encoder, &amf_encoder_) != AMF_OK) {
    RTC_LOG(LS_ERROR) << "AmfH264Encoder: CreateComponent(VCE_AVC) failed";
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }

  // Real-time / low-latency tuning — analogous to NVENC's P3 + LOW_LATENCY:
  //  * Usage = ULTRA_LOW_LATENCY (transcode mode that disables look-ahead)
  //  * Quality preset = SPEED (prioritize throughput over compression)
  //  * Rate control = CBR (constant bitrate; matches WebRTC's pacer model)
  //  * IDR period = infinite — keyframes are driven by WebRTC RTCP requests
  //  * B-frames off (1 reference frame between keyframes)
  ApplyEncoderProperties(amf_encoder_);

  if (amf_encoder_->Init(amf::AMF_SURFACE_NV12, width_, height_) != AMF_OK) {
    RTC_LOG(LS_ERROR) << "AmfH264Encoder: encoder->Init failed";
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }

  reconfigure_needed_ = false;
  force_keyframe_ = false;
  RTC_LOG(LS_INFO) << "AmfH264Encoder: initialized "
                   << width_ << "x" << height_ << "@" << framerate_
                   << " target=" << target_bitrate_bps_ << "bps";
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t AmfH264Encoder::RegisterEncodeCompleteCallback(
    webrtc::EncodedImageCallback* callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t AmfH264Encoder::Release() {
  ReleaseAmfPipeline();
  return WEBRTC_VIDEO_CODEC_OK;
}

void AmfH264Encoder::ReleaseAmfPipeline() {
  if (amf_encoder_) {
    amf_encoder_->Terminate();
    amf_encoder_ = nullptr;
  }
  if (amf_context_) {
    amf_context_->Terminate();
    amf_context_ = nullptr;
  }
  // Stufe 3: Zustand zuruecksetzen, damit sich der GPU-Pfad nach einem Neu-Init
  // (z.B. Aufloesungswechsel) frisch etablieren kann.
  d3d11_context_.Reset();
  d3d11_device_.Reset();
  dx11_mode_ = false;
  dx11_failed_ = false;
}

int32_t AmfH264Encoder::Encode(
    const webrtc::VideoFrame& frame,
    const std::vector<webrtc::VideoFrameType>* frame_types) {
  if (!amf_encoder_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  if (!callback_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;

  if (frame_types && !frame_types->empty()) {
    if ((*frame_types)[0] == webrtc::VideoFrameType::kEmptyFrame) {
      return WEBRTC_VIDEO_CODEC_OK;
    }
    if ((*frame_types)[0] == webrtc::VideoFrameType::kVideoFrameKey) {
      force_keyframe_ = true;
    }
  }

  // Reconfigure bitrate / framerate if SetRates queued a change.
  if (reconfigure_needed_) {
    amf_encoder_->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE,
                               static_cast<amf_int64>(
                                   bitrate_adjuster_.GetAdjustedBitrateBps()));
    amf_encoder_->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE,
                               static_cast<amf_int64>(max_bitrate_bps_));
    amf_encoder_->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE,
                               ::AMFConstructRate(framerate_, 1));
    reconfigure_needed_ = false;
  }

  // Stufe 3 (#77): Liegt der Frame schon als GPU-Textur vor (unser Bildschirm-
  // Capturer liefert kNative), geht er ohne Umweg ueber den Hauptspeicher in
  // den Encoder. Sonst der bisherige Host-Pfad (Kamera, CPU-Fallback,
  // Adaptation): Frame nach I420 holen und in eine Host-Oberflaeche schreiben.
  honeycord::D3D11FrameBuffer* nb = nullptr;
  if (frame.video_frame_buffer()->type() ==
          webrtc::VideoFrameBuffer::Type::kNative &&
      frame.video_frame_buffer()->width() == static_cast<int>(width_) &&
      frame.video_frame_buffer()->height() == static_cast<int>(height_)) {
    nb = static_cast<honeycord::D3D11FrameBuffer*>(
        frame.video_frame_buffer().get());
  }

  amf::AMFSurfacePtr surface;
  bool gpu_path = false;
  if (nb && nb->texture() && EnsureNativeAmf(nb->device())) {
    if (amf_context_->AllocSurface(amf::AMF_MEMORY_DX11, amf::AMF_SURFACE_BGRA,
                                    width_, height_, &surface) == AMF_OK &&
        surface) {
      amf::AMFPlane* plane = surface->GetPlaneAt(0);
      ID3D11Texture2D* dst =
          plane ? static_cast<ID3D11Texture2D*>(plane->GetNative()) : nullptr;
      if (dst && d3d11_context_) {
        // Bewusst eine Kopie statt eines Direktverweises auf die Capturer-
        // Textur: der Capturer schreibt seinen Ring weiter, waehrend der
        // Encoder liest. Die Kopie bleibt auf der GPU (kein Readback).
        d3d11_context_->CopyResource(dst, nb->texture());
        gpu_path = true;
      }
    }
    if (!gpu_path) {
      surface = nullptr;
      RTC_LOG(LS_WARNING) << "AmfH264Encoder: DX11-Oberflaeche fehlgeschlagen "
                             "-> Host-Pfad fuer diesen Frame";
    }
  }

  if (!gpu_path) {
    // Das Format muss zu dem passen, mit dem der Encoder initialisiert wurde.
    const amf::AMF_SURFACE_FORMAT host_fmt =
        dx11_mode_ ? amf::AMF_SURFACE_BGRA : amf::AMF_SURFACE_NV12;
    if (amf_context_->AllocSurface(amf::AMF_MEMORY_HOST, host_fmt, width_,
                                    height_, &surface) != AMF_OK) {
      RTC_LOG(LS_WARNING) << "AmfH264Encoder: AllocSurface failed";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    webrtc::scoped_refptr<const webrtc::I420BufferInterface> i420 =
        frame.video_frame_buffer()->ToI420();
    if (dx11_mode_) {
      amf::AMFPlane* plane = surface->GetPlaneAt(0);
      libyuv::I420ToARGB(i420->DataY(), i420->StrideY(), i420->DataU(),
                         i420->StrideU(), i420->DataV(), i420->StrideV(),
                         static_cast<uint8_t*>(plane->GetNative()),
                         plane->GetHPitch(), width_, height_);
    } else {
      amf::AMFPlane* y_plane = surface->GetPlaneAt(0);
      amf::AMFPlane* uv_plane = surface->GetPlaneAt(1);
      libyuv::I420ToNV12(
          i420->DataY(), i420->StrideY(), i420->DataU(), i420->StrideU(),
          i420->DataV(), i420->StrideV(),
          static_cast<uint8_t*>(y_plane->GetNative()), y_plane->GetHPitch(),
          static_cast<uint8_t*>(uv_plane->GetNative()), uv_plane->GetHPitch(),
          width_, height_);
    }
  }

  if (force_keyframe_) {
    surface->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE,
                          AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR);
    force_keyframe_ = false;
  }

  // Submit. AMF can be back-pressured: retry briefly on AMF_INPUT_FULL.
  AMF_RESULT submit_res = AMF_OK;
  for (int retry = 0; retry < 50; ++retry) {
    submit_res = amf_encoder_->SubmitInput(surface);
    if (submit_res != AMF_INPUT_FULL) break;
    DrainEncoderTo(force_keyframe_ ? webrtc::VideoFrameType::kVideoFrameKey
                                    : webrtc::VideoFrameType::kVideoFrameDelta,
                    frame.rtp_timestamp(), frame.ntp_time_ms(),
                    frame.render_time_ms(), frame.rotation(),
                    screensharing_mode_);
    ::Sleep(1);
  }
  if (submit_res != AMF_OK && submit_res != AMF_NEED_MORE_INPUT) {
    RTC_LOG(LS_ERROR) << "AmfH264Encoder: SubmitInput res=" << submit_res;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  // Drain whatever's ready right now; remaining packets surface on next call.
  DrainEncoderTo(webrtc::VideoFrameType::kVideoFrameDelta,
                  frame.rtp_timestamp(), frame.ntp_time_ms(),
                  frame.render_time_ms(), frame.rotation(),
                  screensharing_mode_);

  return WEBRTC_VIDEO_CODEC_OK;
}

void AmfH264Encoder::DrainEncoderTo(webrtc::VideoFrameType /*frame_type_hint*/,
                                     uint32_t rtp_timestamp,
                                     int64_t ntp_time_ms,
                                     int64_t render_time_ms,
                                     webrtc::VideoRotation rotation,
                                     bool screensharing) {
  while (true) {
    amf::AMFDataPtr data;
    AMF_RESULT r = amf_encoder_->QueryOutput(&data);
    if (r == AMF_REPEAT || !data) {
      // No output yet; come back on the next Encode().
      break;
    }
    if (r != AMF_OK) {
      RTC_LOG(LS_WARNING) << "AmfH264Encoder: QueryOutput res=" << r;
      break;
    }

    amf::AMFBufferPtr buf(data);
    if (!buf) continue;

    const uint8_t* p = static_cast<const uint8_t*>(buf->GetNative());
    const size_t size = buf->GetSize();
    auto packet = webrtc::EncodedImageBuffer::Create(p, size);

    encoded_image_.SetEncodedData(packet);
    encoded_image_._encodedWidth = width_;
    encoded_image_._encodedHeight = height_;
    encoded_image_.content_type_ =
        screensharing ? webrtc::VideoContentType::SCREENSHARE
                      : webrtc::VideoContentType::UNSPECIFIED;
    encoded_image_.timing_.flags = webrtc::VideoSendTiming::kInvalid;
    encoded_image_.SetRtpTimestamp(rtp_timestamp);
    encoded_image_.ntp_time_ms_ = ntp_time_ms;
    encoded_image_.capture_time_ms_ = render_time_ms;
    encoded_image_.rotation_ = rotation;
    encoded_image_._frameType = webrtc::VideoFrameType::kVideoFrameDelta;

    // Detect keyframe from the AMF picture-type output property.
    amf_int64 amf_pic_type = 0;
    if (data->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE,
                           &amf_pic_type) == AMF_OK) {
      if (amf_pic_type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR ||
          amf_pic_type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_I) {
        encoded_image_._frameType = webrtc::VideoFrameType::kVideoFrameKey;
      }
    }

    h264_parser_.ParseBitstream(encoded_image_);
    encoded_image_.qp_ = h264_parser_.GetLastSliceQp().value_or(-1);

    webrtc::CodecSpecificInfo codec_specific;
    codec_specific.codecType = webrtc::kVideoCodecH264;
    codec_specific.codecSpecific.H264.packetization_mode =
        webrtc::H264PacketizationMode::NonInterleaved;

    auto cb_result = callback_->OnEncodedImage(encoded_image_, &codec_specific);
    if (cb_result.error != webrtc::EncodedImageCallback::Result::OK) {
      RTC_LOG(LS_ERROR) << "AmfH264Encoder: OnEncodedImage error="
                        << cb_result.error;
      break;
    }
    bitrate_adjuster_.Update(size);
  }
}

void AmfH264Encoder::SetRates(
    const webrtc::VideoEncoder::RateControlParameters& parameters) {
  if (!amf_encoder_) return;
  if (parameters.framerate_fps < 1.0) return;
  framerate_ = static_cast<uint32_t>(parameters.framerate_fps);
  target_bitrate_bps_ = parameters.bitrate.get_sum_bps();
  bitrate_adjuster_.SetTargetBitrateBps(target_bitrate_bps_);
  reconfigure_needed_ = true;
}

webrtc::VideoEncoder::EncoderInfo AmfH264Encoder::GetEncoderInfo() const {
  webrtc::VideoEncoder::EncoderInfo info;
  info.supports_native_handle = false;  // we accept I420 in host memory
  info.implementation_name = "AMF-VCE";
  info.is_hardware_accelerated = true;
  info.scaling_settings = webrtc::VideoEncoder::ScalingSettings(28, 38);
  return info;
}

}  // namespace libwebrtc
