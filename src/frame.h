// Composing a displayed frame: camera, world blit, hover readout, HUD.
//
// This sits between `views.h` (state -> a world-sized Image, one pixel per cell) and
// whatever presents the result. It is deliberately independent of the window: the same
// ComposeFrame call produces the window's back buffer and the `--snapshot --hud` PNG,
// so the overlay can be inspected offline exactly as the world already could.
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
#include "font.h"
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

// Nearest-neighbour blit of the world image into `out` through the camera.
//
// Nearest, not filtered, on purpose: this is a debugger. A smoothed image invents cell
// values that the sim never held, and the whole point of zooming in is to read the
// cells that are actually there.
inline void BlitWorld(const Image& world, const Camera& cam, Image* out) {
  const int ow = out->width(), oh = out->height();
  const int ww = world.width(), wh = world.height();
  const Rgb backdrop = BackdropColour();
  const float inv = 1.0f / cam.scale;
  for (int sy = 0; sy < oh; ++sy) {
    const int wy = static_cast<int>(std::floor(cam.cy + sy * inv));
    for (int sx = 0; sx < ow; ++sx) {
      const int wx = static_cast<int>(std::floor(cam.cx + sx * inv));
      out->Set(sx, sy, (wx < 0 || wy < 0 || wx >= ww || wy >= wh) ? backdrop
                                                                  : world.Get(wx, wy));
    }
  }
}

// A one-pixel rectangle outline. Used to mark the hovered cell.
inline void DrawRectOutline(Image* img, int x, int y, int w, int h, Rgb c) {
  for (int i = 0; i < w; ++i) {
    img->Set(x + i, y, c);
    img->Set(x + i, y + h - 1, c);
  }
  for (int j = 0; j < h; ++j) {
    img->Set(x, y + j, c);
    img->Set(x + w - 1, y + j, c);
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

// The scrub bar's rectangle, above the key-help panel. Shared by the drawing code and
// by the window's hit test so a click can never land somewhere the bar is not drawn.
struct TimelineBar {
  int x = 0, y = 0, w = 0, h = 0;
  int track_x = 0, track_w = 0;  // the seekable span inside the panel
};

// The readout on the right is given a FIXED width rather than its own measured one.
// Measuring it would make the track -- and therefore where a click lands -- move every
// time the tick number gained a digit, which is a control that shifts under the pointer
// while it is being used. A fixed reserve costs a little space and nothing else.
inline int TimelineLabelWidth() { return 64 * font::Advance() + 8; }

inline TimelineBar TimelineBarRect(int ow, int oh, int inset = 0) {
  const int pad = 4;
  const int lh = font::LineHeight();
  const int row = lh + 2 * pad;
  TimelineBar b;
  b.x = inset;
  b.y = oh - row - row - 2;  // one row up from the help panel
  b.w = ow - inset;
  b.h = row;
  b.track_x = b.x + pad;
  b.track_w = b.w - TimelineLabelWidth() - 2 * pad;
  if (b.track_w < 2) b.track_w = 2;
  return b;
}

// The track maps `base`..`head`, not 0..head. When a run ticked before the timeline was
// started there is no history below `base`, and mapping from 0 would spend the left of the
// bar on ticks no click can reach.
inline int TimelineTickToX(const TimelineBar& b, int tick, int base, int head) {
  const int span = head - base;
  if (span <= 0) return b.track_x;
  double f = static_cast<double>(tick - base) / span;
  if (f < 0.0) f = 0.0;
  if (f > 1.0) f = 1.0;
  return b.track_x + static_cast<int>(f * (b.track_w - 1) + 0.5);
}

inline int TimelineXToTick(const TimelineBar& b, int x, int base, int head) {
  const int span = head - base;
  if (span <= 0 || b.track_w < 2) return base;
  double f = static_cast<double>(x - b.track_x) / (b.track_w - 1);
  if (f < 0.0) f = 0.0;
  if (f > 1.0) f = 1.0;
  return base + static_cast<int>(f * span + 0.5);
}

inline bool TimelineHit(const TimelineBar& b, int x, int y) {
  return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

inline Rgb TimelineTrack() { return Rgb{44, 48, 58}; }
inline Rgb TimelineFill() { return Rgb{70, 110, 160}; }
inline Rgb TimelineMark() { return Rgb{120, 200, 140}; }
inline Rgb TimelineHead() { return Rgb{255, 220, 120}; }

inline Rgb HudInk() { return Rgb{225, 232, 240}; }
inline Rgb HudDim() { return Rgb{150, 160, 175}; }
inline Rgb HudPanel() { return Rgb{16, 18, 24}; }
inline Rgb HudWarn() { return Rgb{240, 190, 90}; }

// Status lines across the top, hover readout beside the cursor, key help at the bottom.
inline void DrawHud(const Session& s, View view, const Range& range, const Camera& cam,
                    const HudState& hud, Image* out) {
  if (!hud.show) return;
  const int ow = out->width(), oh = out->height();
  const int pad = 4;
  const int lh = font::LineHeight();

  // The rng figure is the LIVE stream state, which advances every tick -- it is not the
  // seed, and showing it labelled "pinned" invited exactly that misreading on the first
  // composed frame. Say which seed produced it instead.
  char seeded[48];
  if (hud.seed_pinned) {
    snprintf(seeded, sizeof(seeded), "from seed 0x%08X", hud.seed);
  } else {
    snprintf(seeded, sizeof(seeded), "%s", hud.wall_clock_seeded ? "wall-clock" : "worldgen");
  }

  char l1[256], l2[256];
  if (hud.attached) {
    snprintf(l1, sizeof(l1), "%-11s  poll %-6d  %s  %.2fx  LIVE",
             ViewLabel(s, view).c_str(),
             s.ticks(), hud.playing ? "FOLLOWING" : "HELD     ",
             static_cast<double>(cam.scale));
  } else {
    snprintf(l1, sizeof(l1), "%-11s  tick %-6d  dt %.3f  %s  %.2fx",
             ViewLabel(s, view).c_str(),
             s.ticks(), static_cast<double>(hud.dt), hud.playing ? "PLAYING" : "PAUSED ",
             static_cast<double>(cam.scale));
  }
  if (hud.attached) {
    // The range still belongs on this line when there is one, but the seed half of it
    // does not exist here -- see HudState. What replaces it is the provenance: which
    // game, which of its frames, and how many bytes crossed to draw this one.
    char span[96] = "";
    if (view == View::Temperature || view == View::Mass) {
      snprintf(span, sizeof(span), "range %.1f..%.1f %s  ",
               static_cast<double>(range.lo), static_cast<double>(range.hi),
               view == View::Temperature ? "K" : "kg");
    } else {
      snprintf(span, sizeof(span), "world %dx%d  ", s.width(), s.height());
    }
    snprintf(l2, sizeof(l2), "%s%s  unity %u  %.1f MB  %.0f fps  poll %.1f ms", span,
             hud.endpoint.c_str(), hud.unity_frame,
             hud.poll_bytes / (1024.0 * 1024.0), static_cast<double>(hud.fps),
             static_cast<double>(hud.last_tick_ms));
  } else if (view == View::Temperature || view == View::Mass) {
    snprintf(l2, sizeof(l2), "range %.1f..%.1f %s  rng 0x%08X %s  %.0f fps  tick %.1f ms",
             static_cast<double>(range.lo), static_cast<double>(range.hi),
             view == View::Temperature ? "K" : "kg", hud.rng, seeded, 
             static_cast<double>(hud.fps), static_cast<double>(hud.last_tick_ms));
  } else {
    snprintf(l2, sizeof(l2), "world %dx%d  rng 0x%08X %s  %.0f fps  tick %.1f ms",
             s.width(), s.height(), hud.rng, seeded, static_cast<double>(hud.fps),
             static_cast<double>(hud.last_tick_ms));
  }

  // Panel behind both lines, so text stays readable over white-hot magma.
  const int x0 = hud.left_inset + pad;
  int panel_w = std::max(font::MeasureWidth(l1), font::MeasureWidth(l2)) + 2 * pad;
  out->FillRect(hud.left_inset, 0, std::min(panel_w, ow - hud.left_inset), 2 * lh + 2 * pad,
                HudPanel());
  font::DrawString(out, x0, pad, l1, HudInk());
  font::DrawString(out, x0, pad + lh, l2, HudDim());

  int next_y = 2 * lh + 2 * pad;

  // The determinism warning from FINDINGS.md 5, on screen rather than only in the log:
  // a run whose stream came from the wall clock will not reproduce, and that is worth
  // knowing while looking at it, not afterwards.
  if (hud.wall_clock_seeded && !hud.seed_pinned && !hud.attached) {
    const char* warn = "wall-clock seeded: this run will NOT reproduce (--seed N)";
    font::DrawStringBoxed(out, x0, next_y + pad, warn, HudWarn(), HudPanel(), pad);
    next_y += lh + 2 * pad;
  }
  if (!hud.message.empty()) {
    font::DrawStringBoxed(out, x0, next_y + pad, hud.message.c_str(), HudInk(),
                          HudPanel(), pad);
  }

  // Hover readout.
  {
    const HoverInfo h = HoverAt(s, cam, hud.mouse_x, hud.mouse_y);
    const int cell_x = h.x, cell_y = h.y_img;
    if (h.valid) {
      // Outline the hovered cell when a cell is big enough on screen to see one.
      if (cam.scale >= 3.0f) {
        const int px = static_cast<int>((cell_x - cam.cx) * cam.scale);
        const int py = static_cast<int>((cell_y - cam.cy) * cam.scale);
        const int side = std::max(2, static_cast<int>(cam.scale));
        DrawRectOutline(out, px, py, side, side, Rgb{255, 255, 255});
      }

      // A vector rather than the fixed `char[5][128]` this used to be: the extension
      // properties are whatever the sim carries, so the line count is not knowable here.
      std::vector<std::string> lines;
      char buf[160];
      auto add = [&](const char* text) { lines.push_back(text); };
      snprintf(buf, sizeof(buf), "cell %d,%d  index %d", h.x, h.y_img, h.cell);
      add(buf);
      snprintf(buf, sizeof(buf), "element %u  id %d  %s%s", static_cast<unsigned>(h.idx),
               h.elem_id, PhaseName(h.phase), h.unstable ? " unstable" : "");
      add(buf);
      snprintf(buf, sizeof(buf), "%.4f kg  %.3f K", static_cast<double>(h.mass),
               static_cast<double>(h.temp));
      add(buf);
      // The texture lines are added only when the DLL published the buffers, so an older
      // one reads exactly as it did before rather than showing three zeros that mean
      // "not published" and look like "nothing there".
      if (h.has_flow || h.has_sun) {
        snprintf(buf, sizeof(buf), "flow %+.5f,%+.5f  sun %u",
                 static_cast<double>(h.flow_x), static_cast<double>(h.flow_y),
                 static_cast<unsigned>(h.sun));
        add(buf);
      }
      if (h.has_liquid && (h.liquid || h.liquid_data || h.material)) {
        snprintf(buf, sizeof(buf), "liq %08X  data %08X  mat %08X",
                 (unsigned)h.liquid, (unsigned)h.liquid_data, (unsigned)h.material);
        add(buf);
      }
      // Extension properties last, and capped: the card is meant to be read at a glance
      // while the mouse is moving, and a registry with a dozen properties in it would
      // otherwise cover the world it is describing. The count is stated when it clips,
      // because a silently truncated readout is a readout that lies.
      constexpr size_t kMaxExtLines = 4;
      for (size_t i = 0; i < h.ext.size() && i < kMaxExtLines; ++i) {
        std::string line = h.ext[i];
        if (line.size() > 58) line = line.substr(0, 55) + "...";
        lines.push_back(line);
      }
      if (h.ext.size() > kMaxExtLines) {
        snprintf(buf, sizeof(buf), "+%zu more (--ext-list)", h.ext.size() - kMaxExtLines);
        add(buf);
      }
      const int nlines = static_cast<int>(lines.size());
      int bw = 0;
      for (int i = 0; i < nlines; ++i) {
        bw = std::max(bw, font::MeasureWidth(lines[i].c_str()));
      }
      bw += 2 * pad;
      const int bh = nlines * lh + 2 * pad;
      // Flip the box to the other side of the cursor near an edge rather than let it
      // run off: a readout you have to move the mouse to read is not a readout.
      int bx = hud.mouse_x + 14;
      int by = hud.mouse_y + 14;
      if (bx + bw > ow) bx = hud.mouse_x - 14 - bw;
      if (by + bh > oh) by = hud.mouse_y - 14 - bh;
      bx = std::max(hud.left_inset, bx);
      by = std::max(0, by);
      out->FillRect(bx, by, bw, bh, HudPanel());
      DrawRectOutline(out, bx, by, bw, bh, Rgb{70, 78, 92});
      for (int i = 0; i < nlines; ++i) {
        font::DrawString(out, bx + pad, by + pad + i * lh, lines[i].c_str(),
                         (i % 2) ? HudDim() : HudInk());
      }
    }
  }

  // Key help, bottom left. Attached, the keys that move through history are gone --
  // there is no history to move through, only whatever the game has right now -- so the
  // line does not offer them. A help line that names a key that does nothing is worse
  // than a shorter one.
  // The eighth and ninth keys are named only when a name was given for them: the ext and
  // attr views exist for this run only if a property or an attribute was named, and offering
  // a key that does nothing is the thing this comment's older half already argues against.
  // Built up rather than picked from a table because the two are independent -- an attr view
  // with no ext view offers 9 and not 8, and "1-9" there would name a key that is inert.
  std::string views = "1-7";
  if (!s.ext_view_name().empty()) views += "/8";
  if (!s.attr_view_name().empty()) views += "/9";
  views += " view";
  // Same rule as the view keys above: `e` is offered only when there is a log for it to
  // toggle, because a key that does nothing is worse than an absent one.
  const std::string log_key = hud.watching_events ? "  e log" : "";
  const std::string help =
      hud.attached
          ? "space follow  . poll once  " + views +
                "  r rescan  f fit  +/- zoom  drag/arrows pan  m fade  h hud  s png" +
                log_key + "  esc quit"
          : "space play  ,/. step  [ ] 10  home/end  t bar  " + views +
                "  r rescan  f fit  +/- zoom  drag/arrows pan  m fade  h hud  s png" +
                log_key + "  esc quit";
  const int help_w = std::min(ow - hud.left_inset, font::MeasureWidth(help.c_str()) + 2 * pad);
  out->FillRect(hud.left_inset, oh - lh - 2 * pad, help_w, lh + 2 * pad, HudPanel());
  font::DrawString(out, x0, oh - lh - pad, help.c_str(), HudDim());
}

// The bar: elapsed span, checkpoint marks, playhead, and the cost of the history in
// one line. The memory figure is shown because it is the whole reason the timeline is
// sparse -- a checkpoint per tick would be 889 MB per hundred ticks.
inline void DrawTimeline(const Session& s, const HudState& hud, Image* out) {
  if (!hud.show || !hud.timeline) return;
  const int ow = out->width(), oh = out->height();
  const int pad = 4;
  const int lh = font::LineHeight();
  const TimelineBar b = TimelineBarRect(ow, oh, hud.left_inset);
  if (b.y < 0) return;

  out->FillRect(b.x, b.y, b.w, b.h, HudPanel());

  const int groove_y = b.y + pad + lh / 2 - 1;
  out->FillRect(b.track_x, groove_y, b.track_w, 3, TimelineTrack());

  const int play_x = TimelineTickToX(b, s.ticks(), hud.base, hud.head);
  if (play_x > b.track_x) {
    out->FillRect(b.track_x, groove_y, play_x - b.track_x, 3, TimelineFill());
  }
  for (int t : hud.cp_ticks) {
    const int mx = TimelineTickToX(b, t, hud.base, hud.head);
    out->FillRect(mx, groove_y - 3, 1, 9, TimelineMark());
  }
  out->FillRect(play_x - 1, b.y + 2, 3, b.h - 4, TimelineHead());

  char line[192];
  snprintf(line, sizeof(line), "tick %d / %d   %zu cp every %d   %.1f MB   %s",
           s.ticks(), hud.head, hud.cp_ticks.size(), hud.cp_interval,
           hud.cp_bytes / (1024.0 * 1024.0),
           hud.branched ? "REPLAYED -- diverges from the run" : "exact");
  // Right-aligned inside the reserved strip, which the track already stops short of, so
  // no checkpoint mark can end up underneath it.
  int lx = ow - font::MeasureWidth(line) - pad;
  const int reserved_x = b.track_x + b.track_w + pad;
  if (lx < reserved_x) lx = reserved_x;
  font::DrawString(out, lx, b.y + pad, line, hud.branched ? HudWarn() : HudDim());
}

// The event log: the newest refusals, oldest at the top of the pane, with the three ways a
// gap can happen kept apart on their own line.
//
// WHY A LOG AND NOT A VIEW. Every other readout in this program is a function of the current
// frame -- pick a cell, paint what it holds. An event has no cell and no persistence: it
// describes a tick that has already gone. So it gets a pane that accumulates, and the
// accumulation lives in `Session` rather than here, because this function runs on every repaint
// and the log must not.
//
// AN EMPTY PANE IS THE INTERESTING CASE MOST OF THE TIME. `sim.message_refused` exists because
// the DLL drops malformed messages in silence; a run that produces no rows is a run in which
// nothing was dropped, which is what a mod author wants to see and is a different fact from
// "not watching". The header says which of the two applies.
inline void DrawEventLog(const Session& s, const HudState& hud, Image* out) {
  if (!hud.show || !hud.events || !hud.watching_events) return;
  const int pad = 4;
  const int lh = font::LineHeight();

  // Sized to the longest line the formatter can produce, so the pane does not resize itself as
  // rows arrive -- the same reason the timeline reserves a fixed label strip.
  const char* kWidest = "t000000  msg 0xFFFFFFFF  unknown message  0000/0000 bytes";
  const int w = font::MeasureWidth(kWidest) + 2 * pad;
  const int x = out->width() - w;
  if (x < 0) return;

  // Stops above whatever already owns the bottom of the window: the scrub bar when there is
  // one, and the key-help panel either way. Overlapping either would put the log on top of a
  // control, and the help row is the thing a new user is reading.
  const TimelineBar bar = TimelineBarRect(out->width(), out->height(), hud.left_inset);
  const int help_row = lh + 2 * pad;
  const int bottom =
      (hud.timeline && bar.y > 0) ? bar.y - 2 : out->height() - help_row - 2;

  const std::vector<extevents::Refusal>& log = s.refusals();
  // Header, then a gap line only when there is a gap to report -- a row that always said
  // "0 lost, 0 dropped" would be a row nobody reads, and this one has to be noticed.
  const bool any_gap =
      s.events_lost() > 0 || s.events_dropped() > 0 || s.events_forgotten() > 0;
  const bool any_error = !s.events_error().empty();
  const int rows_available = (bottom - pad - lh * (1 + (any_gap ? 1 : 0) + (any_error ? 1 : 0)) -
                              pad) / lh;
  int rows = static_cast<int>(log.size());
  if (rows > rows_available) rows = rows_available;
  if (rows < 0) rows = 0;

  const int h = pad + lh * (1 + (any_gap ? 1 : 0) + (any_error ? 1 : 0) + rows) + pad;
  const int y = bottom - h;
  if (y < 0) return;
  out->FillRect(x, y, w, h, HudPanel());

  int ty = y + pad;
  char line[192];
  snprintf(line, sizeof(line), "%s  %zu", s.event_streams().c_str(), log.size());
  font::DrawString(out, x + pad, ty, line, HudInk());
  ty += lh;

  if (any_error) {
    font::DrawString(out, x + pad, ty, s.events_error().c_str(), HudWarn());
    ty += lh;
  }
  if (any_gap) {
    // Three counters, never summed. "the route's buffer overran", "the producer overran its
    // own per-frame cap" and "this log forgot its oldest rows" are three different failures
    // with three different fixes, and a single "N missing" would send a reader after the
    // wrong one.
    snprintf(line, sizeof(line), "gaps: route %d  sim %d  log %d", s.events_lost(),
             s.events_dropped(), s.events_forgotten());
    font::DrawString(out, x + pad, ty, line, HudWarn());
    ty += lh;
  }

  // The NEWEST rows, which is why the slice is taken off the end. A pane that filled from the
  // front would freeze on the first screenful and never show what is happening now.
  for (size_t i = log.size() - static_cast<size_t>(rows); i < log.size(); ++i) {
    const std::string text = extevents::Describe(log[i]);
    font::DrawString(out, x + pad, ty, text.c_str(), HudDim());
    ty += lh;
  }
}

// The whole displayed frame: world through the camera, then the overlay.
inline void ComposeFrame(const Session& s, View view, const Range& range,
                         const Camera& cam, const HudState& hud, const Image& world,
                         Image* out) {
  BlitWorld(world, cam, out);
  DrawHud(s, view, range, cam, hud, out);
  DrawTimeline(s, hud, out);
  DrawEventLog(s, hud, out);
}

}  // namespace simviz
