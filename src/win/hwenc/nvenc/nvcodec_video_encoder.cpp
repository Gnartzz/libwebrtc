// Adapted from Shiguredo Momo / sora-cpp-sdk (Apache License 2.0).
//   https://github.com/shiguredo/momo
//   https://github.com/shiguredo/sora-cpp-sdk
// Original location: sora-cpp-sdk/src/hwenc_nvcodec/nvcodec_video_encoder.cpp
// Changes for libwebrtc-honeycord:
//   - includes rewired to local nv_codec_sdk/ vendor copy + sora_compat.h stub
//   - linux-only sora::dyn module references removed
// removed: sora/fix_cuda_noinline_macro_error.h (linux/cuda workaround)

#include "nvcodec_video_encoder.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <d3d11.h>
#include <wrl.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdarg>
#endif

// WebRTC
#include <api/scoped_refptr.h>
#include <api/video/encoded_image.h>
#include <api/video/nv12_buffer.h>
#include <api/video/render_resolution.h>
#include <api/video/video_codec_type.h>
#include <api/video/video_content_type.h>
#include <api/video/video_frame.h>
#include <api/video/video_frame_buffer.h>
#include <api/video/video_frame_type.h>
#include <api/video/video_timing.h>
#include <api/video_codecs/scalability_mode.h>
#include <api/video_codecs/video_codec.h>
#include <api/video_codecs/video_encoder.h>
#include <common_video/h264/h264_bitstream_parser.h>
#include <common_video/h265/h265_bitstream_parser.h>
#include <common_video/include/bitrate_adjuster.h>
#include <modules/video_coding/codecs/h264/include/h264_globals.h>
#include <modules/video_coding/include/video_codec_interface.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <modules/video_coding/svc/create_scalability_structure.h>
#include <modules/video_coding/svc/scalable_video_controller.h>
#include <rtc_base/checks.h>
#include <rtc_base/logging.h>

// libyuv
#include <libyuv/convert_from.h>      // IWYU pragma: keep
#include <libyuv/planar_functions.h>  // IWYU pragma: keep

// NvCodec
#include "nv_codec_sdk/NvEncoder.h"
#include "nv_codec_sdk/nvEncodeAPI.h"

#ifdef _WIN32
#include "nv_codec_sdk/NvEncoderD3D11.h"
#include "../../honeycord_d3d11_frame.h"
#endif

#include "sora_compat.h"
// removed: sora/dyn/dyn.h (linux dynamic-loader)

#ifdef __linux__
#include "nvcodec_video_encoder_cuda.h"
#endif

#ifdef __linux__
// removed: sora/dyn/cuda.h (linux only)
// removed: sora/dyn/nvcuvid.h (linux only)
#endif

namespace sora {

const int kLowH264QpThreshold = 34;
const int kHighH264QpThreshold = 40;

#ifdef _WIN32
// HoneyCord diagnostic logger — RTC_LOG on Windows doesn't reach our
// app_log.dart (which is macOS-only), so we ALSO append the probe trace
// to %LOCALAPPDATA%\HoneyCord\nvenc-probe.log. User can pull that file
// after a failed start to see exactly where IsSupported / CreateEncoder
// died.
static void NvProbeLog(const char* fmt, ...) {
  wchar_t localAppData[MAX_PATH] = {0};
  if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localAppData))) {
    return;
  }
  wchar_t dir[MAX_PATH] = {0};
  swprintf_s(dir, MAX_PATH, L"%s\\HoneyCord", localAppData);
  CreateDirectoryW(dir, NULL);  // ignore EEXIST
  wchar_t file[MAX_PATH] = {0};
  swprintf_s(file, MAX_PATH, L"%s\\nvenc-probe.log", dir);
  FILE* f = nullptr;
  if (_wfopen_s(&f, file, L"a") != 0 || !f) return;
  SYSTEMTIME st;
  GetLocalTime(&st);
  fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond,
          st.wMilliseconds);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fputc('\n', f);
  fclose(f);
}
#else
static void NvProbeLog(const char* /*fmt*/, ...) {}
#endif

struct nal_entry {
  size_t offset;
  size_t size;
};

// Local compat — the vendored NvEncoder.h doesn't expose NvEncOutputFrame
// (newer SDK versions split frame + pictureType; ours returns raw bytes).
// We bridge by wrapping each encoded packet here. pictureType stays UNKNOWN —
// upstream uses it only to flag I/IDR frames, which we instead detect via
// `send_key_frame` (gopLength is set to NVENC_INFINITE_GOPLENGTH so the
// encoder never produces an IDR on its own).
struct NvEncOutputFrame {
  std::vector<uint8_t> frame;
  NV_ENC_PIC_TYPE pictureType = NV_ENC_PIC_TYPE_UNKNOWN;
};

#ifdef _WIN32
using Microsoft::WRL::ComPtr;
#endif

class NvCodecVideoEncoderImpl : public NvCodecVideoEncoder {
 public:
  NvCodecVideoEncoderImpl(std::shared_ptr<CudaContext> cuda_context,
                          CudaVideoCodec codec);
  ~NvCodecVideoEncoderImpl() override;

  static bool IsSupported(std::shared_ptr<CudaContext> cuda_context,
                          CudaVideoCodec codec);

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

  static std::unique_ptr<NvEncoder> CreateEncoder(
      CudaVideoCodec codec,
      int width,
      int height,
      int framerate,
      int target_bitrate_bps,
      int max_bitrate_bps
#ifdef _WIN32
      ,
      ID3D11Device* id3d11_device,
      ID3D11Texture2D** out_id3d11_texture
#endif
#ifdef __linux__
      ,
      NvCodecVideoEncoderCuda* cuda,
      bool is_nv12
#endif
  );

 private:
  std::mutex mutex_;
  webrtc::EncodedImageCallback* callback_ = nullptr;
  webrtc::BitrateAdjuster bitrate_adjuster_;
  uint32_t target_bitrate_bps_ = 0;
  uint32_t max_bitrate_bps_ = 0;

  int32_t InitNvEnc();
  int32_t ReleaseNvEnc();
  webrtc::H264BitstreamParser h264_bitstream_parser_;
  webrtc::H265BitstreamParser h265_bitstream_parser_;

  std::shared_ptr<CudaContext> cuda_context_;
  CudaVideoCodec codec_;
  std::unique_ptr<NvEncoder> nv_encoder_;
#ifdef _WIN32
  Microsoft::WRL::ComPtr<ID3D11Device> id3d11_device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> id3d11_context_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> id3d11_texture_;
  // Zero-Copy: eigener NVENC-Encoder (ARGB) auf dem Device des kNative-Frames.
  std::unique_ptr<NvEncoder> nv_encoder_native_;
  Microsoft::WRL::ComPtr<ID3D11Device> native_device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> native_context_;
  bool EnsureNativeEncoder(ID3D11Device* device);
#endif
#ifdef __linux__
  std::unique_ptr<NvCodecVideoEncoderCuda> cuda_;
  bool is_nv12_ = false;
#endif
  bool reconfigure_needed_ = false;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t framerate_ = 0;
  webrtc::VideoCodecMode mode_ = webrtc::VideoCodecMode::kRealtimeVideo;
  NV_ENC_INITIALIZE_PARAMS initialize_params_;
  std::vector<NvEncOutputFrame> v_packet_;
  webrtc::EncodedImage encoded_image_;

  // AV1 用
  std::unique_ptr<webrtc::ScalableVideoController> svc_controller_;
  webrtc::ScalabilityMode scalability_mode_;
};

NvCodecVideoEncoderImpl::NvCodecVideoEncoderImpl(
    std::shared_ptr<CudaContext> cuda_context,
    CudaVideoCodec codec)
    : cuda_context_(cuda_context), codec_(codec), bitrate_adjuster_(0.5, 0.95) {
#ifdef _WIN32
  // HoneyCord: enumerate adapters and pick the NVIDIA one explicitly. Used
  // to be EnumAdapters(0) which breaks on hybrid systems (Optimus / iGPU +
  // dGPU) AND on machines where Windows lists a "Microsoft Basic Display
  // Adapter" as adapter[0] (no NVENC there). Also: use return-on-error
  // instead of RTC_CHECK so a failed init doesn't tear down the whole DLL.
  ComPtr<IDXGIFactory1> idxgi_factory;
  HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                   (void**)idxgi_factory.GetAddressOf());
  if (FAILED(hr)) {
    NvProbeLog("Impl ctor: CreateDXGIFactory1 failed hr=0x%08lx", (long)hr);
    return;
  }
  ComPtr<IDXGIAdapter1> idxgi_adapter1;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> a;
    if (FAILED(idxgi_factory->EnumAdapters1(i, a.GetAddressOf()))) break;
    DXGI_ADAPTER_DESC1 d{};
    a->GetDesc1(&d);
    if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
    if (d.VendorId == 0x10DE) { idxgi_adapter1 = a; break; }
  }
  if (!idxgi_adapter1) {
    NvProbeLog("Impl ctor: no NVIDIA adapter found, encoder will be unusable");
    return;
  }
  ComPtr<IDXGIAdapter> idxgi_adapter;
  idxgi_adapter1.As(&idxgi_adapter);

  hr = D3D11CreateDevice(idxgi_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
                          NULL, 0, D3D11_SDK_VERSION,
                          id3d11_device_.GetAddressOf(), NULL,
                          id3d11_context_.GetAddressOf());
  if (FAILED(hr)) {
    NvProbeLog("Impl ctor: D3D11CreateDevice failed hr=0x%08lx", (long)hr);
    id3d11_device_.Reset();
    return;
  }

  DXGI_ADAPTER_DESC adapter_desc;
  idxgi_adapter->GetDesc(&adapter_desc);
  char szDesc[80];
  size_t result = 0;
  wcstombs_s(&result, szDesc, adapter_desc.Description, sizeof(szDesc));
  RTC_LOG(LS_INFO) << "NvCodec Impl ctor: GPU in use: " << szDesc;
  NvProbeLog("Impl ctor: GPU in use: %s", szDesc);
#endif
#ifdef __linux__
  cuda_.reset(new NvCodecVideoEncoderCuda(cuda_context_));
#endif
}

NvCodecVideoEncoderImpl::~NvCodecVideoEncoderImpl() {}

int32_t NvCodecVideoEncoderImpl::InitEncode(
    const webrtc::VideoCodec* codec_settings,
    int32_t number_of_cores,
    size_t max_payload_size) {
  RTC_DCHECK(codec_settings);
  NvProbeLog("InitEncode entry: w=%u h=%u maxFR=%u startBR=%u maxBR=%u mode=%d",
             codec_settings ? codec_settings->width : 0,
             codec_settings ? codec_settings->height : 0,
             codec_settings ? codec_settings->maxFramerate : 0,
             codec_settings ? codec_settings->startBitrate : 0,
             codec_settings ? codec_settings->maxBitrate : 0,
             codec_settings ? (int)codec_settings->mode : -1);

  int32_t release_ret = Release();
  if (release_ret != WEBRTC_VIDEO_CODEC_OK) {
    NvProbeLog("InitEncode: Release returned %d, aborting", release_ret);
    return release_ret;
  }

  width_ = codec_settings->width;
  height_ = codec_settings->height;
  target_bitrate_bps_ = codec_settings->startBitrate * 1000;
  max_bitrate_bps_ = codec_settings->maxBitrate * 1000;
  bitrate_adjuster_.SetTargetBitrateBps(target_bitrate_bps_);
  framerate_ = codec_settings->maxFramerate;
  mode_ = codec_settings->mode;

  RTC_LOG(LS_INFO) << "InitEncode " << target_bitrate_bps_ << "bit/sec";

  if (codec_settings->codecType == webrtc::kVideoCodecAV1) {
    auto scalability_mode = codec_settings->GetScalabilityMode();
    if (!scalability_mode) {
      RTC_LOG(LS_WARNING) << "Scalability mode is not set, using 'L1T1'.";
      scalability_mode = webrtc::ScalabilityMode::kL1T1;
    }
    RTC_LOG(LS_INFO) << "InitEncode scalability_mode:"
                     << (int)*scalability_mode;
    svc_controller_ = webrtc::CreateScalabilityStructure(*scalability_mode);
    scalability_mode_ = *scalability_mode;
  }

  int32_t init_ret = InitNvEnc();
  NvProbeLog("InitEncode: InitNvEnc returned %d", init_ret);
  return init_ret;
}

int32_t NvCodecVideoEncoderImpl::RegisterEncodeCompleteCallback(
    webrtc::EncodedImageCallback* callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  callback_ = callback;
  NvProbeLog("RegisterEncodeCompleteCallback: callback=%p", (void*)callback);
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvCodecVideoEncoderImpl::Release() {
  return ReleaseNvEnc();
}

#ifdef _WIN32
// Zero-Copy: lazy NVENC-Encoder (ARGB) auf dem D3D11-Device des Capturer-Frames
// erzeugen, damit Capture-Textur und NVENC-Input dasselbe Device teilen ->
// CopyResource ohne CPU (Spike: 0,64 ms/Frame). Neu erzeugen, wenn Device wechselt.
bool NvCodecVideoEncoderImpl::EnsureNativeEncoder(ID3D11Device* device) {
  if (nv_encoder_native_ && native_device_.Get() == device) return true;
  nv_encoder_native_.reset();
  native_device_ = device;
  device->GetImmediateContext(native_context_.ReleaseAndGetAddressOf());
  try {
    auto enc = std::make_unique<NvEncoderD3D11>(device, width_, height_,
                                                NV_ENC_BUFFER_FORMAT_ARGB);
    NV_ENC_INITIALIZE_PARAMS ip = {NV_ENC_INITIALIZE_PARAMS_VER};
    NV_ENC_CONFIG cfg = {NV_ENC_CONFIG_VER};
    ip.encodeConfig = &cfg;
    enc->CreateDefaultEncoderParams(&ip, NV_ENC_CODEC_H264_GUID,
                                    NV_ENC_PRESET_P3_GUID);
    ip.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
    ip.enableEncodeAsync = 0;
    ip.frameRateNum = 60;
    ip.frameRateDen = 1;
    cfg.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
    cfg.gopLength = NVENC_INFINITE_GOPLENGTH;
    cfg.frameIntervalP = 1;
    cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    cfg.rcParams.averageBitRate = target_bitrate_bps_;
    cfg.rcParams.maxBitRate = max_bitrate_bps_;
    cfg.rcParams.vbvBufferSize = target_bitrate_bps_;
    cfg.encodeCodecConfig.h264Config.chromaFormatIDC = 1;
    cfg.encodeCodecConfig.h264Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    cfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    enc->CreateEncoder(&ip);
    nv_encoder_native_ = std::move(enc);
    NvProbeLog("EnsureNativeEncoder: NVENC(ARGB) auf Frame-Device ok %ux%u",
               width_, height_);
    return true;
  } catch (const NVENCException& e) {
    NvProbeLog("EnsureNativeEncoder THREW: %s", e.what());
    return false;
  }
}
#endif

int32_t NvCodecVideoEncoderImpl::Encode(
    const webrtc::VideoFrame& frame,
    const std::vector<webrtc::VideoFrameType>* frame_types) {
  // Log every 60th entry to confirm Encode is even being called.
  static int s_encode_entry_counter = 0;
  if ((s_encode_entry_counter++ % 60) == 0) {
    NvProbeLog("Encode entry #%d: nv_encoder=%p callback=%p w=%u h=%u "
               "buffer_type=%d frame_types=%s",
               s_encode_entry_counter, (void*)nv_encoder_.get(),
               (void*)callback_, width_, height_,
               (int)frame.video_frame_buffer()->type(),
               frame_types ? "yes" : "null");
  }
  if (!nv_encoder_) {
    NvProbeLog("Encode: nv_encoder is NULL -> UNINITIALIZED");
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (!callback_) {
    NvProbeLog("Encode: callback_ is NULL -> UNINITIALIZED (callback was never registered)");
    RTC_LOG(LS_WARNING)
        << "InitEncode() has been called, but a callback function "
        << "has not been set with RegisterEncodeCompleteCallback()";
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }

#if defined(__linux__)
  if (frame.video_frame_buffer()->type() ==
      webrtc::VideoFrameBuffer::Type::kNV12) {
    if (!is_nv12_) {
      ReleaseNvEnc();
      is_nv12_ = true;
      InitNvEnc();
    }
  } else {
    if (is_nv12_) {
      ReleaseNvEnc();
      is_nv12_ = false;
      InitNvEnc();
    }
  }
#endif

  bool send_key_frame = false;

#ifdef _WIN32
  // Zero-Copy: kNative-Frame (D3D11-Textur vom Capturer) -> eigener NVENC-
  // Encoder auf dem Frame-Device. Sonst der normale CPU-I420-Pfad.
  const bool is_native = frame.video_frame_buffer()->type() ==
                         webrtc::VideoFrameBuffer::Type::kNative;
  honeycord::D3D11FrameBuffer* nb = nullptr;
  NvEncoder* active_enc = nv_encoder_.get();
  if (is_native) {
    nb = static_cast<honeycord::D3D11FrameBuffer*>(
        frame.video_frame_buffer().get());
    if (!EnsureNativeEncoder(nb->device())) return WEBRTC_VIDEO_CODEC_ERROR;
    active_enc = nv_encoder_native_.get();
  }
#else
  const bool is_native = false;
  NvEncoder* active_enc = nv_encoder_.get();
#endif

  if (!is_native && reconfigure_needed_) {
    NV_ENC_RECONFIGURE_PARAMS reconfigure_params = {
        NV_ENC_RECONFIGURE_PARAMS_VER};
    NV_ENC_CONFIG encode_config = {NV_ENC_CONFIG_VER};
    reconfigure_params.reInitEncodeParams.encodeConfig = &encode_config;
    nv_encoder_->GetInitializeParams(&reconfigure_params.reInitEncodeParams);

    reconfigure_params.reInitEncodeParams.frameRateNum = framerate_;

    encode_config.rcParams.averageBitRate =
        bitrate_adjuster_.GetAdjustedBitrateBps();
    encode_config.rcParams.maxBitRate = max_bitrate_bps_;
    // VBV buffer = 1 second of target bitrate. MUST match the create path
    // (CreateEncoder above). The old "* 1 / framerate_" formula computed one
    // FRAME worth of bits, which nvEncReconfigureEncoder rejects as
    // "Invalid VBV buffer size" (err=8) — that aborted every Encode() below
    // so no frames were ever sent.
    encode_config.rcParams.vbvBufferSize =
        encode_config.rcParams.averageBitRate;
    encode_config.rcParams.vbvInitialDelay =
        encode_config.rcParams.vbvBufferSize;
    // Clear up-front: even if Reconfigure fails we must NOT retry it on every
    // frame (that produced the 13k-line error flood) and must NOT abort the
    // encode — the encoder is still valid at its current config.
    reconfigure_needed_ = false;
    try {
      nv_encoder_->Reconfigure(&reconfigure_params);
    } catch (const NVENCException& e) {
      RTC_LOG(LS_ERROR) << __FUNCTION__
                        << " Reconfigure failed (non-fatal): " << e.what();
      NvProbeLog("Encode: Reconfigure FAILED (non-fatal, keep encoding): %s",
                 e.what());
      // fall through and encode the frame at the existing config
    }
  }

  static int s_path_dbg_counter = 0;
  const bool dbg_path = (s_path_dbg_counter++ % 60) == 0;

  if (frame_types != nullptr) {
    int ft0 = (int)(*frame_types)[0];
    if (dbg_path) {
      NvProbeLog("Encode path: frame_types[0]=%d size=%zu (0=Empty 3=Key 4=Delta)",
                 ft0, frame_types->size());
    }
    // We only support a single stream.
    RTC_DCHECK_EQ(frame_types->size(), static_cast<size_t>(1));
    // Skip frame?
    if ((*frame_types)[0] == webrtc::VideoFrameType::kEmptyFrame) {
      // Log every empty-frame skip — if WebRTC sends a steady stream of
      // these the encoder produces no output and the resolution ladder
      // collapses, which is the symptom we're chasing.
      static int s_empty_counter = 0;
      if ((s_empty_counter++ % 60) == 0) {
        NvProbeLog("Encode: kEmptyFrame #%d -> return OK no encode", s_empty_counter);
      }
      return WEBRTC_VIDEO_CODEC_OK;
    }
    // Force key frame?
    send_key_frame =
        (*frame_types)[0] == webrtc::VideoFrameType::kVideoFrameKey;
  } else if (dbg_path) {
    NvProbeLog("Encode path: frame_types=null");
  }

  NV_ENC_PIC_PARAMS pic_params = {NV_ENC_PIC_PARAMS_VER};
  pic_params.encodePicFlags = 0;
  if (send_key_frame) {
    pic_params.encodePicFlags =
        NV_ENC_PIC_FLAG_FORCEINTRA | NV_ENC_PIC_FLAG_FORCEIDR;
  }
  pic_params.inputWidth = width_;
  pic_params.inputHeight = height_;

  v_packet_.clear();

#ifdef _WIN32
  if (is_native) {
    // *** Zero-Copy: Capture-Textur -> NVENC-Input, reiner GPU-CopyResource ***
    const NvEncInputFrame* in = active_enc->GetNextInputFrame();
    if (!in) return WEBRTC_VIDEO_CODEC_ERROR;
    native_context_->CopyResource(
        reinterpret_cast<ID3D11Texture2D*>(in->inputPtr), nb->texture());
  } else {
  if (!id3d11_texture_) {
    static int s_no_tex_counter = 0;
    if ((s_no_tex_counter++ % 60) == 0) {
      NvProbeLog("Encode path: id3d11_texture_ is NULL #%d -> ERROR",
                 s_no_tex_counter);
    }
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  const NvEncInputFrame* input_frame = nv_encoder_->GetNextInputFrame();
  if (!input_frame) {
    static int s_no_in_counter = 0;
    if ((s_no_in_counter++ % 60) == 0) {
      NvProbeLog("Encode path: GetNextInputFrame returned NULL #%d -> ERROR",
                 s_no_in_counter);
    }
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  D3D11_MAPPED_SUBRESOURCE map = {};
  HRESULT map_hr = id3d11_context_->Map(
      id3d11_texture_.Get(), D3D11CalcSubresource(0, 0, 1),
      D3D11_MAP_WRITE, 0, &map);
  if (FAILED(map_hr) || map.pData == nullptr) {
    static int s_map_fail_counter = 0;
    if ((s_map_fail_counter++ % 60) == 0) {
      NvProbeLog("Encode path: D3D11 Map FAILED hr=0x%08lx pData=%p #%d -> ERROR",
                 (long)map_hr, map.pData, s_map_fail_counter);
    }
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  if (dbg_path) {
    NvProbeLog("Encode path: pre-EncodeFrame OK (map.pData=%p rowPitch=%u)",
               map.pData, (unsigned)map.RowPitch);
  }
  if (frame.video_frame_buffer()->type() ==
      webrtc::VideoFrameBuffer::Type::kNV12) {
    webrtc::NV12BufferInterface* frame_buffer =
        static_cast<webrtc::NV12BufferInterface*>(
            frame.video_frame_buffer().get());
    libyuv::NV12Copy(
        frame_buffer->DataY(), frame_buffer->StrideY(), frame_buffer->DataUV(),
        frame_buffer->StrideUV(), (uint8_t*)map.pData, map.RowPitch,
        ((uint8_t*)map.pData + height_ * map.RowPitch), map.RowPitch,
        frame_buffer->width(), frame_buffer->height());
  } else {
    webrtc::scoped_refptr<const webrtc::I420BufferInterface> frame_buffer =
        frame.video_frame_buffer()->ToI420();
    libyuv::I420ToNV12(
        frame_buffer->DataY(), frame_buffer->StrideY(), frame_buffer->DataU(),
        frame_buffer->StrideU(), frame_buffer->DataV(), frame_buffer->StrideV(),
        (uint8_t*)map.pData, map.RowPitch,
        ((uint8_t*)map.pData + height_ * map.RowPitch), map.RowPitch,
        frame_buffer->width(), frame_buffer->height());
  }
  id3d11_context_->Unmap(id3d11_texture_.Get(), D3D11CalcSubresource(0, 0, 1));
  ID3D11Texture2D* nv11_texture =
      reinterpret_cast<ID3D11Texture2D*>(input_frame->inputPtr);
  id3d11_context_->CopyResource(nv11_texture, id3d11_texture_.Get());
  }  // else: CPU-I420/NV12-Pfad
#endif
#ifdef __linux__

  //RTC_LOG(LS_INFO) << "type="
  //                 << VideoFrameBufferTypeToString(
  //                        frame.video_frame_buffer()->type())
  //                 << " width_=" << width_ << " height_=" << height_
  //                 << " frame_width=" << frame.video_frame_buffer()->width()
  //                 << " frame_height=" << frame.video_frame_buffer()->height();

  if (frame.video_frame_buffer()->type() ==
      webrtc::VideoFrameBuffer::Type::kNV12) {
    webrtc::NV12Buffer* buffer =
        static_cast<webrtc::NV12Buffer*>(frame.video_frame_buffer().get());
    try {
      cuda_->Copy(nv_encoder_.get(), buffer->DataY(), width_, height_);
    } catch (const NVENCException& e) {
      RTC_LOG(LS_ERROR) << e.what();
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
  } else {
    webrtc::scoped_refptr<const webrtc::I420BufferInterface> frame_buffer =
        frame.video_frame_buffer()->ToI420();
    cuda_->Copy(nv_encoder_.get(), frame_buffer->DataY(), frame_buffer->width(),
                frame_buffer->height());
  }
#endif

  // Our vendored NvEncoder emits std::vector<std::vector<uint8_t>>; we
  // re-wrap into the local NvEncOutputFrame struct so the iteration below
  // (Momo's original idiom) keeps working.
  std::vector<std::vector<uint8_t>> raw_packets;
#ifdef _WIN32
  LARGE_INTEGER enc_t0, enc_t1, enc_freq;
  QueryPerformanceFrequency(&enc_freq);
  QueryPerformanceCounter(&enc_t0);
#endif
  try {
    active_enc->EncodeFrame(raw_packets, &pic_params);
  } catch (const NVENCException& e) {
    RTC_LOG(LS_ERROR) << __FUNCTION__ << e.what();
    NvProbeLog("Encode: EncodeFrame THREW: %s", e.what());
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
#ifdef _WIN32
  QueryPerformanceCounter(&enc_t1);
  double enc_ms = (double)(enc_t1.QuadPart - enc_t0.QuadPart) * 1000.0 /
                   (double)enc_freq.QuadPart;
  // Light-weight running counter — log every 60 encodes (~1s @ 60fps)
  // so we don't drown the file. Always log if 0 packets came back —
  // that's the smoking gun for "encoder accepts frames but never
  // emits output", which WebRTC reads as "encoder is dead" and
  // ladders the resolution down.
  static int enc_counter = 0;
  static int empty_in_a_row = 0;
  if (raw_packets.empty()) {
    ++empty_in_a_row;
    if (empty_in_a_row <= 5 || (empty_in_a_row % 30) == 0) {
      NvProbeLog("Encode: 0 packets returned (#%d in a row) enc_ms=%.2f",
                 empty_in_a_row, enc_ms);
    }
  } else {
    if (empty_in_a_row > 0) {
      NvProbeLog("Encode: first output after %d empty (enc_ms=%.2f)",
                 empty_in_a_row, enc_ms);
      empty_in_a_row = 0;
    }
    if ((enc_counter++ % 60) == 0) {
      size_t bytes = 0;
      for (auto& p : raw_packets) bytes += p.size();
      NvProbeLog("Encode #%d: %zu packets, %zu bytes, %.2f ms",
                 enc_counter, raw_packets.size(), bytes, enc_ms);
    }
  }
#endif
  v_packet_.clear();
  v_packet_.reserve(raw_packets.size());
  for (auto& raw : raw_packets) {
    v_packet_.push_back({std::move(raw),
                          send_key_frame ? NV_ENC_PIC_TYPE_IDR
                                         : NV_ENC_PIC_TYPE_UNKNOWN});
  }

  for (NvEncOutputFrame& output : v_packet_) {
    std::vector<uint8_t>& packet = output.frame;
    uint8_t* p = packet.data();
    size_t size = packet.size();
    if (codec_ == CudaVideoCodec::AV1) {
      // IVF ヘッダーが付いてるので取り除く
      if ((p[0] == 'D') && (p[1] == 'K') && (p[2] == 'I') && (p[3] == 'F')) {
        p += 32;
        size -= 32;
      }
      p += 12;
      size -= 12;
    }
    auto encoded_image_buffer = webrtc::EncodedImageBuffer::Create(p, size);

    encoded_image_.SetEncodedData(encoded_image_buffer);
    encoded_image_._encodedWidth = width_;
    encoded_image_._encodedHeight = height_;
    encoded_image_.content_type_ =
        (mode_ == webrtc::VideoCodecMode::kScreensharing)
            ? webrtc::VideoContentType::SCREENSHARE
            : webrtc::VideoContentType::UNSPECIFIED;
    encoded_image_.timing_.flags = webrtc::VideoSendTiming::kInvalid;
    encoded_image_.SetRtpTimestamp(frame.rtp_timestamp());
    encoded_image_.ntp_time_ms_ = frame.ntp_time_ms();
    encoded_image_.capture_time_ms_ = frame.render_time_ms();
    encoded_image_.rotation_ = frame.rotation();
    encoded_image_.SetColorSpace(frame.color_space());
    encoded_image_._frameType = webrtc::VideoFrameType::kVideoFrameDelta;

    // IDR フレームまたは I フレームはキーフレームとして扱う
    if (output.pictureType == NV_ENC_PIC_TYPE_IDR ||
        output.pictureType == NV_ENC_PIC_TYPE_I) {
      encoded_image_._frameType = webrtc::VideoFrameType::kVideoFrameKey;
    }

    webrtc::CodecSpecificInfo codec_specific;
    if (codec_ == CudaVideoCodec::H264) {
      codec_specific.codecType = webrtc::kVideoCodecH264;
      codec_specific.codecSpecific.H264.packetization_mode =
          webrtc::H264PacketizationMode::NonInterleaved;

      h264_bitstream_parser_.ParseBitstream(encoded_image_);
      encoded_image_.qp_ = h264_bitstream_parser_.GetLastSliceQp().value_or(-1);
    } else if (codec_ == CudaVideoCodec::H265) {
      codec_specific.codecType = webrtc::kVideoCodecH265;

      h265_bitstream_parser_.ParseBitstream(encoded_image_);
      encoded_image_.qp_ = h265_bitstream_parser_.GetLastSliceQp().value_or(-1);
    } else if (codec_ == CudaVideoCodec::AV1) {
      codec_specific.codecType = webrtc::kVideoCodecAV1;

      bool is_key =
          encoded_image_._frameType == webrtc::VideoFrameType::kVideoFrameKey;
      std::vector<webrtc::ScalableVideoController::LayerFrameConfig>
          layer_frames = svc_controller_->NextFrameConfig(is_key);
      codec_specific.end_of_picture = true;
      codec_specific.scalability_mode = scalability_mode_;
      // layer_frames[0] が無効の場合、アクセス違反となるが、基本的に無効になることはない
      codec_specific.generic_frame_info =
          svc_controller_->OnEncodeDone(layer_frames[0]);
      if (is_key && codec_specific.generic_frame_info) {
        codec_specific.template_structure =
            svc_controller_->DependencyStructure();
        auto& resolutions = codec_specific.template_structure->resolutions;
        resolutions = {webrtc::RenderResolution(encoded_image_._encodedWidth,
                                                encoded_image_._encodedHeight)};
      }
    }

    webrtc::EncodedImageCallback::Result result =
        callback_->OnEncodedImage(encoded_image_, &codec_specific);
    if (result.error != webrtc::EncodedImageCallback::Result::OK) {
      RTC_LOG(LS_ERROR) << __FUNCTION__
                        << " OnEncodedImage failed error:" << result.error;
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    bitrate_adjuster_.Update(packet.size());
  }

  return WEBRTC_VIDEO_CODEC_OK;
}

void NvCodecVideoEncoderImpl::SetRates(
    const webrtc::VideoEncoder::RateControlParameters& parameters) {
  if (!nv_encoder_) {
    RTC_LOG(LS_WARNING) << "SetRates() while uninitialized.";
    return;
  }

  if (parameters.framerate_fps < 1.0) {
    RTC_LOG(LS_WARNING) << "Invalid frame rate: " << parameters.framerate_fps;
    return;
  }

  // bitrate が 0 の時レイヤーを無効にする
  if (svc_controller_) {
    svc_controller_->OnRatesUpdated(parameters.bitrate);
  }

  uint32_t new_framerate = (uint32_t)parameters.framerate_fps;
  uint32_t new_bitrate = parameters.bitrate.get_sum_bps();
  RTC_LOG(LS_INFO) << __FUNCTION__ << " framerate_:" << framerate_
                   << " new_framerate: " << new_framerate
                   << " target_bitrate_bps_:" << target_bitrate_bps_
                   << " new_bitrate:" << new_bitrate
                   << " max_bitrate_bps_:" << max_bitrate_bps_;
  framerate_ = new_framerate;
  target_bitrate_bps_ = new_bitrate;
  bitrate_adjuster_.SetTargetBitrateBps(target_bitrate_bps_);
  reconfigure_needed_ = true;
}

webrtc::VideoEncoder::EncoderInfo NvCodecVideoEncoderImpl::GetEncoderInfo()
    const {
  webrtc::VideoEncoder::EncoderInfo info;
  // Zero-Copy: Bei Bildschirm-Freigabe liefert unser Capturer kNative-Frames
  // (D3D11-Textur in honeycord::D3D11FrameBuffer); Encode() verarbeitet die
  // direkt per GPU-CopyResource in den NVENC-Input (EnsureNativeEncoder).
  // I420/NV12 (Kamera, CPU-Fallback) wird ebenso verarbeitet. Daher TRUE ->
  // webrtc reicht native Frames UNVERAENDERT an Encode() durch und ruft NICHT
  // selbst ToI420()+I420Buffer::Rotate() darauf auf dem Encoder-Thread (das
  // crashte vorher mit Access Violation in I420Buffer::Rotate).
  info.supports_native_handle = true;
  info.implementation_name = "NvCodec";
  info.is_hardware_accelerated = true;
  info.scaling_settings = webrtc::VideoEncoder::ScalingSettings(
      kLowH264QpThreshold, kHighH264QpThreshold);
  return info;
}

int32_t NvCodecVideoEncoderImpl::InitNvEnc() {
#ifdef _WIN32
  // If the constructor couldn't set up D3D11 (no NVIDIA adapter, driver
  // gone), id3d11_device_ is null and any further use crashes. Bail out
  // cleanly so WebRTC falls back to the next encoder in the chain.
  if (!id3d11_device_) {
    NvProbeLog("InitNvEnc: id3d11_device_ is null, aborting");
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  nv_encoder_ = CreateEncoder(
      codec_, width_, height_, framerate_, target_bitrate_bps_,
      max_bitrate_bps_, id3d11_device_.Get(), id3d11_texture_.GetAddressOf());
#endif
#ifdef __linux__
  nv_encoder_ =
      CreateEncoder(codec_, width_, height_, framerate_, target_bitrate_bps_,
                    max_bitrate_bps_, cuda_.get(), is_nv12_);
#endif

  if (nv_encoder_ == nullptr) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  reconfigure_needed_ = false;

  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvCodecVideoEncoderImpl::ReleaseNvEnc() {
  if (nv_encoder_) {
    try {
      std::vector<std::vector<uint8_t>> drain;
      nv_encoder_->EndEncode(drain);
      nv_encoder_->DestroyEncoder();
    } catch (const NVENCException& e) {
      RTC_LOG(LS_ERROR) << __FUNCTION__ << e.what();
    }
    nv_encoder_ = nullptr;
#ifdef _WIN32
    id3d11_texture_.Reset();
#endif
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

std::unique_ptr<NvEncoder> NvCodecVideoEncoderImpl::CreateEncoder(
    CudaVideoCodec codec,
    int width,
    int height,
    int framerate,
    int target_bitrate_bps,
    int max_bitrate_bps
#ifdef _WIN32
    ,
    ID3D11Device* id3d11_device,
    ID3D11Texture2D** out_id3d11_texture
#endif
#ifdef __linux__
    ,
    NvCodecVideoEncoderCuda* cuda,
    bool is_nv12
#endif
) {
  std::unique_ptr<NvEncoder> encoder;

#ifdef _WIN32
  DXGI_FORMAT dxgi_format = DXGI_FORMAT_NV12;
  NV_ENC_BUFFER_FORMAT nvenc_format = NV_ENC_BUFFER_FORMAT_NV12;
  D3D11_TEXTURE2D_DESC desc;
  ZeroMemory(&desc, sizeof(D3D11_TEXTURE2D_DESC));
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = dxgi_format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  id3d11_device->CreateTexture2D(&desc, NULL, out_id3d11_texture);

  // Driver が古いとかに気づくのはココ
  // HoneyCord diagnostic: this is the call that loads nvEncodeAPI64.dll and
  // initialises the NVENC session. If the installed driver is too old or
  // the ABI between our vendored NvEncoder.cpp and the real nvEncodeAPI.h
  // doesn't match, NVENCException fires here.
  NvProbeLog("CreateEncoder: new NvEncoderD3D11(%dx%d, NV12)", width, height);
  try {
    encoder.reset(
        new NvEncoderD3D11(id3d11_device, width, height, nvenc_format));
  } catch (const NVENCException& e) {
    RTC_LOG(LS_ERROR) << "NvCodec::CreateEncoder: NvEncoderD3D11 ctor threw: "
                      << e.what();
    NvProbeLog("NvEncoderD3D11 ctor THREW NVENCException: %s", e.what());
    return nullptr;
  } catch (const std::exception& e) {
    RTC_LOG(LS_ERROR) << "NvCodec::CreateEncoder: NvEncoderD3D11 ctor std::ex: "
                      << e.what();
    NvProbeLog("NvEncoderD3D11 ctor THREW std::ex: %s", e.what());
    return nullptr;
  } catch (...) {
    RTC_LOG(LS_ERROR) << "NvCodec::CreateEncoder: NvEncoderD3D11 ctor unknown ex";
    NvProbeLog("NvEncoderD3D11 ctor THREW unknown");
    return nullptr;
  }
  NvProbeLog("NvEncoderD3D11 ctor ok");
#endif

#ifdef __linux__
  try {
    encoder.reset(cuda->CreateNvEncoder(width, height, is_nv12));
  } catch (const NVENCException& e) {
    RTC_LOG(LS_ERROR) << __FUNCTION__ << e.what();
    return nullptr;
  }
#endif

  NV_ENC_INITIALIZE_PARAMS initialize_params = {NV_ENC_INITIALIZE_PARAMS_VER};
  NV_ENC_CONFIG encode_config = {NV_ENC_CONFIG_VER};
  initialize_params.encodeConfig = &encode_config;
  try {
    // The vendored NvEncoder.h has the older 3-arg CreateDefaultEncoderParams
    // (no NV_ENC_TUNING_INFO). Low-latency tuning is approximated via the
    // P3/LOW_LATENCY_HQ preset families below; the encode-config tweaks
    // (gopLength = INFINITE, frameIntervalP = 1, no B-frames via disableBadapt)
    // give us the real-time profile we need.
    NvProbeLog("calling CreateDefaultEncoderParams (codec=%d, preset=P3)",
               (int)codec);
    if (codec == CudaVideoCodec::H264) {
      encoder->CreateDefaultEncoderParams(
          &initialize_params, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P3_GUID);
    } else if (codec == CudaVideoCodec::H265) {
      encoder->CreateDefaultEncoderParams(
          &initialize_params, NV_ENC_CODEC_HEVC_GUID, NV_ENC_PRESET_P2_GUID);
    } else if (codec == CudaVideoCodec::AV1) {
      encoder->CreateDefaultEncoderParams(
          &initialize_params, NV_ENC_CODEC_AV1_GUID, NV_ENC_PRESET_P2_GUID);
    }
    NvProbeLog("CreateDefaultEncoderParams ok");

    // NVENC SDK 10+ split tuning out of the preset: P1-P7 are generic
    // presets, the actual real-time vs HQ vs lossless decision is the
    // tuningInfo. CreateDefaultEncoderParams in the vendored NvEncoder.cpp
    // is the 3-arg version and never touches this field — it stays at
    // NV_ENC_TUNING_INFO_UNDEFINED (value 0), which the driver rejects
    // with "Unsupported color format" (mapped from INVALID_PARAM).
    //
    // LOW_LATENCY is the realtime profile: no B-frames, predictable
    // GOP timing — matches our frameIntervalP=1 + gopLength=INFINITE
    // setup. ULTRA_LOW_LATENCY is even more aggressive but trades
    // visible quality at our bitrates; HIGH_QUALITY would internally
    // re-enable B-frames on P3+, which conflicts with frameIntervalP=1.
    initialize_params.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;

    // Force SYNCHRONOUS encode. CreateDefaultEncoderParams sets
    // enableEncodeAsync=1 on Windows by default. With async, NvEncoder.cpp's
    // EncodeFrame uses WaitForSingleObject(completionEvent, INFINITE) — and
    // WebRTC's encoder pump silently stalls on that wait because the event
    // is fired on the wrong thread under our pipeline. Symptom: encoder
    // initialises fine, but NO output makes it back to OnEncodedImage,
    // WebRTC sees 0 bytes/sec and starts dropping resolution one step at a
    // time hoping to recover. Sync mode makes EncodeFrame block normally
    // and return packets through nvEncLockBitstream + ProcessOutput — a
    // model that fits our single-call Encode() exactly.
    initialize_params.enableEncodeAsync = 0;

    //initialize_params.enablePTD = 1;
    initialize_params.frameRateDen = 1;
    initialize_params.frameRateNum = framerate;
    initialize_params.maxEncodeWidth = width;
    initialize_params.maxEncodeHeight = height;

    // CreateDefaultEncoderParams leaves rateControlMode = CONSTQP. We're
    // setting averageBitRate / maxBitRate / vbvBufferSize / enableAQ /
    // aqStrength below — all of which are CBR/VBR parameters NVENC rejects
    // (NV_ENC_ERR_UNSUPPORTED_PARAM, err=8) when the mode is still CONSTQP.
    //
    // We use VBR (not CBR): the BitrateAdjuster + WebRTC's SetRates() feed
    // averageBitRate dynamically with the network-estimated rate while
    // maxBitRate stays at the user-chosen ceiling. Plain CBR rejects this
    // avg-different-from-max combination outright; VBR accepts it and lets
    // the encoder consume burst-capacity up to maxBitRate when the source
    // has motion, then idle back down on static frames.
    encode_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    encode_config.rcParams.averageBitRate = target_bitrate_bps;
    encode_config.rcParams.maxBitRate = max_bitrate_bps;

    encode_config.rcParams.disableBadapt = 1;
    // VBV buffer = 1 second of target bitrate. The original code computed
    // averageBitRate * den / num which equals one FRAME worth of bits — way
    // too small for real CBR, makes the rate controller swing wildly. One
    // second is the canonical NVENC low-latency setting.
    encode_config.rcParams.vbvBufferSize =
        encode_config.rcParams.averageBitRate;
    encode_config.rcParams.vbvInitialDelay =
        encode_config.rcParams.vbvBufferSize;
    encode_config.gopLength = NVENC_INFINITE_GOPLENGTH;
    encode_config.frameIntervalP = 1;
    // NVENC forbids changing frameFieldMode via Reconfigure ("Reconfiguration
    // of frame field mode not supported", err 8). The preset leaves this at
    // UNDEFINED(0) while the driver actually runs progressive FRAME mode, so
    // GetInitializeParams() round-trips 0 and every SetRates() reconfigure was
    // rejected — the encoder could never raise its bitrate in place and stayed
    // pinned near the low start bitrate. Pin it explicitly so it matches.
    encode_config.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
    encode_config.rcParams.enableAQ = 1;
    // AQ strength is 1..15; 0 means disabled by spec. Mid-range (8) is the
    // OBS-style default — visibly better grain handling without sacrificing
    // throughput on the encoder block.
    encode_config.rcParams.aqStrength = 8;

    if (codec == CudaVideoCodec::H264) {
      // NV12 is YUV 4:2:0. NV_ENC_CONFIG_H264::chromaFormatIDC must be 1
      // for that. The legacy nvEncGetEncodePresetConfig() (used by our
      // vendored NvEncoder.cpp) doesn't populate this field correctly for
      // the P-presets, leaving chromaFormatIDC at 0. That makes the driver
      // throw "Unsupported color format" out of nvEncInitializeEncoder.
      encode_config.encodeCodecConfig.h264Config.chromaFormatIDC = 1;
      // AUTOSELECT lets the driver pick the right H.264 profile for
      // 4:2:0 + our other settings (typically Main or High). Explicit so
      // we don't inherit a leftover/uninitialised GUID from the preset
      // config blob.
      encode_config.profileGUID = NV_ENC_CODEC_PROFILE_AUTOSELECT_GUID;
      encode_config.encodeCodecConfig.h264Config.idrPeriod =
          NVENC_INFINITE_GOPLENGTH;
      encode_config.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
      encode_config.encodeCodecConfig.h264Config.sliceMode = 0;
      encode_config.encodeCodecConfig.h264Config.sliceModeData = 0;
    } else if (codec == CudaVideoCodec::H265) {
      encode_config.encodeCodecConfig.hevcConfig.idrPeriod =
          NVENC_INFINITE_GOPLENGTH;
      encode_config.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
      encode_config.encodeCodecConfig.hevcConfig.sliceMode = 0;
      encode_config.encodeCodecConfig.hevcConfig.sliceModeData = 0;
    } else if (codec == CudaVideoCodec::AV1) {
      encode_config.encodeCodecConfig.av1Config.idrPeriod =
          NVENC_INFINITE_GOPLENGTH;
      // キーフレームにサイズ情報が付与されていない状態になるのを防ぐ
      encode_config.encodeCodecConfig.av1Config.repeatSeqHdr = 1;
    }

    NvProbeLog("CreateEncoder params: rc=%d tuning=%d avgBR=%u maxBR=%u vbv=%u "
               "aq=%d/%d gop=%u frameInt=%u idr=%u w=%u h=%u fps=%u/%u async=%d",
               (int)encode_config.rcParams.rateControlMode,
               (int)initialize_params.tuningInfo,
               encode_config.rcParams.averageBitRate,
               encode_config.rcParams.maxBitRate,
               encode_config.rcParams.vbvBufferSize,
               (int)encode_config.rcParams.enableAQ,
               (int)encode_config.rcParams.aqStrength,
               (unsigned)encode_config.gopLength,
               (unsigned)encode_config.frameIntervalP,
               (unsigned)encode_config.encodeCodecConfig.h264Config.idrPeriod,
               initialize_params.encodeWidth,
               initialize_params.encodeHeight,
               initialize_params.frameRateNum,
               initialize_params.frameRateDen,
               (int)initialize_params.enableEncodeAsync);
    NvProbeLog("calling encoder->CreateEncoder() (initialize_params_ver=0x%08x, config_ver=0x%08x)",
               (unsigned)NV_ENC_INITIALIZE_PARAMS_VER, (unsigned)NV_ENC_CONFIG_VER);
    encoder->CreateEncoder(&initialize_params);
    NvProbeLog("encoder->CreateEncoder() OK framerate=%d bitrate=%d maxBitRate=%d",
               framerate, target_bitrate_bps,
               (int)encode_config.rcParams.maxBitRate);
  } catch (const NVENCException& e) {
    RTC_LOG(LS_ERROR) << "NvCodec::CreateEncoder: NVENCException: " << e.what();
    NvProbeLog("CreateEncoder/Params THREW NVENCException: %s", e.what());
    return nullptr;
  } catch (const std::exception& e) {
    RTC_LOG(LS_ERROR) << "NvCodec::CreateEncoder: std::exception: " << e.what();
    NvProbeLog("CreateEncoder/Params THREW std::ex: %s", e.what());
    return nullptr;
  } catch (...) {
    RTC_LOG(LS_ERROR) << "NvCodec::CreateEncoder: unknown exception";
    NvProbeLog("CreateEncoder/Params THREW unknown");
    return nullptr;
  }

  return encoder;
}

// HoneyCord diagnostic build: dense logging through the NVENC probe so we
// can pinpoint exactly which step fails on a given GPU/driver/version combo
// from `%LOCALAPPDATA%\HoneyCord\last.log`.
bool NvCodecVideoEncoder::IsSupported(std::shared_ptr<CudaContext> cuda_context,
                                      CudaVideoCodec codec) {
  RTC_LOG(LS_INFO) << "NvCodec::IsSupported: ENTER codec=" << (int)codec;
  NvProbeLog("=== NvCodec::IsSupported ENTER codec=%d ===", (int)codec);
  try {

#ifdef __linux__
    if (cuda_context == nullptr) {
      return false;
    }

    if (!dyn::DynModule::Instance().IsLoadable(dyn::CUDA_SO)) {
      return false;
    }
    if (!dyn::DynModule::Instance().IsLoadable(dyn::NVCUVID_SO)) {
      return false;
    }
    if (dyn::DynModule::Instance().GetFunc(dyn::CUDA_SO, "cuDeviceGetName") ==
        nullptr) {
      return false;
    }
    if (dyn::DynModule::Instance().GetFunc(dyn::NVCUVID_SO,
                                           "cuvidMapVideoFrame") == nullptr) {
      return false;
    }
#endif

#ifdef _WIN32
    // -------- DXGI: enumerate every adapter so we see what Windows hands
    //          us, not just adapter[0]. Confirms whether the box is a
    //          single-NVIDIA desktop or a hybrid system.
    ComPtr<IDXGIFactory1> idxgi_factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                     (void**)idxgi_factory.GetAddressOf());
    if (FAILED(hr)) {
      RTC_LOG(LS_ERROR) << "NvCodec::IsSupported: CreateDXGIFactory1 failed hr=0x"
                        << std::hex << hr;
      NvProbeLog("CreateDXGIFactory1 FAILED hr=0x%08lx", (long)hr);
      return false;
    }
    NvProbeLog("DXGI factory ok");

    int nvidia_idx = -1;
    for (UINT i = 0;; ++i) {
      ComPtr<IDXGIAdapter1> a;
      if (FAILED(idxgi_factory->EnumAdapters1(i, a.GetAddressOf()))) break;
      DXGI_ADAPTER_DESC1 d{};
      a->GetDesc1(&d);
      char name[128];
      size_t n = 0;
      wcstombs_s(&n, name, d.Description, sizeof(name));
      RTC_LOG(LS_INFO) << "NvCodec::IsSupported: adapter[" << i << "] '" << name
                       << "' vendor=0x" << std::hex << d.VendorId
                       << " dev=0x" << d.DeviceId
                       << " flags=0x" << d.Flags;
      NvProbeLog("adapter[%u] '%s' vendor=0x%04x dev=0x%04x flags=0x%lx (0=hw,2=remote,4=software)",
                 i, name, d.VendorId, d.DeviceId, (long)d.Flags);
      if (d.VendorId == 0x10DE && nvidia_idx < 0) nvidia_idx = (int)i;
    }
    if (nvidia_idx < 0) {
      RTC_LOG(LS_WARNING) << "NvCodec::IsSupported: no NVIDIA adapter found";
      NvProbeLog("NO NVIDIA ADAPTER FOUND (vendor 0x10DE missing)");
      return false;
    }
    NvProbeLog("picking NVIDIA adapter idx=%d", nvidia_idx);

    ComPtr<IDXGIAdapter> idxgi_adapter;
    hr = idxgi_factory->EnumAdapters(nvidia_idx, idxgi_adapter.GetAddressOf());
    if (FAILED(hr)) {
      RTC_LOG(LS_ERROR) << "NvCodec::IsSupported: EnumAdapters(" << nvidia_idx
                        << ") failed hr=0x" << std::hex << hr;
      return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> id3d11_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> id3d11_context;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> id3d11_texture;
    hr = D3D11CreateDevice(idxgi_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, NULL,
                            0, NULL, 0, D3D11_SDK_VERSION,
                            id3d11_device.GetAddressOf(), NULL,
                            id3d11_context.GetAddressOf());
    if (FAILED(hr)) {
      RTC_LOG(LS_ERROR) << "NvCodec::IsSupported: D3D11CreateDevice failed hr=0x"
                        << std::hex << hr;
      NvProbeLog("D3D11CreateDevice FAILED hr=0x%08lx", (long)hr);
      return false;
    }
    NvProbeLog("D3D11 device ok, calling CreateEncoder probe (640x480)");

    auto encoder = NvCodecVideoEncoderImpl::CreateEncoder(
        codec, 640, 480, 30, 100 * 1000, 500 * 1000, id3d11_device.Get(),
        id3d11_texture.GetAddressOf());
#endif
#ifdef __linux__
    auto cuda = std::unique_ptr<NvCodecVideoEncoderCuda>(
        new NvCodecVideoEncoderCuda(cuda_context));
    auto encoder = NvCodecVideoEncoderImpl::CreateEncoder(
        codec, 640, 480, 30, 100 * 1000, 500 * 1000, cuda.get(), true);
#endif
    if (encoder == nullptr) {
      RTC_LOG(LS_WARNING) << "NvCodec::IsSupported: CreateEncoder returned nullptr";
      NvProbeLog("CreateEncoder returned nullptr -> probe FAIL");
      return false;
    }

    NvProbeLog("=== NvCodec::IsSupported SUCCESS ===");
    return true;
  } catch (const NVENCException& e) {
    RTC_LOG(LS_ERROR) << "NvCodec::IsSupported: NVENCException: " << e.what();
    NvProbeLog("CAUGHT NVENCException: %s", e.what());
    return false;
  } catch (const std::exception& e) {
    RTC_LOG(LS_ERROR) << "NvCodec::IsSupported: std::exception: " << e.what();
    NvProbeLog("CAUGHT std::exception: %s", e.what());
    return false;
  } catch (...) {
    RTC_LOG(LS_ERROR) << "NvCodec::IsSupported: unknown exception";
    NvProbeLog("CAUGHT unknown exception");
    return false;
  }
}

std::unique_ptr<NvCodecVideoEncoder> NvCodecVideoEncoder::Create(
    std::shared_ptr<CudaContext> cuda_context,
    CudaVideoCodec codec) {
  return std::unique_ptr<NvCodecVideoEncoder>(
      new NvCodecVideoEncoderImpl(cuda_context, codec));
}

}  // namespace sora
