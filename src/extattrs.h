// Registry 2 -- the per-element attribute registry -- as a table this program can print.
//
// The other two registries reach this repo as BYTES: `/extcells` and the checkpoint export
// hand over a blob the sim serialised, and `extprops.h` / `extevents.h` decode it. This one
// does not, and the difference is the shape of the data rather than a preference. A per-cell
// property has one value per cell, so a registry of them is megabytes and a binary format
// earns itself; an element attribute has one value per ELEMENT THAT HAS ONE -- four entries
// for the only attribute a stock custom build carries, and 64 attributes at the ABI's cap.
// So there is no ONIE/ONIV-style wire format here, and deliberately no fourth one to keep two
// writers agreeing on: attached, this reads the JSON `/extelements` serves; offline, it calls
// the sim's three exports directly and builds the same table.
//
// WHY THERE ARE THREE EXPORTS AND NOT ONE. `SIM_ExtElementAttributeIndex` answers only for a
// name the caller already typed and `SIM_ExtElementAttribute` needs an element id as well, so
// before the enumeration exports a tool handed a SimDLL could not discover that this registry held
// anything at all -- not even the sim's own `sim.molecular_mass`, which is registered
// natively in `World`'s constructor and therefore invisible to any ledger of what mods have
// registered.
//
// UNSET IS A REAL ANSWER, and it is the reason `value_count` and `writes` are both carried. An
// attribute with no values is either one nobody has pushed yet or one whose pusher ran and
// cleared everything; those mean opposite things and `writes` is the only field that separates
// them. Nothing in this registry is ever saved and there is no registered default, so unlike
// `extprops.h` there is no "at its default in every cell" case to explain away.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace simviz {
namespace extattrs {

// `ExtScalarType` in `abi/sim_abi_ext.h`. Spelled here rather than included because this
// header is the only thing in this repo that needs the names, and the values are wire values.
inline const char* TypeName(int32_t type) {
  switch (type) {
    case 0: return "f32";
    case 1: return "u8";
    case 2: return "u16";
    case 3: return "i32";
    case 5: return "element";
    default: return "?";
  }
}

// One element's value for one attribute. `bits` is one entry per component, in the sim's own
// order, still as raw bits -- the type is on the attribute, not on the value, exactly as the
// sim stores it.
struct Value {
  int32_t id_hash = 0;
  // The SimHashes name, when something knew it. Offline nothing does: the sim's element table
  // carries the hash and no name at all, so this stays empty and the hash is the identity.
  std::string element;
  // Whether the currently loaded element table has a row for this hash. FALSE IS NOT AN ERROR
  // -- keys are ids precisely so a value survives the table being reloaded, so a mod may
  // legitimately hold one for an element this table does not have.
  bool in_table = false;
  std::vector<uint32_t> bits;
};

struct Attribute {
  int32_t idx = 0;
  std::string name;
  int32_t type = 0;
  int32_t arity = 1;
  int32_t stride = 4;
  // Elements carrying a value. Not the element count, and not `values.size()` when the values
  // were not fetched.
  int32_t value_count = 0;
  bool first_party = false;
  uint64_t writes = 0;
  std::vector<Value> values;

  // The value one element carries, or null for one that carries none. Linear, because the
  // registry is sparse by construction -- a handful of elements out of 212, capped at 64
  // attributes by the ABI -- and because the one caller that runs per cell (the attr view)
  // resolves this ONCE into an element-indexed array rather than calling it 256,944 times.
  const Value* Find(int32_t id_hash) const {
    for (const Value& v : values) {
      if (v.id_hash == id_hash) return &v;
    }
    return nullptr;
  }
};

struct Table {
  // False means this SimDLL cannot enumerate -- stock, or older than stage 5b -- which is a
  // different fact from "there are none" and has to stay distinguishable, because a custom
  // SimDLL always has at least `sim.molecular_mass`.
  bool exported = false;
  bool with_values = false;
  std::string error;
  std::string source;
  std::vector<Attribute> attrs;

  bool empty() const { return attrs.empty(); }

  // By registered name, the way `--view attr:<name>` and the panels name one. Null is the
  // answer for a typo and for an attribute nobody registered alike; the caller has `exported`
  // and `error` to tell those from "this DLL cannot enumerate at all", which is a third thing.
  const Attribute* Find(const std::string& name) const {
    for (const Attribute& a : attrs) {
      if (a.name == name) return &a;
    }
    return nullptr;
  }
};

// One component, rendered the way its registered type reads it. The sim keeps every scalar in
// a uint32 and reinterprets on the way out; this is the same reinterpretation.
inline std::string FormatValue(int32_t type, uint32_t bits) {
  char buf[64];
  switch (type) {
    case 0: {
      float f = 0.0f;
      memcpy(&f, &bits, sizeof(f));
      snprintf(buf, sizeof(buf), "%g", static_cast<double>(f));
      break;
    }
    case 1: snprintf(buf, sizeof(buf), "%u", bits & 0xFFu); break;
    case 2:
    case 5: snprintf(buf, sizeof(buf), "%u", bits & 0xFFFFu); break;
    case 3: snprintf(buf, sizeof(buf), "%d", static_cast<int32_t>(bits)); break;
    default: snprintf(buf, sizeof(buf), "0x%08X", bits); break;
  }
  return buf;
}

// The same reinterpretation as a NUMBER rather than as text, for the view that ramps a
// colour over it and for the range scan that bounds the ramp. It has to stay the same
// reinterpretation: a picture coloured by one reading of the bits while the listing above
// prints another would be a disagreement with no error in it.
inline double ScalarOf(int32_t type, uint32_t bits) {
  switch (type) {
    case 0: {
      float f = 0.0f;
      memcpy(&f, &bits, sizeof(f));
      return static_cast<double>(f);
    }
    case 1: return static_cast<double>(bits & 0xFFu);
    case 2:
    case 5: return static_cast<double>(bits & 0xFFFFu);
    case 3: return static_cast<double>(static_cast<int32_t>(bits));
    // An unknown type is rendered as raw hex above, and there is no number that reading
    // means. Zero here is not a value -- the view never reaches it, because an attribute
    // whose type this build does not know is refused before the ramp is built.
    default: return 0.0;
  }
}

// Every component of one value, as `a, b, c`. An arity-1 attribute renders as one number with
// no punctuation, which is the common case and the one worth reading easily.
inline std::string FormatValues(const Attribute& a, const Value& v) {
  std::string out;
  for (size_t i = 0; i < v.bits.size(); ++i) {
    if (i) out += ", ";
    out += FormatValue(a.type, v.bits[i]);
  }
  return out.empty() ? std::string("(none)") : out;
}

// How an element identifies itself in a listing: its name when one is known, and always its
// hash, because the hash is what the registry is keyed by and the name is a convenience the
// offline path cannot supply.
inline std::string Describe(const Value& v) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%d", v.id_hash);
  if (v.element.empty()) return std::string(buf) + (v.in_table ? "" : " (not in table)");
  return v.element + " (" + buf + (v.in_table ? ")" : ", not in table)");
}

}  // namespace extattrs
}  // namespace simviz
