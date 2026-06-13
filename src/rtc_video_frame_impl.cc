#include "rtc_video_frame_impl.h"

#include "api/video/i420_buffer.h"
#include "libyuv/convert_argb.h"
#include "libyuv/convert_from.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#ifdef _WIN32
#include "win/honeycord_d3d11_frame.h"
#endif

namespace libwebrtc {

VideoFrameBufferImpl::VideoFrameBufferImpl(
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> frame_buffer)
    : buffer_(frame_buffer) {
  // honeycord: nativen (GPU/kNative) Buffer NICHT mehr eager nach I420 wandeln.
  // Der GPU-Vorschau-Renderer holt sich stattdessen native_shared_handle() und
  // rendert direkt aus der GPU-Textur (kein CPU-Readback). CPU-Konsumenten
  // (Data*/ConvertToARGB) loesen die Wandlung lazy in EnsureI420() aus.
}

// Lazy: native/NV12/etc. -> I420 (gecached). ToI420() liefert fuer I420-Buffer
// sich selbst (billig), fuer kNative den GPU->CPU-Readback. nullptr-sicher.
const webrtc::I420BufferInterface* VideoFrameBufferImpl::EnsureI420() const {
  if (!i420_cache_ && buffer_) {
    i420_cache_ = buffer_->ToI420();
    if (!i420_cache_) {
      i420_cache_ =
          webrtc::I420Buffer::Create(buffer_->width(), buffer_->height());
    }
  }
  return i420_cache_.get();
}

void* VideoFrameBufferImpl::native_shared_handle() const {
#ifdef _WIN32
  if (buffer_ &&
      buffer_->type() == webrtc::VideoFrameBuffer::Type::kNative) {
    return static_cast<honeycord::D3D11FrameBuffer*>(buffer_.get())
        ->shared_handle();
  }
#endif
  return nullptr;
}

VideoFrameBufferImpl::VideoFrameBufferImpl(
    webrtc::scoped_refptr<webrtc::I420Buffer> frame_buffer)
    : buffer_(frame_buffer) {}

VideoFrameBufferImpl::~VideoFrameBufferImpl() {}

scoped_refptr<RTCVideoFrame> VideoFrameBufferImpl::Copy() {
  scoped_refptr<VideoFrameBufferImpl> frame =
      scoped_refptr<VideoFrameBufferImpl>(
          new RefCountedObject<VideoFrameBufferImpl>(buffer_));
  return frame;
}

int VideoFrameBufferImpl::width() const { return buffer_->width(); }

int VideoFrameBufferImpl::height() const { return buffer_->height(); }

const uint8_t* VideoFrameBufferImpl::DataY() const {
  const webrtc::I420BufferInterface* i = EnsureI420();
  return i ? i->DataY() : nullptr;
}

const uint8_t* VideoFrameBufferImpl::DataU() const {
  const webrtc::I420BufferInterface* i = EnsureI420();
  return i ? i->DataU() : nullptr;
}

const uint8_t* VideoFrameBufferImpl::DataV() const {
  const webrtc::I420BufferInterface* i = EnsureI420();
  return i ? i->DataV() : nullptr;
}

int VideoFrameBufferImpl::StrideY() const {
  const webrtc::I420BufferInterface* i = EnsureI420();
  return i ? i->StrideY() : 0;
}

int VideoFrameBufferImpl::StrideU() const {
  const webrtc::I420BufferInterface* i = EnsureI420();
  return i ? i->StrideU() : 0;
}

int VideoFrameBufferImpl::StrideV() const {
  const webrtc::I420BufferInterface* i = EnsureI420();
  return i ? i->StrideV() : 0;
}

int VideoFrameBufferImpl::ConvertToARGB(Type type, uint8_t* dst_buffer,
                                        int dst_stride, int dest_width,
                                        int dest_height) {
  const webrtc::I420BufferInterface* src = EnsureI420();
  if (!src) return 0;
  webrtc::scoped_refptr<webrtc::I420Buffer> i420 =
      webrtc::I420Buffer::Rotate(*src, rotation_);

  webrtc::scoped_refptr<webrtc::I420Buffer> dest =
      webrtc::I420Buffer::Create(dest_width, dest_height);

  dest->ScaleFrom(*i420.get());
  int buf_size = dest->width() * dest->height() * (32 >> 3);
  switch (type) {
    case libwebrtc::RTCVideoFrame::Type::kARGB:
      libyuv::I420ToARGB(dest->DataY(), dest->StrideY(), dest->DataU(),
                         dest->StrideU(), dest->DataV(), dest->StrideV(),
                         dst_buffer, dest->width() * 32 / 8, dest->width(),
                         dest->height());
      break;
    case libwebrtc::RTCVideoFrame::Type::kBGRA:
      libyuv::I420ToBGRA(dest->DataY(), dest->StrideY(), dest->DataU(),
                         dest->StrideU(), dest->DataV(), dest->StrideV(),
                         dst_buffer, dest->width() * 32 / 8, dest->width(),
                         dest->height());
      break;
    case libwebrtc::RTCVideoFrame::Type::kABGR:
      libyuv::I420ToABGR(dest->DataY(), dest->StrideY(), dest->DataU(),
                         dest->StrideU(), dest->DataV(), dest->StrideV(),
                         dst_buffer, dest->width() * 32 / 8, dest->width(),
                         dest->height());
      break;
    case libwebrtc::RTCVideoFrame::Type::kRGBA:
      libyuv::I420ToRGBA(dest->DataY(), dest->StrideY(), dest->DataU(),
                         dest->StrideU(), dest->DataV(), dest->StrideV(),
                         dst_buffer, dest->width() * 32 / 8, dest->width(),
                         dest->height());
      break;
    default:
      break;
  }
  return buf_size;
}

libwebrtc::RTCVideoFrame::VideoRotation VideoFrameBufferImpl::rotation() {
  switch (rotation_) {
    case webrtc::kVideoRotation_0:
      return RTCVideoFrame::kVideoRotation_0;
    case webrtc::kVideoRotation_90:
      return RTCVideoFrame::kVideoRotation_90;
    case webrtc::kVideoRotation_180:
      return RTCVideoFrame::kVideoRotation_180;
    case webrtc::kVideoRotation_270:
      return RTCVideoFrame::kVideoRotation_270;
    default:
      break;
  }
  return RTCVideoFrame::kVideoRotation_0;
}

scoped_refptr<RTCVideoFrame> RTCVideoFrame::Create(int width, int height,
                                                   const uint8_t* buffer,
                                                   int length) {
  int stride_y = width;
  int stride_uv = (width + 1) / 2;

  int size_y = stride_y * height;
  int size_u = stride_uv * height / 2;
  // int size_v = size_u;

  RTC_DCHECK(length == (width * height * 3) / 2);

  const uint8_t* data_y = buffer;
  const uint8_t* data_u = buffer + size_y;
  const uint8_t* data_v = buffer + size_y + size_u;

  webrtc::scoped_refptr<webrtc::I420Buffer> i420_buffer = webrtc::I420Buffer::Copy(
      width, height, data_y, stride_y, data_u, stride_uv, data_v, stride_uv);

  scoped_refptr<VideoFrameBufferImpl> frame =
      scoped_refptr<VideoFrameBufferImpl>(
          new RefCountedObject<VideoFrameBufferImpl>(i420_buffer));
  return frame;
}

scoped_refptr<RTCVideoFrame> RTCVideoFrame::Create(
    int width, int height, const uint8_t* data_y, int stride_y,
    const uint8_t* data_u, int stride_u, const uint8_t* data_v, int stride_v) {
  webrtc::scoped_refptr<webrtc::I420Buffer> i420_buffer = webrtc::I420Buffer::Copy(
      width, height, data_y, stride_y, data_u, stride_u, data_v, stride_v);

  scoped_refptr<VideoFrameBufferImpl> frame =
      scoped_refptr<VideoFrameBufferImpl>(
          new RefCountedObject<VideoFrameBufferImpl>(i420_buffer));
  return frame;
}

}  // namespace libwebrtc
