#!/bin/bash
#
# compile-appimage.sh - Build PvZ2Native and package a minimal AppImage.
#
# The AppImage contains ONLY:
#   - the pvz2 binary
#   - assets/  (UI splash/config/achievements images)
#
# Place next to the .AppImage (same folder as today's portable layout):
#   - libPVZ2.so
#   - data.obb  (or any *.obb the game expects)
#
# Requires: same tools as compile.sh, plus wget/curl to fetch appimagetool
# the first time. Pass-through flags are forwarded to compile.sh.
#

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
EXE_DIR="${BUILD_DIR}/pvz2native"
TOOLS_DIR="${PROJECT_ROOT}/tools"
APPIMAGETOOL="${TOOLS_DIR}/appimagetool-x86_64.AppImage"
APPDIR="${BUILD_DIR}/pvz2.AppDir"
OUT_APPIMAGE="${EXE_DIR}/pvz2.AppImage"
ARCH="x86_64"

if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
    RED=$'\e[31m'; GREEN=$'\e[32m'; YELLOW=$'\e[33m'; BLUE=$'\e[34m'; BOLD=$'\e[1m'; RESET=$'\e[0m'
else
    RED=''; GREEN=''; YELLOW=''; BLUE=''; BOLD=''; RESET=''
fi

info()    { echo "${BLUE}[INFO]${RESET} $*"; }
success() { echo "${GREEN}[OK]${RESET} $*"; }
warn()    { echo "${YELLOW}[WARN]${RESET} $*" >&2; }
error()   { echo "${RED}[ERROR]${RESET} $*" >&2; }
fail()    { error "$*"; exit 1; }

usage() {
    cat <<EOF
Usage: $0 [OPTIONS]

Runs ./compile.sh with the same options, then packs pvz2 + assets/ into
${OUT_APPIMAGE##*/} (written next to libPVZ2.so / data.obb in the build tree).

AppImage contents: binary + assets/ only.
Sibling files required at runtime: libPVZ2.so, data.obb

Options: same as compile.sh (--clean, --debug, --jobs N, --no-assets, …).
  -h, --help    Show this help
EOF
}

for arg in "$@"; do
    case "$arg" in
        -h|--help) usage; exit 0 ;;
    esac
done

if [[ "$(uname -m)" != "x86_64" ]]; then
    fail "AppImage packaging in this script is x86_64-only (host is $(uname -m))"
fi

info "Building with compile.sh..."
"${PROJECT_ROOT}/compile.sh" "$@"

[[ -f "${EXE_DIR}/pvz2" ]] || fail "Missing ${EXE_DIR}/pvz2"
[[ -d "${EXE_DIR}/assets" ]] || fail "Missing ${EXE_DIR}/assets (needed inside the AppImage)"

ensure_appimagetool() {
    if [[ -x "${APPIMAGETOOL}" ]]; then
        return 0
    fi
    mkdir -p "${TOOLS_DIR}"
    local url="https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"
    info "Downloading appimagetool → ${APPIMAGETOOL}"
    if command -v wget >/dev/null 2>&1; then
        wget -q -O "${APPIMAGETOOL}" "${url}" || fail "wget failed for appimagetool"
    elif command -v curl >/dev/null 2>&1; then
        curl -fsSL -o "${APPIMAGETOOL}" "${url}" || fail "curl failed for appimagetool"
    else
        fail "Need wget or curl to download appimagetool"
    fi
    chmod +x "${APPIMAGETOOL}"
}

ensure_appimagetool

info "Assembling AppDir at ${APPDIR}"
rm -rf "${APPDIR}"
mkdir -p "${APPDIR}/usr/bin"

cp -a "${EXE_DIR}/pvz2" "${APPDIR}/usr/bin/pvz2"
cp -a "${EXE_DIR}/assets" "${APPDIR}/usr/bin/assets"

# AppRun: exec the binary; AppImage runtime sets APPIMAGE/APPDIR for our path helpers.
cat > "${APPDIR}/AppRun" <<'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
exec "${HERE}/usr/bin/pvz2" "$@"
EOF
chmod +x "${APPDIR}/AppRun"

cat > "${APPDIR}/pvz2.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Plants vs. Zombies 2 (Native)
Comment=PvZ2 desktop port
Exec=pvz2
Icon=pvz2
Categories=Game;
Terminal=false
EOF

# AppImage desktop icon: prefer icon.png; else extract the largest frame from
# assets/icon.ico (that ICO ships many sizes; convert 'ico[0]' is a tiny 1-bit
# stub, so we pick the biggest PNG/BMP entry ourselves).
ICON_DST="${APPDIR}/pvz2.png"
ICO_SRC="${PROJECT_ROOT}/assets/icon.ico"
if [[ -f "${PROJECT_ROOT}/assets/icon.png" ]]; then
    cp -a "${PROJECT_ROOT}/assets/icon.png" "${ICON_DST}"
    success "Using assets/icon.png"
elif [[ -f "${ICO_SRC}" ]]; then
    info "Extracting largest icon from assets/icon.ico"
    if python3 - "${ICO_SRC}" "${ICON_DST}" <<'PY'
import struct, sys
from pathlib import Path
src, dst = Path(sys.argv[1]), Path(sys.argv[2])
data = src.read_bytes()
if len(data) < 6:
    sys.exit(1)
_reserved, typ, count = struct.unpack_from("<HHH", data, 0)
if typ != 1 or count < 1:
    sys.exit(1)
best = None  # (area, bpp, idx, offset, size, is_png)
for i in range(count):
    off = 6 + i * 16
    if off + 16 > len(data):
        break
    w, h, _colors, _res, _planes, bpp, size, img_off = struct.unpack_from("<BBBBHHII", data, off)
    w = 256 if w == 0 else w
    h = 256 if h == 0 else h
    if img_off + size > len(data) or size < 8:
        continue
    blob = data[img_off:img_off + size]
    is_png = blob[:8] == b"\x89PNG\r\n\x1a\n"
    score = (w * h, bpp, 1 if is_png else 0)
    if best is None or score > best[0]:
        best = (score, img_off, size, is_png, w, h)
if best is None:
    sys.exit(1)
_score, img_off, size, is_png, w, h = best
blob = data[img_off:img_off + size]
if is_png:
    dst.write_bytes(blob)
else:
    # Fallback: ask ImageMagick for that size if BMP entry won.
    import subprocess
    r = subprocess.run(
        ["convert", f"{src}[{w}x{h}]", str(dst)],
        capture_output=True,
    )
    if r.returncode != 0 or not dst.is_file() or dst.stat().st_size < 64:
        sys.exit(1)
print(f"{w}x{h} {'png' if is_png else 'bmp'}")
PY
    then
        success "Icon from icon.ico → $(file -b "${ICON_DST}")"
    elif command -v convert >/dev/null 2>&1; then
        # Last ImageMagick attempt: explode frames and keep the largest file.
        tmp_ico="${BUILD_DIR}/ico_frames"
        rm -rf "${tmp_ico}"
        mkdir -p "${tmp_ico}"
        if convert "${ICO_SRC}" "${tmp_ico}/frame.png" 2>/dev/null; then
            best=""
            best_bytes=0
            for f in "${tmp_ico}"/frame*.png; do
                [[ -f "$f" ]] || continue
                sz=$(wc -c <"$f")
                if (( sz > best_bytes )); then
                    best_bytes=$sz
                    best=$f
                fi
            done
            if [[ -n "${best}" ]]; then
                cp -a "${best}" "${ICON_DST}"
                success "Icon from icon.ico (largest convert frame)"
            fi
        fi
        rm -rf "${tmp_ico}"
    fi
fi
if [[ ! -f "${ICON_DST}" ]]; then
    # Minimal valid 1x1 PNG
    printf '\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x02\x00\x00\x00\x90wS\xde\x00\x00\x00\x0cIDATx\x9cc\xf8\x0f\x00\x00\x01\x01\x00\x05\x18\xd8N\x00\x00\x00\x00IEND\xaeB`\x82' > "${ICON_DST}"
    warn "Could not extract icon.ico; using a placeholder icon in the AppImage"
fi

# AppDir icon bits (embedded inside the AppImage only).
rm -f "${APPDIR}/.DirIcon"
cp -a "${ICON_DST}" "${APPDIR}/.DirIcon"
mkdir -p "${APPDIR}/usr/share/icons/hicolor/256x256/apps"
cp -a "${ICON_DST}" "${APPDIR}/usr/share/icons/hicolor/256x256/apps/pvz2.png"
mkdir -p "${APPDIR}/usr/share/applications"
cp -a "${APPDIR}/pvz2.desktop" "${APPDIR}/usr/share/applications/pvz2.desktop"

info "Running appimagetool..."
rm -f "${OUT_APPIMAGE}"
export ARCH
if ! "${APPIMAGETOOL}" "${APPDIR}" "${OUT_APPIMAGE}" 2>"${BUILD_DIR}/appimagetool.log"; then
    if grep -qi 'fuse\|AppImages require' "${BUILD_DIR}/appimagetool.log" 2>/dev/null; then
        info "Retrying appimagetool with APPIMAGE_EXTRACT_AND_RUN=1"
        APPIMAGE_EXTRACT_AND_RUN=1 "${APPIMAGETOOL}" "${APPDIR}" "${OUT_APPIMAGE}" \
            >>"${BUILD_DIR}/appimagetool.log" 2>&1 \
            || fail "appimagetool failed. See ${BUILD_DIR}/appimagetool.log"
    else
        fail "appimagetool failed. See ${BUILD_DIR}/appimagetool.log"
    fi
fi

chmod +x "${OUT_APPIMAGE}"

# Remove leftovers from earlier naming / experiments.
rm -f "${EXE_DIR}/PvZ2.AppImage" "${EXE_DIR}/PvZ2.desktop" "${EXE_DIR}/PvZ2.png"

echo ""
echo "${BOLD}AppImage ready${RESET}"
echo "  ${OUT_APPIMAGE}"
echo ""
echo "Runtime layout (same folder):"
echo "  pvz2.AppImage   ← this file"
echo "  libPVZ2.so"
echo "  data.obb"
echo ""
echo "config.ini / userdata / prefs are created next to the .AppImage on first run."
if [[ -f "${EXE_DIR}/libPVZ2.so" ]]; then
    success "libPVZ2.so already next to the AppImage"
else
    warn "libPVZ2.so missing in ${EXE_DIR} — copy it beside the AppImage before running"
fi
obb_ok=no
for f in "${EXE_DIR}"/*.obb; do
    if [[ -f "$f" ]]; then obb_ok=yes; break; fi
done
if [[ "${obb_ok}" == "yes" ]]; then
    success "OBB present next to the AppImage"
else
    warn "No .obb next to the AppImage — copy data.obb beside it before running"
fi
