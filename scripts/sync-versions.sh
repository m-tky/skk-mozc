#!/usr/bin/env bash
#
# Regenerate scripts/versions.env from flake.lock.
#
# flake.lock is the canonical source of truth for upstream pins. The non-Nix
# build path (scripts/build.sh) reads versions.env to know which fcitx5-skk
# and mozc revs to clone. Keep them in sync by running this after every
# `nix flake update` (the flake check `versions-env-in-sync` fails CI if the
# two drift apart).
#
# Usage:
#   ./scripts/sync-versions.sh         # rewrite versions.env from flake.lock
#   ./scripts/sync-versions.sh --check # exit 1 if regeneration would change it

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
LOCK="${REPO_ROOT}/flake.lock"
OUT="${SCRIPT_DIR}/versions.env"

MODE="write"
if [ "${1:-}" = "--check" ]; then
    MODE="check"
fi

command -v jq >/dev/null 2>&1 || {
    echo "sync-versions: 'jq' is required (run inside 'nix develop' or install it)" >&2
    exit 2
}

# Pull rev + owner/repo for each upstream node. Falls back to "url" form
# for inputs that aren't on GitHub.
extract() {
    local node="$1"
    local rev owner repo url
    rev=$(jq -r ".nodes[\"${node}\"].locked.rev // \"\"" "${LOCK}")
    owner=$(jq -r ".nodes[\"${node}\"].locked.owner // \"\"" "${LOCK}")
    repo=$(jq -r ".nodes[\"${node}\"].locked.repo // \"\"" "${LOCK}")
    url=$(jq -r ".nodes[\"${node}\"].locked.url // \"\"" "${LOCK}")
    if [ -z "${rev}" ]; then
        echo "sync-versions: missing locked rev for node '${node}'" >&2
        exit 2
    fi
    if [ -n "${owner}" ] && [ -n "${repo}" ]; then
        url="https://github.com/${owner}/${repo}.git"
    fi
    echo "${rev}|${url}"
}

FCITX5_SKK="$(extract fcitx5-skk-src)"
MOZC="$(extract mozc-src)"

NEW=$(cat <<EOF
# Upstream commits this build is known to apply against.
# AUTO-GENERATED from flake.lock by scripts/sync-versions.sh — do not edit
# by hand. After 'nix flake update', re-run scripts/sync-versions.sh to
# keep this file in sync; the 'versions-env-in-sync' flake check fails
# if it drifts.

FCITX5_SKK_REV=${FCITX5_SKK%|*}
MOZC_REV=${MOZC%|*}

FCITX5_SKK_REPO=${FCITX5_SKK#*|}
MOZC_REPO=${MOZC#*|}
EOF
)

if [ "${MODE}" = "check" ]; then
    if ! diff -u "${OUT}" <(printf '%s\n' "${NEW}") >/dev/null; then
        echo "sync-versions: scripts/versions.env is out of date relative to flake.lock" >&2
        diff -u "${OUT}" <(printf '%s\n' "${NEW}") >&2 || true
        echo "" >&2
        echo "Run: ./scripts/sync-versions.sh" >&2
        exit 1
    fi
    exit 0
fi

printf '%s\n' "${NEW}" > "${OUT}"
echo "sync-versions: wrote ${OUT}"
