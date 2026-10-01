// An immediate-mode widget layer painted into the software framebuffer.
//
// WHY THERE IS A UI LAYER AT ALL. This program grew as a command-line tool with a window
// attached: 51 flags, of which eight had any runtime control. Everything else -- what to
// probe, which arrays to fetch, how often to poll, which streams to watch, every dump, every
// test -- could only be chosen once, in argv, before the window opened. A person who runs the
// exe cannot reach those features, and a person who can reach them has to restart the process
// to change one number. This layer is what lets a control exist for each of them.
//
// WHY IMMEDIATE MODE. The alternative is a retained tree of widget objects holding copies of
// what they display, and this program displays a simulation that changes every tick: every
// copy is a chance for the panel to disagree with the frame. An immediate-mode control reads
// the live value on the frame it is drawn and writes back through the pointer it was handed,
// so there is nothing to synchronise and nothing to invalidate. The cost is that layout is
// recomputed every frame, which for a few hundred rectangles is nothing beside a 636x404
// blit.
//
// WHY IT IS PAINTED, NOT MADE OF NATIVE CONTROLS. The repo's whole testing story rests on the
// frame being ours: `--snapshot --hud` reproduces exactly what the window shows, with no
// window and no graphics context, which is how the overlay is proof-read offline (font.h's
// header records the two bugs that caught on the first composed frame). Native HWND controls
// live outside the framebuffer, so a UI built from them would be the one part of the program
// that no automated check could ever see. Everything here goes through font::DrawString and
// Image::FillRect, exactly like the HUD.
//
// IDENTITY IS THE BOUND POINTER, not the label. Two panels can both hold a field called
// "path", and a label-keyed focus would jump between them; the address of the value being
// edited is unique for as long as that value exists. Widgets with nothing bound (a button)
// hash their label together with a per-panel counter, which is stable as long as the panel's
// contents are, and a button that moves is a button that has been rebuilt anyway.
//
// AN EDIT BUFFER IS NOT THE VALUE. A field being typed into holds text that is not yet a
// number and may never be one; committing on every keystroke would make "1" out of a user
// halfway through typing "150" and act on it. So editing happens in a buffer, and the bound
// value changes on Enter or on losing focus. Escape abandons the edit.

#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "font.h"
#include "image.h"

namespace simviz {
namespace ui {

// ---------------------------------------------------------------------------------------
// Metrics. All in pixels, all derived from the 5x8 font so a change there moves the UI with
// it rather than leaving the two to drift.
// ---------------------------------------------------------------------------------------
constexpr int kPad = 4;                       // inside a panel, every side
inline int kRowH = font::kGlyphH + 6;      // 14: a row of text with room to click
constexpr int kRowGap = 2;
inline int kTitleH = font::kGlyphH + 7;
inline int kBoxSize = font::kGlyphH + 1;   // checkbox / radio square
// Re-derives the three above after font::UseLarge; they are read, never cached, after it.
inline void SyncFontMetrics() {
  kRowH = font::kGlyphH + 6;
  kTitleH = font::kGlyphH + 7;
  kBoxSize = font::kGlyphH + 1;
}
constexpr int kScrollW = 6;

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
  bool Contains(int px, int py) const {
    return px >= x && py >= y && px < x + w && py < y + h;
  }
};

// ---------------------------------------------------------------------------------------
// Colours. One table, so a panel and a pane cannot end up almost the same grey. Values are
// picked to sit over the world render without hiding it: the panel ground is dark and opaque
// rather than blended, because a blended control is a control you misread on a bright cell.
// ---------------------------------------------------------------------------------------
struct Theme {
  Rgb ground{18, 20, 26};
  Rgb ground_alt{26, 29, 37};
  Rgb edge{62, 68, 82};
  Rgb title{150, 200, 255};
  Rgb text{214, 220, 230};
  Rgb text_dim{128, 136, 150};
  Rgb text_bad{255, 138, 128};
  Rgb text_good{150, 230, 160};
  Rgb accent{88, 150, 240};
  Rgb accent_dim{48, 82, 132};
  Rgb field{10, 12, 16};
  Rgb hot{44, 52, 70};
  Rgb caret{255, 255, 255};
};

// ---------------------------------------------------------------------------------------
// One frame of input, copied out of Window. A struct rather than a Window reference because
// the launcher and the main loop feed this from the same window but at different points, and
// because a test can drive the UI with no window at all.
// ---------------------------------------------------------------------------------------
struct Input {
  int mouse_x = -1, mouse_y = -1;
  bool clicked = false;          // button went down this frame
  int click_x = -1, click_y = -1;
  bool mouse_down = false;       // still held
  int wheel = 0;
  std::string typed;             // ASCII, in order, control chars included
  int key_left = 0, key_right = 0, key_home = 0, key_end = 0, key_delete = 0;
};

// ---------------------------------------------------------------------------------------
// State that has to outlive a frame: what is focused, where the caret is, how far each pane
// is scrolled, which dropdown is open. The app owns one of these; nothing here is global,
// so two independent UIs (the launcher and the main window) cannot fight over focus.
// ---------------------------------------------------------------------------------------
constexpr int kScrollToTail = 0x40000000;

struct State {
  uint64_t focus = 0;            // id of the field being edited, 0 for none
  std::string edit;              // its buffer -- see the header: not the value
  int caret = 0;
  uint64_t open_dropdown = 0;
  // A deque, not a vector: Scroll hands back a reference and a later push_back on a
  // vector would move what it points at.
  std::deque<std::pair<uint64_t, int>> scrolls;   // id -> first visible line

  // A pane that has never been scrolled starts pinned to the TAIL, not the head. New
  // entries are seeded past any possible line count and clamped down on first use, which is
  // simpler than a separate "has been scrolled" flag and behaves identically. The tail is
  // the right default because output arrives at the bottom: a pane that opens showing the
  // first line of a 12,000-line run is showing the least useful line it has.
  int& Scroll(uint64_t id) {
    for (auto& kv : scrolls)
      if (kv.first == id) return kv.second;
    scrolls.push_back({id, kScrollToTail});
    return scrolls.back().second;
  }
};

inline uint64_t HashId(const char* s, uint64_t seed) {
  uint64_t h = seed ^ 0xcbf29ce484222325ull;
  for (const char* p = s; *p; ++p) {
    h ^= static_cast<uint8_t>(*p);
    h *= 0x100000001b3ull;
  }
  return h ? h : 1;
}

inline uint64_t PtrId(const void* p) {
  // The address, mixed so it cannot collide with a label hash by accident.
  return (reinterpret_cast<uintptr_t>(p) * 0x9e3779b97f4a7c15ull) | 1ull;
}

// ---------------------------------------------------------------------------------------
// Deferred drawing. A dropdown's list and a tooltip have to appear OVER whatever is drawn
// after them, and in an immediate-mode pass "after them" is most of the panel. Both are
// captured here and painted by Finish().
//
// Interaction is NOT deferred: the popup's rectangle is known the moment the dropdown is
// reached, so it hit-tests there and consumes the click, and only widgets that come LATER in
// the same panel could have been under it anyway.
// ---------------------------------------------------------------------------------------
struct Deferred {
  Rect rect;
  std::vector<std::string> lines;
  int selected = -1;
  bool is_tooltip = false;
};

class Ctx {
 public:
  Ctx(Image* img, State* state, const Input& in, const Theme& theme = Theme())
      : img_(img), st_(state), in_(in), th_(theme) {
    click_avail_ = in.clicked;
  }

  const Theme& theme() const { return th_; }
  const Input& input() const { return in_; }

  // ------------------------------------------------------------------ layout

  // Opens a panel whose top-left is (x, y) and whose width is fixed. Height is whatever the
  // contents need; Ends returns the rect actually used so a caller can stack panels.
  void BeginPanel(int x, int y, int w, const char* title) {
    panel_ = Rect{x, y, w, 0};
    cursor_y_ = y + kPad;
    panel_title_ = title ? title : "";
    if (!panel_title_.empty()) cursor_y_ += kTitleH;
    // The ground is painted at End, once the height is known, so it goes UNDER the widgets
    // -- an immediate-mode panel does not know how tall it is until its contents are done.
    // Contents are drawn into a scratch list of commands? No: they are drawn straight into
    // the image, and the ground is instead painted here using the height the panel had LAST
    // frame, which is identical unless the contents changed. First frame draws no ground,
    // which nobody sees at 250 fps.
    const int h = LastHeight(HashId(panel_title_.c_str(), 0x9001));
    if (h > 0) {
      img_->FillRect(x, y, w, h, th_.ground);
      DrawEdge(Rect{x, y, w, h}, th_.edge);
      if (!panel_title_.empty()) {
        img_->FillRect(x + 1, y + 1, w - 2, kTitleH - 1, th_.ground_alt);
        font::DrawString(img_, x + kPad, y + 4, panel_title_.c_str(), th_.title);
      }
    }
  }

  Rect EndPanel() {
    panel_.h = cursor_y_ - panel_.y + kPad;
    RememberHeight(HashId(panel_title_.c_str(), 0x9001), panel_.h);
    return panel_;
  }

  int cursor_y() const { return cursor_y_; }
  void Gap(int px) { cursor_y_ += px; }

  void Separator() {
    img_->FillRect(panel_.x + kPad, cursor_y_ + 2, panel_.w - 2 * kPad, 1, th_.edge);
    cursor_y_ += 6;
  }

  // ------------------------------------------------------------------ text

  void Label(Rgb colour, const char* fmt, ...) __attribute__((format(gnu_printf, 3, 4))) {
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    font::DrawString(img_, panel_.x + kPad, cursor_y_ + 3, buf, colour);
    cursor_y_ += kRowH;
  }

  void Text(const char* fmt, ...) __attribute__((format(gnu_printf, 2, 3))) {
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    font::DrawString(img_, panel_.x + kPad, cursor_y_ + 3, buf, th_.text);
    cursor_y_ += kRowH;
  }

  void Dim(const char* fmt, ...) __attribute__((format(gnu_printf, 2, 3))) {
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    font::DrawString(img_, panel_.x + kPad, cursor_y_ + 3, buf, th_.text_dim);
    cursor_y_ += kRowH;
  }

  // Wraps on spaces at the panel width. Used for the one-line explanations that make a
  // control self-describing, which is the whole reason a person can use this without the
  // README open.
  void Wrapped(Rgb colour, const std::string& text) {
    const int cols = (panel_.w - 2 * kPad) / font::kAdvance;
    if (cols <= 4) return;
    size_t at = 0;
    while (at < text.size()) {
      size_t take = text.size() - at;
      if (take > static_cast<size_t>(cols)) {
        take = static_cast<size_t>(cols);
        size_t brk = text.rfind(' ', at + take);
        if (brk != std::string::npos && brk > at) take = brk - at;
      }
      font::DrawString(img_, panel_.x + kPad, cursor_y_ + 3,
                       text.substr(at, take).c_str(), colour);
      cursor_y_ += font::kLineH;
      at += take;
      while (at < text.size() && text[at] == ' ') ++at;
    }
    cursor_y_ += 4;
  }

  // ------------------------------------------------------------------ controls

  bool Button(const char* label, bool enabled = true) {
    const Rect r = Row(font::MeasureWidth(label) + 2 * kPad + 4);
    const bool hot = enabled && r.Contains(in_.mouse_x, in_.mouse_y);
    img_->FillRect(r.x, r.y, r.w, r.h, enabled ? (hot ? th_.hot : th_.ground_alt) : th_.ground);
    DrawEdge(r, enabled ? th_.edge : th_.accent_dim);
    font::DrawString(img_, r.x + kPad + 2, r.y + 3, label,
                     enabled ? th_.text : th_.text_dim);
    return enabled && TakeClick(r);
  }

  // Several buttons on one row, so an actions panel is not a column a screen tall.
  // Returns the index pressed, or -1.
  int ButtonRow(const char* const* labels, int n, bool enabled = true) {
    int hit = -1;
    int x = panel_.x + kPad;
    const int y = cursor_y_;
    for (int i = 0; i < n; ++i) {
      const int w = font::MeasureWidth(labels[i]) + 2 * kPad + 4;
      const Rect r{x, y, w, kRowH};
      const bool hot = enabled && r.Contains(in_.mouse_x, in_.mouse_y);
      img_->FillRect(r.x, r.y, r.w, r.h,
                     enabled ? (hot ? th_.hot : th_.ground_alt) : th_.ground);
      DrawEdge(r, enabled ? th_.edge : th_.accent_dim);
      font::DrawString(img_, r.x + kPad + 2, r.y + 3, labels[i],
                       enabled ? th_.text : th_.text_dim);
      if (enabled && TakeClick(r)) hit = i;
      x += w + 4;
    }
    cursor_y_ += kRowH + kRowGap;
    return hit;
  }

  bool Checkbox(const char* label, bool* value) {
    const Rect r = Row(panel_.w - 2 * kPad);
    const Rect box{r.x, r.y + (kRowH - kBoxSize) / 2, kBoxSize, kBoxSize};
    const bool hot = r.Contains(in_.mouse_x, in_.mouse_y);
    img_->FillRect(box.x, box.y, box.w, box.h, *value ? th_.accent : th_.field);
    DrawEdge(box, hot ? th_.text : th_.edge);
    if (*value) {
      // A tick drawn as two strokes rather than a glyph, because the font's 'x' does not
      // centre in an 9x9 box and a mis-centred tick reads as a smudge.
      for (int i = 0; i < 3; ++i) img_->Set(box.x + 2 + i, box.y + 4 + i, th_.ground);
      for (int i = 0; i < 4; ++i) img_->Set(box.x + 5 + i, box.y + 6 - i, th_.ground);
    }
    font::DrawString(img_, box.x + box.w + 5, r.y + 3, label, th_.text);
    if (TakeClick(r)) {
      *value = !*value;
      return true;
    }
    return false;
  }

  // A row of mutually exclusive choices. Returns true when the selection changed.
  bool Radio(const char* const* labels, int n, int* selected) {
    bool changed = false;
    int x = panel_.x + kPad;
    const int y = cursor_y_;
    for (int i = 0; i < n; ++i) {
      const int w = font::MeasureWidth(labels[i]) + kBoxSize + 9;
      const Rect r{x, y, w, kRowH};
      const Rect box{x, y + (kRowH - kBoxSize) / 2, kBoxSize, kBoxSize};
      const bool hot = r.Contains(in_.mouse_x, in_.mouse_y);
      img_->FillRect(box.x, box.y, box.w, box.h, th_.field);
      DrawEdge(box, hot ? th_.text : th_.edge);
      if (*selected == i)
        img_->FillRect(box.x + 2, box.y + 2, box.w - 4, box.h - 4, th_.accent);
      font::DrawString(img_, box.x + box.w + 5, y + 3, labels[i], th_.text);
      if (TakeClick(r) && *selected != i) {
        *selected = i;
        changed = true;
      }
      x += w + 4;
    }
    cursor_y_ += kRowH + kRowGap;
    return changed;
  }

  // A text field. `label` is drawn to the left; the box takes the rest of the row.
  // Returns true on COMMIT (Enter, or focus lost), never on every keystroke -- see the
  // header. `hint` is drawn dimmed when the value is empty and the field is not focused.
  bool Field(const char* label, std::string* value, const char* hint = "") {
    const uint64_t id = PtrId(value);
    const Rect row = Row(panel_.w - 2 * kPad);
    const int label_w = label && *label ? font::MeasureWidth(label) + 6 : 0;
    if (label_w) font::DrawString(img_, row.x, row.y + 3, label, th_.text_dim);
    const Rect box{row.x + label_w, row.y, row.w - label_w, kRowH};

    const bool focused = st_->focus == id;
    if (TakeClick(box)) Focus(id, *value);

    img_->FillRect(box.x, box.y, box.w, box.h, th_.field);
    DrawEdge(box, focused ? th_.accent : th_.edge);

    bool committed = false;
    if (focused) {
      committed = EditText(value, id);
      DrawEditable(box, st_->edit, st_->caret);
    } else if (value->empty()) {
      font::DrawString(img_, box.x + 3, box.y + 3, hint, th_.text_dim);
    } else {
      DrawClipped(box, *value, th_.text);
    }
    return committed;
  }

  // Integer field. The buffer is text while being typed; the int changes on commit only,
  // and text that is not a number leaves it alone rather than becoming zero.
  bool IntField(const char* label, int* value, int lo, int hi) {
    char shown[32];
    snprintf(shown, sizeof(shown), "%d", *value);
    std::string& buf = Buffer(PtrId(value), shown);
    if (Field(label, &buf, "")) {
      char* end = nullptr;
      const long v = strtol(buf.c_str(), &end, 10);
      if (end != buf.c_str() && *end == '\0') {
        *value = static_cast<int>(v < lo ? lo : (v > hi ? hi : v));
      }
      snprintf(shown, sizeof(shown), "%d", *value);
      buf = shown;
      return true;
    }
    if (st_->focus != PtrId(&buf)) buf = shown;
    return false;
  }

  bool DoubleField(const char* label, double* value, double lo, double hi) {
    char shown[48];
    snprintf(shown, sizeof(shown), "%.6g", *value);
    std::string& buf = Buffer(PtrId(value), shown);
    if (Field(label, &buf, "")) {
      char* end = nullptr;
      const double v = strtod(buf.c_str(), &end);
      if (end != buf.c_str() && *end == '\0') {
        *value = v < lo ? lo : (v > hi ? hi : v);
      }
      snprintf(shown, sizeof(shown), "%.6g", *value);
      buf = shown;
      return true;
    }
    if (st_->focus != PtrId(&buf)) buf = shown;
    return false;
  }

  // A closed list that opens over what follows. Returns true when the selection changed.
  bool Dropdown(const char* label, const std::vector<std::string>& options, int* selected) {
    const uint64_t id = PtrId(selected);
    const Rect row = Row(panel_.w - 2 * kPad);
    const int label_w = label && *label ? font::MeasureWidth(label) + 6 : 0;
    if (label_w) font::DrawString(img_, row.x, row.y + 3, label, th_.text_dim);
    const Rect box{row.x + label_w, row.y, row.w - label_w, kRowH};

    const bool open = st_->open_dropdown == id;
    const bool hot = box.Contains(in_.mouse_x, in_.mouse_y);
    img_->FillRect(box.x, box.y, box.w, box.h, hot ? th_.hot : th_.field);
    DrawEdge(box, open ? th_.accent : th_.edge);
    const std::string shown =
        (*selected >= 0 && *selected < static_cast<int>(options.size()))
            ? options[static_cast<size_t>(*selected)]
            : std::string("(none)");
    DrawClipped(box, shown, th_.text);
    // The arrow, three rows of a triangle. Drawn rather than lettered for the same reason
    // the tick is.
    const int ax = box.x + box.w - 10, ay = box.y + 5;
    for (int i = 0; i < 4; ++i)
      for (int j = -i; j <= i; ++j) img_->Set(ax + 3 + j, ay + i, th_.text_dim);

    bool changed = false;
    if (open) {
      const int n = static_cast<int>(options.size());
      const Rect list{box.x, box.y + kRowH, box.w, n * kRowH + 2};
      // Hit-tested HERE, drawn at Finish. The header explains why that is not a race.
      if (in_.clicked) {
        if (list.Contains(in_.click_x, in_.click_y)) {
          const int idx = (in_.click_y - list.y - 1) / kRowH;
          if (idx >= 0 && idx < n && idx != *selected) {
            *selected = idx;
            changed = true;
          }
          st_->open_dropdown = 0;
          click_avail_ = false;
        } else if (!box.Contains(in_.click_x, in_.click_y)) {
          st_->open_dropdown = 0;
        }
      }
      Deferred d;
      d.rect = list;
      d.selected = *selected;
      for (const auto& o : options) d.lines.push_back(o);
      deferred_.push_back(d);
    }
    if (TakeClick(box)) st_->open_dropdown = open ? 0 : id;
    return changed;
  }

  bool Slider(const char* label, double* value, double lo, double hi) {
    const Rect row = Row(panel_.w - 2 * kPad);
    font::DrawString(img_, row.x, row.y + 3, label, th_.text_dim);
    const int label_w = font::MeasureWidth(label) + 6;
    const Rect bar{row.x + label_w, row.y + 4, row.w - label_w - 60, kRowH - 8};
    img_->FillRect(bar.x, bar.y, bar.w, bar.h, th_.field);
    DrawEdge(bar, th_.edge);
    const double span = hi > lo ? hi - lo : 1.0;
    double t = (*value - lo) / span;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    img_->FillRect(bar.x + 1, bar.y + 1, static_cast<int>(t * (bar.w - 2)), bar.h - 2,
                   th_.accent_dim);
    char shown[32];
    snprintf(shown, sizeof(shown), "%.4g", *value);
    font::DrawString(img_, bar.x + bar.w + 6, row.y + 3, shown, th_.text);

    // A slider is the one widget that wants the button HELD, not the click edge: dragging
    // is the whole interaction.
    const Rect grab{bar.x, row.y, bar.w, kRowH};
    if (in_.mouse_down && grab.Contains(in_.mouse_x, in_.mouse_y)) {
      double nt = static_cast<double>(in_.mouse_x - bar.x) / (bar.w > 1 ? bar.w - 1 : 1);
      nt = nt < 0 ? 0 : (nt > 1 ? 1 : nt);
      const double nv = lo + nt * span;
      if (nv != *value) {
        *value = nv;
        return true;
      }
    }
    return false;
  }

  // Tabs across the top of a panel. Returns true when the tab changed.
  bool Tabs(const char* const* labels, int n, int* selected) {
    bool changed = false;
    int x = panel_.x + kPad;
    const int y = cursor_y_;
    for (int i = 0; i < n; ++i) {
      const int w = font::MeasureWidth(labels[i]) + 2 * kPad + 2;
      const Rect r{x, y, w, kRowH};
      const bool on = *selected == i;
      const bool hot = r.Contains(in_.mouse_x, in_.mouse_y);
      img_->FillRect(r.x, r.y, r.w, r.h, on ? th_.accent_dim : (hot ? th_.hot : th_.ground_alt));
      DrawEdge(r, on ? th_.accent : th_.edge);
      font::DrawString(img_, r.x + kPad, r.y + 3, labels[i], on ? th_.text : th_.text_dim);
      if (TakeClick(r) && !on) {
        *selected = i;
        changed = true;
      }
      x += w + 2;
    }
    cursor_y_ += kRowH + kRowGap;
    return changed;
  }

  // ------------------------------------------------------------------ panes

  // A scrolling read-only text pane, which is where every feature that used to printf puts
  // its output. `lines` is not copied. Follows the tail while the scroll is at the bottom,
  // and stops following the moment the reader scrolls up -- a pane that yanks itself back to
  // the end while somebody is reading is worse than no pane.
  void TextPane(const char* id_label, const std::deque<std::string>& lines, int height,
                size_t dropped = 0) {
    const uint64_t id = HashId(id_label, 0x5EED);
    const Rect r{panel_.x + kPad, cursor_y_, panel_.w - 2 * kPad, height};
    img_->FillRect(r.x, r.y, r.w, r.h, th_.field);
    DrawEdge(r, th_.edge);

    const int rows = (r.h - 4) / font::kLineH;
    const int total = static_cast<int>(lines.size());
    int& first = st_->Scroll(id);
    const int max_first = total > rows ? total - rows : 0;
    if (first > max_first) first = max_first;   // also lands a fresh pane on the tail
    const bool at_tail = first >= max_first;

    if (r.Contains(in_.mouse_x, in_.mouse_y) && in_.wheel != 0) {
      first -= in_.wheel * 3;
    } else if (at_tail) {
      first = max_first;   // follow the tail, but only while already there
    }
    if (first > max_first) first = max_first;
    if (first < 0) first = 0;

    int y = r.y + 2;
    if (dropped > 0 && first == 0) {
      char note[96];
      snprintf(note, sizeof(note), "... %zu earlier lines dropped", dropped);
      font::DrawString(img_, r.x + 3, y, note, th_.text_dim);
      y += font::kLineH;
    }
    const int cols = (r.w - 6 - kScrollW) / font::kAdvance;
    for (int i = first; i < total && y + font::kGlyphH <= r.y + r.h - 2; ++i) {
      const std::string& s = lines[static_cast<size_t>(i)];
      font::DrawString(img_, r.x + 3, y,
                       (static_cast<int>(s.size()) > cols ? s.substr(0, static_cast<size_t>(cols))
                                                          : s).c_str(),
                       th_.text);
      y += font::kLineH;
    }

    // The scrollbar is a position readout, not a grab handle: the wheel scrolls, and a
    // draggable 6-pixel bar over an 8-pixel row would be a target nobody can hit.
    if (total > rows && rows > 0) {
      const int track_h = r.h - 4;
      int thumb = track_h * rows / total;
      if (thumb < 6) thumb = 6;
      const int at = max_first > 0 ? (track_h - thumb) * first / max_first : 0;
      img_->FillRect(r.x + r.w - kScrollW - 1, r.y + 2, kScrollW, track_h, th_.ground_alt);
      img_->FillRect(r.x + r.w - kScrollW - 1, r.y + 2 + at, kScrollW, thumb, th_.accent_dim);
    }
    cursor_y_ += height + kRowGap;
  }

  // ------------------------------------------------------------------ misc

  // Hover help. Registered here, painted at Finish so it is over everything.
  void Tooltip(const Rect& over, const std::string& text) {
    if (!over.Contains(in_.mouse_x, in_.mouse_y)) return;
    Deferred d;
    d.is_tooltip = true;
    d.lines.push_back(text);
    d.rect = Rect{in_.mouse_x + 12, in_.mouse_y + 14,
                  font::MeasureWidth(text.c_str()) + 8, font::kGlyphH + 6};
    deferred_.push_back(d);
  }

  // The row the last widget occupied, for attaching a tooltip to it.
  Rect last_row() const { return last_row_; }

  // True when a click this frame landed on no widget -- the caller uses it to drop focus
  // and to close a dropdown, which is what clicking the world is expected to do.
  bool click_unused() const { return click_avail_; }

  void ClearFocus() {
    st_->focus = 0;
    st_->edit.clear();
    st_->caret = 0;
  }

  // Paints dropdown lists and tooltips over everything drawn this frame.
  void Finish() {
    for (const Deferred& d : deferred_) {
      img_->FillRect(d.rect.x, d.rect.y, d.rect.w, d.rect.h,
                     d.is_tooltip ? th_.ground_alt : th_.field);
      DrawEdge(d.rect, d.is_tooltip ? th_.accent : th_.accent);
      int y = d.rect.y + (d.is_tooltip ? 3 : 1);
      for (size_t i = 0; i < d.lines.size(); ++i) {
        const bool sel = !d.is_tooltip && static_cast<int>(i) == d.selected;
        const Rect line{d.rect.x + 1, y, d.rect.w - 2, d.is_tooltip ? font::kGlyphH + 2 : kRowH};
        if (sel) img_->FillRect(line.x, line.y, line.w, line.h, th_.accent_dim);
        else if (!d.is_tooltip && line.Contains(in_.mouse_x, in_.mouse_y))
          img_->FillRect(line.x, line.y, line.w, line.h, th_.hot);
        font::DrawString(img_, d.rect.x + 4, y + (d.is_tooltip ? 0 : 3), d.lines[i].c_str(),
                         th_.text);
        y += d.is_tooltip ? font::kLineH : kRowH;
      }
    }
    deferred_.clear();
  }

 private:
  Rect Row(int w) {
    const Rect r{panel_.x + kPad, cursor_y_, w, kRowH};
    cursor_y_ += kRowH + kRowGap;
    last_row_ = r;
    return r;
  }

  void DrawEdge(const Rect& r, Rgb c) {
    img_->FillRect(r.x, r.y, r.w, 1, c);
    img_->FillRect(r.x, r.y + r.h - 1, r.w, 1, c);
    img_->FillRect(r.x, r.y, 1, r.h, c);
    img_->FillRect(r.x + r.w - 1, r.y, 1, r.h, c);
  }

  // Takes the frame's click if it landed in `r`. One click can only ever activate one
  // widget, which is what stops a dropdown item and the row beneath it both firing.
  bool TakeClick(const Rect& r) {
    if (!click_avail_) return false;
    if (!r.Contains(in_.click_x, in_.click_y)) return false;
    click_avail_ = false;
    return true;
  }

  void Focus(uint64_t id, const std::string& current) {
    st_->focus = id;
    st_->edit = current;
    st_->caret = static_cast<int>(current.size());
    st_->open_dropdown = 0;
  }

  // Applies this frame's typed characters and editing keys to the buffer. Returns true on
  // Enter, which is a commit; Escape abandons and returns false.
  bool EditText(std::string* value, uint64_t id) {
    (void)id;
    for (char c : in_.typed) {
      if (c == '\r') {
        *value = st_->edit;
        ClearFocus();
        return true;
      }
      if (c == 0x1B) {          // escape: the edit never happened
        ClearFocus();
        return false;
      }
      if (c == '\t') continue;  // tab order is not modelled; ignoring beats surprising
      if (c == 0x08) {
        if (st_->caret > 0) {
          st_->edit.erase(static_cast<size_t>(st_->caret - 1), 1);
          --st_->caret;
        }
        continue;
      }
      st_->edit.insert(static_cast<size_t>(st_->caret), 1, c);
      ++st_->caret;
    }
    for (int i = 0; i < in_.key_left; ++i) if (st_->caret > 0) --st_->caret;
    for (int i = 0; i < in_.key_right; ++i)
      if (st_->caret < static_cast<int>(st_->edit.size())) ++st_->caret;
    if (in_.key_home) st_->caret = 0;
    if (in_.key_end) st_->caret = static_cast<int>(st_->edit.size());
    for (int i = 0; i < in_.key_delete; ++i)
      if (st_->caret < static_cast<int>(st_->edit.size()))
        st_->edit.erase(static_cast<size_t>(st_->caret), 1);
    return false;
  }

  // Draws the buffer with the caret, scrolled so the caret is always visible -- a field
  // whose caret has run off the right edge is a field you cannot see what you are typing in.
  void DrawEditable(const Rect& box, const std::string& text, int caret) {
    const int cols = (box.w - 6) / font::kAdvance;
    int first = 0;
    if (cols > 0 && caret > cols - 1) first = caret - (cols - 1);
    const std::string vis = text.substr(static_cast<size_t>(first),
                                        static_cast<size_t>(cols > 0 ? cols : 0));
    font::DrawString(img_, box.x + 3, box.y + 3, vis.c_str(), th_.text);
    const int cx = box.x + 3 + (caret - first) * font::kAdvance;
    img_->FillRect(cx, box.y + 2, 1, font::kGlyphH + 2, th_.caret);
  }

  void DrawClipped(const Rect& box, const std::string& text, Rgb colour) {
    const int cols = (box.w - 6) / font::kAdvance;
    if (cols <= 0) return;
    if (static_cast<int>(text.size()) <= cols) {
      font::DrawString(img_, box.x + 3, box.y + 3, text.c_str(), colour);
      return;
    }
    // Keep the TAIL of an over-long value, not the head. These fields hold paths, and the
    // end of a path is the part that identifies the file.
    std::string vis = text.substr(text.size() - static_cast<size_t>(cols));
    if (vis.size() > 1) { vis[0] = '<'; }
    font::DrawString(img_, box.x + 3, box.y + 3, vis.c_str(), colour);
  }

  // Per-id scratch strings for the number fields, which need a buffer that outlives the
  // frame but does not belong to the caller's int.
  std::string& Buffer(uint64_t id, const char* initial) {
    for (auto& kv : buffers_)
      if (kv.first == id) return kv.second;
    buffers_.push_back({id, std::string(initial)});
    return buffers_.back().second;
  }

  int LastHeight(uint64_t id) const {
    for (const auto& kv : heights_)
      if (kv.first == id) return kv.second;
    return 0;
  }

  void RememberHeight(uint64_t id, int h) {
    for (auto& kv : heights_)
      if (kv.first == id) { kv.second = h; return; }
    heights_.push_back({id, h});
  }

  Image* img_;
  State* st_;
  Input in_;
  Theme th_;
  Rect panel_;
  Rect last_row_;
  std::string panel_title_;
  int cursor_y_ = 0;
  bool click_avail_ = false;
  std::vector<Deferred> deferred_;

  // Static so panel heights and field buffers survive the Ctx being rebuilt every frame,
  // which is the one piece of immediate-mode bookkeeping that genuinely has to persist and
  // does not belong to the caller. Keyed by id, so two windows cannot collide.
  // A deque for the same reason State::scrolls is one, and here it is not a nicety: a
  // number field's IDENTITY is the address of its buffer, so a reallocation would change
  // the id of every field on screen and drop focus in the middle of a word.
  static std::deque<std::pair<uint64_t, std::string>>& buffers_ref() {
    static std::deque<std::pair<uint64_t, std::string>> v;
    return v;
  }
  static std::vector<std::pair<uint64_t, int>>& heights_ref() {
    static std::vector<std::pair<uint64_t, int>> v;
    return v;
  }
  std::deque<std::pair<uint64_t, std::string>>& buffers_ = buffers_ref();
  std::vector<std::pair<uint64_t, int>>& heights_ = heights_ref();
};

}  // namespace ui
}  // namespace simviz
