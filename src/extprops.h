// Reading the per-cell EXTENSION PROPERTIES out of the sim, generically.
//
// `oni-sim-replacement`'s extension registry replaced a set of hand-written `World` member vectors with a
// registry: a mod (or the sim itself) registers a named per-cell property, gives it a type,
// an arity and a default, and the registry owns the storage. `SIM_DebugExtCellState` hands
// the whole lot back as one blob, and that blob is SELF-DESCRIBING -- every record carries
// its own name, type, arity, stride and cell count. So this program does not need to know
// what properties exist to show them. It needs to know the format once.
//
// THAT IS THE WHOLE POINT OF THIS FILE. Every other readout here is written against a fixed
// struct (`GameDataUpdate`) whose members were typed out by hand, and adding a surface to the
// sim means editing this repo. An extension property added by any mod, tomorrow, appears in
// the hover card and in --ext-list with nothing here changed. If that ever stops being true,
// something has been special-cased that should not have been.
//
// THE FORMAT IS NOT REDEFINED HERE. `sim/ext_state.h` and `sim/saveblob.h` are header-only,
// depend on nothing but `<cstdint>`-and-friends plus the ABI, and are already on this build's
// include path -- so the section is decoded by the sim's OWN `DecodeExtSection`, the same
// function `World::FromBlob` and the checkpoint restore call. A second parser written from
// the documented layout would agree with it until the day it did not, and the failure would
// be a plausible-looking wrong number rather than an error.
//
// WHICH EXPORT, AND WHY THERE ARE TWO. `SIM_DebugExtCellState` is the CHECKPOINT's getter:
// it carries `kRehydrated` and `kCheckpointOnly` and deliberately leaves out `kSaved`,
// because the save blob already carries those and restoring them twice would let two sources
// disagree. That exclusion covers exactly the properties the sim itself writes -- the gas
// mixture triple and `sim.room_promoted` -- so reading a world through it shows a mod's
// property and not the sim's own.
//
// `SIM_DebugExtCellStateAll` is the inspection twin, added the same day for this program:
// same format, all three classes, read-only (no message accepts its blob, and `ext_state`'s
// loader refuses a `kSaved` record by name). `Decode` is fed whichever the DLL has, and the
// table records which -- an older DLL still works, with the gas mixture missing, and says so
// rather than implying the cells are empty.
//
// ALSO ABSENT: A PROPERTY THAT IS ALL-DEFAULT EVERYWHERE. `ext_state::Save` skips those, so
// "not in this list" means "registered but untouched" just as often as it means "not
// registered at all", and this file cannot tell the two apart -- nothing exports the
// registration list. Every message printed from here is worded to leave that open.
//
// CELL INDEXING IS PADDED. The registry is sized by `World::Allocate(n)` with the PADDED cell
// count -- the sim keeps a one-cell border ring -- so a record holds `(w+2)*(h+2)` cells while
// every array in `GameDataUpdate` holds `w*h`. Game cell (x, y) is padded cell
// `(y+1)*(w+2) + (x+1)`. Getting that wrong reads the neighbour's value, which is a wrong
// number that looks completely reasonable, so `Decode` checks the count rather than trusting
// it: `DecodeExtSection` refuses a record whose cell count is not the one it was handed.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "out.h"
#include "sim/ext_state.h"  // oni-sim-replacement root, on the include path

namespace simviz {
namespace extprops {

using oni_sim::SaveExtProperty;

// One decode of one blob. `props` is empty in three different situations and the caller has
// to be able to tell them apart, which is what the two flags are for:
//
//   exported == false                 this DLL has no SIM_DebugExtCellState at all
//   exported, decoded, props empty    the export exists and there was nothing to carry
//   exported, !decoded                the blob was there and did not parse; see `error`
struct Table {
  bool exported = false;
  bool decoded = false;
  // Which export filled this. `SIM_DebugExtCellStateAll` carries all three persistence
  // classes; `SIM_DebugExtCellState` is the checkpoint's and carries two, leaving out the
  // kSaved ones the sim itself writes. A reader that could not tell them apart would report
  // "no gas mixture in this cell" for a world full of it, so this is stated in every
  // listing rather than inferred from what happens to be present.
  bool all_classes = false;
  // WHICH ROUTE OR EXPORT filled this, for the listing to state rather than the reader to
  // guess. `all_classes` above no longer implies it: `/extcells?published=1` also carries
  // every persistence class, because a subscription is not a persistence decision, so a
  // listing that derived the provenance from that flag would name the wrong source. Set by
  // whoever fetched the blob; `Decode` never touches it.
  std::string source;
  std::string error;
  size_t blob_bytes = 0;
  int padded_w = 0;
  int padded_h = 0;
  std::vector<SaveExtProperty> props;

  bool empty() const { return props.empty(); }

  const SaveExtProperty* Find(const std::string& name) const {
    for (const SaveExtProperty& p : props) {
      if (p.name == name) return &p;
    }
    return nullptr;
  }
};

// Game cell (x, y) -- y in SIM coordinates, 0 at the bottom -- as a padded cell index.
inline size_t PaddedIndex(int x, int y, int padded_w) {
  return static_cast<size_t>(y + 1) * static_cast<size_t>(padded_w) +
         static_cast<size_t>(x + 1);
}

inline const char* TypeName(int32_t type) {
  switch (type) {
    case oni_sim::ext::kExtF32: return "f32";
    case oni_sim::ext::kExtU8: return "u8";
    case oni_sim::ext::kExtU16: return "u16";
    case oni_sim::ext::kExtI32: return "i32";
    case oni_sim::ext::kExtElementIdx: return "element";
    default: return "?";
  }
}

// True for the types whose values are worth colouring a view by. `kExtU8` is included: a
// per-cell flag is exactly the kind of thing worth seeing as a map.
inline bool IsNumeric(int32_t type) {
  return type == oni_sim::ext::kExtF32 || type == oni_sim::ext::kExtU8 ||
         type == oni_sim::ext::kExtU16 || type == oni_sim::ext::kExtI32 ||
         type == oni_sim::ext::kExtElementIdx;
}

// A property's registered default, decoded out of its 32-bit pattern. The bit pattern is
// reinterpreted per type exactly as `sim_abi_ext.h` documents for `valueBits`: bit-cast for
// f32, the low byte / low two bytes for u8 / u16 / element index, verbatim for i32.
inline double DefaultValue(const SaveExtProperty& p) {
  switch (p.type) {
    case oni_sim::ext::kExtF32: {
      float f = 0.0f;
      memcpy(&f, &p.default_bits, 4);
      return static_cast<double>(f);
    }
    case oni_sim::ext::kExtU8: return static_cast<double>(p.default_bits & 0xFFu);
    case oni_sim::ext::kExtU16:
    case oni_sim::ext::kExtElementIdx: return static_cast<double>(p.default_bits & 0xFFFFu);
    case oni_sim::ext::kExtI32: {
      int32_t v = 0;
      memcpy(&v, &p.default_bits, 4);
      return static_cast<double>(v);
    }
    default: return 0.0;
  }
}

// Component `comp` of padded cell `cell`, or the property's default if either index is out
// of range. Returning the DEFAULT rather than zero on an out-of-range read matters: zero is
// a legal value for every type here and would read as data.
//
// Layout is cell-major, `(cell * arity + comp) * stride`, which is `SaveExtProperty`'s own
// documented layout and the one `CellPropertyRegistry` stores in.
inline double ScalarAt(const SaveExtProperty& p, size_t cell, int comp) {
  if (comp < 0 || comp >= p.arity) return DefaultValue(p);
  const size_t off = (cell * static_cast<size_t>(p.arity) + static_cast<size_t>(comp)) *
                     static_cast<size_t>(p.stride);
  if (p.stride <= 0 || off + static_cast<size_t>(p.stride) > p.bytes.size()) {
    return DefaultValue(p);
  }
  const uint8_t* q = p.bytes.data() + off;
  switch (p.type) {
    case oni_sim::ext::kExtF32: {
      float f = 0.0f;
      memcpy(&f, q, 4);
      return static_cast<double>(f);
    }
    case oni_sim::ext::kExtU8: return static_cast<double>(*q);
    case oni_sim::ext::kExtU16:
    case oni_sim::ext::kExtElementIdx: {
      uint16_t v = 0;
      memcpy(&v, q, 2);
      return static_cast<double>(v);
    }
    case oni_sim::ext::kExtI32: {
      int32_t v = 0;
      memcpy(&v, q, 4);
      return static_cast<double>(v);
    }
    default: return DefaultValue(p);
  }
}

// How many cells hold something other than the registered default in at least one
// component. This is the number that says whether a property is actually doing anything:
// a record is in the blob because SOME cell differs, not because this one does.
inline size_t NonDefaultCells(const SaveExtProperty& p) {
  if (p.stride <= 0 || p.arity <= 0) return 0;
  const double def = DefaultValue(p);
  const size_t cells = static_cast<size_t>(p.cell_count);
  size_t n = 0;
  for (size_t c = 0; c < cells; ++c) {
    for (int comp = 0; comp < p.arity; ++comp) {
      if (ScalarAt(p, c, comp) != def) {
        ++n;
        break;
      }
    }
  }
  return n;
}

// One value as text, printed the way its type reads: integers as integers, floats with
// enough digits to see a small bonus without filling the card with zeros.
inline std::string FormatValue(const SaveExtProperty& p, double v) {
  char buf[48];
  if (p.type == oni_sim::ext::kExtF32) {
    snprintf(buf, sizeof(buf), "%.6g", v);
  } else {
    snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
  }
  return std::string(buf);
}

// One property at one padded cell, as the hover card and --probe print it:
//
//     sim.thermal_mass_bonus 1250
//     mod.some_vector [0]=1 [2]=4          (arity > 1; only components that differ)
//
// A multi-component property prints only the components that are not at their default,
// because the interesting case (the gas mixture's slot arrays, once they are reachable) is
// mostly empty slots and a card listing sixteen zeros is a card nobody reads. When every
// component is at the default it says so, rather than printing nothing and looking like a
// property that failed to read.
inline std::string FormatCell(const SaveExtProperty& p, size_t cell) {
  const double def = DefaultValue(p);
  if (p.arity == 1) return p.name + " " + FormatValue(p, ScalarAt(p, cell, 0));
  std::string s = p.name;
  int shown = 0;
  for (int c = 0; c < p.arity; ++c) {
    const double v = ScalarAt(p, cell, c);
    if (v == def) continue;
    if (shown == 6) {
      s += " ...";
      break;
    }
    s += " [" + std::to_string(c) + "]=" + FormatValue(p, v);
    ++shown;
  }
  if (shown == 0) s += " (all default)";
  return s;
}

// Decodes `SIM_DebugExtCellState`'s blob. `game_w` / `game_h` are the UNPADDED grid, the
// dimensions every other part of this program uses; the padded count the records must agree
// with is derived here so that the caller never has to remember the border ring.
//
// Returns false and fills `out->error` rather than half-populating: a partially decoded
// table would be indistinguishable from a world where only some properties are set.
inline bool Decode(const std::vector<uint8_t>& blob, int game_w, int game_h, bool all_classes,
                   Table* out) {
  *out = Table();
  out->all_classes = all_classes;
  out->blob_bytes = blob.size();
  out->padded_w = game_w + 2;
  out->padded_h = game_h + 2;
  if (blob.empty()) {
    out->error =
        "this SimDLL exports neither SIM_DebugExtCellStateAll nor SIM_DebugExtCellState -- "
        "it predates the per-cell extension registry";
    return false;
  }
  out->exported = true;
  if (blob.size() < oni_sim::ext_state::kHeaderSize) {
    out->error = "extension-state blob is shorter than its own header";
    return false;
  }
  uint32_t magic = 0, version = 0;
  memcpy(&magic, blob.data(), 4);
  memcpy(&version, blob.data() + 4, 4);
  if (magic != oni_sim::ext_state::kMagic) {
    out->error = "extension-state blob has the wrong magic";
    return false;
  }
  if (version != oni_sim::ext_state::kVersion) {
    out->error = "extension-state blob is version " + std::to_string(version) + ", and this "
                 "build reads version " + std::to_string(oni_sim::ext_state::kVersion);
    return false;
  }
  const size_t cells = static_cast<size_t>(out->padded_w) * static_cast<size_t>(out->padded_h);
  const uint8_t* const begin = blob.data() + oni_sim::ext_state::kHeaderSize;
  const uint8_t* const end = blob.data() + blob.size();
  std::string err;
  const uint8_t* q = oni_sim::DecodeExtSection(begin, end, cells, /*allow_empty=*/true,
                                               &out->props, &err);
  if (q == nullptr) {
    out->props.clear();
    // The cell-count refusal is the one worth naming, because it is the one a reader will
    // hit if this program's idea of the padding ring ever stops matching the sim's.
    out->error = err.empty() ? "extension-state blob did not decode" : err;
    return false;
  }
  if (q != end) {
    out->props.clear();
    out->error = "extension-state blob has bytes after its last record";
    return false;
  }
  out->decoded = true;
  return true;
}

}  // namespace extprops
}  // namespace simviz
