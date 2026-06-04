#!/usr/bin/env bash
#
# Build fcitx5-skk-mozc without Nix.
#
# What this does:
#   1. Clones the pinned upstream fcitx5-skk and mozc into ./build/upstream/.
#   2. Stages this repo's src/ files into fcitx5-skk's src/ tree.
#   3. Copies the necessary .proto files from the mozc tree.
#   4. Applies patches/fcitx5-skk/*.patch.
#   5. Runs cmake + ninja to produce skk.so.
#   6. Optionally installs into $PREFIX (default: /usr/local).
#
# Required system packages (Debian/Ubuntu names; equivalents on other
# distros work):
#   git, cmake, ninja-build, pkg-config, gettext, extra-cmake-modules,
#   libprotobuf-dev, protobuf-compiler,
#   fcitx5, libfcitx5core-dev, libfcitx5config-dev, libfcitx5utils-dev,
#   libskk-dev, libglib2.0-dev,
#   gobject-introspection, libgirepository1.0-dev
#
# Runtime: mozc (the `mozc_server` binary). On Debian/Ubuntu: `mozc-server`.

set -euo pipefail

# --- locate ourselves -------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# shellcheck disable=SC1091
source "${SCRIPT_DIR}/versions.env"

BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"
UPSTREAM_DIR="${BUILD_DIR}/upstream"
WORK_DIR="${BUILD_DIR}/work"
PREFIX="${PREFIX:-/usr/local}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
DO_INSTALL="${INSTALL:-0}"

log() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m!!\033[0m %s\n' "$*" >&2; exit 1; }

# --- preflight --------------------------------------------------------------
need() { command -v "$1" >/dev/null 2>&1 || die "missing tool: $1"; }
need git
need cmake
need pkg-config
need protoc
need ninja

# --- step 1: fetch upstream sources ----------------------------------------
fetch_repo() {
    local name="$1" url="$2" rev="$3" dest="$4"
    shift 4
    local extra_clone_opts=("$@")
    if [[ -d "${dest}/.git" ]]; then
        log "${name}: updating to ${rev}"
        git -C "${dest}" fetch --depth 1 origin "${rev}" >/dev/null 2>&1 || \
            git -C "${dest}" fetch origin >/dev/null
        git -C "${dest}" checkout --quiet "${rev}"
    else
        log "${name}: cloning ${url} @ ${rev}"
        mkdir -p "$(dirname "${dest}")"
        # Shallow + single revision keeps the working copy under ~20 MB.
        git clone --quiet --filter=blob:none "${extra_clone_opts[@]}" \
            "${url}" "${dest}"
        git -C "${dest}" fetch --depth 1 origin "${rev}" >/dev/null
        git -C "${dest}" checkout --quiet "${rev}"
    fi
}

# fcitx5-skk: full checkout (we copy the whole tree).
fetch_repo fcitx5-skk "${FCITX5_SKK_REPO}" "${FCITX5_SKK_REV}" \
    "${UPSTREAM_DIR}/fcitx5-skk" --depth 1

# mozc: sparse checkout — we only need src/protocol/*.proto and
# src/ipc/ipc.proto. Cuts a multi-hundred-MB clone to a handful of files.
fetch_repo mozc "${MOZC_REPO}" "${MOZC_REV}" \
    "${UPSTREAM_DIR}/mozc" --sparse --no-checkout
git -C "${UPSTREAM_DIR}/mozc" sparse-checkout init --cone >/dev/null
git -C "${UPSTREAM_DIR}/mozc" sparse-checkout set src/protocol src/ipc >/dev/null
git -C "${UPSTREAM_DIR}/mozc" checkout --quiet "${MOZC_REV}"

# --- step 2: stage a clean working copy ------------------------------------
log "preparing work dir at ${WORK_DIR}"
rm -rf "${WORK_DIR}"
mkdir -p "${WORK_DIR}"
cp -a "${UPSTREAM_DIR}/fcitx5-skk/." "${WORK_DIR}/"

# Our additional sources go alongside skk.cpp.
cp -r "${REPO_ROOT}/src/mozc_client"      "${WORK_DIR}/src/mozc_client"
cp -r "${REPO_ROOT}/src/candidate_merger" "${WORK_DIR}/src/candidate_merger"
cp -r "${REPO_ROOT}/src/bunsetsu"         "${WORK_DIR}/src/bunsetsu"
cp -r "${REPO_ROOT}/src/skk_integration"  "${WORK_DIR}/src/skk_integration"

# Mozc protos (commands.proto imports a handful of siblings; copy them all).
mkdir -p "${WORK_DIR}/src/proto/protocol" "${WORK_DIR}/src/proto/ipc"
cp "${UPSTREAM_DIR}"/mozc/src/protocol/*.proto "${WORK_DIR}/src/proto/protocol/"
cp "${UPSTREAM_DIR}/mozc/src/ipc/ipc.proto"    "${WORK_DIR}/src/proto/ipc/ipc.proto"

# --- step 3: apply patches --------------------------------------------------
shopt -s nullglob
for p in "${REPO_ROOT}/patches/fcitx5-skk/"*.patch; do
    log "applying $(basename "$p")"
    git -C "${WORK_DIR}" apply --whitespace=nowarn "$p"
done
shopt -u nullglob

# --- step 4: configure + build ---------------------------------------------
log "configuring (cmake)"
cmake -S "${WORK_DIR}" -B "${WORK_DIR}/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DENABLE_QT=Off

log "building (ninja, j=${JOBS})"
cmake --build "${WORK_DIR}/build" --parallel "${JOBS}"

log "build artifact:"
find "${WORK_DIR}/build" -name "skk.so" -printf '  %p (%s bytes)\n'

# --- step 5: install (optional) --------------------------------------------
if [[ "${DO_INSTALL}" == "1" ]]; then
    log "installing to ${PREFIX} (sudo may prompt)"
    if [[ -w "${PREFIX}" ]]; then
        cmake --install "${WORK_DIR}/build"
    else
        sudo cmake --install "${WORK_DIR}/build"
    fi
fi

log "done"
cat <<EOF

Next steps:
  - Make sure mozc_server is installed (Debian: 'sudo apt install mozc-server').
  - Re-run with INSTALL=1 to copy skk.so + metadata under \$PREFIX:
        INSTALL=1 PREFIX=\$HOME/.local $(basename "$0")
  - Restart fcitx5: 'fcitx5 -r' or 'pkill -SIGHUP fcitx5'.
  - Configure runtime options via environment (read by the addon at startup):
        SKK_MOZC_ENABLE=1
        SKK_MOZC_IPC_TIMEOUT_MS=50
        SKK_MOZC_MAX_CANDIDATES=20
        SKK_MOZC_DEBUG=0   # 1 to log to ~/.cache/skk-mozc/log
EOF
