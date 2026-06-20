#!/bin/bash

set -u

TARGET_CMD="$1"
shift || true

REAL_PATH=$(command -v -- "$TARGET_CMD" 2>/dev/null || true)
if [ -z "$REAL_PATH" ]; then
    exec "$TARGET_CMD" "$@"
fi

ACTUAL_FILE=$(realpath "$REAL_PATH")

has_chromium_artifacts() {
    local dir="$1"

    [ -d "$dir" ] || return 1

    compgen -G "$dir/*.asar" >/dev/null ||
        compgen -G "$dir/resources/*.asar" >/dev/null ||
        [ -f "$dir/resources.pak" ] ||
        [ -f "$dir/chrome_100_percent.pak" ] ||
        [ -f "$dir/chrome_200_percent.pak" ] ||
        [ -f "$dir/icudtl.dat" ] ||
        [ -f "$dir/v8_context_snapshot.bin" ] ||
        [ -f "$dir/snapshot_blob.bin" ] ||
        [ -f "$dir/chrome-sandbox" ] ||
        [ -d "$dir/locales" ] ||
        [ -f "$dir/libEGL.so" ] ||
        [ -f "$dir/libGLESv2.so" ] ||
        [ -f "$dir/libvk_swiftshader.so" ]
}

binary_looks_chromiumish() {
    local bin="$1"

    [ -f "$bin" ] || return 1

    if file -Lb "$bin" | grep -qiE 'ELF'; then
        strings -a "$bin" 2>/dev/null |
            grep -qE 'ChromeMain|ELECTRON_RUN_AS_NODE|--type=renderer|--force-device-scale-factor|Chromium|Electron'
        return
    fi

    return 1
}

collect_candidate_bins() {
    local file="$1"

    printf '%s\n' "$file"

    if head -c 2 "$file" 2>/dev/null | grep -q '^#!'; then
        grep -oE '/[^"[:space:]'\'']+' "$file" 2>/dev/null |
            while IFS= read -r path; do
                [ -x "$path" ] && realpath "$path"
            done
    fi
}

is_chromium_or_electron() {
    local file="$1"
    local name
    local candidate
    local dir
    local parent
    local grandparent

    name=$(basename "$file")

    if [[ "${name,,}" == *electron* ||
        "${name,,}" == *chromium* ||
        "${name,,}" == *chrome* ]]; then
        return 0
    fi

    while IFS= read -r candidate; do
        [ -n "$candidate" ] || continue

        dir=$(dirname "$candidate")
        parent=$(dirname "$dir")
        grandparent=$(dirname "$parent")

        if has_chromium_artifacts "$dir" ||
            has_chromium_artifacts "$parent" ||
            has_chromium_artifacts "$grandparent"; then
            return 0
        fi

        if binary_looks_chromiumish "$candidate"; then
            return 0
        fi
    done < <(collect_candidate_bins "$file" | awk '!seen[$0]++')

    return 1
}

if is_chromium_or_electron "$ACTUAL_FILE"; then
    exec "$ACTUAL_FILE" \
        --force-device-scale-factor="${GDK_DPI_SCALE:-1}" \
        "$@"
else
    exec "$ACTUAL_FILE" "$@"
fi
