#!/usr/bin/env bash
#
# Run the real-machine smoke tests:
#   1. mozc-client-cli scenario checks against a real mozc_server.
#   2. fcitx5 TestFrontend e2e checks against the built skk.so addon.
#
# The test runs with an isolated HOME/XDG tree by default so it does not touch
# the user's normal Mozc or SKK state. Override SKK_MOZC_TEST_HOME to inspect
# or reuse that state.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

log() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m!!\033[0m %s\n' "$*" >&2; exit 1; }

find_mozc_server() {
    if [[ -n "${SKK_MOZC_MOZC_SERVER:-}" ]]; then
        [[ -x "${SKK_MOZC_MOZC_SERVER}" ]] ||
            die "SKK_MOZC_MOZC_SERVER is not executable: ${SKK_MOZC_MOZC_SERVER}"
        printf '%s\n' "${SKK_MOZC_MOZC_SERVER}"
        return
    fi

    local p
    for p in \
        /run/current-system/sw/lib/mozc/mozc_server \
        /run/current-system/sw/bin/mozc_server \
        /usr/lib/mozc/mozc_server \
        /usr/libexec/mozc/mozc_server; do
        if [[ -x "$p" ]]; then
            printf '%s\n' "$p"
            return
        fi
    done

    if command -v mozc_server >/dev/null 2>&1; then
        command -v mozc_server
        return
    fi

    if command -v nix >/dev/null 2>&1; then
        log "mozc_server not found in standard locations; building nixpkgs#mozc" >&2
        local out
        out="$(nix build --no-link --print-out-paths nixpkgs#mozc)"
        p="${out}/lib/mozc/mozc_server"
        [[ -x "$p" ]] || die "nixpkgs#mozc did not provide ${p}"
        printf '%s\n' "$p"
        return
    fi

    die "could not find mozc_server; set SKK_MOZC_MOZC_SERVER=/path/to/mozc_server"
}

build_outputs() {
    log "building e2e artifacts"
    local output_text
    output_text="$(nix build --no-link --print-out-paths \
        .#mozc-client-cli \
        .#skk-mozc-e2e-test)"
    mapfile -t outputs <<<"${output_text}"
    [[ "${#outputs[@]}" -eq 2 ]] ||
        die "expected two nix output paths, got ${#outputs[@]}"
    CLI_BIN="${outputs[0]}/bin/mozc-client-cli"
    E2E_BIN="${outputs[1]}/bin/skk-mozc-e2e-test"
    [[ -x "${CLI_BIN}" ]] || die "missing CLI binary: ${CLI_BIN}"
    [[ -x "${E2E_BIN}" ]] || die "missing e2e binary: ${E2E_BIN}"
}

TMP_ROOT=""
cleanup() {
    if [[ -n "${TMP_ROOT}" && -d "${TMP_ROOT}" && -z "${SKK_MOZC_TEST_HOME:-}" ]]; then
        rm -rf "${TMP_ROOT}"
    fi
}
trap cleanup EXIT

main() {
    cd "${REPO_ROOT}"
    command -v nix >/dev/null 2>&1 || die "nix is required for this test"

    build_outputs

    local mozc_server
    mozc_server="$(find_mozc_server)"
    log "using mozc_server: ${mozc_server}"

    if [[ -n "${SKK_MOZC_TEST_HOME:-}" ]]; then
        TMP_ROOT="${SKK_MOZC_TEST_HOME}"
        mkdir -p "${TMP_ROOT}"
    else
        TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/skk-mozc-e2e.XXXXXX")"
    fi

    export HOME="${TMP_ROOT}/home"
    export XDG_CONFIG_HOME="${HOME}/.config"
    export XDG_CACHE_HOME="${HOME}/.cache"
    export XDG_DATA_HOME="${HOME}/.local/share"
    export XDG_RUNTIME_DIR="${TMP_ROOT}/runtime"
    export FCITX_CONFIG_DIR="${XDG_CONFIG_HOME}/fcitx5"
    export SKK_MOZC_E2E_CONFIG_DIR="${FCITX_CONFIG_DIR}"
    export SKK_MOZC_MOZC_SERVER="${mozc_server}"
    export SKK_MOZC_ENABLE="${SKK_MOZC_ENABLE:-1}"
    export SKK_MOZC_IPC_TIMEOUT_MS="${SKK_MOZC_IPC_TIMEOUT_MS:-1000}"
    export SKK_MOZC_MAX_CANDIDATES="${SKK_MOZC_MAX_CANDIDATES:-20}"
    export SKK_MOZC_DEBUG="${SKK_MOZC_DEBUG:-0}"

    mkdir -p "${XDG_CONFIG_HOME}" "${XDG_CACHE_HOME}" \
             "${XDG_DATA_HOME}" "${XDG_RUNTIME_DIR}"
    chmod 700 "${XDG_RUNTIME_DIR}"
    mkdir -p "${FCITX_CONFIG_DIR}/skk"
    cp "${REPO_ROOT}/skk/user.dict" "${FCITX_CONFIG_DIR}/skk/user.dict"
    mkdir -p "${TMP_ROOT}/skk"
    cp "${REPO_ROOT}/skk/user.dict" "${TMP_ROOT}/skk/user.dict"

    log "test HOME: ${HOME}"
    log "running Mozc IPC scenarios"
    "${REPO_ROOT}/tests/scenarios.sh" "${CLI_BIN}"

    log "running fcitx5 TestFrontend e2e"
    if [[ "${SKK_MOZC_E2E_VERBOSE:-0}" == "1" ]]; then
        (cd "${TMP_ROOT}" && "${E2E_BIN}")
    else
        local e2e_log="${TMP_ROOT}/fcitx5-e2e.log"
        if (cd "${TMP_ROOT}" && "${E2E_BIN}") >"${e2e_log}" 2>&1; then
            grep -E 'e2e test: (PASS|FAIL)' "${e2e_log}" || true
        else
            cat "${e2e_log}" >&2
            return 1
        fi
    fi

    log "all real-machine e2e checks passed"
}

main "$@"
