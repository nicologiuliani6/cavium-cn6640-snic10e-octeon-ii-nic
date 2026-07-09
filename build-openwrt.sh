#!/bin/bash
# Autonomous OpenWrt snic10e build. Logs to .build.log. Disk-guard removes the
# riscv toolchain (user pre-authorized) only if free space drops below 3 GB.
LOG=/home/nico/Desktop/cavium/.build.log
exec > "$LOG" 2>&1
cd /home/nico/openwrt || exit 1
# Python 3.13 (Ubuntu 25.04) dropped 'pipes'/'distutils' that OpenWrt-5.10 host tools need.
export PYTHONPATH=/home/nico/openwrt/host-pyshim
export SETUPTOOLS_USE_DISTUTILS=local
# host gcc-14 turns several old-code warnings into hard errors; wrappers downgrade them
export PATH=/home/nico/openwrt/host-ccwrap:$PATH

echo "[build] feeds install..."
./scripts/feeds install -a

echo "[build] config..."
cp /home/nico/Desktop/cavium/snic10e.config .config
make defconfig

# Point OpenWrt's host compiler symlinks at our gcc-14 wrappers (it invokes
# staging_dir/host/bin/gcc, not PATH) so old-code default-errors are downgraded.
for n in gcc cc g++ c++; do
  ln -sf /home/nico/openwrt/host-ccwrap/$n /home/nico/openwrt/staging_dir/host/bin/$n
done

# disk guard
( set +x; while true; do
    free=$(df --output=avail / | tail -1 | tr -d ' ')
    if [ "${free:-9999999}" -lt 3145728 ] && [ -d /home/nico/riscv-gnu-toolchain ]; then
      echo "[guard] free=${free}KB < 3GB -> removing riscv-gnu-toolchain"
      rm -rf /home/nico/riscv-gnu-toolchain
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
