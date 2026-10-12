// oni-sim-visualizer -- a standalone visualizer for the oni-sim-replacement simulation.
//
// It is a separate program and is NEVER loaded into Oxygen Not Included. It loads
// SimDLL the way driver/'s six offline tools already do, drives it, and draws the
// result. The game does not need to be running.
//
// Two modes, one renderer:
//
//   --window     an interactive window: play/pause, single-step, pan, zoom, switch
//                view, and a hover readout of raw cell state. Milestone 1.
//   --snapshot   one frame to a PNG with no window and no graphics context. Milestone
//                0, and it stays the regression check -- it is the only way to see what
//                the renderer actually drew without a person looking at a desktop.
//                With --hud it composes the identical frame the window shows, HUD
//                included, so the overlay is checkable offline too.
//
//   simviz.exe --corpus <corpus.bin> [--dll <SimDLL.dll>] [--ticks N] [--seed N]
//              [--view element|temperature|mass|phase|flow|liquid|sunlight|simcheck]
//              [--window | --snapshot out.png [--hud] [--hover x,y]]
//              [--size WxH] [--ui-scale N] [--font large] [--fullscreen] [--zoom N] [--crop x,y,w,h] [--range lo,hi]
//              [--probe x,y[,w,h]] [--dump-elements] [--font-sample] [--verbose]
//
// Two SOURCES, one renderer, and they are a different axis from the two modes above:
//
//   --corpus     offline. This program owns the simulation: it binds SimDLL, replays a
//                capture into it and steps it. Scrubbable in both directions, because
//                the state is ours to checkpoint.
//   --attach     live. A RUNNING Oxygen Not Included owns the simulation and
//                oni-framework-api's bulk routes hand its arrays over a socket. Forward
//                only -- there is nothing to checkpoint on this side of the wire.
//                Milestone 3. See attach.h.
//
// Every view, probe, stat and readout below is written against `Session::frame()` and
// does not know which of the two filled it.

// FIRST, before <windows.h>: attach.h opens with <winsock2.h>, and windows.h pulls in
// the older winsock.h unless winsock2.h got there first. The order is load-bearing.
#include "attach.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "child.h"
#include "digestlog.h"
#include "frame.h"
#include "help.h"
#include "image.h"
#include "launch.h"
#include "out.h"
#include "panels.h"
#include "qtapp.h"
#include "session.h"
#include "statedump.h"
#include "timeline.h"
#include "views.h"

namespace {

using namespace simviz;

// One definition, in help.h, because the usage text has to quote it too.
const char* kDefaultDll = help::kDefaultDll;

void Usage() {
  out::Printf(
      "usage: simviz.exe --corpus <corpus.bin> [options]\n"
      "       simviz.exe --attach host:port [options]\n"
      "\n"
      " Run it with NO arguments for the start screen, which picks a source, opens a\n"
      " window, and shows the command line it is equivalent to. Every flag below names,\n"
      " in the window's own reference (F1), the control that reaches the same thing.\n");
  // Printed from the same table the in-app reference renders, so the two cannot drift --
  // see help.h. 78 columns is the width a terminal is assumed to have; the window asks for
  // whatever it actually has.
  for (const std::string& line : help::Reference(78, "", false)) {
    out::Printf("%s\n", line.c_str());
  }
}

bool ParseTwo(const char* s, float* a, float* b) {
  return sscanf(s, "%f,%f", a, b) == 2;
}

bool ParseFour(const char* s, int* a, int* b, int* c, int* d) {
  return sscanf(s, "%d,%d,%d,%d", a, b, c, d) == 4;
}

// cx,cy,scale -- the camera itself, as the window holds it. Floats and not cells, because
// the fit-to-window scale is fractional by construction and the origin with it.
bool ParseCamera(const char* s, float* cx, float* cy, float* scale) {
  return sscanf(s, "%f,%f,%f", cx, cy, scale) == 3;
}

bool ParseSize(const char* s, int* w, int* h) {
  return sscanf(s, "%dx%d", w, h) == 2 || sscanf(s, "%d,%d", w, h) == 2;
}

// x,y or x,y,w,h -- the window defaults to a single cell.
bool ParseProbe(const char* s, int* x, int* y, int* w, int* h) {
  *w = *h = 1;
  const int n = sscanf(s, "%d,%d,%d,%d", x, y, w, h);
  return n == 2 || n == 4;
}

// Raw per-cell state as text. Coordinates are IMAGE coordinates -- y measured from the
// top, matching --crop -- so a feature spotted in a snapshot can be probed at the
// coordinates it was seen at, without the reader having to flip anything by hand.
// Printed wherever a boot is reported. Worded for the person who recorded the corpus,
// because the fix is theirs to make and the program cannot make it for them.
void WarnUnpublishedBoot(const Session& s) {
  if (!s.boot_unpublished()) return;
  out::Printf("WARNING: this corpus loads its world after its last frame, so nothing has been\n"
              "      published for that world yet and tick 0 shows meaningless values. It\n"
              "      was probably recorded from a new game, whose worldgen is the session the\n"
              "      shim records. Record again by loading a save as the first world after\n"
              "      starting the game, or run at least one tick before reading anything.\n");
}

void Probe(const Session& s, int px, int py, int pw, int ph) {
  const oni_sim::GameDataUpdate* g = s.frame();
  const simhost::Tables& tab = s.tables();
  const int w = s.width(), h = s.height();
  out::Printf("\nprobe %d,%d %dx%d (image coords; y=0 is the top)\n", px, py, pw, ph);
  // The property textures are printed beside the cell they were derived from, which is the
  // only place the two can be compared without leaving the tool. Columns are omitted rather
  // than zero-filled when the DLL publishes no textures: a zero that means "not published"
  // reads exactly like a zero that means "nothing here".
  const bool tex = g->propertyTextureFlow != nullptr;
  out::Printf("%5s %5s %8s %5s %12s %5s %12s %12s", "x", "y(img)", "cell", "elem", "id(hash)",
         "phase", "mass(kg)", "temp(K)");
  if (tex) out::Printf("  %11s %11s %4s %9s", "flow x", "flow y", "sun", "liquid");
  out::Printf("\n");
  for (int j = 0; j < ph; ++j) {
    for (int i = 0; i < pw; ++i) {
      const int x = px + i, y_img = py + j;
      if (x < 0 || y_img < 0 || x >= w || y_img >= h) continue;
      const int y = h - 1 - y_img;
      const int cell = y * w + x;
      const uint16_t idx = g->elementIdx[cell];
      // Checked, because the grid can hold an index past the table and Tables::At does not
      // bounds-check (its sibling accessors do). Measured on a corpus recorded from a new
      // colony: at tick 0, before any frame has published, 144,832 cells held values such as
      // 16256 (0x3F80, the high half of a float 1.0) and the unchecked probe crashed on one.
      const bool in_table = idx < tab.count;
      out::Printf("%5d %6d %8d %5u %12d %5u %12.4f %12.3f", x, y_img, cell, idx,
             in_table ? tab.At(idx)->id : 0, static_cast<unsigned>(tab.Phase(idx)), g->mass[cell],
             g->temperature[cell]);
      if (tex) {
        float fx, fy;
        FlowAt(g, cell, &fx, &fy);
        out::Printf("  %+11.6f %+11.6f %4u  %08X", static_cast<double>(fx),
               static_cast<double>(fy), (unsigned)SunlightAt(g, cell),
               (unsigned)TextureWord(g->propertyTextureLiquid, cell));
      }
      out::Printf("\n");
    }
  }
  // The extension properties for the same cells, under the table rather than beside it:
  // they are variable in number and in width, so a column per property would reflow the
  // whole table the first time a mod registered one. See --ext-list for the registry-wide
  // view and for what is NOT in here.
  const extprops::Table& ex = s.ExtProps();
  if (ex.decoded && !ex.empty()) {
    out::Printf("\nextension properties (padded grid %dx%d)\n", ex.padded_w, ex.padded_h);
    for (int j = 0; j < ph; ++j) {
      for (int i = 0; i < pw; ++i) {
        const int x = px + i, y_img = py + j;
        if (x < 0 || y_img < 0 || x >= w || y_img >= h) continue;
        const size_t padded = extprops::PaddedIndex(x, h - 1 - y_img, ex.padded_w);
        for (const oni_sim::SaveExtProperty& prop : ex.props) {
          out::Printf("%5d %6d  %s\n", x, y_img, extprops::FormatCell(prop, padded).c_str());
        }
      }
    }
  }
}

// Every per-cell extension property the sim is carrying, with the one number that says
// whether it is doing anything: how many cells hold something other than its default.
//
// This is the generic half of the extension registry landing in this program. Nothing below names a
// property: the names, types, arities and defaults all come out of the blob, so a property
// registered by a mod that did not exist when this was written prints here anyway.
// What the game declares, printed verbatim. This program has no JSON reader -- adding one to
// show a route's own sentence would be a worse trade than printing it -- so the route's answer
// is the output rather than a re-rendering of it.
void EventsList(const Session& s) {
  out::Printf("\nevent streams\n");
  if (!s.attached()) {
    out::Printf("  offline: this process publishes no frames, so it declares no streams\n");
    return;
  }
  std::string json, err;
  if (!s.source().FetchExtEventStreams(&json, &err)) {
    out::Printf("  %s\n", err.c_str());
    return;
  }
  out::Printf("  %s\n", json.c_str());
}

// The headless half of the log pane: subscribe, wait out the two-tick latency the subscription
// costs, drain once, and print what came.
//
// It waits, where the window does not, and the difference is the shape of the run rather than a
// preference. A window polls again in 16ms whatever happens; a one-shot invocation that
// returned "subscribing" would be a command that can never succeed the first time it is used,
// which is how a diagnostic gets a reputation for not working.
void EventsOnce(Session& s) {
  out::Printf("\nevent streams: %s\n", s.event_streams().c_str());
  bool served = false;
  for (int attempt = 0; attempt < 20 && !served; ++attempt) {
    served = s.PollEvents();
    if (!served) Sleep(100);
  }
  if (!served) {
    out::Printf("  %s\n", s.events_error().empty() ? "no reply" : s.events_error().c_str());
    return;
  }
  // A second drain, because the first one that SUCCEEDS is usually the one that arrived on the
  // tick the subscription landed and therefore carries nothing. Draining again a moment later
  // is the difference between "nothing was refused" and "nothing had been collected yet".
  Sleep(300);
  s.PollEvents();

  const std::vector<extevents::Refusal>& log = s.refusals();
  if (log.empty()) {
    out::Printf("  no records -- nothing was refused while this was watching\n");
  }
  for (const extevents::Refusal& f : log) {
    out::Printf("  %s\n", extevents::Describe(f).c_str());
  }
  if (s.events_lost() > 0 || s.events_dropped() > 0 || s.events_forgotten() > 0) {
    // Never summed: the route's buffer overrunning, the producer overrunning its own per-frame
    // cap, and this log forgetting its oldest rows are three failures with three fixes.
    out::Printf("  gaps: route dropped %d batches, sim dropped %d records, log forgot %d\n",
           s.events_lost(), s.events_dropped(), s.events_forgotten());
  }
}

void ExtList(const Session& s) {
  const extprops::Table& ex = s.ExtProps();
  out::Printf("\nper-cell extension properties\n");
  if (!ex.exported) {
    out::Printf("  %s\n", ex.error.c_str());
    return;
  }
  if (!ex.decoded) {
    out::Printf("  blob is %zu bytes and did not decode: %s\n", ex.blob_bytes, ex.error.c_str());
    return;
  }
  out::Printf("  %zu carried, %zu blob bytes, padded grid %dx%d, via %s\n", ex.props.size(),
         ex.blob_bytes, ex.padded_w, ex.padded_h,
         ex.source.empty() ? "(unrecorded)" : ex.source.c_str());
  if (!ex.empty()) {
    out::Printf("\n  %-40s %5s %5s %10s %12s %10s\n", "name", "type", "arity", "cells",
           "non-default", "default");
    for (const oni_sim::SaveExtProperty& p : ex.props) {
      out::Printf("  %-40s %5s %5d %10d %12zu %10s\n", p.name.c_str(), extprops::TypeName(p.type),
             p.arity, p.cell_count, extprops::NonDefaultCells(p),
             extprops::FormatValue(p, extprops::DefaultValue(p)).c_str());
    }
  }
  // Every exclusion below is a property of the SIM's blob, not of this reader, and each one
  // makes an absent property look exactly like one that was never registered. Saying so is
  // the difference between a list and a list that can be trusted.
  if (!ex.all_classes) {
    out::Printf("\n  This SimDLL has only the CHECKPOINT export, which carries the kRehydrated "
           "and\n  kCheckpointOnly classes. kSaved properties -- the gas mixture triple and "
           "\n  sim.room_promoted, which is everything the sim itself writes -- live in the "
           "save\n  blob and are NOT listed above. Rebuild SimDLL for "
           "SIM_DebugExtCellStateAll.\n");
  }
  out::Printf("\n  A property at its registered default in every cell is omitted by the sim, so "
         "on\n  this side \"absent\" and \"registered but untouched\" read the same. An "
         "orphan -- an\n  uninstalled mod's bytes, carried through the save blob verbatim -- "
         "is not a\n  registered property and is not here either.\n");
}

// Registry 2, listed. The companion to ExtList above, and the shape is deliberately
// different because the data is: a per-cell property has a value in every cell, so that
// listing is one row per property with a count; an attribute has a value for a handful of
// elements, so this one prints the values themselves.
//
// TWO NUMBERS THAT LOOK REDUNDANT AND ARE NOT. `values` counts the elements carrying a value
// and `writes` counts the messages that ever touched the attribute, so `0 values, 12 writes`
// says the pusher ran and cleared, and `0 values, 0 writes` says nobody has pushed yet. There
// is no other way to tell those apart, and they mean opposite things to somebody wondering
// why a mod's numbers are missing.
void AttrList(const Session& s) {
  const extattrs::Table t = s.ElementAttributes();
  out::Printf("\nper-element attributes\n");
  if (!t.exported) {
    out::Printf("  %s\n", t.error.c_str());
    return;
  }
  out::Printf("  %zu registered, via %s\n", t.attrs.size(),
         t.source.empty() ? "(unrecorded)" : t.source.c_str());
  if (t.attrs.empty()) {
    out::Printf("\n  Nothing is registered, which on a custom SimDLL means no sim is booted: "
           "the sim\n  registers sim.molecular_mass for itself, in World's constructor.\n");
    return;
  }
  for (const extattrs::Attribute& a : t.attrs) {
    out::Printf("\n  [%d] %s  %s", a.idx, a.name.c_str(), extattrs::TypeName(a.type));
    if (a.arity != 1) out::Printf("[%d]", a.arity);
    out::Printf("  %s  %d value%s, %llu write%s\n", a.first_party ? "sim" : "mod", a.value_count,
           a.value_count == 1 ? "" : "s", static_cast<unsigned long long>(a.writes),
           a.writes == 1 ? "" : "s");
    if (!t.with_values) continue;
    if (a.values.empty() && a.value_count == 0) {
      out::Printf("      %s\n", a.writes == 0
                                    ? "no element carries a value, and none ever did"
                                    : "no element carries a value now -- the pusher cleared them");
      continue;
    }
    for (const extattrs::Value& v : a.values) {
      out::Printf("      %-34s %s\n", extattrs::Describe(v).c_str(),
             extattrs::FormatValues(a, v).c_str());
    }
  }
  // Every one of these is a property of the registry rather than of this reader, and each one
  // makes a value that exists look like one that does not.
  out::Printf("\n  Nothing here is ever saved: an attribute is content data its owner re-pushes "
         "after\n  every load, so an offline listing shows only what the SimDLL registers for "
         "itself.\n  Keys are SimHashes ids, never element-table rows, so a value can outlive "
         "the table\n  it was written for -- \"not in table\" is that, and is a real value.\n");
}

// Prints the real element table lifted from the corpus. This is the ground-truth check
// for the colour byte order in views.h: the packed value is shown alongside both
// readings, so the right one can be picked by looking rather than by reasoning.
int DumpElements(const char* corpus) {
  simhost::Tables t;
  if (!simhost::LoadTables(corpus, &t)) {
    out::Printf("could not lift tables from %s\n", corpus);
    return 1;
  }
  out::Printf("%d elements\n\n", t.count);
  out::Printf("%4s  %-12s %5s %6s  %-10s  %-16s  %-16s\n", "idx", "id(hash)", "state",
         "phase", "packed", "as ABGR (r,g,b)", "as ARGB (r,g,b)");
  for (int32_t i = 0; i < t.count; ++i) {
    const oni_sim::Element* e = t.At(i);
    const uint32_t c = e->colour;
    out::Printf("%4d  %-12d %5u %6u  0x%08X  %3u,%3u,%3u       %3u,%3u,%3u\n", i, e->id,
           e->state, static_cast<unsigned>(e->state & 3), c,
           c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF,
           (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
  }
  return 0;
}

struct Digests {
  uint64_t element = 0, mass = 0, temperature = 0;
  double total_mass = 0.0;
  // The rest of what GameDataUpdate publishes per cell. Hashed because leaving them out was
  // not a decision, it was an oversight that made the timeline's bar narrower than its claim:
  // "every tick reproduces bit for bit" was measured over three of the nine published arrays.
  // Disease matters most of the three -- disease_accum_ and disease_infest_ are per-cell
  // growth state that no blob carries and FromBlob explicitly zeroes on every load (matching
  // Klei, whose SaveDisease is eight bytes too), which is the same shape as the gap
  // ext::kSetStableTicks had to close. The reference corpus carries 3,739 diseased cells
  // across four disease types, so this is exercised rather than hypothetical.
  uint64_t radiation = 0, disease = 0, backwall = 0;
  // The three the game owns rather than the sim: a building writes them and no blob carries
  // them, so a restore leaves Allocate's defaults. Hashed anyway, because "the timeline
  // reproduces the run" should be measured over everything published and then qualified,
  // rather than quietly scoped to the arrays that happen to reproduce.
  uint64_t building = 0;
  // The volume-fractions mixture layer, when there is one. Digested SEPARATELY from the
  // three above rather than folded into them, because the coupling between the two layers
  // runs one way only. MixPair READS PhaseEntry.temperature -- the mixture has no
  // temperature of its own -- and writes gas_mass_, gas_species_, gas_occupied_mask_,
  // gas_dirty_ and gas_sleeping_, none of which is published: GameDataUpdate carries
  // elementIdx, mass and temperature straight off PhaseEntry, and the mixture has no
  // projection into it yet (sim/simdll.cpp's vf block -- "the two layers are still
  // separate"). A run whose mixture diverges is therefore byte-identical in every array
  // hashed above, and without this field the timeline's PASS bar cannot see that class of
  // divergence at all. Zero on a run that never injected, which is every ordinary one.
  uint64_t mixture = 0;
  double mixture_mass = 0.0;
  // The four component registries, hashed straight off SIM_DebugRegistryState rather than
  // inferred from their effect on the grid. The registry gap was found the hard way:
  // a stale registry showed up as a temperature divergence several ticks later, in an array
  // that had nothing to do with it, and only in replays. Hashing the blob makes that class of
  // divergence visible at the frame it happens, and names it. Zero on a DLL without the
  // export, which is the same thing as saying the checkpoint cannot carry it.
  uint64_t registry = 0;
  // The per-cell extension properties no save blob carries, hashed straight off
  // SIM_DebugExtCellState. Same argument as `registry` one field up: the values are physics
  // inputs -- the sim's conduction kernel binds the thermal-mass bonus inside its pair loop --
  // so a checkpoint that dropped them would surface as a temperature divergence several ticks
  // later, in an array that has nothing to do with them. Zero on a DLL without the export.
  uint64_t ext_cells = 0;
  // The five property textures and accumulatedFlow -- the last six pointers in
  // GameDataUpdate, and until now the only published state this digest did not cover. They
  // are DERIVED from the grid rather than state of their own, which is exactly why they are
  // worth hashing: the sim builds them incrementally and caches what it built
  // (PropertyTextureBuffers::last_element, last_liquid, flow_dirty, primed), so a texture can
  // disagree with the grid it was derived from while every array above is identical. That is
  // not hypothetical -- a check once found two extension handlers that wrote the world without
  // marking the projection dirty, by hand, in a class of bug this digest could not see.
  //
  // Grouped by PRODUCER, three hashes rather than six, so a mismatch names the pass that
  // wrote it: FillFlowTexture (flow, plus accumulatedFlow, which is the same quantity before
  // the texture scales it), the liquid loop in FillPropertyTextures (liquid, liquidData and
  // materialData, all three written together and all three zero for a cell holding no
  // liquid), and ComputeSunlight (exposedToSunlight, which is the one texture with a gameplay
  // consumer -- Grid.exposedToSunlight, which solar panels and plants read).
  uint64_t flow = 0, liquid = 0, sunlight = 0;
  bool operator==(const Digests& o) const {
    return element == o.element && mass == o.mass && temperature == o.temperature &&
           mixture == o.mixture && radiation == o.radiation && disease == o.disease &&
           backwall == o.backwall && building == o.building && registry == o.registry &&
           ext_cells == o.ext_cells && flow == o.flow && liquid == o.liquid &&
           sunlight == o.sunlight;
  }
};

// Which components of two digests disagree, as a leading-space list. One place that knows
// the component names, because there are now fifteen of them and three call sites that print
// them: a component added to Digests and forgotten in one printf is a divergence the tool
// reports as "MISMATCH [ ]".
std::string DigestDelta(const Digests& d, const Digests& r) {
  std::string out;
  auto add = [&out](bool differs, const char* name) {
    if (differs) {
      out += ' ';
      out += name;
    }
  };
  add(d.element != r.element, "elementIdx");
  add(d.mass != r.mass, "mass");
  add(d.temperature != r.temperature, "temperature");
  add(d.radiation != r.radiation, "radiation");
  add(d.disease != r.disease, "disease");
  add(d.backwall != r.backwall, "backwall");
  add(d.mixture != r.mixture, "mixture");
  add(d.building != r.building, "building");
  add(d.registry != r.registry, "registry");
  add(d.ext_cells != r.ext_cells, "extCells");
  add(d.flow != r.flow, "flowTex");
  add(d.liquid != r.liquid, "liquidTex");
  add(d.sunlight != r.sunlight, "sunlightTex");
  return out;
}

Digests TakeDigests(const Session& s) {
  const oni_sim::GameDataUpdate* g = s.frame();
  const int n = s.cells();
  auto mix = [](uint64_t h, const void* p, size_t bytes) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < bytes; ++i) {
      h ^= b[i];
      h *= 1099511628211ull;
    }
    return h;
  };
  auto fnv = [&mix](const void* p, size_t bytes) {
    return mix(1469598103934665603ull, p, bytes);
  };
  Digests d;
  // Every array below is nullable, and the guards are not defensive padding: LIVE ATTACH
  // fetches only the surfaces a view needs, so most of these are legitimately absent and
  // reading one would be a segfault rather than a wrong number. That was not a
  // hypothetical -- `--attach --digest` crashed here on the first run, before the fetch
  // set was widened to `all` for exactly this reason, and the widening is a policy that
  // a future flag could undo. A missing array leaves its digest at zero, which is the
  // same convention the property-texture block below already uses and which
  // PrintTextureCoverage exists to qualify.
  if (g->elementIdx) d.element = fnv(g->elementIdx, sizeof(uint16_t) * n);
  if (g->mass) d.mass = fnv(g->mass, sizeof(float) * n);
  if (g->temperature) d.temperature = fnv(g->temperature, sizeof(float) * n);
  if (g->radiation) d.radiation = fnv(g->radiation, sizeof(float) * n);
  // One hash over both disease arrays: an index without its count is not a state.
  if (g->diseaseIdx && g->diseaseCount) {
    uint64_t h = fnv(g->diseaseIdx, sizeof(uint8_t) * n);
    const uint8_t* b = reinterpret_cast<const uint8_t*>(g->diseaseCount);
    for (size_t k = 0; k < sizeof(int32_t) * static_cast<size_t>(n); ++k) {
      h ^= b[k];
      h *= 1099511628211ull;
    }
    d.disease = h;
  }
  if (g->backwallElement && g->backwallMass && g->backwallTemperature) {
    uint64_t h = fnv(g->backwallElement, sizeof(uint16_t) * n);
    const uint8_t* bm = reinterpret_cast<const uint8_t*>(g->backwallMass);
    for (size_t k = 0; k < sizeof(float) * static_cast<size_t>(n); ++k) {
      h ^= bm[k];
      h *= 1099511628211ull;
    }
    const uint8_t* bt = reinterpret_cast<const uint8_t*>(g->backwallTemperature);
    for (size_t k = 0; k < sizeof(float) * static_cast<size_t>(n); ++k) {
      h ^= bt[k];
      h *= 1099511628211ull;
    }
    d.backwall = h;
  }
  if (g->properties && g->insulation && g->strengthInfo) {
    uint64_t h = fnv(g->properties, sizeof(uint8_t) * n);
    const uint8_t* ins = g->insulation;
    for (size_t k = 0; k < static_cast<size_t>(n); ++k) {
      h ^= ins[k];
      h *= 1099511628211ull;
    }
    const uint8_t* str = g->strengthInfo;
    for (size_t k = 0; k < static_cast<size_t>(n); ++k) {
      h ^= str[k];
      h *= 1099511628211ull;
    }
    d.building = h;
  }
  if (g->mass) {
    for (int i = 0; i < n; ++i) d.total_mass += g->mass[i];
  }
  // Not a published array: the sim serializes it on request. One call per digest, which is
  // two serializations inside the DLL (once to size, once to fill) over a few hundred
  // kilobytes -- cheap next to the nine-megabyte save a checkpoint already copies.
  {
    const std::vector<uint8_t> reg = s.RegistryState();
    if (!reg.empty()) d.registry = fnv(reg.data(), reg.size());
    const std::vector<uint8_t> ext = s.ExtCellState();
    if (!ext.empty()) d.ext_cells = fnv(ext.data(), ext.size());
  }
  // The property textures. A null pointer is left as a zero hash rather than skipped
  // silently: a DLL that publishes no textures cannot diverge in them, and the coverage line
  // the timeline test prints says which of them actually carried anything.
  {
    const size_t cells = static_cast<size_t>(n);
    uint64_t h = 1469598103934665603ull;
    if (g->propertyTextureFlow) h = mix(h, g->propertyTextureFlow, sizeof(float) * 2 * cells);
    if (g->accumulatedFlow) h = mix(h, g->accumulatedFlow, sizeof(float) * cells);
    if (g->propertyTextureFlow || g->accumulatedFlow) d.flow = h;

    uint64_t l = 1469598103934665603ull;
    bool any = false;
    if (g->propertyTextureLiquid) {
      l = mix(l, g->propertyTextureLiquid, 4 * cells);
      any = true;
    }
    if (g->propertyTextureLiquidData) {
      l = mix(l, g->propertyTextureLiquidData, 4 * cells);
      any = true;
    }
    if (g->propertyTextureMaterialData) {
      l = mix(l, g->propertyTextureMaterialData, 4 * cells);
      any = true;
    }
    if (any) d.liquid = l;

    if (g->propertyTextureExposedToSunlight) {
      d.sunlight = fnv(g->propertyTextureExposedToSunlight, cells);
    }
  }
  // One SIM_DebugGasMass call per cell per injected species over the seeded window only --
  // see Session::InjectGasRect for why the window is bounded rather than the whole world.
  // Skipped entirely, at no cost, on a session that never injected.
  if (s.has_mixture()) {
    uint64_t h = 1469598103934665603ull;
    for (int y = s.mixture_y0(); y <= s.mixture_y1(); ++y) {
      for (int x = s.mixture_x0(); x <= s.mixture_x1(); ++x) {
        const int cell = y * s.width() + x;
        for (uint16_t sp : s.injected_species()) {
          const float m = s.GasMass(cell, sp);
          const uint8_t* b = reinterpret_cast<const uint8_t*>(&m);
          for (size_t k = 0; k < sizeof(m); ++k) {
            h ^= b[k];
            h *= 1099511628211ull;
          }
          d.mixture_mass += m;
        }
      }
    }
    d.mixture = h;
  }
  return d;
}

// Does a checkpoint actually round-trip? This is milestone 2's load-bearing question and
// it is answered by measurement, not by reading the handler.
//
// Run `span` ticks from a captured state, digest. Restore that same capture, run the
// same `span` again, digest. If a checkpoint is a complete state the two agree exactly;
// if the random stream is not part of it they diverge as soon as physics touches a gas,
// which on this corpus is immediately.
//
// The control matters as much as the test: the same comparison is also run WITHOUT
// restoring the stream, because "it matched" means nothing unless the thing being
// tested can be seen to fail when removed.
int CheckpointTest(Session& s, int span, float dt) {
  out::Printf("\ncheckpoint round trip: capture, run %d ticks, restore, run %d again\n",
         span, span);

  const Session::Checkpoint cp = s.Capture();
  if (!cp.valid()) {
    out::Printf("  FAIL -- SIM_BeginSave returned nothing\n");
    return 1;
  }
  out::Printf("  captured at tick %d: %zu bytes (%.2f MB), rng 0x%08X\n", cp.ticks,
         cp.bytes(), cp.bytes() / (1024.0 * 1024.0), cp.rng);
  out::Printf("  %.1f bytes per cell over %d cells\n",
         static_cast<double>(cp.bytes()) / s.cells(), s.cells());

  for (int i = 0; i < span; ++i) {
    if (!s.Tick(dt)) {
      out::Printf("  FAIL -- tick %d refused\n", i);
      return 1;
    }
  }
  const Digests first = TakeDigests(s);
  const uint32_t rng_after_first = s.RandomState();
  out::Printf("  original  after %d ticks: elem %016llx mass %016llx temp %016llx rng 0x%08X\n",
         s.ticks(), (unsigned long long)first.element, (unsigned long long)first.mass,
         (unsigned long long)first.temperature, rng_after_first);

  // The real test: blob and stream together.
  if (!s.Restore(cp)) {
    out::Printf("  FAIL -- the sim refused the blob on Load\n");
    return 1;
  }
  // kSetRandomState is QUEUED: the sim drains it at the top of the next frame, so the
  // state read back right now is still the old one. Reported rather than hidden, because
  // reading it here and seeing the pre-restore value looks like the message was ignored.
  out::Printf("  restored to tick %d (rng reads 0x%08X; the set is queued for the next frame)\n",
         s.ticks(), s.RandomState());

  // Digest after every tick rather than only at the end. `Load` sets skip_physics_frames
  // = 1 (sim/simdll.cpp), so the replay may be running one frame behind the original --
  // and "it diverged" and "it is offset by one frame" are completely different problems
  // with completely different fixes. This tells them apart instead of assuming.
  int matched_at = -1;
  Digests replayed, replayed_at_span;
  // Which components disagree, tick by tick, around the one that should have matched. "It
  // did not reproduce" and "it reproduced everything except the flow texture" are different
  // answers, and the whole-digest equality above cannot tell them apart.
  std::vector<std::string> deltas(static_cast<size_t>(span) + 3);
  for (int i = 0; i < span + 3; ++i) {
    if (!s.Tick(dt)) {
      out::Printf("  FAIL -- tick %d refused after restore\n", i);
      return 1;
    }
    replayed = TakeDigests(s);
    deltas[static_cast<size_t>(i)] = DigestDelta(replayed, first);
    if (i + 1 == span) replayed_at_span = replayed;
    if (replayed == first && matched_at < 0) matched_at = i + 1;
  }
  {
    const int lo = span - 2 < 1 ? 1 : span - 2;
    out::Printf("  what differs from the original, tick by tick:\n");
    for (int k = lo; k <= span + 3; ++k) {
      const std::string& delta = deltas[static_cast<size_t>(k - 1)];
      out::Printf("    after %2d replayed ticks:%s\n", k,
             delta.empty() ? " nothing -- identical" : delta.c_str());
    }
  }
  const bool matched = (matched_at == span);
  out::Printf("  replayed  after %d ticks: elem %016llx mass %016llx temp %016llx rng 0x%08X\n",
         s.ticks(), (unsigned long long)replayed.element,
         (unsigned long long)replayed.mass, (unsigned long long)replayed.temperature,
         s.RandomState());
  if (matched_at < 0) {
    out::Printf("  no tick count from 1 to %d reproduced the original state\n", span + 3);
  } else {
    out::Printf("  the original %d-tick state was reproduced after %d replayed ticks%s\n",
           span, matched_at,
           matched_at == span ? "" : " -- the replay is running behind by that difference");
  }
  out::Printf("  mass drift %.6f kg\n", replayed.total_mass - first.total_mass);
  out::Printf("  a restored run %s the pristine run it was captured from\n",
         matched ? "REPRODUCES" : "does NOT reproduce");

  // ONE more restore of the IDENTICAL blob at the IDENTICAL stream position. Both
  // restores load the same bytes and set the same rng, so anything they disagree about
  // is state that survives an in-place Load.
  //
  // This is the claim the tool's exit code is set from, because it is the one that is
  // achievable and therefore the one worth regressing against. See the verdict below.
  //
  // The comment that used to sit here said restore B comes back with ~101.86 M kg MORE than
  // restore A and that a third Load takes the process down. That is real and it is still
  // reproducible -- but only on the BARE Load path, which `Session::Restore` has not used
  // since it started sending an AllocateCells first: `--load-stress --bare-load` gives
  // 299,708,733.180 kg, then 401,568,241.734 on the second load, then exit code 5 on the
  // third. With the Alloc, six load-tick-load cycles come back mass-identical to the
  // kilogram.
  if (!s.Restore(cp)) {
    out::Printf("  second restore refused\n");
    return 1;
  }
  for (int i = 0; i < span; ++i) {
    if (!s.Tick(dt)) break;
  }
  const Digests again = TakeDigests(s);
  const bool restores_agree = (again == replayed_at_span);
  out::Printf("  a second restore of the same blob at the same rng:\n");
  out::Printf("    restore A: elem %016llx  total mass %.3f kg\n",
         (unsigned long long)replayed_at_span.element, replayed_at_span.total_mass);
  out::Printf("    restore B: elem %016llx  total mass %.3f kg\n",
         (unsigned long long)again.element, again.total_mass);
  out::Printf("    %s\n",
         restores_agree
             ? "they agree -- two restores are at least self-consistent"
             : "they DISAGREE -- so state outside both the blob and the rng decides the "
               "result");

  const std::string ab = DigestDelta(again, replayed_at_span);
  if (!restores_agree) out::Printf("    they differ in:%s\n", ab.c_str());

  // VERDICT, written against a run rather than against an older reading. Two separate claims live here and only one of them is achievable:
  //
  //   1. A restored run does NOT reproduce the pristine run it was captured from, and it
  //      never will, for a reason that is structural rather than a defect. The first thing
  //      that differs is `building` -- the properties, insulation and strengthInfo arrays --
  //      with mass and temperature still identical; those three are written by the GAME
  //      through messages and are carried by no blob, so `AllocateCells` leaves its own
  //      defaults there. One tick later the conduction that reads them puts `temperature`
  //      out, and a tick after that everything is out. Measured with `--ticks 5
  //      --checkpoint-test 1` and `2`, which print the component list tick by tick.
  //
  //      This is the restore constraint seen from the other side, and it is why
  //      `Timeline::Start` captures tick 0 and restores it before displaying anything: with
  //      both sides restored, the comparison is between like and like, and then every tick
  //      reproduces (--timeline-test, 40 seeks, all fifteen digest components).
  //
  //   2. Two restores of the same blob at the same stream position DO agree, and that is a
  //      real regression check -- it is the property the whole scrub timeline rests on.
  //
  // The exit code follows claim 2. It used to follow claim 1, which meant the tool reported
  // FAIL forever for a reason no change could fix, and a check that cannot pass is not a
  // check.
  out::Printf("\n  VERDICT\n"
         "    a restored run vs the PRISTINE run it came from: %s\n"
         "      %s\n"
         "    two restores of the same blob against each other: %s\n"
         "      %s\n",
         matched ? "reproduces" : "does not reproduce, and cannot",
         matched ? "unexpected -- this world has nothing a restore drops"
                 : "the game-owned arrays (properties, insulation, strengthInfo) are carried "
                   "by no blob,\n      so Allocate's defaults win and conduction diverges "
                   "from the next tick.",
         restores_agree ? "agree" : "DISAGREE",
         restores_agree
             ? "which is the property the scrub timeline rests on -- see --timeline-test"
             : "state outside the blob, the stream and the four ext components decides the "
               "result");
  out::Printf("    %s\n", restores_agree ? "PASS" : "FAIL");
  return restores_agree ? 0 : 1;
}

// Minimal repro for the crash the checkpoint work found: capture ONE blob, then Load it
// back repeatedly, ticking `ticks_between` times after each. Nothing else varies -- the
// same bytes go in every time.
//
// Measured on the reference corpus: the first and second Load are fine and their frames
// tick normally; the tick after the THIRD Load takes the process down (exit 5). Since
// the payload is identical each time, the fault is in state the sim carries across an
// in-place Load rather than in the blob.
int LoadStress(Session& s, int loads, int ticks_between, float dt,
               bool realloc_first) {
  out::Printf("\nload stress: one captured blob, re-Loaded %d times, %d tick(s) after each,\n"
         "  %s AllocateCells before each Load\n",
         loads, ticks_between, realloc_first ? "WITH" : "without");
  const Session::Checkpoint cp = s.Capture();
  if (!cp.valid()) {
    out::Printf("  FAIL -- SIM_BeginSave returned nothing\n");
    return 1;
  }
  out::Printf("  blob %zu bytes, captured at tick %d\n", cp.bytes(), cp.ticks);
  for (int i = 0; i < loads; ++i) {
    out::Printf("  load %d ... ", i + 1);
    if (!s.Restore(cp, realloc_first)) {
      out::Printf("REFUSED (the sim returned null)\n");
      return 1;
    }
    out::Printf("accepted, ticking ... ");
    for (int t = 0; t < ticks_between; ++t) {
      if (!s.Tick(dt)) {
        out::Printf("tick %d refused\n", t);
        return 1;
      }
    }
    {
      const Digests d = TakeDigests(s);
      out::Printf("ok (total mass %.3f kg", d.total_mass);
      // The mixture layer is in the blob's own gas section (kSaveVersionGasMixture) but in
      // none of the published arrays, so a load that drops it would leave the line above
      // unchanged. Reported here for the same reason it is in the digest.
      if (s.has_mixture()) out::Printf(", mixture %.6f kg", d.mixture_mass);
      out::Printf(")\n");
    }
  }
  out::Printf("  survived %d loads\n", loads);
  return 0;
}

// How much of each property texture is actually non-zero, counted rather than assumed.
//
// A hash over a buffer of zeros is a hash of nothing, and three of these can legitimately be
// all zeros for reasons that are not faults: `exposedToSunlight` is per WORLD and stays zero
// until a DefineWorldOffsets message says where the worlds are (sim/textures.h); the liquid
// triple is zero for every cell that holds no liquid, which on a dry world is all of them;
// and `accumulatedFlow` is allocated and published by oni-sim-replacement but never written, so
// its digest is a constant until something starts filling it. Reporting the counts is what
// separates "this reproduced" from "there was nothing here to reproduce" -- the same
// principle as the sleeping-cell count in the timeline test.
struct TextureCoverage {
  bool have_flow = false, have_liquid = false, have_liquid_data = false;
  bool have_material = false, have_sun = false, have_accum = false;
  int flow = 0, liquid = 0, liquid_data = 0, material = 0, sun = 0, accum = 0;
};

TextureCoverage MeasureTextures(const Session& s) {
  TextureCoverage c;
  const oni_sim::GameDataUpdate* g = s.frame();
  if (!g) return c;
  const size_t n = static_cast<size_t>(s.cells());
  if (g->propertyTextureFlow) {
    c.have_flow = true;
    const float* f = static_cast<const float*>(g->propertyTextureFlow);
    for (size_t i = 0; i < n; ++i) {
      if (f[i * 2] != 0.0f || f[i * 2 + 1] != 0.0f) ++c.flow;
    }
  }
  if (g->accumulatedFlow) {
    c.have_accum = true;
    for (size_t i = 0; i < n; ++i) {
      if (g->accumulatedFlow[i] != 0.0f) ++c.accum;
    }
  }
  auto count_rgba = [n](const void* p, int* out) {
    const uint32_t* w = static_cast<const uint32_t*>(p);
    for (size_t i = 0; i < n; ++i) {
      if (w[i] != 0u) ++*out;
    }
  };
  if (g->propertyTextureLiquid) {
    c.have_liquid = true;
    count_rgba(g->propertyTextureLiquid, &c.liquid);
  }
  if (g->propertyTextureLiquidData) {
    c.have_liquid_data = true;
    count_rgba(g->propertyTextureLiquidData, &c.liquid_data);
  }
  if (g->propertyTextureMaterialData) {
    c.have_material = true;
    count_rgba(g->propertyTextureMaterialData, &c.material);
  }
  if (g->propertyTextureExposedToSunlight) {
    c.have_sun = true;
    const uint8_t* b = static_cast<const uint8_t*>(g->propertyTextureExposedToSunlight);
    for (size_t i = 0; i < n; ++i) {
      if (b[i] != 0) ++c.sun;
    }
  }
  return c;
}

// One line per texture: whether the DLL published it at all, and how many of the world's
// cells it holds anything in.
void PrintTextureCoverage(const Session& s, const char* indent) {
  const TextureCoverage c = MeasureTextures(s);
  const int n = s.cells();
  // Attached, an absent texture has two possible causes and they are not the same
  // finding: the game may not publish it, or this poll may not have asked for it. The
  // line says "not in this frame" rather than claiming the first, because claiming the
  // first when the second is true is how a fetch-set bug gets recorded as a sim fact.
  const char* absent =
      s.attached() ? "not in this frame" : "not published by this DLL";
  auto line = [&](const char* name, bool have, int cells) {
    if (!have) {
      out::Printf("%s%-18s %s\n", indent, name, absent);
      return;
    }
    out::Printf("%s%-18s %7d of %d cells non-zero%s\n", indent, name, cells, n,
           cells == 0 ? "  -- nothing here to reproduce" : "");
  };
  line("flow", c.have_flow, c.flow);
  line("accumulatedFlow", c.have_accum, c.accum);
  line("liquid", c.have_liquid, c.liquid);
  line("liquidData", c.have_liquid_data, c.liquid_data);
  line("materialData", c.have_material, c.material);
  line("exposedToSunlight", c.have_sun, c.sun);
}

// FNV-1a over each published array separately. Separates "the sim produced different
// state" from "the renderer drew the same state differently", which a PNG hash cannot:
// a PNG differs if either changes.
void Digest(const Session& s) {
  const Digests d = TakeDigests(s);
  out::Printf("digest after %d %s:\n", s.ticks(), s.attached() ? "polls" : "ticks");
  out::Printf("  elementIdx  %016llx\n", (unsigned long long)d.element);
  out::Printf("  mass        %016llx\n", (unsigned long long)d.mass);
  out::Printf("  temperature %016llx\n", (unsigned long long)d.temperature);
  out::Printf("  radiation   %016llx\n", (unsigned long long)d.radiation);
  out::Printf("  disease     %016llx\n", (unsigned long long)d.disease);
  out::Printf("  backwall    %016llx\n", (unsigned long long)d.backwall);
  out::Printf("  building    %016llx\n", (unsigned long long)d.building);
  out::Printf("  registry    %016llx\n", (unsigned long long)d.registry);
  out::Printf("  extCells    %016llx\n", (unsigned long long)d.ext_cells);
  out::Printf("  flowTex     %016llx\n", (unsigned long long)d.flow);
  out::Printf("  liquidTex   %016llx\n", (unsigned long long)d.liquid);
  out::Printf("  sunlightTex %016llx\n", (unsigned long long)d.sunlight);
  out::Printf("  total mass  %.6f kg\n", d.total_mass);
  if (s.has_mixture()) {
    out::Printf("  mixture     %016llx  (%.6f kg over the seeded window)\n",
           (unsigned long long)d.mixture, d.mixture_mass);
  }
  // A hash of a buffer of zeros is a hash of nothing, so the three texture lines above are
  // qualified by what the textures actually hold.
  out::Printf("  property textures:\n");
  PrintTextureCoverage(s, "    ");
  // The scheduling counters beside them. `displace_rotation` counts SUBSTEPS, so it is the
  // one reading that says how much physics has actually run behind a given publication --
  // which is not the tick number, and cannot be inferred from the digests.
  oni_sim::ext::SetSchedulingStateMessage m{};
  if (s.SchedulingState(&m)) {
    out::Printf("  scheduling: skip %d  pressure_dir %d  shuffle_dir %d  carry %.9g  "
           "rotation %u  first_substep %u  vf %u\n",
           m.skip_physics_frames, m.pressure_dir, m.shuffle_dir,
           static_cast<double>(m.substep_carry), (unsigned)m.displace_rotation,
           (unsigned)m.first_physics_substep, (unsigned)m.vf_active);
  }
}

// One row of the digest log.
//
// This mapping is the only join between three lists that have to agree: `Digests`' fields,
// `DigestDelta`'s names, and `digestlog::kColumns`. Written out by hand and in that order
// rather than generated, because the one failure mode worth designing against is a
// component that exists in the struct, is hashed every tick, and is never written to the
// log -- which would make `--bisect` report agreement about something it never looked at.
void LogDigestRow(FILE* f, const Session& s) {
  const Digests d = TakeDigests(s);
  digestlog::Row r;
  r.tick = s.ticks();
  r.h[0] = d.element;
  r.h[1] = d.mass;
  r.h[2] = d.temperature;
  r.h[3] = d.radiation;
  r.h[4] = d.disease;
  r.h[5] = d.backwall;
  r.h[6] = d.mixture;
  r.h[7] = d.building;
  r.h[8] = d.registry;
  r.h[9] = d.ext_cells;
  r.h[10] = d.flow;
  r.h[11] = d.liquid;
  r.h[12] = d.sunlight;
  r.total_mass = d.total_mass;
  r.mixture_mass = d.mixture_mass;
  digestlog::WriteRow(f, r);
}

// Every published array to a file, for diffing two runs against each other. A digest says
// THAT two runs differ; this says WHERE, which is the only form of the answer that leads
// anywhere.
//
// It used to write three arrays -- elementIdx, mass and temperature -- under a comment that
// claimed it wrote them all. The claim is now true, and the format is self-describing so it
// can stay true: see statedump.h. The two blobs the sim serializes on request go in beside
// the published arrays, because a divergence in the extension registry is exactly the class
// that was found the hard way, surfacing several ticks later in an array that had nothing
// to do with it.
bool DumpState(const Session& s, const char* path, const std::string& source) {
  const oni_sim::GameDataUpdate* g = s.frame();
  if (g == nullptr) return false;
  const int32_t n = static_cast<int32_t>(s.cells());
  statedump::Dump d;
  d.width = s.width();
  d.height = s.height();
  d.cells = n;
  d.source = source;
  statedump::AddArray(&d, "elementIdx", statedump::kU16, 1, g->elementIdx, n);
  statedump::AddArray(&d, "mass", statedump::kF32, 1, g->mass, n);
  statedump::AddArray(&d, "temperature", statedump::kF32, 1, g->temperature, n);
  statedump::AddArray(&d, "radiation", statedump::kF32, 1, g->radiation, n);
  statedump::AddArray(&d, "diseaseIdx", statedump::kU8, 1, g->diseaseIdx, n);
  statedump::AddArray(&d, "diseaseCount", statedump::kI32, 1, g->diseaseCount, n);
  statedump::AddArray(&d, "backwallElement", statedump::kU16, 1, g->backwallElement, n);
  statedump::AddArray(&d, "backwallMass", statedump::kF32, 1, g->backwallMass, n);
  statedump::AddArray(&d, "backwallTemperature", statedump::kF32, 1, g->backwallTemperature,
                      n);
  statedump::AddArray(&d, "properties", statedump::kU8, 1, g->properties, n);
  statedump::AddArray(&d, "insulation", statedump::kU8, 1, g->insulation, n);
  statedump::AddArray(&d, "strengthInfo", statedump::kU8, 1, g->strengthInfo, n);
  // The flow texture is two floats a cell, which is why arity is a field of the format
  // rather than an assumption in the reader.
  statedump::AddArray(&d, "propertyTextureFlow", statedump::kF32, 2, g->propertyTextureFlow,
                      n);
  statedump::AddArray(&d, "accumulatedFlow", statedump::kF32, 1, g->accumulatedFlow, n);
  statedump::AddArray(&d, "propertyTextureLiquid", statedump::kU32, 1,
                      g->propertyTextureLiquid, n);
  statedump::AddArray(&d, "propertyTextureLiquidData", statedump::kU32, 1,
                      g->propertyTextureLiquidData, n);
  statedump::AddArray(&d, "propertyTextureMaterialData", statedump::kU32, 1,
                      g->propertyTextureMaterialData, n);
  statedump::AddArray(&d, "propertyTextureExposedToSunlight", statedump::kU8, 1,
                      g->propertyTextureExposedToSunlight, n);
  d.ext_blob = s.ExtCellStateBlob();
  d.registry_blob = s.RegistryState();
  std::string err;
  if (!statedump::Write(d, path, &err)) {
    out::Printf("%s\n", err.c_str());
    return false;
  }
  size_t bytes = 0;
  for (const statedump::Array& a : d.arrays) bytes += a.bytes.size();
  out::Printf("wrote %s: %zu arrays, %.2f MB, ext %zu B, registry %zu B\n", path, d.arrays.size(),
         bytes / (1024.0 * 1024.0), d.ext_blob.size(), d.registry_blob.size());
  return true;
}

// Two digest logs, and the first tick they stopped agreeing on.
//
// No simulation runs here and none is needed, which is the whole point: the two sides are
// usually two processes, and may be two machines. Everything this needs is in the files.
int Bisect(const char* path_a, const char* path_b) {
  digestlog::Log a, b;
  std::string err;
  if (!digestlog::Read(path_a, &a, &err)) {
    out::Printf("%s\n", err.c_str());
    return 2;
  }
  if (!digestlog::Read(path_b, &b, &err)) {
    out::Printf("%s\n", err.c_str());
    return 2;
  }
  out::Printf("bisect\n");
  out::Printf("  a  %s -- %zu ticks, %s\n", a.path.c_str(), a.rows.size(), a.source.c_str());
  out::Printf("     corpus %s, seed %s\n", a.corpus.c_str(), a.seed.c_str());
  out::Printf("  b  %s -- %zu ticks, %s\n", b.path.c_str(), b.rows.size(), b.source.c_str());
  out::Printf("     corpus %s, seed %s\n", b.corpus.c_str(), b.seed.c_str());

  const digestlog::Verdict v = digestlog::Compare(a, b);
  if (!v.comparable) {
    out::Printf("\nrefused: %s\n", v.refusal.c_str());
    return 2;
  }
  for (const std::string& w : v.warnings) out::Printf("\nWARNING: %s\n", w.c_str());

  out::Printf("\ncompared %d ticks (%d..%d) over %d components\n", v.overlap, v.first_tick,
         v.last_tick, digestlog::kColumnCount);
  if (!v.any) {
    // Said as a span rather than as a verdict. "They agree" is only worth as much as the
    // number of ticks behind it, and a two-tick agreement has been mistaken for a passing
    // test before, where a short range hid a slow divergence.
    out::Printf("no divergence: every component agrees at every one of those %d ticks\n",
           v.overlap);
    return 0;
  }
  out::Printf("FIRST DIVERGENCE: tick %d\n\n", v.divergence_tick);
  for (const digestlog::ColumnVerdict& c : v.columns) {
    if (!c.differs) {
      out::Printf("  %-12s identical through %d\n", c.name, v.last_tick);
      continue;
    }
    out::Printf("  %-12s from tick %d", c.name, c.first);
    if (c.heals) {
      out::Printf(", healed and parted again, %d of %d ticks differ", c.ticks_differing,
             v.overlap);
    } else if (c.persists) {
      out::Printf(", persists to %d", v.last_tick);
    } else {
      out::Printf(", last differs at %d then agrees again", c.last);
    }
    out::Printf("\n");
  }
  // Matter appearing and matter moving are different bugs and the totals separate them at
  // no cost, so they are printed at the tick that matters rather than left to a second run.
  out::Printf("\n  total mass at tick %d: %.6f kg and %.6f kg (delta %.9g kg)\n",
         v.divergence_tick, v.mass_a, v.mass_b, v.mass_a - v.mass_b);
  // Which components part AT the first tick, named. The distinction earns its line: a run
  // whose first divergence is `extCells` at tick 0 has diverged in its INPUT -- somebody
  // passed a different --ext-probe -- and the interesting tick is the later one where an
  // array the sim computes first disagrees. Sending someone to diff tick 0 without saying
  // that is sending them to look at what they typed.
  std::string at_first;
  for (const digestlog::ColumnVerdict& c : v.columns) {
    if (!c.differs || c.first != v.divergence_tick) continue;
    at_first += " ";
    at_first += c.name;
  }
  out::Printf("\n  parting at tick %d:%s\n", v.divergence_tick, at_first.c_str());
  out::Printf(
      "\nwhere, not just when: run each side again to tick %d with --dump-state, then\n"
      "  simviz.exe --diff-state <a.bin> <b.bin> --snapshot <diff.png>\n",
      v.divergence_tick);
  return 1;
}

// One array's line in the --diff-state report. Kept in one place because the report prints
// grid arrays, ungrouped arrays and a summary from it, and three formats for one fact is
// how a report starts contradicting itself.
void PrintArrayDiff(const statedump::ArrayDiff& d, int width) {
  if (!d.comparable) {
    out::Printf("  %-32s %s\n", d.name.c_str(), d.note.c_str());
    return;
  }
  if (d.wrong_values == 0) {
    out::Printf("  %-32s identical (%zu values)\n", d.name.c_str(), d.values);
    return;
  }
  const int x = width > 0 ? d.first_cell % width : -1;
  const int y = width > 0 ? d.first_cell / width : -1;
  out::Printf("  %-32s %zu cells differ, first cell %d (%d,%d)", d.name.c_str(), d.wrong_cells,
         d.first_cell, x, y);
  if (d.arity > 1) out::Printf(" component %d", d.first_comp);
  out::Printf(": %.9g against %.9g", d.first_a, d.first_b);
  if (statedump::IsScalar(d.type)) {
    const int wx = width > 0 ? d.worst_cell % width : -1;
    const int wy = width > 0 ? d.worst_cell / width : -1;
    out::Printf(", worst |delta| %.9g at %d (%d,%d)", d.worst, d.worst_cell, wx, wy);
  }
  out::Printf("\n");
}

// Two state dumps: which cells differ, in what, and by how much -- and a picture of it.
int DiffState(const char* path_a, const char* path_b, const std::string& diff_view,
              const char* snapshot, bool cropped, int crop_x, int crop_y, int crop_w,
              int crop_h, int zoom) {
  statedump::Dump a, b;
  std::string err;
  if (!statedump::Read(path_a, &a, &err)) {
    out::Printf("%s\n", err.c_str());
    return 2;
  }
  if (!statedump::Read(path_b, &b, &err)) {
    out::Printf("%s\n", err.c_str());
    return 2;
  }
  out::Printf("diff-state\n");
  out::Printf("  a  %s -- v%u, %dx%d, %zu arrays, %s\n", a.path.c_str(), a.version, a.width,
         a.height, a.arrays.size(), a.source.c_str());
  out::Printf("  b  %s -- v%u, %dx%d, %zu arrays, %s\n", b.path.c_str(), b.version, b.width,
         b.height, b.arrays.size(), b.source.c_str());
  if (a.width != b.width || a.height != b.height || a.cells != b.cells) {
    out::Printf("\nrefused: these dumps are of different worlds\n");
    return 2;
  }

  // Grouped by the digest component each array feeds, so this report and --bisect name the
  // same things. A component whose arrays are all absent is reported as absent rather than
  // silently skipped: attached, a surface the fetch did not ask for is missing, and
  // "missing" must never render as "identical".
  out::Printf("\npublished arrays:\n");
  std::vector<std::string> reported;
  std::string differing_components;
  const char* group = "";
  for (int i = 0; i < statedump::kGroupCount; ++i) {
    const statedump::Grouping& g = statedump::kGroups[i];
    if (strcmp(group, g.digest_column) != 0) {
      group = g.digest_column;
      out::Printf("\n %s\n", group);
    }
    const statedump::ArrayDiff d = statedump::DiffArray(a, b, g.array);
    reported.push_back(g.array);
    PrintArrayDiff(d, a.width);
    if (d.comparable && d.wrong_values > 0 &&
        differing_components.find(std::string(" ") + group) == std::string::npos) {
      differing_components += " ";
      differing_components += group;
    }
  }
  // Anything either dump carries that this build has no grouping for. Named rather than
  // dropped: a dump written by a later build carrying a new array would otherwise be
  // reported on in full except for the one array that is new, which is the array most
  // likely to be the reason someone is reading this.
  bool any_ungrouped = false;
  for (int side = 0; side < 2; ++side) {
    const statedump::Dump& d = side == 0 ? a : b;
    for (const statedump::Array& arr : d.arrays) {
      bool seen = false;
      for (const std::string& n : reported) {
        if (n == arr.name) seen = true;
      }
      if (seen) continue;
      reported.push_back(arr.name);
      if (!any_ungrouped) {
        out::Printf("\n arrays this build has no digest grouping for\n");
        any_ungrouped = true;
      }
      const statedump::ArrayDiff ad = statedump::DiffArray(a, b, arr.name);
      PrintArrayDiff(ad, a.width);
      if (ad.comparable && ad.wrong_values > 0) {
        differing_components += " ";
        differing_components += arr.name;
      }
    }
  }

  // The extension registry, on the padded grid, decoded by the same reader the views use.
  out::Printf("\nextension properties (padded grid, %d x %d):\n", a.width + 2, a.height + 2);
  if (a.ext_blob.empty() && b.ext_blob.empty()) {
    out::Printf("  neither dump carries an extension blob\n");
  } else {
    extprops::Table ta, tb;
    extprops::Decode(a.ext_blob, a.width, a.height, /*all_classes=*/true, &ta);
    extprops::Decode(b.ext_blob, b.width, b.height, /*all_classes=*/true, &tb);
    if (!ta.decoded) out::Printf("  a: %s\n", ta.error.c_str());
    if (!tb.decoded) out::Printf("  b: %s\n", tb.error.c_str());
    if (ta.decoded && tb.decoded) {
      const std::vector<statedump::ExtDiff> diffs = statedump::DiffExt(ta, tb);
      if (diffs.empty()) out::Printf("  both dumps carry no properties\n");
      for (const statedump::ExtDiff& d : diffs) {
        if (!d.comparable) {
          out::Printf("  %-32s %s\n", d.name.c_str(), d.note.c_str());
          differing_components += " extCells";
          continue;
        }
        if (d.wrong_cells == 0) {
          out::Printf("  %-32s identical (%zu padded cells)\n", d.name.c_str(), d.cells);
          continue;
        }
        // Padded index, and said so. A reader who takes this for a game cell reads a
        // neighbour's value and gets a plausible wrong number rather than an error, which
        // is the pitfall this whole format documents twice already.
        out::Printf("  %-32s %zu padded cells differ, first %d component %d: %.9g against %.9g, "
               "worst |delta| %.9g\n",
               d.name.c_str(), d.wrong_cells, d.first_cell, d.first_comp, d.first_a,
               d.first_b, d.worst);
        if (differing_components.find(" extCells") == std::string::npos) {
          differing_components += " extCells";
        }
      }
    }
  }

  // The component registries, compared as bytes. Opaque on purpose: this file has no
  // decoder for that blob, and inventing one here would be a second thing that can be wrong
  // about a format the sim already owns.
  out::Printf("\ncomponent registries (SIM_DebugRegistryState, compared as bytes):\n");
  if (a.registry_blob.empty() && b.registry_blob.empty()) {
    out::Printf("  neither dump carries a registry blob\n");
  } else if (a.registry_blob.size() != b.registry_blob.size()) {
    out::Printf("  sizes differ: %zu and %zu bytes\n", a.registry_blob.size(),
           b.registry_blob.size());
    differing_components += " registry";
  } else if (a.registry_blob != b.registry_blob) {
    size_t first = 0;
    while (first < a.registry_blob.size() && a.registry_blob[first] == b.registry_blob[first]) {
      ++first;
    }
    out::Printf("  %zu bytes, first difference at byte %zu\n", a.registry_blob.size(), first);
    differing_components += " registry";
  } else {
    out::Printf("  identical (%zu bytes)\n", a.registry_blob.size());
  }

  if (differing_components.empty()) {
    out::Printf("\nno divergence: every array and blob in both dumps is identical\n");
  } else {
    out::Printf("\ndigest components that differ:%s\n", differing_components.c_str());
  }

  if (snapshot != nullptr) {
    // Which array to draw. An explicit --diff-view wins; otherwise the first differing one
    // in the grouped order, which puts elementIdx and mass ahead of the textures derived
    // from them.
    std::string draw = diff_view;
    if (draw.empty()) {
      for (int i = 0; i < statedump::kGroupCount && draw.empty(); ++i) {
        const statedump::ArrayDiff d =
            statedump::DiffArray(a, b, statedump::kGroups[i].array);
        if (d.comparable && d.wrong_values > 0) draw = statedump::kGroups[i].array;
      }
    }
    if (draw.empty()) {
      out::Printf("\nnothing to draw: no array differs. Pass --diff-view <array> to render one "
             "anyway.\n");
      return differing_components.empty() ? 0 : 1;
    }
    Image img;
    if (!statedump::RenderDiff(a, b, draw, &img, &err)) {
      out::Printf("\n%s\n", err.c_str());
      return 2;
    }
    if (cropped) img = img.Cropped(crop_x, crop_y, crop_w, crop_h);
    if (zoom > 1) img = img.Magnified(zoom);
    if (!WritePng(img, snapshot)) {
      out::Printf("\ncould not write %s\n", snapshot);
      return 2;
    }
    out::Printf("\nwrote %s (%dx%d, divergence in %s over the first dump's mass, dim)\n", snapshot,
           img.width(), img.height(), draw.c_str());
  }
  return differing_components.empty() ? 0 : 1;
}

// ------------------------------------------------------------------ the window

double NowSeconds() {
  static LARGE_INTEGER freq = {};
  if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return static_cast<double>(t.QuadPart) / freq.QuadPart;
}

// A cell can be shrunk to a twentieth of a pixel or blown up to fill 64, which covers
// "the whole cluster at once" and "read one cell's neighbours" without a mode switch.
constexpr float kMinScale = 0.05f;
constexpr float kMaxScale = 64.0f;

// Does the scrub timeline actually reproduce the run it is scrubbing? A checkpoint
// round trip (--checkpoint-test) proves one restore reproduces; this proves the whole
// structure does -- sparse captures, thinning, and replay from the nearest one -- by
// keeping a digest of every tick of a straight run and then requiring each seek to land
// on the digest that tick had.
//
// The offset scan matters as much as the pass/fail. `Load` sets skip_physics_frames = 1
// (sim/simdll.cpp), so a restore may cost a frame of replay, and "the timeline is off by
// one tick" and "the timeline is wrong" look identical from a single mismatch. If a
// seek matches ref[target + k] for some small k, that k is the settle cost and it is a
// constant to set, not a bug to chase.
int TimelineTest(Session& s, int span, float dt, int interval, int keep, size_t ext_before) {
  out::Printf("\ntimeline: %d ticks, checkpoint every %d, keep %d\n", span, interval, keep);

  Timeline tl;
  tl.Configure(interval, keep, dt);
  std::string err;
  if (!tl.Start(s, &err)) {
    out::Printf("  FAIL -- %s\n", err.c_str());
    return 1;
  }
  // Two kinds of tick number meet here, and conflating them WAS the --ticks bug: `i` and
  // `target` index the reference run from its own start, while the session and the
  // timeline count absolute session ticks, which begin at `base` rather than at 0 when
  // `--ticks N` ran frames before the timeline was started. Everything indexed into `ref`
  // is relative; everything handed to the timeline or printed as a tick is absolute.
  const int base = tl.base();
  if (base > 0) {
    out::Printf("  the timeline is anchored at tick %d (%d ticks ran before it), so this "
           "sweep covers %d..%d\n", base, base, base, base + span);
  }

  std::vector<Digests> ref(static_cast<size_t>(span) + 1);
  std::vector<uint32_t> ref_rng(static_cast<size_t>(span) + 1);
  // How many cells the flow texture held at each tick of the run. Needed to classify a
  // flow-only difference at an exact stop: see the FLOW PROVENANCE note below, which turns
  // on the seek publishing an EMPTY flow texture where the run published a full one.
  std::vector<int> ref_flow(static_cast<size_t>(span) + 1, 0);
  ref[0] = TakeDigests(s);
  ref_rng[0] = s.RandomState();
  ref_flow[0] = MeasureTextures(s).flow;
  // The mixing layer's sleep bits, watched across the reference run. A restored world forgets
  // every one of them (Allocate resets the array and every Load goes through one), so a sweep
  // that never put a single cell to sleep has not actually tested whether forgetting them
  // matters -- it has only tested a world where there was nothing to forget. Reported rather
  // than assumed, on the same principle as the carries_schedule/carries_countdowns line below.
  int slept_peak = -1, slept_end = -1;
  for (int i = 1; i <= span; ++i) {
    if (!s.Tick(dt)) {
      out::Printf("  FAIL -- tick %d refused during the reference run\n", i);
      return 1;
    }
    tl.Note(s);
    ref[static_cast<size_t>(i)] = TakeDigests(s);
    ref_rng[static_cast<size_t>(i)] = s.RandomState();
    ref_flow[static_cast<size_t>(i)] = MeasureTextures(s).flow;
    if (s.has_mixture()) {
      slept_end = s.SleepingCount();
      if (slept_end > slept_peak) slept_peak = slept_end;
    }
  }
  // THE SEVENTH COMPONENT'S FIRE-ON-PURPOSE CHECK. `Timeline::Start` captures tick 0 and
  // IMMEDIATELY RESTORES it, deliberately, so that every later seek lands on the same run.
  // That anchor restore is also the first thing a missing carrier destroys -- and it destroys
  // it in the DISPLAYED RUN, not only in a replay, so every checkpoint taken afterwards and
  // every reference digest above agree with each other about a world the probe never reached.
  // The sweep below then passes for the wrong reason.
  //
  // Measured rather than assumed: with `Session::Restore`'s SetExtCellState commented out,
  // --ext-probe wrote 1600 cells, the component read 1,036,192 bytes, and the sweep still
  // reported 40 of 40 seeks reproducing bit for bit. This comparison is what turns that into
  // a failure.
  //
  // Read HERE rather than straight after `tl.Start`, and that is not cosmetic: every extension
  // message is queued and the rotation costs two frames, so the restore the anchor performs
  // has not landed yet at that point. The reference run above has ticked, so it has.
  if (ext_before > 12) {
    const size_t ext_after = s.ExtCellState().size();
    if (ext_after != ext_before) {
      out::Printf("  FAIL -- the extension checkpoint component was %zu bytes before the timeline\n"
             "         anchor and is %zu after it. The anchor captures tick 0 and restores\n"
             "         it, so a restore that does not carry this component silently rewrites\n"
             "         the run rather than diverging from it, and every check below would\n"
             "         then pass against a world the probe never reached.\n",
             ext_before, ext_after);
      return 1;
    }
    out::Printf("  the extension checkpoint component survived the anchor restore (%zu bytes)\n",
           ext_after);
  }

  // The reference digest is printed so two runs with different capture schedules can be
  // compared: if taking a checkpoint perturbed the sim, this line would move with the
  // interval, and every conclusion drawn from the sweep below would be about that instead.
  out::Printf("  reference tick %d: elem %016llx mass %016llx temp %016llx  total %.3f\n",
         base + span,
         (unsigned long long)ref[static_cast<size_t>(span)].element,
         (unsigned long long)ref[static_cast<size_t>(span)].mass,
         (unsigned long long)ref[static_cast<size_t>(span)].temperature,
         ref[static_cast<size_t>(span)].total_mass);
  out::Printf("  reference run to tick %d: %zu checkpoints every %d ticks, %.1f MB, %d thins\n",
         tl.head(), tl.count(), tl.interval(), tl.bytes() / (1024.0 * 1024.0),
         tl.thins());
  if (s.has_mixture()) {
    if (slept_peak < 0) {
      out::Printf("  mixture: %.6f kg seeded, but this DLL has no SIM_DebugGasSleeping, so\n"
             "  whether any cell ever slept is unknown and the sweep below cannot claim it\n",
             ref[static_cast<size_t>(span)].mixture_mass);
    } else {
      out::Printf("  mixture: %.6f kg at tick 0, %.6f at tick %d, %d cells asleep at the end,"
             " %d at the peak\n",
             ref[0].mixture_mass, ref[static_cast<size_t>(span)].mixture_mass, span,
             slept_end, slept_peak);
    }
  }
  // What the property textures actually carried at the end of the run. A digest over a
  // buffer of zeros reproduces trivially, so a PASS that includes them is worth only what
  // they held.
  out::Printf("  property textures at tick %d:\n", base + span);
  PrintTextureCoverage(s, "    ");
  {
    const std::vector<int> marks = tl.checkpoint_ticks();
    out::Printf("  checkpoints at:");
    for (int t : marks) out::Printf(" %d", t);
    out::Printf("\n");
  }

  // A descending sweep: every tick from the end down to zero, each one a backwards seek
  // and therefore a restore. The two kinds of landing are counted separately, because
  // they answer different questions. An EXACT STOP -- one frame past a checkpoint -- is
  // the timeline's contract and has to reproduce the run bit for bit. A REPLAYED tick
  // runs real physics frames after the restore, and those do not reproduce, because
  // AllocateCells resets SimData state the save blob does not carry. The
  // divergence is reported with its size rather than hidden: it is what a fix in
  // oni-sim-replacement would have to close.
  int exact_seeks = 0, exact_bad = 0;
  int replay_seeks = 0, replay_bad = 0;
  int flow_provenance = 0;
  double worst_mass_delta = 0.0;
  double seek_ms_total = 0.0;
  double seek_ms_worst = 0.0;

  for (int target = span - 1; target >= 0; --target) {
    const int tick = base + target;  // the same instant, in the timeline's numbering
    const int from = s.ticks();
    const bool exact = tl.exact_stop(tick);
    const double t0 = NowSeconds();
    if (!tl.Seek(s, tick, &err)) {
      out::Printf("  FAIL -- seek %d -> %d: %s\n", from, tick, err.c_str());
      return 1;
    }
    const double ms = (NowSeconds() - t0) * 1000.0;
    seek_ms_total += ms;
    if (ms > seek_ms_worst) seek_ms_worst = ms;

    const Digests d = TakeDigests(s);
    const bool ok = (d == ref[static_cast<size_t>(target)]);
    const double delta = d.total_mass - ref[static_cast<size_t>(target)].total_mass;
    if (exact) ++exact_seeks; else ++replay_seeks;
    if (ok) continue;
    if (exact) {
      const Digests& r = ref[static_cast<size_t>(target)];
      // FLOW PROVENANCE, and like the anchor case below it is about which frame produced a
      // reading rather than about what the sim did.
      //
      // A `PrepareGameData` publishes the frame BEFORE it -- the sim's own words, at
      // sim/simdll.cpp's `last_published`: "the frame the game observes through a given
      // PrepareGameData is the one before it ... the call publishes what has already
      // finished". The first call after a Load is the exception: it runs its frame on the
      // calling thread and primes the pipeline, and that frame runs no physics, because
      // `Load` sets skip_physics_frames and StepPhysics returns before it touches anything.
      //
      // An exact stop IS that frame. So the seek and the run publish the same WORLD -- every
      // grid array, the registries, the mixture -- and differ in the one published buffer
      // that describes what its producing frame DID rather than what the world holds: the
      // flow texture is a per-frame transfer, zero everywhere nothing moved (sim/textures.h,
      // `FillFlowTexture`), so a frame that moved nothing publishes an empty one.
      //
      // Measured rather than argued, on the corpus at tick 11: the seek's publication is
      // byte-identical to the run's in all 256,944 cells of elementIdx, mass and temperature
      // and in the scheduling counters and the random stream, and its flow texture is empty
      // where the run's holds 47,846 cells. `--timeline-diff 11` prints both sides cell by
      // cell.
      //
      // Deliberately narrow: this excuses ONLY an exact stop whose flow texture is entirely
      // empty against a run tick that had one. A flow texture that differs any other way --
      // wrong values, wrong cells, non-empty on both sides -- is a divergence and fails.
      if (d.flow != r.flow && d.element == r.element && d.mass == r.mass &&
          d.temperature == r.temperature && d.radiation == r.radiation &&
          d.disease == r.disease && d.backwall == r.backwall && d.building == r.building &&
          d.registry == r.registry && d.ext_cells == r.ext_cells &&
          d.mixture == r.mixture && d.liquid == r.liquid &&
          d.sunlight == r.sunlight && MeasureTextures(s).flow == 0 &&
          ref_flow[static_cast<size_t>(target)] > 0) {
        if (flow_provenance == 0) {
          out::Printf("  tick %d: flow texture empty (the run published %d cells), every other "
                 "array\n  identical. The exact stop IS the frame that primes the pipeline "
                 "after a Load, and\n  that frame runs no physics, so it has no transfers to "
                 "publish. Reading provenance,\n  not a divergence -- see the comment at this "
                 "line.\n",
                 tick, ref_flow[static_cast<size_t>(target)]);
        }
        ++flow_provenance;
        continue;
      }
      // The anchor tick is the one landing this test cannot compare, and the cause is
      // where the readings come from rather than anything the sim did wrong.
      //
      // The reference's first tick is the anchor as Start leaves it, with no frame run on it. A
      // SEEK to tick 0 restores the same anchor and runs the frame that publishes a restore,
      // and that frame STEPS -- `--timeline-diff` shows displace_rotation going from the
      // Load's 0 to the captured value plus one substep, because the queued scheduling
      // message overwrites the Load's skip ahead of StepPhysics. Everywhere else that costs
      // nothing, because both sides of a comparison sit at the same point in the same
      // publication lag. At tick 0 they do not, and exactly two components can show it:
      //
      //   `mixture` is read from a LIVE export (SIM_DebugGasMass) rather than from the
      //   published arrays, so it has caught up with that frame when nothing else has.
      //   Measured at 1.5e-9 kg in 479.
      //
      //   `building` -- properties, insulation, strength -- is the one thing the RESTORE
      //   itself changes: AllocateCells drops the building registrations the corpus sent at
      //   boot and nothing re-sends them, so those arrays fall back to Allocate's defaults
      //   (the reason the displayed run has to be a restored one). The
      //   reference's publication still holds the pre-restore values; one frame later the
      //   seek's does not.
      //
      // Neither is a divergence, and neither is silently swallowed: any OTHER component
      // differing at tick 0 falls through to the failure path below. Making the reference run
      // that frame too was tried and is worse -- it moves the whole run one frame off the
      // checkpoint schedule, and 9 of 35 replayed ticks stopped reproducing.
      if (target == 0 && d.element == r.element && d.mass == r.mass &&
          d.temperature == r.temperature && d.radiation == r.radiation &&
          d.disease == r.disease && d.backwall == r.backwall &&
          (d.building != r.building || d.mixture != r.mixture)) {
        out::Printf("  tick %d:%s%s differ, every other array identical.\n"
               "  The seek ran the frame that publishes a restore and the reference did "
               "not.\n"
               "  Reading provenance, not a divergence -- see the comment at this line.\n",
               tick, d.building != r.building ? " building" : "",
               d.mixture != r.mixture ? " mixture" : "");
        continue;
      }
      ++exact_bad;
      out::Printf("  EXACT STOP FAILED at tick %d: mass %.3f vs %.3f  rng 0x%08X vs 0x%08X\n",
             tick, d.total_mass, r.total_mass, s.RandomState(),
             ref_rng[static_cast<size_t>(target)]);
      // Which array actually moved. Four digests can disagree in four different ways and the
      // mass line above shows only one of them -- a mixture-only divergence prints two
      // identical mass figures and looks like a reporting bug rather than a finding.
      if (s.has_mixture()) {
        out::Printf("    mixture %.6f vs %.6f kg\n", d.mixture_mass, r.mixture_mass);
      }
      out::Printf("    differs in:%s\n", DigestDelta(d, r).c_str());
    } else {
      ++replay_bad;
      if (delta > worst_mass_delta || -delta > worst_mass_delta) {
        worst_mass_delta = delta < 0.0 ? -delta : delta;
      }
      if (replay_bad <= 12) {
        int landed = -1;
        for (int j = 0; j <= span; ++j) {
          if (d == ref[static_cast<size_t>(j)]) {
            landed = j;
            break;
          }
        }
        const Digests& r = ref[static_cast<size_t>(target)];
        out::Printf("  replay %4d -> %4d  MISMATCH [%s ]", from, tick,
               DigestDelta(d, r).c_str());
        if (landed >= 0) {
          out::Printf(" -- landed on tick %d (%+d)", base + landed, landed - target);
        }
        else out::Printf(" -- landed on no tick of the run");
        out::Printf("  mass %+.3f  rng 0x%08X vs 0x%08X\n", delta, s.RandomState(),
               ref_rng[static_cast<size_t>(target)]);
      }
    }
  }

  out::Printf("  exact stops:    %d seeks, %d reproduced, %d failed\n", exact_seeks,
         exact_seeks - exact_bad, exact_bad);
  if (flow_provenance > 0) {
    out::Printf("                  %d of them published an empty flow texture against a full one "
           "(provenance,\n                  not divergence -- the priming frame runs no "
           "physics)\n", flow_provenance);
  }
  out::Printf("  replayed ticks: %d seeks, %d reproduced, %d diverged (worst %.3f kg)\n",
         replay_seeks, replay_seeks - replay_bad, replay_bad, worst_mass_delta);
  const bool complete = tl.carries_schedule() && tl.carries_countdowns() &&
                        tl.carries_disease() && tl.carries_registries() &&
                        tl.carries_ext_cells() && tl.carries_radiation() &&
                        tl.carries_visibility() && s.RestoreModeSupported();
  out::Printf("  checkpoints carry: schedule %s, unstable countdowns %s, disease growth %s, "
         "registries %s, ext cells %s, radiation %s, visibility %s; restore skips the load-time "
         "transition %s -- %s\n",
         tl.carries_schedule() ? "yes" : "NO", tl.carries_countdowns() ? "yes" : "NO",
         tl.carries_disease() ? "yes" : "NO", tl.carries_registries() ? "yes" : "NO",
         tl.carries_ext_cells() ? "yes" : "NO", tl.carries_radiation() ? "yes" : "NO",
         tl.carries_visibility() ? "yes" : "NO", s.RestoreModeSupported() ? "yes" : "NO",
         complete ? "every tick should reproduce"
                  : "only the exact stops can reproduce");
  out::Printf("  %d seeks, %d frames replayed, %.1f ms total, %.1f ms worst, %.1f ms mean\n",
         tl.seeks(), tl.replayed_ticks(), seek_ms_total, seek_ms_worst,
         seek_ms_total / (span > 0 ? span : 1));
  // The bar is set by what the DLL can do. With scheduling state every tick has to
  // reproduce; without it only the exact stops can, and holding the run to the stronger
  // claim would just fail an older DLL for a reason it cannot do anything about.
  if (complete) {
    if (exact_bad == 0 && replay_bad == 0) {
      out::Printf("  PASS -- every one of the %d seeks reproduced the run bit for bit\n", span);
      if (flow_provenance > 0) {
        out::Printf("         (%d exact stops qualified: an empty flow texture from the frame "
               "that primes\n          the pipeline, which is a reading provenance and not a "
               "divergence)\n", flow_provenance);
      }
      return 0;
    }
    out::Printf("  FAIL -- %d exact stops and %d replayed ticks did not reproduce\n", exact_bad,
           replay_bad);
    return 1;
  }
  if (exact_bad == 0) {
    out::Printf("  PASS -- every exact stop reproduced (this DLL cannot do better)\n");
    return 0;
  }
  out::Printf("  FAIL -- %d exact stops did not reproduce\n", exact_bad);
  return 1;
}

// --timeline-diff T: one seek, reported cell by cell rather than as a digest.
//
// --timeline-test answers "does it reproduce". When the answer is no, this answers "how":
// which cells moved, what is in them, and whether the seven scheduling counters the
// checkpoint carries are themselves back in place. The distinction matters because a
// divergence with the counters MATCHING is a divergence in something the ext message does
// not carry at all, which is a different bug from one where the counters came back wrong.
//
// Deliberately not merged into --timeline-test: the test holds 4 digests per tick, this
// holds three whole cell arrays, and paying that on every sweep would put a 40-tick run
// into hundreds of megabytes for the sake of the one tick that failed.
// FNV-1a over a copied mass array, so the frame-by-frame trace can say which published
// tick a frame is standing on rather than only what its counters read.
uint64_t fnv_mass(const std::vector<float>& v) {
  const uint8_t* b = reinterpret_cast<const uint8_t*>(v.data());
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < v.size() * sizeof(float); ++i) {
    h ^= b[i];
    h *= 1099511628211ull;
  }
  return h;
}

void PrintSched(const char* label, const oni_sim::ext::SetSchedulingStateMessage& m,
                bool valid) {
  if (!valid) {
    out::Printf("  %-9s (this DLL does not export SIM_DebugSchedulingState)\n", label);
    return;
  }
  out::Printf("  %-9s skip %d  pressure_dir %d  shuffle_dir %d  carry %.9g  rotation %u  "
         "first_substep %u  vf %u\n",
         label, m.skip_physics_frames, m.pressure_dir, m.shuffle_dir,
         static_cast<double>(m.substep_carry), (unsigned)m.displace_rotation,
         (unsigned)m.first_physics_substep, (unsigned)m.vf_active);
}

int TimelineDiff(Session& s, int target, float dt, int interval, int keep) {
  out::Printf("\ntimeline diff: tick %d, checkpoint every %d, keep %d\n", target, interval, keep);
  if (target < 1) {
    out::Printf("  FAIL -- tick %d has nothing before it to replay from\n", target);
    return 1;
  }

  Timeline tl;
  tl.Configure(interval, keep, dt);
  std::string err;
  if (!tl.Start(s, &err)) {
    out::Printf("  FAIL -- %s\n", err.c_str());
    return 1;
  }
  // `target` counts from the start of this reference run; the timeline counts absolute
  // session ticks, which begin at `base` when `--ticks N` ran frames first. See the same
  // note in TimelineTest.
  const int base = tl.base();
  const int abs_target = base + target;
  if (base > 0) {
    out::Printf("  the timeline is anchored at tick %d, so the tick under the microscope is "
           "%d\n", base, abs_target);
  }

  const int n = s.cells();
  std::vector<uint16_t> ref_elem(static_cast<size_t>(n));
  std::vector<float> ref_mass(static_cast<size_t>(n)), ref_temp(static_cast<size_t>(n));
  // The property textures too, because they are the one published surface that is DERIVED
  // rather than stored: they are rebuilt from state the blob does not carry (the flow
  // accumulator) and cached across frames (PropertyTextureBuffers, sim/textures.h), so a
  // texture can differ while every array above is identical -- which is exactly what the
  // digest sweep reports and what this has to be able to name.
  std::vector<float> ref_flow(static_cast<size_t>(n) * 2, 0.0f);
  std::vector<uint8_t> ref_sun(static_cast<size_t>(n), 0);
  bool ref_flow_valid = false, ref_sun_valid = false;
  oni_sim::ext::SetSchedulingStateMessage ref_sched{};
  bool ref_sched_valid = false;
  uint32_t ref_rng = 0;

  for (int i = 1; i <= target; ++i) {
    if (!s.Tick(dt)) {
      out::Printf("  FAIL -- tick %d refused during the reference run\n", i);
      return 1;
    }
    tl.Note(s);
  }
  {
    const oni_sim::GameDataUpdate* g = s.frame();
    memcpy(ref_elem.data(), g->elementIdx, sizeof(uint16_t) * static_cast<size_t>(n));
    memcpy(ref_mass.data(), g->mass, sizeof(float) * static_cast<size_t>(n));
    memcpy(ref_temp.data(), g->temperature, sizeof(float) * static_cast<size_t>(n));
    if (g->propertyTextureFlow) {
      memcpy(ref_flow.data(), g->propertyTextureFlow,
             sizeof(float) * 2 * static_cast<size_t>(n));
      ref_flow_valid = true;
    }
    if (g->propertyTextureExposedToSunlight) {
      memcpy(ref_sun.data(), g->propertyTextureExposedToSunlight,
             static_cast<size_t>(n));
      ref_sun_valid = true;
    }
    ref_sched_valid = s.SchedulingState(&ref_sched);
    ref_rng = s.RandomState();
  }

  const std::vector<int> marks = tl.checkpoint_ticks();
  int served_by = base;
  for (int t : marks) {
    if (t <= abs_target && t > served_by) served_by = t;
  }
  out::Printf("  checkpoints at:");
  for (int t : marks) out::Printf(" %d", t);
  out::Printf("\n  tick %d is served by the checkpoint that publishes tick %d, so the seek "
         "replays %d frame(s)\n", abs_target, served_by, abs_target - served_by);

  // Away and back. Seeking to the anchor first forces the restore rather than letting
  // Seek walk forward from where the reference run already stands.
  if (!tl.Seek(s, base, &err) || !tl.Seek(s, abs_target, &err)) {
    out::Printf("  FAIL -- seek: %s\n", err.c_str());
    return 1;
  }

  oni_sim::ext::SetSchedulingStateMessage got_sched{};
  const bool got_sched_valid = s.SchedulingState(&got_sched);
  PrintSched("run:", ref_sched, ref_sched_valid);
  PrintSched("replay:", got_sched, got_sched_valid);
  out::Printf("  rng       run 0x%08X  replay 0x%08X  %s\n", ref_rng, s.RandomState(),
         ref_rng == s.RandomState() ? "in step" : "OUT OF STEP");
  out::Printf("  mass hash run %016llx  replay %016llx\n",
         (unsigned long long)fnv_mass(ref_mass), (unsigned long long)TakeDigests(s).mass);

  const oni_sim::GameDataUpdate* g = s.frame();
  const int w = s.width();
  int diff_elem = 0, diff_mass = 0, diff_temp = 0, diff_any = 0;
  double mass_delta = 0.0, worst_cell = 0.0;
  int shown = 0;
  int min_x = w, max_x = -1, min_y = s.height(), max_y = -1;
  for (int i = 0; i < n; ++i) {
    const bool de = g->elementIdx[i] != ref_elem[static_cast<size_t>(i)];
    const bool dm = g->mass[i] != ref_mass[static_cast<size_t>(i)];
    const bool dt_ = g->temperature[i] != ref_temp[static_cast<size_t>(i)];
    if (!de && !dm && !dt_) continue;
    ++diff_any;
    diff_elem += de ? 1 : 0;
    diff_mass += dm ? 1 : 0;
    diff_temp += dt_ ? 1 : 0;
    const double d = static_cast<double>(g->mass[i]) - ref_mass[static_cast<size_t>(i)];
    mass_delta += d;
    if (d > worst_cell || -d > worst_cell) worst_cell = d < 0.0 ? -d : d;
    const int x = i % w, y = i / w;
    if (x < min_x) min_x = x;
    if (x > max_x) max_x = x;
    if (y < min_y) min_y = y;
    if (y > max_y) max_y = y;
    if (shown < 24) {
      ++shown;
      out::Printf("    (%3d,%3d) elem %5u/%5u  mass %12.6f/%12.6f  temp %10.4f/%10.4f\n", x, y,
             (unsigned)g->elementIdx[i], (unsigned)ref_elem[static_cast<size_t>(i)],
             static_cast<double>(g->mass[i]),
             static_cast<double>(ref_mass[static_cast<size_t>(i)]),
             static_cast<double>(g->temperature[i]),
             static_cast<double>(ref_temp[static_cast<size_t>(i)]));
    }
  }
  const bool identical = diff_any == 0;
  if (identical) {
    out::Printf("  %d cells: identical -- this seek reproduced the run\n", n);
  } else {
    out::Printf("  %d of %d cells differ (%d element, %d mass, %d temperature)\n", diff_any, n,
           diff_elem, diff_mass, diff_temp);
    out::Printf("  bounding box x %d..%d, y %d..%d; total mass %+.6f kg, worst cell %.6f kg\n",
           min_x, max_x, min_y, max_y, mass_delta, worst_cell);
  }

  // The two textures kept above, compared the same way. Flow is printed with its sign and
  // its bit pattern: the buffer holds floats a memcmp separates and a printf may not --
  // -0.0 and +0.0 read the same on screen and hash differently -- so a divergence that is
  // only a sign of zero has to be visible as such rather than as two identical-looking
  // numbers.
  if (ref_flow_valid && g->propertyTextureFlow) {
    const float* f = static_cast<const float*>(g->propertyTextureFlow);
    int diff = 0, zero_sign_only = 0, shown_f = 0;
    for (int i = 0; i < n; ++i) {
      const float* a = &f[static_cast<size_t>(i) * 2];
      const float* b = &ref_flow[static_cast<size_t>(i) * 2];
      uint32_t ax, ay, bx, by;
      memcpy(&ax, &a[0], 4);
      memcpy(&ay, &a[1], 4);
      memcpy(&bx, &b[0], 4);
      memcpy(&by, &b[1], 4);
      if (ax == bx && ay == by) continue;
      ++diff;
      if (a[0] == b[0] && a[1] == b[1]) ++zero_sign_only;
      if (shown_f < 16) {
        ++shown_f;
        out::Printf("    flow (%3d,%3d) replay %+.9g,%+.9g [%08X %08X]  run %+.9g,%+.9g "
               "[%08X %08X]  mass %.6f elem %u\n",
               i % w, i / w, static_cast<double>(a[0]), static_cast<double>(a[1]),
               (unsigned)ax, (unsigned)ay, static_cast<double>(b[0]),
               static_cast<double>(b[1]), (unsigned)bx, (unsigned)by,
               static_cast<double>(g->mass[i]), (unsigned)g->elementIdx[i]);
      }
    }
    if (diff == 0) {
      out::Printf("  flow texture: identical\n");
    } else {
      out::Printf("  flow texture: %d of %d cells differ, %d of them only in the sign of a "
             "zero\n", diff, n, zero_sign_only);
    }
  }
  if (ref_sun_valid && g->propertyTextureExposedToSunlight) {
    const uint8_t* b = static_cast<const uint8_t*>(g->propertyTextureExposedToSunlight);
    int diff = 0;
    for (int i = 0; i < n; ++i) {
      if (b[i] != ref_sun[static_cast<size_t>(i)]) ++diff;
    }
    out::Printf("  sunlight texture: %s\n",
           diff == 0 ? "identical" : "DIFFERS");
    if (diff > 0) out::Printf("    %d of %d cells\n", diff, n);
  }

  // WHEN the counters land, frame by frame. Every extension message is queued, so the
  // scheduling state is not in place at the instant Restore returns -- it arrives some
  // number of frames later, and if any physics runs before it does, that physics runs on
  // whatever `Load` left behind. This restores the same checkpoint again and reads the
  // counters back after each frame, so the arrival frame is measured rather than reasoned
  // about from the queue rotation.
  //
  // Printed even when the seek reproduced the run, which it did not used to be. The trace
  // is the only way to see whether the frame that publishes a restore actually STEPPED --
  // `displace_rotation` advances once per substep, so it says outright what no digest can:
  // an identical seek is equally consistent with "the frame ran and changed nothing" and
  // with "the frame ran nothing". Returning early on identical hid exactly the question
  // this instrument exists to answer.
  const Session::Checkpoint* cp = tl.checkpoint_serving(abs_target);
  if (cp && cp->sched_valid) {
    out::Printf("  when the message lands, frame by frame:" "\n");
    PrintSched("wanted:", cp->sched, true);
    if (!s.Restore(*cp)) {
      out::Printf("    FAIL -- restore refused\n");
      return 1;
    }
    oni_sim::ext::SetSchedulingStateMessage m{};
    if (s.SchedulingState(&m)) PrintSched("+0 frames", m, true);
    // The flow texture beside the counters, for the same reason the mixture is: it is the
    // one published buffer whose value exists only for the frame that moved something, so
    // "how many cells does it hold" is a direct reading of whether this frame ran a
    // transfer -- which `displace_rotation` alone cannot distinguish from a frame that ran
    // a substep and moved nothing.
    out::Printf("    +0 frames  flow %d cells  mass %016llx\n", MeasureTextures(s).flow,
           (unsigned long long)TakeDigests(s).mass);
    // The mixture beside the counters, because the mixing pass runs inside the substep loop
    // and therefore under the same skip: if the counters say a frame stepped nothing and the
    // mixture moved anyway, that is a real contradiction and worth seeing on one screen
    // rather than inferring across two runs.
    if (s.has_mixture()) out::Printf("    +0 frames  mixture %.9f kg\n", TakeDigests(s).mixture_mass);
    for (int f = 1; f <= 3; ++f) {
      if (!s.Tick(dt)) {
        out::Printf("    FAIL -- frame %d refused\n", f);
        return 1;
      }
      char label[16];
      snprintf(label, sizeof(label), "+%d frames", f);
      if (s.SchedulingState(&m)) PrintSched(label, m, true);
      out::Printf("    %s  flow %d cells  mass %016llx\n", label, MeasureTextures(s).flow,
             (unsigned long long)TakeDigests(s).mass);
      if (s.has_mixture()) {
        out::Printf("    %s  mixture %.9f kg\n", label, TakeDigests(s).mixture_mass);
      }
    }
  }
  return identical ? 0 : 1;
}

// Which surfaces one view actually needs off the wire.
//
// Offline every array is already in memory and a view costs nothing to switch to. Live,
// each one is a copy the game makes on its main thread and a body it sends, so the whole
// grid every poll is 57 bytes a cell -- 14.6 MB on the 636x404 reference cluster. Asking
// for what is going to be drawn is 2.6 MB of that, and the difference is the frame rate.
//
// The base three are unconditional because the readers assume them: SampleCell
// dereferences elementIdx, mass and temperature with no null check, and ViewScalar's
// default arm reads mass. attach.h refuses a frame that is missing any of them rather
// than letting one reach a renderer.
//
// The liquid trio travels together on purpose. The hover readout tests
// propertyTextureLiquid and then reads LiquidData and MaterialData beside it; fetching
// only the first would print two zeroes that look like data and are not.
attach::SurfaceSet FetchSetForView(View view, bool everything) {
  attach::SurfaceSet set;
  if (everything) {
    set.AddAll();
    return set;
  }
  set.Add(attach::kElementIdx);
  set.Add(attach::kMass);
  set.Add(attach::kTemperature);
  switch (view) {
    case View::Flow:
      set.Add(attach::kTextureFlow);
      break;
    case View::Liquid:
      set.Add(attach::kTextureLiquid);
      set.Add(attach::kTextureLiquidData);
      set.Add(attach::kTextureMaterialData);
      break;
    case View::Sunlight:
      set.Add(attach::kTextureSunlight);
      break;
    case View::Element:
    case View::Temperature:
    case View::Mass:
    case View::Phase:
    // Ext comes from `/extcells` and not from `/grid`, so it asks for nothing beyond the
    // base three here. Listed rather than left to a default so that adding a view cannot
    // silently inherit the wrong fetch set.
    case View::Ext:
    // Attr comes from `/extelements`, and needs `elementIdx` -- which the base three
    // already carry, because every view needs it for the phase backdrop.
    case View::Attr:
    // SimCheck reads elementIdx, mass and temperature -- the base three, exactly. The
    // element table it also needs is fetched once at attach, not per frame.
    case View::SimCheck:
      break;
  }
  return set;
}

// The surface a texture view is drawn from, so the window can tell "the game does not
// publish this" from "this poll did not ask for it". ViewAvailable answers the second
// question by looking at a pointer, which live is only ever as true as the last fetch
// set -- refusing a view switch on that basis would refuse every one of them.
bool ViewPublishedLive(const Session& s, View view) {
  switch (view) {
    case View::Flow: return s.source().ServerPublishes(attach::kTextureFlow);
    case View::Liquid: return s.source().ServerPublishes(attach::kTextureLiquid);
    case View::Sunlight: return s.source().ServerPublishes(attach::kTextureSunlight);
    // `/extcells` carries the registry, but only a Session that was told to fetch it has
    // one to draw from -- so this answers "could the Session see it", not "does the game
    // have it". Without --ext the window refuses the switch rather than painting flat.
    case View::Ext: return s.ext_live();
    // Attr has no equivalent gate, because `/extelements` has no cost worth gating: it
    // serialises a registry the ABI caps at 64 attributes, not a copy of the world. Whether
    // the ATTRIBUTE is one this game carries is `ViewAvailableIn`'s question, not this one.
    case View::Attr: return true;
    default: return true;
  }
}

// RunWindow's two answers that are not an exit code. Negative because every real exit code
// this program returns is 0, 1 or 2, so a caller that forgets to check cannot mistake one for
// success -- and neither ever reaches the process's exit status: main turns both into a
// launcher pass.
constexpr int kRelaunch = -2;    // the window asked for the start screen back
constexpr int kOpenFailed = -3;  // the session could not be opened; the message is in *err

}  // namespace

// Outside the unnamed namespace, unlike everything around it: the window classes in src/qt/
// hold a copy, and a class with linkage holding a member of an internal-linkage type is a
// -Wsubobject-linkage warning on every one of them.
struct WindowOptions {
  float dt = 0.2f;
  bool mass_fade = true;
  bool fixed_range = false;
  bool full_span = false;
  Range fixed;
  bool seed_pinned = false;
  uint32_t seed = 0;
  int width = 1100, height = 760;
  float ui_scale = 1.0f;
  bool fullscreen = false;
  const char* corpus = "";
  bool timeline = true;
  int timeline_interval = 10;
  int timeline_keep = 8;

  // Live attach. `attached` also forces the timeline off: a checkpoint is a save blob
  // plus five pieces of sim state this program does not have when the sim is in another
  // process, so there is no history to scrub and the bar would be a lie.
  //
  // The fetch set is PINNED when the command line named one (--attach-arrays, or a
  // whole-grid mode like --digest). Pinned, a view change must not touch it: the caller
  // asked for those arrays, and quietly swapping them for the ones the new view happens
  // to want would answer a question nobody asked.
  bool attached = false;
  std::string endpoint;
  int poll_ms = 200;
  bool fetch_pinned = false;
  attach::SurfaceSet fetch_set;

  // Where the camera starts. Default-constructed means "fit to the window", which is what
  // it always did; a `--camera` copied off another run's panels seats it exactly instead,
  // and `--crop`/`--zoom` seat it the way a composed snapshot would. Same `SeatCamera` as
  // the snapshot path uses, deliberately.
  CameraSpec camera;
};

namespace {

}  // namespace

// The window. It lives in src/qt/ and is the window half of this file: those headers call the
// tool functions above (Probe, Digest, ExtList, ...) and take WindowOptions, so they are
// included HERE, after all of it, with the unnamed namespace closed around them -- Qt's own
// headers must not be opened inside it. See qt/viewer.h for what moved where.
#include "qt/launcher.h"
#include "qt/mainwindow.h"

namespace {

// --ui-sample. With no source: the start screen in both of its shapes, which needs no sim and
// so is an OWNED golden. With a source (--corpus or --attach), main() also boots it and writes
// the whole window once per dock -- see qtui::WindowSample -- after this has run.
int UiSample(const char* png_path) {
  if (png_path == nullptr) {
    out::Printf("--ui-sample writes PNGs: pass --snapshot <out.png> with it.\n");
    return 2;
  }
  launch::Spec offline;
  offline.corpus = "sim_corpus.bin";
  offline.dll = kDefaultDll;
  offline.seed = "0x0BADF00D";
  offline.ticks = 120;
  launch::Spec live;
  live.attach = true;
  live.endpoint = "127.0.0.1:9788";
  live.ext_published = true;
  live.ext_props = "sim.gas_mass";
  live.events = "sim.message_refused";
  live.view = View::Temperature;
  const struct { const char* suffix; const launch::Spec* spec; const char* error; } kScreens[] = {
      {"-launch-corpus", &offline, ""},
      {"-launch-attach", &live, "attach failed: connection refused by 127.0.0.1:9788"},
  };
  launch::g_sample_assume_files_exist = true;
  for (const auto& sc : kScreens) {
    QImage screen;
    const std::string out_path = qtui::SuffixedPath(png_path, sc.suffix);
    if (!qtui::LauncherSample(*sc.spec, sc.error, &screen) ||
        !WritePng(qtui::FromQImage(screen), out_path)) {
      out::Printf("could not write %s\n", out_path.c_str());
      return 1;
    }
    out::Printf("wrote %s (start screen)\n", out_path.c_str());
  }
  launch::g_sample_assume_files_exist = false;
  return 0;
}

// The interactive loop, now Qt's. The sim is only advanced by the window's clock or by a step
// or seek action -- a repaint, a resize or a drag can never move it (qt/viewer.h).
int RunWindow(Session& s, View view, const WindowOptions& opt) {
  return qtui::RunQtWindow(s, view, opt);
}

// ---------------------------------------------------------------------------------------
// The GUI path: open what the start screen asked for, and run the window on it.
//
// This is deliberately NOT the same code path as the command line's. The command line can ask
// for eighteen things that have nothing to do with a window -- a digest log, a bisect, a load
// stress, a checkpoint test -- and threading a launcher through all of them would mean a start
// screen that has to offer them all before it can offer anything. What this opens is the
// subset a window can show: a source, a seed, some ticks, and a view. Everything else stays a
// flag, and stage D is where the one-shot actions become buttons INSIDE the window, where they
// belong -- next to the output pane that shows what they printed.
int OpenAndRun(const launch::Spec& spec, std::string* err) {
  err->clear();
  Session session;
  attach::SurfaceSet fetch;
  View view = spec.view;

  if (spec.attach) {
    out::Printf("attaching: %s\n", spec.endpoint.c_str());
    std::string e;
    if (!session.Attach(spec.endpoint.c_str(), spec.attach_timeout_ms, &e)) {
      *err = "attach failed: " + e;
      return kOpenFailed;
    }
    fetch = FetchSetForView(view, false);
    if (!session.Poll(fetch, &e)) {
      *err = "first poll failed: " + e;
      return kOpenFailed;
    }
    out::Printf("attached: %dx%d (%d cells), %d elements, unity frame %u\n", session.width(),
           session.height(), session.cells(), session.tables().count,
           session.source().unity_frame());
    out::Printf("fetching %zu arrays (what the view needs): %.2f MB per frame\n", fetch.size(),
           session.source().last_bytes() / (1024.0 * 1024.0));
    // Same refusal as the command line's, and for the same reason: a texture the game does
    // not publish would render an empty world, which is the one failure a debugger must not
    // have. Here it goes back to the start screen instead of exiting, so the view can be
    // changed and tried again.
    if (view != View::Ext && view != View::Attr && !ViewAvailableIn(session, view)) {
      *err = std::string("that game publishes no ") + ViewName(view) + " texture";
      session.Shutdown();
      return kOpenFailed;
    }
  } else {
    out::Printf("booting: dll=%s\n        corpus=%s\n", spec.dll.c_str(),
           spec.corpus.c_str());
    if (!session.Boot(spec.dll.c_str(), spec.corpus.c_str())) {
      *err = "boot failed -- the console says what the DLL refused";
      return kOpenFailed;
    }
    out::Printf("booted: %dx%d (%d cells) from %zu corpus records, %d elements\n",
           session.width(), session.height(), session.cells(), session.records_read(),
           session.tables().count);
    out::Printf("rng state after boot: 0x%08X\n", session.RandomState());
    // Only when nothing is about to pin it. The warning below and a --seed on the same
    // command line contradict each other, and the seed is the half that is true.
    if (session.wall_clock_seeded() && spec.seed.empty()) {
      out::Printf("NOTE: this corpus boots through AllocateCells, which seeds the random\n"
             "      stream from the wall clock. Two runs of this WILL diverge once physics\n"
             "      touches a gas -- pin a seed, or use a worldgen corpus.\n");
    }
    WarnUnpublishedBoot(session);
    if (!spec.seed.empty()) {
      const uint32_t seed =
          static_cast<uint32_t>(strtoul(spec.seed.c_str(), nullptr, 0));
      if (!session.SetRandomState(seed)) {
        *err = "could not pin the random stream -- this SimDLL has no kSetRandomState, so it "
               "is a stock Klei DLL or one built without the extension surface";
        session.Shutdown();
        return kOpenFailed;
      }
      out::Printf("pinned random stream to 0x%08X\n", seed);
    }
    for (int t = 0; t < spec.ticks; ++t) {
      if (!session.Tick(static_cast<float>(spec.dt))) {
        *err = "tick " + std::to_string(t) + " failed: PrepareGameData returned null";
        session.Shutdown();
        return kOpenFailed;
      }
    }
    if (spec.ticks > 0)
      out::Printf("ran %d ticks at dt=%.3f\n", session.ticks(), spec.dt);
  }

  // Same order and the same reason as the command line: the registry is pulled once, after
  // everything that could change the world has run, so the window and the readouts agree.
  if (!spec.ext_name.empty()) session.SetExtView(spec.ext_name, spec.ext_comp);
  if (!spec.attr_name.empty()) session.SetAttrView(spec.attr_name, spec.attr_comp);
  session.set_ext_live(spec.ext_live || spec.ext_published || view == View::Ext);
  if (session.attached()) {
    if (spec.ext_published) session.set_ext_published(true, spec.ext_props);
    if (!spec.events.empty()) session.set_event_streams(spec.events);
  }
  session.RefreshExtProps();
  session.RefreshExtAttrs();

  if (view == View::Ext && !ViewAvailableIn(session, view)) {
    *err = "no extension property named \"" + spec.ext_name +
           "\" is being carried, or it has no such component";
    session.Shutdown();
    return kOpenFailed;
  }
  if (view == View::Attr && !ViewAvailableIn(session, view)) {
    *err = "no element in this table carries \"" + spec.attr_name +
           "\", or it has no such component";
    session.Shutdown();
    return kOpenFailed;
  }

  WindowOptions opt;
  opt.dt = static_cast<float>(spec.dt);
  opt.mass_fade = spec.mass_fade;
  opt.seed_pinned = !spec.seed.empty();
  opt.seed = opt.seed_pinned
                 ? static_cast<uint32_t>(strtoul(spec.seed.c_str(), nullptr, 0))
                 : 0;
  opt.corpus = spec.corpus.c_str();  // borrowed: `spec` outlives this call by construction
  opt.timeline = spec.timeline;
  opt.attached = session.attached();
  if (session.attached()) {
    opt.endpoint = session.source().host() + ":" + session.source().port();
    opt.poll_ms = spec.poll_ms;
    opt.fetch_pinned = false;
    opt.fetch_set = fetch;
  }
  opt.width = spec.width;
  opt.height = spec.height;
  opt.ui_scale = spec.ui_scale;

  const int rc = RunWindow(session, view, opt);
  session.Shutdown();
  return rc;
}

// Start screen, window, start screen, window, until somebody quits. A failed open comes back
// here with its message rather than ending the process, because the person who double-clicked
// the exe has no console to read it in and a window that vanishes is not an error report.
int LauncherLoop(launch::Spec spec) {
  std::string err;
  for (;;) {
    if (!qtui::RunQtLauncher(&spec, err)) return 0;
    err.clear();
    const int rc = OpenAndRun(spec, &err);
    if (rc == kRelaunch || rc == kOpenFailed) continue;
    return rc;
  }
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  // No arguments at all: the start screen, not usage. This is the front door -- double-clicking
  // the exe has to lead somewhere, and until it did, every feature of this program was behind a
  // flag vocabulary you had to already know. Any argument at all still means the command line,
  // so nothing scripted changes shape: `simviz.exe --corpus x` with a typo in it still fails
  // with a message instead of opening a window a script cannot answer.
  if (argc == 1) {
    launch::Spec spec;
    spec.dll = kDefaultDll;
    return LauncherLoop(spec);
  }

  const char* corpus = nullptr;
  const char* attach_to = nullptr;
  const char* attach_arrays = nullptr;
  int poll_ms = 200;
  int attach_timeout_ms = 5000;
  const char* dll = kDefaultDll;
  const char* snapshot = nullptr;
  const char* dump_state = nullptr;
  const char* digest_log = nullptr;
  // --bisect and --diff-state each take TWO paths and neither needs a sim. Held as pairs
  // rather than as one string so nothing has to be re-split later.
  const char* bisect_a = nullptr;
  const char* bisect_b = nullptr;
  const char* diff_a = nullptr;
  const char* diff_b = nullptr;
  std::string diff_view;
  int ticks = 0, zoom = 1;
  float dt = 0.2f;
  uint32_t seed = 0;
  bool pin_seed = false;
  int crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0;
  int probe_x = 0, probe_y = 0, probe_w = 0, probe_h = 0;
  int inject_x = 0, inject_y = 0, inject_w = 0, inject_h = 0;
  bool inject_gas = false;
  float inject_fraction = 0.5f;
  // --ext-probe: the rectangle a per-cell thermal-mass bonus is written into, and the value.
  // Named ext_* rather than probe_*, which --probe (one cell, read-only) already owns.
  // 50 kJ/K is large enough that a checkpoint dropping it shows up as a temperature
  // divergence within a handful of ticks rather than as a rounding difference.
  int ext_x = 0, ext_y = 0, ext_w = 0, ext_h = 0;
  bool ext_probe = false;
  float ext_jk = 50000.0f;
  // What the component measured straight after the probe wrote. Handed to TimelineTest so it
  // can tell that the anchor restore carried it; 0 when no probe ran.
  size_t ext_probe_bytes = 0;
  int hover_x = 0, hover_y = 0;
  // --camera: the window's own camera, verbatim. Distinct from --crop/--zoom, which are the
  // human forms and can only express an integer magnification about a cell -- the DEFAULT
  // camera, fit to the window, has a fractional scale and a fractional origin and is not
  // reachable through them at all. This is what makes the command line the panels print
  // reproduce what is actually on screen rather than something near it.
  float camera_cx = 0.0f, camera_cy = 0.0f, camera_scale = 0.0f;
  bool camera_set = false;
  int size_w = 0, size_h = 0;
  float ui_scale = 1.0f;
  bool fullscreen = false;
  bool probed = false, hovered = false, sized = false;
  bool cropped = false, fixed_range = false, verbose = false, dump_elements = false;
  bool full_span = false, stats = false, mass_fade = true, digest = false;
  bool ext_list = false;
  bool ext_attrs = false;
  bool ext_live = false;
  bool ext_published = false;
  std::string ext_published_props;
  std::string event_streams;
  bool events_list = false;
  const char* ext_blob_path = nullptr;
  // --view ext:<name>[c]: which extension property to colour by. Empty for the seven
  // fixed views. Handed to the Session after the boot, because nothing is registered
  // before one.
  std::string ext_view_name;
  int ext_view_comp = 0;
  // --view attr:<name>[c]: which per-element attribute to colour by. Registry 2 rather than
  // registry 1, and the same "nothing is registered before a boot" rule applies.
  std::string attr_view_name;
  int attr_view_comp = 0;
  bool windowed = false, hud_snapshot = false;
  bool ui_sample = false;
  int checkpoint_span = 0;
  int timeline_span = 0;
  int timeline_diff = 0;
  int timeline_interval = 10, timeline_keep = 8;
  bool timeline = true;
  int load_stress = 0, load_stress_ticks = 1;
  bool bare_load = false;
  bool replay_registrations = false;
  float range_lo = 0.0f, range_hi = 0.0f;
  View view = View::Element;

  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (!strcmp(a, "--corpus") && i + 1 < argc) corpus = argv[++i];
    else if (!strcmp(a, "--attach") && i + 1 < argc) attach_to = argv[++i];
    else if (!strcmp(a, "--attach-arrays") && i + 1 < argc) attach_arrays = argv[++i];
    else if (!strcmp(a, "--poll") && i + 1 < argc) poll_ms = atoi(argv[++i]);
    else if (!strcmp(a, "--attach-timeout") && i + 1 < argc) {
      attach_timeout_ms = atoi(argv[++i]);
    }
    else if (!strcmp(a, "--dll") && i + 1 < argc) dll = argv[++i];
    else if (!strcmp(a, "--ticks") && i + 1 < argc) ticks = atoi(argv[++i]);
    else if (!strcmp(a, "--dt") && i + 1 < argc) dt = static_cast<float>(atof(argv[++i]));
    else if (!strcmp(a, "--seed") && i + 1 < argc) {
      seed = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 0));
      pin_seed = true;
    }
    else if (!strcmp(a, "--snapshot") && i + 1 < argc) snapshot = argv[++i];
    else if (!strcmp(a, "--dump-state") && i + 1 < argc) dump_state = argv[++i];
    else if (!strcmp(a, "--digest-log") && i + 1 < argc) digest_log = argv[++i];
    else if (!strcmp(a, "--bisect") && i + 2 < argc) {
      bisect_a = argv[++i];
      bisect_b = argv[++i];
    }
    else if (!strcmp(a, "--diff-state") && i + 2 < argc) {
      diff_a = argv[++i];
      diff_b = argv[++i];
    }
    else if (!strcmp(a, "--diff-view") && i + 1 < argc) diff_view = argv[++i];
    else if (!strcmp(a, "--zoom") && i + 1 < argc) zoom = atoi(argv[++i]);
    else if (!strcmp(a, "--window")) windowed = true;
    else if (!strcmp(a, "--checkpoint-test") && i + 1 < argc) {
      checkpoint_span = atoi(argv[++i]);
    }
    else if (!strcmp(a, "--timeline-test") && i + 1 < argc) {
      timeline_span = atoi(argv[++i]);
    }
    else if (!strcmp(a, "--timeline-diff") && i + 1 < argc) {
      timeline_diff = atoi(argv[++i]);
    }
    else if (!strcmp(a, "--timeline-interval") && i + 1 < argc) {
      timeline_interval = atoi(argv[++i]);
    }
    else if (!strcmp(a, "--timeline-keep") && i + 1 < argc) {
      timeline_keep = atoi(argv[++i]);
    }
    else if (!strcmp(a, "--no-timeline")) timeline = false;
    else if (!strcmp(a, "--ext-probe") && i + 1 < argc) {
      if (!ParseFour(argv[++i], &ext_x, &ext_y, &ext_w, &ext_h)) {
        out::Printf("--ext-probe wants x,y,w,h\n");
        return 2;
      }
      ext_probe = true;
    }
    else if (!strcmp(a, "--ext-probe-jk") && i + 1 < argc) {
      ext_jk = static_cast<float>(atof(argv[++i]));
    }
    else if (!strcmp(a, "--inject-gas") && i + 1 < argc) {
      if (!ParseFour(argv[++i], &inject_x, &inject_y, &inject_w, &inject_h)) {
        out::Printf("--inject-gas wants x,y,w,h\n");
        return 2;
      }
      inject_gas = true;
    }
    else if (!strcmp(a, "--inject-fraction") && i + 1 < argc) {
      inject_fraction = static_cast<float>(atof(argv[++i]));
    }
    else if (!strcmp(a, "--load-stress") && i + 1 < argc) {
      load_stress = atoi(argv[++i]);
    }
    else if (!strcmp(a, "--bare-load")) bare_load = true;
    else if (!strcmp(a, "--replay-registrations")) replay_registrations = true;
    else if (!strcmp(a, "--load-stress-ticks") && i + 1 < argc) {
      load_stress_ticks = atoi(argv[++i]);
    }
    else if (!strcmp(a, "--hud")) hud_snapshot = true;
    else if (!strcmp(a, "--ui-sample")) ui_sample = true;
    else if (!strcmp(a, "--qt-check")) return qtapp::Check();
    else if (!strcmp(a, "--verbose")) verbose = true;
    else if (!strcmp(a, "--stats")) stats = true;
    else if (!strcmp(a, "--digest")) digest = true;
    else if (!strcmp(a, "--ext-list")) ext_list = true;
    else if (!strcmp(a, "--ext-attrs")) ext_attrs = true;
    else if (!strcmp(a, "--ext")) ext_live = true;
    else if (!strncmp(a, "--ext-published", 15)) {
      // `--ext-published` and `--ext-published=a,b` are one flag rather than two, because the
      // list is optional and an optional positional argument would swallow the next flag.
      ext_published = true;
      if (a[15] == '=') ext_published_props = a + 16;
      else if (a[15] != '\0') { fprintf(stderr, "unknown flag: %s\n", a); return 2; }
    }
    else if (!strcmp(a, "--events-list")) events_list = true;
    else if (!strncmp(a, "--events", 8) && strcmp(a, "--events-list") != 0) {
      // `--events=a,b` only. Unlike --ext-published there is no useful bare form: the route's
      // bare answer is discovery, and that is what --events-list is for, so a bare --events
      // would either subscribe nothing (silently doing nothing) or subscribe everything
      // (putting the game to work on a typo). Both are worse than a refusal.
      if (a[8] != '=' && a[8] != '\0') {
        // `--eventsX` is a typo, not a bare flag, and saying "wants a stream list" about it
        // would send the reader looking for the list they did pass.
        fprintf(stderr, "unknown flag: %s\n", a);
        return 2;
      }
      if (a[8] != '=' || a[9] == '\0') {
        fprintf(stderr, "--events wants a stream list, e.g. --events=sim.message_refused "
                        "(--events-list shows what the game declares)\n");
        return 2;
      }
      event_streams = a + 9;
    }
    else if (!strcmp(a, "--ext-blob") && i + 1 < argc) ext_blob_path = argv[++i];
    else if (!strcmp(a, "--no-mass-fade")) mass_fade = false;
    else if (!strcmp(a, "--dump-elements")) dump_elements = true;
    else if (!strcmp(a, "--view") && i + 1 < argc) {
      if (!ParseView(argv[++i], &view, &ext_view_name, &ext_view_comp, &attr_view_name,
                     &attr_view_comp)) {
        out::Printf("unknown view '%s'\n", argv[i]);
        return 2;
      }
    } else if (!strcmp(a, "--font") && i + 1 < argc) {
      const char* which = argv[++i];
      if (strcmp(which, "large") && strcmp(which, "small")) {
        out::Printf("--font wants large (7x13) or small (5x8, the default)\n");
        return 2;
      }
      qtapp::g_large_font = !strcmp(which, "large");
    } else if (!strcmp(a, "--fullscreen")) {
      fullscreen = true;
    } else if (!strcmp(a, "--ui-scale") && i + 1 < argc) {
      ui_scale = static_cast<float>(atof(argv[++i]));
      if (ui_scale != 1.0f && ui_scale != 2.0f && ui_scale != 3.0f && ui_scale != 4.0f) {
        out::Printf("--ui-scale wants 1, 2, 3 or 4; for text a little larger, use --font large\n");
        return 2;
      }
    } else if (!strcmp(a, "--size") && i + 1 < argc) {
      if (!ParseSize(argv[++i], &size_w, &size_h)) {
        out::Printf("--size wants WxH\n");
        return 2;
      }
      sized = true;
    } else if (!strcmp(a, "--camera") && i + 1 < argc) {
      if (!ParseCamera(argv[++i], &camera_cx, &camera_cy, &camera_scale)) {
        out::Printf("--camera wants cx,cy,scale\n");
        return 2;
      }
      if (!(camera_scale > 0.0f)) {
        // Zero or negative would divide by zero in every screen<->world conversion and draw
        // a blank frame with no error in it. Refuse the number rather than the picture.
        out::Printf("--camera scale must be greater than 0, got %g\n",
               static_cast<double>(camera_scale));
        return 2;
      }
      camera_set = true;
    } else if (!strcmp(a, "--crop") && i + 1 < argc) {
      if (!ParseFour(argv[++i], &crop_x, &crop_y, &crop_w, &crop_h)) {
        out::Printf("--crop wants x,y,w,h\n");
        return 2;
      }
      cropped = true;
    } else if (!strcmp(a, "--probe") && i + 1 < argc) {
      if (!ParseProbe(argv[++i], &probe_x, &probe_y, &probe_w, &probe_h)) {
        out::Printf("--probe wants x,y or x,y,w,h\n");
        return 2;
      }
      probed = true;
    } else if (!strcmp(a, "--hover") && i + 1 < argc) {
      if (sscanf(argv[++i], "%d,%d", &hover_x, &hover_y) != 2) {
        out::Printf("--hover wants x,y in cell coordinates\n");
        return 2;
      }
      hovered = true;
    } else if (!strcmp(a, "--range") && i + 1 < argc) {
      if (!strcmp(argv[++i], "full")) {
        full_span = true;
      } else if (ParseTwo(argv[i], &range_lo, &range_hi)) {
        fixed_range = true;
      } else {
        out::Printf("--range wants lo,hi or the word 'full'\n");
        return 2;
      }
    } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
      Usage();
      return 0;
    } else {
      out::Printf("unknown argument '%s'\n\n", a);
      Usage();
      return 2;
    }
  }

  // The start-screen sheets need no sim. With a source named, the window sheets follow once
  // it has been opened, further down.
  if (ui_sample) {
    const int rc = UiSample(snapshot);
    if (rc != 0 || (!corpus && !attach_to)) return rc;
  }
  // Nor do these two, and that is the point of them: they read what earlier runs wrote, so
  // the two sides of a comparison can be two processes, two DLLs, or two machines.
  if (bisect_a != nullptr) return Bisect(bisect_a, bisect_b);
  if (diff_a != nullptr) {
    return DiffState(diff_a, diff_b, diff_view, snapshot, cropped, crop_x, crop_y, crop_w,
                     crop_h, zoom);
  }
  if (attach_to && corpus) {
    out::Printf("--attach and --corpus are the two SOURCES and only one can be live at a\n"
           "time: --corpus drives a SimDLL here, --attach reads a game that is driving\n"
           "its own. Pick one.\n");
    return 2;
  }
  if (!attach_to && !corpus) {
    Usage();
    return 2;
  }
  if (dump_elements) {
    if (!corpus) {
      out::Printf("--dump-elements reads the table out of a corpus; pass --corpus. Attached,\n"
             "the same table is one HTTP request away: curl <host>/elements\n");
      return 2;
    }
    return DumpElements(corpus);
  }

  // --camera describes a COMPOSED frame -- the window, or the same frame drawn without one
  // by --hud. A plain snapshot is the world image itself, cropped in cells and magnified by
  // an integer; there is no camera in that path to seat, and a fractional scale could not be
  // honoured by it if there were. Refused by name rather than ignored, and it names the two
  // flags that DO express what the caller almost certainly wants.
  if (camera_set && !windowed && !hud_snapshot) {
    out::Printf("--camera places the camera of a composed frame; pass --window, or --hud with\n"
           "      --snapshot. For a plain snapshot the equivalents are --crop x,y,w,h and\n"
           "      --zoom N, in cell coordinates.\n");
    return 2;
  }

  // Everything that sends the sim a message, checkpoints it, or reasons about its random
  // stream needs a sim in THIS process. Attached there is not one -- the game owns it,
  // across a socket, and none of these have a remote equivalent. Refused by name up
  // front rather than silently ignored, because a flag that appears to be accepted and
  // does nothing is how a measurement gets believed.
  if (attach_to) {
    struct Refusal { bool set; const char* flag; };
    const Refusal refusals[] = {
        {ticks != 0, "--ticks"},          {pin_seed, "--seed"},
        {inject_gas, "--inject-gas"},     {ext_probe, "--ext-probe"},
        {checkpoint_span != 0, "--checkpoint-test"},
        {timeline_span != 0, "--timeline-test"},
        {timeline_diff != 0, "--timeline-diff"},
        {load_stress != 0, "--load-stress"},
        {bare_load, "--bare-load"},       {replay_registrations, "--replay-registrations"},
        {!timeline, "--no-timeline"},     {dll != kDefaultDll, "--dll"},
    };
    for (const Refusal& r : refusals) {
      if (!r.set) continue;
      out::Printf("%s drives a simulation; --attach reads one that is already running and\n"
             "      sends it nothing. Drop %s, or drop --attach.\n", r.flag, r.flag);
      return 2;
    }
    // Refused separately, because the reason is not the one above: the route exists, it is
    // the TICK that does not. Attached there are only polls, which arrive whenever the game
    // got round to a frame, so two such logs cannot be compared position for position --
    // and comparing them position for position is the one thing --bisect does.
    if (digest_log != nullptr) {
      out::Printf("--digest-log writes one line per TICK and attached there are no ticks, only\n"
             "      polls -- so two logs taken this way would be compared frame against\n"
             "      whichever frame happened to land. Use --digest for one live reading.\n");
      return 2;
    }
  }

  Session session;
  session.verbose = verbose;
  attach::SurfaceSet fetch;
  if (attach_to) {
    out::Printf("attaching: %s\n", attach_to);
    std::string err;
    if (!session.Attach(attach_to, attach_timeout_ms, &err)) {
      out::Printf("attach failed: %s\n", err.c_str());
      return 1;
    }
    // --digest hashes twelve arrays and --dump-state writes them all, so neither can be
    // served by a view-shaped fetch. They widen the set rather than reporting on the
    // handful the view happened to need, which would be a digest of a different thing
    // every time the view changed.
    const bool whole_grid = digest || dump_state != nullptr;
    const char* why = "what the view needs";
    if (attach_arrays) {
      std::string aerr;
      if (!fetch.ParseList(attach_arrays, &aerr)) {
        out::Printf("--attach-arrays: %s\n", aerr.c_str());
        return 2;
      }
      // The three the readers dereference without checking. Added silently rather than
      // refused: asking for `mass` alone is a reasonable thing to type and a crash is
      // not a reasonable answer to it.
      fetch.Add(attach::kElementIdx);
      fetch.Add(attach::kMass);
      fetch.Add(attach::kTemperature);
      why = "as --attach-arrays asked, plus the three every reader needs";
    } else {
      fetch = FetchSetForView(view, whole_grid);
      if (whole_grid) why = "everything, for the whole-grid readouts";
    }
    if (!session.Poll(fetch, &err)) {
      out::Printf("first poll failed: %s\n", err.c_str());
      return 1;
    }
    out::Printf("attached: %dx%d (%d cells), %d elements, unity frame %u\n", session.width(),
           session.height(), session.cells(), session.tables().count,
           session.source().unity_frame());
    for (const attach::WorldInfo& w : session.source().worlds()) {
      out::Printf("  world %d '%s' at %d,%d %dx%d\n", w.id, w.name.c_str(), w.offset_x,
             w.offset_y, w.width, w.height);
    }
    out::Printf("fetching %zu arrays (%s): %.2f MB per frame\n", fetch.size(), why,
           session.source().last_bytes() / (1024.0 * 1024.0));
    // A texture view asked for on the command line against a game that does not publish
    // it would render an empty world, which is the one failure a debugger must not have.
    //
    // The ext and attr views are the two this cannot judge yet: they depend on the two
    // extension registries, and those are fetched further down, after every world-changing
    // flag has run. The gates that follow `RefreshExtProps` answer them for the attached and
    // offline paths alike, and say which of "no such name", "no such component" and
    // "nothing fetched it" applies.
    if (view != View::Ext && view != View::Attr && !ViewAvailableIn(session, view)) {
      out::Printf("the game publishes no %s texture\n", ViewName(view));
      return 1;
    }
  } else {
    out::Printf("booting: dll=%s\n        corpus=%s\n", dll, corpus);
    if (!session.Boot(dll, corpus)) {
      out::Printf("boot failed\n");
      return 1;
    }
    out::Printf("booted: %dx%d (%d cells) from %zu corpus records, %d elements\n",
           session.width(), session.height(), session.cells(), session.records_read(),
           session.tables().count);
    out::Printf("rng state after boot: 0x%08X\n", session.RandomState());
  }
  if (replay_registrations) {
    session.replay_registrations = true;
    out::Printf("registration replay ON: %zu of %zu corpus records will be re-sent after every\n"
           "      restore that reallocates (buildings, element chunks, flow, radiation)\n",
           session.registrations_held(), session.records_read());
  }
  // NOT when --seed is pinning the stream three lines below. This warning used to print
  // regardless, so `--seed 7 --ticks 200` announced that two runs of it WILL diverge while
  // in fact two such runs reproduce each other exactly -- measured, and the reason the test
  // suite pins a seed on every arm that ticks. A warning that the next line disproves is
  // worse than no warning: it teaches the reader to distrust a guarantee that holds.
  if (session.wall_clock_seeded() && !pin_seed) {
    out::Printf("NOTE: this corpus boots through AllocateCells, which seeds the random\n"
           "      stream from the wall clock (sim/simdll.cpp:2827 -- Klei's own\n"
           "      behaviour for a LOAD). Two runs of this command WILL diverge once\n"
           "      physics touches a gas. Compare runs only within a single process,\n"
           "      or use a worldgen corpus, which carries a real world seed.\n");
  }
  WarnUnpublishedBoot(session);

  if (pin_seed) {
    if (!session.SetRandomState(seed)) {
      out::Printf("could not pin the random stream: this SimDLL has no SIM_DebugRandomState,\n"
             "      so it has no kSetRandomState either -- it is a stock Klei DLL, or one\n"
             "      built without the extension surface. The stream stays seeded from the\n"
             "      wall clock, so drop --seed and expect two runs to differ, or point\n"
             "      --dll at an oni-sim-replacement build.\n");
      session.Shutdown();
      return 1;
    }
    out::Printf("pinned random stream to 0x%08X\n", seed);
  }

  // Same position and the same reason as the injection below: this changes the world, so a
  // timeline anchored before it would be anchored to a world that no longer exists.
  if (ext_probe) {
    const int n = session.WriteThermalBonusRect(ext_x, ext_y, ext_w, ext_h, ext_jk);
    if (n <= 0) {
      out::Printf("--ext-probe wrote nothing into %d,%d %dx%d\n", ext_x, ext_y, ext_w, ext_h);
      session.Shutdown();
      return 1;
    }
    const std::vector<uint8_t> ext = session.ExtCellState();
    ext_probe_bytes = ext.size();
    out::Printf("ext probe: wrote %.0f J/K into %d cells; checkpoint component is now %zu bytes\n",
           static_cast<double>(ext_jk), n, ext.size());
    if (ext.size() <= 12) {
      // 12 bytes is the bare header. Reaching here means the write did not land in a carried
      // property, and every seek below would then pass for the wrong reason.
      out::Printf("  NOTE: the seventh checkpoint component is still empty -- either this DLL has "
             "no\n        SIM_DebugExtCellState, or the bonus did not land. The timeline "
             "test below\n        cannot tell you anything about that component in this "
             "state.\n");
    }
  }

  // Before the reference run and before any checkpoint: this changes the world, and a
  // timeline whose anchor predates it would be anchored to a world that no longer exists.
  if (inject_gas) {
    const int n = session.InjectGasRect(inject_x, inject_y, inject_w, inject_h,
                                        inject_fraction);
    if (n < 0) {
      out::Printf("the sim refused a zero-length frame while draining the injection\n");
      session.Shutdown();
      return 1;
    }
    if (n == 0) {
      out::Printf("--inject-gas seeded nothing: no gas cell with mass in %d,%d %dx%d\n",
             inject_x, inject_y, inject_w, inject_h);
      session.Shutdown();
      return 1;
    }
    const Digests d = TakeDigests(session);
    out::Printf("injected %d gas cells (%.0f%% of each cell's vanilla mass) into the "
           "volume-fractions layer\n", n, static_cast<double>(inject_fraction) * 100.0);
    out::Printf("  species %zu, digest window %d,%d..%d,%d, mixture now holds %.6f kg\n",
           session.injected_species().size(), session.mixture_x0(), session.mixture_y0(),
           session.mixture_x1(), session.mixture_y1(), d.mixture_mass);
    if (!(d.mixture_mass > 0.0)) {
      out::Printf("  NOTE: the DLL reports no mixture mass -- either it has no "
             "SIM_DebugGasMass\n        export, or the injection did not land. Every "
             "mixture check below is blind.\n");
    }
  }

  // Opened here rather than at the top: everything that changes the world -- the seed, the
  // extension probe, the gas injection -- has now run, so row 0 is the state the run
  // actually starts from. A log anchored before those would claim two runs diverged at tick
  // 0 when what differed was the setup.
  FILE* digest_log_file = nullptr;
  if (digest_log != nullptr) {
    digest_log_file = fopen(digest_log, "wb");
    if (digest_log_file == nullptr) {
      out::Printf("cannot write %s\n", digest_log);
      session.Shutdown();
      return 1;
    }
    char seed_text[64];
    if (pin_seed) {
      snprintf(seed_text, sizeof(seed_text), "0x%08X pinned", seed);
    } else if (session.wall_clock_seeded()) {
      snprintf(seed_text, sizeof(seed_text), "wall-clock 0x%08X", session.RandomState());
    } else {
      snprintf(seed_text, sizeof(seed_text), "unpinned 0x%08X", session.RandomState());
    }
    digestlog::WriteHeader(digest_log_file, dll, corpus, seed_text, session.width(),
                           session.height(), session.cells(), dt);
    out::Printf("digest log: %s, one line per tick\n", digest_log);
    out::Printf("      Hashing every published array and serialising both blobs costs a "
           "fraction of\n      a tick, every tick -- this is a debugging run, not a "
           "rendering one.\n");
    LogDigestRow(digest_log_file, session);
  }
  for (int t = 0; t < ticks; ++t) {
    if (!session.Tick(dt)) {
      out::Printf("tick %d failed: PrepareGameData returned null\n", t);
      if (digest_log_file != nullptr) fclose(digest_log_file);
      session.Shutdown();
      return 1;
    }
    if (digest_log_file != nullptr) LogDigestRow(digest_log_file, session);
  }
  if (digest_log_file != nullptr) {
    fclose(digest_log_file);
    out::Printf("wrote %s (%d rows: tick 0 and %d ticks)\n", digest_log, ticks + 1, ticks);
  }
  if (ticks > 0) {
    // numFramesProcessed is neither a running total nor a substep count, and it is
    // useless as a progress indicator here. oni-sim-replacement pins it to 1 on purpose
    // (sim/simdll.cpp:1966): Klei's sim runs the frame on a worker thread, so a
    // PrepareGameData that finds two frames queued runs both and reports 2, and
    // reproducing that would mean reproducing the scheduler. Ours reports honest
    // one-in-one-out. dt does change the work done -- it buys whole 0.2 s substeps,
    // with the remainder carried -- but that is invisible in this field. Checked by
    // running dt = 0.2, 0.4, 1.0 and 2.0: the field read 1 every time.
    out::Printf("ran %d ticks at dt=%.3f (frames reported per update: %d, pinned by "
           "design)\n",
           session.ticks(), static_cast<double>(dt),
           session.frame()->numFramesProcessed);
  }

  if (load_stress > 0) {
    const int rc = LoadStress(session, load_stress, load_stress_ticks, dt,
                              !bare_load);
    session.Shutdown();
    return rc;
  }

  if (checkpoint_span > 0) {
    const int rc = CheckpointTest(session, checkpoint_span, dt);
    session.Shutdown();
    return rc;
  }

  if (timeline_diff > 0) {
    const int rc =
        TimelineDiff(session, timeline_diff, dt, timeline_interval, timeline_keep);
    session.Shutdown();
    return rc;
  }

  if (timeline_span > 0) {
    const int rc =
        TimelineTest(session, timeline_span, dt, timeline_interval, timeline_keep,
                     ext_probe_bytes);
    session.Shutdown();
    return rc;
  }

  // The decoded extension table is pulled ONCE, here: after every world-changing flag has
  // run and after the ticks, so the readouts below and the window that may follow all read
  // the same world. `RefreshExtProps` copies the whole registry out of the sim, which is
  // why it is a call at a point in the program rather than something an accessor does.
  if (!ext_view_name.empty()) session.SetExtView(ext_view_name, ext_view_comp);
  if (!attr_view_name.empty()) session.SetAttrView(attr_view_name, attr_view_comp);
  // Attached, the fetch is opt-in -- see Session::set_ext_live for the cost. Every switch
  // that cannot do its job without the registry turns it on, so --ext is only ever needed
  // for the window's hover card, which asks for no property by name.
  session.set_ext_live(ext_live || ext_list || probed || view == View::Ext ||
                       ext_blob_path != nullptr);
  // Refused rather than ignored offline. There is no barrier to avoid in this process -- the
  // export is a memcpy here -- so the flag would do nothing, and a flag that silently does
  // nothing is how a measurement gets attributed to the wrong route.
  if (ext_published && !session.attached()) {
    fprintf(stderr, "--ext-published is an attached-only flag: offline the extension registry "
                    "is read through an in-process export with no sim-thread wait to avoid\n");
    return 2;
  }
  if (ext_published) session.set_ext_published(true, ext_published_props);
  // Refused offline for the same reason --ext-published is, and it is a harder no: there is no
  // published frame in this process at all, so there is nothing a stream could be collected
  // from even in principle.
  if ((!event_streams.empty() || events_list) && !session.attached()) {
    fprintf(stderr, "--events and --events-list are attached-only flags: an event stream is a "
                    "per-frame publication and this process publishes no frames\n");
    return 2;
  }
  if (!event_streams.empty()) session.set_event_streams(event_streams);
  session.RefreshExtProps();
  // Registry 2, pulled at the same point and for the same reason -- after everything that
  // could change the world -- but unconditionally rather than behind --ext. `RefreshExtAttrs`
  // says why the two registries cost different things. The attr view, the window's panels
  // and --ext-attrs all read the one result.
  session.RefreshExtAttrs();

  if (ext_blob_path != nullptr) {
    // The bytes, not a rendering of them: this is what tools/attach-stub.py serves back as
    // /extcells, and what a bug report about a property should carry.
    const std::vector<uint8_t> blob = session.ExtCellStateBlob();
    FILE* f = fopen(ext_blob_path, "wb");
    if (f == nullptr) {
      out::Printf("cannot write %s\n", ext_blob_path);
      session.Shutdown();
      return 1;
    }
    if (!blob.empty()) fwrite(blob.data(), 1, blob.size(), f);
    fclose(f);
    out::Printf("wrote %zu extension-blob bytes to %s\n", blob.size(), ext_blob_path);
  }

  // Refused rather than drawn flat. A property name with a typo in it, or one nothing has
  // written, would otherwise paint the dim phase backdrop over the whole world and read as
  // "this world has none of that", which is exactly the wrong conclusion to hand someone
  // debugging a property they believe they just wrote.
  // The same refusal for registry 2, and it has one more way to be empty than registry 1
  // does: an attribute whose values are all keyed to elements this world's table has no row
  // for is carried, correct, and invisible. Drawn flat it would read as "this world has none
  // of that", which is the wrong conclusion in a different direction from a typo.
  if (view == View::Attr && !ViewAvailableIn(session, view)) {
    const extattrs::Table& at = session.ExtAttrs();
    const extattrs::Attribute* a = at.exported ? at.Find(attr_view_name) : nullptr;
    out::Printf("\n");
    if (!at.exported) {
      out::Printf("cannot draw attr:%s -- %s\n", attr_view_name.c_str(), at.error.c_str());
    } else if (a == nullptr) {
      out::Printf("no per-element attribute named \"%s\" is registered.\n"
                  "  %zu registered; --ext-attrs names them.\n",
                  attr_view_name.c_str(), at.attrs.size());
    } else if (attr_view_comp < 0 || attr_view_comp >= a->arity) {
      out::Printf("attribute \"%s\" has arity %d, so there is no component %d.\n",
                  attr_view_name.c_str(), a->arity, attr_view_comp);
    } else {
      out::Printf("attribute \"%s\" is registered and no element of THIS table carries it.\n",
                  attr_view_name.c_str());
      if (session.attr_off_table() > 0) {
        out::Printf("  %d value%s held for element id%s this table has no row for. That is a "
                    "real\n  state -- keys are SimHashes ids, so a value outlives the table it "
                    "was\n  written for -- and no cell can show one.\n",
                    session.attr_off_table(), session.attr_off_table() == 1 ? " is" : "s are",
                    session.attr_off_table() == 1 ? "" : "s");
      } else {
        out::Printf("  %d value%s, %llu write%s -- see --ext-attrs. Nothing in this registry "
                    "is ever\n  saved, so an offline session shows only what the SimDLL "
                    "pushes for itself.\n",
                    a->value_count, a->value_count == 1 ? "" : "s",
                    static_cast<unsigned long long>(a->writes), a->writes == 1 ? "" : "s");
      }
    }
    session.Shutdown();
    return 1;
  }

  if (view == View::Ext && !ViewAvailableIn(session, view)) {
    const extprops::Table& ex = session.ExtProps();
    const oni_sim::SaveExtProperty* p = ex.decoded ? ex.Find(ext_view_name) : nullptr;
    if (p != nullptr) {
      out::Printf("\nextension property \"%s\" has arity %d, so there is no component %d.\n",
             ext_view_name.c_str(), p->arity, ext_view_comp);
    } else {
      out::Printf("\nno extension property named \"%s\" is being carried.\n",
             ext_view_name.c_str());
      if (!ex.exported || !ex.decoded) {
        out::Printf("  %s\n", ex.error.c_str());
      } else {
        out::Printf("  %zu carried; --ext-list names them. A property at its default in every "
               "cell is\n  omitted by the sim and cannot be told from one that was never "
               "registered.\n", ex.props.size());
      }
    }
    session.Shutdown();
    return 1;
  }

  Range range;
  if (fixed_range) {
    range.lo = range_lo;
    range.hi = range_hi;
    range.valid = true;
  } else {
    range = ScanRange(session, view, full_span);
  }
  if (view == View::Temperature || view == View::Mass || view == View::Ext ||
      view == View::Attr) {
    // The attr view's default band is the full span by construction, so saying "10..90
    // percentile" there would name a band it never uses. views.h's `ScanRange` measured why.
    const char* how = fixed_range
                          ? " (fixed)"
                          : (full_span || view == View::Attr
                                 ? " (scanned, full span)"
                                 : " (scanned, 10..90 percentile)");
    out::Printf("%s range over non-empty cells: %.3f .. %.3f%s\n", ViewLabel(session, view).c_str(),
           range.lo, range.hi, how);
    // A component that is carried but empty draws as pure backdrop, which is the same
    // picture as a property nobody registered. The refusal above catches the name and the
    // arity; this catches the third way to get an empty view, and says which it is.
    if (view == View::Ext && !range.valid) {
      out::Printf("  NOTE: every cell holds the registered default for that component, so the "
             "view is\n        entirely backdrop. The property IS carried -- see --ext-list "
             "-- this one\n        component of it is empty.\n");
    }
    // The attr view's two ways to be misread, both of which look like a working picture.
    if (view == View::Attr && !range.valid) {
      out::Printf("  NOTE: %d element%s carr%s this attribute and none of them occupies a "
             "cell in\n        this world, so the view is entirely backdrop.\n",
             session.attr_elements(), session.attr_elements() == 1 ? "" : "s",
             session.attr_elements() == 1 ? "ies" : "y");
    } else if (view == View::Attr && session.attr_lo() == session.attr_hi()) {
      out::Printf("  NOTE: every element carrying this attribute holds the same value (%g), so "
             "the\n        ramp has nothing to span and every lit cell is one colour. The "
             "range above\n        was widened to a unit band by the scan; the data is a "
             "single value.\n", static_cast<double>(session.attr_lo()));
    }
    // Said whenever it is true and not only when the view is empty, because this is the one
    // way the picture can be INCOMPLETE while looking entirely correct: those values are real,
    // they are in --ext-attrs, and no cell can show one. Silence here would let a reader
    // conclude the world has none of an element the registry is holding a value for.
    if (view == View::Attr && session.attr_off_table() > 0) {
      if (session.attr_off_table() == 1) {
        out::Printf("  NOTE: one further value is held for an element id this table has no row "
               "for, so\n        it is not drawn anywhere. Keys are SimHashes ids, so a value "
               "outlives the\n        table it was written for; --ext-attrs shows it as \"not "
               "in table\".\n");
      } else {
        out::Printf("  NOTE: %d further values are held for element ids this table has no row "
               "for, so\n        none of them is drawn anywhere. Keys are SimHashes ids, so a "
               "value outlives\n        the table it was written for; --ext-attrs shows them "
               "as \"not in table\".\n", session.attr_off_table());
      }
    }
  }

  if (dump_state) {
    const std::string source =
        session.attached() ? (session.source().host() + ":" + session.source().port())
                           : std::string(dll);
    if (!DumpState(session, dump_state, source)) out::Printf("could not write %s\n", dump_state);
  }
  if (digest) Digest(session);
  if (ext_list) ExtList(session);
  if (ext_attrs) AttrList(session);
  if (events_list) EventsList(session);
  if (!event_streams.empty() && !windowed) EventsOnce(session);
  if (stats) PrintQuantiles(session, view);
  if (probed) Probe(session, probe_x, probe_y, probe_w, probe_h);

  if (windowed || ui_sample) {
    WindowOptions opt;
    opt.dt = dt;
    opt.mass_fade = mass_fade;
    opt.fixed_range = fixed_range;
    opt.full_span = full_span;
    opt.fixed = range;
    opt.seed_pinned = pin_seed;
    opt.seed = seed;
    opt.corpus = corpus ? corpus : "";
    opt.timeline = timeline;
    opt.timeline_interval = timeline_interval;
    opt.timeline_keep = timeline_keep;
    opt.attached = session.attached();
    if (session.attached()) {
      opt.endpoint = session.source().host() + ":" + session.source().port();
      opt.poll_ms = poll_ms;
      opt.fetch_pinned = attach_arrays != nullptr || digest || dump_state != nullptr;
      opt.fetch_set = fetch;
    }
    if (sized) {
      opt.width = size_w;
      opt.height = size_h;
    }
    opt.ui_scale = ui_scale;
    opt.fullscreen = fullscreen;
    opt.camera.explicit_camera = camera_set;
    opt.camera.cx = camera_cx;
    opt.camera.cy = camera_cy;
    opt.camera.scale = camera_scale;
    opt.camera.zoom = zoom;
    opt.camera.hovered = hovered;
    opt.camera.hover_x = hover_x;
    opt.camera.hover_y = hover_y;
    opt.camera.cropped = cropped;
    opt.camera.crop_x = crop_x;
    opt.camera.crop_y = crop_y;
    opt.camera.crop_w = crop_w;
    opt.camera.crop_h = crop_h;
    if (ui_sample) {
      const int rc = qtui::WindowSample(session, view, opt, snapshot);
      session.Shutdown();
      return rc;
    }
    const int rc = RunWindow(session, view, opt);
    // The source dock asked for another world. Everything the start screen needs is read out
    // BEFORE the shutdown, because a session that has released its DLL is not a session to
    // ask questions of.
    launch::Spec spec;
    if (rc == kRelaunch) {
      spec.attach = session.attached();
      spec.ext_name = session.ext_view_name();
      spec.ext_comp = session.ext_view_comp();
      spec.attr_name = session.attr_view_name();
      spec.attr_comp = session.attr_view_comp();
    }
    session.Shutdown();
    if (rc == kRelaunch) {
      spec.corpus = corpus ? corpus : "";
      spec.dll = dll;
      spec.endpoint = opt.endpoint;
      spec.poll_ms = poll_ms;
      spec.attach_timeout_ms = attach_timeout_ms;
      if (pin_seed) {
        char seed_text[32];
        snprintf(seed_text, sizeof(seed_text), "0x%08X", seed);
        spec.seed = seed_text;
      }
      spec.ticks = ticks;
      spec.dt = dt;
      spec.timeline = timeline;
      spec.mass_fade = mass_fade;
      spec.view = view;
      spec.ext_live = ext_live;
      spec.ext_published = ext_published;
      spec.ext_props = ext_published_props;
      spec.events = event_streams;
      spec.width = opt.width;
      spec.height = opt.height;
      return LauncherLoop(spec);
    }
    return rc;
  }

  if (snapshot) {
    Image img;
    RenderWorld(session, view, range, &img, mass_fade);

    if (hud_snapshot) {
      // The composed frame: exactly what the window shows, drawn with no window. This
      // is how the overlay is checked -- see font.h. The camera comes from `SeatCamera`,
      // which the window calls too; see frame.h for why that has to be one function.
      const int fw = sized ? size_w : img.width();
      const int fh = sized ? size_h : img.height();
      CameraSpec cspec;
      cspec.explicit_camera = camera_set;
      cspec.cx = camera_cx;
      cspec.cy = camera_cy;
      cspec.scale = camera_scale;
      cspec.zoom = zoom;
      cspec.hovered = hovered;
      cspec.hover_x = hover_x;
      cspec.hover_y = hover_y;
      cspec.cropped = cropped;
      cspec.crop_x = crop_x;
      cspec.crop_y = crop_y;
      cspec.crop_w = crop_w;
      cspec.crop_h = crop_h;
      WindowOptions sopt;
      sopt.dt = dt;
      sopt.mass_fade = mass_fade;
      sopt.fixed_range = fixed_range;
      sopt.full_span = full_span;
      sopt.fixed = range;
      sopt.seed_pinned = pin_seed;
      sopt.seed = seed;
      sopt.corpus = corpus ? corpus : "";
      sopt.attached = session.attached();
      if (session.attached())
        sopt.endpoint = session.source().host() + ":" + session.source().port();
      sopt.ui_scale = ui_scale;
      sopt.camera = cspec;
      img = qtui::ComposeSnapshot(session, view, sopt, fw, fh, hovered ? hover_x : -1,
                                  hovered ? hover_y : -1);
    } else {
      if (cropped) img = img.Cropped(crop_x, crop_y, crop_w, crop_h);
      if (zoom > 1) img = img.Magnified(zoom);
    }

    if (!WritePng(img, snapshot)) {
      out::Printf("could not write %s\n", snapshot);
      session.Shutdown();
      return 1;
    }
    out::Printf("wrote %s (%dx%d, view=%s%s)\n", snapshot, img.width(), img.height(),
           ViewLabel(session, view).c_str(), hud_snapshot ? ", composed with HUD" : "");
  } else {
    if (!probed && !ext_list && !ext_attrs && ext_blob_path == nullptr &&
        dump_state == nullptr && digest_log == nullptr) {
      out::Printf("nothing to do: pass --window, --snapshot <out.png>, or --probe x,y\n");
    }
  }

  session.Shutdown();
  return 0;
}
