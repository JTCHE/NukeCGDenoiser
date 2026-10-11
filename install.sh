#!/usr/bin/env bash
# Nuke Denoiser - Build & Install (macOS)
# Detects installed Nuke SDKs, fetches OIDN if missing, builds denoiser.dylib from
# source, and stages the plugin into ~/.nuke/nuke-denoiser/ (or $NUKE_PATH).
# Idempotent: safe to run multiple times.
#
# Usage: ./install.sh [--nuke-path /Applications/Nuke17.1v2/Nuke17.1v2.app]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build/mac"
DEPS_DIR="$SCRIPT_DIR/build/deps"

# macOS uses OIDN 2.3.3, not 2.1.0: 2.1.0 has no arm64 macOS build. 2.3.3 links
# libtbb 2021.12, which Nuke 17's bundled libtbb 2021.13 satisfies. Do not bump
# past a version whose TBB is newer than the one in your Nuke's Contents/MacOS.
OIDN_VERSION='2.3.3'

PLUGIN_FOLDER='nuke-denoiser'
BLOCK_START='# --- nuke-denoiser START ---'
BLOCK_END='# --- nuke-denoiser END ---'

NUKE_PATH_ARG=''
while [[ $# -gt 0 ]]; do
    case "$1" in
        --nuke-path) NUKE_PATH_ARG="$2"; shift 2 ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

die() { echo "ERROR: $*" >&2; exit 1; }

# --- Nuke SDK detection ---
# Accepts an .app bundle, its Contents/MacOS folder, or the /Applications/NukeX.Yvz folder.
resolve_sdk() {
    local p="${1%/}"
    if [[ -f "$p/cmake/NukeConfig.cmake" ]]; then echo "$p"; return 0; fi
    if [[ -f "$p/Contents/MacOS/cmake/NukeConfig.cmake" ]]; then echo "$p/Contents/MacOS"; return 0; fi
    local app
    for app in "$p"/Nuke*.app; do
        if [[ -f "$app/Contents/MacOS/cmake/NukeConfig.cmake" ]]; then echo "$app/Contents/MacOS"; return 0; fi
    done
    return 1
}

select_sdk() {
    if [[ -n "$NUKE_PATH_ARG" ]]; then
        resolve_sdk "$NUKE_PATH_ARG" || die "No cmake/NukeConfig.cmake found under '$NUKE_PATH_ARG' - not a valid Nuke SDK."
        return
    fi

    local sdks=() dir sdk
    for dir in /Applications/Nuke*/; do
        [[ -d "$dir" ]] || continue
        sdk="$(resolve_sdk "$dir")" && sdks+=("$sdk")
    done
    [[ ${#sdks[@]} -gt 0 ]] || die "No Nuke install found in /Applications. Pass --nuke-path."

    if [[ ${#sdks[@]} -eq 1 ]]; then echo "${sdks[0]}"; return; fi

    echo 'Select a Nuke installation to build against:' >&2
    select sdk in "${sdks[@]}"; do
        [[ -n "$sdk" ]] && { echo "$sdk"; return; }
    done
    die 'No Nuke installation selected.'
}

# --- Toolchain checks ---
check_tools() {
    xcode-select -p >/dev/null 2>&1 || {
        xcode-select --install || true
        die 'Xcode Command Line Tools are required. Finish their install, then re-run this script.'
    }
    command -v cmake >/dev/null 2>&1 || die 'CMake not found. Install it (e.g. "brew install cmake"), then re-run.'
}

# --- OIDN fetch ---
get_oidn_root() {
    local arch="$1"
    local name="oidn-$OIDN_VERSION.$arch.macos"
    local root="$DEPS_DIR/$name"
    if [[ ! -f "$root/include/OpenImageDenoise/oidn.hpp" ]]; then
        echo "Downloading OIDN $OIDN_VERSION ($arch)..." >&2
        mkdir -p "$DEPS_DIR"
        curl -fL --progress-bar -o "$DEPS_DIR/$name.tar.gz" \
            "https://github.com/RenderKit/oidn/releases/download/v$OIDN_VERSION/$name.tar.gz"
        tar -xzf "$DEPS_DIR/$name.tar.gz" -C "$DEPS_DIR"
        [[ -f "$root/include/OpenImageDenoise/oidn.hpp" ]] || die "OIDN extraction failed - headers not found in $root"
    fi
    echo "$root"
}

# --- Nuke user dir + init.py patching ---
find_nuke_user_dir() {
    local p
    if [[ -n "${NUKE_PATH:-}" ]]; then
        IFS=':' read -ra parts <<< "$NUKE_PATH"
        for p in "${parts[@]}"; do
            [[ -n "$p" && -d "$p" ]] && { echo "$p"; return; }
        done
    fi
    echo "$HOME/.nuke"
}

update_init_py() {
    local init_py="$1"
    [[ -f "$init_py" ]] || echo 'import nuke' > "$init_py"

    # Drop the old guarded block and any stray legacy line, then append a fresh block.
    local tmp
    tmp="$(mktemp)"
    awk -v s="$BLOCK_START" -v e="$BLOCK_END" '
        { t = $0; gsub(/^[ \t]+|[ \t\r]+$/, "", t) }
        t == s { inside = 1; next }
        t == e { inside = 0; next }
        inside { next }
        index($0, "pluginAddPath(\x27./nuke-denoiser\x27)") { next }
        t == "" { blanks = blanks $0 "\n"; next }   # hold blank lines; drop trailing ones
        { printf "%s", blanks; blanks = ""; print }
    ' "$init_py" > "$tmp"
    printf '\n%s\n%s\n%s\n' "$BLOCK_START" "nuke.pluginAddPath('./nuke-denoiser')" "$BLOCK_END" >> "$tmp"
    cat "$tmp" > "$init_py"
    rm -f "$tmp"
    echo "  Patched: $init_py"
}

# --- Main ---
echo '=== Nuke Denoiser: Build & Install (macOS) ==='

NUKE_SDK="$(select_sdk)"
echo "Nuke SDK       : $NUKE_SDK"

check_tools
echo "CMake          : $(command -v cmake)"

# Build for the arch Nuke runs as. Universal binary: use the host arch.
NUKE_EXE="$(ls "$NUKE_SDK"/Nuke[0-9]* 2>/dev/null | head -1)"
ARCH="$(uname -m)"
if [[ -n "$NUKE_EXE" ]]; then
    NUKE_ARCHS="$(lipo -archs "$NUKE_EXE" 2>/dev/null || echo "$ARCH")"
    [[ " $NUKE_ARCHS " == *" $ARCH "* ]] || ARCH="${NUKE_ARCHS%% *}"
fi
echo "Arch           : $ARCH"

OIDN_ROOT="$(get_oidn_root "$ARCH")"
echo "OIDN           : $OIDN_ROOT"

echo 'Configuring...'
cmake -B "$BUILD_DIR" -S "$SCRIPT_DIR" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
    -DOIDN_ROOT="$OIDN_ROOT" -DCMAKE_PREFIX_PATH="$NUKE_SDK" >/dev/null

echo 'Building...'
cmake --build "$BUILD_DIR" --config Release -j

BUILT="$BUILD_DIR/lib/Denoiser.dylib"
[[ -f "$BUILT" ]] || die "Build succeeded but Denoiser.dylib not found at $BUILT"

NUKE_USER_DIR="$(find_nuke_user_dir)"
DST="$NUKE_USER_DIR/$PLUGIN_FOLDER"
DST_LIB="$DST/oidn/lib"
mkdir -p "$DST_LIB"

echo "Staging plugin into $DST..."
cp -f "$BUILT" "$DST/denoiser.dylib"
cp -f "$SCRIPT_DIR/init.py" "$SCRIPT_DIR/menu.py" "$DST/"

NUKE_MAJOR=''
[[ "$NUKE_SDK" =~ Nuke([0-9]+)\. ]] && NUKE_MAJOR="${BASH_REMATCH[1]}"
printf '%s' "$NUKE_MAJOR" > "$DST/built_for_nuke_version.txt"

# Copy OIDN libs with their symlinks. Skip libtbb: Nuke ships its own, and a second
# TBB runtime in the same process is what crashes Nuke.
rm -f "$DST_LIB"/libOpenImageDenoise*.dylib
cp -a "$OIDN_ROOT"/lib/libOpenImageDenoise*.dylib "$DST_LIB/"

echo 'Patching init.py...'
update_init_py "$NUKE_USER_DIR/init.py"

echo ''
echo 'Installation complete. Restart Nuke to apply changes.'
