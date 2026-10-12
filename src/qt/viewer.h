// The window's state and every action it can take, with no Qt in it.
//
// This is RunWindow's loop body taken apart. The old loop read raw input, mutated a dozen
// locals, advanced the sim and painted, all in one function; under Qt the input arrives as
// actions and the painting happens in paintEvent, so what is left -- the state, and what each
// action does to it -- lives here. The widgets in this directory read it and call it; none of
// them touches the Session directly. That is what keeps the rule the old loop's header stated:
//
//   THE SIM ADVANCES ONLY IN Frame() AND THE STEP/SEEK ACTIONS. A repaint, a resize, a hover
//   or a drag can never move it, so a timeline is reproducible within one process.
//
// INCLUDED FROM main.cpp, AFTER the tool functions it calls (Probe, Digest, ExtList, ...). This
// program is one translation unit by design (build.sh is one compiler line), and those
// functions are main.cpp's; this header is the window half of main.cpp, split out so it can be
// read, not so it can be compiled alone.

#pragma once

#include <QImage>

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "../child.h"
#include "../frame.h"
#include "../out.h"
#include "../panels.h"
#include "../qtapp.h"
#include "../session.h"
#include "../timeline.h"
#include "../views.h"

namespace simviz {
namespace qtui {

// What the command line decided before the window existed. The same struct RunWindow took.
using Options = WindowOptions;

class Viewer {
 public:
  // `quiet`: a composed snapshot builds a viewer only to paint one frame through it, and the
  // window's opening and closing lines ("timeline on", "window closed after N ticks") would be
  // false there -- no window opened -- and would land in output the suite hashes.
  Viewer(Session& s, View view, const Options& opt, bool quiet = false)
      : s_(s), opt_(opt), view_(view), quiet_(quiet) {
    hud_.dt = opt.dt;
    hud_.mass_fade = opt.mass_fade;
    hud_.seed_pinned = opt.seed_pinned;
    hud_.seed = opt.seed;
    hud_.rng = s.ShownRandomState();
    hud_.wall_clock_seeded = s.wall_clock_seeded();
    hud_.attached = opt.attached;
    hud_.endpoint = opt.endpoint;
    hud_.watching_events = s.watching_events();
    if (opt.attached) {
      // The frame the session was opened on is already a poll; say which one it was.
      hud_.unity_frame = s.source().unity_frame();
      hud_.poll_bytes = s.source().last_bytes();
    }

    fetch_ = opt.fetch_pinned ? opt.fetch_set : FetchSetForView(view, false);
    fetch_pinned_ = opt.fetch_pinned;
    poll_ms_ = opt.poll_ms;
    dt_seconds_ = opt.dt;
    timeline_interval_ = opt.timeline_interval;
    timeline_keep_ = opt.timeline_keep;
    range_mode_ = opt.fixed_range
                      ? panels::RangeMode::Fixed
                      : (opt.full_span ? panels::RangeMode::Full : panels::RangeMode::Scan);
    range_lo_ = opt.fixed.lo;
    range_hi_ = opt.fixed.hi;

    // The timeline is started BEFORE the first range scan and the first render, because
    // starting it restores tick 0 (see timeline.h) and everything drawn from here on has to
    // be of the run that will actually be scrubbed, not of the pristine boot.
    tl_.Configure(opt.timeline_interval, opt.timeline_keep, opt.dt);
    timeline_on_ = opt.timeline && !opt.attached;
    if (timeline_on_) {
      std::string terr;
      if (!tl_.Start(s, &terr)) {
        out::Printf("timeline off: %s\n", terr.c_str());
        timeline_on_ = false;
      } else {
        out::Printf("timeline on: one checkpoint every %d ticks, %d kept, %.2f MB each\n",
                    tl_.interval(), tl_.keep(), tl_.bytes() / (1024.0 * 1024.0));
      }
    }
    ApplyRange();
    Render();
    tests_.corpus = opt.attached ? std::string() : std::string(opt.corpus);
    tests_.seeded = true;
  }

  ~Viewer() {
    // A child whose output nobody will read again. Killed rather than orphaned: its stdout is
    // a pipe this process is about to close.
    if (child_.running()) child_.Stop();
    if (quiet_) return;
    if (timeline_on_) {
      out::Printf("timeline: head %d, %zu checkpoints every %d ticks (%.1f MB), %d thins,\n"
                  "  %d seeks replaying %d ticks\n",
                  tl_.head(), tl_.count(), tl_.interval(), tl_.bytes() / (1024.0 * 1024.0),
                  tl_.thins(), tl_.seeks(), tl_.replayed_ticks());
    }
    if (opt_.attached) {
      out::Printf("window closed after %d polls of %s\n", s_.ticks(), opt_.endpoint.c_str());
    } else {
      out::Printf("window closed after %d ticks\n", s_.ticks());
    }
  }

  Viewer(const Viewer&) = delete;
  Viewer& operator=(const Viewer&) = delete;

  // ---------------------------------------------------------------------------- the clock
  //
  // Called by the window's timer. Offline and playing, it ticks ONCE -- the window only calls
  // it again after that tick has been painted, which is the old loop's "one tick per displayed
  // frame". Attached, following is paced by the poll interval, never by the display.
  // Returns true when anything on screen may have changed.
  bool Frame() {
    bool changed = false;
    if (child_.running() && child_.Poll(&capture_)) {
      char msg[96];
      snprintf(msg, sizeof(msg), "finished, exit code %lu",
               static_cast<unsigned long>(child_.exit_code()));
      test_status_ = msg;
      out::Printf("---- %s\n", msg);
      changed = true;
    }
    if (opt_.attached) {
      const double now = NowSeconds();
      if (hud_.playing && now >= next_poll_) {
        PollOnce();
        next_poll_ = NowSeconds() + poll_ms_ / 1000.0;
        changed = true;
      }
      // Drained on the same schedule as the frame, and deliberately NOT only while following:
      // a held session still wants to know what the game refused while it looked.
      if (s_.watching_events() && now >= next_event_poll_) {
        s_.PollEvents();
        next_event_poll_ = NowSeconds() + poll_ms_ / 1000.0;
        changed = true;
      }
    } else if (hud_.playing) {
      // Timed over the tick AND the render, not the tick alone: Tick() returns while the sim's
      // worker thread is still stepping, and the first read of the frame waits for it, so a
      // clock around Tick() alone read 0.2 ms over a 31.5 ms substep (measured:
      // 200 headless ticks of the reference corpus took 6.3 s).
      const double t0 = NowSeconds();
      if (TickOnce()) {
        Render();
        hud_.last_tick_ms = static_cast<float>((NowSeconds() - t0) * 1000.0);
      }
      changed = true;
    }
    if (!hud_.message.empty() && NowSeconds() > message_until_) {
      hud_.message.clear();
      changed = true;
    }
    // A seek, a view switch or a tool can dirty the world between clocks; it is rendered here,
    // once, however many of them landed.
    if (world_dirty_) {
      Render();
      changed = true;
    }
    return changed;
  }

  // --------------------------------------------------------------------------- transport
  void TogglePlay() { hud_.playing = !hud_.playing; }
  void SetPlaying(bool on) { hud_.playing = on; }

  // One step forward: a tick at the head of history, a seek when behind it (a checkpoint near
  // the target beats replaying the whole way there). Attached, a poll.
  void StepForward(int n = 1) {
    // Offline and playing, the clock is already ticking once per displayed frame and a step
    // adds nothing to it -- the old loop ran `playing ? 1 : steps` ticks, never both.
    if (!opt_.attached && hud_.playing) return;
    for (int i = 0; i < n; ++i) {
      if (opt_.attached) {
        if (!PollOnce()) break;
      } else if (timeline_on_ && s_.ticks() < tl_.head()) {
        Seek(s_.ticks() + 1);
      } else if (!TickOnce()) {
        break;
      }
    }
  }
  void StepBack(int n = 1) {
    if (timeline_on_) Seek(s_.ticks() - n);
  }
  void TenForward() {
    if (!timeline_on_) return;
    if (s_.ticks() >= tl_.head()) StepForward(10);
    else Seek(s_.ticks() + 10);
  }
  void TenBack() { StepBack(10); }
  void Home() { if (timeline_on_) Seek(tl_.base()); }
  void End() { if (timeline_on_) Seek(tl_.head()); }

  void Seek(int target) {
    if (!timeline_on_) return;
    std::string err;
    if (!tl_.Seek(s_, target, &err)) {
      hud_.playing = false;
      Message("seek failed: " + err, 8.0);
    }
    world_dirty_ = true;
  }

  // ------------------------------------------------------------------------------- views
  //
  // The availability check every way of switching goes through -- the number keys, the
  // dropdown, the menu. A texture view over a buffer the DLL does not publish would paint an
  // empty world and look like a sim with nothing in it, so it is refused with a message.
  // Returns false when refused; the view is then unchanged.
  bool SetView(View next) {
    if (next == view_) return true;
    // 8 and 9 only do something when a name was given for them.
    if (next == View::Ext && s_.ext_view_name().empty()) {
      Message("no extension property named -- set one on the live dock", 4.0);
      return false;
    }
    if (next == View::Attr && s_.attr_view_name().empty()) {
      Message("no element attribute named -- set one on the view dock", 4.0);
      return false;
    }
    if (next == View::Ext) FreshExt();
    // Live the question is asked of the SERVER's surface list, not of the current frame's
    // pointers: those are null for every texture the last poll did not ask for.
    const bool available =
        opt_.attached ? ViewPublishedLive(s_, next) : ViewAvailableIn(s_, next);
    if (!available) {
      Message(next == View::Ext
                  ? "no extension property named \"" + s_.ext_view_name() + "\" is carried"
                  : next == View::Attr
                        ? "no element in this table carries \"" + s_.attr_view_name() + "\""
                        : std::string(opt_.attached ? "this game publishes no "
                                                    : "this DLL publishes no ") +
                              ViewName(next) + " texture",
              4.0);
      return false;
    }
    view_ = next;
    // Live, the new view's arrays are very likely not in the frame on screen. Re-ask
    // immediately, and only then rescan -- a range scanned over a frame that lacks the array
    // would be scanned over mass.
    if (opt_.attached && !fetch_pinned_) {
      fetch_ = FetchSetForView(view_, false);
      std::string perr;
      if (!s_.Poll(fetch_, &perr)) {
        hud_.playing = false;
        Message("poll failed: " + perr, 8.0);
      }
      next_poll_ = NowSeconds() + poll_ms_ / 1000.0;
    }
    ApplyRange();
    world_dirty_ = true;
    return true;
  }

  void Rescan(bool announce) {
    ApplyRange();
    world_dirty_ = true;
    if (announce) Message("range rescanned", 2.0);
  }
  void SetRangeMode(panels::RangeMode m) {
    range_mode_ = m;
    Rescan(false);
  }
  void SetRangePinned(double lo, double hi) {
    range_lo_ = lo;
    range_hi_ = hi;
    if (range_mode_ == panels::RangeMode::Fixed) Rescan(false);
  }
  void SetMassFade(bool on) {
    if (hud_.mass_fade == on) return;
    hud_.mass_fade = on;
    world_dirty_ = true;
    Message(on ? "mass fade on" : "mass fade off", 2.0);
  }
  void ToggleMassFade() { SetMassFade(!hud_.mass_fade); }
  void SetDt(double dt) { dt_seconds_ = dt; hud_.dt = static_cast<float>(dt); }
  void SetPollMs(int ms) { poll_ms_ = ms; }

  // ------------------------------------------------------------------------------ camera
  //
  // The camera is the old Camera, in screen pixels of the world widget. The widget tells the
  // viewer its size; nothing else here knows a window exists.
  void SetScreen(int w, int h) {
    const bool first = screen_w_ == 0;
    screen_w_ = w;
    screen_h_ = h;
    if (first) {
      cam_ = SeatCamera(opt_.camera, world_.width(), world_.height(), w, h);
    }
    ClampCamera();
  }
  void Fit() { cam_.FitTo(world_.width(), world_.height(), screen_w_, screen_h_); }
  void ZoomAt(int sx, int sy, float factor) {
    cam_.ZoomAt(sx, sy, factor, kMinScale, kMaxScale);
    ClampCamera();
  }
  void Pan(int dx, int dy) {
    cam_.PanByScreen(dx, dy);
    ClampCamera();
  }
  bool CameraIsFit() const {
    Camera fit;
    fit.FitTo(world_.width(), world_.height(), screen_w_, screen_h_);
    return cam_.Equals(fit);
  }

  // ------------------------------------------------------------------- registries, live
  void ApplyExt(const std::string& name, bool live, bool published, const std::string& props) {
    s_.SetExtView(name, s_.ext_view_comp());
    ext_stale_ = true;  // a different subscription is a different table
    if (opt_.attached) {
      s_.set_ext_live(live || published);
      s_.set_ext_published(published, props);
    }
    ext_props_ = props;
    world_dirty_ = true;
  }
  void ApplyAttr(const std::string& name) {
    // Fetched here and not left to the render's conditional refresh: the name was just typed,
    // and the view dock has to be able to say whether it names anything before the switch.
    s_.SetAttrView(name, s_.attr_view_comp());
    s_.RefreshExtAttrs();
    world_dirty_ = true;
  }
  void ApplyEvents(const std::string& streams) {
    if (!opt_.attached) return;
    s_.set_event_streams(streams);
    hud_.watching_events = s_.watching_events();
    next_event_poll_ = 0.0;
  }
  // --attach-arrays, live. An empty list unpins and follows the view again; a bad name is
  // refused by name rather than quietly fetching less than was asked for.
  void ApplyArrays(const std::string& list) {
    if (!opt_.attached) return;
    if (list.empty()) {
      fetch_pinned_ = false;
      fetch_ = FetchSetForView(view_, false);
      out::Printf("array set follows the view again: %s\n", fetch_.Query().c_str());
    } else {
      attach::SurfaceSet wanted;
      std::string aerr;
      if (!wanted.ParseList(list, &aerr)) {
        out::Printf("--attach-arrays: %s\n", aerr.c_str());
        return;
      }
      // The three every reader dereferences without checking, added silently rather than
      // refused: asking for `mass` alone is reasonable to type and a crash is not an answer.
      wanted.Add(attach::kElementIdx);
      wanted.Add(attach::kMass);
      wanted.Add(attach::kTemperature);
      fetch_ = wanted;
      fetch_pinned_ = true;
      out::Printf("array set pinned to %zu: %s\n", fetch_.size(), fetch_.Query().c_str());
    }
    std::string perr;
    if (!s_.Poll(fetch_, &perr)) out::Printf("poll failed: %s\n", perr.c_str());
    world_dirty_ = true;
  }

  // ------------------------------------------------------------------------------- tools
  //
  // Each is one flag, run against the world on screen, reporting by printing -- which is why
  // the output pane exists. Only the two mutations change what is drawn, and they say so.
  void RunProbe(const std::string& rect) {
    FreshExt();
    int px = 0, py = 0, pw = 1, ph = 1;
    if (ParseProbe(rect.c_str(), &px, &py, &pw, &ph)) Probe(s_, px, py, pw, ph);
    else out::Printf("probe wants x,y or x,y,w,h -- got \"%s\"\n", rect.c_str());
  }
  void RunStats() {
    FreshExt();
    PrintQuantiles(s_, view_);
  }
  void RunDigest() {
    FreshExt();
    Digest(s_);
  }
  void RunExtList() {
    FreshExt();
    ExtList(s_);
  }
  void RunExtAttrs() { AttrList(s_); }
  void RunEventsList() { EventsList(s_); }
  void RunDumpElements() { DumpElements(opt_.corpus); }
  void RunDumpState(const std::string& path) {
    FreshExt();
    const std::string source = opt_.attached ? opt_.endpoint : std::string(opt_.corpus);
    if (!DumpState(s_, path.c_str(), source)) out::Printf("could not write %s\n", path.c_str());
  }
  void RunExtBlob(const std::string& path) {
    FreshExt();
    const std::vector<uint8_t> blob = s_.ExtCellStateBlob();
    FILE* f = fopen(path.c_str(), "wb");
    if (f == nullptr) {
      out::Printf("cannot write %s\n", path.c_str());
      return;
    }
    if (!blob.empty()) fwrite(blob.data(), 1, blob.size(), f);
    fclose(f);
    out::Printf("wrote %zu extension-blob bytes to %s\n", blob.size(), path.c_str());
  }
  void RunInject(const std::string& rect, double fraction) {
    int ix = 0, iy = 0, iw = 0, ih = 0;
    if (!ParseFour(rect.c_str(), &ix, &iy, &iw, &ih)) {
      out::Printf("inject wants x,y,w,h -- got \"%s\"\n", rect.c_str());
      return;
    }
    const int n = s_.InjectGasRect(ix, iy, iw, ih, static_cast<float>(fraction));
    if (n < 0) {
      out::Printf("the sim refused a zero-length frame while draining the injection\n");
    } else if (n == 0) {
      out::Printf("seeded nothing: no gas cell with mass in %d,%d %dx%d\n", ix, iy, iw, ih);
    } else {
      out::Printf("injected %d gas cells (%.0f%% of each cell's vanilla mass) into the "
                  "volume-fractions layer\n", n, fraction * 100.0);
      Reanchor("the gas injection");
    }
  }
  void RunExtProbe(const std::string& rect, double jk) {
    int ex = 0, ey = 0, ew = 0, eh = 0;
    if (!ParseFour(rect.c_str(), &ex, &ey, &ew, &eh)) {
      out::Printf("the thermal bonus wants x,y,w,h -- got \"%s\"\n", rect.c_str());
      return;
    }
    const int n = s_.WriteThermalBonusRect(ex, ey, ew, eh, static_cast<float>(jk));
    if (n <= 0) {
      out::Printf("wrote nothing into %d,%d %dx%d\n", ex, ey, ew, eh);
    } else {
      out::Printf("ext probe: wrote %.0f J/K into %d cells; checkpoint component is now "
                  "%zu bytes\n", jk, n, s_.ExtCellState().size());
      Reanchor("the thermal-bonus write");
    }
  }
  void ConfigureTimeline(int interval, int keep) {
    timeline_interval_ = interval;
    timeline_keep_ = keep;
    tl_.Configure(interval, keep, static_cast<float>(dt_seconds_));
    out::Printf("timeline: one checkpoint every %d ticks, %d kept -- from the next "
                "capture on\n", tl_.interval(), tl_.keep());
  }

  // The experiments run as a child process: each one rewrites the world dozens of times and
  // one of them is a crash repro, so running it here would replace what is on screen.
  void StartTest(const std::string& args) {
    if (child_.running()) return;
    std::string cerr;
    if (child_.Start(args, &cerr)) {
      test_status_ = "running";
      out::Printf("\n---- %s\n", child_.command().c_str());
    } else {
      test_status_ = cerr;
      out::Printf("%s\n", cerr.c_str());
    }
  }
  void StopTest() {
    if (!child_.running()) return;
    child_.Stop();
    test_status_ = "stopped";
    out::Printf("---- stopped\n");
  }

  // ---------------------------------------------------------------------------- readouts
  const Session& session() const { return s_; }
  Session& session() { return s_; }
  const Options& options() const { return opt_; }
  View view() const { return view_; }
  const Range& range() const { return range_; }
  panels::RangeMode range_mode() const { return range_mode_; }
  double range_lo() const { return range_lo_; }
  double range_hi() const { return range_hi_; }
  const Camera& camera() const { return cam_; }
  const HudState& hud() const { return hud_; }
  HudState& hud() { return hud_; }
  bool playing() const { return hud_.playing; }
  bool mass_fade() const { return hud_.mass_fade; }
  double dt() const { return dt_seconds_; }
  int poll_ms() const { return poll_ms_; }
  bool timeline_on() const { return timeline_on_; }
  const Timeline& timeline() const { return tl_; }
  int timeline_interval() const { return timeline_interval_; }
  int timeline_keep() const { return timeline_keep_; }
  bool fetch_pinned() const { return fetch_pinned_; }
  std::string fetch_query() const { return fetch_.Query(); }
  const std::string& ext_props() const { return ext_props_; }
  bool test_running() const { return child_.running(); }
  const std::string& test_status() const { return test_status_; }
  const Image& world() const { return world_; }
  // The world as Qt draws it, rebuilt only when the world image is. RGB32 rather than a view
  // of the Rgb buffer: that buffer is 3 bytes a pixel with unaligned rows, which QImage will
  // not paint from directly.
  const QImage& world_image() const { return world_qimage_; }
  out::Capture& capture() { return capture_; }
  panels::Tests& tests() { return tests_; }
  panels::Tools& tools() { return tools_; }
  int screen_w() const { return screen_w_; }
  int screen_h() const { return screen_h_; }

  void Message(const std::string& text, double seconds) {
    hud_.message = text;
    message_until_ = NowSeconds() + seconds;
  }

  void SetFps(float fps) { hud_.fps = fps; }

  // Copies the extension properties out of the sim if a tick has made them stale. `throttle`
  // is the hover card's: while playing it refreshes at most four times a second, so the card's
  // ext lines can lag the cells by up to 250 ms; paused, and for every other reader, it is
  // exact.
  void FreshExt(bool throttle = false) {
    if (!ext_stale_) return;
    if (throttle && hud_.playing && NowSeconds() - ext_at_ < 0.25) return;
    s_.RefreshExtProps();
    ext_stale_ = false;
    ext_at_ = NowSeconds();
  }

  // The bindings the old panels took, filled from this state. CommandLine() and TestCommand()
  // are still panels.h's; they are text, not drawing, and the parity claim they carry -- every
  // control is one flag -- does not care what draws the controls.
  std::string CommandLine(int win_w, int win_h, float ui_scale, bool hover_valid, int hx,
                          int hy) const {
    panels::Bindings b;
    View v = view_;
    bool fade = hud_.mass_fade;
    panels::RangeMode mode = range_mode_;
    double lo = range_lo_, hi = range_hi_, dt = dt_seconds_;
    int poll = poll_ms_;
    b.view = &v;
    b.mass_fade = &fade;
    b.range_mode = &mode;
    b.range_lo = &lo;
    b.range_hi = &hi;
    b.dt = &dt;
    b.poll_ms = &poll;
    b.attached = opt_.attached;
    b.source = opt_.attached ? opt_.endpoint : std::string(opt_.corpus);
    b.seed_pinned = opt_.seed_pinned;
    b.seed = opt_.seed;
    b.tick = s_.ticks();
    b.timeline_on = timeline_on_;
    b.ext_live = s_.ext_live();
    b.ext_published = s_.ext_published();
    b.ext_props = ext_props_;
    b.ext_view_name = s_.ext_view_name();
    b.attr_view_name = s_.attr_view_name();
    b.event_streams = s_.event_streams();
    b.width = win_w;
    b.height = win_h;
    b.ui_scale = ui_scale;
    b.fullscreen = opt_.fullscreen;
    b.large_font = qtapp::g_large_font;
    b.cam = &cam_;
    b.cam_is_fit = CameraIsFit();
    b.hover_valid = hover_valid;
    b.hover_cell_x = hx;
    b.hover_cell_y = hy;
    return panels::CommandLine(b);
  }

 private:
  // One place decides what the range is.
  void ApplyRange() {
    if (range_mode_ == panels::RangeMode::Fixed) {
      range_.lo = static_cast<float>(range_lo_);
      range_.hi = static_cast<float>(range_hi_);
      range_.valid = true;
    } else {
      range_ = ScanRange(s_, view_, range_mode_ == panels::RangeMode::Full);
    }
  }

  bool TickOnce() {
    if (!s_.Tick(dt_seconds_)) {
      out::Printf("tick %d failed: PrepareGameData returned null\n", s_.ticks());
      hud_.playing = false;
      Message("TICK FAILED -- the sim refused PrepareGameData", 60.0);
      return false;
    }
    if (timeline_on_) tl_.Note(s_);
    world_dirty_ = true;
    return true;
  }

  // A failed poll leaves the previous frame on screen -- attach.h validates a whole reply
  // before it publishes a pointer -- so this reports and stops following rather than blanking.
  bool PollOnce() {
    const double t0 = NowSeconds();
    std::string perr;
    if (!s_.Poll(fetch_, &perr)) {
      hud_.playing = false;
      Message("poll failed: " + perr, 8.0);
      return false;
    }
    hud_.last_tick_ms = static_cast<float>((NowSeconds() - t0) * 1000.0);
    hud_.unity_frame = s_.source().unity_frame();
    hud_.poll_bytes = s_.source().last_bytes();
    world_dirty_ = true;
    return true;
  }

  // A world-changing tool has run, so the timeline's checkpoints describe a world that no
  // longer happened; re-anchor at the tick the change landed on.
  void Reanchor(const char* what) {
    world_dirty_ = true;
    if (!timeline_on_) return;
    std::string terr;
    if (!tl_.Start(s_, &terr)) {
      out::Printf("  timeline off: %s\n", terr.c_str());
      timeline_on_ = false;
      return;
    }
    out::Printf("  timeline re-anchored at tick %d -- %s changed the world, so every "
                "checkpoint before it\n  describes one that no longer happened\n",
                s_.ticks(), what);
  }

  void Render() {
    RenderWorld(s_, view_, range_, &world_, hud_.mass_fade);
    // The extension table is now stale. It is copied out of the sim again only when something
    // reads it (FreshExt): every render in the ext view, whose colours ARE that table, and on
    // demand everywhere else. Measured on a real desktop: RefreshExtProps took 66.7 ms of a
    // frame whose render took 1.6 ms and whose paint took 0.5 ms, so copying it every frame
    // capped every view at about 15 fps for the sake of the hover card.
    ext_stale_ = true;
    if (IsExtView(view_)) FreshExt();
    if (IsAttrView(view_)) s_.RefreshExtAttrs();
    world_dirty_ = false;
    hud_.rng = s_.ShownRandomState();
    if (timeline_on_) {
      hud_.base = tl_.base();
      hud_.head = tl_.head();
      hud_.cp_interval = tl_.interval();
      hud_.cp_bytes = tl_.bytes();
      hud_.cp_ticks = tl_.checkpoint_ticks();
      hud_.branched = tl_.branched();
    }
    const int w = world_.width(), h = world_.height();
    if (world_qimage_.width() != w || world_qimage_.height() != h)
      world_qimage_ = QImage(w, h, QImage::Format_RGB32);
    const Rgb* src = world_.data();
    for (int y = 0; y < h; ++y) {
      QRgb* row = reinterpret_cast<QRgb*>(world_qimage_.scanLine(y));
      for (int x = 0; x < w; ++x, ++src) row[x] = qRgb(src->r, src->g, src->b);
    }
  }

  void ClampCamera() {
    if (screen_w_ > 0 && screen_h_ > 0)
      cam_.Clamp(world_.width(), world_.height(), screen_w_, screen_h_);
  }

  Session& s_;
  Options opt_;
  View view_;
  bool quiet_ = false;
  HudState hud_;
  Range range_;
  panels::RangeMode range_mode_ = panels::RangeMode::Scan;
  double range_lo_ = 0.0, range_hi_ = 0.0;
  double dt_seconds_ = 0.2;
  int poll_ms_ = 200;
  int timeline_interval_ = 10, timeline_keep_ = 8;
  attach::SurfaceSet fetch_;
  bool fetch_pinned_ = false;
  std::string ext_props_;
  double next_poll_ = 0.0, next_event_poll_ = 0.0;
  double message_until_ = 0.0;
  Timeline tl_;
  bool timeline_on_ = false;
  Image world_;
  QImage world_qimage_;
  bool world_dirty_ = false;
  bool ext_stale_ = true;
  double ext_at_ = 0.0;
  Camera cam_;
  int screen_w_ = 0, screen_h_ = 0;
  Child child_;
  std::string test_status_;
  out::Capture capture_;
  panels::Tests tests_;
  panels::Tools tools_;
};

}  // namespace qtui
}  // namespace simviz
