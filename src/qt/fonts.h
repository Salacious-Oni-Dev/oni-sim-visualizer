// The four DejaVu faces this program draws every word with, compiled into the exe.
//
// WHY BUNDLED. The goldens are pictures, and a picture with text in it is only reproducible if
// every host draws that text from the same font file through the same rasterizer. A system font
// is a different file on every Windows (and absent under wine), so the font ships inside the
// exe and Qt is told to use it and nothing else -- see qtapp.h for the FreeType half.
//
// WHY .incbin. It puts the file's bytes straight into the object with no generated source and
// no resource compiler, so build.sh stays one compiler line and needs no host Qt tools. The
// paths are relative to the directory the compiler runs in, which build.sh makes the repo root.
//
// Licence: Bitstream Vera (DejaVu changes are public domain) -- LICENSES/DejaVu-fonts.txt.

#pragma once

#include <QByteArray>
#include <QFontDatabase>

#include "../out.h"

#define SIMVIZ_INCBIN(sym, path)                                   \
  __asm__(".section .rdata,\"dr\"\n"                               \
          ".balign 16\n"                                           \
          ".globl " #sym "_start\n" #sym "_start:\n"               \
          ".incbin \"" path "\"\n"                                 \
          ".globl " #sym "_end\n" #sym "_end:\n"                   \
          ".byte 0\n"                                              \
          ".text\n");                                              \
  extern "C" const char sym##_start[];                             \
  extern "C" const char sym##_end[];

SIMVIZ_INCBIN(simviz_font_sans, "assets/fonts/DejaVuSans.ttf")
SIMVIZ_INCBIN(simviz_font_sans_bold, "assets/fonts/DejaVuSans-Bold.ttf")
SIMVIZ_INCBIN(simviz_font_mono, "assets/fonts/DejaVuSansMono.ttf")
SIMVIZ_INCBIN(simviz_font_mono_bold, "assets/fonts/DejaVuSansMono-Bold.ttf")

namespace simviz {
namespace qtfonts {

// The family names the faces register under. Used by name everywhere so a typo is one place.
inline const char* Sans() { return "DejaVu Sans"; }
inline const char* Mono() { return "DejaVu Sans Mono"; }

// Registers all four. Returns false, and says which, if Qt refused one -- a missing face would
// otherwise fall back to a system font silently, which is exactly the drift this file prevents.
inline bool Register() {
  struct Face {
    const char* name;
    const char* start;
    const char* end;
  };
  const Face faces[] = {
      {"DejaVuSans", simviz_font_sans_start, simviz_font_sans_end},
      {"DejaVuSans-Bold", simviz_font_sans_bold_start, simviz_font_sans_bold_end},
      {"DejaVuSansMono", simviz_font_mono_start, simviz_font_mono_end},
      {"DejaVuSansMono-Bold", simviz_font_mono_bold_start, simviz_font_mono_bold_end},
  };
  bool ok = true;
  for (const Face& f : faces) {
    const QByteArray bytes = QByteArray::fromRawData(f.start, static_cast<int>(f.end - f.start));
    if (QFontDatabase::addApplicationFontFromData(bytes) < 0) {
      out::Printf("qt: could not register the bundled font %s\n", f.name);
      ok = false;
    }
  }
  return ok;
}

}  // namespace qtfonts
}  // namespace simviz
