#include "mf_h264_encoder.h"

#include <codecapi.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <mferror.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>

#include "api/video/i420_buffer.h"
#include "api/video_codecs/h264_profile_level_id.h"
#include "libyuv/convert_from.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"

using Microsoft::WRL::ComPtr;

namespace libwebrtc {

namespace {

// Eigene Diagnosedatei neben den anderen Encoder-Protokollen
// (%LOCALAPPDATA%\HoneyCord\mfenc.log, gedeckelt 512 KB). RTC_LOG erreicht die
// Client-Protokolle nicht zuverlässig — dieselbe Lehre wie hwenc.log unter Linux.
void Protokoll(const char* format, ...) {
  static std::mutex sperre;
  std::lock_guard<std::mutex> halt(sperre);
  char basis[MAX_PATH] = {0};
  if (!GetEnvironmentVariableA("LOCALAPPDATA", basis, MAX_PATH)) return;
  std::string pfad = std::string(basis) + "\\HoneyCord";
  CreateDirectoryA(pfad.c_str(), nullptr);
  pfad += "\\mfenc.log";
  WIN32_FILE_ATTRIBUTE_DATA fa{};
  const bool gross = GetFileAttributesExA(pfad.c_str(), GetFileExInfoStandard, &fa) &&
                     fa.nFileSizeLow > 512 * 1024;
  FILE* f = nullptr;
  if (fopen_s(&f, pfad.c_str(), gross ? "w" : "a") != 0 || !f) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  std::fprintf(f, "[%02u-%02u %02u:%02u:%02u] ", t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  va_list a;
  va_start(a, format);
  std::vfprintf(f, format, a);
  va_end(a);
  std::fputc('\n', f);
  std::fclose(f);
}

void MfStarten() {
  static std::once_flag einmal;
  std::call_once(einmal, [] { MFStartup(MF_VERSION, MFSTARTUP_LITE); });
}

// Der Intel-Hardware-Encoder (MFT) für NV12 → H.264. nullptr = keiner.
ComPtr<IMFActivate> IntelMftFinden() {
  MFT_REGISTER_TYPE_INFO ein{MFMediaType_Video, MFVideoFormat_NV12};
  MFT_REGISTER_TYPE_INFO aus{MFMediaType_Video, MFVideoFormat_H264};
  IMFActivate** liste = nullptr;
  UINT32 n = 0;
  if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                       &ein, &aus, &liste, &n))) {
    return nullptr;
  }
  ComPtr<IMFActivate> treffer;
  for (UINT32 i = 0; i < n; ++i) {
    WCHAR* hersteller = nullptr;
    UINT32 len = 0;
    if (!treffer && SUCCEEDED(liste[i]->GetAllocatedString(MFT_ENUM_HARDWARE_VENDOR_ID_Attribute,
                                                           &hersteller, &len)) &&
        hersteller && wcsstr(hersteller, L"VEN_8086")) {
      treffer = liste[i];
    }
    CoTaskMemFree(hersteller);
    liste[i]->Release();
  }
  CoTaskMemFree(liste);
  return treffer;
}

// Die Intel-Grafikkarte als D3D11-Gerät (nicht der Basic Render Driver).
bool IntelGeraet(ComPtr<ID3D11Device>* geraet, ComPtr<ID3D11DeviceContext>* kontext) {
  ComPtr<IDXGIFactory1> fabrik;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fabrik)))) return false;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> a;
    if (fabrik->EnumAdapters1(i, &a) != S_OK) break;
    DXGI_ADAPTER_DESC1 d{};
    if (FAILED(a->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || d.VendorId != 0x8086) continue;
    if (SUCCEEDED(D3D11CreateDevice(a.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                    D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                    geraet->ReleaseAndGetAddressOf(), nullptr,
                                    kontext->ReleaseAndGetAddressOf()))) {
      return true;
    }
  }
  return false;
}

// Das ausgehandelte Profil — der Empfänger hat sich darauf eingelassen.
UINT32 ProfilAus(const webrtc::SdpVideoFormat& format) {
  const auto pl = webrtc::ParseSdpForH264ProfileLevelId(format.parameters);
  if (!pl) return eAVEncH264VProfile_ConstrainedBase;
  switch (pl->profile) {
    case webrtc::H264Profile::kProfileConstrainedBaseline: return eAVEncH264VProfile_ConstrainedBase;
    case webrtc::H264Profile::kProfileBaseline: return eAVEncH264VProfile_Base;
    case webrtc::H264Profile::kProfileMain: return eAVEncH264VProfile_Main;
    case webrtc::H264Profile::kProfileConstrainedHigh:
    case webrtc::H264Profile::kProfileHigh:
    case webrtc::H264Profile::kProfilePredictiveHigh444: return eAVEncH264VProfile_High;
  }
  return eAVEncH264VProfile_ConstrainedBase;
}

bool IstSchluessel(const uint8_t* p, size_t n) {
  for (size_t i = 0; i + 4 < n; ++i) {
    if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && (p[i + 3] & 0x1f) == 5) return true;
  }
  return false;
}

}  // namespace

// ── Verfügbar? ──────────────────────────────────────────────────────────────
bool MfH264Encoder::IsSupported() {
  static const bool ja = [] {
    MfStarten();
    if (!IntelMftFinden()) {
      Protokoll("Probe: kein Intel-Hardware-Encoder (MFT) gefunden");
      return false;
    }
    MfH264Encoder probe(webrtc::SdpVideoFormat("H264"));
    probe.width_ = 640;
    probe.height_ = 360;
    probe.target_bps_ = 800000;
    probe.encoder_bps_ = 800000;
    const bool ok = probe.Oeffnen() == WEBRTC_VIDEO_CODEC_OK;
    probe.Schliessen();
    Protokoll("Probe: Intel Quick Sync %s", ok ? "verfuegbar" : "laesst sich nicht oeffnen");
    return ok;
  }();
  return ja;
}

MfH264Encoder::MfH264Encoder(const webrtc::SdpVideoFormat& format)
    : bitrate_adjuster_(0.5, 0.95), format_(format) {}

MfH264Encoder::~MfH264Encoder() { Release(); }

int32_t MfH264Encoder::InitEncode(const webrtc::VideoCodec* codec_settings,
                                  int32_t /*number_of_cores*/,
                                  size_t /*max_payload_size*/) {
  if (!codec_settings || codec_settings->width < 16 || codec_settings->height < 16) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  Release();
  std::lock_guard<std::mutex> halt(mutex_);
  width_ = codec_settings->width & ~1u;
  height_ = codec_settings->height & ~1u;
  framerate_ = codec_settings->maxFramerate ? codec_settings->maxFramerate : 30;
  target_bps_ = std::max<uint32_t>(codec_settings->startBitrate * 1000, 100000);
  screensharing_ = codec_settings->mode == webrtc::VideoCodecMode::kScreensharing;
  bitrate_adjuster_.SetTargetBitrateBps(target_bps_);
  encoder_bps_ = bitrate_adjuster_.GetAdjustedBitrateBps();
  frame_index_ = 0;
  stempel_.clear();
  offen_seit_ms_ = webrtc::TimeMillis();   // Anlauf zählt ab InitEncode, nicht je Neuaufsetzen
  return Oeffnen();
}

int32_t MfH264Encoder::RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) {
  std::lock_guard<std::mutex> halt(mutex_);
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MfH264Encoder::Release() {
  std::lock_guard<std::mutex> halt(mutex_);
  Schliessen();
  return WEBRTC_VIDEO_CODEC_OK;
}

// ── Öffnen/Schließen (unter mutex_) ────────────────────────────────────────
bool MfH264Encoder::WertSetzen(const GUID& was, uint32_t wert) {
  if (!codec_) return false;
  VARIANT v;
  VariantInit(&v);
  v.vt = VT_UI4;
  v.ulVal = wert;
  return SUCCEEDED(codec_->SetValue(&was, &v));
}

bool MfH264Encoder::RateSetzen(uint32_t bps) {
  // ★ Spitzen-VBR: Mittel und Spitze gemeinsam, sonst folgt der MFT nicht
  // (gemessen: CBR ignoriert eine neue Rate im laufenden Strom).
  const bool a = WertSetzen(CODECAPI_AVEncCommonMaxBitRate, bps);
  const bool b = WertSetzen(CODECAPI_AVEncCommonMeanBitRate, bps);
  if (a && b) encoder_bps_ = bps;
  return a && b;
}

// ★ GEMESSEN 08.11.2026: Jede Ratenänderung am Intel-MFT setzt ein
// Schlüsselbild (der Ausgleicher stellte jede Sekunde nach → vier ungefragte
// IDR in 20 s). Deshalb Regeln wie beim VA-API-Encoder unter Linux:
//   - Anlauf (erste 30 s): ab 10 % Abweichung, höchstens alle 2 s;
//   - danach: ab 25 %, höchstens alle 3 s;
//   - Einbruch um 40 % oder mehr: sofort (Stau, kein Warten).
void MfH264Encoder::RateNachRegeln(uint32_t soll, int64_t jetzt, const char* anlass) {
  if (!mft_ || soll == 0 || encoder_bps_ == 0) return;
  const bool anlauf = jetzt - offen_seit_ms_ < 30000;
  const double q = static_cast<double>(soll) / encoder_bps_;
  const bool einbruch = q <= 0.6;
  const double schwelle = anlauf ? 0.10 : 0.25;
  const int64_t sperre = anlauf ? 2000 : 3000;
  if (!einbruch && (q < 1.0 + schwelle && q > 1.0 - schwelle)) return;
  // Auch ein Einbruch höchstens einmal je Sekunde: fällt die Rate im Stau
  // Schritt für Schritt, gäbe es sonst ein Schlüsselbild je Sekunde (Prüfbefund).
  if (jetzt - letzte_rate_ms_ < (einbruch ? 1000 : sperre)) return;
  const uint32_t alt = encoder_bps_;
  if (RateSetzen(soll)) {
    letzte_rate_ms_ = jetzt;
    Protokoll("Rate %u -> %u kbit/s (%s%s)", alt / 1000, soll / 1000, anlass,
              einbruch ? ", Einbruch" : anlauf ? ", Anlauf" : "");
  }
}

bool MfH264Encoder::AusgabeTypSetzen() {
  ComPtr<IMFMediaType> t;
  if (FAILED(mft_->GetOutputAvailableType(0, 0, &t))) return false;
  const HRESULT hr = mft_->SetOutputType(0, t.Get(), 0);
  Protokoll("Ausgabeformat neu bestaetigt (0x%08lx)", (unsigned long)hr);
  return SUCCEEDED(hr);
}

int32_t MfH264Encoder::Oeffnen() {
  MfStarten();
  ComPtr<IMFActivate> akt = IntelMftFinden();
  if (!akt) return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  if (!IntelGeraet(&device_, &context_)) {
    Protokoll("FEHLER: kein D3D11-Geraet auf der Intel-Grafik");
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }
  // Der MFT benutzt das Gerät aus eigenen Fäden.
  ComPtr<ID3D11Multithread> mt;
  if (SUCCEEDED(device_.As(&mt))) mt->SetMultithreadProtected(TRUE);
  if (FAILED(MFCreateDXGIDeviceManager(&manager_token_, &manager_)) ||
      FAILED(manager_->ResetDevice(device_.Get(), manager_token_))) {
    Protokoll("FEHLER: DXGI-Geraetemanager");
    Schliessen();
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }
  if (FAILED(akt->ActivateObject(IID_PPV_ARGS(&mft_)))) {
    Protokoll("FEHLER: MFT laesst sich nicht aktivieren");
    Schliessen();
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }
  ComPtr<IMFAttributes> at;
  bool async = false;
  if (SUCCEEDED(mft_->GetAttributes(&at)) && at) {
    UINT32 a = 0;
    if (SUCCEEDED(at->GetUINT32(MF_TRANSFORM_ASYNC, &a)) && a) {
      async = true;
      at->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    }
    at->SetUINT32(MF_LOW_LATENCY, TRUE);
  }
  if (!async) {
    // Gemessen ist nur der asynchrone Intel-MFT. Ein synchroner bräuchte
    // eine andere Schleife — lieber Software als ungeprüft.
    Protokoll("FEHLER: MFT ist nicht asynchron");
    Schliessen();
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }
  // ★ Gemessen: ohne Gerätemanager nimmt der Intel-MFT ein Bild an und verstummt.
  if (FAILED(mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager_.Get())))) {
    Protokoll("FEHLER: SET_D3D_MANAGER");
    Schliessen();
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }
  mft_.As(&codec_);
  WertSetzen(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_PeakConstrainedVBR);
  RateSetzen(encoder_bps_);
  WertSetzen(CODECAPI_AVLowLatencyMode, 1);
  WertSetzen(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
  // Schlüsselbilder kommen auf Anforderung (PLI/FIR); dazwischen eines alle
  // fünf Minuten als Rückfall.
  WertSetzen(CODECAPI_AVEncMPVGOPSize, framerate_ * 300);

  ComPtr<IMFMediaType> aus;
  MFCreateMediaType(&aus);
  aus->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  aus->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  aus->SetUINT32(MF_MT_AVG_BITRATE, encoder_bps_);
  MFSetAttributeSize(aus.Get(), MF_MT_FRAME_SIZE, width_, height_);
  MFSetAttributeRatio(aus.Get(), MF_MT_FRAME_RATE, framerate_, 1);
  MFSetAttributeRatio(aus.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  aus->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  aus->SetUINT32(MF_MT_MPEG2_PROFILE, ProfilAus(format_));
  HRESULT hr = mft_->SetOutputType(0, aus.Get(), 0);
  if (FAILED(hr)) {
    Protokoll("FEHLER: SetOutputType %ux%u (0x%08lx)", width_, height_, (unsigned long)hr);
    Schliessen();
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }
  ComPtr<IMFMediaType> ein;
  MFCreateMediaType(&ein);
  ein->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  ein->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  MFSetAttributeSize(ein.Get(), MF_MT_FRAME_SIZE, width_, height_);
  MFSetAttributeRatio(ein.Get(), MF_MT_FRAME_RATE, framerate_, 1);
  MFSetAttributeRatio(ein.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  ein->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  hr = mft_->SetInputType(0, ein.Get(), 0);
  if (FAILED(hr)) {
    Protokoll("FEHLER: SetInputType (0x%08lx)", (unsigned long)hr);
    Schliessen();
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }

  for (auto& t : textur_) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width_;
    td.Height = height_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &t))) {
      Protokoll("FEHLER: NV12-Textur %ux%u", width_, height_);
      Schliessen();
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }
  }
  nv12_.assign(static_cast<size_t>(width_) * height_ * 3 / 2, 0);

  if (FAILED(mft_.As(&events_))) {
    Schliessen();
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }
  {
    std::lock_guard<std::mutex> l(ereignis_mutex_);
    ereignisse_.clear();
    ereignis_ende_ = false;
  }
  ereignis_faden_ = std::thread([this, gen = events_] {
    for (;;) {
      ComPtr<IMFMediaEvent> ev;
      const HRESULT r = gen->GetEvent(0, &ev);
      std::lock_guard<std::mutex> l(ereignis_mutex_);
      if (FAILED(r)) {
        ereignis_ende_ = true;
        ereignis_cv_.notify_all();
        return;
      }
      MediaEventType typ = MEUnknown;
      HRESULT st = S_OK;
      ev->GetType(&typ);
      ev->GetStatus(&st);
      ereignisse_.emplace_back(typ, st);
      ereignis_cv_.notify_all();
    }
  });
  mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  eingabe_frei_ = 0;
  fenster_start_ms_ = 0;
  fenster_bytes_ = 0;
  ueber_folge_ = 0;
  letztes_aufsetzen_ms_ = webrtc::TimeMillis();
  letzte_rate_ms_ = letztes_aufsetzen_ms_;
  zeitueberschreitungen_ = 0;
  erste_zeit_us_ = -1;
  letzte_probe_zeit_ = -1;
  Protokoll("offen: %ux%u @%u  Ziel %u kbit/s (WebRTC %u)  %s  Profil %u", width_, height_, framerate_,
            encoder_bps_ / 1000, target_bps_ / 1000, screensharing_ ? "Bildschirm" : "Kamera",
            ProfilAus(format_));
  return WEBRTC_VIDEO_CODEC_OK;
}

void MfH264Encoder::Schliessen() {
  bool beendet = true;
  if (mft_) {
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    // Shutdown löst das blockierende GetEvent im Ereignisfaden (MF_E_SHUTDOWN).
    ComPtr<IMFShutdown> aus;
    beendet = SUCCEEDED(mft_.As(&aus)) && SUCCEEDED(aus->Shutdown());
  }
  if (ereignis_faden_.joinable()) {
    // Ohne Shutdown käme GetEvent nie zurück — dann nicht warten (der Faden
    // hält seinen eigenen Verweis auf den Ereignisgeber).
    if (beendet) ereignis_faden_.join(); else ereignis_faden_.detach();
  }
  events_.Reset();
  codec_.Reset();
  mft_.Reset();
  for (auto& t : textur_) t.Reset();
  manager_.Reset();
  context_.Reset();
  device_.Reset();
  stempel_.clear();
}

bool MfH264Encoder::EreignisHolen(MediaEventType* typ, HRESULT* status, int ms) {
  std::unique_lock<std::mutex> l(ereignis_mutex_);
  if (!ereignis_cv_.wait_for(l, std::chrono::milliseconds(ms),
                             [this] { return !ereignisse_.empty() || ereignis_ende_; })) {
    return false;
  }
  if (ereignisse_.empty()) return false;   // Ende
  *typ = ereignisse_.front().first;
  *status = ereignisse_.front().second;
  ereignisse_.pop_front();
  return true;
}

// ── Kodieren ────────────────────────────────────────────────────────────────
int32_t MfH264Encoder::Encode(const webrtc::VideoFrame& frame,
                              const std::vector<webrtc::VideoFrameType>* frame_types) {
  std::lock_guard<std::mutex> halt(mutex_);
  if (!callback_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;

  webrtc::scoped_refptr<webrtc::I420BufferInterface> i420 = frame.video_frame_buffer()->ToI420();
  if (!i420) return WEBRTC_VIDEO_CODEC_ERROR;
  const uint32_t w = static_cast<uint32_t>(i420->width()) & ~1u;
  const uint32_t h = static_cast<uint32_t>(i420->height()) & ~1u;
  if (w != width_ || h != height_) {
    Protokoll("Groesse %ux%u -> %ux%u, neu aufsetzen", width_, height_, w, h);
    width_ = w;
    height_ = h;
    neu_aufsetzen_ = true;
  }
  if (neu_aufsetzen_ || !mft_) {
    neu_aufsetzen_ = false;
    Schliessen();
    if (const uint32_t a = bitrate_adjuster_.GetAdjustedBitrateBps()) encoder_bps_ = a;
    // Scheitert das Öffnen, übernimmt die Rückfall-Hülle der Fabrik
    // (VideoEncoderSoftwareFallbackWrapper → OpenH264) — dauerhaft.
    if (Oeffnen() != WEBRTC_VIDEO_CODEC_OK) return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }

  bool schluessel = keyframe_angefordert_;
  if (frame_types) {
    for (auto t : *frame_types) schluessel |= t == webrtc::VideoFrameType::kVideoFrameKey;
  }
  if (schluessel) {
    WertSetzen(CODECAPI_AVEncVideoForceKeyFrame, 1);
    keyframe_angefordert_ = false;
  }

  libyuv::I420ToNV12(i420->DataY(), i420->StrideY(), i420->DataU(), i420->StrideU(), i420->DataV(),
                     i420->StrideV(), nv12_.data(), width_, nv12_.data() + width_ * height_, width_,
                     width_, height_);
  textur_index_ = (textur_index_ + 1) % 4;
  ID3D11Texture2D* tex = textur_[textur_index_].Get();
  context_->UpdateSubresource(tex, 0, nullptr, nv12_.data(), width_, 0);

  ComPtr<IMFSample> probe;
  ComPtr<IMFMediaBuffer> puffer;
  if (FAILED(MFCreateSample(&probe)) ||
      FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), tex, 0, FALSE, &puffer))) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  puffer->SetCurrentLength(static_cast<DWORD>(nv12_.size()));
  probe->AddBuffer(puffer.Get());
  // ★ Echte Aufnahmezeit als Sample-Zeit (Prüfbefund M3): Die
  // Ratensteuerung verteilt sonst Bits nach der deklarierten Bildrate und
  // liefert zu wenig, wenn weniger Bilder kommen (Bildschirm 5–15 fps).
  // Streng steigend, und ohne Zeitstempel die laufende Nummer.
  const LONGLONG dauer = 10000000LL / std::max<uint32_t>(framerate_, 1);
  const int64_t nummer = frame_index_++;
  LONGLONG zeit = nummer * dauer;
  if (frame.timestamp_us() > 0) {
    if (erste_zeit_us_ < 0) erste_zeit_us_ = frame.timestamp_us();
    zeit = (frame.timestamp_us() - erste_zeit_us_) * 10;
  }
  if (zeit <= letzte_probe_zeit_) zeit = letzte_probe_zeit_ + 1;
  // Die Dauer ist der echte Abstand zum vorigen Bild (gedeckelt auf 1 s) —
  // gemessen 08.11.: mit fester 1/fps verteilt der MFT die Rate auf die
  // angemeldete Bildrate und liefert bei 10 statt 30 Bildern/s ein Drittel.
  LONGLONG abstand = letzte_probe_zeit_ >= 0 ? zeit - letzte_probe_zeit_ : dauer;
  if (abstand <= 0 || abstand > 10000000LL) abstand = dauer;
  letzte_probe_zeit_ = zeit;
  probe->SetSampleTime(zeit);
  probe->SetSampleDuration(abstand);
  stempel_[zeit] = Stempel{frame.rtp_timestamp(), frame.ntp_time_ms(), frame.render_time_ms(),
                           frame.rotation()};

  // Auf die Freigabe zur Eingabe warten (kommt praktisch sofort).
  const int64_t start = webrtc::TimeMillis();
  while (eingabe_frei_ == 0) {
    MediaEventType typ = MEUnknown;
    HRESULT st = S_OK;
    const int rest = static_cast<int>(100 - (webrtc::TimeMillis() - start));
    if (rest <= 0 || !EreignisHolen(&typ, &st, rest)) {
      // Kein Platz im Encoder: dieses Bild fallen lassen statt zu stauen —
      // und WebRTC sagen, dass es verworfen ist (Prüfbefund M2).
      stempel_.erase(zeit);
      callback_->OnDroppedFrame(webrtc::EncodedImageCallback::DropReason::kDroppedByEncoder);
      bool ende = false;
      {
        std::lock_guard<std::mutex> l(ereignis_mutex_);
        ende = ereignis_ende_;
      }
      if (++zeitueberschreitungen_ >= 5 || ende) {
        Protokoll("Encoder antwortet nicht (%d Bilder in Folge%s), neu aufsetzen", zeitueberschreitungen_,
                  ende ? ", Ereignisfaden beendet" : "");
        neu_aufsetzen_ = true;
      }
      return WEBRTC_VIDEO_CODEC_OK;
    }
    if (FAILED(st)) {
      Protokoll("FEHLER: Encoder meldet 0x%08lx, neu aufsetzen", (unsigned long)st);
      neu_aufsetzen_ = true;
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    if (typ == METransformNeedInput) ++eingabe_frei_;
    else if (typ == METransformHaveOutput) AusgabeAbholen();
  }
  HRESULT hr = mft_->ProcessInput(0, probe.Get(), 0);
  --eingabe_frei_;
  zeitueberschreitungen_ = 0;
  if (FAILED(hr)) {
    Protokoll("FEHLER: ProcessInput 0x%08lx", (unsigned long)hr);
    stempel_.erase(zeit);
    neu_aufsetzen_ = true;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  // Auf das fertige Bild warten — wie NVENC im Sync-Modus. Bei 720p ~15 ms.
  bool geliefert = false;
  while (!geliefert) {
    MediaEventType typ = MEUnknown;
    HRESULT st = S_OK;
    const int rest = static_cast<int>(200 - (webrtc::TimeMillis() - start));
    if (rest <= 0 || !EreignisHolen(&typ, &st, rest)) break;   // kommt beim nächsten Bild
    if (FAILED(st)) {
      Protokoll("FEHLER: Encoder meldet 0x%08lx, neu aufsetzen", (unsigned long)st);
      neu_aufsetzen_ = true;
      break;
    }
    if (typ == METransformNeedInput) ++eingabe_frei_;
    else if (typ == METransformHaveOutput) geliefert = AusgabeAbholen();
  }
  // Was sich angestaut hat (eine verspätete Ausgabe), gleich mitnehmen —
  // sonst bliebe der Strom dauerhaft ein Bild zurück (Prüfbefund).
  EreignisseAbarbeiten();
  return WEBRTC_VIDEO_CODEC_OK;
}

void MfH264Encoder::EreignisseAbarbeiten() {
  MediaEventType typ = MEUnknown;
  HRESULT st = S_OK;
  while (EreignisHolen(&typ, &st, 0)) {
    if (FAILED(st)) {
      Protokoll("FEHLER: Encoder meldet 0x%08lx, neu aufsetzen", (unsigned long)st);
      neu_aufsetzen_ = true;
      return;
    }
    if (typ == METransformNeedInput) ++eingabe_frei_;
    else if (typ == METransformHaveOutput) AusgabeAbholen();
  }
}

bool MfH264Encoder::AusgabeAbholen() {
  MFT_OUTPUT_STREAM_INFO info{};
  mft_->GetOutputStreamInfo(0, &info);
  const bool liefert = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                        MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
  MFT_OUTPUT_DATA_BUFFER ob{};
  ComPtr<IMFSample> eigen;
  if (!liefert) {
    ComPtr<IMFMediaBuffer> b;
    MFCreateSample(&eigen);
    MFCreateMemoryBuffer(info.cbSize ? info.cbSize : width_ * height_ * 2, &b);
    eigen->AddBuffer(b.Get());
    ob.pSample = eigen.Get();
  }
  DWORD status = 0;
  const HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
  if (ob.pEvents) ob.pEvents->Release();
  ComPtr<IMFSample> s;
  if (ob.pSample) {
    s = ob.pSample;
    if (liefert) ob.pSample->Release();
  }
  if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
    // ★ Gemessen: Intels MFT meldet das beim ersten Bild; erst nach dem
    // Bestätigen gibt er Bilder heraus.
    AusgabeTypSetzen();
    return false;
  }
  if (FAILED(hr) || !s) return false;

  LONGLONG zeit = 0;
  s->GetSampleTime(&zeit);
  Stempel st;
  auto it = stempel_.find(zeit);
  if (it != stempel_.end()) st = it->second;
  stempel_.erase(stempel_.begin(), stempel_.upper_bound(zeit));

  ComPtr<IMFMediaBuffer> buf;
  if (FAILED(s->ConvertToContiguousBuffer(&buf))) return false;
  BYTE* p = nullptr;
  DWORD len = 0;
  if (FAILED(buf->Lock(&p, nullptr, &len)) || !p || !len) return false;
  auto paket = webrtc::EncodedImageBuffer::Create(p, len);
  const bool key = IstSchluessel(p, len);
  buf->Unlock();

  encoded_image_.SetEncodedData(paket);
  encoded_image_._encodedWidth = width_;
  encoded_image_._encodedHeight = height_;
  encoded_image_.content_type_ = screensharing_ ? webrtc::VideoContentType::SCREENSHARE
                                                : webrtc::VideoContentType::UNSPECIFIED;
  encoded_image_.timing_.flags = webrtc::VideoSendTiming::kInvalid;
  encoded_image_.SetRtpTimestamp(st.rtp);
  encoded_image_.ntp_time_ms_ = st.ntp;
  encoded_image_.capture_time_ms_ = st.render;
  encoded_image_.rotation_ = st.rotation;
  encoded_image_._frameType = key ? webrtc::VideoFrameType::kVideoFrameKey
                                  : webrtc::VideoFrameType::kVideoFrameDelta;
  h264_parser_.ParseBitstream(encoded_image_);
  encoded_image_.qp_ = h264_parser_.GetLastSliceQp().value_or(-1);

  webrtc::CodecSpecificInfo info2;
  info2.codecType = webrtc::kVideoCodecH264;
  info2.codecSpecific.H264.packetization_mode = webrtc::H264PacketizationMode::NonInterleaved;
  const auto r = callback_->OnEncodedImage(encoded_image_, &info2);
  if (r.error != webrtc::EncodedImageCallback::Result::OK) {
    RTC_LOG(LS_ERROR) << "MfH264Encoder: OnEncodedImage error=" << r.error;
  }
  bitrate_adjuster_.Update(len);
  RateUeberwachen(len, webrtc::TimeMillis());
  return true;
}

// ── Rate ────────────────────────────────────────────────────────────────────
void MfH264Encoder::RateUeberwachen(size_t bytes, int64_t jetzt) {
  if (fenster_start_ms_ == 0) fenster_start_ms_ = jetzt;
  fenster_bytes_ += bytes;
  const int64_t dauer = jetzt - fenster_start_ms_;
  if (dauer < 1000) return;
  const uint64_t bps = fenster_bytes_ * 8 * 1000 / static_cast<uint64_t>(dauer);
  fenster_start_ms_ = jetzt;
  fenster_bytes_ = 0;

  // Der Ausgleicher passt sein Ziel laufend an; der MFT folgt nur nach den
  // Regeln (jede Änderung kostet ein Schlüsselbild).
  RateNachRegeln(bitrate_adjuster_.GetAdjustedBitrateBps(), jetzt, "Ausgleich");

  // ★ Gemessen: bei 60 Bildern/s folgt der MFT einer neuen Rate nicht immer.
  // Liegt die Ausgabe drei Sekunden in Folge über dem 1,5-Fachen, wird neu
  // aufgesetzt — höchstens alle 10 s (ein Neuaufsetzen kostet ein Schlüsselbild).
  // Weniger als das Ziel ist normal (ruhiges Bild) und kein Grund.
  if (encoder_bps_ > 0 && bps > static_cast<uint64_t>(encoder_bps_) * 3 / 2) {
    ++ueber_folge_;
  } else {
    ueber_folge_ = 0;
  }
  if (ueber_folge_ >= 3 && jetzt - letztes_aufsetzen_ms_ > 10000) {
    Protokoll("Rate: Ausgabe %llu kbit/s bei Ziel %u kbit/s seit %d s, neu aufsetzen",
              (unsigned long long)(bps / 1000), encoder_bps_ / 1000, ueber_folge_);
    ueber_folge_ = 0;
    neu_aufsetzen_ = true;
  }
}

void MfH264Encoder::SetRates(const webrtc::VideoEncoder::RateControlParameters& parameters) {
  std::lock_guard<std::mutex> halt(mutex_);
  const uint32_t neu = parameters.bitrate.get_sum_bps();
  if (neu == 0) return;
  const uint32_t alt = target_bps_;
  target_bps_ = neu;
  bitrate_adjuster_.SetTargetBitrateBps(neu);
  RateNachRegeln(bitrate_adjuster_.GetAdjustedBitrateBps(), webrtc::TimeMillis(), "WebRTC");
  if (alt == 0 || neu > alt * 11 / 10 || neu < alt * 9 / 10) {
    Protokoll("SetRates: WebRTC %u kbit/s @%.1f -> Encoder %u kbit/s", neu / 1000,
              parameters.framerate_fps, encoder_bps_ / 1000);
  }
}

webrtc::VideoEncoder::EncoderInfo MfH264Encoder::GetEncoderInfo() const {
  webrtc::VideoEncoder::EncoderInfo info;
  info.supports_native_handle = false;   // I420 im Arbeitsspeicher, wie AMF
  info.implementation_name = "Intel QSV (MF)";
  info.is_hardware_accelerated = true;
  info.scaling_settings = webrtc::VideoEncoder::ScalingSettings(28, 38);
  info.requested_resolution_alignment = 2;   // NV12 und H.264 wollen gerade Maße
  return info;
}

}  // namespace libwebrtc
