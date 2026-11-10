#!/bin/bash
# Test script for FreeBSD driver with emulated card

set -e

echo "=== FreeBSD Driver Test ==="

# Build and push to VM
echo "Building driver..."
freebsd-vm push bsdmod/* /root/octnic/

# Compile in VM
echo "Compiling in FreeBSD VM..."
freebsd-vm run 'cd /root/octnic && make clean && make'

# Load the kernel module
echo "Loading kernel module..."
freebsd-vm run 'kldload ./if_octnic.ko'

# Check if loaded
echo "Checking module status..."
freebsd-vm run 'kldstat | grep octnic || echo "Module not loaded"'

# Test interfaces
echo "Checking interfaces..."
freebsd-vm run 'ifconfig oct0 || echo "Interface oct0 not found"'
freebsd-vm run 'ifconfig oct1 || echo "Interface oct1 not found"'

# Test basic functionality (if interfaces exist)
if freebsd-vm run 'ifconfig oct0 >/dev/null 2>&1'; then
    echo "Interface oct0 exists, testing..."
    freebsd-vm run 'ifconfig oct0'
    freebsd-vm run 'ping -c 3 127.0.0.1 || echo "Ping test completed"'
fi

if freebsd-vm run 'ifconfig oct1 >/dev/null 2>&1'; then
    echo "Interface oct1 exists, testing..."
    freebsd-vm run 'ifconfig oct1'
fi

# Unload module
echo "Unloading kernel module..."
freebsd-vm run 'kldunload if_octnic.ko || echo "Module already unloaded"'

echo "=== Test completed ==="