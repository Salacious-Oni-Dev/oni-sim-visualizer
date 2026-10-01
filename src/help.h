// The flag reference, as DATA rather than as one printf.
//
// WHY. There were two descriptions of this program's flags and there were about to be three.
// `Usage()` held a hand-wrapped string literal; the README held a table; and the window now
// needs a third, because a person who started the exe by double-clicking it has no console to
// run `--help` in. Three copies of the same list is three copies that drift, and the one that
// drifts first is always the one nobody runs.
//
// So there is one table. `--help` prints it, the in-app reference renders it, and the coverage
// check reads it. A flag added to the parser and not to this table is a flag with no
// documentation anywhere, which is at least a single place to notice.
//
// THE `control` COLUMN IS THE POINT OF THE WHOLE GUI EFFORT. Every flag names where in the
// window it can be reached instead. An empty one is a flag you can only get at by restarting
// with a command line -- which is exactly the state this program was in before, and exactly
// what "every feature available from the GUI" means the absence of. `MissingControls()` counts
// them, and the reference shows the count, so the claim is checked rather than asserted.
//
// The text is the original prose, not a summary of it. Wrapping is done here at whatever width
// the reader has, so the console gets 78 columns and the window's reference gets however wide
// the window is.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace simviz {
namespace help {

// The SimDLL the offline path binds when nothing else is named. Lives here because the usage
// text has to quote it and because there should be exactly one of it.
inline constexpr const char* kDefaultDll = "..\\oni-sim-replacement\\sim\\build\\SimDLL.dll";

struct Entry {
  const char* group;    // non-empty starts a new heading; "" continues the current one
  const char* flag;
  const char* arg;      // "" for a flag that takes none
  const char* text;
  const char* control;  // where the same thing lives in the window; "" means nowhere
};

inline const Entry* Entries(size_t* count) {
  static const Entry table[] = {
      // ------------------------------------------------------------------ source
      {"source -- exactly one of these, and it decides everything else", "--corpus",
       "<file>",
       "Offline. This program owns the simulation: it binds a SimDLL, replays the recorded "
       "message stream of one game load, and drives it from here. Scrubbable in both "
       "directions, because the state is ours to checkpoint.",
       "start screen"},
      {"", "--attach", "host:port",
       "Live. A RUNNING Oxygen Not Included owns the simulation and this reads its grid over "
       "oni-framework-api's bulk routes (default port 9788). Forward only, but it is the colony "
       "rather than a replay of one. From WSL the host is the vEthernet gateway, not "
       "127.0.0.1.",
       "start screen"},
      {"", "--dll", "<path>",
       "Which SimDLL --corpus drives. Default ..\\oni-sim-replacement\\sim\\build\\SimDLL.dll.",
       "start screen, tests tab"},
      {"", "--seed", "N",
       "Pin the sim's random stream, making a run reproducible on a loaded world. Base 0, so "
       "0x1234abcd counts. See --digest for what reproducible is worth.",
       "start screen, tests tab"},
      {"", "--ticks", "N",
       "Sim steps to run after the corpus is loaded, before anything else happens (default "
       "0).",
       "start screen, tests tab"},
      {"", "--dt", "SECONDS",
       "Elapsed seconds per step (default 0.2, which is one substep). The sim runs "
       "max(1, (dt + carry) * 5) substeps, so a larger dt is more physics per displayed "
       "frame rather than a faster one.",
       "start screen, run tab"},
      {"", "--replay-registrations", "",
       "Re-send the corpus's registration messages after every restore that reallocates "
       "(buildings, element chunks, flow, radiation). Without it those registrations are "
       "lost to Allocate's defaults.",
       "tools tab, tests tab"},

      // ------------------------------------------------------------------ live attach
      {"live attach -- these need --attach and mean nothing without it", "--poll", "MS",
       "How often a followed session asks the game for a frame (default 200). Each ask costs "
       "the game a main-thread copy, so this is a request rate, not a frame rate.",
       "start screen, run tab"},
      {"", "--attach-timeout", "MS",
       "Give up on one request after this long (default 5000).", "start screen"},
      {"", "--attach-arrays", "LIST",
       "Fetch exactly these arrays instead of the ones the view needs; 'all' fetches every "
       "published surface. Pinned, a view change does not touch the set: you asked for those "
       "arrays and swapping them would answer a question nobody asked.",
       "live tab"},
      {"", "--ext", "",
       "Also fetch /extcells each refresh. Off by default: it costs the game a sim-thread "
       "wait and a full serialisation -- 12.7 MB on the reference world. Implied by "
       "--ext-list, --probe and --view ext:. Offline the registry is always read.",
       "start screen, live tab"},
      {"", "--ext-published", "[=a,b]",
       "Read the registry off the PUBLISHED path (/extcells?published=1) instead: no "
       "sim-thread wait, and only the properties named. ASKING SUBSCRIBES -- the game "
       "publishes them from the next tick. Defaults to the property --view ext: is showing; "
       "a bare flag with no view shows what is already published and subscribes nothing.",
       "start screen, live tab"},
      {"", "--events", "=a,b",
       "Watch those event streams and show them in an event log pane, or print them once "
       "headless. ASKING SUBSCRIBES -- the game collects them from the next tick, and the "
       "route drops the subscription after 120 idle SECONDS, wall clock, so pausing the game "
       "to look at something does not expire it. The only stream a stock oni-sim-replacement "
       "declares is sim.message_refused.",
       "start screen, live tab"},
      {"", "--events-list", "",
       "Print what event streams the game declares and carry on. Subscribes nothing.",
       "tools tab"},

      // ------------------------------------------------------------------ the view
      {"what is drawn, and how it is coloured", "--view", "NAME",
       "element, temperature, mass, phase, flow, liquid, sunlight -- the last three are "
       "property textures, and a game that does not publish one is refused rather than drawn "
       "empty. Also ext:<property>[c], which colours by a per-cell extension property by "
       "name (--ext-list names the ones being carried), and attr:<name>[c], which colours "
       "by a per-ELEMENT attribute looked up through each cell's element (--ext-attrs names "
       "those). An attr view is never finer than the element map, because the data is not. "
       "And simcheck (alias errormap), which is KLEI'S OWN SimCheckErrorMap overlay, drawn "
       "with the game's colours: red for a NaN mass or temperature, a cell over 10 t or "
       "10 000 K, or a non-vacuum cell under 10 K -- and for a sealed world's 20 000 kg "
       "neutronium border, which is not a fault; yellow and blue for vacuum holding a "
       "temperature or mass, which are; gray for clean vacuum, green for an effectively "
       "massless gas, magenta and cyan for a cell sitting past a phase threshold with a "
       "transition to take. The rule is transcribed in oni-sim-replacement/driver/src/simcheck.h "
       "and the same predicate gates `bench` and `diffsim` offline; live, oni-framework-api's "
       "/simcheck route asks the running game's own copy of it.",
       "start screen, view tab, keys 0-9"},
      {"", "--range", "lo,hi | full",
       "Pin the scalar range, or use the absolute min..max instead of the default 10..90 "
       "percentile band. Pinned is how two runs are compared by eye.",
       "view tab"},
      {"", "--no-mass-fade", "",
       "Draw trace gas as boldly as rock. On by default, because a room of near-vacuum "
       "otherwise reads as hot as a magma chamber.",
       "start screen, view tab, key m"},
      {"", "--size", "WxH",
       "Window, or composed snapshot, size in pixels.", "start screen"},
      {"", "--ui-scale", "N",
       "Draw the window at N times size, 1 to 4, with sharp pixels: the HUD, panels and "
       "text grow, --size stays the window size. For very high-DPI screens.",
       "none in the window: a launch choice, like --size"},
      {"", "--font", "large|small",
       "The UI font: small is 5x8 (the default), large is 7x13 from the public-domain X11 "
       "misc-fixed family, sharp at its own size.",
       "the start screen picks large on a screen 1440 px tall or more"},
      {"", "--fullscreen", "",
       "Open the window borderless over the whole primary monitor; --size is ignored.",
       "none in the window: a launch choice, like --size"},
      {"", "--window", "",
       "Open the interactive window.",
       "the start screen always opens one"},

      // ------------------------------------------------------------------ the timeline
      {"the scrub timeline -- offline only; attached there is no history to hold",
       "--no-timeline", "",
       "Do not checkpoint at all. '.' still steps forward; nothing steps back. What to use "
       "when a checkpoint itself is what is being investigated.",
       "start screen"},
      {"", "--timeline-interval", "N", "Ticks between checkpoints (default 10).",
       "tools tab"},
      {"", "--timeline-keep", "N",
       "Checkpoints held before the history is thinned to half resolution over the same span "
       "(default 8).",
       "tools tab"},

      // ------------------------------------------------------------------ readouts
      {"readouts -- every one of these prints, and the window's output pane is where",
       "--probe", "x,y[,w,h]",
       "Raw cell state as text: element, mass, temperature, and every extension property "
       "carried there. IMAGE coordinates, y from the top, the same as --crop, so a feature "
       "spotted in a snapshot can be probed where it was seen.",
       "tools tab, and the hover card follows the mouse"},
      {"", "--stats", "", "Quantiles of the current view's scalar.", "tools tab"},
      {"", "--digest", "",
       "Hash the published cell arrays -- state, not pixels. Two digests that agree are two "
       "worlds that agree.",
       "tools tab"},
      {"", "--ext-list", "",
       "List the per-cell extension properties the sim carries, whatever they are. Names and "
       "types come out of the sim's own registry, so this repo holds no list of them.",
       "tools tab"},
      {"", "--ext-attrs", "",
       "List the per-element attributes -- registry 2 -- with the elements that carry a "
       "value. Offline this is what the SimDLL registers for itself; attached it is "
       "/extelements, so a mod's attributes are in it too.",
       "tools tab"},
      {"", "--dump-elements", "",
       "Print the element table lifted from the corpus and exit; no sim boot. Attached, the "
       "same table is one request away: curl <host>/elements.",
       "tools tab"},
      {"", "--dump-state", "<file>",
       "Write every published array, plus the extension and registry blobs, for "
       "--diff-state.",
       "tools tab"},
      {"", "--ext-blob", "<file>",
       "Write the raw extension blob and carry on -- exactly the bytes the sim produced, "
       "which is what a bug report about a property should carry.",
       "tools tab"},
      {"", "--verbose", "", "Let the sim's own log lines through.", "tools tab"},

      // ------------------------------------------------------------------ mutations
      {"changing the world -- offline only, and each re-anchors the timeline",
       "--inject-gas", "x,y,w,h",
       "Move part of each gas cell's mass in that rectangle into the volume-fractions "
       "mixture layer (image coords), which is the only way to make the mixing kernels run.",
       "tools tab"},
      {"", "--inject-fraction", "F",
       "How much of each cell to move (default 0.5; must be below 1).", "tools tab"},
      {"", "--ext-probe", "x,y,w,h",
       "Write a per-cell thermal-mass bonus into that rectangle, so the seventh checkpoint "
       "component carries something a replay can diverge on.",
       "tools tab"},
      {"", "--ext-probe-jk", "J/K",
       "The value --ext-probe writes (default 50000, large enough that a checkpoint dropping "
       "it shows up as a temperature divergence within a handful of ticks).",
       "tools tab"},

      // ------------------------------------------------------------------ experiments
      {"experiments -- long, destructive, and one of them crashes on purpose",
       "--checkpoint-test", "N",
       "Capture, run N ticks, restore, run N again, and require both runs to agree. Also "
       "runs the same comparison WITHOUT restoring the random stream, because a pass means "
       "nothing unless the thing under test can be seen to fail.",
       "tests tab"},
      {"", "--timeline-test", "N",
       "Run N ticks with the scrub timeline on, then seek around it and require every seek "
       "to match the run it is seeking within.",
       "tests tab"},
      {"", "--timeline-diff", "T",
       "Seek to one tick and report every cell that differs from the run, with the "
       "scheduling counters from both sides.",
       "tests tab"},
      {"", "--load-stress", "N",
       "Capture ONE blob and Load it back N times, ticking after each. The tick after the "
       "third Load takes the process down, which is the finding: the payload is identical "
       "every time, so the fault is in state the sim carries across an in-place Load.",
       "tests tab"},
      {"", "--load-stress-ticks", "N",
       "Ticks between each Load (default 1).", "tests tab"},
      {"", "--bare-load", "",
       "Skip the AllocateCells before each Load, isolating the Load itself.", "tests tab"},
      {"", "--digest-log", "<file>",
       "Write every tick's digest to a text log, for --bisect. Attached this is refused: "
       "there are no ticks, only polls, so two such logs could not be compared line for "
       "line.",
       "tests tab"},
      {"", "--bisect", "a.log b.log",
       "Read two digest logs and report the first tick they stopped agreeing on, and in "
       "what. No sim runs; the two logs may come from different processes, DLLs or machines.",
       "tests tab"},
      {"", "--diff-state", "a b",
       "Which CELLS differ between two --dump-state files, and by how much. With --snapshot, "
       "draws the divergence in the world. No sim runs.",
       "tests tab"},
      {"", "--diff-view", "<array>",
       "Which array --diff-state draws (default: the first that differs). Names are the ones "
       "--diff-state prints.",
       "tests tab"},

      // ------------------------------------------------------------------ snapshots
      {"snapshots -- one frame, no window", "--snapshot", "<out.png>",
       "Render one frame to a PNG and exit.",
       "view tab: save PNG, or the s key"},
      {"", "--hud", "",
       "Compose the snapshot the way the window does, overlay included, instead of one pixel "
       "per cell. This is how the overlay is proof-read without a person at a window.",
       "view tab: HUD overlay, or the h key"},
      {"", "--hover", "x,y",
       "Place the hover readout at that CELL, for --hud.",
       "the mouse: the card follows the pointer"},
      {"", "--zoom", "N",
       "Integer magnification of a plain (non --hud) snapshot.",
       "the wheel, or + and -"},
      {"", "--crop", "x,y,w,h",
       "Crop in CELL coordinates, y measured from the top.",
       "pan and zoom, then save PNG"},
      {"", "--camera", "cx,cy,scale",
       "Place the camera of a COMPOSED frame exactly -- the window, or --hud with "
       "--snapshot. This is the form the view tab prints, and unlike --crop and --zoom it "
       "can express the fit-to-window scale, which is fractional. Refused for a plain "
       "snapshot, which has no camera.",
       "view tab: the camera readout"},

      // ------------------------------------------------------------------ meta
      {"about the program itself", "--help", "",
       "Print this reference and exit.",
       "help tab: the F1 reference"},
      {"", "--font-sample", "",
       "Print the HUD font as text art, and write it as a sheet with --snapshot. No sim "
       "boot.",
       "help tab: write the sheets"},
      {"", "--ui-sample", "",
       "Compose the control panels and the start screen with no window, no sim and no game, "
       "into --snapshot <out.png>: one PNG per tab, one more for each tab the source changes, "
       "and one per start-screen shape. The layout is then proof-read offline like "
       "everything else painted here.",
       "help tab: write the sheets"},
  };
  *count = sizeof(table) / sizeof(table[0]);
  return table;
}

// Flags that can still only be reached by restarting with a command line. The number this
// returns is the whole claim of the GUI work, which is why it is computed rather than written
// down: it cannot be right in the README and wrong in the program.
inline size_t MissingControls() {
  size_t n = 0, missing = 0;
  const Entry* t = Entries(&n);
  for (size_t i = 0; i < n; ++i) {
    if (t[i].control[0] == '\0') ++missing;
  }
  return missing;
}

inline size_t FlagCount() {
  size_t n = 0;
  Entries(&n);
  return n;
}

// Wraps `text` at `cols`, indenting every line after the first by `indent`. Breaks on spaces;
// a word longer than the width is cut rather than allowed to run off, because a path that
// silently leaves the panel is worse than one with a visible break in it.
inline void WrapInto(std::vector<std::string>* out, const std::string& lead,
                     const std::string& text, int indent, int cols) {
  const int width = cols - indent > 8 ? cols - indent : 8;
  size_t at = 0;
  bool first = true;
  while (at < text.size() || first) {
    size_t take = text.size() - at;
    if (take > static_cast<size_t>(width)) {
      take = static_cast<size_t>(width);
      const size_t brk = text.rfind(' ', at + take);
      if (brk != std::string::npos && brk > at) take = brk - at;
    }
    std::string line = first ? lead : std::string(static_cast<size_t>(indent), ' ');
    if (line.size() < static_cast<size_t>(indent))
      line.resize(static_cast<size_t>(indent), ' ');
    line += text.substr(at, take);
    out->push_back(line);
    at += take;
    while (at < text.size() && text[at] == ' ') ++at;
    first = false;
    if (at >= text.size()) break;
  }
}

// Does this entry match a filter? The flag, the text and the control column all count, so
// "tests" finds everything the tests tab runs and "subscribe" finds the two routes that do.
inline bool Matches(const Entry& e, const std::string& lower_filter) {
  if (lower_filter.empty()) return true;
  std::string hay = std::string(e.flag) + " " + e.arg + " " + e.text + " " + e.control;
  for (char& c : hay) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return hay.find(lower_filter) != std::string::npos;
}

// The reference, rendered. `cols` is the width in characters. `with_controls` adds the line
// naming where the same thing lives in the window -- on in the program, off in `--help`,
// which would otherwise double in length for a reader who is already in a shell.
inline std::vector<std::string> Reference(int cols, const std::string& filter,
                                          bool with_controls) {
  std::string lower = filter;
  for (char& c : lower) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }

  std::vector<std::string> out;
  size_t n = 0;
  const Entry* t = Entries(&n);
  const int indent = 26;
  std::string pending_group;
  size_t shown = 0;

  for (size_t i = 0; i < n; ++i) {
    if (t[i].group[0] != '\0') pending_group = t[i].group;
    if (!Matches(t[i], lower)) continue;
    if (!pending_group.empty()) {
      out.push_back("");
      out.push_back(" " + pending_group + ":");
      pending_group.clear();
    }
    ++shown;
    std::string lead = std::string("  ") + t[i].flag;
    if (t[i].arg[0] != '\0') lead += std::string(" ") + t[i].arg;
    WrapInto(&out, lead, t[i].text, indent, cols);
    if (with_controls) {
      WrapInto(&out, std::string(static_cast<size_t>(indent), ' '),
               std::string("in the window: ") +
                   (t[i].control[0] != '\0' ? t[i].control : "NOWHERE -- command line only"),
               indent, cols);
    }
  }
  if (shown == 0) out.push_back("  nothing matches that.");
  return out;
}

}  // namespace help
}  // namespace simviz
