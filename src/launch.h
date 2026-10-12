// The front door: the screen this program shows when it is run with no arguments.
//
// WHY THIS EXISTS. Until now `simviz.exe` with nothing after it printed usage to a console
// and exited, so the only way in was a command line -- which meant the flag vocabulary had to
// be learned before the program could be seen at all. That is backwards for a debugger whose
// whole point is looking at something. This screen asks for the one thing the program cannot
// guess (which simulation to look at), offers every setting that has to be decided BEFORE the
// world exists, and starts.
//
// WHAT BELONGS HERE, AND WHAT DOES NOT. Only settings that must be fixed before a session is
// opened: the source, the DLL, the random seed, the ticks to run first, the window size. A
// setting that can be changed while the world is on screen belongs in panels.h instead, and
// is deliberately absent here even when it has a flag -- one control per setting, and the
// live one wins. The two exceptions are the view and the fade, which appear in both: they are
// cheap to set up front and they decide what the first frame looks like.
//
// THE COMMAND LINE IS SHOWN, for the same reason panels.h shows it: this screen is a way to
// learn the flags, not a way to avoid them. The line it prints is the line that reproduces
// the session it is about to start.
//
// This file is the screen's DATA -- the Spec, the command it is equivalent to, and the
// file-exists test. The screen itself is qt/launcher.h.

#pragma once

#include <algorithm>

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "out.h"
#include "views.h"

namespace simviz {
namespace launch {

// Everything a session needs before it can exist. One field per flag, and the names match the
// flags rather than the internals, because this struct is also what the command-line preview
// is built from -- a field with no flag would be a setting the preview could not express.
struct Spec {
  bool attach = false;          // --attach instead of --corpus
  std::string corpus;           // --corpus <path>
  std::string dll;              // --dll <path>
  std::string endpoint;         // --attach host:port
  int poll_ms = 200;            // --poll
  int attach_timeout_ms = 5000; // --attach-timeout
  std::string seed;             // --seed (text: the flag takes base 0, so 0xdeadbeef counts)
  int ticks = 0;                // --ticks
  double dt = 0.2;              // --dt
  bool timeline = true;         // --no-timeline when false
  bool mass_fade = true;        // --no-mass-fade when false
  View view = View::Element;    // --view
  std::string ext_name;         // --view ext:<name>
  int ext_comp = 0;             // --view ext:<name>[<comp>]
  std::string attr_name;        // --view attr:<name>
  int attr_comp = 0;            // --view attr:<name>[<comp>]
  bool ext_live = false;        // --ext
  bool ext_published = false;   // --ext-published
  std::string ext_props;        // --ext-published=<list>
  std::string events;           // --events=<list>
  int width = 1100;             // --size WxH
  float ui_scale = 1.0f;        // --ui-scale N
  bool large_font = false;      // --font large: picked by itself on a screen 1440 px tall or more
  int height = 760;
};

// Set only by the UI samples. Whether a path exists is a fact about the machine, and the
// start screen draws it (the corpus line, the DLL warning, the start button), so a sample
// that asked the disk would be an OWNED golden that changed with the checkout's
// surroundings: measured, a worktree with no sibling sim repo drew a different
// ui-launch-corpus.png from the same commit. The samples draw the ready screen everywhere.
inline bool g_sample_assume_files_exist = false;

inline bool FileExists(const std::string& path) {
  if (path.empty()) return false;
  if (g_sample_assume_files_exist) return true;
  const DWORD a = GetFileAttributesA(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// The invocation this spec is equivalent to. Same contract as panels::CommandLine: every
// control on the screen is one flag, so a setting this cannot name is a setting with no flag.
inline std::string Command(const Spec& s) {
  std::string c = "simviz.exe";
  if (s.attach) {
    c += " --attach " + (s.endpoint.empty() ? std::string("<host:port>") : s.endpoint);
    if (s.poll_ms != 200) c += " --poll " + std::to_string(s.poll_ms);
    if (s.attach_timeout_ms != 5000)
      c += " --attach-timeout " + std::to_string(s.attach_timeout_ms);
    if (s.ext_published) {
      c += " --ext-published";
      if (!s.ext_props.empty()) c += "=" + s.ext_props;
    } else if (s.ext_live) {
      c += " --ext";
    }
    if (!s.events.empty()) c += " --events=" + s.events;
  } else {
    c += " --corpus " + (s.corpus.empty() ? std::string("<corpus.bin>") : s.corpus);
    if (!s.dll.empty()) c += " --dll " + s.dll;
    if (!s.seed.empty()) c += " --seed " + s.seed;
    if (s.ticks > 0) c += " --ticks " + std::to_string(s.ticks);
    if (s.dt != 0.2) {
      char dt[32];
      snprintf(dt, sizeof(dt), " --dt %g", s.dt);
      c += dt;
    }
    if (!s.timeline) c += " --no-timeline";
  }

  c += " --view ";
  if (s.view == View::Ext) {
    c += "ext:" + (s.ext_name.empty() ? std::string("<property>") : s.ext_name);
    if (s.ext_comp != 0) c += "[" + std::to_string(s.ext_comp) + "]";
  } else if (s.view == View::Attr) {
    c += "attr:" + (s.attr_name.empty() ? std::string("<attribute>") : s.attr_name);
    if (s.attr_comp != 0) c += "[" + std::to_string(s.attr_comp) + "]";
  } else {
    c += ViewName(s.view);
  }
  if (!s.mass_fade) c += " --no-mass-fade";

  char size[48];
  snprintf(size, sizeof(size), " --size %dx%d", s.width, s.height);
  c += size;
  if (s.ui_scale > 1.0f) {
    char sc[32];
    snprintf(sc, sizeof(sc), " --ui-scale %g", s.ui_scale);
    c += sc;
  }
  if (s.large_font) c += " --font large";
  c += " --window";
  return c;
}

}  // namespace launch
}  // namespace simviz
