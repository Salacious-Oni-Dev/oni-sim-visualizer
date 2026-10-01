#!/usr/bin/env bash
# The offline check suite for oni-sim-visualizer.
#
# WHY THIS EXISTS. `oni-sim-replacement/driver` has vftest and GOLDENS.txt, and
# `oni-framework-api` has --ext-selftest. Without a suite of its own, this program's check
# would be one person typing four commands and looking at twelve PNGs, which is the state that
# lets GUI work break a readout nobody reruns.
#
# WHAT IT CHECKS, AND THE THREE KINDS. The distinction is the whole design, because a suite
# that fails whenever the SimDLL legitimately changes is a suite that gets ignored by the
# third week.
#
#   OWNED   Things this repo produces on its own, with no simulation anywhere near them: the
#           flag table, the font sheet, the twelve UI sample frames. Hard goldens. A change
#           here is a change somebody made in this repo, so it should be recorded on purpose.
#
#   RELATION  Claims of the form "these two things must agree", which stay true no matter what
#           the sim does: an offline render and an attached render of the same world are
#           byte-identical; a seeded run reproduces across processes; a seek reproduces the
#           run; a refusal refuses. These are the arms with teeth and they are DLL-independent.
#
#   PINNED  Numbers the SIMULATION produces -- digests, total mass, quantiles, what the
#           registry holds. They are recorded together with an identity for the SimDLL and the
#           md5 of the corpus that produced them, and they SKIP (loudly, with the reason) when
#           either input has changed. A sim change is then a skip to re-record, never a red suite.
#
#           THAT IDENTITY IS NOT THE md5 OF THE DLL, and it used to be. Measured:
#           two builds of ONE clean commit, same compiler, same flags, minutes apart, produce
#           SimDLL.dll files with different md5s -- the mingw build is not byte-reproducible --
#           and both produce all twenty-five goldens identically, to the digit. So a binary md5
#           made every rebuild indistinguishable from a sim change, and the seven pinned arms
#           skipped for days while the sim they were meant to guard did not move at all. A pin that cannot tell a rebuild from a change guards nothing; see
#           sim_ident() below for what replaced it.
#
# WHY EVERY SIM ARM PINS A SEED. Measured: this corpus boots through AllocateCells,
# which seeds the random stream from the wall clock, so two processes running the same command
# DIVERGE at 200 ticks -- and AGREE at 5, because physics has not reached a gas yet. A
# determinism arm written at 5 ticks passes by luck and guards nothing. Every arm here that
# ticks passes --seed, and the arm that proves the seed is doing work is the one that requires
# two DIFFERENT seeds to disagree.
#
# usage:
#   ./test.sh                 run every arm against GOLDENS.txt
#   ./test.sh --record        re-record GOLDENS.txt from this build (review the diff!)
#   ./test.sh --list          print the arm names and exit
#   SIMVIZ_CORPUS=... ./test.sh      drive a different corpus (PINNED arms will skip)
#   SIMVIZ_DLL=... ./test.sh         drive a different SimDLL   (same)
#
# Exit status is the number of failed arms, capped at 99, so a caller can branch on it.

# NOT `set -e`. A suite that stops at the first failure reports one bug per run, and the
# reason to have a suite at all is to learn everything that broke from one run.
set -uo pipefail

cd "$(dirname "$0")"

EXE=./build/simviz.exe
CORPUS="${SIMVIZ_CORPUS:-sim_corpus.bin}"
DLL="${SIMVIZ_DLL:-../oni-sim-replacement/sim/build/SimDLL.dll}"
GOLDENS=GOLDENS.txt
# Not 9788. That is the port a real inspector server listens on, and this suite must be
# runnable while a game is up without either one answering for the other.
PORT="${SIMVIZ_TEST_PORT:-9799}"
# Every run of the exe is wrapped in this. WHY: --checkpoint-test and --timeline-test do not
# return at all against a SimDLL that has no checkpoint messages -- measured against Klei's
# original, where both sat past a minute for work ours does in two seconds. A suite that can
# hang is worse than no suite, because the thing it blocks is a CI job with nobody watching.
# `timeout` genuinely kills a binfmt-launched .exe here (rc 124, process gone -- measured);
# that is NOT true of the game, which oni-run.sh has to stop for itself.
TIMEOUT_S="${SIMVIZ_TEST_TIMEOUT:-180}"
# Prefix for every invocation of the exe. EMPTY on this machine and that is the point: WSL
# runs a .exe through WSLInterop, which is real Windows, so the goldens below were recorded by
# Windows itself. A hosted Linux runner has no binfmt registration at all and would fail with
# "exec format error" before a single arm ran, so set SIMVIZ_RUNNER=wine there. MEASURED
# with wine 10.0 against WSLInterop: --help, the font sheet and all twelve UI frames
# come out byte-identical, which is what the owned software rasterizer was for.
RUNNER="${SIMVIZ_RUNNER:-}"

RECORD=0
LIST=0
for a in "$@"; do
  case "$a" in
    --record) RECORD=1 ;;
    --list) LIST=1 ;;
    -h|--help) sed -n '2,50p' "$0"; exit 0 ;;
    *) echo "unknown argument '$a' -- try --help" >&2; exit 2 ;;
  esac
done

npass=0; nfail=0; nskip=0
declare -a FAILED=()

# Everything transient lands here and nothing outside it is written, so a failed run leaves
# no half-made PNG for the next one to compare against.
WORK="$(mktemp -d "${TMPDIR:-/tmp}/simviz-test.XXXXXX")"
STUB_PID=""
cleanup() {
  # By PID, never by `%1`: a `kill %1` in a non-interactive shell leaves the stub holding the
  # script's stdout open, and the run then hangs after its last line has already printed.
  if [ -n "$STUB_PID" ] && kill -0 "$STUB_PID" 2>/dev/null; then
    kill "$STUB_PID" 2>/dev/null
    wait "$STUB_PID" 2>/dev/null
  fi
  rm -rf "$WORK"
}
trap 'cleanup' EXIT INT TERM

# The only way the exe is ever started. Returns 124 when the budget ran out.
sv() { timeout "$TIMEOUT_S" $RUNNER "$EXE" "$@"; }

say()  { printf '%s\n' "$*"; }
pass() { npass=$((npass+1)); printf '  PASS  %s\n' "$1"; }
skip() { nskip=$((nskip+1)); printf '  SKIP  %s\n        %s\n' "$1" "$2"; }
fail() {
  nfail=$((nfail+1)); FAILED+=("$1")
  printf '  FAIL  %s\n        %s\n' "$1" "$2"
}

# ---------------------------------------------------------------------------- normalisation
#
# Two things in this program's own output are not properties of the run and must never reach
# a golden. The absolute path of a temporary directory is one. The other is `rng state after
# boot`, which is the wall clock: it is DIFFERENT on every single run by construction, seed or
# no seed, because --seed pins the stream AFTER the boot that reported it.
norm() {
  sed -e "s|$WORK|<work>|g" \
      -e "s|^rng state after boot: 0x[0-9A-F]*$|rng state after boot: <wallclock>|" \
      -e "s|[0-9]\+\.[0-9]\+ ms|<ms>|g"
}

md5() { md5sum "$1" 2>/dev/null | cut -d' ' -f1; }

# sim_ident <dll> -- what the PINNED arms are pinned TO. Sets SIM_ID and SIM_STAMP; it assigns
# rather than prints because a `$(...)` around it would run it in a subshell and the stamp, which
# the header line reports, would never come back.
#
# The question the pinned arms need answered is "could this DLL produce different numbers from
# the one these goldens were recorded from", and the md5 of the file answers a different one:
# "are these the same bytes". Those came apart (see the header) -- the build is
# not byte-reproducible, so the md5 said yes-it-changed on every rebuild and the arms were
# permanently skipped.
#
# What actually decides the numbers is the SOURCE the DLL was built from. oni-sim-replacement's
# sim/build.sh compiles exactly one translation unit with no include path outside sim/ and bakes
# its version into the binary as MAJOR.MINOR.REV+SHA, with a trailing `*` when the tree was
# dirty at build time. So the stamp names the commit, and `<sha>:sim` is that commit's sim tree
# hash -- which changes when, and only when, a file under sim/ changes. sim/build.sh and
# sim/VERSION are in that tree too, so a compiler flag moving counts as a change, which it is.
#
# Four cases, and each degrades to the most specific thing still knowable:
#
#   no stamp      Not a build of oni-sim-replacement's sim (Klei's own DLL, a stub fixture). `unknown`
#                 never matches a recorded value, so the arms skip, which is right: nothing in
#                 this repo can say what such a DLL should produce.
#   clean stamp   `tree:<hash>` -- the exact source identity, and two rebuilds of it agree.
#   dirty stamp   `worktree:<md5>` over the tracked files in sim/ as they stand NOW. The built
#                 source was never committed and cannot be recovered from the binary, so this
#                 assumes the DLL was built from the tree in front of us. It errs toward
#                 skipping: edit a header after building and the hash moves even though the DLL
#                 did not.
#   sha unknown   `commit:<sha>` -- built from a commit this checkout does not have (another
#                 worktree, an unfetched branch). Still stable across rebuilds, which is the
#                 property the md5 lacked.
sim_ident() {
  local dll="$1" stamp sha dirty=0 repo tree
  # grep -a rather than strings(1): one less tool to require, and the pattern is specific.
  stamp=$(grep -a -o -E '[0-9]+\.[0-9]+\.[0-9]+\+[0-9a-f]{7,40}\*?' "$dll" 2>/dev/null | head -1)
  SIM_STAMP="${stamp:-none}"
  if [ -z "$stamp" ]; then SIM_ID=unknown; return; fi
  sha="${stamp#*+}"
  case "$sha" in *\*) dirty=1; sha="${sha%\*}" ;; esac
  # The repo the DLL came out of, and failing that this checkout's own oni-sim-replacement: a commit
  # hash names the same commit in any clone that has it, so resolving a DLL from elsewhere
  # against the sibling checkout is exact, not a guess. Only a commit NOBODY here has degrades.
  repo=$(git -C "$(dirname "$dll")" rev-parse --show-toplevel 2>/dev/null) \
     || repo=$(git -C ../oni-sim-replacement rev-parse --show-toplevel 2>/dev/null) || repo=""
  if [ -z "$repo" ]; then SIM_ID="commit:$sha"; return; fi
  if [ "$dirty" = 1 ]; then
    SIM_ID="worktree:$( (cd "$repo" && git ls-files -z sim | xargs -0 md5sum) 2>/dev/null \
                        | sort | md5sum | cut -d' ' -f1)"
    return
  fi
  tree=$(git -C "$repo" rev-parse "$sha^{commit}:sim" 2>/dev/null) || tree=""
  if [ -n "$tree" ]; then SIM_ID="tree:$tree"; else SIM_ID="commit:$sha"; fi
}

# ---------------------------------------------------------------------------- goldens
#
# `key<space>value`, one per line, `#` comments. Read into an associative array once.
declare -A GOLD=()
if [ -f "$GOLDENS" ]; then
  while IFS=' ' read -r k v; do
    case "$k" in ''|'#'*) continue ;; esac
    GOLD["$k"]="$v"
  done < "$GOLDENS"
fi
declare -A NEWGOLD=()
declare -a NEWORDER=()

record_value() {
  NEWGOLD["$1"]="$2"
  NEWORDER+=("$1")
}

# golden <name> <value> -- an OWNED or RELATION golden. Hard.
golden() {
  local name="$1" val="$2"
  if [ "$RECORD" = 1 ]; then record_value "$name" "$val"; pass "$name (recorded $val)"; return; fi
  local want="${GOLD[$name]:-}"
  if [ -z "$want" ]; then
    fail "$name" "no golden recorded -- run ./test.sh --record"
  elif [ "$want" = "$val" ]; then
    pass "$name"
  else
    fail "$name" "golden $want, got $val"
  fi
}

# pinned <name> <value> -- a number the SIM produced. Skipped when the inputs that produced
# the recorded one are not the inputs in front of us, because that difference is an input
# difference and calling it a failure is how a suite trains its reader to ignore it.
pinned() {
  if [ "$INPUTS_MATCH" != 1 ] && [ "$RECORD" != 1 ]; then
    skip "$1" "$INPUTS_WHY"
    return
  fi
  golden "$@"
}

# check <name> <expected> <actual> -- a RELATION, asserted here rather than recorded.
check() {
  if [ "$2" = "$3" ]; then pass "$1"; return; fi
  # Multi-line values (a digest block) are reported as their first difference rather than
  # in full: a failure nobody can read scrolling past is a failure nobody acts on.
  if [ "$(printf '%s' "$2" | wc -l)" -gt 1 ]; then
    fail "$1" "the two differ, first at:
$(diff <(printf '%s\n' "$2") <(printf '%s\n' "$3") | head -6 | sed 's/^/          /')"
  else
    fail "$1" "expected '$2', got '$3'"
  fi
}

check_contains() {
  if printf '%s' "$2" | grep -qF -- "$3"; then pass "$1"
  else fail "$1" "output does not contain '$3'"; fi
}

# The arm inventory, and it is ABOVE the preflight on purpose. `--list` answers "what does
# this suite claim to check", which is a question about this file and not about the machine
# it is sitting on -- but it used to be printed after the corpus, the DLL, the exe and the
# port had all been checked, so on a fresh clone with nothing built `./test.sh --list` exited
# 2 and printed a complaint about a missing corpus. CI reads this list to assert that every
# arm named here actually reported a result, which is exactly the check that has to work on a
# tree where the build has not run yet.
SEED=7
TICKS=200

# Every arm, in order. `--list` prints these and stops.
ARMS="
owned.help              the flag table --help prints
owned.flagtable         every parsed flag is in the table and every table flag is parsed
owned.coverage          no flag is reachable only from a command line
owned.font              the bitmap font sheet
owned.uisample          the twelve UI sample frames
owned.uisample.sourcedep  the tools tab reads the source and the tests tab does not
relation.determinism    a seeded run reproduces across two processes
relation.seedworks      two different seeds do not produce the same run
relation.png            the same seeded run draws the same PNG twice
relation.camera.seat    --camera and --zoom/--hover seat one camera, not two
relation.camera.fit     --camera reaches the fit-to-window scale --zoom cannot
relation.camera.refuse  --camera is refused where there is no camera to place
relation.attr.perelement  the attr view has one colour per attribute value, not per cell
relation.attr.consistent  the attr range and the attr quantiles agree on what is lit
relation.attr.refuse    an attribute nothing registers is refused by name, not drawn flat
relation.checkpoint     --checkpoint-test
relation.timeline       --timeline-test
relation.attach.views   attached and offline render the same world identically
relation.attach.digest  and hash identically, over the arrays rather than the pixels
relation.attach.ext     the same, for the extension registry view
relation.attach.attrs   the attached element-attribute reader
relation.attach.attrview  the attr view over the wire, and what it says it cannot draw
relation.attach.noext   a stub with no /extcells reports a missing route
relation.attach.legacy  an older oni-framework-api is refused by name
relation.attach.corrupt a truncated body is refused
relation.refuse.drive   a sim-driving flag is refused while attached
relation.refuse.source  --corpus and --attach together are refused
relation.refuse.seed    a DLL with no extension surface refuses --seed, not claims it
relation.bisect.same    a run does not diverge from itself
relation.bisect.differ  a planted divergence is found, with a tick and a component
pinned.digest           the digest block after $TICKS seeded ticks
pinned.mass             total mass after $TICKS seeded ticks
pinned.stats            the temperature quantiles
pinned.elements         the element table this corpus carries
pinned.extlist          what registry 1 holds offline
pinned.extattrs         what registry 2 holds offline
pinned.attrview         the attr render of sim.molecular_mass
"

if [ "$LIST" = 1 ]; then printf '%s\n' "$ARMS"; exit 0; fi

# ---------------------------------------------------------------------------- preflight

if [ ! -x "$EXE" ]; then
  say "no $EXE -- run ./build.sh first"; exit 2
fi
if [ ! -f "$CORPUS" ]; then
  say "no corpus at $CORPUS"
  say "record one with oni-sim-replacement's shim/ (see README.md) and put it here,"
  say "or set SIMVIZ_CORPUS to its path"; exit 2
fi
if [ ! -f "$DLL" ]; then
  say "no SimDLL at $DLL -- build it in oni-sim-replacement, or set SIMVIZ_DLL"; exit 2
fi

# Refused rather than run into. MEASURED, once, while writing the attr arms: a stub left over
# from a hand-run test was still listening on $PORT, the suite's own start_stub saw the port
# accept and went ahead, and the attach arms then compared this run's world against THAT
# stub's -- which failed as "7 views differ", a message that describes the symptom and points
# nowhere near the cause. A suite whose failures mislead is worse than one arm short.
if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
  exec 3<&- 3>&-
  say "something is already listening on 127.0.0.1:$PORT."
  say "The attach arms serve their own fixtures there and would compare against whatever that"
  say "is. Stop it, or set SIMVIZ_TEST_PORT to a free port."
  exit 2
fi

SIM_STAMP=none
SIM_ID=unknown
sim_ident "$DLL"
CORPUS_MD5="$(md5 "$CORPUS")"
INPUTS_MATCH=1
INPUTS_WHY=""
if [ "$RECORD" != 1 ]; then
  want_sim="${GOLD[input.sim.src]:-}"
  want_corpus="${GOLD[input.corpus.md5]:-}"
  if [ "$SIM_ID" = unknown ]; then
    INPUTS_MATCH=0
    INPUTS_WHY="the DLL under test carries no oni-sim-replacement build stamp, so what it should produce is not something this repo can say. Point --dll/SIMVIZ_DLL at a build of oni-sim-replacement's sim."
  elif [ -n "$want_sim" ] && [ "$want_sim" != "$SIM_ID" ]; then
    INPUTS_MATCH=0
    INPUTS_WHY="the sim source changed since these were recorded ($want_sim -> $SIM_ID). Re-record with ./test.sh --record after checking the change was intended."
  elif [ -n "$want_corpus" ] && [ "$want_corpus" != "$CORPUS_MD5" ]; then
    INPUTS_MATCH=0
    INPUTS_WHY="corpus changed since these were recorded ($want_corpus -> $CORPUS_MD5)."
  fi
fi


# ------------------------------------------------------------------ the corpus's world
#
# THE RELATION ARMS HOLD FOR ANY CORPUS, SO THEIR CELLS MUST TOO. They were written against
# the 636x404 reference world and used to name its cells outright -- a hover at 300,200, a
# gas injection at 350,395, a fit-to-window camera worked out for 636x404 -- so a corpus of
# any other shape failed six of them for reasons that had nothing to do with the program:
# on a 64x48 capture, every one of those cells is off the world.
# The world's size is read once here, and every cell below is placed at the same PROPORTION
# of it that the reference cell had, clamped so a rectangle stays inside. On the reference
# world that is the identity, so what those arms test there has not moved.
BOOT_OUT="$(sv --corpus "$CORPUS" --dll "$DLL" --ticks 0 2>&1 | tr -d '\r')"
read -r WORLD_W WORLD_H < <(printf '%s\n' "$BOOT_OUT" \
                            | sed -n 's/^booted: \([0-9]*\)x\([0-9]*\) .*/\1 \2/p')
if [ -z "${WORLD_W:-}" ] || [ -z "${WORLD_H:-}" ]; then
  say "could not boot $CORPUS on $DLL to read its world size"; exit 2
fi
# Refused rather than run into. A corpus that loads its world after its last frame boots onto
# a frame published for an EARLIER world, and every arm that reads tick 0 then compares
# meaningless values: measured on a corpus recorded from a new game, it failed attach.views,
# skipped attach.ext for "no gas" in a world full of it, and failed the timeline's tick-0
# stop -- three misleading reports and no true one. The program's own warning names the
# recording mistake, so it is passed on as it is.
if printf '%s\n' "$BOOT_OUT" | grep -q '^WARNING: this corpus loads its world after its last frame'; then
  printf '%s\n' "$BOOT_OUT" | grep -A4 '^WARNING: this corpus loads'
  say "This suite needs a corpus whose world has been published at boot; see the warning."
  exit 2
fi
# wx X [W]: a reference-world x at the same proportion of this world, leaving room for a
# rectangle W wide. wy likewise for y. Integer arithmetic, so 300 on 636 is exactly 300.
wx() { local x=$(( $1 * WORLD_W / 636 )) w=${2:-1}; [ $((x + w)) -gt "$WORLD_W" ] && x=$((WORLD_W - w)); [ "$x" -lt 0 ] && x=0; echo "$x"; }
wy() { local y=$(( $1 * WORLD_H / 404 )) h=${2:-1}; [ $((y + h)) -gt "$WORLD_H" ] && y=$((WORLD_H - h)); [ "$y" -lt 0 ] && y=0; echo "$y"; }
HOVER_X=$(wx 300); HOVER_Y=$(wy 200)
PROBE_RECT="$(wx 300 12),$(wy 200 12),12,12"
# One probe of the whole world at tick 0 -- the state the attach stub serves -- for the two
# arms that need to know what the world HOLDS rather than only how big it is. About 30 s on
# the reference world. Columns: x y cell elem hash phase mass ...; phase 1 is gas.
sv --corpus "$CORPUS" --dll "$DLL" --probe "0,0,$WORLD_W,$WORLD_H" 2>/dev/null | tr -d '\r' \
  | awk '$1 ~ /^[0-9]+$/ && $7 > 0' > "$WORK/probe.txt"
# The gas injection needs gas under it: --inject-gas refuses a rectangle with none, and says
# so. The reference world's rectangle when this world has gas there, else one on the first
# gas cell the probe found; empty when the world holds no gas at all.
gas_in() { awk -v x0="$1" -v y0="$2" '$6 == 1 && $1 >= x0 && $1 < x0 + 10 && $2 >= y0 && $2 < y0 + 8 {f = 1}
                                      END {exit !f}' "$WORK/probe.txt"; }
GAS_X=$(wx 350 10); GAS_Y=$(wy 395 8)
if ! gas_in "$GAS_X" "$GAS_Y"; then
  read -r GAS_X GAS_Y < <(awk '$6 == 1 {print $1, $2; exit}' "$WORK/probe.txt")
  if [ -n "${GAS_X:-}" ]; then
    [ $((GAS_X + 10)) -gt "$WORLD_W" ] && GAS_X=$((WORLD_W - 10)); [ "$GAS_X" -lt 0 ] && GAS_X=0
    [ $((GAS_Y + 8)) -gt "$WORLD_H" ] && GAS_Y=$((WORLD_H - 8)); [ "$GAS_Y" -lt 0 ] && GAS_Y=0
  fi
fi
GAS_RECT="${GAS_X:+$GAS_X,$GAS_Y,10,8}"

say "oni-sim-visualizer check suite"
say "  exe     $EXE"
say "  corpus  $(basename "$CORPUS")  md5 $CORPUS_MD5"
say "  dll     $DLL  stamp $SIM_STAMP"
say "  sim     $SIM_ID"
say "  world   ${WORLD_W}x${WORLD_H}"
if [ "$RECORD" = 1 ]; then say "  MODE    RECORDING -- review the diff to $GOLDENS before committing"; fi
if [ "$INPUTS_MATCH" != 1 ]; then say "  NOTE    pinned arms will skip: $INPUTS_WHY"; fi
say ""

record_value input.sim.src "$SIM_ID"
record_value input.corpus.md5 "$CORPUS_MD5"

# A run of the offline path. Always seeded, always through norm().
run_off() { sv --corpus "$CORPUS" --dll "$DLL" "$@" 2>&1 | norm; }

# ---------------------------------------------------------------------------- OWNED
say "OWNED -- produced by this repo alone, no simulation involved"

golden owned.help "$(sv --help 2>&1 | norm | md5sum | cut -d' ' -f1)"

# The parser and the table are two lists of the same flags and they drift apart silently:
# a flag added to one and not the other is either undocumented or documented and dead.
# Both forms of parse arm count -- strcmp for a plain flag, strncmp for the `=a,b` ones.
parser_flags="$(grep -oE 'strn?cmp\(a, "(--[a-z-]+)"' src/main.cpp \
                | sed 's/.*"\(--[a-z-]*\)".*/\1/' | sort -u)"
table_flags="$(python3 - <<'PY'
import re
s = open('src/help.h').read()
print("\n".join(sorted(set(re.findall(r'\{(?:"[^"]*"),\s*"(--[a-z-]+)"', s)))))
PY
)"
if [ "$parser_flags" = "$table_flags" ]; then
  pass "owned.flagtable ($(printf '%s\n' "$parser_flags" | wc -l) flags)"
else
  fail "owned.flagtable" "parser and help.h disagree:
$(diff <(printf '%s\n' "$parser_flags") <(printf '%s\n' "$table_flags") | head -20)"
fi

# The point of the whole GUI effort, asserted rather than asserted-in-prose: a flag with an
# empty `control` column is a flag you can only reach by restarting with a command line.
missing="$(python3 - <<'PY'
import re
s = open('src/help.h').read()
body = s[s.index('static const Entry table[]'):]
# Each entry is {group, flag, arg, text..., control}. The control is the last string before
# the closing brace, so match the whole entry and take its final literal.
ents = re.findall(r'\{\s*"(?:[^"\\]|\\.)*"\s*,\s*"(--[a-z-]+)".*?\}', body, re.S)
bad = []
for m in re.finditer(r'\{\s*"(?:[^"\\]|\\.)*"\s*,\s*"(--[a-z-]+)"(.*?)\},', body, re.S):
    flag, rest = m.group(1), m.group(2)
    lits = re.findall(r'"((?:[^"\\]|\\.)*)"', rest)
    if not lits or lits[-1] == "":
        bad.append(flag)
print(" ".join(bad))
PY
)"
if [ -z "$missing" ]; then pass "owned.coverage (0 flags without a control)"
else fail "owned.coverage" "reachable only from a command line: $missing"; fi

sv --font-sample --snapshot "$WORK/font.png" >/dev/null 2>&1
golden owned.font "$(md5 "$WORK/font.png")"

mkdir -p "$WORK/ui"
# The exe's own stdout goes to a file rather than to this script's, so that a caller piping
# the suite into `head` cannot close the pipe under a sample that is still being written --
# observed once, as an arm that reported 2 frames where every other run reports 12.
sv --ui-sample --snapshot "$WORK/ui/ui.png" >"$WORK/uisample.log" 2>&1
ui_manifest="$(cd "$WORK/ui" && md5sum * 2>/dev/null | sort -k2)"
ui_count="$(printf '%s\n' "$ui_manifest" | grep -c . )"
if [ "$ui_count" -lt 12 ]; then
  fail "owned.uisample" "expected 12 sample frames, got $ui_count:
$(printf '%s\n' "$ui_manifest" | sed 's/^/          /')
        the exe said:
$(sed 's/^/          /' "$WORK/uisample.log" | tail -5)"
else
  golden owned.uisample "$(printf '%s\n' "$ui_manifest" | md5sum | cut -d' ' -f1)"
  # Recorded per frame as well, so a failure names the frame instead of the set.
  while read -r h f; do golden "owned.uisample.$f" "$h"; done <<< "$ui_manifest"

  # Two of the twelve are the same tab drawn against both sources, and which of them moves
  # is a claim rather than a coincidence. The TOOLS tab must differ: its third list button
  # reads `events list` attached and `elements` offline. The TESTS tab must NOT: every run
  # it starts drives a corpus in this process, so a tests tab that changed with the source
  # would be offering a run it cannot make.
  ui_md5() { printf '%s\n' "$ui_manifest" | awk -v f="$1" '$2==f{print $1}'; }
  if [ "$(ui_md5 ui-4.png)" = "$(ui_md5 ui-4-offline.png)" ]; then
    fail "owned.uisample.sourcedep" "the tools tab renders identically attached and offline, so nothing on it is reading the source"
  elif [ "$(ui_md5 ui-5.png)" != "$(ui_md5 ui-5-offline.png)" ]; then
    fail "owned.uisample.sourcedep" "the tests tab changed with the source; every run it starts drives a corpus, so it has nothing to read the source for"
  else
    pass "owned.uisample.sourcedep (tools tab moves, tests tab does not)"
  fi
fi

# ---------------------------------------------------------------------------- RELATION
say ""
say "RELATION -- claims that hold whatever the sim does"

# Two processes, same seed, same corpus. This is the arm the whole suite's sim coverage
# rests on, and the reason it runs at $TICKS rather than at 5 is that at 5 it passes
# without a seed as well.
a="$(run_off --seed $SEED --ticks $TICKS --digest | sed -n '/^digest after/,/^  scheduling/p')"
b="$(run_off --seed $SEED --ticks $TICKS --digest | sed -n '/^digest after/,/^  scheduling/p')"
if [ -z "$a" ]; then fail "relation.determinism" "no digest block in the output"
else check "relation.determinism" "$a" "$b"; fi

# And the seed is doing work rather than being accepted and ignored -- the failure mode that
# would make the arm above pass for the wrong reason.
c="$(run_off --seed 8 --ticks $TICKS --digest | sed -n '/^digest after/,/^  scheduling/p')"
if [ "$a" != "$c" ]; then pass "relation.seedworks"
else fail "relation.seedworks" "--seed 7 and --seed 8 produced the same $TICKS ticks"; fi

run_off --seed $SEED --ticks 20 --view temperature --snapshot "$WORK/p1.png" >/dev/null 2>&1
run_off --seed $SEED --ticks 20 --view temperature --snapshot "$WORK/p2.png" >/dev/null 2>&1
check "relation.png" "$(md5 "$WORK/p1.png")" "$(md5 "$WORK/p2.png")"

# ------------------------------------------------------ the camera, and the line that shows it
#
# WHAT THESE GUARD is the claim the help tab makes in so many words: "this line reproduces
# what is on screen". It did not. The command it printed carried --size and --window and
# nothing about where the camera was, so a picture found by dragging and zooming was handed
# on as a picture of the whole world at 1x. `--camera cx,cy,scale` is what closed that, and
# these arms are what keep it closed.
#
# The first is the one with teeth. `--camera` and `--zoom`/`--hover` are two ways of asking
# for the same camera and they run through ONE function (SeatCamera, frame.h) precisely so
# they cannot disagree; a second copy of that arithmetic would drift, and the drift would be
# invisible because both halves would still be pictures of the same world -- the exact defect
# the attr view was once caught in. At 1100x760 with zoom 8 centred on cell x,y,
# the seat is cx = x + 0.5 - 1100/16, cy = y + 0.5 - 760/16 -- on the reference world's
# 300,200 that is 231.75,153.
SEAT="$(awk -v x="$HOVER_X" -v y="$HOVER_Y" 'BEGIN { printf "%.2f,%.2f", x + 0.5 - 1100/16, y + 0.5 - 760/16 }')"
run_off --seed $SEED --ticks 5 --view temperature --hud --size 1100x760 \
        --zoom 8 --hover "$HOVER_X,$HOVER_Y" --snapshot "$WORK/cam-human.png" >/dev/null 2>&1
run_off --seed $SEED --ticks 5 --view temperature --hud --size 1100x760 \
        --camera "$SEAT,8" --hover "$HOVER_X,$HOVER_Y" --snapshot "$WORK/cam-exact.png" >/dev/null 2>&1
if [ ! -s "$WORK/cam-human.png" ] || [ ! -s "$WORK/cam-exact.png" ]; then
  fail "relation.camera.seat" "one of the two composed frames was not written"
else
  check "relation.camera.seat" "$(md5 "$WORK/cam-human.png")" "$(md5 "$WORK/cam-exact.png")"
fi

# And the case --crop and --zoom CANNOT express, which is why the flag exists at all: the
# default camera is fit-to-window, and that scale is fractional -- on the reference world
# 1100/636 vs 760/404, so min(1.72956, 1.88119). An integer magnification about a cell cannot
# land there from any argument. The three numbers are COMPUTED HERE from FitTo's arithmetic
# (frame.h), rounded to single precision at every step as the program does, not read back
# out of the program: a value the subject printed for itself would agree with the subject by
# construction and prove nothing. On the reference world this gives
# 0,-17.7090912,1.72955978, the values that used to be written here by hand.
FIT_CAM="$(python3 - "$WORLD_W" "$WORLD_H" <<'EOF'
import struct, sys
f = lambda x: struct.unpack('<f', struct.pack('<f', x))[0]
w, h = int(sys.argv[1]), int(sys.argv[2])
s = min(f(1100 / w), f(760 / h))
print('%.9g,%.9g,%.9g' % (f(f(w - f(1100 / s)) * 0.5), f(f(h - f(760 / s)) * 0.5), s))
EOF
)"
run_off --seed $SEED --ticks 5 --view temperature --hud --size 1100x760 \
        --snapshot "$WORK/cam-fit.png" >/dev/null 2>&1
run_off --seed $SEED --ticks 5 --view temperature --hud --size 1100x760 \
        --camera "$FIT_CAM" --snapshot "$WORK/cam-fitexact.png" >/dev/null 2>&1
if [ ! -s "$WORK/cam-fit.png" ] || [ ! -s "$WORK/cam-fitexact.png" ]; then
  fail "relation.camera.fit" "one of the two composed frames was not written"
else
  check "relation.camera.fit" "$(md5 "$WORK/cam-fit.png")" "$(md5 "$WORK/cam-fitexact.png")"
fi

# Three refusals, each of which is a different mistake. A plain snapshot has no camera to
# seat and a fractional scale could not be honoured by it if it had; two numbers is a typo;
# a scale of zero would divide by zero in every screen<->world conversion and draw a blank
# frame with no error in it. All three must exit non-zero AND name --camera -- a refusal
# nobody can act on is barely better than none.
cam_refuse=0
for cam_args in "--camera 1,2,3 --snapshot $WORK/cam-refuse.png" \
                "--camera 1,2 --window" \
                "--camera 1,2,0 --window"; do
  # `sv` and not `run_off`: run_off pipes through norm, so `$?` would be norm's status and
  # every one of these would read as accepted.
  # shellcheck disable=SC2086
  cam_out="$(sv --corpus "$CORPUS" --dll "$DLL" --view mass $cam_args 2>&1)"; cam_rc=$?
  if [ $cam_rc -eq 0 ]; then
    fail "relation.camera.refuse" "\`$cam_args\` was accepted (rc 0)"
    cam_refuse=1; break
  fi
  case "$cam_out" in
    *--camera*) ;;
    *) fail "relation.camera.refuse" "\`$cam_args\` was refused without naming --camera:
$(printf '%s' "$cam_out" | sed 's/^/          /' | head -4)"
       cam_refuse=1; break ;;
  esac
done
[ $cam_refuse -eq 0 ] && pass "relation.camera.refuse (plain snapshot, short tuple, zero scale)"

# ------------------------------------------------------- the attr view, registry 2 as pixels
#
# THE ARM WITH THE TEETH IS THE FIRST ONE, and what it guards is the difference between the
# two registries. A per-CELL property (--view ext:) has a value per cell and its render can
# legitimately hold thousands of colours; a per-ELEMENT attribute (--view attr:) has one
# value per element, so every cell of the same element MUST take the same colour and the
# render can hold no more colours than there are distinct values, plus one backdrop per
# phase. Four phases -- vacuum, gas, liquid, solid -- is a fact of the sim rather than of
# this repo's palette, so the bound does not have to name a single one of views.h's colours.
#
# Both ends are asserted. Above the bound is the failure this exists to catch: an attr view
# that had drifted into reading something per-cell would blow straight past it. Below it is
# the failure that would make the first meaningless -- a view that painted one flat colour
# passes any upper bound at all.
#
# The PNG is decoded here rather than compared by md5 because a hash cannot count colours, and
# counting them is the whole claim. image.h writes filter byte 0 on every scanline (its own
# comment says so), so the decode is a zlib inflate and one byte dropped per row.
ATTR=sim.molecular_mass
run_off --view "attr:$ATTR" --snapshot "$WORK/attr.png" >"$WORK/attr.log" 2>&1
if [ ! -s "$WORK/attr.png" ]; then
  fail "relation.attr.perelement" "no attr render: $(tail -3 "$WORK/attr.log")"
else
  # Distinct values among the elements this table actually has a row for. Oxygen and polluted
  # oxygen share 31.9988, so four VALUES are three COLOURS, and the bound has to count what is
  # drawn rather than what is registered.
  # `tr -d '\r'` because this is a Windows build and every line it prints ends CRLF, which
  # a pattern anchored at end-of-line will never match. Only the arms that anchor need it.
  vals="$(run_off --ext-attrs 2>/dev/null | tr -d '\r' \
          | sed -n "/^  \[[0-9]*\] $ATTR /,/^$/p" \
          | grep -v 'not in table' | grep -oE '[0-9]+\.[0-9]+$' | sort -u | wc -l)"
  colours="$(python3 - "$WORK/attr.png" <<'EOF'
import struct, sys, zlib
d = open(sys.argv[1], 'rb').read()
i, idat, w, h, ct = 8, b'', 0, 0, 0
while i < len(d):
    n = struct.unpack('>I', d[i:i + 4])[0]
    t, body = d[i + 4:i + 8], d[i + 8:i + 8 + n]
    if t == b'IHDR':
        w, h, _, ct = struct.unpack('>IIBB', body[:10])
    elif t == b'IDAT':
        idat += body
    i += 12 + n
bpp = {0: 1, 2: 3, 6: 4}[ct]
raw = zlib.decompress(idat)
stride = w * bpp
seen = set()
for y in range(h):
    p = y * (stride + 1)
    assert raw[p] == 0, "image.h is meant to write filter byte 0 on every row"
    row = raw[p + 1:p + 1 + stride]
    for x in range(0, stride, bpp):
        seen.add(row[x:x + 3])
print(len(seen))
EOF
)"
  if [ -z "$colours" ] || [ -z "$vals" ] || [ "$vals" -lt 1 ]; then
    fail "relation.attr.perelement" "could not count colours ($colours) or values ($vals)"
  elif [ "$colours" -gt $((vals + 4)) ]; then
    fail "relation.attr.perelement" \
         "$colours colours for $vals distinct values -- at most $((vals + 4)) (one per value, one backdrop per phase). A per-cell read looks exactly like this."
  elif [ "$colours" -lt "$vals" ]; then
    fail "relation.attr.perelement" \
         "$colours colours for $vals distinct values -- values are being collapsed, so the view cannot tell two elements apart"
  else
    pass "relation.attr.perelement"
  fi
fi

# The range the ramp spans and the quantiles printed under it are computed by two different
# functions over two different rules -- the range takes the span of the values that are
# REACHABLE, the quantiles weight by area -- and the one thing they must agree on is which
# cells count as lit. p0 and p100 are the extremes of the lit cells, so they are the range,
# and a disagreement here is the two functions having drifted apart on what "unset" means.
attr_out="$(run_off --view "attr:$ATTR" --stats 2>&1 | tr -d '\r')"
r_lo="$(printf '%s' "$attr_out" | sed -n 's/^.* range over non-empty cells: \([0-9.-]*\) \.\. .*$/\1/p')"
r_hi="$(printf '%s' "$attr_out" | sed -n 's/^.* range over non-empty cells: [0-9.-]* \.\. \([0-9.-]*\) .*$/\1/p')"
q_lo="$(printf '%s' "$attr_out" | sed -n 's/^  p0\.0  *\([0-9.-]*\)$/\1/p')"
q_hi="$(printf '%s' "$attr_out" | sed -n 's/^  p100\.0  *\([0-9.-]*\)$/\1/p')"
if [ -z "$r_lo" ] || [ -z "$q_lo" ]; then
  fail "relation.attr.consistent" "no range or no quantiles: $(printf '%s' "$attr_out" | tail -2)"
else
  # Compared at the range line's three decimals, which is the coarser of the two printings.
  # One exception, and it is the range's own rule rather than a tolerance: a world where every
  # lit cell holds ONE value has a zero-width span, which ScanRange (views.h) widens to
  # lo .. lo + 1 so the ramp has somewhere to go. The quantiles are not widened, so there the
  # range's upper end is p100 + 1 by construction. Seen on a 64x48 capture holding only oxygen.
  want="$(printf '%.3f %.3f' "$q_lo" "$q_hi")"
  if [ "$(printf '%.3f' "$q_lo")" = "$(printf '%.3f' "$q_hi")" ]; then
    want="$(awk -v q="$q_lo" 'BEGIN { printf "%.3f %.3f", q, q + 1 }')"
  fi
  got="$(printf '%.3f %.3f' "$r_lo" "$r_hi")"
  if [ "$want" != "$got" ]; then
    check "relation.attr.consistent" "$want" "$got"
  elif [ -n "${vals:-}" ] && [ "$vals" -gt 1 ] && [ "$r_lo" = "$r_hi" ]; then
    # The colour-count bound above has an upper end with teeth and a lower end without one:
    # a lookup that collapsed every element onto ONE value would draw a single colour over
    # four backdrops, which is five, which is inside [values, values+4]. This is the check
    # that catches that -- $vals distinct values that all read the same are not $vals values.
    fail "relation.attr.consistent" \
         "$vals distinct values in the registry and the range spans none of them ($r_lo .. $r_hi) -- the per-element lookup is collapsing them"
  else
    pass "relation.attr.consistent"
  fi
fi

# A name nothing registers must be refused BY NAME and with a non-zero status. Drawn flat it
# would be the dim phase backdrop over the whole world -- which is a picture, and reads as
# "this world has none of that" rather than as "you typed something that does not exist".
out="$(run_off --view "attr:sim.no_such_attribute" 2>&1)"; rc=$?
if [ "$rc" = 0 ]; then
  fail "relation.attr.refuse" "an unregistered attribute was accepted"
elif printf '%s' "$out" | grep -qF "sim.no_such_attribute"; then
  pass "relation.attr.refuse"
else
  fail "relation.attr.refuse" "refused without naming it: $(printf '%s' "$out" | tail -2)"
fi

sv --corpus "$CORPUS" --dll "$DLL" --seed $SEED --checkpoint-test 20 >"$WORK/cp.log" 2>&1
rc=$?
if [ "$rc" = 0 ]; then pass "relation.checkpoint"
elif [ "$rc" = 124 ]; then fail "relation.checkpoint" "no answer in ${TIMEOUT_S}s -- a DLL with no checkpoint messages does not return here"
else fail "relation.checkpoint" "$(tail -3 "$WORK/cp.log")"; fi

sv --corpus "$CORPUS" --dll "$DLL" --seed $SEED --timeline-test 30 >"$WORK/tl.log" 2>&1
rc=$?
if [ "$rc" = 0 ]; then pass "relation.timeline"
elif [ "$rc" = 124 ]; then fail "relation.timeline" "no answer in ${TIMEOUT_S}s -- see relation.checkpoint"
else fail "relation.timeline" "$(tail -5 "$WORK/tl.log")"; fi

# ------------------------------------------------------- the attach path, with no game
#
# `tools/attach-stub.py` serves oni-framework-api's routes out of a state file the OFFLINE path
# wrote. Both sides then carry byte-identical arrays and the same element table, so any
# difference between the two renders is the attach path and nothing else.
start_stub() {
  python3 tools/attach-stub.py "$@" --port "$PORT" >"$WORK/stub.log" 2>&1 &
  STUB_PID=$!
  local i
  for i in $(seq 1 60); do
    if ! kill -0 "$STUB_PID" 2>/dev/null; then return 1; fi
    if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then exec 3<&- 3>&-; return 0; fi
    sleep 0.2
  done
  return 1
}
stop_stub() {
  [ -n "$STUB_PID" ] || return 0
  kill "$STUB_PID" 2>/dev/null
  wait "$STUB_PID" 2>/dev/null
  STUB_PID=""
}

sv --corpus "$CORPUS" --dll "$DLL" --dump-state "$WORK/state.bin" >/dev/null 2>&1
sv --corpus "$CORPUS" --dll "$DLL" --dump-elements >"$WORK/elements.txt" 2>/dev/null

if [ ! -s "$WORK/state.bin" ] || [ ! -s "$WORK/elements.txt" ]; then
  fail "relation.attach.views" "could not capture a state dump to serve"
elif ! start_stub "$WORK/state.bin" "$WORK/elements.txt"; then
  fail "relation.attach.views" "stub did not come up on $PORT: $(tail -3 "$WORK/stub.log")"
else
  bad=""
  for v in element temperature mass phase flow liquid sunlight; do
    sv --corpus "$CORPUS" --dll "$DLL" --view "$v" \
           --snapshot "$WORK/off-$v.png" >/dev/null 2>&1
    sv --attach "127.0.0.1:$PORT" --view "$v" \
           --snapshot "$WORK/live-$v.png" >/dev/null 2>&1
    if ! cmp -s "$WORK/off-$v.png" "$WORK/live-$v.png"; then bad="$bad $v"; fi
  done
  if [ -z "$bad" ]; then pass "relation.attach.views (7 views byte-identical)"
  else fail "relation.attach.views" "attached render differs offline for:$bad"; fi

  # Stronger than the seven PNGs above, and here because they cannot do it: a PNG only shows
  # what a view PAINTS. A fault injected into one vacuum cell's temperature changed no pixel
  # at all -- the temperature view paints a vacuum cell by its mass -- while the same fault
  # in a mid-world cell failed the arm immediately. The digest hashes the ARRAYS, so it sees
  # the cells no view draws.
  #
  # `registry` and `extCells` are dropped from the comparison rather than expected to match:
  # this stub serves no /extcells route, so the attached side reports them as zero, and that
  # is the correct answer to "the route is missing" rather than a difference in the data.
  # The arm below checks the missing-route case on its own.
  dig_strip() { sed -n '/^digest/,/total mass/p' | grep -vE '^  (registry|extCells)' | tail -n +2; }
  off_dig="$(run_off --digest | dig_strip)"
  att_dig="$(sv --attach "127.0.0.1:$PORT" --digest 2>&1 | norm | dig_strip)"
  if [ -z "$off_dig" ]; then fail "relation.attach.digest" "no offline digest to compare"
  else check "relation.attach.digest" "$off_dig" "$att_dig"; fi

  # The two compatibility refusals are checked against the same running stub, because what
  # they refuse is a REPLY, not a state of the world.
  stop_stub
fi

# An older oni-framework-api: /elements without idHash, colour and simState. Refused by name.
if start_stub "$WORK/state.bin" "$WORK/elements.txt" --legacy-elements; then
  out="$(sv --attach "127.0.0.1:$PORT" --view element --snapshot "$WORK/x.png" 2>&1)"
  check_contains "relation.attach.legacy" "$out" "/elements is missing"
  stop_stub
else fail "relation.attach.legacy" "stub did not come up"; fi

# Every /grid body truncated.
if start_stub "$WORK/state.bin" "$WORK/elements.txt" --corrupt; then
  out="$(sv --attach "127.0.0.1:$PORT" --view element --snapshot "$WORK/x.png" 2>&1)"
  rc=$?
  if [ "$rc" != 0 ] || printf '%s' "$out" | grep -qiE "short|truncat|refus|incomplete|bytes"; then
    pass "relation.attach.corrupt"
  else
    fail "relation.attach.corrupt" "a truncated body was accepted: $(printf '%s' "$out" | tail -2)"
  fi
  stop_stub
else fail "relation.attach.corrupt" "stub did not come up"; fi

# The extension registry over the wire: the sim's own blob, handed back by the stub
# untouched, must decode to the same picture the offline path draws from the sim direct.
[ -n "$GAS_RECT" ] && sv --corpus "$CORPUS" --dll "$DLL" --inject-gas "$GAS_RECT" \
       --ext-blob "$WORK/ext.bin" --dump-state "$WORK/state2.bin" >/dev/null 2>&1
if [ -z "$GAS_RECT" ]; then
  skip "relation.attach.ext" "this world holds no gas to inject into the mixture layer"
elif [ ! -s "$WORK/ext.bin" ]; then
  fail "relation.attach.ext" "no extension blob captured"
elif start_stub "$WORK/state2.bin" "$WORK/elements.txt" --extcells "$WORK/ext.bin"; then
  sv --corpus "$CORPUS" --dll "$DLL" --inject-gas "$GAS_RECT" \
         --view "ext:sim.gas_mass[0]" --snapshot "$WORK/off-ext.png" >/dev/null 2>&1
  sv --attach "127.0.0.1:$PORT" --view "ext:sim.gas_mass[0]" \
         --snapshot "$WORK/live-ext.png" >/dev/null 2>&1
  if cmp -s "$WORK/off-ext.png" "$WORK/live-ext.png"; then pass "relation.attach.ext"
  else fail "relation.attach.ext" "the ext view differs between the two sources"; fi
  stop_stub
else fail "relation.attach.ext" "stub did not come up"; fi

# Registry 2 over the wire. The stub synthesises this table rather than replaying one the sim
# wrote -- including an attribute with 0 values and 7 writes, and a key for an element the
# table does not carry -- so what this pins is the READER, and it is owned by this repo.
if start_stub "$WORK/state.bin" "$WORK/elements.txt" --element-attributes; then
  attrs="$(sv --attach "127.0.0.1:$PORT" --ext-attrs 2>&1 | norm \
           | sed -n '/per-element attributes/,$p')"
  if [ -z "$attrs" ]; then fail "relation.attach.attrs" "no attribute listing in the output"
  else golden relation.attach.attrs "$(printf '%s' "$attrs" | md5sum | cut -d' ' -f1)"; fi
  stop_stub
else fail "relation.attach.attrs" "stub did not come up"; fi

# The same registry, drawn rather than listed. Three claims in one stub run, because starting
# it is the expensive part:
#
#   the attribute the stub holds two REACHABLE values for draws, and its range is those two;
#   the attribute it holds zero values for is refused rather than drawn as backdrop;
#   the value keyed to an element the table has no row for is REPORTED and not silently lost.
#
# That last one is the state this view can be wrong about while looking entirely right, and
# the stub is the only place in this repo where it exists -- the sim's own registry keys
# everything to elements the corpus carries.
# The stub serves sim.molecular_mass for oxygen (31.999) and hydrogen (2.016) only, so the range
# the attached view reports is the span of whichever of those two this world actually holds --
# both on the reference world. Read off the same tick-0 state the stub serves, by element hash
# in cells with mass; a lone value is widened by one, as relation.attr.consistent explains.
has_gas="$(awk '$5 == -1528777920 {o = 1} $5 == -1046145888 {h = 1}
               END {print (o ? "O" : "") (h ? "H" : "")}' "$WORK/probe.txt")"
case "$has_gas" in
  OH) ATTR_RANGE="2.016 .. 31.999" ;;
  O)  ATTR_RANGE="31.999 .. 32.999" ;;
  H)  ATTR_RANGE="2.016 .. 3.016" ;;
  *)  ATTR_RANGE="" ;;
esac
if start_stub "$WORK/state.bin" "$WORK/elements.txt" --element-attributes; then
  bad=""
  out="$(sv --attach "127.0.0.1:$PORT" --view attr:sim.molecular_mass --stats 2>&1 | norm)"
  # A world with neither gas has no range to check; the three sub-checks after it still run.
  if [ -n "$ATTR_RANGE" ]; then
    printf '%s' "$out" | grep -qF "$ATTR_RANGE" || bad="$bad range(want $ATTR_RANGE)"
  fi
  printf '%s' "$out" | grep -qi "no row for" || bad="$bad off-table-note"
  sv --attach "127.0.0.1:$PORT" --view attr:mod9.cleared >"$WORK/cleared.log" 2>&1
  [ $? = 0 ] && bad="$bad accepted-empty"
  grep -q "0 values, 7 writes" "$WORK/cleared.log" || bad="$bad empty-reason"
  if [ -z "$bad" ]; then pass "relation.attach.attrview"
  else fail "relation.attach.attrview" "wrong or missing:$bad"; fi
  stop_stub
else fail "relation.attach.attrview" "stub did not come up"; fi

# A stub started with no --extcells has no /extcells route at all, which is what an older
# oni-framework-api looks like. "The route is missing" and "the registry is empty" are different
# answers and the client must not collapse them.
if start_stub "$WORK/state.bin" "$WORK/elements.txt"; then
  out="$(sv --attach "127.0.0.1:$PORT" --ext-list 2>&1)"
  check_contains "relation.attach.noext" "$out" "404"
  stop_stub
else fail "relation.attach.noext" "stub did not come up"; fi

# ------------------------------------------------------- refusals that need no server
out="$(sv --attach "127.0.0.1:$PORT" --ticks 5 2>&1)"
check_contains "relation.refuse.drive" "$out" "--ticks"

out="$(sv --corpus "$CORPUS" --attach "127.0.0.1:$PORT" 2>&1)"
check_contains "relation.refuse.source" "$out" "only one can be live"

# A SimDLL with no extension surface must REFUSE --seed rather than report a pin it did not
# make. The game's own SimDLL.dll is the DLL that proves it, and it is the one file
# guaranteed to lack the surface; point SIMVIZ_STOCK_DLL at it to run this arm. Before this arm existed, `--seed 7` against it printed
# "pinned random stream to 0x00000007" and the run diverged from itself anyway.
STOCK="${SIMVIZ_STOCK_DLL:-}"
if [ -f "$STOCK" ]; then
  out="$(sv --corpus "$CORPUS" --dll "$STOCK" --seed 7 --ticks 1 2>&1)"
  if printf '%s' "$out" | grep -q "could not pin the random stream"; then
    pass "relation.refuse.seed"
  elif printf '%s' "$out" | grep -q "pinned random stream"; then
    fail "relation.refuse.seed" "a DLL with no kSetRandomState reported a pinned stream"
  else
    fail "relation.refuse.seed" "neither a pin nor a refusal: $(printf '%s' "$out" | tail -2)"
  fi
else
  skip "relation.refuse.seed" "set SIMVIZ_STOCK_DLL to a copy of the game's own SimDLL.dll to run this arm"
fi

# ------------------------------------------------------- the divergence tools
#
# A run does not diverge from itself, and a planted difference is found. Both halves matter:
# the first is what makes a report of "no divergence" mean something, and without it a
# bisect that always answers "tick 0" would pass the second.
run_off --seed $SEED --ticks 60 --digest-log "$WORK/a.log" --dump-state "$WORK/a.bin" >/dev/null 2>&1
run_off --seed $SEED --ticks 60 --digest-log "$WORK/a2.log" >/dev/null 2>&1
run_off --seed $SEED --ticks 60 --ext-probe "$PROBE_RECT" --ext-probe-jk 10000 \
        --digest-log "$WORK/b.log" --dump-state "$WORK/b.bin" >/dev/null 2>&1

same="$(sv --bisect "$WORK/a.log" "$WORK/a2.log" 2>&1 | norm)"
if printf '%s' "$same" | grep -qiE "identical|no diver|never part|agree"; then
  pass "relation.bisect.same"
else
  fail "relation.bisect.same" "a run diverged from itself: $(printf '%s' "$same" | tail -3)"
fi

differ="$(sv --bisect "$WORK/a.log" "$WORK/b.log" 2>&1 | norm)"
if printf '%s' "$differ" | grep -qiE "tick [0-9]+" && \
   printf '%s' "$differ" | grep -qiE "temperature|extCells|mass|elementIdx"; then
  pass "relation.bisect.differ"
else
  fail "relation.bisect.differ" "a planted divergence was not located: $(printf '%s' "$differ" | tail -5)"
fi

# ---------------------------------------------------------------------------- PINNED
say ""
if [ "$INPUTS_MATCH" = 1 ]; then
  say "PINNED -- numbers this SimDLL and this corpus produce"
else
  say "PINNED -- skipped, the inputs are not the ones these were recorded from"
fi

full="$(run_off --seed $SEED --ticks $TICKS --digest --view temperature --stats)"

digest_block="$(printf '%s\n' "$full" | sed -n '/^digest after/,/^  total mass/p' | grep -v '^  total mass')"
pinned pinned.digest "$(printf '%s' "$digest_block" | md5sum | cut -d' ' -f1)"

# tr: a Windows build ends every line CRLF, and a golden must not depend on whether the
# checkout kept the CR.
pinned pinned.mass "$(printf '%s\n' "$full" | sed -n 's/^  total mass *//p' | tr -d '\r')"

quantiles="$(printf '%s\n' "$full" | sed -n '/^temperature over/,$p' | grep -E '^  p[0-9]')"
if [ -z "$quantiles" ]; then
  fail "pinned.stats" "no quantile block in the output"
else
  pinned pinned.stats "$(printf '%s' "$quantiles" | md5sum | cut -d' ' -f1)"
fi

pinned pinned.elements "$(md5 "$WORK/elements.txt")"

extlist="$(run_off --seed $SEED --ext-list | sed -n '/per-cell extension properties/,$p')"
pinned pinned.extlist "$(printf '%s' "$extlist" | md5sum | cut -d' ' -f1)"

extattrs="$(run_off --seed $SEED --ext-attrs | sed -n '/per-element attributes/,$p')"
pinned pinned.extattrs "$(printf '%s' "$extattrs" | md5sum | cut -d' ' -f1)"

# The attr render itself. PINNED and not owned: what it draws is the sim's registry over the
# corpus's element map, so both inputs are the sim's and a change to either legitimately
# moves it. `relation.attr.perelement` above is the arm that keeps its teeth when this skips.
pinned pinned.attrview "$(md5 "$WORK/attr.png")"

# ---------------------------------------------------------------------------- report
say ""
if [ "$RECORD" = 1 ]; then
  {
    echo "# oni-sim-visualizer check goldens -- written by ./test.sh --record"
    echo "#"
    echo "# OWNED and RELATION goldens are this repo's own output and a change to one is a"
    echo "# change somebody made here. PINNED goldens are the SIMULATION's numbers: they are"
    echo "# valid only for the sim source and corpus recorded below, and test.sh skips them"
    echo "# rather than failing when either has moved."
    echo "#"
    echo "# input.sim.src is the git tree hash of oni-sim-replacement sim/ at the commit the DLL under"
    echo "# test was stamped with -- NOT the md5 of the DLL, which moves on every rebuild of"
    echo "# unchanged source and so skipped these arms permanently."
    echo "#"
    echo "# recorded $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    for k in "${NEWORDER[@]}"; do
      printf '%s %s\n' "$k" "${NEWGOLD[$k]}"
    done
  } > "$GOLDENS"
  say "wrote $GOLDENS ($(grep -vc '^#' "$GOLDENS") goldens)"
fi

say "done: $npass PASS  $nfail FAIL  $nskip SKIP"
if [ "$nfail" -gt 0 ]; then
  say ""
  say "failed arms:"
  for f in "${FAILED[@]}"; do say "  $f"; done
fi
[ "$nfail" -gt 99 ] && exit 99
exit "$nfail"
