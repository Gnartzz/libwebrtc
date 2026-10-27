#ifndef LIBWEBRTC_WIN_HWDEC_HONEYCORD_H264_SPS_DIAG_H_
#define LIBWEBRTC_WIN_HWDEC_HONEYCORD_H264_SPS_DIAG_H_

// PRUEF (Diagnose, 2026-10-27): liest aus einem Annex-B-H.264-Zugriffsblock die
// SPS-Felder, die fuer die Frage "nimmt der Windows-H.264-MFT den DXVA-Pfad
// oder faellt er still auf Software zurueck?" zaehlen: profile_idc,
// constraint_set0..5, level_idc, max_num_ref_frames, Bildgroesse und aus der
// VUI (falls vorhanden) max_num_reorder_frames / max_dec_frame_buffering.
//
// Plattformneutral (kein Windows-Header), damit es ausserhalb des Windows-Baus
// geprueft werden kann. Nur Lesen, kein Eingriff in den Datenstrom. Bei jedem
// Lese-Fehler kommt std::nullopt zurueck, nie ein Abbruch.
//
// Hinweis: Der WebRTC-Empfaenger (VideoRtpDepacketizerH264) schreibt die VUI
// eingehender SPS um (SpsVuiRewriter: bitstream_restriction mit
// max_dec_frame_buffering = max_num_ref_frames). Gelesen wird also die SPS,
// die der Decoder WIRKLICH bekommt, nicht zwingend die des Senders.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

#include "api/array_view.h"
#include "common_video/h264/h264_common.h"
#include "common_video/h264/sps_parser.h"
#include "rtc_base/bitstream_reader.h"

namespace libwebrtc {
namespace hwdec_diag {

struct SpsInfo {
  uint8_t profile_idc = 0;
  uint8_t constraint_flags = 0;  // Bit 7 = constraint_set0 ... Bit 2 = set5
  uint8_t level_idc = 0;
  uint32_t max_num_ref_frames = 0;
  uint32_t width = 0;   // nach Crop
  uint32_t height = 0;  // nach Crop
  uint32_t mbs_per_frame = 0;  // Makrobloecke je Bild (fuer MaxFS/MaxMBPS)
  bool vui = false;
  bool bitstream_restriction = false;
  uint32_t max_num_reorder_frames = 0;   // nur gueltig mit bitstream_restriction
  uint32_t max_dec_frame_buffering = 0;  // nur gueltig mit bitstream_restriction
  bool timing = false;
  uint32_t num_units_in_tick = 0;
  uint32_t time_scale = 0;

  bool operator==(const SpsInfo& o) const {
    return profile_idc == o.profile_idc &&
           constraint_flags == o.constraint_flags &&
           level_idc == o.level_idc &&
           max_num_ref_frames == o.max_num_ref_frames && width == o.width &&
           height == o.height && vui == o.vui &&
           bitstream_restriction == o.bitstream_restriction &&
           max_num_reorder_frames == o.max_num_reorder_frames &&
           max_dec_frame_buffering == o.max_dec_frame_buffering;
  }
  bool operator!=(const SpsInfo& o) const { return !(*this == o); }
};

inline const char* ProfileName(uint8_t p, uint8_t cflags) {
  switch (p) {
    case 66:
      return (cflags & 0x40) ? "Constrained Baseline" : "Baseline";
    case 77:
      return "Main";
    case 88:
      return "Extended";
    case 100:
      return (cflags & 0x0C) == 0x0C ? "Constrained High" : "High";
    case 110:
      return "High 10";
    case 122:
      return "High 4:2:2";
    case 244:
      return "High 4:4:4";
    default:
      return "?";
  }
}

namespace internal {

// Zugriff auf den geschuetzten Teil-Parser von webrtc::SpsParser: er liest bis
// einschliesslich vui_parameters_present_flag und laesst den Leser genau dort
// stehen. Den VUI-Rest lesen wir selbst.
class SpsUpToVui : public webrtc::SpsParser {
 public:
  static std::optional<SpsState> Parse(webrtc::BitstreamReader& reader) {
    return ParseSpsUpToVui(reader);
  }
};

inline void SkipHrd(webrtc::BitstreamReader& r) {
  uint32_t cpb_cnt_minus1 = r.ReadExponentialGolomb();
  if (!r.Ok() || cpb_cnt_minus1 > 31) {
    r.Invalidate();
    return;
  }
  r.ConsumeBits(8);  // bit_rate_scale u(4), cpb_size_scale u(4)
  for (uint32_t i = 0; i <= cpb_cnt_minus1 && r.Ok(); ++i) {
    r.ReadExponentialGolomb();  // bit_rate_value_minus1
    r.ReadExponentialGolomb();  // cpb_size_value_minus1
    r.ConsumeBits(1);           // cbr_flag
  }
  // initial_cpb_removal_delay_length_minus1, cpb_removal_delay_length_minus1,
  // dpb_output_delay_length_minus1, time_offset_length: je u(5)
  r.ConsumeBits(20);
}

// VUI ab aspect_ratio_info_present_flag (H.264 Anhang E.1.1).
inline bool ParseVui(webrtc::BitstreamReader& r, SpsInfo& out) {
  if (r.Read<bool>()) {                 // aspect_ratio_info_present_flag
    if (r.Read<uint8_t>() == 255) {     // aspect_ratio_idc == Extended_SAR
      r.ConsumeBits(32);                // sar_width, sar_height
    }
  }
  if (r.Read<bool>()) r.ConsumeBits(1);  // overscan_info / overscan_appropriate
  if (r.Read<bool>()) {                  // video_signal_type_present_flag
    r.ConsumeBits(4);                    // video_format u(3), full_range u(1)
    if (r.Read<bool>()) r.ConsumeBits(24);  // colour_description: 3x u(8)
  }
  if (r.Read<bool>()) {                  // chroma_loc_info_present_flag
    r.ReadExponentialGolomb();
    r.ReadExponentialGolomb();
  }
  out.timing = r.Read<bool>();           // timing_info_present_flag
  if (out.timing) {
    out.num_units_in_tick = r.Read<uint32_t>();
    out.time_scale = r.Read<uint32_t>();
    r.ConsumeBits(1);                    // fixed_frame_rate_flag
  }
  const bool nal_hrd = r.Read<bool>();
  if (nal_hrd) SkipHrd(r);
  const bool vcl_hrd = r.Read<bool>();
  if (vcl_hrd) SkipHrd(r);
  if (nal_hrd || vcl_hrd) r.ConsumeBits(1);  // low_delay_hrd_flag
  r.ConsumeBits(1);                      // pic_struct_present_flag
  out.bitstream_restriction = r.Read<bool>();
  if (out.bitstream_restriction) {
    r.ConsumeBits(1);  // motion_vectors_over_pic_boundaries_flag
    r.ReadExponentialGolomb();  // max_bytes_per_pic_denom
    r.ReadExponentialGolomb();  // max_bits_per_mb_denom
    r.ReadExponentialGolomb();  // log2_max_mv_length_horizontal
    r.ReadExponentialGolomb();  // log2_max_mv_length_vertical
    out.max_num_reorder_frames = r.ReadExponentialGolomb();
    out.max_dec_frame_buffering = r.ReadExponentialGolomb();
  }
  return r.Ok();
}

}  // namespace internal

// Eine SPS-NAL-Nutzlast OHNE Startcode, MIT NAL-Kopfbyte.
inline std::optional<SpsInfo> ParseSpsNalu(webrtc::ArrayView<const uint8_t> nalu) {
  if (nalu.size() < webrtc::H264::kNaluTypeSize + 3) return std::nullopt;
  std::vector<uint8_t> rbsp =
      webrtc::H264::ParseRbsp(nalu.subview(webrtc::H264::kNaluTypeSize));
  if (rbsp.size() < 3) return std::nullopt;
  SpsInfo info;
  info.profile_idc = rbsp[0];
  info.constraint_flags = rbsp[1];
  info.level_idc = rbsp[2];

  webrtc::BitstreamReader reader(rbsp);
  std::optional<webrtc::SpsParser::SpsState> sps =
      internal::SpsUpToVui::Parse(reader);
  if (!sps) return std::nullopt;
  info.max_num_ref_frames = sps->max_num_ref_frames;
  info.width = sps->width;
  info.height = sps->height;
  info.mbs_per_frame = (sps->pic_width_in_mbs_minus1 + 1) *
                       (sps->pic_height_in_map_units_minus1 + 1) *
                       (2 - sps->frame_mbs_only_flag);
  info.vui = sps->vui_params_present != 0;
  if (info.vui) {
    // Ein Fehler in der VUI macht die Kernfelder nicht ungueltig: dann nur
    // ohne VUI-Werte melden.
    SpsInfo vui_info = info;
    if (internal::ParseVui(reader, vui_info)) {
      info = vui_info;
    } else {
      info.bitstream_restriction = false;
      info.timing = false;
    }
  }
  return info;
}

// Erste SPS in einem Annex-B-Zugriffsblock suchen und lesen. found_sps sagt,
// ob ueberhaupt eine SPS-NAL drin war (sonst ist nullopt kein Lesefehler).
inline std::optional<SpsInfo> FindAndParseSps(webrtc::ArrayView<const uint8_t> au,
                                              bool* found_sps) {
  if (found_sps) *found_sps = false;
  for (const webrtc::H264::NaluIndex& idx : webrtc::H264::FindNaluIndices(au)) {
    if (idx.payload_size == 0) continue;
    if (webrtc::H264::ParseNaluType(au[idx.payload_start_offset]) !=
        webrtc::H264::kSps)
      continue;
    if (found_sps) *found_sps = true;
    return ParseSpsNalu(au.subview(idx.payload_start_offset, idx.payload_size));
  }
  return std::nullopt;
}

// Eine Logzeile (ohne Zeilenende-Pflicht des Aufrufers; '\n' ist enthalten).
inline void FormatSpsLine(const SpsInfo& s, const void* strom, char* buf,
                          size_t n) {
  char cs[16];
  std::snprintf(cs, sizeof(cs), "%d%d%d%d%d%d", (s.constraint_flags >> 7) & 1,
                (s.constraint_flags >> 6) & 1, (s.constraint_flags >> 5) & 1,
                (s.constraint_flags >> 4) & 1, (s.constraint_flags >> 3) & 1,
                (s.constraint_flags >> 2) & 1);
  char vui[96];
  if (!s.vui) {
    std::snprintf(vui, sizeof(vui), "vui=nein");
  } else if (!s.bitstream_restriction) {
    std::snprintf(vui, sizeof(vui), "vui=ja dpb=- reorder=-");
  } else {
    std::snprintf(vui, sizeof(vui), "vui=ja dpb=%u reorder=%u",
                  s.max_dec_frame_buffering, s.max_num_reorder_frames);
  }
  char timing[48] = "";
  if (s.timing && s.num_units_in_tick > 0) {
    std::snprintf(timing, sizeof(timing), " vui_fps=%.2f",
                  s.time_scale / (2.0 * s.num_units_in_tick));
  }
  std::snprintf(buf, n,
                "[hwdec-sps] %ux%u profil=%u (%s) cs=%s (0x%02X) level=%u "
                "refs=%u mbs=%u %s%s | strom=%p\n",
                s.width, s.height, s.profile_idc,
                ProfileName(s.profile_idc, s.constraint_flags), cs,
                s.constraint_flags, s.level_idc, s.max_num_ref_frames,
                s.mbs_per_frame, vui, timing, strom);
}

}  // namespace hwdec_diag
}  // namespace libwebrtc

#endif  // LIBWEBRTC_WIN_HWDEC_HONEYCORD_H264_SPS_DIAG_H_
