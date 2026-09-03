#!/usr/bin/env bash
# Written by Richard Christopher, Copyright 2026 NeoTec Digital
#
# LAYER PURITY: the layer chain as a standing test, not a one-time measurement.
#
# A4 split the tree into four archives and made a deliberate upward include fail
# the build. That enforcement is real but PER-TARGET: static-archive link order
# only catches a violation in a target that happens to scan the archives in the
# wrong order. `vazio` and `test_scene_input` built straight THROUGH the seeded
# violation, because their own objects had already extracted the Clouds members
# before libSplash.a was scanned. A link error that depends on scan order is not
# an invariant; it is a coincidence that has been holding.
#
# The invariant itself is a property of the object code and is checkable
# directly: no archive may reference or define a symbol belonging to a layer
# above it.
#
#   Nova    <- Splash <- { Vazio, Clouds }
#
# Vazio and Clouds are SIBLINGS, so the rule is not a stack: Vazio may not name
# Clouds and Clouds may not name Vazio, on top of neither being allowed to name
# anything above Splash. Run against the archives the build just produced, so it
# cannot drift from what was actually compiled.
#
# usage: layer_purity.sh <label> <layer> <archive> [<label> <layer> <archive>]...
#        layer is one of Nova/Splash/Vazio/Clouds, or `none` for an archive that
#        may reference no first-party layer at all (generated protocol glue).
#
# Negative controls (VAZIO_TEST_NEG), which MUST make this script fail:
#   demangler  point the demangler at /bin/cat            - proves V1 is live
#   nodetect   blind both classifiers' layer rules        - proves V2 is live
#   nomangle   disable the mangled-form classifier        - proves V4 is live
#   noedge     expect a Splash -> Clouds edge (0 by law)  - proves V5 is live

set -uo pipefail

NEG="${VAZIO_TEST_NEG:-}"

fail() { printf 'layer_purity: FAIL: %s\n' "$*" >&2; exit 1; }
note() { printf 'layer_purity: %s\n' "$*"; }

# ---------------------------------------------------------------------------
# The layer model. `allowed` is what an archive at that layer MAY name - itself
# and everything strictly beneath it. Everything else is upward. Sub-namespaces
# (Nova::Math, Nova::RAII, Nova::SDL, Clouds::UI) live under their layer's root,
# so the four roots below cover the whole tree.
# ---------------------------------------------------------------------------
readonly LAYER_ROOTS="Nova Splash Vazio Clouds"

allowed_for() {
    case "$1" in
        Nova)   printf 'Nova' ;;
        Splash) printf 'Nova Splash' ;;
        Vazio)  printf 'Nova Splash Vazio' ;;
        Clouds) printf 'Nova Splash Clouds' ;;
        none)   printf '' ;;
        *)      fail "unknown layer '$1'" ;;
    esac
}

# ===========================================================================
# HOW THIS TEST IS KEPT FROM PASSING VACUOUSLY
#
# V1 demangler self-test. Seven control manglings, one per layer root plus a
#    real C++20 requires-clause name carrying `Vazio::`, must demangle to their
#    expected text. GCC's c++filt cannot read requires-clause manglings - it
#    leaves 323 of this tree's symbols mangled where llvm-cxxfilt leaves 80 -
#    so `nm -C` is not an acceptable substitute, and substituting it fails here
#    instead of silently narrowing what gets looked at.
# V2 detector self-test. A synthetic Splash -> Clouds reference goes through the
#    SAME classifier the archives get and MUST be reported. If the detector is
#    dead the run stops before it ever opens an archive.
# V3 every named archive must exist, be non-empty, yield symbols, and attribute
#    at least one of them to some layer. A missing archive is a failure, never a
#    skip.
# V4 dual-classifier agreement. Every symbol is classified TWICE by independent
#    means - by qualified name in the demangled text, and by the length-prefixed
#    namespace token in the raw mangled text - and the two must agree exactly on
#    every symbol that demangled. 17,251 symbols agree today. A demangler that
#    quietly stops attributing makes the demangled half go empty while the
#    mangled half still finds tokens, and the run fails on the disagreement.
# V5 expected downward edges must be NON-ZERO. Splash is built on Nova; Vazio
#    and Clouds are built on Splash. If attribution ever degrades to nothing,
#    every count collapses to zero - and a zero where the chain requires a
#    positive number fails. This is what makes "0 violations" mean "looked and
#    found none" rather than "did not look".
#
# The mangled-form classifier is not a fallback of convenience, it is what makes
# attribution COMPLETE: Itanium substitutions (S_, S2_) can only back-reference
# a component that already appeared literally in the same name, so a symbol that
# names a namespace names it literally at least once. Scanning the mangled text
# therefore cannot miss a layer, whatever the demangler does.
# ===========================================================================

# --- V1 ---------------------------------------------------------------------
CXXFILT="${LLVM_CXXFILT:-}"
[[ -n "${CXXFILT}" ]] || CXXFILT="$(command -v llvm-cxxfilt 2>/dev/null || true)"
[[ "${NEG}" == "demangler" ]] && CXXFILT=/bin/cat

if [[ -z "${CXXFILT}" || ! -x "${CXXFILT}" ]]; then
    fail "llvm-cxxfilt not found.
  This test demangles every archive symbol to attribute it to a layer. GCC's
  c++filt cannot read C++20 requires-clause manglings and would leave symbols
  unattributed, so it is not an acceptable substitute and this test will not
  silently fall back to it.
  Install LLVM's demangler (Arch: llvm, Debian: llvm), or point LLVM_CXXFILT at
  it and re-run CMake."
fi

demangler_self_test() {
    local expected actual mangled
    while IFS='|' read -r mangled expected; do
        [[ -n "${mangled}" ]] || continue
        actual="$(printf '%s\n' "${mangled}" | "${CXXFILT}")"
        if [[ "${actual}" != *"${expected}"* ]]; then
            fail "demangler self-test failed for ${mangled}
  using:    ${CXXFILT}
  expected: substring '${expected}'
  actual:   '${actual}'
  A demangler that cannot read this mangling leaves real symbols unattributed
  and would make this whole test pass by not looking."
        fi
    done <<'CONTROLS'
_ZN4Nova1fEv|Nova::f()
_ZN6Splash1fEv|Splash::f()
_ZN5Vazio1fEv|Vazio::f()
_ZN6Clouds1fEv|Clouds::f()
_ZN4Nova4Math1fEv|Nova::Math::f()
_ZN6Clouds2UI1fEv|Clouds::UI::f()
_ZNKSt10unique_ptrIN5Vazio18SpatialPresentLoop6OutputESt14default_deleteIS2_EEdeEvQrqXdecl7declvalINSt15__uniq_ptr_implIT_T0_E7pointerEEEE|Vazio::SpatialPresentLoop::Output
CONTROLS
    note "V1 demangler ok: ${CXXFILT} - 7/7 controls, requires-clause included"
}

# ---------------------------------------------------------------------------
# The classifier. Reads TSV `raw<TAB>demangled` on stdin, emits a report of
#   EDGE <layer> <n> | UP <layer> <symbol> | DISAGREE <dem> <mang> <symbol>
#   TOTAL/CLASSIFIED/DISAGREE_N/UNDEMANGLED <n>
#
# Demangled matching deliberately accepts a layer root ANYWHERE at a qualified
# name position, not only leading: `std::unique_ptr<Clouds::X>::~unique_ptr()`
# referenced from Splash is a violation and a leading-namespace rule would miss
# it. The `[^A-Za-z0-9_:]` guard keeps a hypothetical nested Nova::Clouds:: from
# being read as the Clouds layer, and the length prefix does the same job on the
# mangled side (`13CloudsInterim`, a real namespace in Vazio, does not contain
# the token `6Clouds`).
# ---------------------------------------------------------------------------
CLASSIFY_AWK='
BEGIN {
    n = split(roots, R, " ");
    split(allowed, A, " ");
    for (i in A) ok[A[i]] = 1;
    mang["Nova"] = "4Nova"; mang["Splash"] = "6Splash";
    mang["Vazio"] = "5Vazio"; mang["Clouds"] = "6Clouds";
}
{
    raw = $1; dem = $2; total++;
    demangled = (dem != raw);
    ds = ""; ms = ""; hit = 0;
    for (i = 1; i <= n; i++) {
        L = R[i];
        d = (demangled && dem ~ ("(^|[^A-Za-z0-9_:])" L "::"));
        m = (index(raw, mang[L]) > 0);
        if (nomangle) m = 0;
        if (blind) { d = 0; m = 0; }
        if (d) ds = ds L ",";
        if (m) ms = ms L ",";
        if (!d && !m) continue;
        hit = 1; count[L]++;
        if (!(L in ok)) print "UP\t" L "\t" (demangled ? dem : raw);
    }
    if (demangled && ds != ms) {
        disagree++;
        print "DISAGREE\t[" ds "]\t[" ms "]\t" dem;
    }
    if (hit) classified++;
    if (!demangled && raw ~ /^_Z/) undemangled++;
}
END {
    for (i = 1; i <= n; i++) printf "EDGE\t%s\t%d\n", R[i], count[R[i]] + 0;
    printf "TOTAL\t%d\nCLASSIFIED\t%d\nDISAGREE_N\t%d\nUNDEMANGLED\t%d\n",
        total + 0, classified + 0, disagree + 0, undemangled + 0;
}'

classify() {  # <self-layer> ; TSV on stdin, report on stdout
    local blind=0 nomangle=0
    [[ "${NEG}" == "nodetect" ]] && blind=1
    [[ "${NEG}" == "nomangle" ]] && nomangle=1
    awk -F'\t' -v roots="${LAYER_ROOTS}" -v allowed="$(allowed_for "$1")" \
        -v blind="${blind}" -v nomangle="${nomangle}" "${CLASSIFY_AWK}"
}

# --- V2 ---------------------------------------------------------------------
detector_self_test() {
    local probe caught
    probe='_ZN6Clouds2UI6Button4drawEv'
    caught="$(printf '%s\t%s\n' "${probe}" "$(printf '%s\n' "${probe}" | "${CXXFILT}")" \
              | classify Splash | grep -c '^UP' || true)"
    if (( caught < 1 )); then
        fail "detector self-test failed: a synthetic Splash -> Clouds::UI::Button::draw()
  reference was NOT reported as upward. The classifier cannot detect the thing
  this test exists to detect, so a clean result would be meaningless."
    fi
    note "V2 detector ok: synthetic Splash -> Clouds reference is caught"
}

# ---------------------------------------------------------------------------
# Symbol extraction. Undefined symbols carry the "what does this archive REACH
# FOR" half of the invariant; defined symbols carry the "what does it OWN" half,
# which is the only way to see an upward reach that was inlined or instantiated
# out of a higher layer's header and so never left an undefined reference.
# ---------------------------------------------------------------------------
symbols() {  # <archive> <undefined|defined>
    if [[ "$2" == "undefined" ]]; then
        nm --undefined-only --no-demangle "$1" 2>/dev/null \
            | awk '($1 == "U" || $1 == "w") && NF == 2 { print $2 }' | sort -u
    else
        nm --defined-only --no-demangle "$1" 2>/dev/null \
            | awk 'NF == 3 { print $3 }' | sort -u
    fi
}

# Writes the report to $4 rather than to stdout, and is called DIRECTLY rather
# than through $( ). That is load-bearing: `fail` runs `exit 1`, and inside a
# command substitution that only kills the subshell - the gate would print its
# message and the run would carry on with an empty report.
analyse() {  # <archive> <layer> <mode> <report-out>
    local raw
    raw="${WORK}/$(basename "$1").$3.raw"
    symbols "$1" "$3" > "${raw}"
    [[ -s "${raw}" ]] || fail "$1: nm produced no $3 symbols. Either the archive is
  empty or nm is not reading it; either way nothing was checked."
    paste -d'\t' "${raw}" <("${CXXFILT}" < "${raw}") | classify "$2" > "$4"
}

# --- V5 ---------------------------------------------------------------------
# Structural edges of the chain, asserted non-zero on the undefined-symbol pass.
expected_edges() {
    # The negative control ASSERTS an edge the invariant forbids. V5 must then
    # measure it at zero and fail, which is the only way to show V5 detects a
    # zero rather than merely tolerating one.
    [[ "${NEG}" == "noedge" ]] && printf 'Splash Clouds\n'
    cat <<'EDGES'
Nova Nova
NovaSDL Nova
Splash Nova
Splash Splash
Vazio Nova
Vazio Splash
Vazio Vazio
Clouds Nova
Clouds Splash
Clouds Clouds
EDGES
}

# Defaults to 0. An empty result would make `(( $(field ...) == 0 ))` a bash
# syntax error, which `if` reads as false - a missing count would silently pass
# the gate that exists to catch missing counts.
field() {
    printf '%s\n' "$2" | awk -F'\t' -v k="$1" '$1 == k { print $2 + 0; found = 1; exit }
                                                 END { if (!found) print 0 }'
}

report_archive() {  # <label> <layer> <mode> <report>
    local label="$1" layer="$2" mode="$3" report="$4" L n
    local row="" edges=""
    for L in ${LAYER_ROOTS}; do
        n="$(printf '%s\n' "${report}" | awk -F'\t' -v l="${L}" '$1=="EDGE" && $2==l {print $3}')"
        row="${row}$(printf '%10s' "${n}")"
        edges="${edges}${label} ${L} ${n}"$'\n'
    done
    printf '%-10s %-8s%s %10s  %s\n' "${label}" "${layer}" "${row}" \
        "$(field TOTAL "${report}")" "${mode}"
    [[ "${mode}" == "undefined" ]] && printf '%s' "${edges}" >> "${WORK}/edges"
    return 0
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
(( $# >= 3 && $# % 3 == 0 )) || fail "usage: $0 <label> <layer> <archive> ..."

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
: > "${WORK}/edges"

demangler_self_test
detector_self_test

violations=0
disagreements=0
ANALYSED=""
printf '\n%-10s %-8s%10s%10s%10s%10s %10s  %s\n' \
    ARCHIVE LAYER Nova Splash Vazio Clouds SYMBOLS MODE

while (( $# )); do
    label="$1"; layer="$2"; archive="$3"; shift 3

    [[ -f "${archive}" ]] || fail "${label}: archive not found: ${archive}
  The test is registered against a target that did not build, so nothing was
  inspected. That is a failure, not a skip."
    [[ -s "${archive}" ]] || fail "${label}: archive is empty: ${archive}"
    # Direct call, not $( ): an unknown layer must abort the run, and `fail`
    # inside a command substitution only exits the subshell.
    allowed_for "${layer}" > /dev/null
    [[ "${layer}" == "none" ]] || ANALYSED="${ANALYSED}${label} "

    for mode in undefined defined; do
        analyse "${archive}" "${layer}" "${mode}" "${WORK}/report"
        report="$(cat "${WORK}/report")"
        report_archive "${label}" "${layer}" "${mode}" "${report}"

        # V3(b): an archive whose symbols all landed outside every layer means
        # attribution stopped working, not that the archive is pure.
        if [[ "${layer}" != "none" ]] && (( $(field CLASSIFIED "${report}") == 0 )); then
            fail "${label} (${mode}): zero of $(field TOTAL "${report}") symbols were
  attributed to any layer. Attribution is broken; a clean result here would be a
  result nobody produced."
        fi

        # V4: the two independent classifiers must agree.
        n_dis="$(field DISAGREE_N "${report}")"
        if (( n_dis > 0 )); then
            disagreements=$(( disagreements + n_dis ))
            printf '\nlayer_purity: classifier DISAGREEMENT in %s (%s):\n' "${label}" "${mode}" >&2
            printf '%s\n' "${report}" | awk -F'\t' '$1=="DISAGREE" {print "  demangled" $2 " mangled" $3 " " $4}' >&2
        fi

        # The invariant itself.
        n_up="$(printf '%s\n' "${report}" | grep -c '^UP' || true)"
        if (( n_up > 0 )); then
            violations=$(( violations + n_up ))
            printf '\nlayer_purity: UPWARD REFERENCES in %s (%s layer, %s symbols):\n' \
                "${label}" "${layer}" "${mode}" >&2
            printf '%s\n' "${report}" | awk -F'\t' '$1=="UP" {print "  -> " $2 ": " $3}' >&2
        fi
    done
done
printf '\n'

# --- V5 verdict -------------------------------------------------------------
missing=0
while read -r elabel elayer; do
    [[ -n "${elabel}" ]] || continue
    got="$(awk -v a="${elabel}" -v l="${elayer}" '$1==a && $2==l {print $3}' "${WORK}/edges")"
    if [[ -z "${got}" ]] || (( got == 0 )); then
        printf 'layer_purity: expected edge %s -> %s is ZERO or unmeasured\n' \
            "${elabel}" "${elayer}" >&2
        missing=$(( missing + 1 ))
    fi
done < <(expected_edges)

(( missing == 0 )) || fail "${missing} expected downward edge(s) measured zero. The
  chain requires them to be positive, so this is attribution failing rather than
  purity holding."

# The table cannot be neutered one archive at a time either: every archive that
# was analysed at a real layer must be named by at least one expected edge.
for label in ${ANALYSED}; do
    expected_edges | grep -q "^${label} " || fail "archive '${label}' was inspected
  but is named by no expected downward edge. Without one, a zero row for it
  would read as purity instead of as attribution having stopped."
done
note "V5 expected downward edges ok: $(expected_edges | grep -c .) present and non-zero, covering ${ANALYSED}"

(( disagreements == 0 )) || fail "${disagreements} symbol(s) classified differently by
  the demangled-name and mangled-name rules, listed above. The two must agree;
  a disagreement means one of them has stopped attributing correctly and the
  purity result cannot be trusted."
note "V4 classifier agreement ok: demangled and mangled rules agree on every symbol"

(( violations == 0 )) || fail "${violations} upward reference(s), listed above. The
  layer chain is Nova <- Splash <- { Vazio, Clouds }; Vazio and Clouds are
  siblings and may not name each other. Move the code down, or invert the
  dependency."

note "PASS: no archive references or defines a symbol belonging to a layer above it"
