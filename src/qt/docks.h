// The control panels, as Qt docks: every runtime knob this program has, with its name on it.
//
// These are the old column's seven tabs -- view, run, source, live, tools, tests, help -- one
// dock each, tabbed together on the left by default and movable anywhere. The rules the old
// panels.h stated still hold, and are why this file looks the way it does:
//
//   * EVERY CONTROL IS ONE FLAG. Each one's tooltip names the flag, and the help dock shows the
//     command line that reproduces the session (panels::CommandLine, unchanged).
//   * A CONTROL WRITES THE VIEWER, NEVER A COPY. Every edit calls a Viewer action; nothing here
//     holds sim state of its own.
//   * WHAT IS SHOWN IS WHAT IS TRUE. Refresh() rewrites every readout from the viewer, and a
//     choice the viewer refused (a view the DLL does not publish) snaps back rather than
//     staying selected.
//
// Refresh() never touches a field that has keyboard focus: a readout that overwrote what
// somebody was typing would be a field nobody could type into.

#pragma once

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <functional>
#include <string>
#include <vector>

#include "../help.h"
#include "../panels.h"
#include "viewer.h"

namespace simviz {
namespace qtui {

inline QString Q(const std::string& s) { return QString::fromStdString(s); }
inline std::string S(const QString& s) { return s.toStdString(); }

// One dock's contents: a scrollable column of rows. The builder helpers keep each tab's code
// reading like the old immediate-mode one -- a list of labelled rows, top to bottom.
class Column {
 public:
  explicit Column(QWidget* host) {
    auto* scroll = new QScrollArea(host);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    inner_ = new QWidget(scroll);
    lay_ = new QVBoxLayout(inner_);
    lay_->setContentsMargins(8, 8, 8, 8);
    lay_->setSpacing(6);
    scroll->setWidget(inner_);
    auto* outer = new QVBoxLayout(host);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);
  }

  QVBoxLayout* layout() { return lay_; }
  QWidget* parent() { return inner_; }

  QLabel* Text(const QString& t = QString()) {
    auto* l = new QLabel(t, inner_);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay_->addWidget(l);
    return l;
  }
  // The explanatory prose the old panels carried under each control. Kept: it is the
  // program's documentation in the place it is needed, not decoration.
  QLabel* Dim(const QString& t) {
    auto* l = new QLabel(t, inner_);
    l->setWordWrap(true);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QPalette pal = l->palette();
    pal.setColor(QPalette::WindowText, QColor(150, 160, 175));
    l->setPalette(pal);
    lay_->addWidget(l);
    return l;
  }
  QLabel* Warn(const QString& t) {
    QLabel* l = Text(t);
    l->setWordWrap(true);
    QPalette pal = l->palette();
    pal.setColor(QPalette::WindowText, QColor(240, 120, 100));
    l->setPalette(pal);
    return l;
  }
  QLabel* Good(const QString& t) {
    QLabel* l = Text(t);
    QPalette pal = l->palette();
    pal.setColor(QPalette::WindowText, QColor(120, 200, 140));
    l->setPalette(pal);
    return l;
  }
  QPushButton* Button(const QString& t, const QString& flag, std::function<void()> fn) {
    auto* b = new QPushButton(t, inner_);
    if (!flag.isEmpty()) b->setToolTip(flag);
    QObject::connect(b, &QPushButton::clicked, [fn]() { fn(); });
    lay_->addWidget(b);
    return b;
  }
  // A row of buttons sharing the width, like the old ButtonRow.
  std::vector<QPushButton*> Buttons(const std::vector<QString>& names,
                                    std::function<void(int)> fn) {
    auto* row = new QWidget(inner_);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    std::vector<QPushButton*> out;
    for (int i = 0; i < static_cast<int>(names.size()); ++i) {
      auto* b = new QPushButton(names[i], row);
      QObject::connect(b, &QPushButton::clicked, [fn, i]() { fn(i); });
      h->addWidget(b);
      out.push_back(b);
    }
    lay_->addWidget(row);
    return out;
  }
  QCheckBox* Check(const QString& t, const QString& flag, std::function<void(bool)> fn) {
    auto* c = new QCheckBox(t, inner_);
    if (!flag.isEmpty()) c->setToolTip(flag);
    QObject::connect(c, &QCheckBox::toggled, [fn](bool on) { fn(on); });
    lay_->addWidget(c);
    return c;
  }
  // A labelled text field. `commit` runs when editing finishes (enter, or focus leaves), which
  // is when the old fields committed -- not on every keystroke.
  QLineEdit* Field(const QString& label, const QString& placeholder, const QString& flag,
                   std::function<void(const std::string&)> commit) {
    auto* row = new QWidget(inner_);
    auto* f = new QFormLayout(row);
    f->setContentsMargins(0, 0, 0, 0);
    auto* e = new QLineEdit(row);
    e->setPlaceholderText(placeholder);
    if (!flag.isEmpty()) e->setToolTip(flag);
    f->addRow(label, e);
    if (commit) {
      QObject::connect(e, &QLineEdit::editingFinished, [e, commit]() { commit(S(e->text())); });
    }
    lay_->addWidget(row);
    return e;
  }
  QSpinBox* Int(const QString& label, int lo, int hi, const QString& flag,
                std::function<void(int)> commit) {
    auto* row = new QWidget(inner_);
    auto* f = new QFormLayout(row);
    f->setContentsMargins(0, 0, 0, 0);
    auto* sb = new QSpinBox(row);
    sb->setRange(lo, hi);
    sb->setKeyboardTracking(false);
    if (!flag.isEmpty()) sb->setToolTip(flag);
    f->addRow(label, sb);
    if (commit) QObject::connect(sb, &QSpinBox::valueChanged, [commit](int v) { commit(v); });
    lay_->addWidget(row);
    return sb;
  }
  QDoubleSpinBox* Double(const QString& label, double lo, double hi, int decimals,
                         const QString& flag, std::function<void(double)> commit) {
    auto* row = new QWidget(inner_);
    auto* f = new QFormLayout(row);
    f->setContentsMargins(0, 0, 0, 0);
    auto* sb = new QDoubleSpinBox(row);
    sb->setRange(lo, hi);
    sb->setDecimals(decimals);
    sb->setKeyboardTracking(false);
    if (!flag.isEmpty()) sb->setToolTip(flag);
    f->addRow(label, sb);
    if (commit)
      QObject::connect(sb, &QDoubleSpinBox::valueChanged, [commit](double v) { commit(v); });
    lay_->addWidget(row);
    return sb;
  }
  void Separator() {
    auto* line = new QFrame(inner_);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    lay_->addWidget(line);
  }
  void End() { lay_->addStretch(1); }

 private:
  QWidget* inner_;
  QVBoxLayout* lay_;
};

// Sets a widget's value without firing its change signal: Refresh() reports the viewer's state
// INTO a control, and that must not read back as the user asking for it.
template <typename W, typename V, typename Setter>
void Quietly(W* w, const V& v, Setter set) {
  if (w->hasFocus()) return;
  const bool old = w->blockSignals(true);
  set(w, v);
  w->blockSignals(old);
}

class Docks {
 public:
  // `owner` hosts the docks; `relaunch`, `save_png`, `reference`, `font_sheet` and `ui_sheet`
  // are window actions the panels offer buttons for.
  struct Hooks {
    std::function<void()> relaunch, save_png, reference, ui_sheet, changed;
    std::function<void(bool)> show_bar;
    std::function<bool()> bar_shown;
    std::function<std::string()> command_line;
  };

  Docks(QMainWindow* owner, Viewer* v, Hooks hooks) : v_(v), hooks_(std::move(hooks)) {
    const bool live = v_->options().attached;
    QDockWidget* first = nullptr;
    auto dock = [&](const char* title, std::function<void(Column&)> build) {
      auto* d = new QDockWidget(title, owner);
      d->setObjectName(QString("dock-") + title);
      d->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable |
                     QDockWidget::DockWidgetClosable);
      auto* host = new QWidget(d);
      Column c(host);
      build(c);
      c.End();
      d->setWidget(host);
      d->setMinimumWidth(320);
      owner->addDockWidget(Qt::LeftDockWidgetArea, d);
      if (first) owner->tabifyDockWidget(first, d);
      else first = d;
      docks_.push_back(d);
    };
    dock("view", [&](Column& c) { BuildView(c); });
    dock("run", [&](Column& c) { BuildRun(c, live); });
    dock("source", [&](Column& c) { BuildSource(c, live); });
    dock("live", [&](Column& c) { BuildLive(c, live); });
    dock("tools", [&](Column& c) { BuildTools(c, live); });
    dock("tests", [&](Column& c) { BuildTests(c); });
    dock("help", [&](Column& c) { BuildHelp(c); });
    first->raise();

    // The output pane, along the bottom: everything the tools print lands here.
    console_ = new QDockWidget("output", owner);
    console_->setObjectName("dock-output");
    output_ = new QPlainTextEdit(console_);
    output_->setReadOnly(true);
    output_->setMaximumBlockCount(static_cast<int>(out::kMaxCapturedLines));
    output_->setFont(QFont(qtfonts::Mono(), QApplication::font().pixelSize()));
    output_->setLineWrapMode(QPlainTextEdit::NoWrap);
    console_->setWidget(output_);
    owner->addDockWidget(Qt::BottomDockWidgetArea, console_);
    // Opened by ` -- the world keeps the height until somebody asks for the pane, and when
    // they do it takes about ten lines, not half the window.
    owner->resizeDocks({console_}, {160}, Qt::Vertical);
    console_->hide();
    Refresh();
  }

  const std::vector<QDockWidget*>& docks() const { return docks_; }
  QDockWidget* console() { return console_; }

  // Rewrites every readout from the viewer. Called after anything changed and on a slow clock.
  void Refresh() {
    const Session& s = v_->session();
    const bool live = v_->options().attached;

    // VIEW
    Quietly(view_combo_, static_cast<int>(v_->view()),
            [](QComboBox* w, int i) { w->setCurrentIndex(i); });
    ext_unnamed_->setVisible(v_->view() == View::Ext && s.ext_view_name().empty());
    attr_unnamed_->setVisible(v_->view() == View::Attr && s.attr_view_name().empty());
    Quietly(attr_field_, Q(s.attr_view_name()), [](QLineEdit* w, const QString& t) {
      if (w->text() != t) w->setText(t);
    });
    range_buttons_->button(static_cast<int>(v_->range_mode()))->setChecked(true);
    const bool pinned = v_->range_mode() == panels::RangeMode::Fixed;
    range_lo_->parentWidget()->setVisible(pinned);
    range_hi_->parentWidget()->setVisible(pinned);
    Quietly(range_lo_, v_->range_lo(), [](QDoubleSpinBox* w, double d) { w->setValue(d); });
    Quietly(range_hi_, v_->range_hi(), [](QDoubleSpinBox* w, double d) { w->setValue(d); });
    if (!pinned && v_->range().valid) {
      char r[96];
      snprintf(r, sizeof(r), "%.4g .. %.4g", static_cast<double>(v_->range().lo),
               static_cast<double>(v_->range().hi));
      range_readout_->setText(r);
    } else {
      range_readout_->setText(QString());
    }
    Quietly(fade_check_, v_->mass_fade(), [](QCheckBox* w, bool b) { w->setChecked(b); });
    Quietly(hud_check_, v_->hud().show, [](QCheckBox* w, bool b) { w->setChecked(b); });
    if (hooks_.bar_shown)
      Quietly(bar_check_, hooks_.bar_shown(), [](QCheckBox* w, bool b) { w->setChecked(b); });
    bar_check_->setVisible(v_->timeline_on());
    events_check_->setVisible(live && s.watching_events());
    Quietly(events_check_, v_->hud().events, [](QCheckBox* w, bool b) { w->setChecked(b); });
    {
      const Camera& cam = v_->camera();
      char c[128];
      snprintf(c, sizeof(c), "camera  %.9g,%.9g,%.9g", static_cast<double>(cam.cx),
               static_cast<double>(cam.cy), static_cast<double>(cam.scale));
      camera_readout_->setText(c);
      camera_note_->setText(
          v_->CameraIsFit()
              ? "Fit to the window, so the command line needs no --camera to land here."
              : "Moved from the fit, so the command line carries a --camera that puts it back "
                "exactly. --crop and --zoom cannot: they magnify by a whole number about a "
                "cell, and this scale is fractional.");
    }

    // RUN
    {
      char t[96];
      snprintf(t, sizeof(t), live ? "poll %d   %.0f fps" : "tick %d   %.0f fps", s.ticks(),
               static_cast<double>(v_->hud().fps));
      run_tick_->setText(t);
      transport_[1]->setText(v_->playing() ? (live ? "hold" : "pause") : (live ? "follow" : "play"));
      if (v_->timeline_on()) {
        snprintf(t, sizeof(t), "timeline %d .. %d", v_->timeline().base(), v_->timeline().head());
        timeline_readout_->setText(t);
      }
    }
    if (dt_spin_) Quietly(dt_spin_, v_->dt(), [](QDoubleSpinBox* w, double d) { w->setValue(d); });
    if (poll_spin_)
      Quietly(poll_spin_, v_->poll_ms(), [](QSpinBox* w, int i) { w->setValue(i); });

    // LIVE
    if (live) {
      Quietly(ext_live_, s.ext_live(), [](QCheckBox* w, bool b) { w->setChecked(b); });
      Quietly(ext_pub_, s.ext_published(), [](QCheckBox* w, bool b) { w->setChecked(b); });
      ext_props_->parentWidget()->setVisible(s.ext_published());
      collecting_->setVisible(s.watching_events());
      pinned_note_->setVisible(v_->fetch_pinned());
    }
    Quietly(ext_view_, Q(s.ext_view_name()), [](QLineEdit* w, const QString& t) {
      if (w->text() != t) w->setText(t);
    });

    // TOOLS
    if (verbose_)
      Quietly(verbose_, s.verbose, [](QCheckBox* w, bool b) { w->setChecked(b); });
    if (cp_every_) {
      Quietly(cp_every_, v_->timeline_interval(), [](QSpinBox* w, int i) { w->setValue(i); });
      Quietly(cp_kept_, v_->timeline_keep(), [](QSpinBox* w, int i) { w->setValue(i); });
    }

    // TESTS
    RefreshTests();

    // HELP
    if (hooks_.command_line) command_->setText(Q(hooks_.command_line()));

    // OUTPUT: append whatever the capture gained since last time. The capture is a bounded ring
    // that counts what it dropped, so "lines ever finished" is dropped + size, and the new ones
    // are the tail past what was already shown.
    const out::Capture& cap = v_->capture();
    const size_t total = cap.dropped + cap.lines.size();
    if (total > shown_) {
      size_t from = shown_ > cap.dropped ? shown_ - cap.dropped : 0;
      QScrollBar* sb = output_->verticalScrollBar();
      const bool at_end = sb->value() == sb->maximum();
      for (size_t i = from; i < cap.lines.size(); ++i) output_->appendPlainText(Q(cap.lines[i]));
      if (at_end) sb->setValue(sb->maximum());
      shown_ = total;
    }
  }

 private:
  void Changed() {
    if (hooks_.changed) hooks_.changed();
  }

  // ------------------------------------------------------------------------------- VIEW
  void BuildView(Column& c) {
    view_combo_ = new QComboBox(c.parent());
    for (const char* n : {"element", "temperature", "mass", "phase", "flow", "liquid", "sunlight",
                          "ext", "attr", "simcheck"})
      view_combo_->addItem(n);
    view_combo_->setToolTip("--view NAME, ext:<property> or attr:<attribute>");
    QObject::connect(view_combo_, &QComboBox::activated, [this](int i) {
      v_->SetView(static_cast<View>(i));
      Changed();
    });
    c.layout()->addWidget(view_combo_);
    c.Dim("keys 1-9 and 0 pick the same ten");
    ext_unnamed_ = c.Warn("no extension property named. Set one on the live dock, or pass "
                          "--view ext:<property>. The list comes out of the sim's own registry.");
    attr_unnamed_ = c.Warn("no element attribute named. Set one below, or pass --view "
                           "attr:<attribute>. --ext-attrs, or the tools dock, lists them.");
    attr_field_ = c.Field("attr view", "sim.molecular_mass", "--view attr:<attribute>",
                          [this](const std::string& t) {
                            v_->ApplyAttr(t);
                            Changed();
                          });
    c.Dim("Colours every cell by its ELEMENT's value for this attribute, so the picture is never "
          "finer than the element map. An element carrying no value is drawn as backdrop, which "
          "is a different answer from one whose value is zero.");
    c.Separator();

    auto* row = new QWidget(c.parent());
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    range_buttons_ = new QButtonGroup(row);
    const char* modes[] = {"scan", "full", "pin"};
    for (int i = 0; i < 3; ++i) {
      auto* rb = new QRadioButton(modes[i], row);
      rb->setToolTip("--range, --range full, --range lo,hi");
      range_buttons_->addButton(rb, i);
      h->addWidget(rb);
    }
    QObject::connect(range_buttons_, &QButtonGroup::idClicked, [this](int id) {
      v_->SetRangeMode(static_cast<panels::RangeMode>(id));
      Changed();
    });
    c.layout()->addWidget(row);
    range_lo_ = c.Double("lo", -1e30, 1e30, 4, "--range lo,hi", [this](double d) {
      v_->SetRangePinned(d, v_->range_hi());
      Changed();
    });
    range_hi_ = c.Double("hi", -1e30, 1e30, 4, "--range lo,hi", [this](double d) {
      v_->SetRangePinned(v_->range_lo(), d);
      Changed();
    });
    range_readout_ = c.Text();
    c.Dim("scan takes the 10th-90th percentile of the current view; full takes the whole span, "
          "outliers included; pin holds the two numbers so two runs can be compared by eye.");
    c.Button("rescan now (r)", "recomputes the range for the current view", [this]() {
      v_->Rescan(true);
      Changed();
    });
    fade_check_ = c.Check("mass fade (m)", "--no-mass-fade turns this off", [this](bool on) {
      v_->SetMassFade(on);
      Changed();
    });
    c.Dim("Off draws trace gas as boldly as rock. On is the default because a room of "
          "near-vacuum otherwise reads as hot as a magma chamber.");
    c.Separator();
    hud_check_ = c.Check("HUD overlay (h)", "--hud, for a snapshot", [this](bool on) {
      v_->hud().show = on;
      Changed();
    });
    bar_check_ = c.Check("scrub bar (t)", "", [this](bool on) {
      if (hooks_.show_bar) hooks_.show_bar(on);
      Changed();
    });
    events_check_ = c.Check("event log pane (e)", "", [this](bool on) {
      v_->hud().events = on;
      Changed();
    });
    c.Button("fit to window (f)", "", [this]() {
      v_->Fit();
      Changed();
    });
    camera_readout_ = c.Text();
    camera_readout_->setToolTip("--camera cx,cy,scale");
    camera_note_ = c.Dim(QString());
    c.Button("save PNG (s)", "--snapshot <out.png>", [this]() {
      if (hooks_.save_png) hooks_.save_png();
    });
  }

  // -------------------------------------------------------------------------------- RUN
  void BuildRun(Column& c, bool live) {
    run_tick_ = c.Text();
    transport_ = c.Buttons({"<", "play", ">"}, [this](int i) {
      if (i == 0) v_->StepBack(1);
      if (i == 1) v_->TogglePlay();
      if (i == 2) v_->StepForward(1);
      Changed();
    });
    if (live) transport_[0]->setEnabled(false);
    c.Dim(live ? "space follows or holds, . polls one frame"
               : "space toggles, . steps, , steps back");
    c.Separator();
    if (live) {
      poll_spin_ = c.Int("poll ms", 16, 60000, "--poll MS", [this](int ms) { v_->SetPollMs(ms); });
      c.Dim("How often a followed session asks the game for a frame. Each ask costs the game a "
            "main-thread copy, so this is a request rate, not a frame rate: the window still "
            "redraws as fast as it likes between polls.");
      c.Dim(Q("endpoint " + v_->options().endpoint));
      timeline_readout_ = c.Text();
      timeline_readout_->hide();
      return;
    }
    dt_spin_ = c.Double("dt seconds", 0.001, 10.0, 3, "--dt SECONDS", [this](double d) {
      v_->SetDt(d);
      Changed();
    });
    c.Dim("Elapsed seconds per step. 0.2 is one substep; the sim runs max(1, (dt + carry) * 5) "
          "of them, so a larger dt is more physics per displayed frame rather than a faster one.");
    c.Separator();
    timeline_readout_ = c.Text();
    if (v_->timeline_on()) {
      c.Buttons({"|<", "-10", "+10", ">|"}, [this](int i) {
        if (i == 0) v_->Home();
        if (i == 1) v_->TenBack();
        if (i == 2) v_->TenForward();
        if (i == 3) v_->End();
        Changed();
      });
      c.Dim("home/end, [ and ] -- drag the scrub bar to seek");
    } else {
      c.Dim("timeline off (--no-timeline)");
    }
  }

  // ----------------------------------------------------------------------------- SOURCE
  void BuildSource(Column& c, bool live) {
    const Session& s = v_->session();
    if (live) c.Good("attached to a running game");
    else c.Text("driving a SimDLL in this process");
    QLabel* src = c.Text(Q(live ? v_->options().endpoint : std::string(v_->options().corpus)));
    src->setWordWrap(true);
    char d[96];
    snprintf(d, sizeof(d), "%d x %d, %d cells", s.width(), s.height(), s.cells());
    c.Dim(d);
    if (v_->options().seed_pinned) {
      snprintf(d, sizeof(d), "seed pinned to 0x%08X", v_->options().seed);
      c.Dim(d);
    }
    c.Separator();
    c.Button("open another source...", "", [this]() {
      if (hooks_.relaunch) hooks_.relaunch();
    });
    c.Dim("Shuts this world down and returns to the start screen with these settings filled in, "
          "so a corpus can be swapped for a live game -- or one corpus for the next -- without "
          "leaving the program.");
  }

  // ------------------------------------------------------------------------------- LIVE
  void BuildLive(Column& c, bool live) {
    if (!live) {
      c.Dim("Offline. The extension registry is always read here, and event streams do not "
            "exist: a stream is produced by a running game and never serialised, so there is "
            "nothing in a corpus to replay.");
      ext_view_ = c.Field("ext view", "sim.gas_mass", "--view ext:<property>",
                          [this](const std::string& t) { CommitExt(t); });
      c.Dim("--view ext:<property>, then pick ext on the view dock");
      return;
    }
    QLineEdit* arrays = c.Field("arrays", "what the view needs", "--attach-arrays LIST, or 'all'",
                                [this](const std::string& t) {
                                  v_->ApplyArrays(t);
                                  Changed();
                                });
    (void)arrays;
    pinned_note_ = c.Dim("array set pinned by --attach-arrays");
    c.Dim("Which arrays each poll asks for. Empty follows the view, which is the default; a list "
          "PINS them, so changing the view no longer changes what is fetched -- you asked for "
          "those arrays and swapping them would answer a different question.");
    c.Separator();
    ext_live_ = c.Check("fetch /extcells", "--ext", [this](bool) { CommitExt(S(ext_view_->text())); });
    c.Dim("Off by default: this route makes the game wait for its sim thread to go idle and "
          "serialise every non-default property -- 12.7 MB on the reference world.");
    ext_pub_ = c.Check("use the published path", "--ext-published[=a,b]",
                       [this](bool) { CommitExt(S(ext_view_->text())); });
    c.Dim("/extcells?published=1 instead: no sim-thread wait, and only the properties named "
          "below. ASKING SUBSCRIBES -- the game starts publishing them on the next tick, and the "
          "route drops a subscription nothing has asked for in 120 seconds.");
    ext_props_ = c.Field("props", "sim.gas_mass,sim.gas_species", "",
                         [this](const std::string&) { CommitExt(S(ext_view_->text())); });
    c.Separator();
    ext_view_ = c.Field("ext view", "sim.gas_mass", "--view ext:<property>",
                        [this](const std::string& t) { CommitExt(t); });
    c.Dim("colours the ext view; component as name[1]");
    c.Separator();
    c.Field("events", "sim.message_refused", "--events=a,b", [this](const std::string& t) {
      v_->ApplyEvents(t);
      Changed();
    });
    c.Dim("Comma-separated streams to collect. Also subscribes. sim.message_refused is the only "
          "stream the simulation library declares on its own -- the messages it dropped because it does not "
          "implement them.");
    collecting_ = c.Good("collecting");
  }

  void CommitExt(const std::string& name) {
    const bool live = ext_live_ != nullptr && ext_live_->isChecked();
    const bool pub = ext_pub_ != nullptr && ext_pub_->isChecked();
    const std::string props = ext_props_ != nullptr ? S(ext_props_->text()) : std::string();
    v_->ApplyExt(name, live, pub, props);
    Changed();
  }

  // ------------------------------------------------------------------------------ TOOLS
  void BuildTools(Column& c, bool live) {
    panels::Tools& t = v_->tools();
    QLineEdit* probe = c.Field("probe", "x,y or x,y,w,h", "--probe x,y[,w,h], image coords",
                               nullptr);
    probe->setText(Q(t.probe_rect));
    c.Button("read those cells", "--probe", [this, probe]() {
      v_->RunProbe(S(probe->text()));
      Changed();
    });
    c.Dim("--probe: element, mass, temperature, ext props");
    c.Separator();
    c.Buttons({"stats", "digest"}, [this](int i) {
      if (i == 0) v_->RunStats();
      else v_->RunDigest();
      Changed();
    });
    c.Dim("--stats percentiles; --digest hashes 12 arrays");
    // The third is whichever of its two the source can answer: an event stream is a live
    // publication and the element table is lifted from a corpus file.
    c.Buttons({"ext list", "ext attrs", live ? "events list" : "elements"}, [this, live](int i) {
      if (i == 0) v_->RunExtList();
      if (i == 1) v_->RunExtAttrs();
      if (i == 2) {
        if (live) v_->RunEventsList();
        else v_->RunDumpElements();
      }
      Changed();
    });
    c.Dim(live ? "--ext-list, --ext-attrs, --events-list" : "--ext-list, --ext-attrs, --dump-elements");
    c.Separator();
    QLineEdit* state = c.Field("state file", "state.txt", "--dump-state <file>", nullptr);
    state->setText(Q(t.dump_state_path));
    c.Button("dump state", "--dump-state <file>", [this, state]() {
      v_->RunDumpState(S(state->text()));
      Changed();
    });
    QLineEdit* blob = c.Field("blob file", "extcells.bin", "--ext-blob <file>", nullptr);
    blob->setText(Q(t.ext_blob_path));
    c.Button("write ext blob", "--ext-blob <file>", [this, blob]() {
      v_->RunExtBlob(S(blob->text()));
      Changed();
    });
    c.Dim("--dump-state feeds --diff-state; --ext-blob raw");

    if (!live) {
      c.Separator();
      QLineEdit* inject = c.Field("inject gas", "x,y,w,h", "--inject-gas x,y,w,h", nullptr);
      QDoubleSpinBox* frac = c.Double("fraction", 0.0, 1.0, 3, "--inject-fraction F", nullptr);
      frac->setValue(t.inject_fraction);
      c.Button("seed the mixture", "", [this, inject, frac]() {
        v_->RunInject(S(inject->text()), frac->value());
        Changed();
      });
      QLineEdit* bonus = c.Field("thermal bonus", "x,y,w,h", "--ext-probe x,y,w,h", nullptr);
      QDoubleSpinBox* jk = c.Double("J/K", -1e12, 1e12, 1, "--ext-probe-jk J/K", nullptr);
      jk->setValue(t.ext_probe_jk);
      c.Button("write the bonus", "", [this, bonus, jk]() {
        v_->RunExtProbe(S(bonus->text()), jk->value());
        Changed();
      });
      c.Dim("Both change the world. The scrub timeline is re-anchored to now when they run, "
            "because its checkpoints describe a world that no longer exists -- seeking back into "
            "them would show a history that did not happen.");
    }
    c.Separator();
    verbose_ = c.Check("verbose (--verbose)", "--verbose",
                       [this](bool on) { v_->session().verbose = on; });
    if (!live) {
      QCheckBox* rr = c.Check("replay registrations", "--replay-registrations",
                              [this](bool on) { v_->session().replay_registrations = on; });
      rr->setChecked(v_->session().replay_registrations);
    }
    if (v_->timeline_on()) {
      cp_every_ = c.Int("checkpoint every", 1, 100000, "--timeline-interval", [this](int n) {
        v_->ConfigureTimeline(n, v_->timeline_keep());
        Changed();
      });
      cp_kept_ = c.Int("checkpoints kept", 4, 4096, "--timeline-keep", [this](int n) {
        v_->ConfigureTimeline(v_->timeline_interval(), n);
        Changed();
      });
    }
  }

  // ------------------------------------------------------------------------------ TESTS
  //
  // The experiments run as a separate process (child.h): each one rewrites the world dozens of
  // times, so running one here would replace what is on screen. The command shown IS the run.
  void BuildTests(Column& c) {
    panels::Tests& t = v_->tests();
    t.dll = "";
    test_combo_ = new QComboBox(c.parent());
    for (const char* n : {"checkpoint round trip", "timeline seek", "timeline diff", "load stress",
                          "digest log", "bisect two logs", "diff two states"})
      test_combo_->addItem(n);
    QObject::connect(test_combo_, &QComboBox::activated, [this](int i) {
      v_->tests().which = i;
      RefreshTests();
    });
    c.layout()->addWidget(test_combo_);

    auto browse = [this](QLineEdit* field, const char* title, const char* filter) {
      return [this, field, title, filter]() {
        const QString f = QFileDialog::getOpenFileName(field->window(), title, field->text(), filter);
        if (!f.isEmpty()) {
          field->setText(f);
          emit_edit(field);
        }
      };
    };
    corpus_ = c.Field("corpus", "sim_corpus.bin", "", [this](const std::string& s) {
      v_->tests().corpus = s;
      RefreshTests();
    });
    corpus_->setText(Q(t.corpus));
    corpus_browse_ = c.Button("browse...", "", browse(corpus_, "Pick a sim corpus",
                                                       "Sim corpus (*.bin);;All files (*.*)"));
    dll_ = c.Field("dll", "SimDLL.dll", "", [this](const std::string& s) {
      v_->tests().dll = s;
      RefreshTests();
    });
    seed_ = c.Field("seed", "optional", "", [this](const std::string& s) {
      v_->tests().seed = s;
      RefreshTests();
    });
    span_ = c.Int("span ticks", 1, 1000000, "", [this](int n) { v_->tests().span = n; RefreshTests(); });
    span_->setValue(t.span);
    interval_ = c.Int("interval", 1, 100000, "", [this](int n) { v_->tests().interval = n; RefreshTests(); });
    interval_->setValue(t.interval);
    keep_ = c.Int("kept", 4, 4096, "", [this](int n) { v_->tests().keep = n; RefreshTests(); });
    keep_->setValue(t.keep);
    loads_ = c.Int("loads", 1, 100000, "", [this](int n) { v_->tests().loads = n; RefreshTests(); });
    loads_->setValue(t.loads);
    between_ = c.Int("ticks between", 0, 100000, "",
                     [this](int n) { v_->tests().ticks_between = n; RefreshTests(); });
    between_->setValue(t.ticks_between);
    bare_ = c.Check("no AllocateCells first (--bare-load)", "",
                    [this](bool on) { v_->tests().bare_load = on; RefreshTests(); });
    replay_ = c.Check("replay registrations", "",
                      [this](bool on) { v_->tests().replay_registrations = on; RefreshTests(); });
    ticks_ = c.Int("ticks", 1, 1000000, "", [this](int n) { v_->tests().ticks = n; RefreshTests(); });
    ticks_->setValue(t.ticks);
    out_path_ = c.Field("write to", "digest.txt", "", [this](const std::string& s) {
      v_->tests().out_path = s;
      RefreshTests();
    });
    out_path_->setText(Q(t.out_path));
    path_a_ = c.Field("A", "a.txt", "", [this](const std::string& s) {
      v_->tests().path_a = s;
      RefreshTests();
    });
    browse_a_ = c.Button("browse A...", "", browse(path_a_, "Pick file A", "Text (*.txt);;All files (*.*)"));
    path_b_ = c.Field("B", "b.txt", "", [this](const std::string& s) {
      v_->tests().path_b = s;
      RefreshTests();
    });
    browse_b_ = c.Button("browse B...", "", browse(path_b_, "Pick file B", "Text (*.txt);;All files (*.*)"));
    diff_view_ = c.Field("view", "temperature", "", [this](const std::string& s) {
      v_->tests().diff_view = s;
      RefreshTests();
    });
    snapshot_ = c.Field("png", "diff.png", "", [this](const std::string& s) {
      v_->tests().snapshot = s;
      RefreshTests();
    });
    snapshot_->setText(Q(t.snapshot));
    test_about_ = c.Dim(QString());
    c.Separator();
    test_command_ = c.Text();
    test_command_->setWordWrap(true);
    run_test_ = c.Button("run", "", [this]() {
      v_->StartTest(panels::TestCommand(v_->tests()));
      Changed();
    });
    stop_test_ = c.Button("stop", "", [this]() {
      v_->StopTest();
      Changed();
    });
    test_state_ = c.Dim(QString());
    c.Dim("Runs as a separate process: these rewrite the world dozens of times, so running one "
          "here would replace what is on screen and freeze the window while it did.");
    tests_built_ = true;
  }

  // A browse button filled a field programmatically; commit it the way typing would.
  void emit_edit(QLineEdit* f) { emit f->editingFinished(); }

  void RefreshTests() {
    // Seeding a spin box with its first value fires its change handler, which lands here
    // before the fields below it exist. Nothing to refresh until the whole dock is built.
    if (!tests_built_) return;
    panels::Tests& t = v_->tests();
    const panels::Test which = static_cast<panels::Test>(t.which);
    Quietly(test_combo_, t.which, [](QComboBox* w, int i) { w->setCurrentIndex(i); });
    const bool needs_corpus = which != panels::Test::Bisect && which != panels::Test::DiffState;
    auto show = [](QWidget* w, bool on) {
      QWidget* row = w->parentWidget() && qobject_cast<QFormLayout*>(w->parentWidget()->layout())
                         ? w->parentWidget()
                         : w;
      row->setVisible(on);
    };
    for (QWidget* w : {static_cast<QWidget*>(corpus_), static_cast<QWidget*>(dll_),
                       static_cast<QWidget*>(seed_)})
      show(w, needs_corpus);
    corpus_browse_->setVisible(needs_corpus);
    const bool spans = which == panels::Test::Checkpoint || which == panels::Test::TimelineSeek ||
                       which == panels::Test::TimelineDiff;
    show(span_, spans);
    show(interval_, which == panels::Test::TimelineSeek || which == panels::Test::TimelineDiff);
    show(keep_, which == panels::Test::TimelineSeek || which == panels::Test::TimelineDiff);
    show(loads_, which == panels::Test::LoadStress);
    show(between_, which == panels::Test::LoadStress);
    bare_->setVisible(which == panels::Test::LoadStress);
    replay_->setVisible(which == panels::Test::LoadStress);
    show(ticks_, which == panels::Test::DigestLog);
    show(out_path_, which == panels::Test::DigestLog);
    const bool two = which == panels::Test::Bisect || which == panels::Test::DiffState;
    show(path_a_, two);
    show(path_b_, two);
    browse_a_->setVisible(two);
    browse_b_->setVisible(two);
    show(diff_view_, which == panels::Test::DiffState);
    show(snapshot_, which == panels::Test::DiffState);
    static const char* kAbout[] = {
        "Runs the span, restores the capture, runs it again, and compares. Also runs the same "
        "comparison WITHOUT restoring the random stream, because a pass means nothing unless the "
        "thing under test can be seen to fail.",
        "Seeks the scrub timeline everywhere it can reach and checks that arriving at a tick by "
        "replay gives the same world as ticking to it.",
        "Names what differs between a ticked world and a seeked one at the same tick, array by "
        "array, when the two disagree.",
        "THIS ONE IS A CRASH REPRO. One blob, re-Loaded. The tick after the third Load takes the "
        "process down -- which is the finding. It is a CHILD process, so what dies is the "
        "experiment and the exit code is reported here.",
        "One line per tick, hashing every published array. Two of these, from two DLLs or two "
        "machines, are what bisect compares.",
        "Names the first tick at which two digest logs part, and which array parted first. Needs "
        "no sim: it reads what earlier runs wrote.",
        "Compares two --dump-state files array by array and paints where they differ. Needs no "
        "sim either.",
    };
    test_about_->setText(kAbout[t.which]);
    test_command_->setText(Q("simviz.exe " + panels::TestCommand(t)));
    const bool running = v_->test_running();
    run_test_->setVisible(!running);
    run_test_->setEnabled(panels::TestReady(t));
    stop_test_->setVisible(running);
    std::string state = running ? "running -- output is in the output pane (`)" : v_->test_status();
    if (!running && !panels::TestReady(t)) state = "fill in the paths first";
    test_state_->setText(Q(state));
  }

  // ------------------------------------------------------------------------------- HELP
  void BuildHelp(Column& c) {
    c.Button("flag reference (F1)", "", [this]() {
      if (hooks_.reference) hooks_.reference();
    });
    const size_t missing = help::MissingControls();
    char line[96];
    if (missing == 0) {
      snprintf(line, sizeof(line), "all %zu flags have a control", help::FlagCount());
      c.Good(line);
    } else {
      snprintf(line, sizeof(line), "%zu of %zu flags have no control", missing, help::FlagCount());
      c.Warn(line);
    }
    c.Dim("Counted from the same table the reference renders, not written down here -- a flag "
          "with nowhere to click is the thing this window exists to not have, so the claim is "
          "computed rather than asserted.");
    c.Separator();
    c.Text("keys -- every one is also in the menus, beside its action");
    for (const char* k : {"space  play / pause", ".  ,   one tick forward / back",
                          "[  ]   ten ticks", "home end  the ends of the history",
                          "1-9 0  the ten views", "t  scrub bar    e  event log",
                          "h      HUD overlay", "r  rescan range   m  mass fade",
                          "f      fit to window", "s  save a PNG beside the exe",
                          "+ -    zoom (or wheel)", "arrows drag  pan",
                          "tab    the panels", "`      the output pane",
                          "F1     the flag reference", "esc    quit"})
      c.Dim(k);
    c.Separator();
    c.Text("this session, as a command");
    command_ = c.Text();
    command_->setWordWrap(true);
    command_->setFont(QFont(qtfonts::Mono(), QApplication::font().pixelSize()));
    c.Dim("Every control in these panels is one flag, so this line reproduces what is on screen "
          "-- in a script, on another machine, or in a bug report. The camera is in it too, "
          "which is what makes that true of the picture and not only of the window.");
    c.Dim("Swap --window for --hud --snapshot out.png and the same line writes this frame as a "
          "PNG with no window at all.");
    c.Separator();
    c.Text("proof sheet");
    c.Button("write this window as a PNG", "--ui-sample", [this]() {
      if (hooks_.ui_sheet) hooks_.ui_sheet();
    });
    c.Dim("simviz-ui.png, beside the exe. --ui-sample writes one per dock, and the start "
          "screen, with no window at all.");
  }

  Viewer* v_;
  Hooks hooks_;
  std::vector<QDockWidget*> docks_;
  QDockWidget* console_ = nullptr;
  QPlainTextEdit* output_ = nullptr;
  size_t shown_ = 0;

  QComboBox* view_combo_ = nullptr;
  QLabel* ext_unnamed_ = nullptr;
  QLabel* attr_unnamed_ = nullptr;
  QLineEdit* attr_field_ = nullptr;
  QButtonGroup* range_buttons_ = nullptr;
  QDoubleSpinBox* range_lo_ = nullptr;
  QDoubleSpinBox* range_hi_ = nullptr;
  QLabel* range_readout_ = nullptr;
  QCheckBox* fade_check_ = nullptr;
  QCheckBox* hud_check_ = nullptr;
  QCheckBox* bar_check_ = nullptr;
  QCheckBox* events_check_ = nullptr;
  QLabel* camera_readout_ = nullptr;
  QLabel* camera_note_ = nullptr;

  QLabel* run_tick_ = nullptr;
  std::vector<QPushButton*> transport_;
  QDoubleSpinBox* dt_spin_ = nullptr;
  QSpinBox* poll_spin_ = nullptr;
  QLabel* timeline_readout_ = nullptr;

  QCheckBox* ext_live_ = nullptr;
  QCheckBox* ext_pub_ = nullptr;
  QLineEdit* ext_props_ = nullptr;
  QLineEdit* ext_view_ = nullptr;
  QLabel* collecting_ = nullptr;
  QLabel* pinned_note_ = nullptr;

  QCheckBox* verbose_ = nullptr;
  QSpinBox* cp_every_ = nullptr;
  QSpinBox* cp_kept_ = nullptr;

  bool tests_built_ = false;
  QComboBox* test_combo_ = nullptr;
  QLineEdit* corpus_ = nullptr;
  QPushButton* corpus_browse_ = nullptr;
  QLineEdit* dll_ = nullptr;
  QLineEdit* seed_ = nullptr;
  QSpinBox* span_ = nullptr;
  QSpinBox* interval_ = nullptr;
  QSpinBox* keep_ = nullptr;
  QSpinBox* loads_ = nullptr;
  QSpinBox* between_ = nullptr;
  QCheckBox* bare_ = nullptr;
  QCheckBox* replay_ = nullptr;
  QSpinBox* ticks_ = nullptr;
  QLineEdit* out_path_ = nullptr;
  QLineEdit* path_a_ = nullptr;
  QPushButton* browse_a_ = nullptr;
  QLineEdit* path_b_ = nullptr;
  QPushButton* browse_b_ = nullptr;
  QLineEdit* diff_view_ = nullptr;
  QLineEdit* snapshot_ = nullptr;
  QLabel* test_about_ = nullptr;
  QLabel* test_command_ = nullptr;
  QPushButton* run_test_ = nullptr;
  QPushButton* stop_test_ = nullptr;
  QLabel* test_state_ = nullptr;

  QLabel* command_ = nullptr;
};

}  // namespace qtui
}  // namespace simviz
