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

    # Chromium/Electron-*specific* artifacts only. Deliberately excludes
    # generic GL libs (libEGL.so / libGLESv2.so): those are system Mesa
    # libraries that live in /usr/lib on every desktop and are NOT evidence
    # that the launched binary is Chromium/Electron.
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
        [ -f "$dir/libvk_swiftshader.so" ]
}

binary_looks_chromiumish() {
    local bin="$1"

    [ -f "$bin" ] || return 1

    if file -Lb "$bin" | grep -qiE 'ELF'; then
        # Word boundaries on Chromium/Electron so we don't match substrings
        # like "Electronic" (present in e.g. REAPER's binary) or a
        # "Chromium/123" user-agent fragment embedded by an unrelated app.
        strings -a "$bin" 2>/dev/null |
            grep -qE 'ChromeMain|ELECTRON_RUN_AS_NODE|--type=renderer|--force-device-scale-factor|\bChromium\b|\bElectron\b'
        return
    fi

    return 1
}

# Given a shebang script, attempt to resolve the absolute path it exec's,
# expanding simple variables the script itself sets (e.g. name=electron42),
# WITHOUT running the program. This handles launchers whose final line is
# something like:  exec /usr/lib/${name}/electron "$@"
# where the real target only exists after parameter expansion.
#
# Safety: assignments and the target are filtered to reject command
# substitution ($(...) / backticks) before any eval, and the result is
# only accepted if it points at a real executable file.
resolve_exec_target() {
    local file="$1"
    local target assigns expanded

    target=$(grep -oE '^[[:space:]]*exec[[:space:]]+[^[:space:]]+' "$file" 2>/dev/null |
        head -n1 | awk '{print $2}')
    [ -n "$target" ] || return 1

    case "$target" in
        *'$('* | *'`'*) return 1 ;;
    esac

    # Only simple, substitution-free name=value assignments are considered.
    assigns=$(grep -oE '^[[:space:]]*[a-zA-Z_][a-zA-Z0-9_]*=[^`$(]*$' "$file" 2>/dev/null)

    expanded=$(
        eval "$assigns" 2>/dev/null
        eval "printf '%s' \"$target\"" 2>/dev/null
    )

    [ -n "$expanded" ] || return 1
    [ -x "$expanded" ] && [ -f "$expanded" ] || return 1
    realpath "$expanded"
}

collect_candidate_bins() {
    local file="$1"
    local depth="${2:-0}"

    # Guard against runaway recursion / cycles in wrapper chains.
    [ "$depth" -gt 6 ] && return 0

    printf '%s\n' "$file"

    head -c 2 "$file" 2>/dev/null | grep -q '^#!' || return 0

    local path rp resolved word target

    # 1) Absolute paths referenced literally in the script; recurse into any
    #    that are themselves scripts (wrapper -> wrapper -> real binary).
    while IFS= read -r path; do
        [ -x "$path" ] || continue
        rp=$(realpath "$path")
        collect_candidate_bins "$rp" "$((depth + 1))"
    done < <(grep -oE '/[^"[:space:]'\'']+' "$file" 2>/dev/null)

    # 2) Bare command names that are exec'd, resolved via PATH, then recursed
    #    into (e.g. `exec electron .` -> /usr/bin/electron, itself a script).
    while IFS= read -r word; do
        resolved=$(command -v -- "$word" 2>/dev/null) || continue
        case "$resolved" in
            /*)
                rp=$(realpath "$resolved")
                collect_candidate_bins "$rp" "$((depth + 1))"
                ;;
        esac
    done < <(grep -oE '(^|[[:space:]])exec[[:space:]]+[a-zA-Z][a-zA-Z0-9._-]*' "$file" 2>/dev/null |
        awk '{print $2}')

    # 3) Variable-interpolated exec target (e.g. exec /usr/lib/${name}/electron).
    #    Resolved by expanding the script's own simple assignments.
    target=$(resolve_exec_target "$file") || target=""
    if [ -n "$target" ]; then
        collect_candidate_bins "$target" "$((depth + 1))"
    fi
}

is_chromium_or_electron() {
    local file="$1"
    local name candidate dir parent

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

        # Never treat a shared system library directory as the app's own
        # directory; otherwise a binary in /usr/lib/<app>/ would inherit
        # /usr/lib's contents (Mesa, an unrelated app's swiftshader, etc.)
        # as if they were its own Chromium artifacts. The app's real dir
        # (e.g. /usr/lib/electron42) is still checked as `dir`.
        case "$parent" in
            /usr/lib | /usr/lib64 | /usr/local/lib | /lib | /lib64)
                parent=""
                ;;
        esac

        if has_chromium_artifacts "$dir" ||
            { [ -n "$parent" ] && has_chromium_artifacts "$parent"; }; then
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
