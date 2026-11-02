#!/usr/bin/env bash
# Boot a kernel + initramfs in QEMU with an edu PCI card and print the result.
# Usage: run-qemu.sh <bzImage> <initramfs.cpio.gz>
set -euo pipefail

KERNEL=${1:?bzImage}
INITRD=${2:?initramfs}
QEMU=${QEMU:-qemu-system-x86_64}
LOG=${LOG:-qemu.log}

timeout 300 "$QEMU" \
    -m 512M -smp 2 \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -append "console=ttyS0 panic=-1 oops=panic" \
    -device edu \
    -nographic -no-reboot -monitor none -serial stdio | tee "$LOG"

grep -q "RESULT: PASS" "$LOG"
