#!/bin/sh
# Report u-boot env status over /proc/octshm/env -> ctrl page +0x200 (host reads BAR2 +0x200).
# No serial, no card login.
#
# FINDING (HW-confirmed 2026-07-11): u-boot's REAL env — the one octboot depends on, holding
# bootdelay=1 + the custom bootcmd ending in `bootoctlinux` — is NOT in NOR. A full scan of
# every NOR mtd partition (mtd0 bootloader / mtd1 rootfs_data / mtd2 environment) found no
# valid env whose bootcmd contains "bootoctlinux". It lives in NAND (1GiB), which this OpenWrt
# kernel does not expose as an mtd -> fw_setenv from Linux cannot reach it. The NOR partition
# literally named "environment" (mtd2) is BLANK (u-boot ran compiled defaults there) = a decoy.
#
# Consequence: bootdelay=1 (and the octboot bootcmd) is already PERMANENT — set once via serial
# `saveenv` and proven every boot by octboot working. To change u-boot env on this board you
# still need the serial console once (see docs/FLASHING §3). fw_setenv here only writes the NOR
# decoy, which u-boot ignores. We still point fw_env.config at it so the tool doesn't error.
pub() { printf '%s' "$1" > /proc/octshm/env 2>/dev/null; }

LINE=$(grep '"environment"' /proc/mtd 2>/dev/null | head -1)
if [ -z "$LINE" ]; then
  pub "ENV_NOMTD (no NOR environment partition; u-boot env in NAND)"
  exit 0
fi
NUM=$(echo "$LINE" | sed -n 's/^mtd\([0-9]*\):.*/\1/p')
ESZ=0x$(echo "$LINE" | awk '{print $3}')
printf '/dev/mtd%s 0x0 %s\n' "$NUM" "$ESZ" > /etc/fw_env.config
pub "ENV_NAND: u-boot env in NAND (not fw_setenv-reachable). bootdelay=1 already permanent (octboot). NOR /dev/mtd$NUM=writable decoy."
