#!/bin/bash
O=/home/nico/openwrt
L=/home/nico/Desktop/cavium/.build.log
est() {
  if ls $O/bin/targets/octeon/generic/*snic10e*initramfs*.bin >/dev/null 2>&1; then echo "100% - IMAGE READY"; return; fi
  if grep -q "Building images\|Image:.*snic10e\|DONE_MARKER" $L 2>/dev/null; then echo "~92% - building image"; return; fi
  # count built target packages
  ipk=$(find $O/bin/packages $O/bin/targets -name '*.ipk' 2>/dev/null | wc -l)
  if ls $O/build_dir/target-*/linux-octeon*/vmlinux >/dev/null 2>&1; then echo "~85% - kernel linked, packaging (${ipk} pkgs)"; return; fi
  if ls -d $O/build_dir/target-*/linux-octeon*/linux-5.10* >/dev/null 2>&1; then echo "~55-80% - kernel+pkgs compiling (${ipk} pkgs built)"; return; fi
  if [ "$(du -sm $O/staging_dir 2>/dev/null|cut -f1)" -gt 300 ] 2>/dev/null; then echo "~40% - toolchain done, compiling packages (${ipk} pkgs)"; return; fi
  if ls -d $O/build_dir/toolchain-* >/dev/null 2>&1; then echo "~20% - building MIPS toolchain"; return; fi
  if grep -q "compiling\|tools/" $L 2>/dev/null && [ "$(du -sm $O/build_dir 2>/dev/null|cut -f1)" -gt 50 ] 2>/dev/null; then echo "~12% - building host tools"; return; fi
  echo "~8% - downloading sources"
}
while ! grep -q "DONE_MARKER" $L 2>/dev/null; do
  echo "BUILD PROGRESS: $(est)  | disk free $(df -h / | awk 'NR==2{print $4}')"
  sleep 600
done
echo "BUILD PROGRESS: build finished (DONE_MARKER) - checking result"
