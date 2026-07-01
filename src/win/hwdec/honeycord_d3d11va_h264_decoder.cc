#include "src/win/hwdec/honeycord_d3d11va_h264_decoder.h"

#ifdef _WIN32

#include <codecapi.h>
#include <d3d11_1.h>
#include <mfapi.h>
#include <mferror.h>
#include <wmcodecdsp.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "api/video/video_frame.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "src/win/honeycord_d3d11_frame.h"

using Microsoft::WRL::ComPtr;

namespace libwebrtc {

namespace {
constexpr char kImplName[] = "HoneyCordD3D11VA_H264";

// RTP-Zeitbasis (90 kHz) -> Media-Foundation-Einheit (100 ns).
inline LONGLONG RtpTo100ns(uint32_t rtp_90khz) {
  return static_cast<LONGLONG>(rtp_90khz) * 1000 / 9;  // /90000 * 1e7
}

// Diagnose-Timing: ms seit t0.
inline double MsSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

// Eine IMFMediaType anlegen (Major=Video, Subtype=subtype).
ComPtr<IMFMediaType> MakeVideoType(const GUID& subtype) {
  ComPtr<IMFMediaType> t;
  if (FAILED(MFCreateMediaType(&t))) return nullptr;
  t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  t->SetGUID(MF_MT_SUBTYPE, subtype);
  return t;
}
}  // namespace

D3D11VAH264Decoder::D3D11VAH264Decoder() = default;

D3D11VAH264Decoder::~D3D11VAH264Decoder() {
  ReleaseInternal();
}

// ---------------------------------------------------------------------------
// Probe: laesst sich der MF-H.264-Decoder mit einem D3D11-Device-Manager
// erzeugen + die D3D-Acceleration setzen? Wenn ja, ist HW-Decode verfuegbar.
// ---------------------------------------------------------------------------
bool D3D11VAH264Decoder::IsSupported() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool mf_started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
  bool ok = false;
  do {
    ComPtr<ID3D11Device> dev;
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                 nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr,
                                 nullptr)))
      break;
    UINT token = 0;
    ComPtr<IMFDXGIDeviceManager> mgr;
    if (FAILED(MFCreateDXGIDeviceManager(&token, &mgr))) break;
    if (FAILED(mgr->ResetDevice(dev.Get(), token))) break;
    ComPtr<IMFTransform> mft;
    if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&mft))))
      break;
    if (FAILED(mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                   reinterpret_cast<ULONG_PTR>(mgr.Get()))))
      break;
    ok = true;
  } while (false);
  if (mf_started) MFShutdown();
  RTC_LOG(LS_INFO) << "[hwdec] D3D11VA H264 IsSupported=" << ok;
  return ok;
}

bool D3D11VAH264Decoder::Configure(const Settings& /*settings*/) {
  // Lazy-Init beim ersten Frame (Aufloesung erst aus dem Stream bekannt).
  return true;
}

int32_t D3D11VAH264Decoder::RegisterDecodeCompleteCallback(
    webrtc::DecodedImageCallback* callback) {
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

// ---------------------------------------------------------------------------
// Device + DeviceManager + MFT erzeugen + Input/Output-Type setzen.
// ---------------------------------------------------------------------------
bool D3D11VAH264Decoder::EnsureMft() {
  if (mft_configured_) return true;

  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
    RTC_LOG(LS_ERROR) << "[hwdec] MFStartup failed";
    return false;
  }

  UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                 nullptr, 0, D3D11_SDK_VERSION, &device_, nullptr,
                                 &context_);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "[hwdec] D3D11CreateDevice failed hr=0x" << hr;
    return false;
  }
  // Multithread-Schutz: MFT-Decode (Decode-Thread) + Renderer/ANGLE (Raster-
  // Thread) greifen auf dasselbe Device/dieselben Shared-Texturen zu.
  ComPtr<ID3D10Multithread> mt;
  if (SUCCEEDED(context_.As(&mt))) mt->SetMultithreadProtected(TRUE);

  // Diagnose (2026-07-01): auf welchem GPU-Adapter laeuft der Decoder, und welcher
  // Adapter treibt das Display (= den ANGLE zum Fenster-Composite nutzt)? Sind das
  // VERSCHIEDENE Adapter, sampelt ANGLE die dekodierte Shared-Textur ueber die
  // Adapter-Grenze -> teures Cross-Adapter-Sampling pro Composite -> ~8 fps.
  {
    auto logline = [](const char* s) {
      if (const char* base = std::getenv("LOCALAPPDATA")) {
        std::string p = std::string(base) + "\\HoneyCord";
        CreateDirectoryA(p.c_str(), nullptr);
        if (FILE* f = std::fopen((p + "\\hwdec.log").c_str(), "a")) { std::fputs(s, f); std::fclose(f); }
      }
    };
    ComPtr<IDXGIDevice> dd;
    ComPtr<IDXGIAdapter> da;
    ComPtr<IDXGIFactory1> fac;
    LUID decLuid = {};
    if (SUCCEEDED(device_.As(&dd)) && SUCCEEDED(dd->GetAdapter(&da))) {
      DXGI_ADAPTER_DESC ddesc = {};
      if (SUCCEEDED(da->GetDesc(&ddesc))) decLuid = ddesc.AdapterLuid;
      da->GetParent(IID_PPV_ARGS(&fac));  // Factory ueber den Adapter (kein dxgi.lib noetig)
    }
    if (fac) {
      ComPtr<IDXGIAdapter1> a;
      for (UINT i = 0; fac->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 ad = {};
        a->GetDesc1(&ad);
        ComPtr<IDXGIOutput> o;
        bool hasOut = SUCCEEDED(a->EnumOutputs(0, &o));
        bool isDec = (ad.AdapterLuid.LowPart == decLuid.LowPart &&
                      ad.AdapterLuid.HighPart == decLuid.HighPart);
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "[hwdec] adapter[%u] '%ls' vram=%lluMB display-output=%d%s\n",
                      i, ad.Description,
                      static_cast<unsigned long long>(ad.DedicatedVideoMemory >> 20),
                      hasOut ? 1 : 0, isDec ? "  <= DECODER benutzt diesen" : "");
        logline(buf);
      }
    }
  }

  if (FAILED(device_.As(&video_device_)) || FAILED(context_.As(&video_context_))) {
    RTC_LOG(LS_ERROR) << "[hwdec] no ID3D11VideoDevice/Context";
    return false;
  }

  if (FAILED(MFCreateDXGIDeviceManager(&reset_token_, &dxgi_manager_)) ||
      FAILED(dxgi_manager_->ResetDevice(device_.Get(), reset_token_))) {
    RTC_LOG(LS_ERROR) << "[hwdec] DXGIDeviceManager init failed";
    return false;
  }

  if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&mft_)))) {
    RTC_LOG(LS_ERROR) << "[hwdec] CoCreateInstance H264 MFT failed";
    return false;
  }
  if (FAILED(mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                  reinterpret_cast<ULONG_PTR>(dxgi_manager_.Get())))) {
    RTC_LOG(LS_ERROR) << "[hwdec] SET_D3D_MANAGER failed (no DXVA)";
    return false;
  }

  // Low-Latency fuer Echtzeit (kein B-Frame-Reordering-Puffer).
  ComPtr<ICodecAPI> codec;
  if (SUCCEEDED(mft_.As(&codec))) {
    VARIANT v;
    v.vt = VT_BOOL;
    v.boolVal = VARIANT_TRUE;
    codec->SetValue(&CODECAPI_AVLowLatencyMode, &v);
  }

  // Input = H.264. Output = NV12 (erste passende verfuegbare Output-Type).
  ComPtr<IMFMediaType> in = MakeVideoType(MFVideoFormat_H264);
  if (!in || FAILED(mft_->SetInputType(0, in.Get(), 0))) {
    RTC_LOG(LS_ERROR) << "[hwdec] SetInputType(H264) failed";
    return false;
  }
  // Output-Type jetzt versuchen; klappt es noch nicht (Bildgroesse ist erst nach
  // dem SPS bekannt), holt der erste ProcessOutput sie via STREAM_CHANGE bzw.
  // TYPE_NOT_SET nach. Daher hier bewusst NICHT fatal.
  NegotiateOutputType();

  mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  mft_configured_ = true;
  RTC_LOG(LS_INFO) << "[hwdec] D3D11VA H264 decoder configured";
  return true;
}

// ---------------------------------------------------------------------------
// Output-Type (NV12) (neu) aushandeln + coded/display-Groesse auslesen.
// ---------------------------------------------------------------------------
bool D3D11VAH264Decoder::NegotiateOutputType() {
  ComPtr<IMFMediaType> out;
  for (DWORD i = 0;; ++i) {
    ComPtr<IMFMediaType> cand;
    HRESULT hr = mft_->GetOutputAvailableType(0, i, &cand);
    if (hr == MF_E_NO_MORE_TYPES) break;
    if (FAILED(hr)) return false;
    GUID sub;
    if (SUCCEEDED(cand->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFVideoFormat_NV12) {
      out = cand;
      break;
    }
  }
  if (!out) {
    RTC_LOG(LS_ERROR) << "[hwdec] no NV12 output type";
    return false;
  }
  if (FAILED(mft_->SetOutputType(0, out.Get(), 0))) {
    RTC_LOG(LS_ERROR) << "[hwdec] SetOutputType(NV12) failed";
    return false;
  }

  UINT32 cw = 0, ch = 0;
  MFGetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, &cw, &ch);
  coded_w_ = static_cast<int>(cw);
  coded_h_ = static_cast<int>(ch);

  // Sichtbare Apertur (Crop gegen 16-Alignment-Padding). Fehlt sie, = coded.
  MFVideoArea area = {};
  UINT32 blob = 0;
  if (SUCCEEDED(out->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,
                             reinterpret_cast<UINT8*>(&area), sizeof(area), &blob)) &&
      blob >= sizeof(area)) {
    disp_x_ = area.OffsetX.value;
    disp_y_ = area.OffsetY.value;
    out_w_ = area.Area.cx;
    out_h_ = area.Area.cy;
  } else {
    disp_x_ = disp_y_ = 0;
    out_w_ = coded_w_;
    out_h_ = coded_h_;
  }
  if (out_w_ <= 0 || out_h_ <= 0) {
    out_w_ = coded_w_;
    out_h_ = coded_h_;
  }
  RTC_LOG(LS_INFO) << "[hwdec] output NV12 coded=" << coded_w_ << "x" << coded_h_
                   << " display=" << out_w_ << "x" << out_h_ << " @(" << disp_x_
                   << "," << disp_y_ << ")";
  return true;
}

// ---------------------------------------------------------------------------
// VideoProcessor + Shared-Textur-Ring (BGRA, Display-Groesse) (re)aufbauen.
// ---------------------------------------------------------------------------
bool D3D11VAH264Decoder::EnsureConverter() {
  if (conv_w_ == out_w_ && conv_h_ == out_h_ && video_processor_ && share_view_[0])
    return true;

  // Alten Ring/Prozessor verwerfen (Aufloesungswechsel).
  video_processor_.Reset();
  video_enum_.Reset();
  for (int i = 0; i < kShareRing; ++i) {
    share_view_[i].Reset();
    share_tex_[i].Reset();
    share_handle_[i] = nullptr;
  }
  share_idx_ = 0;

  // VideoProcessor: Input = coded NV12, Output = display BGRA.
  D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
  cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  cd.InputWidth = static_cast<UINT>(coded_w_);
  cd.InputHeight = static_cast<UINT>(coded_h_);
  cd.OutputWidth = static_cast<UINT>(out_w_);
  cd.OutputHeight = static_cast<UINT>(out_h_);
  cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
  if (FAILED(video_device_->CreateVideoProcessorEnumerator(&cd, &video_enum_)) ||
      FAILED(video_device_->CreateVideoProcessor(video_enum_.Get(), 0,
                                                 &video_processor_))) {
    RTC_LOG(LS_ERROR) << "[hwdec] CreateVideoProcessor failed";
    return false;
  }

  // Farbraum: YCbCr (BT.709, Studio-Range) -> RGB Full-Range. Moderne API,
  // sonst Legacy-Fallback.
  ComPtr<ID3D11VideoContext1> vctx1;
  if (SUCCEEDED(video_context_.As(&vctx1))) {
    vctx1->VideoProcessorSetStreamColorSpace1(
        video_processor_.Get(), 0, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    vctx1->VideoProcessorSetOutputColorSpace1(
        video_processor_.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
  } else {
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs = {};
    in_cs.YCbCr_Matrix = 1;   // BT.709
    in_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    video_context_->VideoProcessorSetStreamColorSpace(video_processor_.Get(), 0,
                                                      &in_cs);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs = {};
    out_cs.RGB_Range = 0;     // Full
    out_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    video_context_->VideoProcessorSetOutputColorSpace(video_processor_.Get(),
                                                      &out_cs);
  }
  // Crop auf die sichtbare Apertur.
  RECT src = {disp_x_, disp_y_, disp_x_ + out_w_, disp_y_ + out_h_};
  video_context_->VideoProcessorSetStreamSourceRect(video_processor_.Get(), 0, TRUE,
                                                    &src);

  // Shared-Textur-Ring (Display-Groesse, BGRA, Legacy-Shared) + Output-Views.
  for (int i = 0; i < kShareRing; ++i) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = static_cast<UINT>(out_w_);
    td.Height = static_cast<UINT>(out_h_);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;  // Legacy-Handle wie der Capturer
    ComPtr<IDXGIResource> res;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &share_tex_[i])) ||
        FAILED(share_tex_[i].As(&res)) ||
        FAILED(res->GetSharedHandle(&share_handle_[i])) || !share_handle_[i]) {
      RTC_LOG(LS_ERROR) << "[hwdec] shared texture[" << i << "] failed";
      return false;
    }
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ovd.Texture2D.MipSlice = 0;
    if (FAILED(video_device_->CreateVideoProcessorOutputView(
            share_tex_[i].Get(), video_enum_.Get(), &ovd, &share_view_[i]))) {
      RTC_LOG(LS_ERROR) << "[hwdec] output view[" << i << "] failed";
      return false;
    }
  }

  conv_w_ = out_w_;
  conv_h_ = out_h_;
  RTC_LOG(LS_INFO) << "[hwdec] converter ready " << out_w_ << "x" << out_h_;
  return true;
}

// ---------------------------------------------------------------------------
// NV12-Decode-Textur -> BGRA-Shared-Textur -> honeycord::D3D11FrameBuffer.
// ---------------------------------------------------------------------------
bool D3D11VAH264Decoder::EmitFrame(ID3D11Texture2D* nv12, UINT array_index,
                                   uint32_t timestamp_rtp, int64_t ntp_time_ms) {
  if (!callback_) return false;
  if (!EnsureConverter()) return false;

  // Input-View auf die richtige Array-Slice der MFT-Output-Pool-Textur.
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
  ivd.FourCC = 0;
  ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  ivd.Texture2D.MipSlice = 0;
  ivd.Texture2D.ArraySlice = array_index;
  ComPtr<ID3D11VideoProcessorInputView> in_view;
  auto _t_v = std::chrono::steady_clock::now();
  if (FAILED(video_device_->CreateVideoProcessorInputView(nv12, video_enum_.Get(),
                                                          &ivd, &in_view))) {
    RTC_LOG(LS_ERROR) << "[hwdec] input view failed";
    return false;
  }
  dbg_view_ms_ += MsSince(_t_v);

  const int idx = share_idx_;
  share_idx_ = (share_idx_ + 1) % kShareRing;

  D3D11_VIDEO_PROCESSOR_STREAM stream = {};
  stream.Enable = TRUE;
  stream.pInputSurface = in_view.Get();
  HRESULT hr = video_context_->VideoProcessorBlt(video_processor_.Get(),
                                                 share_view_[idx].Get(), 0, 1,
                                                 &stream);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "[hwdec] VideoProcessorBlt failed hr=0x" << hr;
    return false;
  }
  context_->Flush();

  auto buffer = honeycord::D3D11FrameBuffer::Create(
      device_.Get(), share_tex_[idx].Get(), out_w_, out_h_, share_handle_[idx]);
  webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
                                 .set_video_frame_buffer(buffer)
                                 .set_rtp_timestamp(timestamp_rtp)
                                 .set_ntp_time_ms(ntp_time_ms)
                                 .build();
  callback_->Decoded(frame);
  return true;
}

// ---------------------------------------------------------------------------
// Decode: H.264-Sample -> MFT -> NV12-Texturen -> EmitFrame.
// ---------------------------------------------------------------------------
int32_t D3D11VAH264Decoder::Decode(const webrtc::EncodedImage& input_image,
                                   int64_t /*render_time_ms*/) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!callback_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  if (!input_image.data() || input_image.size() == 0)
    return WEBRTC_VIDEO_CODEC_ERROR;
  if (!EnsureMft()) return WEBRTC_VIDEO_CODEC_ERROR;

  const uint32_t rtp = input_image.RtpTimestamp();
  const int64_t ntp = input_image.NtpTimeMs();

  // Input-Sample (Annex-B H.264, von webrtc) bauen.
  ComPtr<IMFSample> sample;
  ComPtr<IMFMediaBuffer> mbuf;
  if (FAILED(MFCreateSample(&sample)) ||
      FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(input_image.size()), &mbuf)))
    return WEBRTC_VIDEO_CODEC_ERROR;
  BYTE* dst = nullptr;
  DWORD maxlen = 0;
  if (FAILED(mbuf->Lock(&dst, &maxlen, nullptr))) return WEBRTC_VIDEO_CODEC_ERROR;
  memcpy(dst, input_image.data(), input_image.size());
  mbuf->Unlock();
  mbuf->SetCurrentLength(static_cast<DWORD>(input_image.size()));
  sample->AddBuffer(mbuf.Get());
  sample->SetSampleTime(RtpTo100ns(rtp));

  // ProcessInput; bei NOTACCEPTING erst Output abziehen, dann erneut.
  auto _t_pi = std::chrono::steady_clock::now();
  HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
  dbg_pi_ms_ += MsSince(_t_pi);
  if (hr == MF_E_NOTACCEPTING) {
    // wird unten nach dem Drain nochmal versucht
  } else if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "[hwdec] ProcessInput failed hr=0x" << hr;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  bool retried_input = (hr != MF_E_NOTACCEPTING);
  int renegotiations = 0;
  for (;;) {
    MFT_OUTPUT_DATA_BUFFER odb = {};
    odb.dwStreamID = 0;
    odb.pSample = nullptr;  // MFT stellt D3D11-Sample selbst (PROVIDES_SAMPLES)
    DWORD status = 0;
    auto _t_po = std::chrono::steady_clock::now();
    hr = mft_->ProcessOutput(0, 1, &odb, &status);
    dbg_po_ms_ += MsSince(_t_po);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      if (!retried_input) {
        retried_input = true;
        auto _t_pi2 = std::chrono::steady_clock::now();
        bool pi_fail = FAILED(mft_->ProcessInput(0, sample.Get(), 0));
        dbg_pi_ms_ += MsSince(_t_pi2);
        if (pi_fail) break;
        continue;
      }
      break;
    }
    // Output-Type (neu) aushandeln: nach dem SPS (erste Frames) bzw. bei
    // Aufloesungswechsel. Cap gegen eine theoretische Endlosschleife.
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE || hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
      if (++renegotiations > 4 || !NegotiateOutputType())
        return WEBRTC_VIDEO_CODEC_ERROR;
      continue;
    }
    if (FAILED(hr)) {
      RTC_LOG(LS_ERROR) << "[hwdec] ProcessOutput failed hr=0x" << hr;
      break;
    }

    // NV12-D3D11-Textur aus dem Output-Sample ziehen. Attach() uebernimmt die
    // vom MFT bereits gehaltene Ref OHNE zusaetzliches AddRef (sonst Leak).
    ComPtr<IMFSample> out;
    out.Attach(odb.pSample);
    if (odb.pEvents) odb.pEvents->Release();
    ComPtr<IMFMediaBuffer> ob;
    ComPtr<IMFDXGIBuffer> dxgi;
    if (out && SUCCEEDED(out->GetBufferByIndex(0, &ob)) && SUCCEEDED(ob.As(&dxgi))) {
      ComPtr<ID3D11Texture2D> tex;
      UINT slice = 0;
      if (SUCCEEDED(dxgi->GetResource(IID_PPV_ARGS(&tex))) &&
          SUCCEEDED(dxgi->GetSubresourceIndex(&slice))) {
        auto _t_em = std::chrono::steady_clock::now();
        EmitFrame(tex.Get(), slice, rtp, ntp);
        dbg_emit_ms_ += MsSince(_t_em);
        ++dbg_frames_;
      }
    }
  }

  ++dbg_calls_;
  if (dbg_calls_ >= 120) DbgFlush();
  return WEBRTC_VIDEO_CODEC_OK;
}

void D3D11VAH264Decoder::DbgFlush() {
  const char* base = std::getenv("LOCALAPPDATA");
  if (base && dbg_calls_ > 0) {
    std::string dir = std::string(base) + "\\HoneyCord";
    CreateDirectoryA(dir.c_str(), nullptr);
    std::string path = dir + "\\hwdec.log";
    if (FILE* f = std::fopen(path.c_str(), "a")) {
      std::fprintf(f,
                   "[hwdec %dx%d] calls=%llu frames=%llu | ProcessInput=%.1f "
                   "ms/call | ProcessOutput=%.1f ms/call | EmitFrame=%.1f ms/frame "
                   "(View=%.2f ms)\n",
                   out_w_, out_h_, dbg_calls_, dbg_frames_,
                   dbg_pi_ms_ / static_cast<double>(dbg_calls_),
                   dbg_po_ms_ / static_cast<double>(dbg_calls_),
                   dbg_frames_ ? dbg_emit_ms_ / static_cast<double>(dbg_frames_) : 0.0,
                   dbg_frames_ ? dbg_view_ms_ / static_cast<double>(dbg_frames_) : 0.0);
      std::fclose(f);
    }
  }
  dbg_calls_ = dbg_frames_ = 0;
  dbg_po_ms_ = dbg_emit_ms_ = dbg_view_ms_ = dbg_pi_ms_ = 0;
}

int32_t D3D11VAH264Decoder::Release() {
  std::lock_guard<std::mutex> lock(mutex_);
  ReleaseInternal();
  return WEBRTC_VIDEO_CODEC_OK;
}

void D3D11VAH264Decoder::ReleaseInternal() {
  if (mft_) {
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
  }
  for (int i = 0; i < kShareRing; ++i) {
    share_view_[i].Reset();
    share_tex_[i].Reset();
    share_handle_[i] = nullptr;
  }
  video_processor_.Reset();
  video_enum_.Reset();
  video_context_.Reset();
  video_device_.Reset();
  mft_.Reset();
  dxgi_manager_.Reset();
  context_.Reset();
  device_.Reset();
  if (mft_configured_) {
    MFShutdown();
    mft_configured_ = false;
  }
  conv_w_ = conv_h_ = coded_w_ = coded_h_ = out_w_ = out_h_ = 0;
}

webrtc::VideoDecoder::DecoderInfo D3D11VAH264Decoder::GetDecoderInfo() const {
  DecoderInfo info;
  info.implementation_name = kImplName;
  info.is_hardware_accelerated = true;
  return info;
}

const char* D3D11VAH264Decoder::ImplementationName() const {
  return kImplName;
}

}  // namespace libwebrtc

#endif  // _WIN32
