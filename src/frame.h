// What a displayed frame is made of, as data: the camera, the cell under the pointer, and the
// HUD's state. The drawing is qt/worldwidget.h's; this file is what it draws FROM, and is free
// of any window so the command line (--camera, --hover) and the window agree by construction.
//
// Coordinates. Three spaces are in play and confusing them renders something plausible
// and wrong, so they are named consistently here:
//
//   sim cell     `y * width + x`, y = 0 at the BOTTOM of the world
//   world image  the Image RenderWorld fills, y = 0 at the TOP (it does the flip)
//   screen       the composed frame, y = 0 at the top
//
// The camera maps screen <-> world image. Only SampleCell touches sim cells, and it
// does the flip in one place, matching --probe so a coordinate read off the HUD can be
// pasted straight into --probe.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "out.h"
#include "image.h"
#include "session.h"
#include "views.h"

namespace simviz {

// Outside the world entirely -- distinct from VacuumColour(), which means "a cell that
// exists and holds nothing". Keeping those two apart is the same reason vacuum is not
// pure black in views.h.
inline Rgb BackdropColour() { return Rgb{6, 6, 9}; }

struct Camera {
  float scale = 1.0f;  // screen pixels per world-image pixel
  float cx = 0.0f;     // world-image coordinate at screen x = 0
  float cy = 0.0f;     // world-image coordinate at screen y = 0

  // Largest scale that shows the whole world, centred. Not clamped to integers: a 636
  // px world in a 900 px window would otherwise jump straight from 1x to 2x and clip.
  void FitTo(int world_w, int world_h, int screen_w, int screen_h) {
    if (world_w <= 0 || world_h <= 0 || screen_w <= 0 || screen_h <= 0) return;
    const float sx = static_cast<float>(screen_w) / world_w;
    const float sy = static_cast<float>(screen_h) / world_h;
    scale = std::min(sx, sy);
    cx = (world_w - screen_w / scale) * 0.5f;
    cy = (world_h - screen_h / scale) * 0.5f;
  }

  void ScreenToWorld(int sx, int sy, float* wx, float* wy) const {
    *wx = cx + sx / scale;
    *wy = cy + sy / scale;
  }

  // Zooms about a screen point, so the world pixel under the cursor stays under it.
  void ZoomAt(int sx, int sy, float factor, float min_scale, float max_scale) {
    float wx, wy;
    ScreenToWorld(sx, sy, &wx, &wy);
    const float next = std::min(max_scale, std::max(min_scale, scale * factor));
    if (next == scale) return;
    scale = next;
    cx = wx - sx / scale;
    cy = wy - sy / scale;
  }

  void PanByScreen(int dx, int dy) {
    cx -= dx / scale;
    cy -= dy / scale;
  }

  // Keeps at least a little of the world on screen in every direction, so a stray drag
  // cannot lose it off the edge with no way back but 'f'.
  void Clamp(int world_w, int world_h, int screen_w, int screen_h) {
    const float view_w = screen_w / scale, view_h = screen_h / scale;
    const float margin_x = std::min(view_w, static_cast<float>(world_w)) * 0.5f;
    const float margin_y = std::min(view_h, static_cast<float>(world_h)) * 0.5f;
    cx = std::min(world_w - margin_x, std::max(margin_x - view_w, cx));
    cy = std::min(world_h - margin_y, std::max(margin_y - view_h, cy));
  }

  // Exact equality, and it is meant to be exact. The only caller asks "is this still the
  // camera `FitTo` produced" in order to decide whether the command line needs a `--camera`
  // at all, and `FitTo` is deterministic -- so after opening a world or pressing `f`, the
  // two agree to the bit. A tolerance here would mean a nudge too small to see silently
  // dropping out of the command, which is the one thing that line must not do.
  bool Equals(const Camera& other) const {
    return scale == other.scale && cx == other.cx && cy == other.cy;
  }
};

// What the flags asked the camera to be, before a world is in hand to seat it against.
// `--camera` is the exact form and wins outright; the rest are the human forms.
struct CameraSpec {
  bool explicit_camera = false;
  float cx = 0.0f, cy = 0.0f, scale = 0.0f;
  int zoom = 0;  // --zoom, 0 when it was not given
  bool hovered = false;
  int hover_x = 0, hover_y = 0;
  bool cropped = false;
  int crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0;
};

// The camera a composed snapshot or a freshly opened window starts at.
//
// ONE FUNCTION, called by both, and that is the whole point of it existing. `--hud` is meant
// to be the picture the window shows with no window -- so if the window seated its camera by
// one piece of arithmetic and the snapshot by another, `--camera` copied off the screen would
// render something subtly else, and BOTH HALVES WOULD STILL LOOK LIKE PICTURES OF THE SAME
// WORLD. That is the drift the attr view was once caught in, found only because a
// planted fault moved one half and not the other; this is the same shape, written once.
inline Camera SeatCamera(const CameraSpec& spec, int world_w, int world_h, int frame_w,
                         int frame_h) {
  Camera cam;
  if (spec.explicit_camera) {
    cam.cx = spec.cx;
    cam.cy = spec.cy;
    cam.scale = spec.scale;
    return cam;
  }
  cam.FitTo(world_w, world_h, frame_w, frame_h);
  if (spec.zoom > 1) {
    // Zooming has to be told WHERE. The first cut pinned the camera to --crop's origin,
    // which defaults to 0,0 -- so `--hud --zoom 8` with no --crop rendered the empty
    // top-left corner of the world and dropped the hover box off screen. Centre on the most
    // specific thing that was asked for.
    cam.scale = static_cast<float>(spec.zoom);
    float focus_x, focus_y;
    if (spec.hovered) {
      focus_x = spec.hover_x + 0.5f;
      focus_y = spec.hover_y + 0.5f;
    } else if (spec.cropped) {
      focus_x = spec.crop_x + (spec.crop_w > 0 ? spec.crop_w : 1) * 0.5f;
      focus_y = spec.crop_y + (spec.crop_h > 0 ? spec.crop_h : 1) * 0.5f;
    } else {
      focus_x = world_w * 0.5f;
      focus_y = world_h * 0.5f;
    }
    cam.cx = focus_x - frame_w / (2.0f * cam.scale);
    cam.cy = focus_y - frame_h / (2.0f * cam.scale);
  }
  return cam;
}

// A camera as the flag that reproduces it. `%.9g` and not `%g`: nine significant decimal
// digits is the round-trip width of an IEEE single, and six -- what `%g` gives -- is not.
// The difference is invisible in the text and is the difference between a pasted command
// drawing the same pixels and drawing almost the same pixels.
inline std::string CameraFlag(const Camera& cam) {
  char buf[96];
  snprintf(buf, sizeof(buf), " --camera %.9g,%.9g,%.9g", static_cast<double>(cam.cx),
           static_cast<double>(cam.cy), static_cast<double>(cam.scale));
  return buf;
}

// One cell's raw state, read at world-image coordinates.
struct HoverInfo {
  bool valid = false;
  int x = 0, y_img = 0, cell = 0;
  uint16_t idx = 0;
  int32_t elem_id = 0;
  unsigned phase = 0;
  bool unstable = false;
  float mass = 0.0f, temp = 0.0f;
  // What the property textures hold for this cell. The hover readout is the one place a
  // texture can be checked against the cell it was derived from without leaving the window:
  // a liquid word that is zero over a cell full of water, or a flow vector pointing the
  // wrong way out of a plume, is a texture bug and not a sim one, and the two look identical
  // in any view drawn from the grid.
  bool has_flow = false, has_liquid = false, has_sun = false;
  float flow_x = 0.0f, flow_y = 0.0f;
  uint32_t liquid = 0, liquid_data = 0, material = 0;
  uint8_t sun = 0;
  // One line per per-cell EXTENSION PROPERTY the sim carries, already formatted. Held as
  // text rather than as values because nothing here knows what any of them are: the names,
  // types and arities come out of the blob, so the readout is generic in exactly the way
  // `GameDataUpdate`'s hand-typed members are not. Empty when the DLL exports none, when
  // none is set anywhere, and when attached -- three different things, distinguished by
  // `Session::ExtProps()` for a caller that needs to tell them apart.
  std::vector<std::string> ext;
};

inline HoverInfo SampleCell(const Session& s, int wx, int wy_img) {
  HoverInfo h;
  const GameDataUpdate* g = s.frame();
  const int w = s.width(), ht = s.height();
  if (!g || wx < 0 || wy_img < 0 || wx >= w || wy_img >= ht) return h;
  h.valid = true;
  h.x = wx;
  h.y_img = wy_img;
  h.cell = (ht - 1 - wy_img) * w + wx;  // the one place the y flip happens
  h.idx = g->elementIdx[h.cell];
  h.elem_id = s.tables().At(h.idx)->id;
  h.phase = static_cast<unsigned>(s.tables().Phase(h.idx));
  h.unstable = s.tables().IsUnstable(h.idx);
  h.mass = g->mass[h.cell];
  h.temp = g->temperature[h.cell];
  if (g->propertyTextureFlow) {
    h.has_flow = true;
    FlowAt(g, h.cell, &h.flow_x, &h.flow_y);
  }
  if (g->propertyTextureLiquid) {
    h.has_liquid = true;
    h.liquid = TextureWord(g->propertyTextureLiquid, h.cell);
    h.liquid_data = TextureWord(g->propertyTextureLiquidData, h.cell);
    h.material = TextureWord(g->propertyTextureMaterialData, h.cell);
  }
  if (g->propertyTextureExposedToSunlight) {
    h.has_sun = true;
    h.sun = SunlightAt(g, h.cell);
  }
  // The extension registry is indexed on the PADDED grid -- the sim keeps a one-cell border
  // ring that no `GameDataUpdate` array has -- so this is the second place in this function
  // where a coordinate is converted, and the only one that does not use `h.cell`.
  const extprops::Table& ex = s.ExtProps();
  if (ex.decoded && !ex.empty()) {
    const size_t padded = extprops::PaddedIndex(wx, ht - 1 - wy_img, ex.padded_w);
    for (const oni_sim::SaveExtProperty& p : ex.props) {
      h.ext.push_back(extprops::FormatCell(p, padded));
    }
  } else if (s.attached() && !s.ext_live()) {
    // Said rather than left blank. Attached, the registry is a route the Session chose not
    // to poll, so an empty card here means "not asked for" and not "this world has none" --
    // and someone who saw these lines offline would otherwise read it as the second.
    h.ext.push_back("ext: not fetched (--ext)");
  }
  return h;
}

// The cell under a screen point -- invalid when the pointer is off the window or off the
// world. ONE function, called by the hover readout below and by the window loop that puts
// the same cell into the command line as `--hover x,y`. Two copies of a floor and a bounds
// test would agree almost always, and the day they did not the readout on screen and the
// flag offered for reproducing it would name different cells.
inline HoverInfo HoverAt(const Session& s, const Camera& cam, int screen_x, int screen_y) {
  if (screen_x < 0 || screen_y < 0) return HoverInfo();
  float wx, wy;
  cam.ScreenToWorld(screen_x, screen_y, &wx, &wy);
  return SampleCell(s, static_cast<int>(std::floor(wx)), static_cast<int>(std::floor(wy)));
}

inline const char* PhaseName(unsigned phase) {
  switch (phase) {
    case 0: return "vacuum";
    case 1: return "gas";
    case 2: return "liquid";
    default: return "solid";
  }
}

// Everything the HUD needs that is not already in the Session. Kept as plain data so
// the window loop and the headless snapshot path fill it the same way.
struct HudState {
  bool show = true;
  bool playing = false;
  float dt = 0.2f;
  float fps = 0.0f;
  float last_tick_ms = 0.0f;
  bool seed_pinned = false;
  uint32_t seed = 0;  // what --seed asked for; rng below is the LIVE state, which moves
  uint32_t rng = 0;
  bool wall_clock_seeded = false;
  bool mass_fade = true;

  // Width of a control column the window draws OVER the left edge of the frame, or 0 when
  // there is none (every snapshot). The HUD is composed first and the panels painted last, so
  // anything anchored at x = 0 -- the status lines, the key help, the scrub bar's left end --
  // would sit under the column and be cut off; they start here instead.
  int left_inset = 0;

  // Live attach. The rng/seed pair above is meaningless when the world arrives over a
  // socket -- there is no local stream and the game exposes none -- so the second status
  // line says what IS true instead: where the frames come from, which Unity frame the
  // last one was taken on, and how big it was. Labelled `unity` rather than `frame`
  // because it is Unity's counter, not a published sim-frame count; the sim does not
  // expose one to managed code at all.
  bool attached = false;
  std::string endpoint;
  uint32_t unity_frame = 0;
  size_t poll_bytes = 0;
  int mouse_x = -1, mouse_y = -1;  // screen; negative means no pointer
  std::string message;             // transient line, e.g. "wrote frame.png"

  // Scrub timeline. The bar spans `base`..`head`: the tick the timeline was anchored on
  // (0 unless --ticks ran frames before it) to the furthest tick ever simulated. The
  // playhead is s.ticks(). cp_ticks are drawn as marks so the resolution of the history
  // is visible rather than implied -- a seek between two marks costs a replay, and the
  // picture says how far apart they are.
  bool timeline = false;
  bool branched = false;  // the displayed state is a replay that left the original run
  int base = 0;
  int head = 0;
  int cp_interval = 0;
  size_t cp_bytes = 0;
  std::vector<int> cp_ticks;  // the exact stops, one frame past each checkpoint

  // The event log pane. `watching` is whether the Session asked for any stream at all, and
  // it is separate from `show` because an empty log is a RESULT: a pane that said
  // "sim.message_refused: nothing" for an hour is the strongest statement this program can
  // make about a mod's messages, and hiding it when it has no rows would throw that away.
  bool events = true;
  bool watching_events = false;
};

inline Rgb TimelineTrack() { return Rgb{44, 48, 58}; }
inline Rgb TimelineFill() { return Rgb{70, 110, 160}; }
inline Rgb TimelineMark() { return Rgb{120, 200, 140}; }
inline Rgb TimelineHead() { return Rgb{255, 220, 120}; }

inline Rgb HudInk() { return Rgb{225, 232, 240}; }
inline Rgb HudDim() { return Rgb{150, 160, 175}; }
inline Rgb HudPanel() { return Rgb{16, 18, 24}; }
inline Rgb HudWarn() { return Rgb{240, 190, 90}; }

}  // namespace simviz
