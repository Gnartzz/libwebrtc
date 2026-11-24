#!/usr/bin/env bash
# Baut den mitgelieferten radeonsi-VA-Treiber (hc-va/radeonsi_drv_video.so) für die
# Linux-Pakete: Mesa 26.2.7 + patches/honeycord_mesa_vcn_sitzungsspeicher.patch
# (VCN-Sitzungsspeicher löschen; nur __vaDriverInit exportieren). Ohne LLVM, libstdc++
# und libgcc statisch, GLIBC <= 2.38. Hintergrund: src/linux/hwenc/ffmpeg/ffmpeg_lader.cc.
#
# Läuft in einem Fedora-42-Container (wie die Paketbauten), z. B.:
#   podman run --rm -v "$PWD":/fork -v "$AUS":/aus registry.fedoraproject.org/fedora:42 \
#     bash /fork/build/mesa_va_amd_bauen.sh
set -euo pipefail
FASSUNG=mesa-26.2.7
dnf -y -q install gcc gcc-c++ libstdc++-static meson ninja-build python3-mako python3-pyyaml \
  python3-packaging bison flex libdrm-devel libva-devel expat-devel libzstd-devel \
  elfutils-libelf-devel zlib-devel glslang git-core pkgconf-pkg-config
A=/tmp/mesa-va; rm -rf $A; git clone -q --depth 1 -b $FASSUNG https://gitlab.freedesktop.org/mesa/mesa.git $A
cd $A && git apply /fork/patches/honeycord_mesa_vcn_sitzungsspeicher.patch
meson setup bau -Dbuildtype=release -Dstrip=true -Dgallium-drivers=radeonsi -Dvulkan-drivers= \
  -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled \
  -Dgles2=disabled -Dgallium-va=enabled -Dllvm=disabled -Damd-use-llvm=false -Dvideo-codecs=all \
  -Dshared-glapi=disabled -Dzstd=disabled \
  -Dcpp_link_args="-static-libstdc++ -static-libgcc" -Dc_link_args=-static-libgcc
ninja -C bau
mkdir -p /aus/hc-va
strip -s -o /aus/hc-va/radeonsi_drv_video.so bau/src/gallium/targets/va/libgallium_drv_video.so
cp docs/license.rst /aus/hc-va/MESA-LIZENZ.rst
sha256sum /aus/hc-va/radeonsi_drv_video.so
