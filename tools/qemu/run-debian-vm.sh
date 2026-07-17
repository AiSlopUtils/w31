#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
vm_dir=${WIN31X_VM_DIR:-"$project_dir/.vm"}
disk_image="$vm_dir/win31x-debian.qcow2"
seed_iso="$vm_dir/seed.iso"
vars_file="$vm_dir/edk2-arm-vars.fd"

if ! command -v qemu-system-aarch64 >/dev/null 2>&1; then
    echo "run-debian-vm: qemu-system-aarch64 is required" >&2
    exit 2
fi
for required_file in "$disk_image" "$seed_iso" "$vars_file"; do
    if [ ! -f "$required_file" ]; then
        echo "run-debian-vm: missing $required_file" >&2
        echo "Run tools/qemu/prepare-debian-vm.sh first." >&2
        exit 2
    fi
done

firmware_root=${WIN31X_QEMU_FIRMWARE_DIR:-}
if [ -z "$firmware_root" ] && command -v brew >/dev/null 2>&1; then
    firmware_root=$(brew --prefix qemu)/share/qemu
fi
if [ -z "$firmware_root" ]; then
    firmware_root=/usr/share/qemu
fi
code_file="$firmware_root/edk2-aarch64-code.fd"
if [ ! -f "$code_file" ]; then
    echo "run-debian-vm: cannot find edk2-aarch64-code.fd under $firmware_root" >&2
    exit 2
fi

exec qemu-system-aarch64 \
    -name "Win31 X Debian" \
    -machine virt,highmem=on \
    -accel hvf \
    -cpu host \
    -smp 4 \
    -m 4096 \
    -drive "if=pflash,format=raw,unit=0,file=$code_file,readonly=on" \
    -drive "if=pflash,format=raw,unit=1,file=$vars_file" \
    -drive "if=none,id=system,file=$disk_image,format=qcow2,discard=unmap" \
    -device virtio-blk-pci,drive=system \
    -drive "if=none,id=seed,file=$seed_iso,format=raw,readonly=on" \
    -device virtio-blk-pci,drive=seed \
    -device virtio-gpu-pci \
    -device qemu-xhci \
    -device usb-kbd \
    -device usb-tablet \
    -device virtio-rng-pci \
    -nic user,model=virtio-net-pci,hostfwd=tcp:127.0.0.1:2222-:22 \
    -display cocoa,show-cursor=on \
    -serial stdio \
    -monitor none
