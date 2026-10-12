// The start screen, as a Qt dialog: what simviz.exe shows when run with no arguments.
//
// Same contract as the old one in launch.h, which still owns the Spec, the command line it is
// equivalent to, and the file-exists test:
//
//   * Only settings that must be fixed BEFORE a session exists are here: the source, the DLL,
//     the seed, the ticks to run first, the window size -- plus the view and the fade, which
//     decide what the first frame looks like. Everything else is a dock once the world is open.
//   * The command line is shown, live: this screen is a way to learn the flags, not to avoid
//     them, and the line it shows is the line that reproduces the session it will start.
//   * `error` is what went wrong on the last attempt, shown at the top. Started from Explorer
//     there is no console, so this is the only place a refused boot can be read.

#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <string>

#include "../launch.h"
#include "../qtapp.h"
#include "docks.h"

namespace simviz {
namespace qtui {

class Launcher : public QDialog {
 public:
  Launcher(launch::Spec* spec, const std::string& error) : spec_(spec) {
    setWindowTitle("oni-sim-visualizer");
    auto* lay = new QVBoxLayout(this);

    auto* title = new QLabel("<b>oni-sim-visualizer</b> &nbsp;-- look at an ONI simulation, cell by cell", this);
    lay->addWidget(title);
    lay->addWidget(Dim("Pick a source and press start. Everything else can be changed while the "
                       "world is on screen, from the panels the tab key opens."));
    if (!error.empty()) {
      auto* e = new QLabel(Q(error), this);
      e->setWordWrap(true);
      e->setTextInteractionFlags(Qt::TextSelectableByMouse);
      e->setStyleSheet("QLabel { color: rgb(240,120,100); background: rgb(40,26,26); padding: 6px; }");
      lay->addWidget(e);
    }

    // ---- source
    auto* src = new QGroupBox("source -- which simulation", this);
    auto* sl = new QVBoxLayout(src);
    corpus_mode_ = new QRadioButton("corpus (this program drives a SimDLL)", src);
    attach_mode_ = new QRadioButton("attach (a running game drives its own)", src);
    sl->addWidget(corpus_mode_);
    sl->addWidget(attach_mode_);
    pages_ = new QStackedWidget(src);
    sl->addWidget(pages_);

    auto* corpus_page = new QWidget(pages_);
    auto* cf = new QFormLayout(corpus_page);
    corpus_ = new QLineEdit(Q(spec->corpus), corpus_page);
    corpus_->setPlaceholderText("sim_corpus.bin");
    cf->addRow("corpus", WithBrowse(corpus_, "Pick a sim corpus", "Sim corpus (*.bin);;All files (*.*)"));
    corpus_state_ = new QLabel(corpus_page);
    corpus_state_->setWordWrap(true);
    cf->addRow("", corpus_state_);
    dll_ = new QLineEdit(Q(spec->dll), corpus_page);
    dll_->setPlaceholderText("SimDLL.dll");
    cf->addRow("SimDLL", WithBrowse(dll_, "Pick a SimDLL", "SimDLL (*.dll);;All files (*.*)"));
    dll_state_ = new QLabel(corpus_page);
    cf->addRow("", dll_state_);
    seed_ = new QLineEdit(Q(spec->seed), corpus_page);
    seed_->setPlaceholderText("leave empty to let the corpus decide");
    cf->addRow("seed", seed_);
    ticks_ = new QSpinBox(corpus_page);
    ticks_->setRange(0, 1000000);
    ticks_->setValue(spec->ticks);
    cf->addRow("ticks first", ticks_);
    dt_ = new QDoubleSpinBox(corpus_page);
    dt_->setRange(0.001, 10.0);
    dt_->setDecimals(3);
    dt_->setValue(spec->dt);
    cf->addRow("dt seconds", dt_);
    timeline_ = new QCheckBox("keep a scrub timeline", corpus_page);
    timeline_->setChecked(spec->timeline);
    cf->addRow("", timeline_);
    cf->addRow("", Dim("The timeline checkpoints the sim every few ticks so the bar can seek "
                       "backwards. Off is --no-timeline, which is what to use when a checkpoint "
                       "itself is what is being investigated."));
    pages_->addWidget(corpus_page);

    auto* attach_page = new QWidget(pages_);
    auto* af = new QFormLayout(attach_page);
    endpoint_ = new QLineEdit(Q(spec->endpoint), attach_page);
    endpoint_->setPlaceholderText("127.0.0.1:9788");
    af->addRow("endpoint", endpoint_);
    af->addRow("", Dim("host:port of oni-framework-api's debug server in a running game. The port is "
                       "the one the game's log prints when the bulk routes come up."));
    poll_ = new QSpinBox(attach_page);
    poll_->setRange(16, 60000);
    poll_->setValue(spec->poll_ms);
    af->addRow("poll ms", poll_);
    timeout_ = new QSpinBox(attach_page);
    timeout_->setRange(100, 600000);
    timeout_->setValue(spec->attach_timeout_ms);
    af->addRow("request timeout ms", timeout_);
    ext_live_ = new QCheckBox("fetch the extension registry (--ext)", attach_page);
    ext_live_->setChecked(spec->ext_live);
    af->addRow("", ext_live_);
    af->addRow("", Dim("Costs the game a sim-thread wait and a serialisation of every non-default "
                       "property -- 12.7 MB on the reference world. Off unless something needs it."));
    ext_pub_ = new QCheckBox("use the published path (--ext-published)", attach_page);
    ext_pub_->setChecked(spec->ext_published);
    af->addRow("", ext_pub_);
    props_ = new QLineEdit(Q(spec->ext_props), attach_page);
    props_->setPlaceholderText("sim.gas_mass");
    af->addRow("props", props_);
    events_ = new QLineEdit(Q(spec->events), attach_page);
    events_->setPlaceholderText("sim.message_refused");
    af->addRow("events", events_);
    af->addRow("", Dim("Both of those SUBSCRIBE: the game starts publishing on the next tick and "
                       "drops the subscription after 120 idle seconds."));
    pages_->addWidget(attach_page);
    lay->addWidget(src);

    // ---- first frame
    auto* view = new QGroupBox("first frame -- what to draw", this);
    auto* vf = new QFormLayout(view);
    view_ = new QComboBox(view);
    for (const char* n : {"element", "temperature", "mass", "phase", "flow", "liquid", "sunlight",
                          "ext", "attr", "simcheck"})
      view_->addItem(n);
    view_->setCurrentIndex(static_cast<int>(spec->view));
    vf->addRow("view", view_);
    ext_name_ = new QLineEdit(Q(spec->ext_name), view);
    ext_name_->setPlaceholderText("sim.gas_mass");
    vf->addRow("property", ext_name_);
    attr_name_ = new QLineEdit(Q(spec->attr_name), view);
    attr_name_->setPlaceholderText("sim.molecular_mass");
    vf->addRow("attribute", attr_name_);
    comp_ = new QSpinBox(view);
    comp_->setRange(0, 63);
    comp_->setValue(spec->view == View::Attr ? spec->attr_comp : spec->ext_comp);
    vf->addRow("component", comp_);
    fade_ = new QCheckBox("fade by mass", view);
    fade_->setChecked(spec->mass_fade);
    vf->addRow("", fade_);
    width_ = new QSpinBox(view);
    width_->setRange(320, 8192);
    width_->setValue(spec->width);
    vf->addRow("window width", width_);
    height_ = new QSpinBox(view);
    height_->setRange(240, 8192);
    height_->setValue(spec->height);
    vf->addRow("window height", height_);
    view_note_ = new QLabel(view);
    QPalette bad = view_note_->palette();
    bad.setColor(QPalette::WindowText, QColor(240, 120, 100));
    view_note_->setPalette(bad);
    vf->addRow("", view_note_);
    lay->addWidget(view);

    // ---- start
    command_ = new QLabel(this);
    command_->setWordWrap(true);
    command_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    command_->setFont(QFont(qtfonts::Mono(), font().pixelSize()));
    lay->addWidget(command_);
    buttons_ = new QDialogButtonBox(this);
    start_ = buttons_->addButton("start", QDialogButtonBox::AcceptRole);
    buttons_->addButton("quit", QDialogButtonBox::RejectRole);
    start_->setDefault(true);
    lay->addWidget(buttons_);
    ready_note_ = new QLabel(this);
    lay->addWidget(ready_note_);
    QObject::connect(buttons_, &QDialogButtonBox::accepted, [this]() {
      if (Ready()) accept();
    });
    QObject::connect(buttons_, &QDialogButtonBox::rejected, [this]() { reject(); });

    (spec->attach ? attach_mode_ : corpus_mode_)->setChecked(true);

    // Every edit rewrites the spec and the preview, so the line on screen is always the line
    // that "start" would run.
    auto sync = [this]() { Sync(); };
    for (QLineEdit* e : {corpus_, dll_, seed_, endpoint_, props_, events_, ext_name_, attr_name_})
      QObject::connect(e, &QLineEdit::textChanged, sync);
    for (QSpinBox* sb : {ticks_, poll_, timeout_, comp_, width_, height_})
      QObject::connect(sb, &QSpinBox::valueChanged, sync);
    QObject::connect(dt_, &QDoubleSpinBox::valueChanged, sync);
    for (QCheckBox* c : {timeline_, ext_live_, ext_pub_, fade_})
      QObject::connect(c, &QCheckBox::toggled, sync);
    QObject::connect(view_, &QComboBox::currentIndexChanged, sync);
    QObject::connect(corpus_mode_, &QRadioButton::toggled, sync);
    Sync();
  }

 private:
  QLabel* Dim(const QString& t) {
    auto* l = new QLabel(t, this);
    l->setWordWrap(true);
    QPalette pal = l->palette();
    pal.setColor(QPalette::WindowText, QColor(150, 160, 175));
    l->setPalette(pal);
    return l;
  }

  QWidget* WithBrowse(QLineEdit* field, const char* title, const char* filter) {
    auto* row = new QWidget(field->parentWidget());
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    field->setParent(row);
    h->addWidget(field, 1);
    auto* b = new QPushButton("browse...", row);
    h->addWidget(b);
    QObject::connect(b, &QPushButton::clicked, [this, field, title, filter]() {
      const QString f = QFileDialog::getOpenFileName(this, title, field->text(), filter);
      if (!f.isEmpty()) field->setText(QDir::toNativeSeparators(f));
    });
    return row;
  }

  bool Ready() const {
    return spec_->attach ? !spec_->endpoint.empty()
                         : (!spec_->corpus.empty() && launch::FileExists(spec_->corpus));
  }

  void Sync() {
    launch::Spec& s = *spec_;
    s.attach = attach_mode_->isChecked();
    pages_->setCurrentIndex(s.attach ? 1 : 0);
    s.corpus = S(corpus_->text());
    s.dll = S(dll_->text());
    s.seed = S(seed_->text());
    s.ticks = ticks_->value();
    s.dt = dt_->value();
    s.timeline = timeline_->isChecked();
    s.endpoint = S(endpoint_->text());
    s.poll_ms = poll_->value();
    s.attach_timeout_ms = timeout_->value();
    s.ext_live = ext_live_->isChecked();
    s.ext_published = ext_pub_->isChecked();
    s.ext_props = S(props_->text());
    s.events = S(events_->text());
    s.view = static_cast<View>(view_->currentIndex());
    s.ext_name = S(ext_name_->text());
    s.attr_name = S(attr_name_->text());
    s.ext_comp = s.view == View::Ext ? comp_->value() : 0;
    s.attr_comp = s.view == View::Attr ? comp_->value() : 0;
    s.mass_fade = fade_->isChecked();
    s.width = width_->value();
    s.height = height_->value();

    props_->setEnabled(s.ext_published);
    ext_name_->setEnabled(s.view == View::Ext);
    attr_name_->setEnabled(s.view == View::Attr);
    comp_->setEnabled(s.view == View::Ext || s.view == View::Attr);
    if (s.view == View::Ext && s.ext_name.empty()) view_note_->setText("the ext view needs a property name");
    else if (s.view == View::Attr && s.attr_name.empty()) view_note_->setText("the attr view needs an attribute name");
    else view_note_->setText(QString());

    auto paint = [](QLabel* l, const QString& t, QColor c) {
      l->setText(t);
      QPalette pal = l->palette();
      pal.setColor(QPalette::WindowText, c);
      l->setPalette(pal);
    };
    if (s.corpus.empty())
      paint(corpus_state_, "a corpus is the recorded stream of messages the game sent its sim "
                           "during one load", QColor(150, 160, 175));
    else if (launch::FileExists(s.corpus)) paint(corpus_state_, "found", QColor(120, 200, 140));
    else paint(corpus_state_, "no such file", QColor(240, 120, 100));
    if (!s.dll.empty() && !launch::FileExists(s.dll))
      paint(dll_state_, "no such file -- the boot will fail", QColor(240, 120, 100));
    else dll_state_->setText(QString());

    command_->setText(Q(launch::Command(s)));
    // A start that looks pressable and does nothing is worse than one that is visibly not ready.
    const bool ready = Ready();
    start_->setEnabled(ready);
    if (ready) paint(ready_note_, "enter starts, escape quits", QColor(150, 160, 175));
    else paint(ready_note_, s.attach ? "give an endpoint first" : "point at a corpus file first",
               QColor(240, 120, 100));
  }

  launch::Spec* spec_;
  QRadioButton* corpus_mode_;
  QRadioButton* attach_mode_;
  QStackedWidget* pages_;
  QLineEdit* corpus_;
  QLabel* corpus_state_;
  QLineEdit* dll_;
  QLabel* dll_state_;
  QLineEdit* seed_;
  QSpinBox* ticks_;
  QDoubleSpinBox* dt_;
  QCheckBox* timeline_;
  QLineEdit* endpoint_;
  QSpinBox* poll_;
  QSpinBox* timeout_;
  QCheckBox* ext_live_;
  QCheckBox* ext_pub_;
  QLineEdit* props_;
  QLineEdit* events_;
  QComboBox* view_;
  QLineEdit* ext_name_;
  QLineEdit* attr_name_;
  QSpinBox* comp_;
  QCheckBox* fade_;
  QSpinBox* width_;
  QSpinBox* height_;
  QLabel* view_note_;
  QLabel* command_;
  QDialogButtonBox* buttons_;
  QPushButton* start_;
  QLabel* ready_note_;
};

// --ui-sample: the start screen in both of its shapes, as PNGs, with no window, no sim and no
// game. Files are assumed to exist (launch::g_sample_assume_files_exist) so the sheet does not
// change with what happens to be on the disk beside the checkout.
inline bool LauncherSample(const launch::Spec& spec, const std::string& error, QImage* out) {
  qtapp::App("offscreen");
  qtapp::SetFonts(spec.ui_scale, qtapp::g_large_font);
  launch::Spec copy = spec;
  Launcher dlg(&copy, error);
  dlg.resize(dlg.sizeHint().expandedTo(QSize(700, 0)));
  dlg.show();
  QApplication::processEvents();
  *out = dlg.grab().toImage();
  dlg.hide();
  return !out->isNull();
}

// RunLauncher's replacement. `spec` is both the prefill and the result; false means the person
// quit, which is not a failure and prints nothing.
inline bool RunQtLauncher(launch::Spec* spec, const std::string& error) {
  qtapp::App();
  qtapp::SetFonts(spec->ui_scale, qtapp::g_large_font);
  launch::Spec edited = *spec;
  Launcher dlg(&edited, error);
  if (dlg.exec() != QDialog::Accepted) return false;
  *spec = edited;
  return true;
}

}  // namespace qtui
}  // namespace simviz
