NvCodec video encoder port for libwebrtc-honeycord.

Adapted from Shiguredo Momo / sora-cpp-sdk (Apache License 2.0):
  https://github.com/shiguredo/momo
  https://github.com/shiguredo/sora-cpp-sdk

Original NvCodec encoder lives in sora-cpp-sdk/src/hwenc_nvcodec/.
Ported to the Gnartzz libwebrtc CMake wrapper, m144_release branch,
with platform pruned to _WIN32 only and SoraCudaContext replaced by
a minimal compat stub (sora_compat.h).
