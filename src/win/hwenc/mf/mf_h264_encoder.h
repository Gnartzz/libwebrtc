#ifndef LIBWEBRTC_WIN_HWENC_MF_H264_ENCODER_H_
#define LIBWEBRTC_WIN_HWENC_MF_H264_ENCODER_H_

// HoneyCord H.264-Encoder für Intel-Grafik unter Windows (#94, November 2026):
// Intels „Quick Sync Video H.264 Encoder MFT“ über Media Foundation.
//
// ★ WARUM MEDIA FOUNDATION: Der Encoder kommt mit dem Intel-Grafiktreiber,
// ohne SDK und ohne Laufzeitbibliothek von uns — wie AMF bei AMD. Das alte
// Intel Media SDK (src/win/msdk*) ist abgekündigt.
//
// ★ GEMESSEN in der Mess-VM hc-win-intel (UHD 630 über GVT-g, Treiber
// 31.0.101.2115, 08.11.2026):
//   - Der MFT ist asynchron und braucht einen D3D11-Gerätemanager; ohne ihn
//     nimmt er ein Bild an und verstummt.
//   - Beim ersten Bild meldet ProcessOutput MF_E_TRANSFORM_STREAM_CHANGE;
//     erst nach erneutem SetOutputType gibt er Bilder heraus.
//   - 720p30: ~15 ms je Bild, CPU ~0, Schlüsselbild auf Anforderung sofort.
//   - CBR ignoriert eine neue Rate im laufenden Strom. Spitzen-VBR folgt bei
//     30 Bildern/s (~20 % darüber), bei 60 nur teilweise. Deshalb: VBR mit
//     laufender Nachführung UND Neuaufsetzen bei anhaltender Übererfüllung
//     (wie der Linux-Encoder, ratenregeln.h).
//
// Arbeitsweise wie NVENC im Sync-Modus: Encode() gibt ein Bild hinein und
// wartet auf das fertige (bei 720p ~15 ms). Die Ereignisse des asynchronen
// MFT holt ein eigener Faden ab.

#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wrl.h>

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "common_video/h264/h264_bitstream_parser.h"
#include "common_video/include/bitrate_adjuster.h"

namespace libwebrtc {

class MfH264Encoder : public webrtc::VideoEncoder {
 public:
  // Wahr, wenn ein Intel-Hardware-H.264-Encoder (MFT) da ist UND sich mit
  // D3D11-Gerätemanager und unseren Typen öffnen lässt. Einmal geprüft.
  static bool IsSupported();

  explicit MfH264Encoder(const webrtc::SdpVideoFormat& format);
  ~MfH264Encoder() override;

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
  struct Stempel {
    uint32_t rtp = 0;
    int64_t ntp = 0;
    int64_t render = 0;
    webrtc::VideoRotation rotation = webrtc::kVideoRotation_0;
  };

  int32_t Oeffnen();       // MFT, Gerät, Typen, Ereignisfaden
  void Schliessen();
  bool RateSetzen(uint32_t bps);
  bool WertSetzen(const GUID& was, uint32_t wert);
  bool AusgabeTypSetzen();
  // Wartet auf ein Ereignis des MFT (höchstens `ms`). false = Zeit um oder Ende.
  bool EreignisHolen(MediaEventType* typ, HRESULT* status, int ms);
  // Holt ein fertiges Bild ab und reicht es weiter. MF_E_TRANSFORM_STREAM_CHANGE
  // wird hier bestätigt. Liefert true, wenn ein Bild ausgeliefert wurde.
  bool AusgabeAbholen();
  void RateUeberwachen(size_t bytes, int64_t jetzt_ms);

  std::mutex mutex_;
  webrtc::EncodedImageCallback* callback_ = nullptr;
  webrtc::H264BitstreamParser h264_parser_;
  webrtc::BitrateAdjuster bitrate_adjuster_;
  webrtc::EncodedImage encoded_image_;
  const webrtc::SdpVideoFormat format_;

  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t framerate_ = 30;
  uint32_t target_bps_ = 0;     // Forderung von WebRTC
  uint32_t encoder_bps_ = 0;    // was der MFT gerade als Ziel hat
  bool screensharing_ = false;
  bool keyframe_angefordert_ = false;
  bool neu_aufsetzen_ = false;
  int64_t frame_index_ = 0;
  std::map<int64_t, Stempel> stempel_;

  // Ratenüberwachung: Ausgabe der letzten Sekunden gegen das Ziel.
  int64_t fenster_start_ms_ = 0;
  uint64_t fenster_bytes_ = 0;
  int ueber_folge_ = 0;          // Sekunden in Folge deutlich über dem Ziel
  int64_t letztes_aufsetzen_ms_ = 0;

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> manager_;
  UINT manager_token_ = 0;
  Microsoft::WRL::ComPtr<IMFTransform> mft_;
  Microsoft::WRL::ComPtr<ICodecAPI> codec_;
  Microsoft::WRL::ComPtr<IMFMediaEventGenerator> events_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> textur_[2];
  int textur_index_ = 0;
  std::vector<uint8_t> nv12_;
  int eingabe_frei_ = 0;        // gesammelte METransformNeedInput

  // Ereignisfaden: GetEvent blockiert; was er holt, landet hier.
  std::thread ereignis_faden_;
  std::mutex ereignis_mutex_;
  std::condition_variable ereignis_cv_;
  std::deque<std::pair<MediaEventType, HRESULT>> ereignisse_;
  bool ereignis_ende_ = false;
};

}  // namespace libwebrtc

#endif  // LIBWEBRTC_WIN_HWENC_MF_H264_ENCODER_H_
