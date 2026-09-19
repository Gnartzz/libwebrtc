#ifndef LIBWEBRTC_LINUX_HWENC_VAAPI_ENCODER_FACTORY_H_
#define LIBWEBRTC_LINUX_HWENC_VAAPI_ENCODER_FACTORY_H_

// Die Encoder-Fabrik für Linux: H.264 über die Grafikkarte, alles andere wie
// bisher.
//
// ★ EINE HÜLLE, KEIN ERSATZ. Die Fabrik meldet exakt dieselben Formate wie die
// eingebaute — sie greift nur beim Erzeugen ein und liefert für H.264 den
// VA-API-Encoder, wenn die Hardware ihn hergibt. Damit ändert sich an der
// Aushandlung nichts: Wer kein VA-API hat, verhandelt dieselben Codecs und
// bekommt OpenH264. Eine Fabrik, die eigene Formate meldet, hätte genau die
// Klasse von Fehlern erzeugt, die sich erst beim Gegenüber zeigt.

#include <memory>
#include <string>
#include <vector>

#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"

namespace libwebrtc {

class VaapiVideoEncoderFactory : public webrtc::VideoEncoderFactory {
 public:
  VaapiVideoEncoderFactory();
  ~VaapiVideoEncoderFactory() override = default;

  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;
  std::vector<webrtc::SdpVideoFormat> GetImplementations() const override;
  webrtc::VideoEncoderFactory::CodecSupport QueryCodecSupport(
      const webrtc::SdpVideoFormat& format,
      std::optional<std::string> scalability_mode) const override;
  std::unique_ptr<webrtc::VideoEncoder> Create(
      const webrtc::Environment& env,
      const webrtc::SdpVideoFormat& format) override;

 private:
  // Erzeugt EINZELNE Encoder — H.264 auf der Grafikkarte, alles andere aus der
  // eingebauten Fabrik. Der SimulcastEncoderAdapter ruft sie je Lage auf.
  class InnenFabrik : public webrtc::VideoEncoderFactory {
   public:
    explicit InnenFabrik(webrtc::VideoEncoderFactory* intern) : intern_(intern) {}
    std::unique_ptr<webrtc::VideoEncoder> Create(
        const webrtc::Environment& env,
        const webrtc::SdpVideoFormat& format) override;
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;

   private:
    webrtc::VideoEncoderFactory* const intern_;  // gehoert der aeusseren Fabrik
  };

  // Reihenfolge zaehlt: `innen_` bekommt den Zeiger auf `intern_`.
  std::unique_ptr<webrtc::VideoEncoderFactory> intern_;
  InnenFabrik innen_;
};

// Liefert die Linux-Fabrik. Ohne brauchbare Hardware kommt die eingebaute
// zurück — der Aufrufer muss nichts unterscheiden.
std::unique_ptr<webrtc::VideoEncoderFactory> CreateLinuxVideoEncoderFactory();

// Was im Fuß des Clients stehen soll, z. B. „VA-API (libavcodec.so.61)" oder
// „Software (OpenH264)". Wird beim Start einmal protokolliert.
std::string LinuxEncoderBezeichnung();

}  // namespace libwebrtc

#endif  // LIBWEBRTC_LINUX_HWENC_VAAPI_ENCODER_FACTORY_H_
