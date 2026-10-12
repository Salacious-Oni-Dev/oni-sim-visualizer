// The main window: the world in the middle, the menus and keys around it, and the clock.
//
// Every key the old window had is an action here, with the same key, so nobody who learned
// the old one has to relearn it -- and every action is also in a menu with its key beside it,
// which is the discoverability the bitmap help line was standing in for.
//
// THE CLOCK. One QTimer drives Viewer::Frame(). Offline and playing it runs flat out but never
// ticks twice without a paint in between, which is the old loop's "one tick per displayed
// frame". Otherwise it idles at ~60 Hz, which is enough for polls, the child process and the
// message timeout, and costs nothing while nothing changes.

#pragma once

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QKeySequence>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QScreen>
#include <QTimer>

#include <cstdio>
#include <memory>
#include <string>

#include <QToolBar>

#include "../out.h"
#include "../qtapp.h"
#include "docks.h"
#include "reference.h"
#include "timelinebar.h"
#include "viewer.h"
#include "worldwidget.h"

namespace simviz {
namespace qtui {

class MainWindow : public QMainWindow {
 public:
  MainWindow(Session& s, View view, const Options& opt)
      : viewer_(std::make_unique<Viewer>(s, view, opt)), opt_(opt) {
    // The capture gives every printing feature somewhere to print; installed here rather than
    // in main() so a headless run keeps writing straight to stdout with no ring in between.
    viewer_->capture().echo = true;
    out::SetSink(&viewer_->capture());

    char title[512];
    snprintf(title, sizeof(title), "oni-sim-visualizer -- %s",
             opt.attached ? opt.endpoint.c_str() : opt.corpus);
    setWindowTitle(title);

    world_ = new WorldWidget(viewer_.get(), opt.ui_scale, qtapp::g_large_font, this);
    setCentralWidget(world_);
    world_->on_painted = [this]() { OnPainted(); };

    // The scrub bar, along the bottom edge of the world. A toolbar so it can be hidden (t)
    // without a dock's title bar; only offline, because attached there is no history.
    if (viewer_->timeline_on()) {
      bar_tools_ = new QToolBar("timeline", this);
      bar_tools_->setObjectName("timeline");
      bar_tools_->setMovable(false);
      bar_ = new TimelineBar(viewer_.get(), opt.ui_scale, qtapp::g_large_font, bar_tools_);
      bar_->on_seek = [this]() { Changed(); };
      bar_tools_->addWidget(bar_);
      addToolBar(Qt::BottomToolBarArea, bar_tools_);
    }

    reference_ = new ReferenceWindow(this);

    Docks::Hooks hooks;
    hooks.relaunch = [this]() {
      relaunch_ = true;
      close();
    };
    hooks.save_png = [this]() { SavePng(); };
    hooks.reference = [this]() { ToggleReference(); };
    hooks.ui_sheet = [this]() { UiSheet(); };
    hooks.changed = [this]() { Changed(); };
    hooks.show_bar = [this](bool on) {
      if (bar_tools_) bar_tools_->setVisible(on);
    };
    hooks.bar_shown = [this]() { return bar_tools_ != nullptr && bar_tools_->isVisible(); };
    hooks.command_line = [this]() { return CommandLine(); };
    docks_ = std::make_unique<Docks>(this, viewer_.get(), hooks);

    BuildMenus();

    resize(opt.width, opt.height);
    timer_.setTimerType(Qt::PreciseTimer);
    QObject::connect(&timer_, &QTimer::timeout, [this]() { OnClock(); });
    timer_.start(kIdleMs);
    fps_window_start_ = NowSeconds();

    PrintKeys();
  }

  ~MainWindow() override {
    timer_.stop();
    // Children are destroyed by QWidget's destructor, AFTER this body has dropped the viewer,
    // and a field losing focus as it is destroyed emits editingFinished into a lambda that
    // calls the viewer. Silence every child first, so nothing can call into what is gone.
    for (QObject* o : findChildren<QObject*>()) o->blockSignals(true);
    // The viewer prints its closing lines (timeline totals, "window closed after N ticks"), so
    // it goes before the sink it prints into.
    viewer_.reset();
    out::Flush();
    out::SetSink(nullptr);
  }

  bool relaunch() const { return relaunch_; }
  // A proof sheet is a still: no clock, and no frame rate measured off however long the
  // grabs happen to take -- that number is the one thing in a sheet that is about the host.
  void Freeze() {
    timer_.stop();
    frozen_ = true;
  }
  Docks& docks() { return *docks_; }
  ReferenceWindow* reference() { return reference_; }
  Viewer& viewer() { return *viewer_; }
  WorldWidget* world() { return world_; }

 protected:
  void closeEvent(QCloseEvent* e) override {
    timer_.stop();
    e->accept();
  }

 private:
  static constexpr int kIdleMs = 16;

  QAction* Add(QMenu* menu, const char* text, const QKeySequence& key,
               std::function<void()> fn, bool repeat = false) {
    QAction* a = menu->addAction(text);
    if (!key.isEmpty()) a->setShortcut(key);
    a->setAutoRepeat(repeat);
    // Window-wide, so the keys work whichever pane has focus -- except while a text field has
    // it, which Qt arranges by itself: a line edit claims printable keys before shortcuts see
    // them, which is exactly the old loop's `typing` gate.
    a->setShortcutContext(Qt::WindowShortcut);
    QObject::connect(a, &QAction::triggered, [this, fn]() {
      fn();
      Changed();
    });
    return a;
  }

  void BuildMenus() {
    Viewer& v = *viewer_;
    const bool live = opt_.attached;

    QMenu* file = menuBar()->addMenu("&File");
    Add(file, "Open another source...", QKeySequence(), [this]() {
      relaunch_ = true;
      close();
    });
    Add(file, "Save PNG of this frame", QKeySequence(Qt::Key_S), [this]() { SavePng(); });
    file->addSeparator();
    Add(file, "Quit", QKeySequence(Qt::Key_Escape), [this]() { close(); });

    QMenu* run = menuBar()->addMenu("&Run");
    play_ = Add(run, live ? "Follow / hold" : "Play / pause", QKeySequence(Qt::Key_Space),
                [&v]() { v.TogglePlay(); });
    play_->setCheckable(true);
    Add(run, live ? "Poll one frame" : "Step forward", QKeySequence(Qt::Key_Period),
        [&v]() { v.StepForward(1); }, true);
    if (!live) {
      Add(run, "Step back", QKeySequence(Qt::Key_Comma), [&v]() { v.StepBack(1); }, true);
      Add(run, "Ten ticks forward", QKeySequence(Qt::Key_BracketRight),
          [&v]() { v.TenForward(); }, true);
      Add(run, "Ten ticks back", QKeySequence(Qt::Key_BracketLeft), [&v]() { v.TenBack(); },
          true);
      Add(run, "Start of history", QKeySequence(Qt::Key_Home), [&v]() { v.Home(); });
      Add(run, "End of history", QKeySequence(Qt::Key_End), [&v]() { v.End(); });
    }

    QMenu* view = menuBar()->addMenu("&View");
    views_ = new QActionGroup(this);
    struct Entry {
      View v;
      const char* name;
      Qt::Key key;
    };
    const Entry entries[] = {
        {View::Element, "element", Qt::Key_1},     {View::Temperature, "temperature", Qt::Key_2},
        {View::Mass, "mass", Qt::Key_3},           {View::Phase, "phase", Qt::Key_4},
        {View::Flow, "flow", Qt::Key_5},           {View::Liquid, "liquid", Qt::Key_6},
        {View::Sunlight, "sunlight", Qt::Key_7},   {View::Ext, "ext (named property)", Qt::Key_8},
        {View::Attr, "attr (named attribute)", Qt::Key_9}, {View::SimCheck, "simcheck", Qt::Key_0},
    };
    for (const Entry& e : entries) {
      const View target = e.v;
      QAction* a = Add(view, e.name, QKeySequence(e.key), [&v, target]() { v.SetView(target); });
      a->setCheckable(true);
      a->setData(static_cast<int>(target));
      views_->addAction(a);
    }
    view->addSeparator();
    Add(view, "Rescan range", QKeySequence(Qt::Key_R), [&v]() { v.Rescan(true); });
    fade_ = Add(view, "Mass fade", QKeySequence(Qt::Key_M), [&v]() { v.ToggleMassFade(); });
    fade_->setCheckable(true);
    hud_ = Add(view, "HUD overlay", QKeySequence(Qt::Key_H),
               [&v]() { v.hud().show = !v.hud().show; });
    hud_->setCheckable(true);
    events_ = Add(view, "Event log", QKeySequence(Qt::Key_E), [&v]() {
      if (!v.hud().watching_events) return;
      v.hud().events = !v.hud().events;
      v.Message(v.hud().events ? "event log shown" : "event log hidden", 2.0);
    });
    events_->setCheckable(true);
    view->addSeparator();
    Add(view, "Fit to window", QKeySequence(Qt::Key_F), [&v]() { v.Fit(); });
    // Both the main and the keypad keys, as before; `=` because + is shift-= on most layouts.
    QAction* zin = Add(view, "Zoom in", QKeySequence(Qt::Key_Plus),
                       [this]() { world_->ZoomKey(1.25f); }, true);
    zin->setShortcuts({QKeySequence(Qt::Key_Plus), QKeySequence(Qt::Key_Equal)});
    Add(view, "Zoom out", QKeySequence(Qt::Key_Minus), [this]() { world_->ZoomKey(0.8f); },
        true);
    view->addSeparator();
    if (bar_tools_ != nullptr) {
      Add(view, "Scrub bar", QKeySequence(Qt::Key_T),
          [this]() { bar_tools_->setVisible(!bar_tools_->isVisible()); });
    }
    Add(view, "Panels", QKeySequence(Qt::Key_Tab), [this]() {
      // All together, as the old column was one thing: shown if any is hidden, else hidden.
      bool any_hidden = false;
      for (QDockWidget* d : docks_->docks()) any_hidden = any_hidden || d->isHidden();
      for (QDockWidget* d : docks_->docks()) d->setVisible(any_hidden);
    });
    Add(view, "Output pane", QKeySequence(Qt::Key_QuoteLeft), [this]() {
      docks_->console()->setVisible(!docks_->console()->isVisible());
    });

    QMenu* helpm = menuBar()->addMenu("&Help");
    Add(helpm, "Flag reference", QKeySequence(Qt::Key_F1), [this]() { ToggleReference(); });
    Add(helpm, "About Qt", QKeySequence(), [this]() { QApplication::aboutQt(); });
    SyncChecks();
  }

  void PrintKeys() {
    if (opt_.attached) {
      out::Printf("window open on %s. keys (also in the menus): space follow/hold,\n"
                  "  '.' poll one frame, 1-0 views, r rescan, f fit, +/- zoom, drag or arrows\n"
                  "  pan, m mass fade, h hud, s save png, tab panels, ` output, F1 flags, esc quit.\n",
                  opt_.endpoint.c_str());
    } else {
      out::Printf("window open. keys (also in the menus): space play/pause, '.' step,\n"
                  "  ',' step back, [ ] ten ticks, home/end ends of history, t timeline bar,\n"
                  "  1-0 views, r rescan, f fit, +/- zoom, drag or arrows pan, m mass fade,\n"
                  "  h hud, s save png, tab panels, ` output, F1 flags, esc quit.\n");
    }
  }

  // Menus show the state, whichever way it was changed -- a checkbox that keeps showing a
  // refused or superseded choice is a checkbox that lies.
  void SyncChecks() {
    Viewer& v = *viewer_;
    play_->setChecked(v.playing());
    fade_->setChecked(v.mass_fade());
    hud_->setChecked(v.hud().show);
    events_->setEnabled(v.hud().watching_events);
    events_->setChecked(v.hud().events && v.hud().watching_events);
    for (QAction* a : views_->actions())
      a->setChecked(a->data().toInt() == static_cast<int>(v.view()));
  }

  void Changed() {
    SyncChecks();
    world_->update();
    if (bar_) bar_->update();
    if (docks_) docks_->Refresh();
    // Starting play kicks the paint-driven chain (OnPainted) off with one frame. Only on the
    // edge: a second kick while the chain runs would tick twice for one paint.
    const bool playing = PlayingOffline();
    if (playing && !chain_playing_) Advance();
    chain_playing_ = playing;
  }

  bool PlayingOffline() const { return viewer_->playing() && !opt_.attached && !frozen_; }

  // One clock step: Frame() ticks once when playing offline, polls when attached and due.
  void Advance() {
    const bool was_playing = viewer_->playing();
    if (viewer_->Frame()) {
      painted_ = false;
      last_tick_at_ = NowSeconds();
      world_->update();
      if (bar_) bar_->update();
    }
    if (was_playing != viewer_->playing()) Changed();
  }

  // The 16 ms housekeeping clock: polls, the child process, the message timeout, the panels.
  //
  // Offline PLAY is not driven from here. It is driven by the paint (OnPainted queues the next
  // tick), so each displayed frame is exactly one tick by construction rather than by a flag,
  // and nothing spins while a paint is pending.
  void OnClock() {
    if (!PlayingOffline()) {
      Advance();
    } else if (!painted_ && NowSeconds() - last_tick_at_ >= 0.25) {
      // A minimised window never paints, so the chain stalls; keep it ticking regardless.
      Advance();
    }
    // The panels' readouts (tick, fps, the command line, new output) at a reading pace rather
    // than per tick: ten times a second is faster than anyone reads and costs nothing.
    const double now = NowSeconds();
    if (now - last_refresh_ >= 0.1) {
      last_refresh_ = now;
      docks_->Refresh();
    }
  }

  void ToggleReference() {
    if (reference_->isVisible()) reference_->hide();
    else reference_->show();
  }

  // The command line that reproduces what is on screen, from the viewer and the window.
  std::string CommandLine() const {
    const HoverInfo h = world_->Hovered();
    return viewer_->CommandLine(world_->width(), world_->height(), opt_.ui_scale, h.valid, h.x,
                                h.y_img);
  }

  // The help dock's proof sheet: this window as it is now, docks and all, beside the exe.
  void UiSheet() {
    const char* path = "simviz-ui.png";
    if (WritePng(FromQImage(grab().toImage()), path)) out::Printf("wrote %s\n", path);
    else out::Printf("could not write %s\n", path);
  }

  void OnPainted() {
    painted_ = true;
    if (frozen_) return;
    // The next tick, queued now and run once this paint has returned: one tick per frame.
    if (PlayingOffline()) QTimer::singleShot(0, this, [this]() { Advance(); });
    ++fps_frames_;
    const double now = NowSeconds();
    if (now - fps_window_start_ >= 0.5) {
      viewer_->SetFps(static_cast<float>(fps_frames_ / (now - fps_window_start_)));
      fps_frames_ = 0;
      fps_window_start_ = now;
    }
  }

  // The frame as it is on screen, overlay included, at the widget's own size.
  void SavePng() {
    char path[128];
    snprintf(path, sizeof(path), "simviz-%s-t%d-%d.png", ViewName(viewer_->view()),
             viewer_->session().ticks(), saved_frames_++);
    const QImage img = world_->grab().toImage();
    if (img.save(path)) {
      out::Printf("wrote %s\n", path);
      viewer_->Message(std::string("wrote ") + path, 3.0);
    } else {
      viewer_->Message(std::string("could not write ") + path, 3.0);
    }
  }

  std::unique_ptr<Viewer> viewer_;
  Options opt_;
  WorldWidget* world_ = nullptr;
  QTimer timer_;
  QAction* play_ = nullptr;
  QAction* fade_ = nullptr;
  QAction* hud_ = nullptr;
  QAction* events_ = nullptr;
  QActionGroup* views_ = nullptr;
  std::unique_ptr<Docks> docks_;
  QToolBar* bar_tools_ = nullptr;
  TimelineBar* bar_ = nullptr;
  ReferenceWindow* reference_ = nullptr;
  double last_refresh_ = 0.0;
  bool painted_ = true;
  double last_tick_at_ = 0.0;
  double fps_window_start_ = 0.0;
  int fps_frames_ = 0;
  int saved_frames_ = 0;
  bool relaunch_ = false;
  bool frozen_ = false;
  bool chain_playing_ = false;
};

// RunWindow's replacement: open the window on a booted session and run Qt's loop until it
// closes. Returns kRelaunch when the window asked for the start screen back.
inline int RunQtWindow(Session& s, View view, const Options& opt) {
  QApplication* app = qtapp::App();
  qtapp::SetFonts(opt.ui_scale, qtapp::g_large_font);
  bool relaunch = false;
  {
    MainWindow w(s, view, opt);
    if (opt.fullscreen) {
      w.showFullScreen();
    } else {
      w.show();
      // --size is the size of the WORLD, not of the window around it: it is the size a
      // composed snapshot is drawn at, and the command line the help dock prints carries the
      // world's size so that a pasted --hud --snapshot reproduces exactly the area on screen.
      // So the window grows by whatever the menus, docks and scrub bar take.
      QApplication::processEvents();
      const QSize extra = w.size() - w.world()->size();
      w.resize(opt.width + extra.width(), opt.height + extra.height());
    }
    w.world()->setFocus();
    app->exec();
    relaunch = w.relaunch();
  }
  return relaunch ? kRelaunch : 0;
}

inline std::string SuffixedPath(const std::string& p, const std::string& suffix) {
  const size_t dot = p.rfind('.');
  return (dot == std::string::npos ? p : p.substr(0, dot)) + suffix +
         (dot == std::string::npos ? std::string() : p.substr(dot));
}

// --ui-sample with a source: the whole window, drawn offscreen, once per dock -- so the panels
// are proof-read as PNGs with no desktop, exactly as the old column's tabs were. The output
// pane is open in every sheet; the flag reference gets a sheet of its own.
inline int WindowSample(Session& s, View view, const Options& opt, const std::string& png) {
  qtapp::App("offscreen");
  qtapp::SetFonts(opt.ui_scale, qtapp::g_large_font);
  // What this run reports is held until the window is gone. While it is open, out::Printf
  // feeds its output pane, so a "wrote <path>" line would be drawn into the NEXT sheet -- and
  // the path is a temporary directory, which would make every sheet after the first differ
  // between two runs of the same build.
  std::vector<std::string> report;
  int rc = 0;
  {
    MainWindow w(s, view, opt);
    w.Freeze();
    w.resize(opt.width, opt.height);
    w.show();
    w.docks().console()->show();
    w.resizeDocks({w.docks().console()}, {160}, Qt::Vertical);
    QApplication::processEvents();
    auto write = [&](const QImage& img, const std::string& path, const std::string& what) {
      if (rc != 0) return;
      if (!WritePng(FromQImage(img), path)) {
        report.push_back("could not write " + path);
        rc = 1;
        return;
      }
      report.push_back("wrote " + path + " (" + what + ")");
    };
    for (QDockWidget* d : w.docks().docks()) {
      d->raise();
      w.docks().Refresh();
      QApplication::processEvents();
      const std::string name = d->windowTitle().toStdString();
      write(w.grab().toImage(), SuffixedPath(png, "-" + name), name + " dock");
      // The dock's contents alone, too: the world and the HUD beside it change with the
      // source, so only this half can show whether a PANEL reads the source.
      write(d->widget()->grab().toImage(), SuffixedPath(png, "-" + name + "-panel"),
            name + " panel");
    }
    w.reference()->resize(900, 700);
    w.reference()->show();
    QApplication::processEvents();
    write(w.reference()->grab().toImage(), SuffixedPath(png, "-reference"), "flag reference");
    w.reference()->hide();
  }
  for (const std::string& line : report) out::Printf("%s\n", line.c_str());
  return rc;
}

// `--snapshot --hud`: the frame the window would show, drawn with no window. The SAME viewer
// and the SAME WorldWidget::Paint the window uses, into an image of the world's size, so a
// composed snapshot and the window cannot drift apart -- the property the old ComposeFrame
// existed for, kept by having one painter rather than two. `hover` is a cell, or -1.
inline Image ComposeSnapshot(Session& s, View view, Options opt, int fw, int fh, int hover_x,
                             int hover_y) {
  qtapp::App("offscreen");
  qtapp::SetFonts(opt.ui_scale, qtapp::g_large_font);
  opt.timeline = false;  // a snapshot scrubs nothing, and starting one would restore tick 0
  Viewer v(s, view, opt, /*quiet=*/true);
  v.SetScreen(fw, fh);
  WorldWidget w(&v, opt.ui_scale, qtapp::g_large_font);
  int mx = -1, my = -1;
  if (hover_x >= 0 && hover_y >= 0) {
    const Camera& cam = v.camera();
    mx = static_cast<int>((hover_x - cam.cx) * cam.scale);
    my = static_cast<int>((hover_y - cam.cy) * cam.scale);
  }
  QImage img(fw, fh, QImage::Format_RGB32);
  {
    QPainter p(&img);
    w.Paint(p, fw, fh, mx, my);
  }
  return FromQImage(img);
}

}  // namespace qtui
}  // namespace simviz
