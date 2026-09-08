#!/bin/bash
# Autonomous OpenWrt snic10e build. Logs to .build.log. Disk-guard removes the
# riscv toolchain (user pre-authorized) only if free space drops below 3 GB.
DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
OPENWRT_DIR=${OPENWRT_DIR:-$HOME/openwrt}
RISCV_TOOLCHAIN=${RISCV_TOOLCHAIN:-$HOME/riscv-gnu-toolchain}
LOG=${LOG:-$DIR/.build.log}
exec > "$LOG" 2>&1
cd "$OPENWRT_DIR" || { echo "[build] OPENWRT_DIR not found: $OPENWRT_DIR"; exit 1; }
# Python 3.13 (Ubuntu 25.04) dropped 'pipes'/'distutils' that OpenWrt-5.10 host tools need.
export PYTHONPATH="$OPENWRT_DIR/host-pyshim"
export SETUPTOOLS_USE_DISTUTILS=local
# host gcc-14 turns several old-code warnings into hard errors; wrappers downgrade them
export PATH="$OPENWRT_DIR/host-ccwrap:$PATH"

echo "[build] feeds install..."
./scripts/feeds install -a

echo "[build] config..."
cp "$DIR/snic10e.config" .config
make defconfig

# Point OpenWrt's host compiler symlinks at our gcc-14 wrappers (it invokes
# staging_dir/host/bin/gcc, not PATH) so old-code default-errors are downgraded.
for n in gcc cc g++ c++; do
  ln -sf "$OPENWRT_DIR/host-ccwrap/$n" "$OPENWRT_DIR/staging_dir/host/bin/$n"
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
