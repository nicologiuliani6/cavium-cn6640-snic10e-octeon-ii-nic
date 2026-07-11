#!/bin/sh
# Configure serial-free fw_setenv, no serial / no card shell needed.
# Progress + verdict stream to /proc/octshm/env -> ctrl page +0x200; host reads BAR2 +0x200.
#
# The card's NOR exposes a partition named "environment" in /proc/mtd (the u-boot
# CONFIG_ENV_* location). Point /etc/fw_env.config at it so fw_setenv/fw_printenv work.
# Verify: read the env; if it has no valid CRC yet (blank/default) initialise it once with
# a probe fw_setenv (that is exactly what saveenv does). A valid round-trip = FWENV_RW_OK,
# proving serial-free persistence. From then on read-only (no per-boot NOR wear).
#
# To persist real settings serial-free: add `fw_setenv KEY VAL` lines below, rebake, octboot
# once. u-boot reads this same partition on the next cold boot.
pub() { printf '%s' "$1" > /proc/octshm/env 2>/dev/null; }

pub "START"
LINE=$(grep '"environment"' /proc/mtd 2>/dev/null | head -1)
if [ -z "$LINE" ]; then
  MTDS=$(sed 's/[[:space:]]\+/ /g' /proc/mtd 2>/dev/null | tr '\n' '|')
  pub "FWENV_NOPART mtd=[$MTDS]"
  exit 0
fi
NUM=$(echo "$LINE" | sed -n 's/^mtd\([0-9]*\):.*/\1/p')
ESZ=0x$(echo "$LINE" | awk '{print $3}')
MTD="/dev/mtd$NUM"
printf '%s 0x0 %s\n' "$MTD" "$ESZ" > /etc/fw_env.config

# Read-verify first (no write if the env is already valid).
ERR=$(fw_printenv 2>&1 >/tmp/e.out)
if [ $? -eq 0 ] && ! echo "$ERR" | grep -qiE 'bad crc|cannot|default|error'; then
  BC=$(sed -n 's/^bootcmd=//p' /tmp/e.out | head -c 200)
  pub "FWENV_OK $MTD 0x0 $ESZ | bootcmd=${BC:-<none>}"
  exit 0
fi

# Blank/default env -> initialise once and prove the round-trip.
TS=$(cut -d' ' -f1 /proc/uptime)
if fw_setenv envdiag_probe "$TS" 2>/tmp/e.err; then
  RB=$(fw_printenv envdiag_probe 2>/dev/null | sed -n 's/^envdiag_probe=//p')
  if [ "$RB" = "$TS" ]; then
    pub "FWENV_RW_OK $MTD 0x0 $ESZ | probe=$RB (env initialised, persists serial-free)"
    exit 0
  fi
  pub "FWENV_RW_MISMATCH $MTD 0x0 $ESZ wrote=$TS read=$RB"
  exit 0
fi
pub "FWENV_WRITE_FAIL $MTD 0x0 $ESZ err=$(head -c 80 /tmp/e.err)"
