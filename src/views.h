// Turning one frame of sim state into pixels.
//
// Every view is a pure function of `GameDataUpdate` plus the element table, so the same
// code paints a snapshot PNG and a window, and a view added here appears in both.
//
// Grid orientation: an ONI cell index is `y * width + x` with y = 0 at the BOTTOM of the
// world. Image rows run top-down. Every view therefore flips y, and getting that wrong
// renders a world that looks plausible and is upside down.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstdint>
#include <string>

#include "out.h"
#include "image.h"
#include "session.h"
// Klei's cell validator, from oni-sim-replacement/driver/src (already on the include path for
// simhost.h). Included, not copied: a second transcription is a second thing to keep right.
#include "simcheck.h"

namespace simviz {

// The first four views read the grid arrays; the last three read the PROPERTY TEXTURES,
// which are the buffers the game hands to Texture2D rather than state of their own. They are
// worth drawing for exactly that reason: they are derived from the grid by a pass with its
// own cache (sim/textures.h), so "the grid is right" and "the picture the game will draw is
// right" are two different claims, and only these three views can check the second one.
// `Ext` is the eighth and is not like the other seven: the other seven each name a member
// of `GameDataUpdate` that was typed out in this repo, and `Ext` names whatever per-cell
// EXTENSION PROPERTY asked for by name. Which property is not part of the enum --
// it is carried on the Session (`ext_view_name`), the same object every renderer here
// already takes -- because the set of them is the sim's to decide and can change without
// this file. See extprops.h.
//
// `Attr` is the ninth and is the same idea aimed at the OTHER registry. `Ext` draws a value
// the sim holds per cell; `Attr` draws a value the sim holds per ELEMENT (extattrs.h,
// registry 2), looked up through the cell's `elementIdx`. Registry 2 had a listing
// (--ext-attrs) and no picture at all before this, so "which parts of this world carry
// sim.molecular_mass" was a question the tool could not answer spatially. What it draws is
// never finer than the element map, because the data is not: two cells of the same element
// always take the same colour, and that is the shape of the answer rather than a limitation
// of the drawing.
// SimCheck is Klei's own SimCheckErrorMap overlay, drawn here. It is the one view in this
// list whose colours are not this repo's: `oni-sim-replacement/driver/src/simcheck.h` is the
// game's `SimDebugView.GetSimCheckErrorMapColour` transcribed, included from the fork rather
// than copied, so the picture cannot drift from what the game draws. The overlay answers "is
// anything in this world impossible" -- NaN mass or temperature, over 10 t or 10 000 K, under
// 10 K, vacuum holding something -- which is a question no other view here asks. See the
// class table in that header for what each colour means, and note that a sealed world's
// neutronium border is 20 000 kg a cell and therefore red all the way round.
enum class View { Element, Temperature, Mass, Phase, Flow, Liquid, Sunlight, Ext, Attr, SimCheck };

inline const char* ViewName(View v) {
  switch (v) {
    case View::Element: return "element";
    case View::Temperature: return "temperature";
    case View::Mass: return "mass";
    case View::Phase: return "phase";
    case View::Flow: return "flow";
    case View::Liquid: return "liquid";
    case View::Sunlight: return "sunlight";
    case View::Ext: return "ext";
    case View::Attr: return "attr";
    case View::SimCheck: return "simcheck";
  }
  return "?";
}

// `<name>` or `<name>[c]`, the tail of `ext:` and `attr:` alike. Shared because the two
// selectors differ only in which registry the name belongs to, and a second copy of this
// would be a second place for `[2` with no bracket to be accepted.
inline bool ParseNamedView(const std::string& in, std::string* name, int* comp) {
  std::string body = in;
  int c = 0;
  const size_t br = body.find('[');
  if (br != std::string::npos) {
    if (body.empty() || body.back() != ']') return false;
    const std::string digits = body.substr(br + 1, body.size() - br - 2);
    if (digits.empty()) return false;
    for (char ch : digits) {
      if (ch < '0' || ch > '9') return false;
    }
    c = std::atoi(digits.c_str());
    body = body.substr(0, br);
  }
  if (body.empty()) return false;
  *name = body;
  *comp = c;
  return true;
}

// `ext:<name>[c]` selects a per-cell extension property and `attr:<name>[c]` a per-element
// attribute; `c` is the component of one whose arity is above one, and defaults to 0.
// Neither name is validated here -- nothing is registered until the sim has booted, and
// refusing at parse time would mean this file holding a list of names out of the sim, which
// is the one thing it must not do. Both are refused later, by name, with the reason.
inline bool ParseView(const std::string& s, View* out, std::string* ext_name = nullptr,
                      int* ext_comp = nullptr, std::string* attr_name = nullptr,
                      int* attr_comp = nullptr) {
  if (s == "element") { *out = View::Element; return true; }
  if (s == "temperature" || s == "temp") { *out = View::Temperature; return true; }
  if (s == "mass") { *out = View::Mass; return true; }
  if (s == "phase") { *out = View::Phase; return true; }
  if (s == "flow") { *out = View::Flow; return true; }
  if (s == "liquid") { *out = View::Liquid; return true; }
  if (s == "sunlight" || s == "sun") { *out = View::Sunlight; return true; }
  if (s == "simcheck" || s == "errormap") { *out = View::SimCheck; return true; }
  if (s.compare(0, 4, "ext:") == 0 && ext_name != nullptr && ext_comp != nullptr) {
    if (!ParseNamedView(s.substr(4), ext_name, ext_comp)) return false;
    *out = View::Ext;
    return true;
  }
  if (s.compare(0, 5, "attr:") == 0 && attr_name != nullptr && attr_comp != nullptr) {
    if (!ParseNamedView(s.substr(5), attr_name, attr_comp)) return false;
    *out = View::Attr;
    return true;
  }
  return false;
}

// True for the views drawn from a property texture rather than from the grid arrays.
inline bool IsTextureView(View v) {
  return v == View::Flow || v == View::Liquid || v == View::Sunlight;
}

// True for the view drawn from the extension registry. It shares the texture views'
// freedom from the "vacuum first" rule -- an extension property can mean something on a
// cell holding no mass, and a per-cell flag usually does -- without being a texture.
inline bool IsExtView(View v) { return v == View::Ext; }

// True for the view drawn from the per-element attribute registry. It shares the ext view's
// freedom from the "vacuum first" rule for a different reason: an attribute belongs to an
// ELEMENT, and Vacuum is an element, so a value registered against it is real data about
// every empty cell in the world rather than a reading of nothing.
inline bool IsAttrView(View v) { return v == View::Attr; }

// True for Klei's error map. It shares the ext view's freedom from the "vacuum first" rule
// for the most direct reason of all: three of its eight classes describe a cell with no mass
// in it, and two of those three are faults.
inline bool IsSimCheckView(View v) { return v == View::SimCheck; }

// Whether the DLL published the buffer a view needs. A texture view over a null pointer
// would silently paint an empty world, which is the one failure a debugger must not have.
inline bool ViewAvailable(const GameDataUpdate* g, View v) {
  if (!g) return false;
  switch (v) {
    case View::Flow: return g->propertyTextureFlow != nullptr;
    case View::Liquid: return g->propertyTextureLiquid != nullptr;
    case View::Sunlight: return g->propertyTextureExposedToSunlight != nullptr;
    // Ext cannot be answered from the frame: the extension registry is not in
    // `GameDataUpdate` at all. `ViewAvailableIn` below is the version that can.
    case View::Ext: return false;
    // Nor Attr: the attribute registry is not in `GameDataUpdate` either. Same answer,
    // separately spelled, so that adding a view cannot inherit one of them by accident.
    case View::Attr: return false;
    // SimCheck needs only the three arrays every view needs, and the element table -- both
    // of which attach mode fills as completely as a corpus run does, since /elements carries
    // lowTemp, highTemp and the two transition indices.
    case View::SimCheck: return true;
    default: return true;
  }
}

// The same question asked of the whole session rather than of one frame, which is the only
// form that can answer it for `Ext`: whether the named property is one the sim is actually
// carrying. False for a name with a typo in it, for a property that is at its default in
// every cell (the sim omits those from the blob), and for an attached session that was not
// told to fetch `/extcells` -- all of them being reasons a view would otherwise paint a flat
// world and look like a sim with nothing in it.
inline bool ViewAvailableIn(const Session& s, View v) {
  if (v == View::Ext) {
    if (!s.ExtProps().decoded) return false;
    const oni_sim::SaveExtProperty* p = s.ExtProps().Find(s.ext_view_name());
    // The component is checked here and not only where it is read. `ScalarAt` answers an
    // out-of-range component with the property's DEFAULT, which is the right answer for a
    // reader and the wrong one for a view: every cell then equals the default, the whole
    // world draws as backdrop, and `--view ext:sim.gas_mass[9]` on an arity-8 property
    // renders "this world has no gas" instead of "there is no component 9".
    return p != nullptr && s.ext_view_comp() >= 0 && s.ext_view_comp() < p->arity;
  }
  if (v == View::Attr) {
    // FOUR ways this is false and the caller prints which, because they call for different
    // things: this DLL cannot enumerate registry 2 at all (stock, or older than the
    // enumeration exports); no attribute goes by that name; the component is past the arity; and the one that
    // is easiest to mistake for "this world has none" -- every value the attribute holds is
    // keyed to an element this world's table has no row for, which is a legitimate state
    // (keys are ids so a value outlives its table) and paints an entirely backdrop world.
    if (!s.ExtAttrs().exported) return false;
    const extattrs::Attribute* a = s.ExtAttrs().Find(s.attr_view_name());
    if (a == nullptr) return false;
    if (s.attr_view_comp() < 0 || s.attr_view_comp() >= a->arity) return false;
    return s.attr_elements() > 0;
  }
  return ViewAvailable(s.frame(), v);
}

// What to call the view on screen and in a message. Identical to `ViewName` for the seven
// fixed views; for `Ext` it is the property, minus its owner prefix so the HUD column does
// not have to fit "sim.thermal_mass_bonus", plus the component when there is a choice.
inline std::string ViewLabel(const Session& s, View v) {
  if (v == View::Attr) {
    std::string n = s.attr_view_name();
    const size_t dot = n.find('.');
    if (dot != std::string::npos && dot + 1 < n.size()) n = n.substr(dot + 1);
    const extattrs::Attribute* a = s.ExtAttrs().Find(s.attr_view_name());
    if (a != nullptr && a->arity > 1) n += "[" + std::to_string(s.attr_view_comp()) + "]";
    return n;
  }
  if (v != View::Ext) return ViewName(v);
  std::string n = s.ext_view_name();
  const size_t dot = n.find('.');
  if (dot != std::string::npos && dot + 1 < n.size()) n = n.substr(dot + 1);
  const oni_sim::SaveExtProperty* p = s.ExtProps().Find(s.ext_view_name());
  if (p != nullptr && p->arity > 1) n += "[" + std::to_string(s.ext_view_comp()) + "]";
  return n;
}

// The flow texture's two floats for one game cell: x is right-positive, y is up-positive,
// both already divided by the cell's mass (sim/textures.h, `FillFlowTexture`), so the value
// is a transfer per kilogram and a heavy cell reads far smaller than a light one moving the
// same way.
inline void FlowAt(const GameDataUpdate* g, int cell, float* fx, float* fy) {
  *fx = *fy = 0.0f;
  if (!g || !g->propertyTextureFlow) return;
  const float* f = static_cast<const float*>(g->propertyTextureFlow);
  *fx = f[static_cast<size_t>(cell) * 2];
  *fy = f[static_cast<size_t>(cell) * 2 + 1];
}

inline float FlowMagnitude(const GameDataUpdate* g, int cell) {
  float fx, fy;
  FlowAt(g, cell, &fx, &fy);
  return std::sqrt(fx * fx + fy * fy);
}

inline uint8_t SunlightAt(const GameDataUpdate* g, int cell) {
  if (!g || !g->propertyTextureExposedToSunlight) return 0;
  return static_cast<const uint8_t*>(g->propertyTextureExposedToSunlight)[cell];
}

// One RGBA32 word out of a property texture, in the byte order the sim stores it: r, g, b, a
// from low byte to high (sim/textures.h `store`), which is the same order Unity reads.
inline uint32_t TextureWord(const void* tex, int cell) {
  if (!tex) return 0;
  return static_cast<const uint32_t*>(tex)[cell];
}

// Element.colour is a Unity Color32 seen as a little-endian uint32, which puts the
// bytes in memory as r, g, b, a and therefore reads back as 0xAABBGGRR: the LOW byte is
// red. MEASURED with --dump-elements against the corpus's own table, on
// three elements whose colour is not in doubt --
//
//   Water  0xFFD2BE55 -> (85,190,210) light blue,   the other reading gives tan
//   Sand   0xFF94B5D6 -> (214,181,148) tan,         the other reading gives blue
//   Oxygen 0x85E7C02E -> (46,192,231) pale blue,    the other reading gives gold
//
// The wrong reading swaps red and blue: silent on grey rock, obvious on water.
//
// The alpha byte is real data, not padding -- Oxygen is 0x85, and several elements are
// 0x00. ONI draws gases translucent. This view is opaque and ignores it, which is a
// deliberate simplification and not a reading of alpha as meaningless.
inline Rgb ElementColour(uint32_t packed) {
  return Rgb{static_cast<uint8_t>(packed & 0xFF),
             static_cast<uint8_t>((packed >> 8) & 0xFF),
             static_cast<uint8_t>((packed >> 16) & 0xFF)};
}

// Black -> blue -> cyan -> green -> yellow -> red -> white. Monotonic in luminance for
// most of its range, so a hot region reads as hot in a greyscale print too.
inline Rgb Thermal(float t01) {
  const float t = std::min(1.0f, std::max(0.0f, t01));
  struct Stop { float at; float r, g, b; };
  static const Stop stops[] = {
      {0.00f, 0.00f, 0.00f, 0.10f}, {0.20f, 0.10f, 0.15f, 0.75f},
      {0.40f, 0.10f, 0.75f, 0.80f}, {0.55f, 0.15f, 0.80f, 0.25f},
      {0.72f, 0.95f, 0.90f, 0.15f}, {0.88f, 0.95f, 0.25f, 0.10f},
      {1.00f, 1.00f, 1.00f, 1.00f},
  };
  const int n = sizeof(stops) / sizeof(stops[0]);
  for (int i = 1; i < n; ++i) {
    if (t <= stops[i].at) {
      const Stop& a = stops[i - 1];
      const Stop& b = stops[i];
      const float span = b.at - a.at;
      const float f = span > 0.0f ? (t - a.at) / span : 0.0f;
      return Rgb{static_cast<uint8_t>((a.r + (b.r - a.r) * f) * 255.0f),
                 static_cast<uint8_t>((a.g + (b.g - a.g) * f) * 255.0f),
                 static_cast<uint8_t>((a.b + (b.b - a.b) * f) * 255.0f)};
    }
  }
  return Rgb{255, 255, 255};
}

// Vacuum is drawn as a near-black that is deliberately NOT pure black, so "no cell
// data" (black) and "a cell holding nothing" stay distinguishable in a snapshot.
inline Rgb VacuumColour() { return Rgb{14, 14, 20}; }

// How solidly a cell is drawn, from how much matter is actually in it.
//
// This exists because of a measured failure, not a preference. Rendering temperature
// with no regard for mass makes a trace gas indistinguishable from rock: over 200 ticks
// on the reference corpus a one-cell column of polluted oxygen expanded across the
// vacuum to 0.012-0.027 kg per cell, and the temperature view painted that whole region
// as boldly as a 900 kg ice shelf, because both simply have mass > 0 at ~300 K. A
// thermal debugger that shows 0.012 kg and 900 kg identically is worse than no view.
//
// Full strength at 1 kg and above -- roughly one cell of gas at ONI's normal pressure --
// fading to nearly nothing at 0.001 kg. Solids and liquids are never affected.
inline float MassOpacity(float mass) {
  if (mass >= 1.0f) return 1.0f;
  if (mass <= 0.001f) return 0.06f;
  const float t = (std::log10(mass) + 3.0f) / 3.0f;  // 0.001 kg -> 0, 1 kg -> 1
  return 0.06f + 0.94f * t;
}

inline Rgb Blend(Rgb fg, Rgb bg, float a) {
  return Rgb{static_cast<uint8_t>(bg.r + (fg.r - bg.r) * a),
             static_cast<uint8_t>(bg.g + (fg.g - bg.g) * a),
             static_cast<uint8_t>(bg.b + (fg.b - bg.b) * a)};
}

struct Range {
  float lo = 0.0f, hi = 1.0f;
  bool valid = false;
};

// The scalar a view ranges and quantiles over. Element, phase and liquid have none -- they
// are categorical or already a colour -- and fall back to mass, which is what the range scan
// wants for the cells-with-anything-in-them test anyway.
inline float ViewScalar(const GameDataUpdate* g, View view, int i) {
  switch (view) {
    case View::Temperature: return g->temperature[i];
    case View::Flow: return FlowMagnitude(g, i);
    case View::Sunlight: return static_cast<float>(SunlightAt(g, i));
    default: return g->mass[i];
  }
}

// The selected extension property at one GAME cell. The registry is indexed on the padded
// grid -- the sim's one-cell border ring -- so this is where that conversion happens for
// every scalar the ext view produces; `extprops.h` explains why reading it wrong would be
// a plausible-looking wrong number rather than an error.
inline float ExtScalarAt(const Session& s, int cell) {
  const extprops::Table& ex = s.ExtProps();
  const oni_sim::SaveExtProperty* p = ex.Find(s.ext_view_name());
  if (p == nullptr || s.width() <= 0) return 0.0f;
  const int w = s.width();
  return static_cast<float>(extprops::ScalarAt(
      *p, extprops::PaddedIndex(cell % w, cell / w, ex.padded_w), s.ext_view_comp()));
}

// The selected element attribute at one game cell: the cell's element, then that element's
// value. `Session::ResolveAttrView` has already flattened the registry onto the element
// table, so this is an array index and not a search -- which is the whole reason the
// flattening exists, at 256,944 cells per frame.
inline float AttrScalarAt(const Session& s, int cell) {
  const GameDataUpdate* g = s.frame();
  if (g == nullptr) return 0.0f;
  return s.AttrAt(g->elementIdx[cell]);
}

// Whether the cell's element carries a value at all. SEPARATE from the scalar on purpose:
// unset and zero are different answers here, and unlike registry 1 -- where the sim omits a
// property sitting at its registered default and the two become indistinguishable -- this
// registry says outright which elements have one. Losing that distinction would draw an
// element whose molecular mass is genuinely 0 exactly like one nobody ever pushed.
inline bool AttrSetAt(const Session& s, int cell) {
  const GameDataUpdate* g = s.frame();
  return g != nullptr && s.AttrHas(g->elementIdx[cell]);
}

// Every scalar view through one call. The frame-only overload above still exists and is
// still correct for the seven fixed views; this is the one every caller with a Session in
// hand uses, because it is the only one that can answer for `Ext` and `Attr`.
inline float ViewScalar(const Session& s, View view, int i) {
  if (IsExtView(view)) return ExtScalarAt(s, i);
  if (IsAttrView(view)) return AttrScalarAt(s, i);
  return ViewScalar(s.frame(), view, i);
}

// The registered default of the property the ext view is showing, and whether there is one
// at all. Cells sitting at the default are what the ext view draws as backdrop and what
// `ScanRange` leaves out of the band: a property written into 1,600 cells of 259,028 would
// otherwise range over 257,428 zeros and put every written cell in the top bucket.
inline const oni_sim::SaveExtProperty* ExtViewProperty(const Session& s) {
  return s.ExtProps().Find(s.ext_view_name());
}

// Direction as hue, magnitude as brightness. Hue runs right = red, up = green, left = cyan,
// down = magenta, so a rising plume and a falling one are opposite colours rather than two
// shades of one -- which is the whole reason to draw flow as a field instead of a heatmap.
inline Rgb FlowColour(float fx, float fy, float mag01) {
  const float m = std::min(1.0f, std::max(0.0f, mag01));
  if (fx == 0.0f && fy == 0.0f) return VacuumColour();
  float angle = std::atan2(fy, fx);                 // -pi .. pi
  if (angle < 0.0f) angle += 6.28318530718f;
  const float h = angle / 6.28318530718f * 6.0f;    // 0 .. 6
  const int sector = static_cast<int>(h) % 6;
  const float f = h - std::floor(h);
  // Full saturation, value from the magnitude, with a floor so a slow cell is dim rather
  // than invisible: the direction is the reading, and a cell that moved at all should show
  // which way.
  const float v = 0.25f + 0.75f * m;
  const float p = 0.0f, q = v * (1.0f - f), t = v * f;
  float r, g, b;
  switch (sector) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
  return Rgb{static_cast<uint8_t>(r * 255.0f), static_cast<uint8_t>(g * 255.0f),
             static_cast<uint8_t>(b * 255.0f)};
}

// Scan for the spread of a scalar, ignoring cells with no mass: a world is mostly
// vacuum, and letting vacuum set the low end flattens everything else into one colour.
//
// The default is the 10th..90th PERCENTILE, not the absolute min and max. Full range is
// the obvious choice and it is the wrong one: every ONI world has magma, so a single
// 1967 K pocket compresses the entire habitable asteroid -- the part anyone is looking
// at -- into one flat blue.
//
// The band was picked from the measured distribution, not from reasoning about it. A
// first attempt at 2..98 was tried and FAILED, returning 1.0 .. 1908 K, because on the
// reference corpus (--view temperature --stats):
//
//   p0 .. p5    1.0 K        a cluster's eight asteroid borders are 1 K neutronium,
//                            which is ~7% of all cells that hold any mass
//   p10         146.2 K
//   p50         294.7 K
//   p90         322.3 K
//   p95 .. p100 1650 .. 1967.5 K   magma, another ~5-10%
//
// so neither tail is thin enough for a 2% trim to clear it. p10..p90 spans 146..322 K
// and puts the whole terrain across the ramp; magma clamps to white and the border
// clamps to black, which is legible rather than lost. Pass --range full for the
// absolute span, or --range lo,hi to pin it.
inline Range ScanRange(const Session& s, View view, bool full_span) {
  Range r;
  const GameDataUpdate* g = s.frame();
  if (!g) return r;
  const int n = s.cells();
  std::vector<float> values;
  values.reserve(static_cast<size_t>(n) / 4);
  // The ext view ranges over the cells that are NOT at the property's default, for the
  // reason given at `ExtViewProperty`. A property nothing has written ranges over nothing
  // and leaves `r.valid` false, which is the same answer an empty world gives.
  const oni_sim::SaveExtProperty* ext_p = IsExtView(view) ? ExtViewProperty(s) : nullptr;
  const double ext_def = ext_p != nullptr ? extprops::DefaultValue(*ext_p) : 0.0;
  const bool attr = IsAttrView(view);
  for (int i = 0; i < n; ++i) {
    // The mass gate is for the GRID views, where a cell holding nothing has no scalar worth
    // ranging. It does not apply to the textures: a flow cell can hold 0.000000 kg and still
    // publish a transfer -- the corpus has them, at the leading edge of an expanding gas --
    // and sunlight is at full brightness across vacuum, which is most of what it covers.
    // Nor to the ext view: a per-cell flag on a vacuum cell is exactly the kind of thing
    // this is for. Nor to the attr view: Vacuum is an element and may carry an attribute.
    if (!IsTextureView(view) && !IsExtView(view) && !attr && g->mass[i] <= 0.0f) continue;
    const float v = ViewScalar(s, view, i);
    if (attr) {
      // Unset, not zero -- see `AttrSetAt`. A cell whose element carries nothing is not in
      // the band for the same reason a cell at a property's default is not.
      if (!AttrSetAt(s, i)) continue;
    } else if (IsExtView(view)) {
      if (ext_p == nullptr || static_cast<double>(v) == ext_def) continue;
    } else if (!(v > 0.0f)) {
      continue;
    }
    values.push_back(v);
  }
  if (values.empty()) return r;
  r.valid = true;
  // THE ATTR VIEW'S BAND IS ALWAYS THE FULL SPAN, and this is the one place the per-element
  // shape of the data changes an answer rather than only the picture. A percentile band over
  // CELLS is a percentile over AREA, and an attribute has one value per element however many
  // cells that element happens to fill. On the reference corpus `sim.molecular_mass` holds
  // four values -- hydrogen 2.01588, oxygen and polluted oxygen 31.9988, chlorine 70.906 --
  // and oxygen alone covers so much of the map that p10..p90 collapses onto its single value
  // (measured: p10 = p90 = 31.9988). `ScanRange` would then widen a zero-width band to
  // lo..lo+1, hydrogen and chlorine would clamp to the two ends, and every element in between
  // -- which is the entire question the view is asked -- would be one flat colour.
  //
  // Ranging over what is actually reachable puts those four values across the ramp instead.
  // `--range lo,hi` still pins it, and `--range full` is what this already does.
  if (full_span || attr) {
    r.lo = *std::min_element(values.begin(), values.end());
    r.hi = *std::max_element(values.begin(), values.end());
  } else {
    const size_t lo_i = static_cast<size_t>(values.size() * 0.10);
    const size_t hi_i = static_cast<size_t>(values.size() * 0.90);
    std::nth_element(values.begin(), values.begin() + lo_i, values.end());
    r.lo = values[lo_i];
    std::nth_element(values.begin(), values.begin() + hi_i, values.end());
    r.hi = values[hi_i];
  }
  if (r.hi <= r.lo) r.hi = r.lo + 1.0f;
  return r;
}

// Quantiles of the scalar over non-empty cells. Exists because a range chosen by
// reasoning about what a world "should" contain was wrong on the first world tried:
// the 2..98 band came back 1.0 .. 1908 K, nearly the full span, because a cluster's
// eight asteroid borders are enough 1 K neutronium to fill the bottom 2% on their own.
inline void PrintQuantiles(const Session& s, View view) {
  const GameDataUpdate* g = s.frame();
  if (!g) return;
  const int n = s.cells();
  std::vector<float> v;
  int empty = 0;
  const oni_sim::SaveExtProperty* ext_p = IsExtView(view) ? ExtViewProperty(s) : nullptr;
  const double ext_def = ext_p != nullptr ? extprops::DefaultValue(*ext_p) : 0.0;
  const bool attr = IsAttrView(view);
  for (int i = 0; i < n; ++i) {
    if (!IsTextureView(view) && !IsExtView(view) && !attr && g->mass[i] <= 0.0f) {
      ++empty;
      continue;
    }
    const float x = ViewScalar(s, view, i);
    if (IsTextureView(view) && !(x > 0.0f)) { ++empty; continue; }
    // "Empty" for the ext view means "at the registered default", which is the same
    // distinction the range scan makes and for the same reason.
    if (IsExtView(view) &&
        (ext_p == nullptr || static_cast<double>(x) == ext_def)) { ++empty; continue; }
    // And for the attr view it means "this cell's element carries no value". These
    // quantiles ARE weighted by area, deliberately and unlike the range above: "what is
    // the median cell's molecular mass" is a question about the world, where "what should
    // the ramp span" is a question about the values, and they have different answers.
    if (attr && !AttrSetAt(s, i)) { ++empty; continue; }
    v.push_back(x);
  }
  if (v.empty()) return;
  std::sort(v.begin(), v.end());
  static const double qs[] = {0, .005, .01, .02, .05, .10, .25, .50,
                              .75, .90, .95, .98, .99, .995, 1.0};
  out::Printf("\n%s over %zu non-empty cells (%d empty of %d):\n", ViewLabel(s, view).c_str(),
         v.size(), empty, n);
  for (double q : qs) {
    size_t i = static_cast<size_t>(q * (v.size() - 1));
    out::Printf("  p%-6.1f %14.4f\n", q * 100.0, v[i]);
  }
}

// The still-cell backdrop, shared by the flow view and the ext view. Both draw a reading
// that covers only part of the world, and both draw the rest as a dim phase map so the
// reading is seen against the world it happened in rather than against black.
inline Rgb DimPhase(unsigned phase) {
  switch (phase) {
    case 0: return Rgb{10, 10, 14};
    case 1: return Rgb{20, 24, 32};
    case 2: return Rgb{18, 26, 48};
    default: return Rgb{42, 42, 48};
  }
}

// Paints the world into `out`, one pixel per cell.
inline void RenderWorld(const Session& s, View view, const Range& range, Image* out,
                        bool mass_fade = true) {
  const GameDataUpdate* g = s.frame();
  const int w = s.width(), h = s.height();
  if (!g || w <= 0 || h <= 0) return;
  if (out->width() != w || out->height() != h) out->Reset(w, h);

  const simhost::Tables& tab = s.tables();
  const float lo = range.lo;
  const float span = (range.hi > range.lo) ? (range.hi - range.lo) : 1.0f;
  const float log_lo = std::log10(std::max(lo, 1e-6f));
  const float log_span =
      std::max(1e-6f, std::log10(std::max(range.hi, 1e-6f)) - log_lo);

  // The ext view is drawn in its own loop for the same reason the texture views are: it
  // does not share the grid views' "vacuum first" rule. What it draws is the property, on
  // the cells that hold something other than its default, over a dim phase map -- so a
  // bonus written into 1,600 cells reads as 1,600 lit cells in a recognisable world rather
  // than as a world-sized flat colour with a slightly different flat patch in it.
  // The attr view, drawn in its own loop for the same reason the ext view is and with the
  // same backdrop: what it has to say covers only the cells whose ELEMENT carries a value,
  // and those are read against a dim phase map rather than against black.
  //
  // Deliberately NOT faded by mass, unlike the temperature view. The value belongs to the
  // element, not to the cell's contents, so dimming a trace cell of hydrogen would draw it
  // as though its molecular mass were smaller than a full cell's -- a wrong number rather
  // than a quiet one. "Is there anything here" is what the element and mass views answer.
  if (IsAttrView(view)) {
    for (int y = 0; y < h; ++y) {
      const int row = h - 1 - y;
      for (int x = 0; x < w; ++x) {
        const int cell = y * w + x;
        // Through `AttrScalarAt`/`AttrSetAt` and not through the Session accessors direct,
        // even though `elementIdx` is already in hand for the backdrop. ONE function has to
        // decide what this view shows, or the picture and the range that bounds it can drift
        // apart -- and the drift would be invisible, because both would still look like
        // pictures of the same world. (The ext view is written the same way, and this is the
        // reason.) Measured while proving the check suite fails on purpose: with the render
        // reading the Session direct, a per-cell fault planted in `AttrScalarAt` moved the
        // range and left the pixels alone, so the arm that counts colours passed.
        const uint16_t idx = g->elementIdx[cell];
        // Same 0.2 ramp floor as the ext view, and here it matters more: an attribute with
        // one reachable value puts every cell at the bottom of a band `ScanRange` had to
        // widen, and ramping from 0 would paint a correct reading in the ramp's near-black.
        const Rgb c = AttrSetAt(s, cell)
                          ? Thermal(0.2f + 0.8f * ((AttrScalarAt(s, cell) - lo) / span))
                          : DimPhase(tab.Phase(idx));
        out->Set(x, row, c);
      }
    }
    return;
  }

  if (IsExtView(view)) {
    const oni_sim::SaveExtProperty* p = ExtViewProperty(s);
    const double def = p != nullptr ? extprops::DefaultValue(*p) : 0.0;
    for (int y = 0; y < h; ++y) {
      const int row = h - 1 - y;
      for (int x = 0; x < w; ++x) {
        const int cell = y * w + x;
        const float v = p != nullptr ? ExtScalarAt(s, cell) : 0.0f;
        // The ramp starts at 0.2, not 0, for the reason `FlowColour` gives for its own
        // brightness floor: a cell at the BOTTOM of the band is still a cell that holds
        // something, and the common case here -- a property written with one value into a
        // rectangle -- puts every written cell at the bottom. Ramping from 0 painted that
        // rectangle in the ramp's near-black and made a correct reading look like an empty
        // one, which is the whole failure this view exists to avoid.
        const Rgb c = (p == nullptr || static_cast<double>(v) == def)
                          ? DimPhase(tab.Phase(g->elementIdx[cell]))
                          : Thermal(0.2f + 0.8f * ((v - lo) / span));  // Thermal clamps
        out->Set(x, row, c);
      }
    }
    return;
  }

  // Klei's error map, in its own loop for the reason IsSimCheckView gives: three of its
  // eight classes are about a cell with nothing in it, so the grid views' "vacuum first"
  // short-circuit would erase exactly the cells this view exists to show.
  //
  // The colours are Klei's own constants, converted once here. They are deliberately the
  // saturated primaries the game uses rather than anything tuned for this palette: the point
  // of the view is that a screenshot of it and a screenshot of the in-game overlay are the
  // same picture, and a prettier red would break that.
  if (IsSimCheckView(view)) {
    static const Rgb kClassColour[simcheck::kClassCount] = {
        Rgb{18, 18, 22},     // black   -- ordinary (not pure black: a dead render must
                             //            still be distinguishable from a clean world)
        Rgb{230, 40, 40},    // red
        Rgb{235, 225, 60},   // yellow
        Rgb{60, 90, 235},    // blue
        Rgb{128, 128, 128},  // gray
        Rgb{60, 210, 90},    // green
        Rgb{225, 60, 220},   // magenta
        Rgb{70, 220, 230},   // cyan
    };
    for (int y = 0; y < h; ++y) {
      const int row = h - 1 - y;
      for (int x = 0; x < w; ++x) {
        const int cell = y * w + x;
        const simcheck::Class cl = simcheck::Classify(
            *tab.At(g->elementIdx[cell]), g->mass[cell], g->temperature[cell]);
        out->Set(x, row, kClassColour[cl]);
      }
    }
    return;
  }

  // The texture views are drawn in their own loop. They do not share the grid views'
  // "vacuum first" rule -- a massless cell can carry flow, and sunlight covers vacuum -- and
  // folding them into the switch below would mean testing mass twice with opposite meanings.
  if (IsTextureView(view)) {
    const void* liquid_tex = g->propertyTextureLiquid;
    const float flow_hi = (range.valid && range.hi > 0.0f) ? range.hi : 1.0f;
    for (int y = 0; y < h; ++y) {
      const int row = h - 1 - y;
      for (int x = 0; x < w; ++x) {
        const int cell = y * w + x;
        Rgb c = VacuumColour();
        switch (view) {
          case View::Flow: {
            float fx, fy;
            FlowAt(g, cell, &fx, &fy);
            if (fx == 0.0f && fy == 0.0f) {
              // Not blank: the still cells are drawn as a dim phase map, so a moving plume is
              // read against the world it is moving through rather than against nothing.
              c = DimPhase(tab.Phase(g->elementIdx[cell]));
            } else {
              c = FlowColour(fx, fy, std::sqrt(fx * fx + fy * fy) / flow_hi);
            }
            break;
          }
          case View::Liquid: {
            // The texture verbatim: r, g, b from the low three bytes and the cell's own alpha
            // from the high one, composited over the backdrop exactly as the game would.
            const uint32_t word = TextureWord(liquid_tex, cell);
            const float a = static_cast<float>((word >> 24) & 0xFF) / 255.0f;
            const Rgb rgb{static_cast<uint8_t>(word & 0xFF),
                          static_cast<uint8_t>((word >> 8) & 0xFF),
                          static_cast<uint8_t>((word >> 16) & 0xFF)};
            c = Blend(rgb, VacuumColour(), a);
            break;
          }
          default: {
            const uint8_t lit = SunlightAt(g, cell);
            c = Rgb{lit, lit, lit};
            break;
          }
        }
        out->Set(x, row, c);
      }
    }
    return;
  }

  for (int y = 0; y < h; ++y) {
    // Flip: sim row 0 is the bottom of the world, image row 0 is the top.
    const int row = h - 1 - y;
    for (int x = 0; x < w; ++x) {
      const int cell = y * w + x;
      const float mass = g->mass[cell];
      const uint16_t idx = g->elementIdx[cell];
      Rgb c;
      if (mass <= 0.0f) {
        c = VacuumColour();
      } else {
        switch (view) {
          case View::Element:
            c = ElementColour(tab.At(idx)->colour);
            break;
          case View::Temperature:
            c = Thermal((g->temperature[cell] - lo) / span);  // Thermal clamps
            if (mass_fade) c = Blend(c, VacuumColour(), MassOpacity(mass));
            break;
          case View::Mass:
            // Mass spans vacuum-adjacent grams to 20-tonne neutronium, so a linear
            // ramp shows two colours and nothing between them.
            c = Thermal((std::log10(std::max(mass, 1e-6f)) - log_lo) / log_span);
            break;
          case View::Flow:
          case View::Liquid:
          case View::Sunlight:
          case View::Ext:
          case View::Attr:
          case View::SimCheck:
            c = VacuumColour();  // handled above; here only to keep the switch total
            break;
          case View::Phase: {
            switch (tab.Phase(idx)) {
              case 0: c = Rgb{20, 20, 28}; break;             // vacuum
              case 1: c = Rgb{80, 170, 235}; break;           // gas
              case 2: c = Rgb{30, 80, 200}; break;            // liquid
              default:
                c = tab.IsUnstable(idx) ? Rgb{200, 160, 70}   // unstable solid (sand)
                                        : Rgb{110, 105, 100};  // solid
                break;
            }
            break;
          }
        }
      }
      out->Set(x, row, c);
    }
  }
}

}  // namespace simviz
