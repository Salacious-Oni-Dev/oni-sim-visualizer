// The control panels: every runtime knob this program has, as something you can see.
//
// WHAT THIS IS FOR. Until now a person could change eight things while the window was open
// -- the view, mass fade, the range rescan, fit, play/pause, the step keys, the scrub bar and
// the event pane -- and every one of them was an unlabelled keystroke they had to already
// know. Everything else was fixed in argv before the window existed. These panels put the
// runtime knobs on screen with their names, and the output pane gives the printing features
// somewhere to print.
//
// THE PANELS ARE DRAWN LAST AND READ FIRST, which is the one piece of ordering worth stating.
// An immediate-mode control handles its input and paints in the same pass, and that pass runs
// at the END of the frame, after the camera has already decided what this frame's drag meant.
// So the camera asks `WantsMouse` using the panel rectangles from the PREVIOUS frame. At 250
// fps a panel that just appeared can swallow one frame of pan; the alternative -- laying the
// panels out twice per frame so the rects are current -- costs more than the artifact it
// removes. It is written down here because "the click went to the world" is otherwise a
// mystery rather than a known one-frame edge.
//
// THE COMMAND LINE IS PART OF THE UI, not a debug aid. Every control here maps to exactly one
// flag, and the panel shows the invocation that would reproduce the current session. That is
// the answer to "how do I script this", it teaches the flag vocabulary while somebody clicks,
// and it is a standing check on the thing this work is for: a flag with no control is a flag
// this string can name and the panels cannot set.

#pragma once

#include <deque>
#include <string>
#include <vector>

#include "frame.h"
#include "help.h"
#include "launch.h"
#include "out.h"
#include "session.h"
#include "ui.h"
#include "views.h"

namespace simviz {
namespace panels {

// How the scalar range is chosen. Mirrors --range: the default scan, `full`, or a pinned
// pair. Kept as a mode rather than inferred from the numbers, because "the scan happened to
// return 0..100" and "0..100 was pinned" behave differently on the next view change.
enum class RangeMode { Scan = 0, Full = 1, Fixed = 2 };

// The column and the console pane are sized in characters and lines of the font in use, so
// --font large keeps every one-line label inside the column and the same number of lines in
// the pane: 306 and 168 at 5x8, 357 and 252 at 7x13.
inline int ColumnW() { return 306 * font::kAdvance / 6; }
inline int ConsoleH() { return 168 * font::kLineH / 10; }

// Everything the panels read or write. Pointers into the loop's own locals rather than copies,
// because a control that edits a copy is a control that silently does nothing.
struct Bindings {
  // --- live state, written in place
  View* view = nullptr;
  bool* playing = nullptr;
  bool* mass_fade = nullptr;
  bool* show_hud = nullptr;
  bool* show_bar = nullptr;
  bool* show_events = nullptr;
  RangeMode* range_mode = nullptr;
  double* range_lo = nullptr;
  double* range_hi = nullptr;
  int* poll_ms = nullptr;
  double* dt = nullptr;

  // --- read-only context
  Session* session = nullptr;
  const Range* range = nullptr;
  bool attached = false;
  bool timeline_on = false;
  bool fetch_pinned = false;
  int tick = 0;
  int base = 0, head = 0;
  float fps = 0.0f;
  std::string source;        // corpus path or host:port
  std::string dll;
  bool seed_pinned = false;
  uint32_t seed = 0;
  int width = 0, height = 0;
  float ui_scale = 1.0f;  // --ui-scale; width/height above are the window's, not the framebuffer's
  bool fullscreen = false;
  // Where the camera is, and whether it is still where opening the world put it. Null in
  // --ui-sample, which draws the panels with no world behind them. `cam_is_fit` is an exact
  // comparison against `FitTo` rather than a flag the loop maintains: a flag would have to be
  // cleared by every path that moves the camera -- wheel, drag, arrows, +/- -- and the one
  // somebody forgets is a command line that quietly stops reproducing the screen.
  const Camera* cam = nullptr;
  bool cam_is_fit = true;
  // The cell under the pointer, if it is over the world. `--hover` takes the same
  // coordinates, so a readout on screen is a readout in the reproduced frame.
  bool hover_valid = false;
  int hover_cell_x = 0, hover_cell_y = 0;
  // Two session flags with no other home. Pointers rather than copies for the same reason as
  // the block above: a checkbox that edits a copy is a checkbox that does nothing.
  bool* verbose = nullptr;
  bool* replay_registrations = nullptr;
  int* timeline_interval = nullptr;
  int* timeline_keep = nullptr;
  // What the child process is doing, if anything. Read-only here: the loop owns the process.
  bool test_running = false;
  std::string test_status;
  // The window, so a path field can open the system file picker parented to it. Nothing here
  // draws through it; see launch.h for why a native dialog is allowed at all.
  HWND owner = nullptr;

  // --- one-shot requests, cleared by the loop after it acts on them
  bool want_rescan = false;
  bool want_fit = false;
  bool want_snapshot = false;
  bool want_step_back = false;
  bool want_step = false;
  bool want_home = false;
  bool want_end = false;
  bool want_ext_apply = false;    // the ext toggles below changed
  bool want_events_apply = false; // the stream list below changed
  // Close this world and go back to the start screen. Not a setting: the source is the one
  // thing that cannot be changed in place, because every array, every table and the timeline
  // itself belong to the session that is about to be shut down.
  bool want_relaunch = false;
  // Tools tab. Each is one flag run against the session that is on screen; the parameters
  // live in State below, because they belong to the panel rather than to the session and a
  // field's identity is the address of what it edits.
  bool want_probe = false;
  bool want_stats = false;
  bool want_digest = false;
  bool want_ext_list = false;
  bool want_ext_attrs = false;
  bool want_events_list = false;
  bool want_dump_elements = false;
  bool want_dump_state = false;
  bool want_ext_blob = false;
  bool want_inject = false;
  bool want_ext_probe = false;
  bool want_timeline_config = false;
  // Tests tab. The run is a child process, so all the loop does is start and stop one.
  bool want_test_run = false;
  bool want_test_stop = false;
  // Help tab.
  bool want_reference = false;    // open or close the full-window flag reference
  bool want_font_sample = false;
  bool want_ui_sample = false;
  // --view attr:<name>: the attribute the attr view colours by. Its own flag rather than
  // want_ext_apply's, because applying it costs a registry-2 fetch and the two registries
  // are not fetched by the same call.
  bool want_attr_apply = false;
  // Live tab: --attach-arrays, which pins what each poll asks the game for.
  bool want_arrays_apply = false;

  // --- editable text, committed into the session by the loop
  bool ext_live = false;
  bool ext_published = false;
  std::string ext_props;
  std::string ext_view_name;
  std::string attr_view_name;
  std::string event_streams;
  std::string fetch_arrays;   // empty means "follow the view", which is the default
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

// Persistent panel state. One of these lives beside the window for as long as it is open.
struct State {
  ui::State ui;
  bool show_column = true;
  bool show_console = false;
  int tab = 0;
  out::Capture capture;
  Tools tools;
  Tests tests;

  // The flag reference, drawn over the whole window rather than in the 306-pixel column: 52
  // flags with their prose do not fit in a column, and a reference you have to scroll one
  // line at a time is a reference nobody reads to the end of.
  bool show_reference = false;
  std::string help_filter;
  std::deque<std::string> help_lines;   // rebuilt only when the width or the filter changes
  int help_cols = 0;
  std::string help_built_for = "\x01";  // a filter no one can type, so the first pass builds

  // Panel rectangles from the frame just drawn. See the header: the camera consults these
  // one frame late, on purpose.
  std::vector<ui::Rect> mouse_rects;

  bool WantsMouse(int x, int y) const {
    for (const ui::Rect& r : mouse_rects)
      if (r.Contains(x, y)) return true;
    return false;
  }
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
  if (font::g_large) c += " --font large";

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
// Tab 0 -- VIEW. What is drawn and how it is coloured.
// ---------------------------------------------------------------------------------------
inline void DrawViewTab(ui::Ctx& u, Bindings* b) {
  static const char* kViewNames[] = {"element", "temperature", "mass", "phase",   "flow",
                                     "liquid",  "sunlight",    "ext",  "attr"};
  int sel = static_cast<int>(*b->view);
  std::vector<std::string> options;
  for (int i = 0; i < 9; ++i) options.push_back(kViewNames[i]);
  const bool view_changed = u.Dropdown("view", options, &sel);
  u.Tooltip(u.last_row(), "--view NAME, ext:<property> or attr:<attribute>");
  if (view_changed) {
    // `ext` or `attr` with nothing named would draw an empty view and read as a bug. Refuse
    // it here the way the command line refuses `--view ext:` with no property.
    const bool named = sel == static_cast<int>(View::Ext)    ? !b->ext_view_name.empty()
                       : sel == static_cast<int>(View::Attr) ? !b->attr_view_name.empty()
                                                             : true;
    if (named) {
      *b->view = static_cast<View>(sel);
      b->want_rescan = true;
    }
  }
  u.Dim("keys 1-9 pick the same nine");

  if (*b->view == View::Ext && b->ext_view_name.empty()) {
    u.Label(u.theme().text_bad, "no extension property named");
    u.Wrapped(u.theme().text_dim,
              "Set one on the LIVE tab, or pass --view ext:<property>. Nothing here "
              "names a property: the list comes out of the sim's own registry.");
  }

  if (*b->view == View::Attr && b->attr_view_name.empty()) {
    u.Label(u.theme().text_bad, "no element attribute named");
    u.Wrapped(u.theme().text_dim,
              "Set one below, or pass --view attr:<attribute>. The names come out of the "
              "sim's own registry -- --ext-attrs, or the TOOLS tab, lists them.");
  }
  if (u.Field("attr view", &b->attr_view_name, "sim.molecular_mass")) b->want_attr_apply = true;
  u.Tooltip(u.last_row(), "--view attr:<attribute>");
  u.Wrapped(u.theme().text_dim,
            "Colours every cell by its ELEMENT's value for this attribute, so the picture "
            "is never finer than the element map. An element carrying no value is drawn as "
            "backdrop, which is a different answer from one whose value is zero.");

  u.Separator();

  int mode = static_cast<int>(*b->range_mode);
  static const char* kRangeModes[] = {"scan", "full", "pin"};
  if (u.Radio(kRangeModes, 3, &mode)) {
    *b->range_mode = static_cast<RangeMode>(mode);
    b->want_rescan = true;
  }
  if (*b->range_mode == RangeMode::Fixed) {
    if (u.DoubleField("lo", b->range_lo, -1e30, 1e30)) b->want_rescan = true;
    u.Tooltip(u.last_row(), "--range lo,hi");
    if (u.DoubleField("hi", b->range_hi, -1e30, 1e30)) b->want_rescan = true;
    u.Tooltip(u.last_row(), "--range lo,hi");
  } else if (b->range && b->range->valid) {
    u.Dim("%.4g .. %.4g", static_cast<double>(b->range->lo),
          static_cast<double>(b->range->hi));
  }
  u.Wrapped(u.theme().text_dim,
            "scan takes the 10th-90th percentile of the current view; full takes the "
            "whole span, outliers included; pin holds the two numbers so two runs can "
            "be compared by eye.");

  if (u.Button("rescan now (r)")) b->want_rescan = true;
  u.Tooltip(u.last_row(), "recomputes the range for the current view");
  u.Checkbox("mass fade (m)", b->mass_fade);
  u.Tooltip(u.last_row(), "--no-mass-fade turns this off");
  u.Wrapped(u.theme().text_dim,
            "Off draws trace gas as boldly as rock. On is the default because a room "
            "of near-vacuum otherwise reads as hot as a magma chamber.");

  u.Separator();
  u.Checkbox("HUD overlay (h)", b->show_hud);
  u.Tooltip(u.last_row(), "--hud, for a snapshot");
  if (b->timeline_on) u.Checkbox("scrub bar (t)", b->show_bar);
  if (b->attached && b->session && b->session->watching_events())
    u.Checkbox("event log pane (e)", b->show_events);
  if (u.Button("fit to window (f)")) b->want_fit = true;
  if (b->cam != nullptr) {
    u.Dim("camera  %.9g,%.9g,%.9g", static_cast<double>(b->cam->cx),
          static_cast<double>(b->cam->cy), static_cast<double>(b->cam->scale));
    u.Tooltip(u.last_row(), "--camera cx,cy,scale");
    u.Wrapped(u.theme().text_dim,
              b->cam_is_fit
                  ? "Fit to the window, so the command below needs no --camera to land here."
                  : "Moved from the fit, so the command below carries a --camera that puts "
                    "it back exactly. --crop and --zoom cannot: they magnify by a whole "
                    "number about a cell, and this scale is fractional.");
  }
  if (u.Button("save PNG (s)")) b->want_snapshot = true;
  u.Tooltip(u.last_row(), "--snapshot <out.png>");
}

// ---------------------------------------------------------------------------------------
// Tab 1 -- RUN. Time: ticking, polling, and the scrub timeline.
// ---------------------------------------------------------------------------------------
inline void DrawRunTab(ui::Ctx& u, Bindings* b) {
  u.Text(b->attached ? "poll %d   %.0f fps" : "tick %d   %.0f fps", b->tick,
         static_cast<double>(b->fps));

  static const char* kTransport[] = {"<", "play", ">"};
  static const char* kTransportPaused[] = {"<", "pause", ">"};
  const int hit = u.ButtonRow(*b->playing ? kTransportPaused : kTransport, 3);
  if (hit == 0) b->want_step_back = true;
  if (hit == 1) *b->playing = !*b->playing;
  if (hit == 2) b->want_step = true;
  u.Dim("space toggles, . steps, , steps back");

  if (b->attached) {
    u.Separator();
    if (u.IntField("poll ms", b->poll_ms, 16, 60000)) { /* the loop reads it next frame */ }
    u.Tooltip(u.last_row(), "--poll MS");
    u.Wrapped(u.theme().text_dim,
              "How often a followed session asks the game for a frame. Each ask costs "
              "the game a main-thread copy, so this is a request rate, not a frame rate: "
              "the window still redraws as fast as it likes between polls.");
    u.Dim("endpoint %s", b->source.c_str());
    if (b->fetch_pinned)
      u.Label(u.theme().text_dim, "array set pinned by --attach-arrays");
  } else {
    u.Separator();
    if (u.DoubleField("dt seconds", b->dt, 0.001, 10.0)) { /* applies to the next tick */ }
    u.Tooltip(u.last_row(), "--dt SECONDS");
    u.Wrapped(u.theme().text_dim,
              "Elapsed seconds per step. 0.2 is one substep; the sim runs "
              "max(1, (dt + carry) * 5) of them, so a larger dt is more physics per "
              "displayed frame rather than a faster one.");
    if (b->timeline_on) {
      u.Separator();
      u.Text("timeline %d .. %d", b->base, b->head);
      static const char* kSeek[] = {"|<", "-10", "+10", ">|"};
      const int s = u.ButtonRow(kSeek, 4);
      if (s == 0) b->want_home = true;
      if (s == 3) b->want_end = true;
      if (s == 1) { b->want_step_back = true; }
      if (s == 2) { b->want_step = true; }
      u.Dim("home/end, [ and ]  -- drag the bar to seek");
    } else {
      u.Label(u.theme().text_dim, "timeline off (--no-timeline)");
    }
  }
}

// ---------------------------------------------------------------------------------------
// Tab 2 -- SOURCE. What is open, and the way back to the start screen.
// ---------------------------------------------------------------------------------------
inline void DrawSourceTab(ui::Ctx& u, Bindings* b) {
  u.Label(b->attached ? u.theme().text_good : u.theme().text,
          b->attached ? "attached to a running game" : "driving a SimDLL in this process");
  u.Wrapped(u.theme().text, b->source);
  if (!b->attached && !b->dll.empty()) {
    u.Dim("SimDLL");
    u.Wrapped(u.theme().text_dim, b->dll);
  }
  if (b->session)
    u.Dim("%d x %d, %d cells", b->session->width(), b->session->height(),
          b->session->cells());
  if (b->seed_pinned) u.Dim("seed pinned to 0x%08X", b->seed);
  u.Dim("window %d x %d", b->width, b->height);

  u.Separator();
  if (u.Button("open another source...")) b->want_relaunch = true;
  u.Wrapped(u.theme().text_dim,
            "Shuts this world down and returns to the start screen with these settings "
            "filled in, so a corpus can be swapped for a live game -- or one corpus for "
            "the next -- without leaving the program.");
}

// ---------------------------------------------------------------------------------------
// Tab 3 -- LIVE. The two routes that cost the game something, and the one that does not.
// ---------------------------------------------------------------------------------------
inline void DrawLiveTab(ui::Ctx& u, Bindings* b) {
  if (!b->attached) {
    u.Wrapped(u.theme().text_dim,
              "Offline. The extension registry is always read here, and event streams do "
              "not exist: a stream is produced by a running game and never serialised, so "
              "there is nothing in a corpus to replay.");
    if (u.Field("ext view", &b->ext_view_name, "sim.gas_mass")) b->want_ext_apply = true;
    u.Dim("--view ext:<property>, then pick ext on the VIEW tab");
    return;
  }

  if (u.Field("arrays", &b->fetch_arrays, "what the view needs")) b->want_arrays_apply = true;
  u.Tooltip(u.last_row(), "--attach-arrays LIST, or 'all'");
  u.Wrapped(u.theme().text_dim,
            "Which arrays each poll asks for. Empty follows the view, which is the default; "
            "a list PINS them, so changing the view no longer changes what is fetched -- you "
            "asked for those arrays and swapping them would answer a different question.");

  u.Separator();
  bool live = b->ext_live, pub = b->ext_published;
  if (u.Checkbox("fetch /extcells", &live)) {
    b->ext_live = live;
    b->want_ext_apply = true;
  }
  u.Tooltip(u.last_row(), "--ext");
  u.Wrapped(u.theme().text_dim,
            "Off by default: this route makes the game wait for its sim thread to go idle "
            "and serialise every non-default property -- 12.7 MB on the reference world.");

  if (u.Checkbox("use the published path", &pub)) {
    b->ext_published = pub;
    b->want_ext_apply = true;
  }
  u.Tooltip(u.last_row(), "--ext-published[=a,b]");
  u.Wrapped(u.theme().text_dim,
            "/extcells?published=1 instead: no sim-thread wait, and only the properties "
            "named below. ASKING SUBSCRIBES -- the game starts publishing them on the next "
            "tick, and the route drops a subscription nothing has asked for in 120 seconds.");
  if (pub) {
    if (u.Field("props", &b->ext_props, "sim.gas_mass,sim.gas_species"))
      b->want_ext_apply = true;
  }

  u.Separator();
  if (u.Field("ext view", &b->ext_view_name, "sim.gas_mass")) b->want_ext_apply = true;
  u.Dim("colours the ext view; component as name[1]");

  u.Separator();
  if (u.Field("events", &b->event_streams, "sim.message_refused"))
    b->want_events_apply = true;
  u.Tooltip(u.last_row(), "--events=a,b");
  u.Wrapped(u.theme().text_dim,
            "Comma-separated streams to collect. Also subscribes. sim.message_refused is "
            "the only stream a stock oni-sim-replacement declares -- the messages it dropped "
            "because it does not implement them.");
  if (b->session && b->session->watching_events())
    u.Label(u.theme().text_good, "collecting");
}

// ---------------------------------------------------------------------------------------
// Tab 4 -- TOOLS. Every readout this program has, against the world that is on screen.
//
// These are the features that used to be reachable only by deciding, before the window
// existed, that you were going to want them. `--probe` is the clearest case: you had to know
// the coordinates of the interesting cell before you had ever seen the world.
// ---------------------------------------------------------------------------------------
inline void DrawToolsTab(ui::Ctx& u, Bindings* b, Tools* t) {
  u.Field("probe", &t->probe_rect, "x,y or x,y,w,h");
  u.Tooltip(u.last_row(), "--probe x,y[,w,h], image coords");
  if (u.Button("read those cells")) b->want_probe = true;
  u.Dim("--probe: element, mass, temperature, ext props");

  u.Separator();
  static const char* kReadouts[] = {"stats", "digest"};
  const int r = u.ButtonRow(kReadouts, 2);
  if (r == 0) b->want_stats = true;
  if (r == 1) b->want_digest = true;
  u.Dim("--stats percentiles; --digest hashes 12 arrays");

  // The first two are the two extension registries a listing can enumerate -- per-cell
  // properties and per-element attributes -- and both answer from either source. The THIRD is
  // whichever of its two the source can answer, rather than both with one greyed out: an event
  // stream is a live publication and the element table is lifted from a corpus file, so on any
  // given source exactly one of them exists.
  const char* kLists[3] = {"ext list", "ext attrs",
                           b->attached ? "events list" : "elements"};
  const int l = u.ButtonRow(kLists, 3);
  if (l == 0) b->want_ext_list = true;
  if (l == 1) b->want_ext_attrs = true;
  if (l == 2) {
    if (b->attached) b->want_events_list = true;
    else b->want_dump_elements = true;
  }
  u.Dim(b->attached ? "--ext-list, --ext-attrs, --events-list"
                    : "--ext-list, --ext-attrs, --dump-elements");

  u.Separator();
  u.Field("state file", &t->dump_state_path, "state.txt");
  u.Tooltip(u.last_row(), "--dump-state <file>");
  static const char* kWrites[] = {"dump state", "ext blob"};
  const int w = u.ButtonRow(kWrites, 2);
  if (w == 0) b->want_dump_state = true;
  if (w == 1) b->want_ext_blob = true;
  u.Field("blob file", &t->ext_blob_path, "extcells.bin");
  u.Tooltip(u.last_row(), "--ext-blob <file>");
  u.Dim("--dump-state feeds --diff-state; --ext-blob raw");

  if (!b->attached) {
    u.Separator();
    u.Field("inject gas", &t->inject_rect, "x,y,w,h");
    u.Tooltip(u.last_row(), "--inject-gas x,y,w,h");
    u.DoubleField("fraction", &t->inject_fraction, 0.0, 1.0);
    u.Tooltip(u.last_row(), "--inject-fraction F");
    if (u.Button("seed the mixture", !t->inject_rect.empty())) b->want_inject = true;
    u.Field("thermal bonus", &t->ext_probe_rect, "x,y,w,h");
    u.Tooltip(u.last_row(), "--ext-probe x,y,w,h");
    u.DoubleField("J/K", &t->ext_probe_jk, -1e12, 1e12);
    u.Tooltip(u.last_row(), "--ext-probe-jk J/K");
    if (u.Button("write the bonus", !t->ext_probe_rect.empty())) b->want_ext_probe = true;
    u.Wrapped(u.theme().text_dim,
              "Both change the world. The scrub timeline is re-anchored to now when they "
              "run, because its checkpoints describe a world that no longer exists -- "
              "seeking back into them would show a history that did not happen.");
  }

  u.Separator();
  if (b->verbose) u.Checkbox("verbose (--verbose)", b->verbose);
  if (!b->attached && b->replay_registrations)
    u.Checkbox("replay registrations", b->replay_registrations);
  if (b->timeline_on && b->timeline_interval && b->timeline_keep) {
    if (u.IntField("checkpoint every", b->timeline_interval, 1, 100000))
      b->want_timeline_config = true;
    if (u.IntField("checkpoints kept", b->timeline_keep, 4, 4096))
      b->want_timeline_config = true;
    u.Dim("--timeline-interval and --timeline-keep");
  }
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

inline void DrawTestsTab(ui::Ctx& u, Bindings* b, Tests* t) {
  if (!t->seeded) {
    // Seeded once from the session on screen, so the obvious experiment -- "run the
    // checkpoint test on the corpus I am looking at" -- is one click rather than a path to
    // retype. Attached there is no corpus to inherit and the field starts empty.
    if (!b->attached) t->corpus = b->source;
    t->dll = b->dll;
    t->seeded = true;
  }

  static const char* kNames[] = {"checkpoint round trip", "timeline seek",
                                 "timeline diff",         "load stress",
                                 "digest log",            "bisect two logs",
                                 "diff two states"};
  std::vector<std::string> options;
  for (const char* n : kNames) options.push_back(n);
  u.Dropdown("run", options, &t->which);

  const Test which = static_cast<Test>(t->which);
  const bool needs_corpus = which != Test::Bisect && which != Test::DiffState;

  if (needs_corpus) {
    u.Field("corpus", &t->corpus, "sim_corpus.bin");
    if (u.Button("browse...")) {
      launch::BrowseForFile(b->owner, "Pick a sim corpus",
                            "Sim corpus (*.bin)\0*.bin\0All files (*.*)\0*.*\0", &t->corpus);
    }
    u.Field("dll", &t->dll, "SimDLL.dll");
    u.Field("seed", &t->seed, "optional");
  }

  switch (which) {
    case Test::Checkpoint:
      u.IntField("span ticks", &t->span, 1, 1000000);
      u.Wrapped(u.theme().text_dim,
                "Runs the span, restores the capture, runs it again, and compares. Also "
                "runs the same comparison WITHOUT restoring the random stream, because a "
                "pass means nothing unless the thing under test can be seen to fail.");
      break;
    case Test::TimelineSeek:
      u.IntField("span ticks", &t->span, 1, 1000000);
      u.IntField("interval", &t->interval, 1, 100000);
      u.IntField("kept", &t->keep, 4, 4096);
      u.Wrapped(u.theme().text_dim,
                "Seeks the scrub timeline everywhere it can reach and checks that arriving "
                "at a tick by replay gives the same world as ticking to it.");
      break;
    case Test::TimelineDiff:
      u.IntField("target tick", &t->span, 1, 1000000);
      u.IntField("interval", &t->interval, 1, 100000);
      u.IntField("kept", &t->keep, 4, 4096);
      u.Wrapped(u.theme().text_dim,
                "Names what differs between a ticked world and a seeked one at the same "
                "tick, array by array, when the two disagree.");
      break;
    case Test::LoadStress:
      u.IntField("loads", &t->loads, 1, 100000);
      u.IntField("ticks between", &t->ticks_between, 0, 100000);
      u.Checkbox("no AllocateCells first (--bare-load)", &t->bare_load);
      u.Checkbox("replay registrations", &t->replay_registrations);
      u.Label(u.theme().text_bad, "this one is a crash repro");
      u.Wrapped(u.theme().text_dim,
                "One blob, re-Loaded. The tick after the third Load takes the process down "
                "-- which is the finding. It is a CHILD process, so what dies is the "
                "experiment and the exit code is reported here.");
      break;
    case Test::DigestLog:
      u.IntField("ticks", &t->ticks, 1, 1000000);
      u.Field("write to", &t->out_path, "digest.txt");
      u.Wrapped(u.theme().text_dim,
                "One line per tick, hashing every published array. Two of these, from two "
                "DLLs or two machines, are what bisect compares.");
      break;
    case Test::Bisect:
      u.Field("log A", &t->path_a, "a.txt");
      if (u.Button("browse A...")) {
        launch::BrowseForFile(b->owner, "Pick a digest log",
                              "Digest log (*.txt)\0*.txt\0All files (*.*)\0*.*\0",
                              &t->path_a);
      }
      u.Field("log B", &t->path_b, "b.txt");
      if (u.Button("browse B...")) {
        launch::BrowseForFile(b->owner, "Pick a digest log",
                              "Digest log (*.txt)\0*.txt\0All files (*.*)\0*.*\0",
                              &t->path_b);
      }
      u.Wrapped(u.theme().text_dim,
                "Names the first tick at which two digest logs part, and which array parted "
                "first. Needs no sim: it reads what earlier runs wrote.");
      break;
    case Test::DiffState:
      u.Field("state A", &t->path_a, "a.txt");
      if (u.Button("browse A...")) {
        launch::BrowseForFile(b->owner, "Pick a state dump",
                              "State dump (*.txt)\0*.txt\0All files (*.*)\0*.*\0",
                              &t->path_a);
      }
      u.Field("state B", &t->path_b, "b.txt");
      if (u.Button("browse B...")) {
        launch::BrowseForFile(b->owner, "Pick a state dump",
                              "State dump (*.txt)\0*.txt\0All files (*.*)\0*.*\0",
                              &t->path_b);
      }
      u.Field("view", &t->diff_view, "temperature");
      u.Field("png", &t->snapshot, "diff.png");
      u.Wrapped(u.theme().text_dim,
                "Compares two --dump-state files array by array and paints where they "
                "differ. Needs no sim either.");
      break;
  }

  u.Separator();
  u.Wrapped(u.theme().text, "simviz.exe " + TestCommand(*t));
  if (b->test_running) {
    u.Label(u.theme().text_good, "running -- output is in the pane (`)");
    if (u.Button("stop")) b->want_test_stop = true;
  } else {
    if (u.Button("run", TestReady(*t))) b->want_test_run = true;
    if (!TestReady(*t)) u.Label(u.theme().text_bad, "fill in the paths first");
  }
  if (!b->test_status.empty()) u.Wrapped(u.theme().text_dim, b->test_status);
  u.Wrapped(u.theme().text_dim,
            "Runs as a separate process: these rewrite the world dozens of times, so "
            "running one here would replace what is on screen and freeze the window while "
            "it did.");
}

// ---------------------------------------------------------------------------------------
// Tab 6 -- HELP. The keys, and the command line that reproduces what is on screen.
// ---------------------------------------------------------------------------------------
inline void DrawHelpTab(ui::Ctx& u, Bindings* b) {
  if (u.Button("flag reference (F1)")) b->want_reference = true;
  const size_t missing = help::MissingControls();
  if (missing == 0) {
    u.Label(u.theme().text_good, "all %zu flags have a control", help::FlagCount());
  } else {
    u.Label(u.theme().text_bad, "%zu of %zu flags have no control", missing,
            help::FlagCount());
  }
  u.Wrapped(u.theme().text_dim,
            "Counted from the same table the reference renders, not written down here -- a "
            "flag with nowhere to click is the thing this window exists to not have, so the "
            "claim is computed rather than asserted.");

  u.Separator();
  u.Text("keys");
  static const char* kKeys[] = {
      "space  play / pause",     ".  ,   one tick forward / back",
      "[  ]   ten ticks",        "home end  the ends of the history",
      "1-9    the nine views",   "t  scrub bar    e  event pane",
      "h      HUD overlay",      "r  rescan range   m  mass fade",
      "f      fit to window",    "s  save a PNG beside the exe",
      "+ -    zoom (or wheel)",  "arrows drag  pan",
      "tab    these panels",     "`      the output pane",
      "F1     the flag reference",
      "esc    quit",             "source tab: open another world",
      "tools tab: probe, stats, digest, dumps, injections",
      "tests tab: the experiments, as child processes",
  };
  for (const char* k : kKeys) u.Dim("%s", k);

  u.Separator();
  u.Text("the current view, as a command");
  const std::string cmd = CommandLine(*b);
  u.Wrapped(u.theme().text, cmd);
  u.Wrapped(u.theme().text_dim,
            "Every control in these panels is one flag, so this line reproduces what is on "
            "screen -- in a script, on another machine, or in a bug report. The camera is "
            "in it too, which is what makes that true of the picture and not only of the "
            "window.");
  u.Wrapped(u.theme().text_dim,
            "Swap --window for --hud --snapshot out.png and the same line writes this frame "
            "as a PNG with no window at all.");

  u.Separator();
  u.Text("proof sheets");
  static const char* kSheets[] = {"font", "panels"};
  const int sh = u.ButtonRow(kSheets, 2);
  if (sh == 0) b->want_font_sample = true;
  if (sh == 1) b->want_ui_sample = true;
  u.Wrapped(u.theme().text_dim,
            "--font-sample and --ui-sample, written beside the exe. Everything drawn here is "
            "painted into a software framebuffer with an owned bitmap font, which is what "
            "lets it be proof-read as a PNG with no window at all.");
}

// ---------------------------------------------------------------------------------------
// The flag reference, over the whole window.
//
// Not in the column: 52 flags with their prose do not fit in 306 pixels, and a reference read
// four words at a time is a reference nobody reaches the end of. It is the same table --help
// prints, wrapped to whatever width the window actually has, with the line naming where each
// flag lives in this window turned on.
//
// The filter matches the flag, the text AND the control column, so "tests" finds everything
// the tests tab runs and "subscribe" finds the two routes that do.
// ---------------------------------------------------------------------------------------
inline void DrawReference(Image* frame, State* st, ui::Ctx& u) {
  const int m = 24;
  const int x = m, y = m;
  const int w = frame->width() - 2 * m;
  const int h = frame->height() - 2 * m;
  if (w < 320 || h < 160) return;

  frame->FillRect(x - 1, y - 1, w + 2, h + 2, u.theme().edge);
  frame->FillRect(x, y, w, h, u.theme().ground);

  // The width the PANE will actually draw at, not the panel's: it insets by its own border
  // and reserves the scrollbar, and wrapping two characters wider than that would clip every
  // long line by exactly the amount nobody would notice until a path lost its last letters.
  const int cols = (w - 2 * ui::kPad - 6 - ui::kScrollW) / font::kAdvance;
  if (cols != st->help_cols || st->help_filter != st->help_built_for) {
    const std::vector<std::string> lines = help::Reference(cols, st->help_filter, true);
    st->help_lines.assign(lines.begin(), lines.end());
    st->help_cols = cols;
    st->help_built_for = st->help_filter;
    // A new filter is a new document, so it starts at the top rather than wherever the last
    // one had been scrolled to. The pane's own id is stable, so this is the one place that
    // has to say so.
    st->ui.Scroll(ui::HashId("reference", 0x5EED)) = 0;
  }

  u.BeginPanel(x, y, w, "flag reference -- F1 or escape closes");
  u.Field("filter", &st->help_filter, "a flag, a word, or a tab name");
  // Sized so the panel's own computed height lands on `h` exactly, rather than drawing its
  // edge two rows above the ground this function painted.
  const int pane_h = h - 2 * ui::kPad - ui::kTitleH - (ui::kRowH + 2 * ui::kRowGap);
  u.TextPane("reference", st->help_lines, pane_h, 0);
  u.EndPanel();
  st->mouse_rects.push_back(ui::Rect{x, y, w, h});
}

// ---------------------------------------------------------------------------------------
// The whole overlay: the column, the console, and the rectangles the camera must not read
// a drag through.
// ---------------------------------------------------------------------------------------
inline void Draw(Image* frame, State* st, Bindings* b, const ui::Input& in) {
  st->mouse_rects.clear();
  ui::Ctx u(frame, &st->ui, in);

  // The reference takes the whole window, so the column and the pane are not drawn under it.
  // Left in, a 24-pixel sliver of the column shows down the left edge and reads as a panel
  // that has been cut off rather than one that is not there.
  const bool chrome = !st->show_reference;

  if (st->show_column && chrome) {
    const int h = frame->height() - (st->show_console ? ConsoleH() : 0);
    // The column's own ground, painted full height so the tabs do not appear to float over
    // the world. BeginPanel paints only as far as its contents reach.
    frame->FillRect(0, 0, ColumnW(), h, u.theme().ground);
    frame->FillRect(ColumnW() - 1, 0, 1, h, u.theme().edge);

    u.BeginPanel(0, 0, ColumnW(), "oni-sim-visualizer");
    static const char* kTabs[] = {"view", "run", "source", "live", "tools", "tests", "help"};
    u.Tabs(kTabs, 7, &st->tab);
    switch (st->tab) {
      case 0: DrawViewTab(u, b); break;
      case 1: DrawRunTab(u, b); break;
      case 2: DrawSourceTab(u, b); break;
      case 3: DrawLiveTab(u, b); break;
      case 4: DrawToolsTab(u, b, &st->tools); break;
      case 5: DrawTestsTab(u, b, &st->tests); break;
      default: DrawHelpTab(u, b); break;
    }
    u.EndPanel();
    st->mouse_rects.push_back(ui::Rect{0, 0, ColumnW(), h});
  }

  if (st->show_console && chrome) {
    const int y = frame->height() - ConsoleH();
    frame->FillRect(0, y, frame->width(), ConsoleH(), u.theme().ground);
    frame->FillRect(0, y, frame->width(), 1, u.theme().edge);
    u.BeginPanel(0, y, frame->width(), "output");
    u.TextPane("console", st->capture.lines, ConsoleH() - ui::kTitleH - 3 * ui::kPad,
               st->capture.dropped);
    u.EndPanel();
    st->mouse_rects.push_back(ui::Rect{0, y, frame->width(), ConsoleH()});
  }

  if (st->show_reference) DrawReference(frame, st, u);

  // A click that hit no control drops focus, which is what clicking the world means.
  if (u.click_unused()) {
    u.ClearFocus();
    st->ui.open_dropdown = 0;
  }
  u.Finish();
}

}  // namespace panels
}  // namespace simviz
