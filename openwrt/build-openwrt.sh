#!/bin/bash
# Autonomous OpenWrt snic10e build. Logs to .build.log. Disk-guard removes the
# riscv toolchain (user pre-authorized) only if free space drops below 3 GB.
#
# Source of the released card image (GPL-2.0 corresponding source):
#   upstream  https://codeberg.org/stintel/openwrt.git  branch snic10e-5.10  commit 7bbf4b7
#   + openwrt/patches/*.diff   (applied here, on top of that commit)
#   + openwrt/snic10e.config   (the .config used)
#   + openwrt/files/           (root overlay: rc.local + the card modules)
# Clone it yourself with: OPENWRT_DIR=<dir> CLONE=1 ./openwrt/build-openwrt.sh
DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
OPENWRT_DIR=${OPENWRT_DIR:-$HOME/openwrt}
OPENWRT_URL=${OPENWRT_URL:-https://codeberg.org/stintel/openwrt.git}
OPENWRT_REF=${OPENWRT_REF:-7bbf4b7}
RISCV_TOOLCHAIN=${RISCV_TOOLCHAIN:-$HOME/riscv-gnu-toolchain}
LOG=${LOG:-$DIR/.build.log}
exec > "$LOG" 2>&1
if [ "${CLONE:-0}" = 1 ] && [ ! -d "$OPENWRT_DIR/.git" ]; then
  echo "[build] cloning $OPENWRT_URL -> $OPENWRT_DIR"
  git clone "$OPENWRT_URL" "$OPENWRT_DIR" || exit 1
  git -C "$OPENWRT_DIR" checkout "$OPENWRT_REF" || exit 1
fi
cd "$OPENWRT_DIR" || { echo "[build] OPENWRT_DIR not found: $OPENWRT_DIR"; exit 1; }
# the one local change to upstream that the released image was built with
for p in "$DIR"/patches/*.diff; do
  [ -f "$p" ] || continue
  git apply --check "$p" 2>/dev/null && { git apply "$p"; echo "[build] applied $(basename "$p")"; } \
    || echo "[build] $(basename "$p") already applied or does not fit this tree"
done
# Python 3.13 (Ubuntu 25.04) dropped 'pipes'/'distutils' that OpenWrt-5.10 host tools need.
export PYTHONPATH="$DIR/hostfix/pyshim"
export SETUPTOOLS_USE_DISTUTILS=local
# host gcc-14 turns several old-code warnings into hard errors; wrappers downgrade them
export PATH="$DIR/hostfix/ccwrap:$PATH"

echo "[build] feeds install..."
./scripts/feeds install -a

echo "[build] config..."
cp "$DIR/snic10e.config" .config
make defconfig

# Point OpenWrt's host compiler symlinks at our gcc-14 wrappers (it invokes
# staging_dir/host/bin/gcc, not PATH) so old-code default-errors are downgraded.
for n in gcc cc g++ c++; do
  ln -sf "$DIR/hostfix/ccwrap/$n" "$OPENWRT_DIR/staging_dir/host/bin/$n"
done

# disk guard
( set +x; while true; do
    free=$(df --output=avail / | tail -1 | tr -d ' ')
    if [ "${free:-9999999}" -lt 3145728 ] && [ -d "$RISCV_TOOLCHAIN" ]; then
      echo "[guard] free=${free}KB < 3GB -> removing $RISCV_TOOLCHAIN"
      rm -rf "$RISCV_TOOLCHAIN"
    fi
    sleep 20
  done ) &
GUARD=$!

echo "[build] download sources..."
make -j"$(nproc)" download

echo "[build] compiling (this is the long part)..."
if make -j"$(nproc)"; then
  echo "[build] PARALLEL BUILD OK"
else
  echo "[build] parallel failed, retry single-threaded verbose for the error..."
  make -j1 V=s
fi
RC=$?
kill $GUARD 2>/dev/null

echo "[build] EXIT_CODE=$RC"
echo "[build] images:"
ls -la bin/targets/octeon/generic/ 2>/dev/null
echo "[build] DONE_MARKER"
