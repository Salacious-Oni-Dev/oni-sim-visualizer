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

# Qt 6 (LGPLv3, linked dynamically) is the UI toolkit (see the README). The prefix is the
# `win64_mingw` binary package aqtinstall fetches:
#
#   python3 -m venv ~/qt/venv && ~/qt/venv/bin/pip install aqtinstall
#   ~/qt/venv/bin/aqt install-qt windows desktop 6.8.3 win64_mingw -O ~/qt
#
# Qt builds that package with MinGW 13.1 posix-seh, so this compiles with the POSIX-threads
# mingw (GCC 13), not the win32-threads default: the two do not agree on std::thread/mutex
# and Qt's headers use both. No moc and no host Qt tools: signals are wired with functor
# connects only, so there is no Q_OBJECT anywhere and this stays one compiler line.
QT_VERSION=6.8.3
ONI_QT="${ONI_QT:-$HOME/qt/$QT_VERSION/mingw_64}"
if [ ! -f "$ONI_QT/include/QtWidgets/QApplication" ]; then
  echo "cannot find Qt $QT_VERSION (win64_mingw) at '$ONI_QT'." >&2
  echo "install it as the comment at the top of this section says, or set ONI_QT=/path/to/mingw_64" >&2
  exit 1
fi
CXX=x86_64-w64-mingw32-g++-posix

# -O2 matches driver/'s optimisation intent. This binary never times the sim's kernels
# -- bench.exe does that, with sim/build.sh's flags -- so the -O3/-march tier those two
# share is not needed here and is deliberately not copied.
#
# Our own code still links libgcc/libstdc++ statically, as before. The Qt DLLs carry their
# own copies (shipped beside the exe below), and nothing crosses that boundary that owns
# memory from the other side: Qt's API is QString/QImage, never a std container.
FLAGS=(-O2 -std=c++17 -static-libgcc -static-libstdc++
       -Wall -Wextra -Wno-cast-function-type
       -I"$ONI_SIM_REPO/driver/src"
       -I"$ONI_SIM_REPO"
       -isystem "$ONI_QT/include" -isystem "$ONI_QT/include/QtCore"
       -isystem "$ONI_QT/include/QtGui" -isystem "$ONI_QT/include/QtWidgets")

# -lws2_32 is live attach (attach.h): a socket to oni-framework-api's bulk routes, a system
# library. Qt is the one third-party dependency; it brings its own window, drawing and file
# dialogs, which is why gdi32 and comdlg32 are no longer named here.
$CXX "${FLAGS[@]}" -o build/simviz.exe src/main.cpp \
    -L"$ONI_QT/lib" -lQt6Widgets -lQt6Gui -lQt6Core \
    -lws2_32 -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic

# The runtime the exe needs beside it: the three Qt modules, the MinGW runtime Qt itself
# links against, and the two platform plugins (windows for the window, offscreen for the
# goldens and for CI, where there is no display). Copied, not linked, so build/ is exactly
# what a release zip holds.
mkdir -p build/platforms
for f in Qt6Core.dll Qt6Gui.dll Qt6Widgets.dll libgcc_s_seh-1.dll libstdc++-6.dll \
         libwinpthread-1.dll; do
  cp -u "$ONI_QT/bin/$f" build/
done
cp -u "$ONI_QT/plugins/platforms/qwindows.dll" "$ONI_QT/plugins/platforms/qoffscreen.dll" \
      build/platforms/

echo "built build/simviz.exe (Qt $QT_VERSION)"
