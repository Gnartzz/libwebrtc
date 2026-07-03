#ifndef LIBWEBRTC_WIN_HWDEC_HONEYCORD_D3D11VA_H264_DECODER_H_
#define LIBWEBRTC_WIN_HWDEC_HONEYCORD_D3D11VA_H264_DECODER_H_

// Vendor-neutraler HW-H.264-Decoder fuer Windows ueber Media Foundation
// (CMSH264DecoderMFT) mit D3D11VA. Dekodiert auf der GPU (egal ob NVIDIA, AMD
// oder Intel — DXVA waehlt den HW-Pfad der jeweiligen GPU), gibt eine NV12-
// D3D11-Textur aus, konvertiert sie per D3D11-VideoProcessor nach BGRA und
// uebergibt sie ZERO-COPY als honeycord::D3D11FrameBuffer (kNative + Legacy-
// Shared-Handle einer KEYED_MUTEX-Textur). Der flutter_webrtc-Renderer greift
// dann via native_shared_handle() direkt zu — kein CPU-Readback, kein SW-Decode.
//
// Ziel (Leitprinzip): max. Qualitaet bei min. CPU/GPU. SW-H.264-Decode (Status
// quo) ist CPU-teuer; HW-Decode verlagert das auf die GPU-Decode-Einheit + spart
// den I420->BGRA-CPU-Convert + den Re-Upload im Renderer (der jetzt die fertige
// GPU-Textur bekommt).

#ifdef _WIN32

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>

#include <d3d11.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include "api/video_codecs/video_decoder.h"
#include "api/video/encoded_image.h"

namespace libwebrtc {

class D3D11VAH264Decoder : public webrtc::VideoDecoder {
 public:
  D3D11VAH264Decoder();
  ~D3D11VAH264Decoder() override;

  // Schneller Probe-Test (einmalig in der Factory): laesst sich der MFT mit
  // D3D11-Device-Manager + HW-Acceleration ueberhaupt erzeugen? Faellt das fehl
  // (kein DXVA-faehiger H.264-Decoder), nutzt die Factory den SW-Builtin.
  static bool IsSupported();

  // webrtc::VideoDecoder
  bool Configure(const Settings& settings) override;
  int32_t Decode(const webrtc::EncodedImage& input_image,
                 int64_t render_time_ms) override;
  int32_t RegisterDecodeCompleteCallback(
      webrtc::DecodedImageCallback* callback) override;
  int32_t Release() override;
  DecoderInfo GetDecoderInfo() const override;
  const char* ImplementationName() const override;

 private:
  // Lazy-Init beim ersten Frame (oder bei Aufloesungswechsel via STREAM_CHANGE):
  // D3D11-Device + IMFDXGIDeviceManager + CMSH264DecoderMFT + VideoProcessor.
  bool EnsureMft();
  // Output-Type nach MF_E_TRANSFORM_STREAM_CHANGE neu aushandeln (NV12 + Groesse).
  bool NegotiateOutputType();
  // Die fertige NV12-Decode-Textur per VideoProcessor nach BGRA wandeln, in die
  // KEYED_MUTEX-Shared-Textur kopieren und als honeycord::D3D11FrameBuffer-
  // VideoFrame an den Callback liefern. timestamp_rtp = input.RtpTimestamp().
  bool EmitFrame(ID3D11Texture2D* nv12, UINT array_index,
                 uint32_t timestamp_rtp, int64_t ntp_time_ms);
  // VideoProcessor (NV12->BGRA) + Shared-Textur-Ring fuer die aktuelle
  // coded_/out_-Groesse (re)initialisieren.
  bool EnsureConverter();
  void ReleaseInternal();

  webrtc::DecodedImageCallback* callback_ = nullptr;

  // D3D11
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> dxgi_manager_;
  UINT reset_token_ = 0;

  // Media Foundation H.264 Decoder-MFT
  Microsoft::WRL::ComPtr<IMFTransform> mft_;
  bool mft_configured_ = false;

  // NV12->BGRA via D3D11-VideoProcessor. Der Blt schreibt DIREKT in die jeweilige
  // Shared-Textur des Rings (spart eine CopyResource ggue. dem Capturer-Pfad).
  Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
  Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessor> video_processor_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> video_enum_;

  // Shared-Textur-RING (D3D11_RESOURCE_MISC_SHARED, Legacy-Handle via
  // IDXGIResource::GetSharedHandle, kein Keyed-Mutex — wie der Capturer).
  // Ring=3, damit die Textur hinter einem emittierten D3D11FrameBuffer ~100ms
  // gueltig bleibt: WebRTCs Render-Pacing zeigt den Frame erst 15-70ms nach dem
  // Decode an. Mit Ring=1 (Experiment 2026-07-01, widerlegt: aenderte am 8-fps-
  // Stall NICHTS — der war der 21-tiefe MFT-Puffer, s. MF_LOW_LATENCY in EnsureMft)
  // ueberschreibt der Decoder die EINE Textur waehrend ANGLE sie sampelt ->
  // Pacing ausgehebelt (es erscheint immer der neueste Frame) + Tearing-Risiko.
  // Trade: Handle wechselt pro Frame -> Flutters Embedder legt die EGL-Surface
  // pro Composite neu an; gemessen billig (Self-View-Capturer-Ring schafft so
  // seine volle Composite-Rate, mark~0ms).
  static constexpr int kShareRing = 3;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> share_tex_[kShareRing];
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> share_view_[kShareRing];
  HANDLE share_handle_[kShareRing] = {nullptr};
  int share_idx_ = 0;

  // Coded-Groesse = Decoder-Output-Textur (16-aligned, evtl. groesser); Display-
  // Groesse = sichtbare Apertur (MF_MT_MINIMUM_DISPLAY_APERTURE), die wir
  // ausgeben. disp_x/y = Apertur-Offset (Crop gegen Padding-Raender, z.B. 1080->1088).
  int coded_w_ = 0, coded_h_ = 0;
  int disp_x_ = 0, disp_y_ = 0;
  int out_w_ = 0, out_h_ = 0;   // sichtbare Display-Groesse (== Shared-Textur-Groesse)
  int conv_w_ = 0, conv_h_ = 0; // Groesse, fuer die der VideoProcessor/Ring grad init ist

  // TEMPORAERE Instrumentierung (Diagnose Mehr-Stream-Decode): misst, ob die
  // ms/Frame im MS-Decode (ProcessOutput) oder unserem Convert (EmitFrame inkl.
  // View-Erzeugung) stecken. Schreibt alle 120 Decode-Calls eine Zeile nach
  // %LOCALAPPDATA%\HoneyCord\hwdec.log. Nach der Diagnose wieder entfernen.
  // FIFO offener Input-rtps: der MFT gibt Frames verzoegert, aber in Reihenfolge
  // aus -> jeder Output nimmt den aeltesten (front). Verhindert falsche rtp-Tags.
  std::deque<uint32_t> pending_rtp_;
  unsigned long long dbg_calls_ = 0, dbg_frames_ = 0;
  unsigned dbg_max_pending_ = 0;  // max. MFT-Puffertiefe (offene Inputs) im Fenster
  double dbg_po_ms_ = 0, dbg_emit_ms_ = 0, dbg_view_ms_ = 0;
  double dbg_pi_ms_ = 0;  // ProcessInput-Zeit: misst, ob der Decode-Input selbst stallt (vs. WebRTC-Drossel oben)
  // Fensterstart (wall-clock): erlaubt echte Decode-CALL-Rate (calls/s) statt nur
  // ms/Call. Entscheidet: werden Frames VOR dem Decode verworfen (decode_fps ~8,
  // Decoder gedrosselt) oder NACH dem Decode (decode_fps ~30, Render-Queue droppt)?
  std::chrono::steady_clock::time_point dbg_win_{};
  void DbgFlush();

  std::mutex mutex_;
};

}  // namespace libwebrtc

#endif  // _WIN32
#endif  // LIBWEBRTC_WIN_HWDEC_HONEYCORD_D3D11VA_H264_DECODER_H_
