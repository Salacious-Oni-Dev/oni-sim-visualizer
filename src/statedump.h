// The whole published frame in a file, and the diff of two of them.
//
// `--digest-log` and `--bisect` answer WHEN two runs parted. This answers WHERE, which is
// the half a hash cannot: an FNV over 257,000 cells has no geometry, so a digest that
// differs is a fact with no location in it. A divergence that is one cell is a bug in one
// kernel; the same divergence spread over a front is a bug in a boundary; the same count
// scattered over the world is a seed. The three are indistinguishable in a hash and obvious
// in a picture, which is why `--diff-state` renders as well as counts.
//
// THE FORMAT IS SELF-DESCRIBING, for the same reason the sim's extension blob is: a reader
// that needs a list of array names compiled into it is a reader that silently reports
// "identical" for anything the writer added since. Every record carries its own name, type,
// arity, stride and cell count, so a dump written by a later build is readable by this one
// -- it reports the arrays it understands and names the ones it does not.
//
//   uint32  magic        'O' 'N' 'I' 'S', little-endian
//   uint32  version      2
//   int32   width, height, cells
//   int32   sourceBytes  followed by that many bytes of UTF-8, no NUL
//   int32   arrayCount
//   per array:
//     char  name[32]     NUL-padded
//     int32 type         kF32 / kU8 / kU16 / kI32 / kU32 / kElementIdx
//     int32 arity        components per cell
//     int32 stride       bytes per component
//     int32 cells
//     bytes[cells * arity * stride]
//   int32   extBytes     followed by that many bytes: SIM_DebugExtCellStateAll, verbatim
//   int32   registryBytes followed by that many bytes: SIM_DebugRegistryState, verbatim
//
// VERSION 1 IS STILL READ. It had no magic and no records -- int32 width, int32 height,
// then elementIdx, mass and temperature back to back -- and `tools/attach-stub.py` serves
// one. Reading it here costs twenty lines and means a dump taken before this format existed
// is still worth something, which is the entire argument for keeping old debugging
// artefacts readable.
//
// The two blobs at the end are carried VERBATIM and not unpacked into arrays. They are
// already self-describing in their own right, they are on the PADDED grid rather than this
// file's game grid, and re-encoding them here would create a second thing that can be wrong
// about the sim's layout. The diff decodes them with the same `extprops` reader the rest of
// this program uses.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "out.h"
#include "extprops.h"
#include "image.h"

namespace simviz {
namespace statedump {

constexpr uint32_t kMagic = 0x53494E4Fu;  // 'O','N','I','S' little-endian
constexpr uint32_t kVersion = 2;
// 48, the same width the sim's own extension records use. It was 32 for one build, and the
// first real run caught it: `propertyTextureExposedToSunlight` is exactly 32 characters, so
// it was truncated to 31 in both dumps, matched neither dump's grouping, and was reported as
// "only in the second dump" -- a wrong answer about a surface that was in fact identical.
constexpr int kNameBytes = 48;
constexpr int kMaxArrays = 64;

// Deliberately the same numbering as `sim_abi_ext.h`'s value types, extended with the one
// this file needs and that has no per-cell extension equivalent: the RGBA property textures
// are four bytes read as a unit. Sharing the numbering means a reader can hold one idea of
// what a type is; the static_asserts are what keep that true if the ABI ever renumbers.
// `kElementIdx` is the ABI's kExtElementIdx, which skips 4 so that `kU32` keeps its number.
enum Type { kF32 = 0, kU8 = 1, kU16 = 2, kI32 = 3, kU32 = 4, kElementIdx = 5 };
// Cast to int on both sides: these are two distinct enum types, and comparing them
// directly is a warning in every compiler that has an opinion about it.
static_assert(static_cast<int>(kF32) == static_cast<int>(oni_sim::ext::kExtF32),
              "f32 renumbered in the ABI");
static_assert(static_cast<int>(kU8) == static_cast<int>(oni_sim::ext::kExtU8),
              "u8 renumbered in the ABI");
static_assert(static_cast<int>(kU16) == static_cast<int>(oni_sim::ext::kExtU16),
              "u16 renumbered in the ABI");
static_assert(static_cast<int>(kI32) == static_cast<int>(oni_sim::ext::kExtI32),
              "i32 renumbered in the ABI");
static_assert(static_cast<int>(kElementIdx) == static_cast<int>(oni_sim::ext::kExtElementIdx),
              "element index renumbered in the ABI");

inline int StrideOf(int32_t type) {
  switch (type) {
    case kF32: return 4;
    case kU8: return 1;
    case kU16: return 2;
    case kI32: return 4;
    case kU32: return 4;
    case kElementIdx: return 2;
    default: return 0;
  }
}

inline const char* TypeName(int32_t type) {
  switch (type) {
    case kF32: return "f32";
    case kU8: return "u8";
    case kU16: return "u16";
    case kI32: return "i32";
    case kU32: return "u32";
    case kElementIdx: return "element";
    default: return "?";
  }
}

// True for the types whose difference is a QUANTITY. An elementIdx that differs by 3 has not
// changed by three of anything, so a "worst delta" over it would be a number that looks like
// a measurement and is not one.
inline bool IsScalar(int32_t type) { return type == kF32; }

struct Array {
  std::string name;
  int32_t type = kF32;
  int32_t arity = 1;
  int32_t stride = 4;
  int32_t cells = 0;
  std::vector<uint8_t> bytes;

  size_t values() const {
    return static_cast<size_t>(cells) * static_cast<size_t>(arity);
  }
  size_t expected_bytes() const { return values() * static_cast<size_t>(stride); }

  // One component of one cell as a double. Integer types widen; f32 converts. Out of range
  // returns 0, and every caller here bounds-checks before asking.
  double Value(size_t index) const {
    const size_t off = index * static_cast<size_t>(stride);
    if (off + static_cast<size_t>(stride) > bytes.size()) return 0.0;
    const uint8_t* p = bytes.data() + off;
    switch (type) {
      case kF32: {
        float f = 0.0f;
        memcpy(&f, p, 4);
        return static_cast<double>(f);
      }
      case kU8: return static_cast<double>(*p);
      case kU16:
      case kElementIdx: {
        uint16_t u = 0;
        memcpy(&u, p, 2);
        return static_cast<double>(u);
      }
      case kI32: {
        int32_t i = 0;
        memcpy(&i, p, 4);
        return static_cast<double>(i);
      }
      case kU32: {
        uint32_t u = 0;
        memcpy(&u, p, 4);
        return static_cast<double>(u);
      }
      default: return 0.0;
    }
  }
};

struct Dump {
  std::string path;
  std::string source;   // the DLL that produced it, or the endpoint it was polled from
  int32_t width = 0, height = 0, cells = 0;
  uint32_t version = kVersion;
  std::vector<Array> arrays;
  std::vector<uint8_t> ext_blob;
  std::vector<uint8_t> registry_blob;

  const Array* Find(const std::string& name) const {
    for (const Array& a : arrays) {
      if (a.name == name) return &a;
    }
    return nullptr;
  }
};

// ------------------------------------------------------------------------ writing

inline void PutI32(std::vector<uint8_t>* out, int32_t v) {
  uint8_t b[4];
  memcpy(b, &v, 4);
  out->insert(out->end(), b, b + 4);
}

inline void PutU32(std::vector<uint8_t>* out, uint32_t v) {
  uint8_t b[4];
  memcpy(b, &v, 4);
  out->insert(out->end(), b, b + 4);
}

inline void PutBlob(std::vector<uint8_t>* out, const std::vector<uint8_t>& blob) {
  PutI32(out, static_cast<int32_t>(blob.size()));
  out->insert(out->end(), blob.begin(), blob.end());
}

inline bool Write(const Dump& d, const char* path, std::string* err) {
  std::vector<uint8_t> out;
  PutU32(&out, kMagic);
  PutU32(&out, kVersion);
  PutI32(&out, d.width);
  PutI32(&out, d.height);
  PutI32(&out, d.cells);
  PutI32(&out, static_cast<int32_t>(d.source.size()));
  out.insert(out.end(), d.source.begin(), d.source.end());
  PutI32(&out, static_cast<int32_t>(d.arrays.size()));
  for (const Array& a : d.arrays) {
    // Refused rather than truncated. Truncation is what produced the bug this field was
    // widened for: two dumps that agree perfectly about a surface, both carrying its name
    // cut short in the same place, and a diff that reports it as present in one of them.
    // A name too long is a bug in the caller a few lines away and should fail there.
    if (a.name.size() >= static_cast<size_t>(kNameBytes)) {
      *err = "array name \"" + a.name + "\" is " + std::to_string(a.name.size()) +
             " characters and the format holds " + std::to_string(kNameBytes - 1);
      return false;
    }
    char name[kNameBytes] = {};
    memcpy(name, a.name.c_str(), a.name.size());
    out.insert(out.end(), name, name + kNameBytes);
    PutI32(&out, a.type);
    PutI32(&out, a.arity);
    PutI32(&out, a.stride);
    PutI32(&out, a.cells);
    out.insert(out.end(), a.bytes.begin(), a.bytes.end());
  }
  PutBlob(&out, d.ext_blob);
  PutBlob(&out, d.registry_blob);

  FILE* f = fopen(path, "wb");
  if (f == nullptr) {
    *err = std::string("cannot write ") + path;
    return false;
  }
  const size_t wrote = out.empty() ? 0 : fwrite(out.data(), 1, out.size(), f);
  fclose(f);
  if (wrote != out.size()) {
    *err = std::string("short write to ") + path;
    return false;
  }
  return true;
}

// Append one array, copying `cells * arity` values out of a published pointer. A null
// pointer appends NOTHING rather than a zero-filled record: attached, a surface the fetch
// did not ask for is absent, and a record of zeros would be indistinguishable from a
// surface the sim published as zeros. "Absent" and "empty" are different findings.
inline void AddArray(Dump* d, const char* name, int32_t type, int32_t arity,
                     const void* data, int32_t cells) {
  if (data == nullptr || cells <= 0) return;
  Array a;
  a.name = name;
  a.type = type;
  a.arity = arity;
  a.stride = StrideOf(type);
  a.cells = cells;
  a.bytes.resize(a.expected_bytes());
  if (!a.bytes.empty()) memcpy(a.bytes.data(), data, a.bytes.size());
  d->arrays.push_back(std::move(a));
}

// ------------------------------------------------------------------------ reading

inline bool ReadI32(const std::vector<uint8_t>& b, size_t* p, int32_t* v) {
  if (*p + 4 > b.size()) return false;
  memcpy(v, b.data() + *p, 4);
  *p += 4;
  return true;
}

inline bool ReadU32(const std::vector<uint8_t>& b, size_t* p, uint32_t* v) {
  if (*p + 4 > b.size()) return false;
  memcpy(v, b.data() + *p, 4);
  *p += 4;
  return true;
}

// The v1 layout, which had no header of its own: int32 width, int32 height, then
// elementIdx, mass and temperature back to back, and nothing else. Recognised by its exact
// length, which is what `tools/attach-stub.py` has always checked too.
inline bool ReadV1(const std::vector<uint8_t>& blob, Dump* out, std::string* err) {
  size_t p = 0;
  int32_t w = 0, h = 0;
  if (!ReadI32(blob, &p, &w) || !ReadI32(blob, &p, &h) || w <= 0 || h <= 0) {
    *err = out->path + " is neither a v2 dump (bad magic) nor a v1 one (no world size)";
    return false;
  }
  const size_t cells = static_cast<size_t>(w) * static_cast<size_t>(h);
  const size_t want = 8 + cells * (2 + 4 + 4);
  if (blob.size() != want) {
    char buf[192];
    snprintf(buf, sizeof(buf), " is %zu bytes; a v1 dump of %dx%d needs %zu", blob.size(), w,
             h, want);
    *err = out->path + buf;
    return false;
  }
  out->version = 1;
  out->width = w;
  out->height = h;
  out->cells = static_cast<int32_t>(cells);
  out->source = "(v1 dump: the source is not recorded in this format)";
  AddArray(out, "elementIdx", kU16, 1, blob.data() + 8, out->cells);
  AddArray(out, "mass", kF32, 1, blob.data() + 8 + cells * 2, out->cells);
  AddArray(out, "temperature", kF32, 1, blob.data() + 8 + cells * 6, out->cells);
  return true;
}

inline bool Read(const char* path, Dump* out, std::string* err) {
  *out = Dump();
  out->path = path;
  FILE* f = fopen(path, "rb");
  if (f == nullptr) {
    *err = std::string("cannot open ") + path;
    return false;
  }
  std::vector<uint8_t> blob;
  uint8_t chunk[65536];
  size_t n = 0;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
    blob.insert(blob.end(), chunk, chunk + n);
  }
  fclose(f);
  if (blob.size() < 8) {
    *err = out->path + " is too short to be a state dump";
    return false;
  }
  uint32_t magic = 0;
  memcpy(&magic, blob.data(), 4);
  if (magic != kMagic) return ReadV1(blob, out, err);

  size_t p = 4;
  uint32_t version = 0;
  if (!ReadU32(blob, &p, &version)) {
    *err = out->path + " has a magic and no version";
    return false;
  }
  if (version != kVersion) {
    *err = out->path + " is state-dump version " + std::to_string(version) +
           ", and this build reads version " + std::to_string(kVersion);
    return false;
  }
  out->version = version;
  int32_t count = 0;
  int32_t source_bytes = 0;
  if (!ReadI32(blob, &p, &out->width) || !ReadI32(blob, &p, &out->height) ||
      !ReadI32(blob, &p, &out->cells) || !ReadI32(blob, &p, &source_bytes)) {
    *err = out->path + " ends inside its header";
    return false;
  }
  if (source_bytes < 0 || p + static_cast<size_t>(source_bytes) > blob.size()) {
    *err = out->path + " claims a source string longer than the file";
    return false;
  }
  out->source.assign(reinterpret_cast<const char*>(blob.data() + p),
                     static_cast<size_t>(source_bytes));
  p += static_cast<size_t>(source_bytes);
  if (!ReadI32(blob, &p, &count)) {
    *err = out->path + " ends before its array count";
    return false;
  }
  if (count < 0 || count > kMaxArrays) {
    *err = out->path + " claims " + std::to_string(count) + " arrays";
    return false;
  }
  for (int32_t i = 0; i < count; ++i) {
    if (p + static_cast<size_t>(kNameBytes) > blob.size()) {
      *err = out->path + " ends inside array " + std::to_string(i) + "'s name";
      return false;
    }
    Array a;
    const char* name = reinterpret_cast<const char*>(blob.data() + p);
    if (memchr(name, 0, kNameBytes) == nullptr) {
      *err = out->path + ": array " + std::to_string(i) + " has an unterminated name";
      return false;
    }
    a.name = name;
    p += static_cast<size_t>(kNameBytes);
    if (!ReadI32(blob, &p, &a.type) || !ReadI32(blob, &p, &a.arity) ||
        !ReadI32(blob, &p, &a.stride) || !ReadI32(blob, &p, &a.cells)) {
      *err = out->path + ": array \"" + a.name + "\" ends inside its header";
      return false;
    }
    if (a.arity <= 0 || a.stride <= 0 || a.cells < 0) {
      *err = out->path + ": array \"" + a.name + "\" has an impossible shape";
      return false;
    }
    const size_t want = a.expected_bytes();
    if (p + want > blob.size()) {
      *err = out->path + ": array \"" + a.name + "\" claims " + std::to_string(want) +
             " bytes and the file has " + std::to_string(blob.size() - p) + " left";
      return false;
    }
    a.bytes.assign(blob.begin() + static_cast<long>(p),
                   blob.begin() + static_cast<long>(p + want));
    p += want;
    out->arrays.push_back(std::move(a));
  }
  int32_t ext_bytes = 0, reg_bytes = 0;
  if (!ReadI32(blob, &p, &ext_bytes) || ext_bytes < 0 ||
      p + static_cast<size_t>(ext_bytes) > blob.size()) {
    *err = out->path + " ends inside its extension blob";
    return false;
  }
  out->ext_blob.assign(blob.begin() + static_cast<long>(p),
                       blob.begin() + static_cast<long>(p + static_cast<size_t>(ext_bytes)));
  p += static_cast<size_t>(ext_bytes);
  if (!ReadI32(blob, &p, &reg_bytes) || reg_bytes < 0 ||
      p + static_cast<size_t>(reg_bytes) > blob.size()) {
    *err = out->path + " ends inside its registry blob";
    return false;
  }
  out->registry_blob.assign(
      blob.begin() + static_cast<long>(p),
      blob.begin() + static_cast<long>(p + static_cast<size_t>(reg_bytes)));
  p += static_cast<size_t>(reg_bytes);
  if (p != blob.size()) {
    *err = out->path + " has " + std::to_string(blob.size() - p) +
           " bytes after its last record";
    return false;
  }
  return true;
}

// ------------------------------------------------------------------------ diffing

struct ArrayDiff {
  std::string name;
  bool in_a = false, in_b = false;
  bool comparable = false;
  std::string note;          // why not comparable, when it is not
  int32_t type = kF32;
  int32_t arity = 1;
  size_t values = 0;
  size_t wrong_cells = 0;    // CELLS in which any component differs
  size_t wrong_values = 0;
  size_t first_value = 0;    // index into the value stream
  int first_cell = -1, first_comp = 0;
  double first_a = 0.0, first_b = 0.0;
  double worst = 0.0;        // meaningful only when IsScalar(type)
  int worst_cell = -1;
};

inline ArrayDiff DiffArray(const Dump& a, const Dump& b, const std::string& name) {
  ArrayDiff d;
  d.name = name;
  const Array* pa = a.Find(name);
  const Array* pb = b.Find(name);
  d.in_a = pa != nullptr;
  d.in_b = pb != nullptr;
  if (!d.in_a || !d.in_b) {
    // Three ways to be absent, not two. "In neither" is a different finding from "in one":
    // it says this build asked for an array that no dump on either side carries, which is
    // what a stale grouping table looks like -- and reporting it as "only in the second
    // dump" is exactly the wrong answer, because it names a difference that is not there.
    d.note = !d.in_a && !d.in_b ? "in neither dump"
                                : (d.in_a ? "only in the first dump" : "only in the second dump");
    return d;
  }
  if (pa->type != pb->type || pa->arity != pb->arity || pa->cells != pb->cells) {
    d.note = "shape differs between the two dumps";
    return d;
  }
  d.comparable = true;
  d.type = pa->type;
  d.arity = pa->arity;
  d.values = pa->values();
  // Exact bytes first. Two identical arrays are the common case and memcmp settles them in
  // one pass rather than 257,000 double conversions.
  if (pa->bytes.size() == pb->bytes.size() &&
      (pa->bytes.empty() || memcmp(pa->bytes.data(), pb->bytes.data(), pa->bytes.size()) == 0)) {
    return d;
  }
  const size_t stride = static_cast<size_t>(pa->stride);
  int last_wrong_cell = -1;
  for (size_t i = 0; i < d.values; ++i) {
    const size_t off = i * stride;
    if (memcmp(pa->bytes.data() + off, pb->bytes.data() + off, stride) == 0) continue;
    const int cell = static_cast<int>(i / static_cast<size_t>(pa->arity));
    const int comp = static_cast<int>(i % static_cast<size_t>(pa->arity));
    if (d.wrong_values == 0) {
      d.first_value = i;
      d.first_cell = cell;
      d.first_comp = comp;
      d.first_a = pa->Value(i);
      d.first_b = pb->Value(i);
    }
    ++d.wrong_values;
    if (cell != last_wrong_cell) {
      ++d.wrong_cells;
      last_wrong_cell = cell;
    }
    if (IsScalar(d.type)) {
      const double delta = std::fabs(pa->Value(i) - pb->Value(i));
      if (delta > d.worst) {
        d.worst = delta;
        d.worst_cell = cell;
      }
    }
  }
  return d;
}

// One extension property, compared between two blobs. The padded-grid indexing is the
// decoder's problem and stays there: this counts PADDED cells, and the ring is part of the
// property, so a divergence in it is a real divergence and not an artefact to hide.
struct ExtDiff {
  std::string name;
  bool in_a = false, in_b = false;
  bool comparable = false;
  std::string note;
  int32_t type = 0, arity = 1;
  size_t cells = 0;
  size_t wrong_cells = 0;
  int first_cell = -1, first_comp = 0;
  double first_a = 0.0, first_b = 0.0;
  double worst = 0.0;
};

inline std::vector<ExtDiff> DiffExt(const extprops::Table& ta, const extprops::Table& tb) {
  std::vector<ExtDiff> out;
  std::vector<std::string> names;
  for (const oni_sim::SaveExtProperty& p : ta.props) names.push_back(p.name);
  for (const oni_sim::SaveExtProperty& p : tb.props) {
    bool seen = false;
    for (const std::string& n : names) {
      if (n == p.name) seen = true;
    }
    if (!seen) names.push_back(p.name);
  }
  for (const std::string& name : names) {
    ExtDiff d;
    d.name = name;
    const oni_sim::SaveExtProperty* pa = ta.Find(name);
    const oni_sim::SaveExtProperty* pb = tb.Find(name);
    d.in_a = pa != nullptr;
    d.in_b = pb != nullptr;
    if (!d.in_a || !d.in_b) {
      // Absent is not zero. A property at its registered default in every cell is OMITTED by
      // the sim, so "only in the first dump" can mean the second run left it untouched --
      // which is itself a divergence worth seeing, and is why this is reported rather than
      // skipped.
      d.note = d.in_a ? "only in the first dump (the second may have left it all-default)"
                      : "only in the second dump (the first may have left it all-default)";
      out.push_back(d);
      continue;
    }
    if (pa->type != pb->type || pa->arity != pb->arity ||
        pa->bytes.size() != pb->bytes.size()) {
      d.note = "shape differs between the two dumps";
      out.push_back(d);
      continue;
    }
    d.comparable = true;
    d.type = pa->type;
    d.arity = pa->arity;
    d.cells = pa->cell_count;
    for (size_t c = 0; c < d.cells; ++c) {
      bool cell_wrong = false;
      for (int comp = 0; comp < pa->arity; ++comp) {
        const double va = extprops::ScalarAt(*pa, c, comp);
        const double vb = extprops::ScalarAt(*pb, c, comp);
        if (va == vb) continue;
        if (!cell_wrong) {
          cell_wrong = true;
          ++d.wrong_cells;
        }
        if (d.first_cell < 0) {
          d.first_cell = static_cast<int>(c);
          d.first_comp = comp;
          d.first_a = va;
          d.first_b = vb;
        }
        const double delta = std::fabs(va - vb);
        if (delta > d.worst) d.worst = delta;
      }
    }
    out.push_back(d);
  }
  return out;
}

// The published arrays this program knows how to group, and which digest column each one
// feeds. The mapping exists so `--bisect` and `--diff-state` speak ONE vocabulary: a bisect
// that says "flowTex parted at 137" is useless if the diff then reports on something called
// `propertyTextureFlow` and never mentions the name the reader was given.
struct Grouping {
  const char* digest_column;
  const char* array;
};
inline const Grouping kGroups[] = {
    {"elementIdx", "elementIdx"},
    {"mass", "mass"},
    {"temperature", "temperature"},
    {"radiation", "radiation"},
    {"disease", "diseaseIdx"},
    {"disease", "diseaseCount"},
    {"backwall", "backwallElement"},
    {"backwall", "backwallMass"},
    {"backwall", "backwallTemperature"},
    {"building", "properties"},
    {"building", "insulation"},
    {"building", "strengthInfo"},
    {"flowTex", "propertyTextureFlow"},
    {"flowTex", "accumulatedFlow"},
    {"liquidTex", "propertyTextureLiquid"},
    {"liquidTex", "propertyTextureLiquidData"},
    {"liquidTex", "propertyTextureMaterialData"},
    {"sunlightTex", "propertyTextureExposedToSunlight"},
};
constexpr int kGroupCount = sizeof(kGroups) / sizeof(kGroups[0]);

// ------------------------------------------------------------------------ rendering

// The divergence, drawn in the world it happened in.
//
// Two layers, and the backdrop is the load-bearing one. A map of only the differing cells
// is a constellation with no ground under it -- 412 lit pixels that could be anywhere. The
// backdrop is `mass` from the FIRST dump, dim, so the lit cells land in a recognisable
// world and "the divergence follows the wall" is a thing that can be seen rather than
// deduced. When the dump carries no mass array the backdrop is flat and the picture is
// honest about that by being flat.
inline bool RenderDiff(const Dump& a, const Dump& b, const std::string& name, Image* out,
                       std::string* err) {
  const ArrayDiff d = DiffArray(a, b, name);
  if (!d.comparable) {
    *err = "cannot draw \"" + name + "\": " + (d.note.empty() ? "not comparable" : d.note);
    return false;
  }
  const int w = a.width, h = a.height;
  if (w <= 0 || h <= 0) {
    *err = "the dump carries no world size";
    return false;
  }
  const Array* pa = a.Find(name);
  const Array* pb = b.Find(name);
  if (pa->cells != a.cells) {
    *err = "\"" + name + "\" holds " + std::to_string(pa->cells) + " cells and the world is " +
           std::to_string(a.cells);
    return false;
  }
  const Array* mass = a.Find("mass");
  const size_t cells = static_cast<size_t>(a.cells);
  const size_t stride = static_cast<size_t>(pa->stride);
  const int arity = pa->arity;

  // Pass one: how much each cell differs by, and the band those magnitudes actually span.
  // A negative entry means the cell agrees, which is not the same as differing by zero --
  // two floats can differ in their last bit and produce a magnitude that rounds to it.
  std::vector<double> magnitude(cells, -1.0);
  double lo = 0.0, hi = 0.0;
  bool any = false;
  for (size_t cell = 0; cell < cells; ++cell) {
    double m = 0.0;
    bool differs = false;
    for (int comp = 0; comp < arity; ++comp) {
      const size_t i = cell * static_cast<size_t>(arity) + static_cast<size_t>(comp);
      const size_t off = i * stride;
      if (memcmp(pa->bytes.data() + off, pb->bytes.data() + off, stride) == 0) continue;
      differs = true;
      if (IsScalar(pa->type)) {
        const double delta = std::fabs(pa->Value(i) - pb->Value(i));
        if (delta > m) m = delta;
      }
    }
    if (!differs) continue;
    magnitude[cell] = m;
    if (m > 0.0) {
      if (!any || m < lo) lo = m;
      if (!any || m > hi) hi = m;
      any = true;
    }
  }

  // LOG scale, and this is the whole reason the render is worth having rather than a list.
  // A divergence spans orders of magnitude by its nature -- it starts in a float's last bit
  // and ends at hundreds of kelvin -- so a linear ramp against the largest delta paints
  // every cell that has just started to part in the same colour as one that never parted at
  // all. The first version of this did exactly that and drew 61,062 differing cells as one
  // flat wash, which is a picture of nothing.
  const bool log_ramp = any && hi > lo;
  const double log_lo = log_ramp ? std::log10(lo) : 0.0;
  const double log_span = log_ramp ? (std::log10(hi) - log_lo) : 1.0;

  out->Reset(w, h);
  for (int y = 0; y < h; ++y) {
    const int row = h - 1 - y;   // sim y is 0 at the bottom; the image's is 0 at the top
    for (int x = 0; x < w; ++x) {
      const size_t cell = static_cast<size_t>(y) * static_cast<size_t>(w) +
                          static_cast<size_t>(x);
      const double m = magnitude[cell];
      if (m < 0.0) {
        // The backdrop, from the FIRST dump's mass. Dim on purpose: it is there so the lit
        // cells land in a recognisable world, and a backdrop bright enough to read as data
        // would be competing with the thing being reported.
        uint8_t g = 10;
        if (mass != nullptr && mass->cells == a.cells) {
          g = mass->Value(cell) > 0.0 ? 34 : 12;
        }
        out->Set(x, row, Rgb{g, g, static_cast<uint8_t>(g + 4)});
        continue;
      }
      if (!IsScalar(pa->type)) {
        // An exact type has no magnitude to ramp, so every differing cell is drawn the same.
        // Painting a ramp over an index difference would invent a gradient.
        out->Set(x, row, Rgb{240, 72, 56});
        continue;
      }
      double t = 0.0;
      if (m > 0.0 && log_ramp) {
        t = (std::log10(m) - log_lo) / log_span;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
      }
      // Deep red at the bottom of the band, white at the top, through orange. Monotone in
      // brightness as well as hue, so it survives being read on a bad screen and in a
      // greyscale printout of a bug report.
      const double r = 120.0 + 135.0 * t;
      const double g = 20.0 + 225.0 * t * t;
      const double bl = 15.0 + 185.0 * t * t * t * t;
      out->Set(x, row,
               Rgb{static_cast<uint8_t>(r), static_cast<uint8_t>(g),
                   static_cast<uint8_t>(bl)});
    }
  }
  return true;
}

}  // namespace statedump
}  // namespace simviz
