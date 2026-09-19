#include "src/linux/hwenc/vaapi_encoder_factory.h"

#include "absl/strings/match.h"
#include "media/base/media_constants.h"
#include "media/engine/internal_encoder_factory.h"
#include "media/engine/simulcast_encoder_adapter.h"
#include "rtc_base/logging.h"
#include "src/linux/hwenc/vaapi/vaapi_h264_encoder.h"

namespace libwebrtc {

// ── Die innere Fabrik ───────────────────────────────────────────────────────
//
// ★ SIE ERZEUGT NACKTE ENCODER, und das ist der springende Punkt: Der
// SimulcastEncoderAdapter ruft sie EINMAL JE LAGE auf. Gäbe sie selbst schon
// wieder einen Adapter zurück, hätten wir Adapter in Adapter.
std::unique_ptr<webrtc::VideoEncoder> VaapiVideoEncoderFactory::InnenFabrik::Create(
    const webrtc::Environment& env,
    const webrtc::SdpVideoFormat& format) {
  if (absl::EqualsIgnoreCase(format.name, webrtc::kH264CodecName) &&
      VaapiH264Encoder::IstVerfuegbar()) {
    return std::make_unique<VaapiH264Encoder>(format);
  }
  return intern_->Create(env, format);
}

std::vector<webrtc::SdpVideoFormat>
VaapiVideoEncoderFactory::InnenFabrik::GetSupportedFormats() const {
  return intern_->GetSupportedFormats();
}

// ── Die äußere Fabrik ───────────────────────────────────────────────────────
VaapiVideoEncoderFactory::VaapiVideoEncoderFactory()
    : intern_(std::make_unique<webrtc::InternalEncoderFactory>()),
      innen_(intern_.get()) {}

std::vector<webrtc::SdpVideoFormat> VaapiVideoEncoderFactory::GetSupportedFormats()
    const {
  return intern_->GetSupportedFormats();
}

std::vector<webrtc::SdpVideoFormat> VaapiVideoEncoderFactory::GetImplementations()
    const {
  return intern_->GetImplementations();
}

webrtc::VideoEncoderFactory::CodecSupport
VaapiVideoEncoderFactory::QueryCodecSupport(
    const webrtc::SdpVideoFormat& format,
    std::optional<std::string> scalability_mode) const {
  return intern_->QueryCodecSupport(format, scalability_mode);
}

std::unique_ptr<webrtc::VideoEncoder> VaapiVideoEncoderFactory::Create(
    const webrtc::Environment& env,
    const webrtc::SdpVideoFormat& format) {
  // ★ GENAU DER AUFBAU DER EINGEBAUTEN FABRIK — nur mit unserer inneren
  // Fabrik davor. Das ist kein Beiwerk, sondern trägt zwei Dinge, die uns
  // sonst fehlten (Prüfbefund 19.09.2026):
  //
  //  1. SIMULCAST. LiveKit veröffentlicht die Kamera in drei Lagen. Ohne den
  //     Adapter entstünde nur ein Strom ohne Lagen-Index — die Empfänger der
  //     übrigen Lagen bekämen nichts. Hat die Verbindung nur eine Lage,
  //     schaltet der Adapter selbst auf Durchreichen.
  //  2. DEN RÜCKFALL. Scheitert `InitEncode` auf dem VA-API-Encoder, nimmt der
  //     Adapter die Ersatz-Fabrik. Und dieser Fall ist real: Unsere Probe
  //     öffnet nur das Gerät — ob der Treiber DIESE Auflösung und DIESES
  //     Profil hergibt, weiß man erst beim Öffnen. Ohne Rückfall bliebe das
  //     Bild schwarz.
  if (format.IsCodecInList(intern_->GetSupportedFormats())) {
    return std::make_unique<webrtc::SimulcastEncoderAdapter>(
        env, /*primary_factory=*/&innen_, /*fallback_factory=*/intern_.get(),
        format);
  }
  return nullptr;
}

std::unique_ptr<webrtc::VideoEncoderFactory> CreateLinuxVideoEncoderFactory() {
  // ★ Die Probe einmal beim Aufbau, nicht beim ersten Anruf: So steht im
  // Protokoll VOR dem ersten Gespräch, woran es lag — und nicht erst, wenn
  // sich jemand fragt, warum die Last hoch ist.
  RTC_LOG(LS_INFO) << "[vaapi] Video-Encoder: " << LinuxEncoderBezeichnung();
  return std::make_unique<VaapiVideoEncoderFactory>();
}

std::string LinuxEncoderBezeichnung() {
  const std::string name = VaapiH264Encoder::BackendName();
  return name.empty() ? "Software (OpenH264)" : name;
}

}  // namespace libwebrtc
