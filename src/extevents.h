// The reader for `/extevents`, and the decoder for the one stream whose records mean anything
// to this program.
//
// WHY THIS FILE EXISTS AT ALL, when `extprops.h` did not have to. The extension CELL properties
// arrive in the sim's own save-section format, and `extprops.h` is `oni-sim-replacement/sim/saveblob.h`
// -- the same header, decoding the same bytes with the same code, so there is no second parser
// to drift. Event streams have no such file to borrow: a stream is deliberately never
// serialised (not a checkpoint component, not a save section), so the wire format is defined by
// the route and this is the only reader of it. That is a cost, and it is why the layout below
// is spelled out beside the numbers rather than assumed.
//
// WHAT A BATCH IS. One stream, from one published frame. `count` and `dropped` describe THAT
// frame; two frames' records concatenated would lose the only thing that says when each
// happened. So a reply is a list of batches, not a list of records.
//
// THREE KINDS OF GAP, and the whole point of carrying all three: no batches means the game was
// quiet, a batch with `dropped > 0` means the PRODUCER overran its per-frame cap and that batch
// is a prefix, and `lost > 0` on the reply means the ROUTE overran its own buffer and whole
// frames are missing. A reader that folds these together is a reader that will report a silent
// simulation and a lossy pipe with the same words.
//
// DECODING IS BY NAME, NOT BY METADATA. `stride` says how big a record is and never what is in
// it. Everything here works on any stream -- enumerate, count, hand back bytes -- and only
// `DecodeRefusals` knows a record shape, because it tests the name first.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace simviz {
namespace extevents {

// oni-framework-api/OniFramework/DebugBulkRoutes.cs, `EventBlobMagic` and the block above it.
constexpr uint32_t kMagic = 0x56494E4Fu;  // "ONIV", little-endian
constexpr uint32_t kVersion = 1u;
constexpr int kHeaderBytes = 16;
constexpr int kRecordHeaderBytes = 68;

// The first declared stream, and the only record shape this program knows. Spelled here rather
// than included from the sim headers for the same reason `extprops.h` spells its scalar types:
// this binary is built against a corpus, not against the DLL.
constexpr char kStreamMessageRefused[] = "sim.message_refused";
constexpr int kRefusedRecordBytes = 16;

struct Batch {
  std::string name;
  int32_t tick = 0;
  int32_t stride = 0;
  int32_t count = 0;
  int32_t dropped = 0;  // records the SIM refused this frame at its own cap
  std::vector<uint8_t> bytes;
};

struct Reply {
  std::vector<Batch> batches;
  int32_t lost = 0;  // whole batches the ROUTE dropped since the last drain
};

// One `sim.message_refused` record: `oni_sim::ext::ExtRefusedMessage`.
struct Refusal {
  int32_t tick = 0;
  int32_t message_id = 0;
  int32_t reason = 0;
  int32_t payload_bytes = 0;
  int32_t expected_bytes = 0;
};

inline const char* ReasonName(int32_t reason) {
  switch (reason) {
    case 1: return "short payload";
    case 2: return "unknown message";
    case 3: return "bad target";
    default: return "unknown reason";
  }
}

inline uint32_t ReadU32(const uint8_t* p) {
  uint32_t v = 0;
  memcpy(&v, p, 4);
  return v;
}

inline int32_t ReadI32(const uint8_t* p) {
  int32_t v = 0;
  memcpy(&v, p, 4);
  return v;
}

inline int Align4(int n) { return (n + 3) & ~3; }

// Decodes a whole reply. Every bound is checked against the buffer that actually arrived rather
// than against the counts the buffer claims, because the counts are the untrusted part: a
// truncated body and a body that lies about `batchCount` are the same bug to a reader that
// trusts the header.
inline bool Parse(const std::vector<uint8_t>& blob, Reply* out, std::string* err) {
  *out = Reply();
  if (blob.size() < static_cast<size_t>(kHeaderBytes)) {
    *err = "event blob is " + std::to_string(blob.size()) + " bytes, shorter than its header";
    return false;
  }
  const uint32_t magic = ReadU32(blob.data());
  if (magic != kMagic) {
    char buf[64];
    snprintf(buf, sizeof(buf), "0x%08X", magic);
    *err = std::string("event blob magic is ") + buf + ", not \"ONIV\"";
    return false;
  }
  const uint32_t version = ReadU32(blob.data() + 4);
  if (version != kVersion) {
    *err = "event blob is version " + std::to_string(version) + ", this build reads " +
           std::to_string(kVersion);
    return false;
  }
  const int32_t batch_count = ReadI32(blob.data() + 8);
  out->lost = ReadI32(blob.data() + 12);
  if (batch_count < 0) {
    *err = "event blob declares " + std::to_string(batch_count) + " batches";
    return false;
  }

  size_t cursor = static_cast<size_t>(kHeaderBytes);
  for (int32_t i = 0; i < batch_count; ++i) {
    if (cursor + static_cast<size_t>(kRecordHeaderBytes) > blob.size()) {
      *err = "event blob ends inside batch " + std::to_string(i) + "'s header";
      return false;
    }
    const uint8_t* h = blob.data() + cursor;
    Batch b;
    // The name field is 48 bytes and NUL-terminated by the writer; a field with no terminator
    // is a malformed blob rather than a long name, because the sim caps a declared name at 47
    // characters precisely so it fits.
    const void* nul = memchr(h, 0, 48);
    if (nul == nullptr) {
      *err = "batch " + std::to_string(i) + " has an unterminated name field";
      return false;
    }
    b.name.assign(reinterpret_cast<const char*>(h),
                  static_cast<size_t>(static_cast<const uint8_t*>(nul) - h));
    b.tick = ReadI32(h + 48);
    b.stride = ReadI32(h + 52);
    b.count = ReadI32(h + 56);
    b.dropped = ReadI32(h + 60);
    const int32_t byte_count = ReadI32(h + 64);
    cursor += static_cast<size_t>(kRecordHeaderBytes);

    if (b.stride <= 0 || b.count < 0 || byte_count < 0) {
      *err = "batch " + std::to_string(i) + " (" + b.name + ") has stride " +
             std::to_string(b.stride) + ", count " + std::to_string(b.count) + ", byteCount " +
             std::to_string(byte_count);
      return false;
    }
    // The one derived quantity in the format, and it is checked rather than trusted: a
    // byteCount that is not count*stride means the writer and this reader disagree about the
    // record shape, and every offset after it would be wrong.
    if (byte_count != b.count * b.stride) {
      *err = "batch " + std::to_string(i) + " (" + b.name + ") says " +
             std::to_string(byte_count) + " bytes for " + std::to_string(b.count) +
             " records of " + std::to_string(b.stride);
      return false;
    }
    const size_t padded = static_cast<size_t>(Align4(byte_count));
    if (cursor + padded > blob.size()) {
      *err = "event blob ends inside batch " + std::to_string(i) + " (" + b.name + ")";
      return false;
    }
    b.bytes.assign(blob.begin() + static_cast<long>(cursor),
                   blob.begin() + static_cast<long>(cursor) + byte_count);
    cursor += padded;
    out->batches.push_back(std::move(b));
  }
  return true;
}

// Decodes the refusal stream's records out of whichever batches carry it. Batches of any other
// stream are skipped in silence -- not an error, because a reply may legitimately carry streams
// this build has no decoder for, and that is the format working rather than failing.
inline std::vector<Refusal> DecodeRefusals(const Reply& reply) {
  std::vector<Refusal> out;
  for (const Batch& b : reply.batches) {
    if (b.name != kStreamMessageRefused) continue;
    if (b.stride != kRefusedRecordBytes) continue;  // the record shape changed under us
    for (int32_t i = 0; i < b.count; ++i) {
      const uint8_t* r = b.bytes.data() + static_cast<size_t>(i) * kRefusedRecordBytes;
      Refusal f;
      f.tick = b.tick;
      f.message_id = ReadI32(r);
      f.reason = ReadI32(r + 4);
      f.payload_bytes = ReadI32(r + 8);
      f.expected_bytes = ReadI32(r + 12);
      out.push_back(f);
    }
  }
  return out;
}

// One line for a log pane or a terminal. `expectedBytes` is 0 when the reason is not about
// size, so it is printed only when it says something.
inline std::string Describe(const Refusal& f) {
  char buf[160];
  if (f.expected_bytes > 0) {
    snprintf(buf, sizeof(buf), "t%d  msg 0x%08X  %s  %d/%d bytes", f.tick,
             static_cast<unsigned>(f.message_id), ReasonName(f.reason), f.payload_bytes,
             f.expected_bytes);
  } else {
    snprintf(buf, sizeof(buf), "t%d  msg 0x%08X  %s  %d bytes", f.tick,
             static_cast<unsigned>(f.message_id), ReasonName(f.reason), f.payload_bytes);
  }
  return buf;
}

}  // namespace extevents
}  // namespace simviz
