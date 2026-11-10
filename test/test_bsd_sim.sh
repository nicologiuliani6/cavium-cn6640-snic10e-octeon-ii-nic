#!/bin/bash
# Test script for FreeBSD driver with simulated card using ivshmem

set -e

echo "=== FreeBSD Driver Test with Simulated Card ==="

# Create shared memory for simulation
echo "Creating shared memory for simulation..."
SHM_SIZE=$((64 * 1024 * 1024))  # 64MB
SHM_PATH="/dev/shm/octnic_sim"
dd if=/dev/zero of="$SHM_PATH" bs=1 count=1 seek=$((SHM_SIZE - 1)) 2>/dev/null

# Build and push driver & simulator to VM
echo "Building driver and simulator..."
freebsd-vm push bsdmod/* /root/octnic/
freebsd-vm push test/card_sim /root/octnic_sim/ 2>/dev/null || true

# Compile in VM
echo "Compiling in FreeBSD VM..."
freebsd-vm run 'cd /root/octnic && make clean && make'

# Push simulation setup
echo "Setting up simulation in VM..."
freebsd-vm run "mkdir -p /root/octnic_sim"
freebsd-vm run "cp $SHM_PATH /root/octnic_sim/ivshmem" 2>/dev/null || true

# Load the kernel module
echo "Loading kernel module..."
freebsd-vm run 'kldload /root/octnic/if_octnic.ko || true'

# Check if loaded
echo "Checking module status..."
freebsd-vm run 'kldstat | grep octnic || echo "Module not loaded"'

# Check dmesg
echo "Checking driver logs..."
freebsd-vm run 'dmesg | tail -20'

# Clean up
echo "Cleaning up..."
freebsd-vm run 'kldunload if_octnic.ko || true'

echo "=== Test completed ==="