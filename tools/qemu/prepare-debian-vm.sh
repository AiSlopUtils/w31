#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
vm_dir=${WIN31X_VM_DIR:-"$project_dir/.vm"}
image_name=debian-13-generic-arm64.qcow2
image_url=${WIN31X_DEBIAN_IMAGE_URL:-"https://cloud.debian.org/images/cloud/trixie/latest/$image_name"}
sums_url=${WIN31X_DEBIAN_SUMS_URL:-"https://cloud.debian.org/images/cloud/trixie/latest/SHA512SUMS"}
base_image="$vm_dir/$image_name"
disk_image="$vm_dir/win31x-debian.qcow2"
seed_iso="$vm_dir/seed.iso"
vars_file="$vm_dir/edk2-arm-vars.fd"
seed_stage=$(mktemp -d "${TMPDIR:-/tmp}/win31x-seed.XXXXXX")

cleanup() {
    rm -rf "$seed_stage"
}
trap cleanup EXIT HUP INT TERM

for command_name in curl qemu-img tar; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        echo "prepare-debian-vm: $command_name is required" >&2
        exit 2
    fi
done
if ! command -v sha512sum >/dev/null 2>&1 &&
   ! command -v shasum >/dev/null 2>&1; then
    echo "prepare-debian-vm: sha512sum or shasum is required" >&2
    exit 2
fi

mkdir -p "$vm_dir"

if [ ! -f "$base_image" ]; then
    echo "Downloading $image_url"
    curl --fail --location --silent --show-error \
        -o "$vm_dir/SHA512SUMS.part" "$sums_url"
    mv "$vm_dir/SHA512SUMS.part" "$vm_dir/SHA512SUMS"
    curl --fail --location --progress-bar -o "$base_image.part" "$image_url"
elif [ ! -f "$vm_dir/SHA512SUMS" ]; then
    echo "prepare-debian-vm: cached base image has no pinned SHA512SUMS" >&2
    echo "Remove $base_image and rerun preparation to download a verified pair." >&2
    exit 1
fi

expected=$(awk -v image="$image_name" '$2 == image { print $1; exit }' "$vm_dir/SHA512SUMS")
if [ -z "$expected" ]; then
    echo "prepare-debian-vm: image checksum was not present in SHA512SUMS" >&2
    exit 1
fi
image_to_verify=$base_image
if [ ! -f "$base_image" ] && [ -f "$base_image.part" ]; then
    image_to_verify=$base_image.part
fi
if command -v sha512sum >/dev/null 2>&1; then
    actual=$(sha512sum "$image_to_verify" | awk '{print $1}')
else
    actual=$(shasum -a 512 "$image_to_verify" | awk '{print $1}')
fi
if [ "$actual" != "$expected" ]; then
    echo "prepare-debian-vm: Debian image checksum did not match the pinned sum" >&2
    exit 1
fi
if [ "$image_to_verify" = "$base_image.part" ]; then
    mv "$base_image.part" "$base_image"
fi
qemu-img check "$base_image" >/dev/null

if [ ! -f "$disk_image" ]; then
    qemu-img create -f qcow2 -F qcow2 -b "$base_image" "$disk_image" 24G
else
    qemu-img check "$disk_image" >/dev/null
fi

firmware_root=${WIN31X_QEMU_FIRMWARE_DIR:-}
if [ -z "$firmware_root" ] && command -v brew >/dev/null 2>&1; then
    firmware_root=$(brew --prefix qemu)/share/qemu
fi
if [ -z "$firmware_root" ]; then
    firmware_root=/usr/share/qemu
fi
if [ ! -f "$firmware_root/edk2-arm-vars.fd" ]; then
    echo "prepare-debian-vm: cannot find edk2-arm-vars.fd under $firmware_root" >&2
    exit 2
fi
if [ ! -f "$vars_file" ]; then
    cp "$firmware_root/edk2-arm-vars.fd" "$vars_file"
fi

cp "$project_dir/tools/qemu/cloud-init/user-data" "$seed_stage/user-data"
instance_id=$(date -u +win31x-%Y%m%d%H%M%S)
printf 'instance-id: %s\nlocal-hostname: win31x-debian\n' "$instance_id" >"$seed_stage/meta-data"
COPYFILE_DISABLE=1 tar --no-xattrs --exclude='./.vm' --exclude='./build' \
    -czf "$seed_stage/source.tgz" -C "$project_dir" .

rm -f "$seed_iso"
if command -v hdiutil >/dev/null 2>&1; then
    hdiutil makehybrid -iso -joliet -default-volume-name cidata \
        -o "$seed_iso" "$seed_stage" >/dev/null
elif command -v genisoimage >/dev/null 2>&1; then
    genisoimage -quiet -output "$seed_iso" -volid cidata -joliet -rock "$seed_stage"
elif command -v xorriso >/dev/null 2>&1; then
    xorriso -as mkisofs -quiet -output "$seed_iso" -volid cidata \
        -joliet -rock "$seed_stage"
else
    echo "prepare-debian-vm: hdiutil, genisoimage, or xorriso is required" >&2
    exit 2
fi

echo "Debian VM prepared in $vm_dir"
echo "Run: $project_dir/tools/qemu/run-debian-vm.sh"
