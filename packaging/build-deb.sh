#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Build the Debian packages for one architecture:
#
#   packaging/build-deb.sh arm64|riscv64 [version]
#
# duos-detect_<version>_<arch>.deb  this program, the person models and the
#                                    systemd service (AGPL-3.0-or-later)
# cviruntime_<rtversion>_<arch>.deb  the TPU runtime from tools/build-cviruntime.sh.
#                                    Sophgo grants no licence to redistribute
#                                    it: for your own boards only.
#
# MODELS is a directory with the person-*.cvimodel files (default work/models).
# Output: work/pkg/. CVIRT, MODELS and MAINTAINER can be set in the environment.
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
ARCH=${1:-}
VER=${2:-0.2.0}
RTVER=${RTVER:-1.4.0-2}
MODELS=${MODELS:-$REPO/work/models}
MAINTAINER=${MAINTAINER:-morikon <leafpmpmp@gmail.com>}
case $ARCH in
arm64)   TRIPLET=aarch64-linux-gnu ;;
riscv64) TRIPLET=riscv64-linux-gnu ;;
*)       echo "usage: $0 arm64|riscv64 [version]" >&2; exit 1 ;;
esac
RT=${CVIRT:-$REPO/build/cviruntime/$ARCH/install}
set -- "$MODELS"/person-*.cvimodel
[ -f "$1" ] || { echo "no person-*.cvimodel in $MODELS" >&2; exit 1; }
OUT=$REPO/work/pkg
mkdir -p "$OUT"

CVIRT=$RT "$REPO/build.sh" "$ARCH" >/dev/null

# --- duos-detect ---------------------------------------------------------
D=$OUT/duos-detect_${VER}_$ARCH
rm -rf "$D"
mkdir -p "$D/DEBIAN" "$D/usr/bin" "$D/usr/share/duos-detect/models" \
	"$D/etc/default" "$D/usr/lib/systemd/system" "$D/usr/share/doc/duos-detect"
install -m 755 "$REPO/build/$TRIPLET/duos-detect" "$D/usr/bin/"
install -m 644 "$@" "$D/usr/share/duos-detect/models/"
install -m 644 "$REPO/packaging/duos-detect.service" "$D/usr/lib/systemd/system/"
install -m 644 "$REPO/packaging/duos-detect.default" "$D/etc/default/duos-detect"
cat > "$D/usr/share/doc/duos-detect/copyright" <<'EOF'
duos-detect: person detection for the Milk-V Duo S (SG2000).
License: AGPL-3.0-or-later (/usr/share/common-licenses/AGPL-3 or
https://www.gnu.org/licenses/agpl-3.0.txt).
The models are Ultralytics YOLO11 networks (AGPL-3.0), fine-tuned on the
person class of COCO 2017 (annotations CC BY 4.0).
EOF
cat > "$D/DEBIAN/control" <<EOF
Package: duos-detect
Version: $VER
Architecture: $ARCH
Maintainer: $MAINTAINER
Depends: cviruntime, libjpeg62-turbo, libc6, libstdc++6
Section: video
Priority: optional
Description: person detection for the Milk-V Duo S (camera and TPU)
 Reads the Camera Module 3 (IMX708), runs a YOLO person detector on the
 SG2000 TPU and serves the results and a live preview over HTTP.
EOF
echo /etc/default/duos-detect > "$D/DEBIAN/conffiles"
cat > "$D/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = configure ] && [ -d /run/systemd/system ]; then
	systemctl daemon-reload
	if systemctl -q is-enabled duos-detect 2>/dev/null; then
		systemctl restart duos-detect
	else
		echo "duos-detect: to start it now and at every boot:" \
		     "systemctl enable --now duos-detect"
	fi
fi
EOF
cat > "$D/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e
if [ -d /run/systemd/system ]; then
	systemctl stop duos-detect 2>/dev/null || true
	[ "$1" = remove ] && systemctl disable duos-detect 2>/dev/null || true
fi
EOF
cat > "$D/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
[ -d /run/systemd/system ] && systemctl daemon-reload || true
EOF
chmod 755 "$D/DEBIAN/postinst" "$D/DEBIAN/prerm" "$D/DEBIAN/postrm"

# --- cviruntime ----------------------------------------------------------
C=$OUT/cviruntime_${RTVER}_$ARCH
rm -rf "$C"
mkdir -p "$C/DEBIAN" "$C/usr/lib/$TRIPLET" "$C/usr/lib/cviruntime/bin" \
	"$C/usr/share/doc/cviruntime"
install -m 644 "$RT/lib/libcviruntime.so" "$RT/lib/libcvikernel.so" "$RT/lib/libcnpy.so" \
	"$C/usr/lib/$TRIPLET/"
install -m 755 "$RT/bin/model_runner" "$RT/bin/cvimodel_tool" "$C/usr/lib/cviruntime/bin/"
cat > "$C/usr/share/doc/cviruntime/copyright" <<'EOF'
cviruntime and cvikernel: Sophgo/CVITEK TPU runtime, with the dma-buf heap
port for mainline Linux. Copyright (C) Cvitek Co., Ltd. and Bitmain
Technologies Inc.; the sources carry no licence grant. Built locally from
the vendor SDK: do not redistribute this package.

libcnpy: cnpy, Copyright (C) 2011 Carl Rogers, MIT licence. Statically
linked zlib: Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler,
zlib licence.
EOF
cat > "$C/DEBIAN/control" <<EOF
Package: cviruntime
Version: $RTVER
Architecture: $ARCH
Maintainer: $MAINTAINER
Depends: libc6, libstdc++6, libgcc-s1
Section: libs
Priority: optional
Description: Sophgo CV18xx TPU runtime (mainline dma-buf port, local build)
 libcviruntime and libcvikernel for /dev/cvi-tpu0 on mainline kernels, plus
 model_runner and cvimodel_tool in /usr/lib/cviruntime/bin.
EOF
printf '#!/bin/sh\nset -e\nldconfig\n' > "$C/DEBIAN/postinst"
printf '#!/bin/sh\nset -e\nldconfig\n' > "$C/DEBIAN/postrm"
chmod 755 "$C/DEBIAN/postinst" "$C/DEBIAN/postrm"

docker run --rm -u "$(id -u):$(id -g)" -v "$OUT:/o" "duos-detect-build:$ARCH" sh -c \
	"dpkg-deb --root-owner-group --build /o/$(basename "$D") /o/ &&
	 dpkg-deb --root-owner-group --build /o/$(basename "$C") /o/"
ls -l "$OUT"/*_"$ARCH".deb
