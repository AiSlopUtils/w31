#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
icon_dir="$project_dir/assets/icons"

if command -v sha256sum >/dev/null 2>&1; then
    (cd "$icon_dir" && sha256sum --check --status SHA256SUMS)
elif command -v shasum >/dev/null 2>&1; then
    (cd "$icon_dir" && shasum -a 256 --check SHA256SUMS >/dev/null)
else
    echo "check-icon-provenance: sha256sum or shasum is required" >&2
    exit 2
fi

expected=$(wc -l <"$icon_dir/SHA256SUMS" | tr -d ' ')
actual=$(find "$icon_dir" -type f \( -name '*.png' -o -name '*.ico' \) | wc -l | tr -d ' ')
if [ "$actual" != "$expected" ]; then
    echo "check-icon-provenance: expected $expected tracked assets, found $actual" >&2
    exit 1
fi

echo "supplied icon provenance checks passed"
