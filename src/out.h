// The output sink: one place every stdout-bound line in this program passes through.
//
// WHY THIS EXISTS. Nearly every feature here reports by printing -- `--probe` prints cell
// state, `--stats` prints quantiles, `--ext-list` prints the registry, `--bisect` prints the
// tick two runs parted, and every test prints a verdict. That is exactly right for a program
// driven from a shell, and it is nothing at all for one driven from its own window: a person
// who double-clicks the exe has no console to read, and a feature whose entire output goes
// somewhere they cannot see is not a feature they have.
//
// So `printf` becomes `out::Printf`, which writes to stdout exactly as before AND, when a
// capture is open, into a bounded line buffer the window can draw. Nothing about the headless
// behaviour changes: with no capture open this is a `vprintf` and one branch.
//
// WHAT IS DELIBERATELY NOT ROUTED THROUGH HERE:
//
//   * `fprintf(stderr, ...)` for a bad flag. Those are the command line's own errors, they
//     happen before any window exists, and a shell that wants to see them on stderr should
//     keep seeing them on stderr.
//   * `fprintf(f, ...)` into a file the command line asked for -- digestlog.h writes a real artifact.
//     Capturing that would put a log's contents in a pane and leave the file empty.
//
// LINE-ORIENTED, because the consumer is a scrolling text pane rather than a terminal. A
// `printf` with no trailing newline leaves a partial line held here until the newline that
// finishes it arrives, so a progress line built out of three calls becomes one line in the
// pane rather than three fragments -- and `Flush` closes whatever is still open at the end of
// a run so the last line is never swallowed.
//
// The ring is bounded. A `--load-stress 5000` prints tens of thousands of lines and a window
// that held all of them would be a leak with a scrollbar; the oldest go, the count of dropped
// lines is kept, and a pane that says "12,431 earlier lines dropped" is honest in the one way
// a silently truncated buffer is not.

#pragma once

#include <cstdarg>
#include <cstdio>
#include <deque>
#include <string>

namespace simviz {
namespace out {

// How many finished lines the capture holds before the oldest are dropped. 20,000 lines of
// typical width is a couple of megabytes -- irrelevant beside a 12.7 MB extension blob, and
// far more than a person will scroll through.
constexpr size_t kMaxCapturedLines = 20000;

struct Capture {
  std::deque<std::string> lines;
  std::string partial;      // the line built so far, still waiting for its newline
  size_t dropped = 0;       // lines pushed out of the front, never silently forgotten
  bool echo = true;         // also write to stdout; false silences the console half
};

// The single active capture, or null. A pointer rather than a flag because the window owns
// the buffer and this file should not decide how long it lives.
inline Capture*& Sink() {
  static Capture* sink = nullptr;
  return sink;
}

inline void SetSink(Capture* c) { Sink() = c; }

inline void PushLine(Capture* c, std::string line) {
  c->lines.push_back(std::move(line));
  while (c->lines.size() > kMaxCapturedLines) {
    c->lines.pop_front();
    ++c->dropped;
  }
}

// Splits `text` on newlines into the capture, holding an unterminated tail in `partial`.
inline void Absorb(Capture* c, const char* text) {
  for (const char* p = text; *p != '\0'; ++p) {
    if (*p == '\n') {
      PushLine(c, c->partial);
      c->partial.clear();
    } else if (*p != '\r') {
      c->partial.push_back(*p);
    }
  }
}

// The whole point of the file. Same signature and same meaning as `printf`.
//
// THE FORMAT ATTRIBUTE IS NOT DECORATION. -Wall -Wextra checks printf formats, and a wrapper
// without it silently turns every one of the ~280 converted call sites into an unchecked one.
//
// AND THE ARCHETYPE MUST BE `gnu_printf`, NOT `printf`, which is not a detail: on mingw-w64
// the `printf` archetype means the MSVCRT conversions, where `%zu` does not exist, while the
// C++ build actually links mingw's ANSI stdio, where it does. Getting this wrong does not
// break the build -- it produces a warning on every correct `%zu` call site, which is how it
// was found: the first build after the conversion reported two, on code that had compiled
// clean for weeks because GCC was checking the builtin against the right archetype all along.
#if defined(__MINGW32__) || defined(__MINGW64__)
inline void Printf(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
#else
inline void Printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#endif

inline void Printf(const char* fmt, ...) {
  Capture* c = Sink();
  if (c == nullptr) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    return;
  }

  // Format once into a buffer sized by the formatter itself, so a long line -- a 700-element
  // table row, a probe of a wide rectangle -- is never truncated to whatever a fixed array
  // guessed. The first vsnprintf is measurement only; ap must be restarted for the second.
  va_list ap;
  va_start(ap, fmt);
  va_list measure;
  va_copy(measure, ap);
  const int n = vsnprintf(nullptr, 0, fmt, measure);
  va_end(measure);
  if (n < 0) {
    va_end(ap);
    return;
  }
  std::string text(static_cast<size_t>(n) + 1, '\0');
  vsnprintf(&text[0], text.size(), fmt, ap);
  va_end(ap);
  text.resize(static_cast<size_t>(n));

  if (c->echo) {
    fputs(text.c_str(), stdout);
  }
  Absorb(c, text.c_str());
}

// Closes an unterminated last line. Called at the end of a captured run: without it, a
// feature whose final `printf` has no newline loses that line entirely, which is the one
// place a person would look for the answer.
inline void Flush() {
  Capture* c = Sink();
  if (c == nullptr) {
    fflush(stdout);
    return;
  }
  if (!c->partial.empty()) {
    PushLine(c, c->partial);
    c->partial.clear();
  }
  if (c->echo) fflush(stdout);
}

}  // namespace out
}  // namespace simviz
