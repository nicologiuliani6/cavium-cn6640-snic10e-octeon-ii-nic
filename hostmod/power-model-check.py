#!/usr/bin/env python3
# Check the octnic power1 model arithmetic (hostmod/octnic.c oct_power_mw): same integer
# expression, so a units/overflow slip shows up here instead of in `sensors`. Run: python3 this.
HZ = 250


def mw(base, gbps_coef, delta_bytes, dj):
    rate = (delta_bytes * 8 * HZ * gbps_coef) // (dj * 1000000000) if dj else 0
    return base + rate


# no traffic -> baseline only (matches the live card reading)
assert mw(18000, 700, 0, HZ) == 18000
# 10 Gbit/s for one second -> +7 W of traffic term
assert mw(18000, 700, 1_250_000_000, HZ) == 25000
# 1 Gbit/s for one second -> +0.7 W
assert mw(18000, 700, 125_000_000, HZ) == 18700
# 2x10 Gbit/s over a 4 s window: no u64 overflow, still 14 W of traffic term
assert mw(18000, 700, 10_000_000_000, 4 * HZ) == 32000
print("power model OK")
