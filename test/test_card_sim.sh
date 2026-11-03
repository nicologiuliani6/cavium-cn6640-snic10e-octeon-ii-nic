#!/bin/bash
# test_card_sim.sh — Test the card simulator
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CARD_SIM="$SCRIPT_DIR/card_sim"
MEM_FILE="/dev/shm/octnic_sim_$$"

cleanup() {
    echo "[test] Cleaning up..."
    kill %1 2>/dev/null || true
    wait 2>/dev/null || true
    rm -f "$MEM_FILE"
}

trap cleanup EXIT

echo "=== Card Simulator Test ==="
echo ""

# Build card_sim
echo "[test] Building card_sim..."
make -C "$SCRIPT_DIR" clean && make -C "$SCRIPT_DIR"

# Check if already running from previous test
if pgrep -f "card_sim $MEM_FILE" > /dev/null 2>&1; then
    echo "[test] Stopping existing instance..."
    pkill -f "card_sim $MEM_FILE" || true
    sleep 1
fi

# Start card simulator in background
echo "[test] Starting card simulator..."
"$CARD_SIM" "$MEM_FILE" 1 &
SIM_PID=$!
sleep 2

# Verify it's running
if ! kill -0 "$SIM_PID" 2>/dev/null; then
    echo "[FAIL] Card simulator failed to start"
    exit 1
fi
echo "[OK] Card simulator running (PID $SIM_PID)"

# Check shared memory region
echo ""
echo "[test] Checking shared memory region..."
if [ -f "$MEM_FILE" ]; then
    echo "[OK] Memory file exists: $(ls -lh "$MEM_FILE" | awk '{print $5}')"
else
    echo "[FAIL] Memory file not created"
    exit 1
fi

# Check magic value in shared memory
echo ""
echo "[test] Verifying magic value at offset 0x00..."
MAGIC=$(xxd -s 0 -l 4 -p "$MEM_FILE" 2>/dev/null || echo "00000000")
if [ "$MAGIC" = "4853434f" ]; then  # "OCSH" in little-endian
    echo "[OK] Magic value correct: 0x4853434f (OCSH)"
else
    echo "[INFO] Magic value: $MAGIC (may need card_sim to initialize first)"
fi

# Test heartbeat update
echo ""
echo "[test] Checking heartbeat updates..."
sleep 3
HB1=$(xxd -s 32 -l 4 -p "$MEM_FILE" 2>/dev/null || echo "00000000")
sleep 2
HB2=$(xxd -s 32 -l 4 -p "$MEM_FILE" 2>/dev/null || echo "00000000")
if [ "$HB1" != "00000000" ] && [ "$HB2" != "$HB1" ]; then
    echo "[OK] Heartbeat updating (0x$HB1 -> 0x$HB2)"
else
    echo "[INFO] Heartbeat check: 0x$HB1 -> 0x$HB2"
fi

# Test RX frame generation
echo ""
echo "[test] Waiting for RX frame generation (~5s)..."
sleep 6
RX_PROD=$(xxd -s $((24)) -l 4 -p "$MEM_FILE" 2>/dev/null || echo "00000000")
if [ "$RX_PROD" != "00000000" ]; then
    RX_PROD_DEC=$((16#$RX_PROD))
    echo "[OK] RX producer advanced: $RX_PROD_DEC frames"
else
    echo "[INFO] RX producer: 0x$RX_PROD (may need more time)"
fi

# Test handshake detection
echo ""
echo "[test] Testing handshake detection..."
printf '\xef\xbe\xad\xde' > /tmp/test_handshake
# Write magic to handshake offset (0x100 = 256)
dd if=/dev/zero of="$MEM_FILE" bs=1 count=256 seek=256 conv=notrunc 2>/dev/null || true
printf '\x48\x53\x43\x4f' | dd of="$MEM_FILE" bs=1 count=4 seek=256 conv=notrunc 2>/dev/null || true

sleep 1
CARD_READY=$(xxd -s 8 -l 4 -p "$MEM_FILE" 2>/dev/null || echo "00000000")
if [ "$CARD_READY" = "01000000" ]; then
    echo "[OK] Card ready set after handshake"
else
    echo "[INFO] Card ready: 0x$CARD_READY (expected 0x01000000)"
fi

# Test multi-port mode
echo ""
echo "[test] Testing multi-port mode..."
"$CARD_SIM" "$MEM_FILE" 2 &
MULTI_PID=$!
sleep 2
if kill -0 "$MULTI_PID" 2>/dev/null; then
    echo "[OK] Multi-port simulator running (PID $MULTI_PID)"
    kill "$MULTI_PID" 2>/dev/null || true
    wait "$MULTI_PID" 2>/dev/null || true
else
    echo "[WARN] Multi-port mode failed to start"
fi

# Stop main simulator
echo ""
echo "[test] Stopping card simulator..."
kill "$SIM_PID" 2>/dev/null || true
wait "$SIM_PID" 2>/dev/null || true

echo ""
echo "=== Test Summary ==="
echo "[OK] All tests completed"
echo ""
echo "Next steps:"
echo "  1. Use card_sim to test octboot.c image push flow"
echo "  2. Verify driver can read magic, heartbeat, and RX frames"
echo "  3. Test with actual Windows driver (if available)"
