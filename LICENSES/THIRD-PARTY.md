# Third-party components shipped with simviz.exe

The visualizer's own source is covered by the repository licence. The files below are not
ours; they ship beside `simviz.exe` because the program loads them at run time.

## Qt 6.8.3 (LGPLv3)

`Qt6Core.dll`, `Qt6Gui.dll`, `Qt6Widgets.dll`, `platforms/qwindows.dll`,
`platforms/qoffscreen.dll`.

- Licence: GNU Lesser General Public License v3 (`LGPL-3.0.txt`), which incorporates the
  GNU General Public License v3 (`GPL-3.0.txt`).
- These are the unmodified official binaries of The Qt Company's `win64_mingw` package for
  Qt 6.8.3. The corresponding source is published by the Qt Project on
  https://download.qt.io (the Qt 6.8.3 source archives).
- Qt is linked dynamically. To use your own build of Qt, replace these DLLs with a
  binary-compatible Qt 6.8 build for MinGW x64; to relink the program itself against a
  different Qt, build it from this repository's source with `build.sh`, setting `ONI_QT`
  to your Qt prefix.

## MinGW-w64 runtime

`libgcc_s_seh-1.dll`, `libstdc++-6.dll` (GCC): GPLv3 with the GCC Runtime Library
Exception 3.1 (`GPL-3.0.txt`, `GCC-exception-3.1.txt`). The exception is what allows these
runtime libraries to ship with a program that is not itself under the GPL.

`libwinpthread-1.dll` (mingw-w64 winpthreads): expat (MIT) and BSD-style terms, reproduced
in `winpthreads.txt`.

## DejaVu fonts (Bitstream Vera licence)

`assets/fonts/DejaVuSans.ttf`, `DejaVuSans-Bold.ttf`, `DejaVuSansMono.ttf`,
`DejaVuSansMono-Bold.ttf`, compiled into `simviz.exe` so every machine draws the same text.
Bitstream Vera licence; DejaVu's changes are public domain. Full terms in `DejaVu-fonts.txt`.
