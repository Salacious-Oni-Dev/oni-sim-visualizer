// The world on screen: the cell image through the camera, and everything drawn over it.
//
// The cells are still RenderWorld's arithmetic -- one pixel per cell, the same colours the PNG
// snapshot writes -- drawn scaled by QPainter with no smoothing (a smoothed image invents cell
// values the sim never held, and the point of zooming in is to read the cells that are there).
// Everything ON TOP is drawn at screen resolution instead: the cell grid, the flow arrows, the
// hover outline and card, the HUD, the event log. Those were bitmap-font pixels in the old
// framebuffer and blurred into the cells when magnified; here they stay sharp at any zoom.
//
// Input arrives here only for the things that are about the world itself -- drag to pan, the
// wheel to zoom about the cursor, the arrow keys while the world has focus, and the pointer
// position for the hover readout. Everything else is an action on the main window.

#pragma once

#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "../frame.h"
#include "../qtapp.h"
#include "viewer.h"

namespace simviz {
namespace qtui {

inline QColor ToQ(Rgb c, int alpha = 255) { return QColor(c.r, c.g, c.b, alpha); }

// A QImage as the program's own Image, so every PNG this program writes goes through one
// encoder (image.h's WritePng) whatever drew it.
inline Image FromQImage(const QImage& src) {
  const QImage img = src.convertToFormat(QImage::Format_RGB32);
  Image out(img.width(), img.height());
  for (int y = 0; y < img.height(); ++y) {
    const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
    for (int x = 0; x < img.width(); ++x)
      out.Set(x, y, Rgb{static_cast<uint8_t>(qRed(row[x])), static_cast<uint8_t>(qGreen(row[x])),
                        static_cast<uint8_t>(qBlue(row[x]))});
  }
  return out;
}


// The two status lines, as text. The same content DrawHud printed, so a person who knew the
// old overlay reads the new one without relearning it.
inline std::vector<std::string> HudLines(const Session& s, View view, const Range& range,
                                         const Camera& cam, const HudState& hud) {
  char seeded[48];
  if (hud.seed_pinned) snprintf(seeded, sizeof(seeded), "from seed 0x%08X", hud.seed);
  else snprintf(seeded, sizeof(seeded), "%s", hud.wall_clock_seeded ? "wall-clock" : "worldgen");

  char l1[256], l2[256];
  if (hud.attached) {
    snprintf(l1, sizeof(l1), "%-11s  poll %-6d  %s  %.2fx  LIVE", ViewLabel(s, view).c_str(),
             s.ticks(), hud.playing ? "FOLLOWING" : "HELD     ",
             static_cast<double>(cam.scale));
    char span[96] = "";
    if (view == View::Temperature || view == View::Mass) {
      snprintf(span, sizeof(span), "range %.1f..%.1f %s  ", static_cast<double>(range.lo),
               static_cast<double>(range.hi), view == View::Temperature ? "K" : "kg");
    } else {
      snprintf(span, sizeof(span), "world %dx%d  ", s.width(), s.height());
    }
    snprintf(l2, sizeof(l2), "%s%s  unity %u  %.1f MB  %.0f fps  poll %.1f ms", span,
             hud.endpoint.c_str(), hud.unity_frame, hud.poll_bytes / (1024.0 * 1024.0),
             static_cast<double>(hud.fps), static_cast<double>(hud.last_tick_ms));
  } else {
    snprintf(l1, sizeof(l1), "%-11s  tick %-6d  dt %.3f  %s  %.2fx", ViewLabel(s, view).c_str(),
             s.ticks(), static_cast<double>(hud.dt), hud.playing ? "PLAYING" : "PAUSED ",
             static_cast<double>(cam.scale));
    if (view == View::Temperature || view == View::Mass) {
      snprintf(l2, sizeof(l2), "range %.1f..%.1f %s  rng 0x%08X %s  %.0f fps  frame %.1f ms",
               static_cast<double>(range.lo), static_cast<double>(range.hi),
               view == View::Temperature ? "K" : "kg", hud.rng, seeded,
               static_cast<double>(hud.fps), static_cast<double>(hud.last_tick_ms));
    } else {
      snprintf(l2, sizeof(l2), "world %dx%d  rng 0x%08X %s  %.0f fps  frame %.1f ms", s.width(),
               s.height(), hud.rng, seeded, static_cast<double>(hud.fps),
               static_cast<double>(hud.last_tick_ms));
    }
  }
  return {l1, l2};
}

// The hover card's lines for one cell. Same rows, same caps, same order as the old card.
inline std::vector<std::string> HoverLines(const HoverInfo& h) {
  std::vector<std::string> lines;
  char buf[160];
  snprintf(buf, sizeof(buf), "cell %d,%d  index %d", h.x, h.y_img, h.cell);
  lines.push_back(buf);
  snprintf(buf, sizeof(buf), "element %u  id %d  %s%s", static_cast<unsigned>(h.idx), h.elem_id,
           PhaseName(h.phase), h.unstable ? " unstable" : "");
  lines.push_back(buf);
  snprintf(buf, sizeof(buf), "%.4f kg  %.3f K", static_cast<double>(h.mass),
           static_cast<double>(h.temp));
  lines.push_back(buf);
  // Texture lines only when the DLL published the buffers: three zeros that mean "not
  // published" look exactly like "nothing there".
  if (h.has_flow || h.has_sun) {
    snprintf(buf, sizeof(buf), "flow %+.5f,%+.5f  sun %u", static_cast<double>(h.flow_x),
             static_cast<double>(h.flow_y), static_cast<unsigned>(h.sun));
    lines.push_back(buf);
  }
  if (h.has_liquid && (h.liquid || h.liquid_data || h.material)) {
    snprintf(buf, sizeof(buf), "liq %08X  data %08X  mat %08X", (unsigned)h.liquid,
             (unsigned)h.liquid_data, (unsigned)h.material);
    lines.push_back(buf);
  }
  // Extension properties last, and capped, with the count stated when it clips: a silently
  // truncated readout is a readout that lies.
  constexpr size_t kMaxExtLines = 4;
  for (size_t i = 0; i < h.ext.size() && i < kMaxExtLines; ++i) {
    std::string line = h.ext[i];
    if (line.size() > 58) line = line.substr(0, 55) + "...";
    lines.push_back(line);
  }
  if (h.ext.size() > kMaxExtLines) {
    snprintf(buf, sizeof(buf), "+%zu more (--ext-list)", h.ext.size() - kMaxExtLines);
    lines.push_back(buf);
  }
  return lines;
}

class WorldWidget : public QWidget {
 public:
  WorldWidget(Viewer* v, float ui_scale, bool large, QWidget* parent = nullptr)
      : QWidget(parent), v_(v), mono_(qtapp::MonoFont(ui_scale, large)) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(200, 150);
  }

  // Set by the window: called after every paint, so the clock can hold the next tick until
  // the last one has been seen.
  std::function<void()> on_painted;
  // Set by the window: the hovered cell changed (the panels show it, and the command line
  // carries it as --hover).
  std::function<void()> on_hover;

  int mouse_x() const { return mouse_x_; }
  int mouse_y() const { return mouse_y_; }
  HoverInfo Hovered() const { return HoverAt(v_->session(), v_->camera(), mouse_x_, mouse_y_); }

 protected:
  void resizeEvent(QResizeEvent*) override {
    // A resize keeps a fitted camera fitted, and leaves a moved one where it was. The old
    // window refitted on every resize, but its panels were an overlay; here a dock opening is
    // a resize, and throwing away a zoom because a panel was shown would be a camera that
    // moves when nothing was asked of it.
    // The FIRST size is where the camera is seated from --camera / --zoom / --hover (or fit,
    // when none was given), so it must not be refitted on top.
    const bool first = v_->screen_w() == 0;
    const bool was_fit = !first && v_->CameraIsFit();
    v_->SetScreen(width(), height());
    if (was_fit) v_->Fit();
  }

  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    Paint(p, width(), height(), mouse_x_, mouse_y_);
    if (on_painted) on_painted();
  }

  void mousePressEvent(QMouseEvent* e) override {
    if (e->button() == Qt::LeftButton) {
      dragging_ = true;
      drag_from_ = e->position().toPoint();
    }
    setFocus(Qt::MouseFocusReason);
  }
  void mouseReleaseEvent(QMouseEvent* e) override {
    if (e->button() == Qt::LeftButton) dragging_ = false;
  }
  void mouseMoveEvent(QMouseEvent* e) override {
    const QPoint pt = e->position().toPoint();
    if (dragging_) {
      v_->Pan(pt.x() - drag_from_.x(), pt.y() - drag_from_.y());
      drag_from_ = pt;
    }
    mouse_x_ = pt.x();
    mouse_y_ = pt.y();
    if (on_hover) on_hover();
    update();
  }
  void leaveEvent(QEvent*) override {
    mouse_x_ = mouse_y_ = -1;
    if (on_hover) on_hover();
    update();
  }
  void wheelEvent(QWheelEvent* e) override {
    // Every notch applies, rather than one per event: zoom is the one control where a dropped
    // repeat is visible as the wrong magnification. Partial notches (touchpads) accumulate.
    wheel_accum_ += e->angleDelta().y();
    const QPoint pt = e->position().toPoint();
    while (wheel_accum_ >= 120) {
      v_->ZoomAt(pt.x(), pt.y(), 1.25f);
      wheel_accum_ -= 120;
    }
    while (wheel_accum_ <= -120) {
      v_->ZoomAt(pt.x(), pt.y(), 0.8f);
      wheel_accum_ += 120;
    }
    update();
  }
  void keyPressEvent(QKeyEvent* e) override {
    const int pan_step = 12;
    switch (e->key()) {
      case Qt::Key_Left: v_->Pan(pan_step, 0); break;
      case Qt::Key_Right: v_->Pan(-pan_step, 0); break;
      case Qt::Key_Up: v_->Pan(0, pan_step); break;
      case Qt::Key_Down: v_->Pan(0, -pan_step); break;
      default: QWidget::keyPressEvent(e); return;
    }
    update();
  }

 public:
  // Zoom about the pointer when there is one, otherwise about the centre. Used by +/-.
  void ZoomKey(float factor) {
    const int x = mouse_x_ >= 0 ? mouse_x_ : width() / 2;
    const int y = mouse_y_ >= 0 ? mouse_y_ : height() / 2;
    v_->ZoomAt(x, y, factor);
    update();
  }

  // The whole picture, into any painter of any size. paintEvent calls it for the window; the
  // composed snapshot calls it into a QImage with no window at all, so the two are one code path.
  void Paint(QPainter& p, int w, int h, int mouse_x, int mouse_y) const {
    const Camera& cam = v_->camera();
    const QImage& img = v_->world_image();
    p.fillRect(0, 0, w, h, ToQ(BackdropColour()));

    // The cells. A transform from world-image pixels to screen, nearest-neighbour.
    p.save();
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.translate(-cam.cx * cam.scale, -cam.cy * cam.scale);
    p.scale(cam.scale, cam.scale);
    p.drawImage(QPointF(0, 0), img);
    p.restore();

    PaintGrid(p, w, h);
    if (v_->view() == View::Flow) PaintFlowArrows(p, w, h);
    if (v_->hud().show) {
      PaintLegend(p, h);
      PaintHover(p, w, h, mouse_x, mouse_y);
      PaintStatus(p, w);
      PaintEventLog(p, w, h);
    }
  }

 private:
  // A rounded, translucent card behind text, so the text stays readable over white-hot magma.
  void Card(QPainter& p, const QRect& r) const {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(70, 78, 92), 1.0));
    p.setBrush(QColor(16, 18, 24, 225));
    p.drawRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    p.restore();
  }

  // The visible cell rectangle, in world-image coordinates, clamped to the world.
  void VisibleCells(int w, int h, int* x0, int* y0, int* x1, int* y1) const {
    const Camera& cam = v_->camera();
    const int ww = v_->world().width(), wh = v_->world().height();
    *x0 = std::max(0, static_cast<int>(std::floor(cam.cx)));
    *y0 = std::max(0, static_cast<int>(std::floor(cam.cy)));
    *x1 = std::min(ww, static_cast<int>(std::ceil(cam.cx + w / cam.scale)));
    *y1 = std::min(wh, static_cast<int>(std::ceil(cam.cy + h / cam.scale)));
  }

  // Cell boundaries, once a cell is big enough on screen to be read one at a time. Faint on
  // purpose: the grid is for counting cells, and it must not change how a colour reads.
  void PaintGrid(QPainter& p, int w, int h) const {
    const Camera& cam = v_->camera();
    if (cam.scale < 12.0f) return;
    int x0, y0, x1, y1;
    VisibleCells(w, h, &x0, &y0, &x1, &y1);
    if (x1 <= x0 || y1 <= y0) return;
    p.save();
    p.setPen(QPen(QColor(255, 255, 255, 28), 0));
    const double top = (y0 - cam.cy) * cam.scale, bottom = (y1 - cam.cy) * cam.scale;
    const double left = (x0 - cam.cx) * cam.scale, right = (x1 - cam.cx) * cam.scale;
    for (int x = x0; x <= x1; ++x) {
      const double sx = (x - cam.cx) * cam.scale;
      p.drawLine(QPointF(sx, top), QPointF(sx, bottom));
    }
    for (int y = y0; y <= y1; ++y) {
      const double sy = (y - cam.cy) * cam.scale;
      p.drawLine(QPointF(left, sy), QPointF(right, sy));
    }
    p.restore();
  }

  // One arrow per moving cell in the flow view, once cells are big enough to hold one. The
  // colour already says which way (hue) and how fast (brightness); the arrow says it in a way
  // nobody has to decode. Length is the speed over the view's range, capped at the cell.
  // Screen y runs down and the sim's runs up, so the y component is negated.
  void PaintFlowArrows(QPainter& p, int w, int h) const {
    const Camera& cam = v_->camera();
    if (cam.scale < 10.0f) return;
    const Session& s = v_->session();
    const GameDataUpdate* g = s.frame();
    if (g == nullptr || g->propertyTextureFlow == nullptr) return;
    const Range& r = v_->range();
    const float hi = (r.valid && r.hi > 0.0f) ? r.hi : 1.0f;
    int x0, y0, x1, y1;
    VisibleCells(w, h, &x0, &y0, &x1, &y1);
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(255, 255, 255, 200), std::max(1.0, cam.scale / 16.0)));
    const int sh = s.height();
    for (int yi = y0; yi < y1; ++yi) {
      for (int x = x0; x < x1; ++x) {
        const int cell = (sh - 1 - yi) * s.width() + x;
        float fx, fy;
        FlowAt(g, cell, &fx, &fy);
        const float mag = std::sqrt(fx * fx + fy * fy);
        if (mag <= 0.0f) continue;
        const double len = std::min(1.0, static_cast<double>(mag / hi)) * 0.45 * cam.scale;
        if (len < 2.0) continue;
        const double ux = fx / mag, uy = -fy / mag;
        const QPointF c((x + 0.5 - cam.cx) * cam.scale, (yi + 0.5 - cam.cy) * cam.scale);
        const QPointF tip(c.x() + ux * len, c.y() + uy * len);
        const QPointF tail(c.x() - ux * len, c.y() - uy * len);
        p.drawLine(tail, tip);
        const double head = std::max(2.0, len * 0.45);
        const double ax = -ux * head, ay = -uy * head;
        p.drawLine(tip, QPointF(tip.x() + ax * 0.866 - ay * 0.5, tip.y() + ay * 0.866 + ax * 0.5));
        p.drawLine(tip, QPointF(tip.x() + ax * 0.866 + ay * 0.5, tip.y() + ay * 0.866 - ax * 0.5));
      }
    }
    p.restore();
  }

  // What the colours mean, bottom left: a ramp with its ends labelled for the scalar views,
  // swatches for the categorical ones, and a scale bar in cells for every view. The ramps are
  // drawn from the same functions RenderWorld colours the cells with, so the key cannot
  // disagree with the picture.
  void PaintLegend(QPainter& p, int h) const {
    const View view = v_->view();
    const Range& r = v_->range();
    p.setFont(mono_);
    const QFontMetrics fm(mono_);
    const int pad = 6, lh = fm.height();
    const int bar_w = 180, bar_h = std::max(8, lh / 2);

    struct Swatch {
      Rgb c;
      std::string label;
    };
    std::vector<Swatch> swatches;
    std::string title, lo_text, hi_text;
    float ramp_from = 0.0f;  // where the ramp starts (the ext and attr views start at 0.2)
    bool ramp = false, hue = false;
    char buf[64];
    switch (view) {
      case View::Temperature:
        ramp = r.valid;
        title = "temperature";
        snprintf(buf, sizeof(buf), "%.1f K", static_cast<double>(r.lo));
        lo_text = buf;
        snprintf(buf, sizeof(buf), "%.1f K", static_cast<double>(r.hi));
        hi_text = buf;
        break;
      case View::Mass:
        ramp = r.valid;
        title = "mass (log)";
        snprintf(buf, sizeof(buf), "%.3g kg", static_cast<double>(r.lo));
        lo_text = buf;
        snprintf(buf, sizeof(buf), "%.3g kg", static_cast<double>(r.hi));
        hi_text = buf;
        break;
      case View::Ext:
      case View::Attr:
        ramp = r.valid;
        ramp_from = 0.2f;
        title = ViewLabel(v_->session(), view);
        snprintf(buf, sizeof(buf), "%.4g", static_cast<double>(r.lo));
        lo_text = buf;
        snprintf(buf, sizeof(buf), "%.4g", static_cast<double>(r.hi));
        hi_text = buf;
        swatches.push_back({DimPhase(3), view == View::Ext ? "at default" : "no value"});
        break;
      case View::Flow:
        hue = true;
        title = "flow: hue = direction, brightness = speed";
        snprintf(buf, sizeof(buf), "0 .. %.3g", static_cast<double>(r.valid ? r.hi : 1.0f));
        hi_text = buf;
        break;
      case View::Phase:
        title = "phase";
        swatches = {{Rgb{20, 20, 28}, "vacuum"},       {Rgb{80, 170, 235}, "gas"},
                    {Rgb{30, 80, 200}, "liquid"},      {Rgb{110, 105, 100}, "solid"},
                    {Rgb{200, 160, 70}, "unstable solid"}};
        break;
      case View::SimCheck:
        title = "simcheck (Klei's error map)";
        for (int c = 0; c < simcheck::kClassCount; ++c) {
          const auto cl = static_cast<simcheck::Class>(c);
          swatches.push_back({SimCheckColour(cl), simcheck::Meaning(cl)});
        }
        break;
      case View::Sunlight:
        ramp = true;
        title = "sunlight";
        lo_text = "0";
        hi_text = "255";
        break;
      default:
        break;  // element and liquid: hundreds of colours, and the hover card names each
    }

    // The scale bar: the largest round number of cells that fits in ~120 px.
    const Camera& cam = v_->camera();
    int cells = 1;
    for (int c : {1, 2, 5, 10, 20, 50, 100, 200, 500}) {
      if (c * cam.scale <= 120.0f) cells = c;
    }
    const int scale_px = std::max(1, static_cast<int>(cells * cam.scale + 0.5f));

    int rows = 1;  // the scale bar
    if (!title.empty()) ++rows;
    if (ramp || hue) rows += 2;
    rows += static_cast<int>(swatches.size());
    int width = std::max(bar_w, fm.horizontalAdvance(QString::fromStdString(title)));
    for (const Swatch& sw : swatches)
      width = std::max(width, lh + 6 + fm.horizontalAdvance(QString::fromStdString(sw.label)));
    width = std::max(width, scale_px + 6 + fm.horizontalAdvance("500 cells"));
    const QRect box(8, h - 8 - (rows * lh + 2 * pad), width + 2 * pad, rows * lh + 2 * pad);
    if (box.top() < 0) return;
    Card(p, box);
    int y = box.top() + pad;
    const int x = box.left() + pad;
    if (!title.empty()) {
      p.setPen(ToQ(HudInk()));
      p.drawText(x, y + fm.ascent(), QString::fromStdString(title));
      y += lh;
    }
    if (ramp || hue) {
      for (int i = 0; i < bar_w; ++i) {
        const float t = static_cast<float>(i) / (bar_w - 1);
        Rgb c;
        if (hue) {
          const float a = t * 6.28318530718f;
          c = FlowColour(std::cos(a), std::sin(a), 1.0f);
        } else if (view == View::Sunlight) {
          const uint8_t lit = static_cast<uint8_t>(t * 255.0f + 0.5f);
          c = Rgb{lit, lit, lit};
        } else {
          c = Thermal(ramp_from + (1.0f - ramp_from) * t);
        }
        p.fillRect(x + i, y + (lh - bar_h) / 2, 1, bar_h, ToQ(c));
      }
      y += lh;
      p.setPen(ToQ(HudDim()));
      if (hue) {
        p.drawText(x, y + fm.ascent(), "right   up   left   down   right");
      } else {
        p.drawText(x, y + fm.ascent(), QString::fromStdString(lo_text));
        const QString hi = QString::fromStdString(hi_text);
        p.drawText(x + bar_w - fm.horizontalAdvance(hi), y + fm.ascent(), hi);
      }
      y += lh;
    }
    for (const Swatch& sw : swatches) {
      p.fillRect(x, y + 2, lh - 4, lh - 4, ToQ(sw.c));
      p.setPen(ToQ(HudDim()));
      p.drawText(x + lh + 2, y + fm.ascent(), QString::fromStdString(sw.label));
      y += lh;
    }
    // The scale bar, last.
    p.fillRect(x, y + lh / 2 - 1, scale_px, 3, ToQ(HudInk()));
    snprintf(buf, sizeof(buf), "%d cell%s", cells, cells == 1 ? "" : "s");
    p.setPen(ToQ(HudDim()));
    p.drawText(x + scale_px + 6, y + fm.ascent(), buf);
  }

  void PaintStatus(QPainter& p, int w) const {
    const Session& s = v_->session();
    const HudState& hud = v_->hud();
    const std::vector<std::string> lines = HudLines(s, v_->view(), v_->range(), v_->camera(), hud);
    p.setFont(mono_);
    const QFontMetrics fm(mono_);
    const int pad = 6, lh = fm.height();
    int bw = 0;
    for (const std::string& l : lines) bw = std::max(bw, fm.horizontalAdvance(QString::fromStdString(l)));
    QRect box(8, 8, std::min(bw + 2 * pad, w - 16), static_cast<int>(lines.size()) * lh + 2 * pad);
    Card(p, box);
    p.setPen(ToQ(HudInk()));
    p.drawText(box.left() + pad, box.top() + pad + fm.ascent(), QString::fromStdString(lines[0]));
    p.setPen(ToQ(HudDim()));
    p.drawText(box.left() + pad, box.top() + pad + lh + fm.ascent(),
               QString::fromStdString(lines[1]));
    int y = box.bottom() + 6;

    // The determinism warning, on screen rather than only in the log: a run whose stream came
    // from the wall clock will not reproduce, and that is worth knowing while looking at it.
    auto boxed = [&](const std::string& text, QColor ink) {
      const QString t = QString::fromStdString(text);
      QRect r(8, y, fm.horizontalAdvance(t) + 2 * pad, lh + 2 * pad);
      Card(p, r);
      p.setPen(ink);
      p.drawText(r.left() + pad, r.top() + pad + fm.ascent(), t);
      y = r.bottom() + 6;
    };
    if (hud.wall_clock_seeded && !hud.seed_pinned && !hud.attached)
      boxed("wall-clock seeded: this run will NOT reproduce (--seed N)", ToQ(HudWarn()));
    if (!hud.message.empty()) boxed(hud.message, ToQ(HudInk()));
  }

  void PaintHover(QPainter& p, int w, int h, int mouse_x, int mouse_y) const {
    if (mouse_x < 0 || mouse_y < 0) return;
    v_->FreshExt(/*throttle=*/true);  // the card's ext lines; see Viewer::FreshExt
    const Camera& cam = v_->camera();
    const HoverInfo hv = HoverAt(v_->session(), cam, mouse_x, mouse_y);
    if (!hv.valid) return;

    // The hovered cell's outline, once a cell is big enough on screen to see one.
    if (cam.scale >= 3.0f) {
      const QRectF cell((hv.x - cam.cx) * cam.scale, (hv.y_img - cam.cy) * cam.scale, cam.scale,
                        cam.scale);
      p.save();
      p.setPen(QPen(Qt::white, 1.0));
      p.setBrush(Qt::NoBrush);
      p.drawRect(cell.adjusted(0.5, 0.5, -0.5, -0.5));
      p.restore();
    }

    const std::vector<std::string> lines = HoverLines(hv);
    p.setFont(mono_);
    const QFontMetrics fm(mono_);
    const int pad = 6, lh = fm.height();
    int bw = 0;
    for (const std::string& l : lines)
      bw = std::max(bw, fm.horizontalAdvance(QString::fromStdString(l)));
    bw += 2 * pad;
    const int bh = static_cast<int>(lines.size()) * lh + 2 * pad;
    // Flip the card to the other side of the cursor near an edge rather than let it run off:
    // a readout you have to move the mouse to read is not a readout.
    int bx = mouse_x + 16, by = mouse_y + 16;
    if (bx + bw > w) bx = mouse_x - 16 - bw;
    if (by + bh > h) by = mouse_y - 16 - bh;
    bx = std::max(0, bx);
    by = std::max(0, by);
    Card(p, QRect(bx, by, bw, bh));
    for (size_t i = 0; i < lines.size(); ++i) {
      p.setPen(ToQ((i % 2) ? HudDim() : HudInk()));
      p.drawText(bx + pad, by + pad + static_cast<int>(i) * lh + fm.ascent(),
                 QString::fromStdString(lines[i]));
    }
  }

  // The event log: the newest refusals, with the three ways a gap can happen kept apart. An
  // empty pane is a RESULT -- nothing was dropped -- so it is drawn whenever streams are
  // watched, rows or not.
  void PaintEventLog(QPainter& p, int w, int h) const {
    const HudState& hud = v_->hud();
    if (!hud.events || !hud.watching_events) return;
    const Session& s = v_->session();
    p.setFont(mono_);
    const QFontMetrics fm(mono_);
    const int pad = 6, lh = fm.height();
    // Sized to the widest line the formatter can produce, so the pane does not resize itself
    // as rows arrive.
    const int bw =
        fm.horizontalAdvance("t000000  msg 0xFFFFFFFF  unknown message  0000/0000 bytes") +
        2 * pad;
    const std::vector<extevents::Refusal>& log = s.refusals();
    const bool any_gap = s.events_lost() > 0 || s.events_dropped() > 0 || s.events_forgotten() > 0;
    const bool any_error = !s.events_error().empty();
    const int fixed_rows = 1 + (any_gap ? 1 : 0) + (any_error ? 1 : 0);
    const int bottom = h - 8;
    int rows = static_cast<int>(log.size());
    const int available = (bottom - 8 - 2 * pad) / lh - fixed_rows;
    rows = std::max(0, std::min(rows, available));
    const int bh = (fixed_rows + rows) * lh + 2 * pad;
    const QRect box(w - bw - 8, bottom - bh, bw, bh);
    if (box.left() < 0 || box.top() < 0) return;
    Card(p, box);
    int ty = box.top() + pad + fm.ascent();
    char line[192];
    snprintf(line, sizeof(line), "%s  %zu", s.event_streams().c_str(), log.size());
    p.setPen(ToQ(HudInk()));
    p.drawText(box.left() + pad, ty, line);
    ty += lh;
    if (any_error) {
      p.setPen(ToQ(HudWarn()));
      p.drawText(box.left() + pad, ty, QString::fromStdString(s.events_error()));
      ty += lh;
    }
    if (any_gap) {
      // Three counters, never summed: three different failures with three different fixes.
      snprintf(line, sizeof(line), "gaps: route %d  sim %d  log %d", s.events_lost(),
               s.events_dropped(), s.events_forgotten());
      p.setPen(ToQ(HudWarn()));
      p.drawText(box.left() + pad, ty, line);
      ty += lh;
    }
    p.setPen(ToQ(HudDim()));
    for (size_t i = log.size() - static_cast<size_t>(rows); i < log.size(); ++i) {
      p.drawText(box.left() + pad, ty, QString::fromStdString(extevents::Describe(log[i])));
      ty += lh;
    }
  }

  Viewer* v_;
  QFont mono_;
  bool dragging_ = false;
  QPoint drag_from_;
  int mouse_x_ = -1, mouse_y_ = -1;
  int wheel_accum_ = 0;
};

}  // namespace qtui
}  // namespace simviz
