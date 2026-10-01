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
// A NATIVE DIALOG IS USED FOR FILE PICKING, and only for that. GetOpenFileName is transient
// -- it is not part of any frame this program renders, so `--snapshot --hud` still reproduces
// exactly what the window shows. Writing a file browser out of the owned widgets would be a
// week of work to arrive somewhere worse than the one every Windows user already knows.

#pragma once

#include <algorithm>

#include <windows.h>

#include <commdlg.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "font.h"
#include "image.h"
#include "out.h"
#include "ui.h"
#include "views.h"
#include "window.h"

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

// The one thing this screen cannot do with its own widgets. Returns false when the dialog was
// cancelled, which is not an error and must leave `*out` alone.
inline bool BrowseForFile(HWND owner, const char* title, const char* filter,
                          std::string* out) {
  char buf[MAX_PATH];
  buf[0] = '\0';
  // Seeded with the current value so re-browsing opens where the last pick came from. A path
  // longer than MAX_PATH is left out rather than truncated: a truncated seed would open the
  // dialog in the wrong directory and look like the dialog lost it.
  if (out->size() < sizeof(buf)) snprintf(buf, sizeof(buf), "%s", out->c_str());

  OPENFILENAMEA ofn = {};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = filter;
  ofn.lpstrFile = buf;
  ofn.nMaxFile = sizeof(buf);
  ofn.lpstrTitle = title;
  // NOCHANGEDIR: the dialog must not move the process's working directory, because every
  // relative path this program was given -- the default DLL among them -- resolves against it.
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
  if (!GetOpenFileNameA(&ofn)) return false;
  *out = buf;
  return true;
}

// ---------------------------------------------------------------------------------------
// The screen itself.
// ---------------------------------------------------------------------------------------
constexpr int kWidth = 700;
constexpr int kHeight = 520;
// The window, its panels and the header grow with --font large (7/5, the 7x13 font's line
// over the 5x8 one's), so the large font gets the small font's layout rather than the same
// 700 pixels with fewer words per line.
inline int LauncherW() { return font::g_large ? kWidth * 7 / 5 : kWidth; }
inline int LauncherH() { return font::g_large ? kHeight * 7 / 5 : kHeight; }
inline int PanelW() { return LauncherW() - 2 * 12; }
// Below the header's three lines: 48 at 5x8. The error band sits in the last 4 pixels of it
// and the first panel 10 below it.
inline int HeaderBottom() { return 12 + 3 * (font::kLineH + 2); }

// What the top of the screen says. Kept to four lines: this is the first thing a person who
// has never used the program reads, and a wall of text at the door is a wall of text nobody
// reads.
inline void DrawHeader(Image* img, const ui::Theme& th, int x, int y) {
  font::DrawString(img, x, y, "oni-sim-visualizer", th.title);
  font::DrawString(img, x + font::MeasureWidth("oni-sim-visualizer") + 10, y,
                   "-- look at an ONI simulation, cell by cell", th.text_dim);
  font::DrawString(img, x, y + font::kLineH + 2,
                   "Pick a source and press start. Everything else can be changed while the "
                   "world is on screen,", th.text_dim);
  font::DrawString(img, x, y + 2 * (font::kLineH + 2),
                   "from the panels the tab key opens.", th.text_dim);
}

inline ui::Rect DrawSourcePanel(ui::Ctx& u, Spec* s, HWND owner) {
  u.BeginPanel(12, HeaderBottom() + 10, PanelW(), "source -- which simulation");

  int mode = s->attach ? 1 : 0;
  static const char* kModes[] = {"corpus (this program drives a SimDLL)",
                                 "attach (a running game drives its own)"};
  if (u.Radio(kModes, 2, &mode)) s->attach = (mode == 1);

  if (!s->attach) {
    u.Field("corpus", &s->corpus, "sim_corpus.bin");
    static const char* kBrowseCorpus[] = {"browse for a corpus..."};
    if (u.ButtonRow(kBrowseCorpus, 1) == 0) {
      BrowseForFile(owner, "Pick a sim corpus",
                    "Sim corpus (*.bin)\0*.bin\0All files (*.*)\0*.*\0", &s->corpus);
    }
    if (s->corpus.empty()) {
      u.Label(u.theme().text_dim, "a corpus is the recorded stream of messages the game "
                                  "sent its sim during one load");
    } else if (FileExists(s->corpus)) {
      u.Label(u.theme().text_good, "found");
    } else {
      u.Label(u.theme().text_bad, "no such file");
    }

    u.Field("SimDLL", &s->dll, "SimDLL.dll");
    static const char* kBrowseDll[] = {"browse for a SimDLL..."};
    if (u.ButtonRow(kBrowseDll, 1) == 0) {
      BrowseForFile(owner, "Pick a SimDLL", "SimDLL (*.dll)\0*.dll\0All files (*.*)\0*.*\0",
                    &s->dll);
    }
    if (!s->dll.empty() && !FileExists(s->dll))
      u.Label(u.theme().text_bad, "no such file -- the boot will fail");

    u.Separator();
    u.Field("seed", &s->seed, "leave empty to let the corpus decide");
    u.IntField("ticks first", &s->ticks, 0, 1000000);
    u.DoubleField("dt seconds", &s->dt, 0.001, 10.0);
    u.Checkbox("keep a scrub timeline", &s->timeline);
    u.Wrapped(u.theme().text_dim,
              "The timeline checkpoints the sim every few ticks so the bar can seek "
              "backwards. Off is --no-timeline, which is what to use when a checkpoint "
              "itself is what is being investigated.");
  } else {
    u.Field("endpoint", &s->endpoint, "127.0.0.1:9788");
    u.Wrapped(u.theme().text_dim,
              "host:port of oni-framework-api's debug server in a running game. The port is "
              "the one the game's log prints when the bulk routes come up.");
    u.IntField("poll ms", &s->poll_ms, 16, 60000);
    u.IntField("request timeout ms", &s->attach_timeout_ms, 100, 600000);

    u.Separator();
    u.Checkbox("fetch the extension registry (--ext)", &s->ext_live);
    u.Wrapped(u.theme().text_dim,
              "Costs the game a sim-thread wait and a serialisation of every non-default "
              "property -- 12.7 MB on the reference world. Off unless something needs it.");
    u.Checkbox("use the published path (--ext-published)", &s->ext_published);
    if (s->ext_published) u.Field("props", &s->ext_props, "sim.gas_mass");
    u.Field("events", &s->events, "sim.message_refused");
    u.Wrapped(u.theme().text_dim,
              "Both of those SUBSCRIBE: the game starts publishing on the next tick and "
              "drops the subscription after 120 idle seconds.");
  }

  return u.EndPanel();
}

inline ui::Rect DrawViewPanel(ui::Ctx& u, Spec* s, int y) {
  u.BeginPanel(12, y, PanelW(), "first frame -- what to draw");

  static const char* kViewNames[] = {"element", "temperature", "mass", "phase",   "flow",
                                     "liquid",  "sunlight",    "ext",  "attr"};
  std::vector<std::string> options;
  for (int i = 0; i < 9; ++i) options.push_back(kViewNames[i]);
  int sel = static_cast<int>(s->view);
  if (u.Dropdown("view", options, &sel)) s->view = static_cast<View>(sel);
  if (s->view == View::Ext) {
    u.Field("property", &s->ext_name, "sim.gas_mass");
    u.IntField("component", &s->ext_comp, 0, 63);
    if (s->ext_name.empty())
      u.Label(u.theme().text_bad, "the ext view needs a property name");
  }
  if (s->view == View::Attr) {
    u.Field("attribute", &s->attr_name, "sim.molecular_mass");
    u.IntField("component", &s->attr_comp, 0, 63);
    if (s->attr_name.empty())
      u.Label(u.theme().text_bad, "the attr view needs an attribute name");
  }
  u.Checkbox("fade by mass", &s->mass_fade);

  u.Separator();
  u.IntField("window width", &s->width, 320, 8192);
  u.IntField("window height", &s->height, 240, 8192);

  return u.EndPanel();
}

// Returns 1 to start, -1 to quit, 0 to keep going.
inline int DrawStartPanel(ui::Ctx& u, const Spec& s, int y) {
  u.BeginPanel(12, y, PanelW(), "start");

  const bool ready = s.attach ? !s.endpoint.empty()
                              : (!s.corpus.empty() && FileExists(s.corpus));
  u.Wrapped(u.theme().text_dim, Command(s));

  int action = 0;
  // Two buttons, two rows, because only one of them can be disabled: a start that looks
  // pressable and does nothing is worse than one that is visibly not ready yet.
  if (u.Button("start", ready)) action = 1;
  if (u.Button("quit")) action = -1;
  if (!ready) {
    u.Label(u.theme().text_bad, s.attach ? "give an endpoint first"
                                         : "point at a corpus file first");
  } else {
    u.Dim("enter starts, escape quits");
  }

  u.EndPanel();
  return action;
}

// One frame of the start screen, drawn into `frame`. Split out of the loop so the same pass
// can be rendered with no window at all -- see main.cpp's --ui-sample, which is the only way
// this screen is checkable without a person sitting in front of it.
//
// Returns 1 to start, -1 to quit, 0 to keep going.
inline int DrawScreen(Image* frame, ui::State* st, Spec* spec, const ui::Input& in,
                      const std::string& error, HWND owner, const ui::Theme& th) {
  ui::Ctx u(frame, st, in, th);
  DrawHeader(frame, th, 12, 12);
  if (!error.empty()) {
    frame->FillRect(12, HeaderBottom() - 4, PanelW(), font::kLineH + 2, th.ground_alt);
    font::DrawString(frame, 16, HeaderBottom() - 2, error.c_str(), th.text_bad);
  }
  const ui::Rect source = DrawSourcePanel(u, spec, owner);
  const ui::Rect shown = DrawViewPanel(u, spec, source.y + source.h + 8);
  const int decision = DrawStartPanel(u, *spec, shown.y + shown.h + 8);

  // A click that hit no control drops focus, the same rule the main window's panels use.
  if (u.click_unused()) {
    u.ClearFocus();
    st->open_dropdown = 0;
  }
  u.Finish();
  return decision;
}

// Runs the launcher to a decision. `spec` is both the prefill and the result: the caller seeds
// it with whatever it already knows (the defaults, or the session being replaced), and on true
// it comes back holding what to open. False means the person closed the window or pressed
// quit, which is not a failure and must not print anything.
//
// `error` is what went wrong on the previous attempt, drawn at the top. This is the only place
// a failed boot or a refused attach can be READ when the program was started from Explorer:
// there is no console to print it to, and a window that closes again immediately is a program
// that looks broken rather than one that has an answer.
inline bool RunLauncher(Spec* spec, const std::string& error = std::string()) {
  // On a large screen the 5x8 font is too small to read, so the launcher switches to the
  // 7x13 font (--font large), which the window it opens keeps, and proposes a window
  // enlarged with it, kept inside the screen.
  if (GetSystemMetrics(SM_CYSCREEN) >= 1440 && !font::g_large) {
    font::UseLarge(true);
    ui::SyncFontMetrics();
    spec->large_font = true;
    spec->width = std::min(spec->width * 7 / 5, GetSystemMetrics(SM_CXSCREEN) - 80);
    spec->height = std::min(spec->height * 7 / 5, GetSystemMetrics(SM_CYSCREEN) - 120);
  }
  Window win;
  if (!win.Open("oni-sim-visualizer", LauncherW(), LauncherH())) {
    out::Printf("could not open a window\n");
    return false;
  }

  ui::State st;
  ui::Theme th;
  Image frame(win.width(), win.height());
  int decision = 0;

  while (win.Pump() && decision == 0) {
    if (win.resized() || frame.width() != win.width() || frame.height() != win.height()) {
      win.clear_resized();
      frame = Image(win.width() > 0 ? win.width() : 1, win.height() > 0 ? win.height() : 1);
    }
    frame.Fill(th.ground);

    const bool typing = st.focus != 0;

    ui::Input in;
    in.mouse_x = win.mouse_x();
    in.mouse_y = win.mouse_y();
    in.clicked = win.clicked();
    in.click_x = win.click_x();
    in.click_y = win.click_y();
    in.mouse_down = win.mouse_down();
    in.wheel = win.wheel();
    in.typed = win.typed();
    if (typing) {
      in.key_left = win.key_repeats(VK_LEFT);
      in.key_right = win.key_repeats(VK_RIGHT);
      in.key_home = win.key_repeats(VK_HOME);
      in.key_end = win.key_repeats(VK_END);
      in.key_delete = win.key_repeats(VK_DELETE);
    }

    decision = DrawScreen(&frame, &st, spec, in, error, win.hwnd(), th);

    // Enter and escape are the keyboard half of the two buttons, and only when no field has
    // focus: inside a field they belong to the editor, which commits on Enter and abandons
    // on Escape.
    if (!typing) {
      const bool ready = spec->attach ? !spec->endpoint.empty()
                                      : (!spec->corpus.empty() && FileExists(spec->corpus));
      if (win.pressed(VK_RETURN) && ready) decision = 1;
      if (win.pressed(VK_ESCAPE)) decision = -1;
    }

    win.Present(frame);
    Sleep(8);  // a settings screen has nothing to animate; 125 Hz is more than enough
  }

  win.Close();
  return decision == 1;
}

// The start screen written to a PNG, with no window, no sim and no game -- the same trick
// --ui-sample plays on the panels, and for the same reason: a screen nobody can look at
// offline is a screen whose regressions are found by a person noticing them.
inline void WriteSample(const Spec& spec, const std::string& error, Image* out) {
  Spec copy = spec;
  ui::State st;
  ui::Theme th;
  *out = Image(LauncherW(), LauncherH());
  ui::Input in;
  // Parked over nothing, so no row is drawn in its hover colour.
  in.mouse_x = -1;
  in.mouse_y = -1;
  // Twice: a panel's ground is painted from the height it had on the previous frame, so a
  // single-frame render shows every panel with no ground under it.
  for (int pass = 0; pass < 2; ++pass) {
    out->Fill(th.ground);
    DrawScreen(out, &st, &copy, in, error, nullptr, th);
  }
}

}  // namespace launch
}  // namespace simviz
