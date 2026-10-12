// The text half of the control panels: the command line that reproduces a session, and the
// command each experiment runs. The panels themselves are Qt docks (qt/docks.h); this file has
// no drawing in it, so the CLI and the docks build those two strings the same way.
//
// THE COMMAND LINE IS PART OF THE UI, not a debug aid. Every control maps to exactly one flag,
// and the help dock shows the invocation that would reproduce the current session. That is the
// answer to "how do I script this", it teaches the flag vocabulary while somebody clicks, and
// it is a standing check on the thing this work is for: a flag with no control is a flag this
// string can name and the panels cannot set.

#pragma once

#include <deque>
#include <string>
#include <vector>

#include "frame.h"
#include "help.h"
#include "launch.h"
#include "out.h"
#include "session.h"
#include "views.h"

namespace simviz {
namespace panels {

// How the scalar range is chosen. Mirrors --range: the default scan, `full`, or a pinned
// pair. Kept as a mode rather than inferred from the numbers, because "the scan happened to
// return 0..100" and "0..100 was pinned" behave differently on the next view change.
enum class RangeMode { Scan = 0, Full = 1, Fixed = 2 };

// What the command line is built from. Pointers for the settings, as before, so a caller
// fills it from its own live state rather than from a copy that could go stale.
struct Bindings {
  View* view = nullptr;
  bool* mass_fade = nullptr;
  RangeMode* range_mode = nullptr;
  double* range_lo = nullptr;
  double* range_hi = nullptr;
  int* poll_ms = nullptr;
  double* dt = nullptr;

  bool attached = false;
  bool timeline_on = false;
  int tick = 0;
  std::string source;        // corpus path or host:port
  std::string dll;
  bool seed_pinned = false;
  uint32_t seed = 0;
  int width = 0, height = 0;  // the WORLD's size: --size
  float ui_scale = 1.0f;
  bool fullscreen = false;
  bool large_font = false;  // --font large
  // Where the camera is, and whether it is still where opening the world put it -- compared
  // against a fresh FitTo, not tracked by a flag every camera move would have to clear.
  const Camera* cam = nullptr;
  bool cam_is_fit = true;
  // The cell under the pointer: --hover takes the same coordinates.
  bool hover_valid = false;
  int hover_cell_x = 0, hover_cell_y = 0;

  bool ext_live = false;
  bool ext_published = false;
  std::string ext_props;
  std::string ext_view_name;
  std::string attr_view_name;
  std::string event_streams;
};

// The parameters of the one-shot actions. They live with the panel rather than in Bindings
// because nothing on the session side owns them -- a probe rectangle is not sim state -- and
// because a text field's identity is the ADDRESS of the string it edits, so it has to be a
// string that stays put between frames.
//
// Every one is spelled the way its flag is spelled. `x,y,w,h` is a text field rather than four
// number boxes for that reason: it is one row instead of four, and what you type is what you
// would pass, which is the parity claim doing a second job as documentation.
struct Tools {
  std::string probe_rect = "0,0";
  std::string dump_state_path = "state.txt";
  std::string ext_blob_path = "extcells.bin";
  std::string inject_rect;
  double inject_fraction = 0.5;
  std::string ext_probe_rect;
  double ext_probe_jk = 50000.0;
};

// Which experiment the tests tab is on. The order is the order of the dropdown.
enum class Test {
  Checkpoint = 0,   // --checkpoint-test
  TimelineSeek,     // --timeline-test
  TimelineDiff,     // --timeline-diff
  LoadStress,       // --load-stress
  DigestLog,        // --digest-log
  Bisect,           // --bisect
  DiffState,        // --diff-state
};

struct Tests {
  int which = 0;
  std::string corpus;          // filled from the session the first time the tab is drawn
  std::string dll;
  std::string seed;
  int span = 40;
  int ticks = 200;
  int interval = 10;
  int keep = 8;
  int loads = 5;
  int ticks_between = 1;
  bool bare_load = false;
  bool replay_registrations = false;
  double dt = 0.2;
  std::string path_a;
  std::string path_b;
  std::string out_path = "digest.txt";
  std::string diff_view;
  std::string snapshot = "diff.png";
  bool seeded = false;         // corpus/dll copied out of the session once
};

// ---------------------------------------------------------------------------------------
// The command line that would reproduce what is on screen. Built from the bindings rather
// than from the original argv, so it reflects every control touched since.
// ---------------------------------------------------------------------------------------
inline std::string CommandLine(const Bindings& b) {
  std::string c = "simviz.exe";
  if (b.attached) {
    c += " --attach " + b.source;
    c += " --poll " + std::to_string(*b.poll_ms);
  } else {
    c += " --corpus " + b.source;
    if (!b.dll.empty()) c += " --dll " + b.dll;
    if (b.seed_pinned) c += " --seed " + std::to_string(b.seed);
    if (b.tick > 0) c += " --ticks " + std::to_string(b.tick);
    char dt[32];
    snprintf(dt, sizeof(dt), "%g", *b.dt);
    if (*b.dt != 0.2) c += std::string(" --dt ") + dt;
    if (!b.timeline_on) c += " --no-timeline";
  }

  c += " --view ";
  if (*b.view == View::Ext) {
    c += "ext:" + (b.ext_view_name.empty() ? std::string("<name>") : b.ext_view_name);
  } else if (*b.view == View::Attr) {
    c += "attr:" + (b.attr_view_name.empty() ? std::string("<name>") : b.attr_view_name);
  } else {
    c += ViewName(*b.view);
  }

  switch (*b.range_mode) {
    case RangeMode::Scan: break;
    case RangeMode::Full: c += " --range full"; break;
    case RangeMode::Fixed: {
      char r[64];
      snprintf(r, sizeof(r), " --range %g,%g", *b.range_lo, *b.range_hi);
      c += r;
      break;
    }
  }
  if (!*b.mass_fade) c += " --no-mass-fade";

  if (b.attached) {
    if (b.ext_published) {
      c += " --ext-published";
      if (!b.ext_props.empty()) c += "=" + b.ext_props;
    } else if (b.ext_live) {
      c += " --ext";
    }
    if (!b.event_streams.empty()) c += " --events=" + b.event_streams;
  }

  char size[48];
  snprintf(size, sizeof(size), " --size %dx%d", b.width, b.height);
  c += size;
  if (b.ui_scale > 1.0f) {
    char sc[32];
    snprintf(sc, sizeof(sc), " --ui-scale %g", b.ui_scale);
    c += sc;
  }
  if (b.fullscreen) c += " --fullscreen";
  if (b.large_font) c += " --font large";

  // THE CAMERA, and it is the difference between a line that reproduces the screen and one
  // that reproduces the window. Only when it has been moved: at the fit-to-window default
  // every run of this command lands there anyway, and a `--camera` on every line would be
  // noise on the common case. `--hover` goes with it because the readout box is part of what
  // is on screen, and without the cell it is drawn over nothing would put it back.
  if (b.cam != nullptr && !b.cam_is_fit) c += CameraFlag(*b.cam);
  if (b.hover_valid) {
    char h[64];
    snprintf(h, sizeof(h), " --hover %d,%d", b.hover_cell_x, b.hover_cell_y);
    c += h;
  }

  c += " --window";
  return c;
}

// ---------------------------------------------------------------------------------------
// Tab 5 -- TESTS. The experiments, run as a separate process.
//
// See child.h for why they are not simply called: each one destroys the session it runs in,
// they take seconds to minutes, and --load-stress is a crash repro by design. The command
// shown here is not a description of the run -- it IS the run, handed to CreateProcess.
// ---------------------------------------------------------------------------------------
inline std::string Quoted(const std::string& s) {
  return s.find(' ') == std::string::npos ? s : "\"" + s + "\"";
}

inline std::string TestCommand(const Tests& t) {
  const std::string corpus = Quoted(t.corpus.empty() ? std::string("<corpus.bin>") : t.corpus);
  const std::string a = Quoted(t.path_a.empty() ? std::string("<a>") : t.path_a);
  const std::string bb = Quoted(t.path_b.empty() ? std::string("<b>") : t.path_b);
  const Test which = static_cast<Test>(t.which);

  std::string c;
  switch (which) {
    case Test::Checkpoint:
      c = "--corpus " + corpus + " --checkpoint-test " + std::to_string(t.span);
      break;
    case Test::TimelineSeek:
      c = "--corpus " + corpus + " --timeline-test " + std::to_string(t.span) +
          " --timeline-interval " + std::to_string(t.interval) + " --timeline-keep " +
          std::to_string(t.keep);
      break;
    case Test::TimelineDiff:
      c = "--corpus " + corpus + " --timeline-diff " + std::to_string(t.span) +
          " --timeline-interval " + std::to_string(t.interval) + " --timeline-keep " +
          std::to_string(t.keep);
      break;
    case Test::LoadStress:
      c = "--corpus " + corpus + " --load-stress " + std::to_string(t.loads) +
          " --load-stress-ticks " + std::to_string(t.ticks_between);
      if (t.bare_load) c += " --bare-load";
      if (t.replay_registrations) c += " --replay-registrations";
      break;
    case Test::DigestLog:
      c = "--corpus " + corpus + " --ticks " + std::to_string(t.ticks) + " --digest-log " +
          Quoted(t.out_path);
      break;
    case Test::Bisect:
      return "--bisect " + a + " " + bb;
    case Test::DiffState:
      c = "--diff-state " + a + " " + bb;
      if (!t.diff_view.empty()) c += " --diff-view " + t.diff_view;
      if (!t.snapshot.empty()) c += " --snapshot " + Quoted(t.snapshot);
      return c;
  }
  // Everything above this point drives a SimDLL over a corpus, so all three of these apply.
  if (!t.dll.empty()) c += " --dll " + Quoted(t.dll);
  if (!t.seed.empty()) c += " --seed " + t.seed;
  if (t.dt != 0.2) {
    char dt[32];
    snprintf(dt, sizeof(dt), " --dt %g", t.dt);
    c += dt;
  }
  return c;
}

inline bool TestReady(const Tests& t) {
  switch (static_cast<Test>(t.which)) {
    case Test::Bisect:
    case Test::DiffState:
      return !t.path_a.empty() && !t.path_b.empty();
    default:
      return !t.corpus.empty();
  }
}

}  // namespace panels
}  // namespace simviz
