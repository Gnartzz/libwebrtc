/*
 *  Copyright (c) 2017 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "src/internal/video_capturer.h"

#include <algorithm>

#include "api/scoped_refptr.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame_buffer.h"
#include "api/video/video_rotation.h"

namespace webrtc {
namespace internal {
VideoCapturer::VideoCapturer() = default;
VideoCapturer::~VideoCapturer() = default;

void VideoCapturer::OnFrame(const VideoFrame& frame) {
  // honeycord Zero-Copy: kNative-Frames (GPU-D3D11-Textur, vom Bildschirm-
  // Capturer bereits per Shader auf Zielaufloesung herunterskaliert) NIEMALS
  // durch den VideoAdapter-Scale-Pfad schicken. Der wuerde unten
  // frame.video_frame_buffer()->ToI420() + ScaleFrom() aufrufen -> CPU-Readback
  // (zerstoert Zero-Copy) bzw. Absturz in I420Buffer (ToI420 kann den GPU-Frame
  // nicht liefern). Direkt an die Senken durchreichen; der NVENC-Encoder
  // verarbeitet den nativen Frame (supports_native_handle=true). Die Bildrate
  // steuert der Capturer selbst (capture_delay_).
  if (frame.video_frame_buffer()->type() ==
      webrtc::VideoFrameBuffer::Type::kNative) {
    broadcaster_.OnFrame(frame);
    return;
  }

  int cropped_width = 0;
  int cropped_height = 0;
  int out_width = 0;
  int out_height = 0;

  if (!video_adapter_.AdaptFrameResolution(
          frame.width(), frame.height(), frame.timestamp_us() * 1000,
          &cropped_width, &cropped_height, &out_width, &out_height)) {
    // Drop frame in order to respect frame rate constraint.
    return;
  }

  if (out_height != frame.height() || out_width != frame.width()) {
    // Video adapter has requested a down-scale. Allocate a new buffer and
    // return scaled version.
    webrtc::scoped_refptr<I420Buffer> scaled_buffer =
        I420Buffer::Create(out_width, out_height);
    scaled_buffer->ScaleFrom(*frame.video_frame_buffer()->ToI420());
    broadcaster_.OnFrame(VideoFrame::Builder()
                             .set_video_frame_buffer(scaled_buffer)
                             .set_rotation(kVideoRotation_0)
                             .set_timestamp_us(frame.timestamp_us())
                             .set_id(frame.id())
                             .build());
  } else {
    // No adaptations needed, just return the frame as is.
    broadcaster_.OnFrame(frame);
  }
}

webrtc::VideoSinkWants VideoCapturer::GetSinkWants() {
  return broadcaster_.wants();
}

void VideoCapturer::AddOrUpdateSink(webrtc::VideoSinkInterface<VideoFrame>* sink,
                                    const webrtc::VideoSinkWants& wants) {
  broadcaster_.AddOrUpdateSink(sink, wants);
  UpdateVideoAdapter();
}

void VideoCapturer::RemoveSink(webrtc::VideoSinkInterface<VideoFrame>* sink) {
  broadcaster_.RemoveSink(sink);
  UpdateVideoAdapter();
}

void VideoCapturer::UpdateVideoAdapter() {
  webrtc::VideoSinkWants wants = broadcaster_.wants();

  if (0 < wants.resolutions.size()) {
    auto size = wants.resolutions.at(0);
    std::pair<int, int> target_aspect_ratiot(size.width, size.height);
    video_adapter_.OnOutputFormatRequest(
        target_aspect_ratiot, wants.max_pixel_count, wants.max_framerate_fps);
  } else {
    video_adapter_.OnSinkWants(wants);
  }
}

}  // namespace internal
}  // namespace webrtc
