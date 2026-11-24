#!/usr/bin/env bash
# Baut den mitgelieferten radeonsi-VA-Treiber (hc-va/radeonsi_drv_video.so) für die
# Linux-Pakete: Mesa 26.2.7 + patches/honeycord_mesa_vcn_sitzungsspeicher.patch
# (VCN-Sitzungsspeicher löschen; nur __vaDriverInit exportieren). Ohne LLVM, libstdc++
# und libgcc statisch, GLIBC <= 2.38. Hintergrund: src/linux/hwenc/ffmpeg/va_treiber.cc.
#
# Läuft in einem Fedora-42-Container mit FESTEM Abbild, z. B.:
#   podman run --rm -v "$PWD":/fork:ro -v "$AUS":/aus \
#     registry.fedoraproject.org/fedora@sha256:63773f454664cd77e239f8e0b13ae7f18effe9e3d6612a325b5646eb3bda11f1 \
#     bash /fork/build/mesa_va_amd_bauen.sh
#
# ★ Prüfbefund M4 (24.11.2026):
#  • libva-Untergrenze: Der Treiber exportiert genau EIN Init-Symbol,
#    __vaDriverInit_1_<Header-Fassung>, und libva sucht nur Symbole bis zu
#    seiner eigenen Fassung. Mehrere Init-Fassungen aus EINEM Bau gehen nicht:
#    Mesa kopiert beim Init seine ganze VADriverVTable (Größe der Header) in
#    die von libva angelegte — mit neueren Headern als die libva liefe das
#    über deren Ende hinaus. Also gegen ÄLTERE Header bauen, und zwar libva
#    2.21 (VA-API 1.21): Das mitgelieferte FFmpeg (libavutil_hc) bindet
#    vaMapBuffer2 und lädt deshalb selbst erst ab libva 2.21 — darunter gibt
#    es ohnehin keine VA-API über unser FFmpeg. Mesa 26.2.7 verliert mit 1.21
#    gegenüber 1.22 nichts (seine Fassungsweichen stehen bei 1.15/1.16/1.18/
#    1.21/1.23; 1.23 ist nur AV1 Profil 2).
#  • Feste Stände: Abbild per Prüfsumme (oben), Pakete per genauer Fassung
#    (PAKETE), Mesa und libva per Commit. Ist eine Paketfassung nicht mehr zu
#    bekommen (Fedora-updates trägt nur die neueste), bricht dnf ab — dann
#    bewusst neu festlegen und die Soll-Prüfsumme neu bestimmen.
#  • Prüfungen am Ergebnis: GLIBC <= 2.38, kein GLIBCXX/CXXABI, Exportliste
#    genau __vaDriverInit_1_21, NEEDED nur aus der erlaubten Liste,
#    Soll-Prüfsumme (SOLL_SHA256; HC_MESA_SUMME_NEU=1 gibt die neue nur aus).
set -euo pipefail

MESA_TAG=mesa-26.2.7
MESA_COMMIT=b69adc2adfbfe3eb985bd66cd06c61ca719ae9ae
LIBVA_TAG=2.21.0
LIBVA_COMMIT=0b01aed44ef1a6ad660261284ff266fa812829ef
VA_INIT=__vaDriverInit_1_21
GLIBC_HOECHSTENS=2.38
SOLL_SHA256=b9bde53f91e32b2a1e5fed84962394f3ca515c52eb54eb71229cd0572dcfd299   # zwei unabhängige Bauten 24.11.2026, bitgleich
PAKETE=(
  gcc-15.2.1-7.fc42 gcc-c++-15.2.1-7.fc42 libstdc++-static-15.2.1-7.fc42
  glibc-devel-2.41-18.fc42 binutils-2.44-12.fc42
  meson-1.7.2-1.fc42 ninja-build-1.12.1-4.fc42 python3-mako-1.2.3-9.fc42
  python3-pyyaml-6.0.2-2.fc42 python3-packaging-24.2-3.fc42 bison-3.8.2-11.fc42
  flex-2.6.4-19.fc42 libdrm-devel-2.4.133-1.fc42 expat-devel-2.7.3-1.fc42
  libzstd-devel-1.5.7-1.fc42 elfutils-libelf-devel-0.195-1.fc42
  zlib-ng-compat-devel-2.2.5-2.fc42 glslang-15.3.0-1.fc42 git-core-2.54.0-1.fc42
  pkgconf-pkg-config-2.3.0-2.fc42
)
ERLAUBT_NEEDED="libc.so.6 libm.so.6 libdrm.so.2 libdrm_amdgpu.so.1 libz.so.1 libexpat.so.1 libelf.so.1"

dnf -y -q install "${PAKETE[@]}"
# Die Fassungen wirklich prüfen (dnf nimmt bei schon vorhandenen Paketen keine Rücksicht).
for p in "${PAKETE[@]}"; do
  n=$(rpm -q --qf '%{NAME}-%{VERSION}-%{RELEASE}\n' "${p%%-[0-9]*}" | head -1)
  [ "$n" = "$p" ] || { echo "FEHL: Paket $n statt $p"; exit 3; }
done
if rpm -q libva-devel >/dev/null 2>&1; then echo "FEHL: libva-devel installiert (Header 2.22)"; exit 3; fi

# libva 2.21: nur Header und pkg-config für den Mesa-Bau (der Treiber bindet libva nicht).
git clone -q --depth 1 -b $LIBVA_TAG https://github.com/intel/libva.git /tmp/libva
[ "$(git -C /tmp/libva rev-parse HEAD)" = $LIBVA_COMMIT ] || { echo "FEHL: libva-Commit"; exit 3; }
(cd /tmp/libva && meson setup bau --prefix=/opt/libva --libdir=lib -Dwith_x11=no -Dwith_glx=no \
   -Dwith_wayland=no -Dwith_win32=no -Denable_docs=false >/dev/null && ninja -C bau install >/dev/null)
export PKG_CONFIG_PATH=/opt/libva/lib/pkgconfig

A=/tmp/mesa-va; rm -rf $A; git clone -q --depth 1 -b $MESA_TAG https://gitlab.freedesktop.org/mesa/mesa.git $A
[ "$(git -C $A rev-parse HEAD)" = $MESA_COMMIT ] || { echo "FEHL: Mesa-Commit"; exit 3; }
cd $A && git apply /fork/patches/honeycord_mesa_vcn_sitzungsspeicher.patch
meson setup bau -Dbuildtype=release -Dstrip=true -Dgallium-drivers=radeonsi -Dvulkan-drivers= \
  -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled \
  -Dgles2=disabled -Dgallium-va=enabled -Dllvm=disabled -Damd-use-llvm=false -Dvideo-codecs=all \
  -Dshared-glapi=disabled -Dzstd=disabled \
  -Dcpp_link_args="-static-libstdc++ -static-libgcc" -Dc_link_args=-static-libgcc
grep -q "libva.*2\.21\.0\|1\.21\.0" bau/meson-logs/meson-log.txt || { echo "FEHL: Mesa sieht nicht libva 2.21"; exit 3; }
ninja -C bau
mkdir -p /aus/hc-va
T=/aus/hc-va/radeonsi_drv_video.so
strip -s -o $T bau/src/gallium/targets/va/libgallium_drv_video.so
cp docs/license.rst /aus/hc-va/MESA-LIZENZ.rst
cp /fork/patches/honeycord_mesa_vcn_sitzungsspeicher.patch /aus/hc-va/QUELLE-mesa-26.2.7.patch

# ── Prüfungen ───────────────────────────────────────────────────────────────
g=$(objdump -T $T | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -uV | tail -1)
[ "$(printf '%s\n%s\n' "$g" $GLIBC_HOECHSTENS | sort -V | tail -1)" = $GLIBC_HOECHSTENS ] \
  || { echo "FEHL: braucht GLIBC $g > $GLIBC_HOECHSTENS"; exit 4; }
! objdump -T $T | grep -qE 'GLIBCXX|CXXABI' || { echo "FEHL: braucht libstdc++ des Systems"; exit 4; }
ex=$(nm -D --defined-only $T | awk '{print $3}' | sort | tr '\n' ' ')
[ "$ex" = "$VA_INIT " ] || { echo "FEHL: Exporte '$ex' statt '$VA_INIT'"; exit 4; }
for n in $(readelf -d $T | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p'); do
  case " $ERLAUBT_NEEDED " in *" $n "*) ;; *) echo "FEHL: NEEDED $n nicht erlaubt"; exit 4;; esac
done
ist=$(sha256sum $T | cut -d' ' -f1)
{ echo "Mesa $MESA_TAG $MESA_COMMIT + honeycord_mesa_vcn_sitzungsspeicher.patch"
  echo "libva-Header $LIBVA_TAG $LIBVA_COMMIT (Export $VA_INIT)"
  echo "GLIBC hoechstens $g"; echo "sha256 $ist"; printf '%s\n' "${PAKETE[@]}"; } > /aus/hc-va/BAU-STAND.txt
echo "Prüfungen bestanden: GLIBC $g, Export $ex, NEEDED erlaubt, sha256 $ist"
if [ "$ist" != "$SOLL_SHA256" ]; then
  if [ "${HC_MESA_SUMME_NEU:-0}" = 1 ]; then echo "NEUE Soll-Prüfsumme: $ist"; else
    echo "FEHL: sha256 $ist statt Soll $SOLL_SHA256"; exit 5; fi
fi
