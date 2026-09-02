#!/usr/bin/env bash
# Written by Richard Christopher, Copyright 2026 NeoTec Digital
#
# Staged-boot milestone, end to end: drive the real `vazio` binary headless in
# all three staging cases and assert on what it printed AND on how it died.
#
# In-repo port of the Phase-3a run_milestone.sh that used to live in a scratch
# directory and was lost with it. Same two gates, unchanged, because both were
# load-bearing:
#   1. an abort-signature grep over the log, widened to every libc/glibc
#      teardown diagnostic that actually shows up (double free, corruption,
#      free(), malloc(), Aborted, stack smashing, glibc detected). The narrower
#      predecessor reported "ok" while the process was aborting;
#   2. an explicit exit-code assertion that NAMES 134 (128+SIGABRT), 139
#      (128+SIGSEGV) and 133 (128+SIGTRAP) rather than only testing != 0, so a
#      signal death can never be mistaken for a clean non-zero exit.
#
# usage: staged_boot_e2e.sh <vazio-binary> [run-dir]
#
# Negative controls (VAZIO_TEST_NEG), which MUST make this script fail:
#   abortlog - append a fake `double free or corruption` line to the splash log
#              before it is graded. Proves gate 1 is live.
#   noready  - write the readiness token to a path vazio was not given. Proves
#              the staged case is actually asserting on the flip and not just
#              on the run completing.

set -uo pipefail

VAZIO_BIN="${1:-}"
if [[ -z "${VAZIO_BIN}" || ! -x "${VAZIO_BIN}" ]]; then
    echo "usage: $0 <vazio-binary> [run-dir]   (argument 1 must be executable)" >&2
    exit 2
fi
VAZIO_BIN="$(cd "$(dirname "${VAZIO_BIN}")" && pwd)/$(basename "${VAZIO_BIN}")"

# A unix socket path is capped at 108 bytes, and vazio's session root and the
# runtime dir both live under here. Short by construction, never the caller's
# live session, and removed on the way out.
RUN_DIR="${2:-}"
if [[ -z "${RUN_DIR}" ]]; then
    RUN_DIR="$(mktemp -d "/tmp/vz-e2e-XXXXXX")"
else
    mkdir -p "${RUN_DIR}"
    RUN_DIR="$(cd "${RUN_DIR}" && pwd)"
fi
trap 'rm -rf "${RUN_DIR}"' EXIT

NEG="${VAZIO_TEST_NEG:-none}"
printf 'staged_boot_e2e: binary=%s run_dir=%s neg=%s\n' "${VAZIO_BIN}" "${RUN_DIR}" "${NEG}" >&2

CHECKS=0
FAILURES=0

check() {
    local ok="$1"; shift
    CHECKS=$((CHECKS + 1))
    if [[ "${ok}" == "0" ]]; then
        printf '[ ok ] %s\n' "$*"
    else
        FAILURES=$((FAILURES + 1))
        printf '[FAIL] %s\n' "$*"
    fi
}

check_grep() {
    local pattern="$1" log="$2"; shift 2
    grep -Eq -- "${pattern}" "${log}"
    check $? "$*"
}

check_no_grep() {
    local pattern="$1" log="$2"; shift 2
    if grep -Eq -- "${pattern}" "${log}"; then
        check 1 "$*"
        grep -En -- "${pattern}" "${log}" | head -5 | sed 's/^/       > /'
    else
        check 0 "$*"
    fi
}

ABORT_SIGNATURES='double free|corruption|free\(\): |malloc\(\): |munmap_chunk|stack smashing|glibc detected|Aborted|Segmentation fault|core dumped|AddressSanitizer|LeakSanitizer|terminate called|Assertion .* failed|VMA_ASSERT'

assert_exit() {
    local rc="$1" expected="$2" label="$3"
    case "${rc}" in
        134) check 1 "${label}: exit ${rc} = SIGABRT (process aborted at teardown)" ;;
        139) check 1 "${label}: exit ${rc} = SIGSEGV" ;;
        133) check 1 "${label}: exit ${rc} = SIGTRAP" ;;
        *)   check 0 "${label}: exit ${rc} is not a signal death (134/139/133)" ;;
    esac
    if [[ "${rc}" == "${expected}" ]]; then
        check 0 "${label}: exit ${rc} == expected ${expected}"
    else
        check 1 "${label}: exit ${rc} != expected ${expected}"
    fi
}

# Only the exit code reaches stdout: this runs inside $( ), so anything else
# printed here would be captured as part of it.
run_case() {
    local label="$1" log="$2"; shift 2
    printf '\n--- %s ---\n' "${label}" >&2
    (
        cd "${RUN_DIR}" || exit 97
        # WLR_RENDERER is deliberately NOT forced: an empty value is not the
        # same as unset, and the caller sets it to pick the pixman path.
        WLR_BACKENDS=headless \
        LIBGL_ALWAYS_SOFTWARE=1 \
        XDG_RUNTIME_DIR="${RUN_DIR}" \
        timeout --signal=KILL 180 "${VAZIO_BIN}" "$@"
    ) > "${log}" 2>&1
    printf '%s' "$?"
}

# ---------------------------------------------------------------------------
# Case 1 - splash only: no readiness channel, bounded by --max-frames.
# ---------------------------------------------------------------------------
SPLASH_LOG="${RUN_DIR}/splash.log"
SPLASH_RC="$(run_case 'splash (no readiness channel)' "${SPLASH_LOG}" \
             --headless --max-frames 120 --session-root "${RUN_DIR}")"

if [[ "${NEG}" == "abortlog" ]]; then
    printf 'double free or corruption (!prev)\n' >> "${SPLASH_LOG}"
fi

check_grep 'NovaGraphics - Offscreen mode initialized' "${SPLASH_LOG}" \
    'splash: Nova came up offscreen (no surface, no swapchain)'
check_grep 'Bringing the substrate up latent' "${SPLASH_LOG}" \
    'splash: substrate came up latent'
check_grep 'Substrate latent: [0-9]+ output\(s\), 0 sockets, 0 globals' "${SPLASH_LOG}" \
    'splash: latent stage reported 0 sockets and 0 globals'
check_grep 'SpatialPresentLoop - Presentation path: (dmabuf-import|pixman-sidecar)' "${SPLASH_LOG}" \
    'splash: a presentation path was selected'
check_grep 'SpatialPresentLoop - Driving output' "${SPLASH_LOG}" \
    'splash: the present loop adopted an output'
check_grep 'vazio - Splash stage on [0-9]+ output' "${SPLASH_LOG}" \
    'splash: splash stage reported its outputs'
check_grep 'No readiness channel; the splash is the whole of this run' "${SPLASH_LOG}" \
    'splash: absent readiness channel is plan B.2 option (i), stated'
check_no_grep 'CLOUDS DISPLAY SERVER ACTIVE' "${SPLASH_LOG}" \
    'splash: no socket was ever opened'
check_no_grep 'WAYLAND_DISPLAY=' "${SPLASH_LOG}" \
    'splash: no WAYLAND_DISPLAY was advertised'
check_grep 'vazio - Shutting down: [0-9]+ frames committed' "${SPLASH_LOG}" \
    'splash: shutdown summary printed'
check_grep 'vazio - Shutting down: [0-9]+ frames committed, 0 failed' "${SPLASH_LOG}" \
    'splash: zero failed frames'
check_grep 'vazio - Shutting down:.*stage latent' "${SPLASH_LOG}" \
    'splash: the session never left the latent stage'
check_grep 'NovaGraphics - Destroying' "${SPLASH_LOG}" \
    'splash: Nova teardown was reached'
check_no_grep "${ABORT_SIGNATURES}" "${SPLASH_LOG}" \
    'splash: no abort signature in the log'
assert_exit "${SPLASH_RC}" 0 'splash'

SPLASH_FRAMES="$(sed -n 's/.*Shutting down: \([0-9]\+\) frames committed.*/\1/p' "${SPLASH_LOG}" | tail -1)"
if [[ -n "${SPLASH_FRAMES}" && "${SPLASH_FRAMES}" -ge 120 ]]; then
    check 0 "splash: committed ${SPLASH_FRAMES} frames (>= the 120 requested)"
else
    check 1 "splash: committed '${SPLASH_FRAMES:-none}' frames, expected >= 120"
fi

# ---------------------------------------------------------------------------
# Case 2 - staged: a readiness token flips latent -> open mid-run.
# ---------------------------------------------------------------------------
READY_PATH="${RUN_DIR}/ready.token"
WRITE_PATH="${READY_PATH}"
if [[ "${NEG}" == "noready" ]]; then
    WRITE_PATH="${RUN_DIR}/wrong.token"
fi
rm -f "${READY_PATH}" "${WRITE_PATH}"
( sleep 3; printf 'ready\n' > "${WRITE_PATH}" ) &
TOKEN_WRITER=$!

STAGED_LOG="${RUN_DIR}/staged.log"
STAGED_RC="$(run_case 'staged (readiness token arrives mid-run)' "${STAGED_LOG}" \
             --headless --max-frames 400 --ready-path "${READY_PATH}" \
             --session-root "${RUN_DIR}")"
wait "${TOKEN_WRITER}" 2>/dev/null

check_grep 'Bringing the substrate up latent' "${STAGED_LOG}" \
    'staged: substrate came up latent first'
check_grep 'Readiness token received after [0-9.]+s; opening the session' "${STAGED_LOG}" \
    'staged: the readiness token was observed'
check_grep 'Opening the session on socket .wayland-vazio-0' "${STAGED_LOG}" \
    'staged: open() ran on the same substrate'
check_grep 'CLOUDS DISPLAY SERVER ACTIVE' "${STAGED_LOG}" \
    'staged: the session announced itself'
check_grep 'WAYLAND_DISPLAY=wayland-vazio-0' "${STAGED_LOG}" \
    'staged: the socket name is the one open() was given'
check_grep 'vazio - Session Desktop built from' "${STAGED_LOG}" \
    'staged: the session Desktop replaced the boot Desktop'
check_grep 'vazio - Shutting down:.*stage open' "${STAGED_LOG}" \
    'staged: the run ended in the open stage'
check_grep 'vazio - Shutting down: [0-9]+ frames committed, 0 failed' "${STAGED_LOG}" \
    'staged: zero failed frames across the flip'
check_no_grep 'Session refused to open on a live substrate' "${STAGED_LOG}" \
    'staged: the flip was not refused'
check_no_grep "${ABORT_SIGNATURES}" "${STAGED_LOG}" \
    'staged: no abort signature in the log'
assert_exit "${STAGED_RC}" 0 'staged'

# ---------------------------------------------------------------------------
# Case 3 - negative control: an unreadable readiness path must NOT open.
# ---------------------------------------------------------------------------
NEG_LOG="${RUN_DIR}/negctl.log"
NEG_RC="$(run_case 'negctl (readiness path that never appears)' "${NEG_LOG}" \
          --headless --max-frames 100 --ready-path "${RUN_DIR}/never-written.token" \
          --session-root "${RUN_DIR}")"

check_no_grep 'Readiness token received' "${NEG_LOG}" \
    'negctl: no token was reported for a path that was never written'
check_no_grep 'CLOUDS DISPLAY SERVER ACTIVE' "${NEG_LOG}" \
    'negctl: the session stayed closed'
check_grep 'vazio - Shutting down:.*stage latent' "${NEG_LOG}" \
    'negctl: the run ended latent'
check_no_grep "${ABORT_SIGNATURES}" "${NEG_LOG}" \
    'negctl: no abort signature in the log'
assert_exit "${NEG_RC}" 0 'negctl'

# ---------------------------------------------------------------------------
printf '\n=== staged_boot_e2e: %d/%d checks passed, %d failures ===\n' \
    $((CHECKS - FAILURES)) "${CHECKS}" "${FAILURES}"
printf 'VAZIO_EXIT splash=%s staged=%s negctl=%s\n' "${SPLASH_RC}" "${STAGED_RC}" "${NEG_RC}"

[[ "${FAILURES}" -eq 0 ]]
