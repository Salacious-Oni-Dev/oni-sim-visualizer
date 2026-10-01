// A per-tick digest log, and the bisector that reads two of them.
//
// `--digest` answers "do these two states differ" for ONE state. That is the wrong shape
// for the question a divergence actually poses, which is "at which tick did they stop
// agreeing, and in what". Answering that by eye means running both sides to a guessed tick,
// digesting, guessing again -- and the guess is usually wrong in the expensive direction,
// because a divergence that is one cell wide at tick 137 is a continent by tick 500 and
// looks the same as one that started at tick 300.
//
// So a run can write every tick's digest to a file, and two files can be compared with no
// simulation running at all. That last part is the point: the two sides need not be, and
// usually are not, the same process, the same DLL, or even the same machine. Anything that
// can produce a log can be bisected against anything else that can -- two builds of
// oni-sim-replacement, two seeds, two flag sets, a DLL against Klei's.
//
// The log is TEXT for the same reason `--dump-elements` is: it is a debugging artefact,
// it is read by people at least as often as by this program, and `diff` and `grep` are
// already better at half of what a reader would want than anything written here. The cost
// is 16 hex columns per tick, which is nothing next to the frame they summarise.
//
// WHAT THIS CANNOT TELL YOU. A digest says two runs differ; it never says where in the
// world, and it cannot: an FNV hash of 257,000 cells has no geometry. That is what
// `--dump-state` and `--diff-state` are for, and why the bisect report ends by printing
// the command that takes the answer to the next step.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace simviz {
namespace digestlog {

// The thirteen hashed components, in the order `DigestDelta` names them. One list, because
// a column added here and forgotten in the writer is a divergence this tool would report
// as agreement -- the exact failure the digest itself was widened twice to avoid.
inline const char* const kColumns[] = {
    "elementIdx", "mass",     "temperature", "radiation", "disease",
    "backwall",   "mixture",  "building",    "registry",  "extCells",
    "flowTex",    "liquidTex", "sunlightTex",
};
constexpr int kColumnCount = 13;
static_assert(sizeof(kColumns) / sizeof(kColumns[0]) == kColumnCount,
              "kColumns and kColumnCount disagree");

// One tick's line. The two doubles are not hashed and are not part of the divergence test:
// they are context for the report, because "the mass totals differ by 4e-4 kg at the tick
// the hashes part" and "they are identical to the last digit" point at different bugs.
struct Row {
  int tick = 0;
  uint64_t h[kColumnCount] = {};
  double total_mass = 0.0;
  double mixture_mass = 0.0;
};

// A whole log, header included. The header fields exist so the bisector can REFUSE a
// comparison that cannot mean anything (two different worlds) and WARN about one that
// probably does not (a wall-clock-seeded pair, which diverges by design).
struct Log {
  std::string path;
  std::string source;    // the DLL that produced it, or the endpoint it was polled from
  std::string corpus;
  std::string seed;      // "0x00000007 pinned", "wall-clock", or "unpinned"
  int width = 0, height = 0, cells = 0;
  double dt = 0.0;
  std::vector<Row> rows;
};

inline void WriteHeader(FILE* f, const char* source, const char* corpus, const char* seed,
                        int width, int height, int cells, double dt) {
  fprintf(f, "# oni-sim-visualizer digest log v1\n");
  fprintf(f, "# source %s\n", source ? source : "(unknown)");
  fprintf(f, "# corpus %s\n", corpus ? corpus : "(none)");
  fprintf(f, "# seed %s\n", seed ? seed : "unpinned");
  fprintf(f, "# world %d %d %d\n", width, height, cells);
  fprintf(f, "# dt %.9g\n", dt);
  fprintf(f, "# columns tick");
  for (int i = 0; i < kColumnCount; ++i) fprintf(f, " %s", kColumns[i]);
  fprintf(f, " totalMass mixtureMass\n");
  fflush(f);
}

// One line, flushed. Flushed because a run being bisected is exactly the kind of run that
// crashes at tick 412, and a log that lost its tail to a buffer would hide the one tick
// worth having.
inline void WriteRow(FILE* f, const Row& r) {
  fprintf(f, "%d", r.tick);
  for (int i = 0; i < kColumnCount; ++i) {
    fprintf(f, " %016llx", static_cast<unsigned long long>(r.h[i]));
  }
  fprintf(f, " %.17g %.17g\n", r.total_mass, r.mixture_mass);
  fflush(f);
}

inline bool ParseHeaderLine(const std::string& line, Log* out) {
  // "# key rest-of-line". An unrecognised key is not an error: a log written by a later
  // build may carry fields this one does not know, and refusing to read it for that would
  // make the format un-extendable for no gain.
  const char* p = line.c_str() + 1;
  while (*p == ' ') ++p;
  auto starts = [&p](const char* key, const char** rest) {
    const size_t n = strlen(key);
    if (strncmp(p, key, n) != 0 || (p[n] != ' ' && p[n] != '\0')) return false;
    *rest = p + n;
    while (**rest == ' ') ++*rest;
    return true;
  };
  const char* rest = nullptr;
  if (starts("source", &rest)) out->source = rest;
  else if (starts("corpus", &rest)) out->corpus = rest;
  else if (starts("seed", &rest)) out->seed = rest;
  else if (starts("world", &rest)) {
    sscanf(rest, "%d %d %d", &out->width, &out->height, &out->cells);
  } else if (starts("dt", &rest)) {
    out->dt = atof(rest);
  }
  return true;
}

inline bool Read(const char* path, Log* out, std::string* err) {
  *out = Log();
  out->path = path;
  FILE* f = fopen(path, "rb");
  if (f == nullptr) {
    *err = std::string("cannot open ") + path;
    return false;
  }
  std::string line;
  int c = 0;
  bool saw_magic = false;
  auto flush_line = [&]() -> bool {
    if (line.empty()) return true;
    if (line[0] == '#') {
      if (!saw_magic) {
        if (line.find("oni-sim-visualizer digest log") == std::string::npos) {
          *err = std::string(out->path) + " is not an oni-sim-visualizer digest log (first line is "
                                          "\"" + line + "\")";
          return false;
        }
        saw_magic = true;
        return true;
      }
      ParseHeaderLine(line, out);
      return true;
    }
    if (!saw_magic) {
      *err = std::string(out->path) + " has no digest-log header";
      return false;
    }
    Row r;
    const char* p = line.c_str();
    char* end = nullptr;
    r.tick = static_cast<int>(strtol(p, &end, 10));
    if (end == p) {
      *err = out->path + ": line does not begin with a tick number: " + line;
      return false;
    }
    p = end;
    for (int i = 0; i < kColumnCount; ++i) {
      r.h[i] = strtoull(p, &end, 16);
      if (end == p) {
        *err = out->path + ": tick " + std::to_string(r.tick) + " has fewer than " +
               std::to_string(kColumnCount) + " hash columns";
        return false;
      }
      p = end;
    }
    r.total_mass = strtod(p, &end);
    p = end;
    r.mixture_mass = strtod(p, &end);
    out->rows.push_back(r);
    return true;
  };
  bool ok = true;
  while ((c = fgetc(f)) != EOF) {
    if (c == '\r') continue;
    if (c != '\n') {
      line.push_back(static_cast<char>(c));
      continue;
    }
    ok = flush_line();
    line.clear();
    if (!ok) break;
  }
  if (ok) ok = flush_line();
  fclose(f);
  if (!ok) return false;
  if (out->rows.empty()) {
    *err = out->path + " has a header but no ticks";
    return false;
  }
  return true;
}

// What one column did over the overlap. `heals` is the field that keeps this honest: a
// bisect assumes divergence is monotone -- once two runs part they stay parted -- and that
// assumption is FALSE in a simulation that clamps. A cell driven past a phase boundary in
// one run and not the other can rejoin it a few ticks later, and a report that said only
// "first differs at 137" would be describing a transient as an origin.
struct ColumnVerdict {
  const char* name = "";
  bool differs = false;
  int first = 0;         // tick of the first difference
  int last = 0;          // tick of the last difference
  int ticks_differing = 0;
  bool heals = false;    // agreed again at some tick after `first`
  bool persists = false; // differs at the last tick of the overlap
};

struct Verdict {
  bool comparable = false;
  std::string refusal;
  std::vector<std::string> warnings;
  int overlap = 0;           // ticks compared
  int first_tick = 0, last_tick = 0;
  bool any = false;          // did anything differ at all
  int divergence_tick = 0;   // the earliest tick any column differs at
  std::vector<ColumnVerdict> columns;
  // The mass totals at the divergence, for the report. A divergence with identical totals
  // is matter moving; one with different totals is matter appearing.
  double mass_a = 0.0, mass_b = 0.0;
};

inline Verdict Compare(const Log& a, const Log& b) {
  Verdict v;
  if (a.cells != b.cells || a.width != b.width || a.height != b.height) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%dx%d (%d cells) against %dx%d (%d cells)", a.width,
             a.height, a.cells, b.width, b.height, b.cells);
    v.refusal = std::string("these logs are of different worlds: ") + buf;
    return v;
  }
  v.comparable = true;
  if (a.corpus != b.corpus) {
    v.warnings.push_back("the two runs used different corpora (" + a.corpus + " and " +
                         b.corpus + "), so a divergence may be the world, not the sim");
  }
  if (a.dt != b.dt) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%.9g and %.9g", a.dt, b.dt);
    v.warnings.push_back(std::string("the two runs used different dt (") + buf +
                         "), which changes the substeps each tick buys");
  }
  // The one warning that is nearly always the whole answer when it fires.
  const bool a_wall = a.seed.find("wall-clock") != std::string::npos;
  const bool b_wall = b.seed.find("wall-clock") != std::string::npos;
  if (a_wall || b_wall) {
    v.warnings.push_back(
        "at least one run seeded its random stream from the WALL CLOCK, so the two will "
        "diverge as soon as physics touches a gas no matter what the sims do. Pass --seed "
        "to both runs, or use a worldgen corpus. Any divergence below is unattributable.");
  } else if (a.seed != b.seed) {
    v.warnings.push_back("the two runs pinned different seeds (" + a.seed + " and " +
                         b.seed + "), so they are expected to diverge");
  }
  if (a.rows.size() != b.rows.size()) {
    v.warnings.push_back("the logs are different lengths (" + std::to_string(a.rows.size()) +
                         " and " + std::to_string(b.rows.size()) +
                         " ticks); comparing the overlap");
  }

  const size_t n = a.rows.size() < b.rows.size() ? a.rows.size() : b.rows.size();
  v.overlap = static_cast<int>(n);
  v.first_tick = a.rows[0].tick;
  v.last_tick = a.rows[n - 1].tick;
  v.columns.resize(kColumnCount);
  for (int col = 0; col < kColumnCount; ++col) {
    ColumnVerdict& cv = v.columns[static_cast<size_t>(col)];
    cv.name = kColumns[col];
    bool agreed_since = false;
    for (size_t i = 0; i < n; ++i) {
      // Ticks are matched by POSITION, not by number. Two logs whose tick numbering starts
      // at different places (one ran --ticks before the log opened) are still comparable
      // frame for frame, and matching by number would silently compare tick 40 with nothing.
      if (a.rows[i].h[col] == b.rows[i].h[col]) {
        if (cv.differs) agreed_since = true;
        continue;
      }
      if (!cv.differs) {
        cv.differs = true;
        cv.first = a.rows[i].tick;
      } else if (agreed_since) {
        cv.heals = true;
        agreed_since = false;
      }
      cv.last = a.rows[i].tick;
      ++cv.ticks_differing;
    }
    cv.persists = cv.differs && !agreed_since;
    if (!cv.differs) continue;
    if (!v.any || cv.first < v.divergence_tick) {
      v.divergence_tick = cv.first;
    }
    v.any = true;
  }
  if (v.any) {
    for (size_t i = 0; i < n; ++i) {
      if (a.rows[i].tick != v.divergence_tick) continue;
      v.mass_a = a.rows[i].total_mass;
      v.mass_b = b.rows[i].total_mass;
      break;
    }
  }
  return v;
}

}  // namespace digestlog
}  // namespace simviz
