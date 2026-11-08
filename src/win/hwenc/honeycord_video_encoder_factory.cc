#include "honeycord_video_encoder_factory.h"

#include <utility>

#include "absl/strings/match.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "rtc_base/logging.h"

#if defined(USE_NVENC) || defined(USE_AMF)
#include <dxgi.h>
#include <wrl.h>
#endif

#if defined(USE_NVENC)
#include "nvenc/nvcodec_video_encoder.h"
#endif

#if defined(USE_AMF)
#include "amf/amf_h264_encoder.h"
#endif

#if defined(USE_MFENC)
#include "api/video_codecs/video_encoder_software_fallback_wrapper.h"
#include "mf/mf_h264_encoder.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#endif

namespace libwebrtc {

namespace {

constexpr uint32_t kVendorIdNvidia = 0x10DE;
constexpr uint32_t kVendorIdAmd = 0x1002;
constexpr uint32_t kVendorIdIntel = 0x8086;

#if defined(USE_NVENC) || defined(USE_AMF)
// Iterate every DXGI adapter, skip software ones, and prefer NVIDIA over
// AMD over anything else. EnumAdapters(0) is unreliable on hybrid systems
// (Optimus / iGPU-plus-dGPU) and on machines where Windows enumerates the
// "Microsoft Basic Render Driver" as adapter[0]. Returns 0 if no real
// hardware adapter is present.
uint32_t DetectPrimaryGpuVendor() {
  using Microsoft::WRL::ComPtr;
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                reinterpret_cast<void**>(factory.GetAddressOf())))) {
    return 0;
  }
  uint32_t nvidia_vendor = 0;
  uint32_t amd_vendor = 0;
  uint32_t fallback_vendor = 0;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> a;
    if (FAILED(factory->EnumAdapters1(i, a.GetAddressOf()))) break;
    DXGI_ADAPTER_DESC1 d{};
    if (FAILED(a->GetDesc1(&d))) continue;
    char name[128];
    size_t converted = 0;
    wcstombs_s(&converted, name, d.Description, sizeof(name));
    RTC_LOG(LS_INFO) << "HoneycordVideoEncoderFactory: adapter[" << i << "] '"
                     << name << "' vendor=0x" << std::hex << d.VendorId
                     << " flags=0x" << d.Flags;
    if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
    if (d.VendorId == 0x10DE && !nvidia_vendor) nvidia_vendor = d.VendorId;
    else if (d.VendorId == 0x1002 && !amd_vendor) amd_vendor = d.VendorId;
    else if (!fallback_vendor) fallback_vendor = d.VendorId;
  }
  uint32_t picked = nvidia_vendor ? nvidia_vendor
                  : amd_vendor    ? amd_vendor
                  : fallback_vendor;
  RTC_LOG(LS_INFO) << "HoneycordVideoEncoderFactory: picked vendor=0x"
                   << std::hex << picked;
  return picked;
}
#endif

#if defined(USE_MFENC)
// Steckt irgendwo eine Intel-Grafik (nicht nur als „primär" erkannt)? Auf
// Umschalt-Laptops gewinnt NVIDIA/AMD die Wahl; scheitert deren Probe, soll
// Intel übernehmen statt Software. Und ein vorher aufgezählter virtueller
// Adapter (Hyper-V, IDD) darf Intel nicht verdecken (Prüfbefund M4).
bool IntelAdapterVorhanden() {
  using Microsoft::WRL::ComPtr;
  ComPtr<IDXGIFactory1> f;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(f.GetAddressOf()))))
    return false;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> a;
    if (FAILED(f->EnumAdapters1(i, a.GetAddressOf()))) break;
    DXGI_ADAPTER_DESC1 d{};
    if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId == 0x8086)
      return true;
  }
  return false;
}

// Abschalter: %LOCALAPPDATA%\HoneyCord\intel-encoder.txt mit „aus". Liest auch
// UTF-16 mit BOM (PowerShell 5 schreibt so) und ignoriert Groß-/Kleinschreibung.
bool IntelEncoderAbgeschaltet() {
  char basis[MAX_PATH] = {0};
  if (!GetEnvironmentVariableA("LOCALAPPDATA", basis, MAX_PATH)) return false;
  std::ifstream f(std::string(basis) + "\\HoneyCord\\intel-encoder.txt", std::ios::binary);
  if (!f) return false;
  std::string roh((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::string w;
  for (unsigned char c : roh)
    if (c >= 'A' && c <= 'z') w += static_cast<char>(std::tolower(c));
  return w.rfind("aus", 0) == 0 || w.rfind("off", 0) == 0 || (!roh.empty() && roh[0] == '0');
}
#endif

const char* VendorName(uint32_t vendor_id) {
  switch (vendor_id) {
    case kVendorIdNvidia: return "NVIDIA";
    case kVendorIdAmd:    return "AMD";
    case kVendorIdIntel:  return "Intel";
    default:              return "unknown";
  }
}

}  // namespace

HoneycordVideoEncoderFactory::HoneycordVideoEncoderFactory()
    : builtin_factory_(webrtc::CreateBuiltinVideoEncoderFactory()) {
#if defined(USE_NVENC) || defined(USE_AMF)
  gpu_vendor_id_ = DetectPrimaryGpuVendor();
#endif

#if defined(USE_NVENC)
  if (gpu_vendor_id_ == kVendorIdNvidia) {
    if (sora::NvCodecVideoEncoder::IsSupported(/*cuda_context=*/nullptr,
                                                sora::CudaVideoCodec::H264)) {
      backend_ = Backend::kNvenc;
      RTC_LOG(LS_INFO) << "HoneycordVideoEncoderFactory: NVENC enabled";
      return;
    }
    RTC_LOG(LS_WARNING)
        << "HoneycordVideoEncoderFactory: NVIDIA GPU detected but NVENC "
           "probe failed (driver too old?), falling back to software";
  }
#endif

#if defined(USE_AMF)
  if (gpu_vendor_id_ == kVendorIdAmd) {
    if (AmfH264Encoder::IsSupported()) {
      backend_ = Backend::kAmf;
      RTC_LOG(LS_INFO) << "HoneycordVideoEncoderFactory: AMF enabled (Radeon "
                          "VCN/VCE)";
      return;
    }
    RTC_LOG(LS_WARNING)
        << "HoneycordVideoEncoderFactory: AMD GPU detected but AMF probe "
           "failed (driver too old / amfrt64.dll missing?), falling back to "
           "software";
  }
#endif

#if defined(USE_MFENC)
  // #94 (November 2026): Intel-Grafik → Quick Sync über Media Foundation —
  // als primäre Karte ODER als Rückfall, wenn NVENC/AMF scheiterten.
  if (gpu_vendor_id_ == kVendorIdIntel || IntelAdapterVorhanden()) {
    if (IntelEncoderAbgeschaltet()) {
      RTC_LOG(LS_INFO) << "HoneycordVideoEncoderFactory: Intel encoder switched off (intel-encoder.txt)";
    } else if (MfH264Encoder::IsSupported()) {
      backend_ = Backend::kMf;
      RTC_LOG(LS_INFO) << "HoneycordVideoEncoderFactory: Intel Quick Sync (Media Foundation) enabled";
      return;
    }
  }
#endif

#if defined(USE_NVENC) || defined(USE_AMF)
  RTC_LOG(LS_INFO) << "HoneycordVideoEncoderFactory: " << VendorName(gpu_vendor_id_)
                   << " GPU without HW encoder pathway, software fallback";
#endif
}

HoneycordVideoEncoderFactory::~HoneycordVideoEncoderFactory() = default;

std::vector<webrtc::SdpVideoFormat>
HoneycordVideoEncoderFactory::GetSupportedFormats() const {
  // Software fallback already advertises H264/VP8/VP9; we don't need to add
  // extra entries when NVENC is the H264 producer — both encoders accept
  // the same SDP H264 formats. WebRTC negotiates by format string, then
  // routes through our Create() which picks the backend.
  return builtin_factory_->GetSupportedFormats();
}

std::unique_ptr<webrtc::VideoEncoder> HoneycordVideoEncoderFactory::Create(
    const webrtc::Environment& env,
    const webrtc::SdpVideoFormat& format) {
#if defined(USE_NVENC)
  if (backend_ == Backend::kNvenc &&
      absl::EqualsIgnoreCase(format.name, "H264")) {
    auto encoder = sora::NvCodecVideoEncoder::Create(/*cuda_context=*/nullptr,
                                                      sora::CudaVideoCodec::H264);
    if (encoder) {
      RTC_LOG(LS_INFO) << "HoneycordVideoEncoderFactory: instantiated NVENC H264 encoder";
      return encoder;
    }
    RTC_LOG(LS_WARNING)
        << "HoneycordVideoEncoderFactory: NVENC create failed, falling back to builtin";
  }
#endif
#if defined(USE_AMF)
  if (backend_ == Backend::kAmf &&
      absl::EqualsIgnoreCase(format.name, "H264")) {
    auto encoder = std::make_unique<AmfH264Encoder>();
    RTC_LOG(LS_INFO) << "HoneycordVideoEncoderFactory: instantiated AMF H264 encoder";
    return encoder;
  }
#endif
#if defined(USE_MFENC)
  if (backend_ == Backend::kMf && absl::EqualsIgnoreCase(format.name, "H264")) {
    RTC_LOG(LS_INFO) << "HoneycordVideoEncoderFactory: instantiated Intel QSV (MF) H264 encoder";
    // ★ In WebRTCs Rückfall-Hülle (Prüfbefund M1): Nur sie wertet
    // FALLBACK_SOFTWARE aus — scheitert Quick Sync beim Öffnen oder später,
    // übernimmt OpenH264 für diesen Strom, statt dass das Bild steht.
    if (auto sw = builtin_factory_->Create(env, format)) {
      return webrtc::CreateVideoEncoderSoftwareFallbackWrapper(
          env, std::move(sw), std::make_unique<MfH264Encoder>(format), /*prefer_temporal_support=*/false);
    }
    return std::make_unique<MfH264Encoder>(format);
  }
#endif
  return builtin_factory_->Create(env, format);
}

const char* HoneycordVideoEncoderFactory::selected_backend_name() const {
  switch (backend_) {
    case Backend::kNvenc:   return "nvenc";
    case Backend::kAmf:     return "amf";
    case Backend::kMf:      return "mf";
    case Backend::kBuiltin: return "builtin";
  }
  return "unknown";
}

}  // namespace libwebrtc
