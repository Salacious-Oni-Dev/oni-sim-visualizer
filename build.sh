#!/usr/bin/env bash
# Cross-compile the visualizer to Windows x64, same convention as
# oni-sim-replacement/driver/build.sh: explicit flags, no generator. It runs directly from
# WSL via binfmt interop, so no Windows shell is needed.
#
# The ABI header and the offline host plumbing are NOT vendored -- they belong to
# oni-sim-replacement and vendoring them would let this repo drift silently against the DLL
# it drives. They are reached on the include path instead. The default assumes both
# repositories are cloned side by side; override with ONI_SIM_REPO.
set -euo pipefail

cd "$(dirname "$0")"
mkdir -p build

ONI_SIM_REPO="${ONI_SIM_REPO:-../oni-sim-replacement}"
if [ ! -f "$ONI_SIM_REPO/driver/src/simhost.h" ]; then
  echo "cannot find oni-sim-replacement at '$ONI_SIM_REPO'." >&2
  echo "clone it beside this repo, or set ONI_SIM_REPO=/path/to/oni-sim-replacement" >&2
  exit 1
fi
# abi/sim_abi.h is generated from the game's own assemblies by oni-sim-replacement's first
# build, so that build has to have run once before this one can.
if [ ! -f "$ONI_SIM_REPO/abi/sim_abi.h" ]; then
  echo "'$ONI_SIM_REPO/abi/sim_abi.h' is missing: build oni-sim-replacement first" >&2
  echo "(ONI_GAME=/path/to/OxygenNotIncluded ./sim/build.sh in that repo)" >&2
  exit 1
fi

# -O2 matches driver/'s optimisation intent. This binary never times the sim's kernels
# -- bench.exe does that, with sim/build.sh's flags -- so the -O3/-march tier those two
# share is not needed here and is deliberately not copied.
FLAGS=(-O2 -std=c++17 -static-libgcc -static-libstdc++
       -Wall -Wextra -Wno-cast-function-type
       -I"$ONI_SIM_REPO/driver/src"
       -I"$ONI_SIM_REPO")

# -lws2_32 is live attach (attach.h): a socket to oni-framework-api's bulk routes. -lcomdlg32
# is the start screen's file picker (launch.h), and only that. Both are system libraries on
# every Windows this runs on, so they are link flags, not dependencies -- the "no third-party
# library" property of this build is unchanged.
x86_64-w64-mingw32-g++ "${FLAGS[@]}" -o build/simviz.exe src/main.cpp -lgdi32 -lcomdlg32 -lws2_32

echo "built build/simviz.exe"
