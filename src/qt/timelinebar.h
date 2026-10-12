// The scrub bar: the span of history, its checkpoints, the playhead, and what the history costs.
//
// The track maps `base`..`head`, not 0..head: ticks before the timeline was anchored have no
// history, and mapping from 0 would spend the left of the bar on ticks no click can reach. The
// checkpoint marks are drawn so the resolution of the history is visible rather than implied --
// a seek between two marks costs a replay, and the picture says how far apart they are.
//
// A press latches the scrub until release, so dragging off the bar keeps scrubbing rather than
// turning the gesture into something else halfway through.

#pragma once

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QWidget>

#include <algorithm>
#include <cstdio>
#include <functional>

#include "../frame.h"
#include "../qtapp.h"
#include "viewer.h"
#include "worldwidget.h"

namespace simviz {
namespace qtui {

class TimelineBar : public QWidget {
 public:
  TimelineBar(Viewer* v, float ui_scale, bool large, QWidget* parent = nullptr)
      : QWidget(parent), v_(v), mono_(qtapp::MonoFont(ui_scale, large)) {
    const QFontMetrics fm(mono_);
    setFixedHeight(fm.height() + 14);
    setMinimumWidth(200);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  }

  std::function<void()> on_seek;

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.fillRect(rect(), QColor(16, 18, 24));
    const HudState& hud = v_->hud();
    const Session& s = v_->session();
    p.setFont(mono_);
    const QFontMetrics fm(mono_);
    char line[192];
    snprintf(line, sizeof(line), "tick %d / %d   %zu cp every %d   %.1f MB   %s", s.ticks(),
             hud.head, hud.cp_ticks.size(), hud.cp_interval, hud.cp_bytes / (1024.0 * 1024.0),
             hud.branched ? "REPLAYED -- diverges from the run" : "exact");
    const int label_w = LabelWidth(fm);
    const QRect track = Track(label_w);

    // Groove, the played part, the checkpoint marks, then the playhead on top.
    const int gy = track.center().y();
    p.fillRect(track.left(), gy - 1, track.width(), 3, ToQ(TimelineTrack()));
    const int px = TickToX(track, s.ticks());
    if (px > track.left()) p.fillRect(track.left(), gy - 1, px - track.left(), 3, ToQ(TimelineFill()));
    for (int t : hud.cp_ticks) p.fillRect(TickToX(track, t), gy - 4, 1, 9, ToQ(TimelineMark()));
    p.fillRect(px - 1, 3, 3, height() - 6, ToQ(TimelineHead()));

    p.setPen(ToQ(hud.branched ? HudWarn() : HudDim()));
    const int lx = std::max(track.right() + 10, width() - fm.horizontalAdvance(line) - 8);
    p.drawText(lx, (height() + fm.ascent() - fm.descent()) / 2, line);
  }

  void mousePressEvent(QMouseEvent* e) override {
    if (e->button() != Qt::LeftButton) return;
    scrubbing_ = true;
    SeekTo(e->position().toPoint().x());
  }
  void mouseMoveEvent(QMouseEvent* e) override {
    if (scrubbing_) SeekTo(e->position().toPoint().x());
  }
  void mouseReleaseEvent(QMouseEvent* e) override {
    if (e->button() == Qt::LeftButton) scrubbing_ = false;
  }

 private:
  // A FIXED reserve for the readout rather than its measured width: measuring would move the
  // track -- and where a click lands -- every time the tick gained a digit.
  int LabelWidth(const QFontMetrics& fm) const {
    return fm.horizontalAdvance(QString(64, QChar('0'))) + 16;
  }
  QRect Track(int label_w) const {
    const int w = std::max(2, width() - label_w - 16);
    return QRect(8, 0, w, height());
  }
  int TickToX(const QRect& track, int tick) const {
    const int base = v_->hud().base, head = v_->hud().head;
    const int span = head - base;
    if (span <= 0) return track.left();
    double f = static_cast<double>(tick - base) / span;
    f = std::min(1.0, std::max(0.0, f));
    return track.left() + static_cast<int>(f * (track.width() - 1) + 0.5);
  }
  void SeekTo(int x) {
    const QFontMetrics fm(mono_);
    const QRect track = Track(LabelWidth(fm));
    const int base = v_->hud().base, head = v_->hud().head;
    const int span = head - base;
    int target = base;
    if (span > 0 && track.width() >= 2) {
      double f = static_cast<double>(x - track.left()) / (track.width() - 1);
      f = std::min(1.0, std::max(0.0, f));
      target = base + static_cast<int>(f * span + 0.5);
    }
    if (target != v_->session().ticks()) {
      v_->Seek(target);
      if (on_seek) on_seek();
    }
  }

  Viewer* v_;
  QFont mono_;
  bool scrubbing_ = false;
};

}  // namespace qtui
}  // namespace simviz
