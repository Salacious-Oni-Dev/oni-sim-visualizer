// Owning the simulation: bind SimDLL, replay a corpus into it, then drive ticks.
//
// This is `driver/src/replay.cpp` with the printing removed and a handle kept on the
// result. Nothing here is injected into Oxygen Not Included and the game does not need
// to be running -- SimDLL is a library, and this program is its host, exactly as
// `driver/`'s six tools already are.
//
// The corpus supplies both the element/disease tables and the world. Hand-building an
// element table is not an option: `driver/src/simhost.h` records that a synthetic
// six-element table makes PrepareGameData fault with a read at -1, and that the real
// tables have to be lifted from a capture.

#pragma once

#include <cerrno>
#include <cstring>

// FIRST, before <windows.h> anywhere in the translation unit: attach.h opens with
// <winsock2.h>, and windows.h drags in the original winsock.h unless winsock2.h has
// already defined _WINSOCKAPI_. Including them the other way round is a wall of
// redefinition errors, so the ordering is load-bearing and not alphabetical.
#include "out.h"
#include "attach.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "abi/sim_abi_ext.h"  // oni-sim-replacement root, on the include path
#include "simhost.h"          // oni-sim-replacement/driver/src, on the include path

#include "extevents.h"
#include "extprops.h"

namespace simviz {

using oni_sim::GameDataUpdate;
using oni_sim::NewGameFrame;
using oni_sim::SimMessageHash;

// Corpus record: int32 seq | int32 msg_id | int32 length | int32 count | length*count.
struct CorpusRecord {
  int32_t seq = 0, id = 0, length = 0, count = 0;
  std::vector<uint8_t> payload;
};

// `why`, when given, says which way it failed. Added after one run in about six of test.sh
// reported "could not read corpus" moments after LoadTables had opened the same file without
// complaint -- a failure with no reason attached is one nobody can chase.
inline bool ReadCorpus(const char* path, std::vector<CorpusRecord>* out, std::string* why = nullptr) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    if (why) *why = std::string("open failed: ") + strerror(errno);
    return false;
  }
  for (;;) {
    int32_t header[4];
    if (fread(header, sizeof(header), 1, f) != 1) break;
    CorpusRecord r;
    r.seq = header[0];
    r.id = header[1];
    r.length = header[2];
    r.count = header[3];
    const size_t bytes =
        static_cast<size_t>(r.length) * (r.count > 0 ? r.count : 1);
    r.payload.resize(bytes);
    if (bytes && fread(r.payload.data(), 1, bytes, f) != bytes) break;
    out->push_back(std::move(r));
  }
  const bool read_error = ferror(f) != 0;
  const int err = errno;
  fclose(f);
  if (out->empty() && why) {
    *why = read_error ? std::string("read failed: ") + strerror(err)
                      : std::string("no complete record in the file");
  }
  return !out->empty();
}

// The messages that populate the four component registries `AllocateCells` clears --
// `g->buildings`, `g->chunks`, `g->flow`, `g->radiation` (`sim/simdll.cpp`, the
// AllocateCells case) -- plus `RadiationParamsModification`, which sets five World scalars
// that the same Allocate resets to their defaults (`sim/world.h`, the block that assigns
// radiation_linger_rate_ and the four beside it).
//
// This is the list from `sim/simdll.cpp`'s own two switches (the queued-message filter and
// the ext_message disjunction), minus `SetDebugProperties`, which is not registration state.
// The three DiseaseConsumer hashes are deliberately absent: the sim accepts neither, because
// a disease consumer has only Register/Unregister and no per-tick update (`simdll.cpp:1309`),
// so there is nothing for a replay to restore.
//
// A save blob carries none of this. Klei's own `SaveBlob` is cells, and the game re-registers
// every building handle it holds after a load -- which is exactly what the visualizer cannot
// do, because it is not the game. Keeping the corpus's own records and re-sending them is the
// nearest thing available to the game's re-registration, and it is what makes a restored world
// the same world as the pristine boot rather than a world with no buildings in it.
inline bool IsRegistrationMessage(int32_t id) {
  switch (static_cast<SimMessageHash>(id)) {
    // g->buildings
    case SimMessageHash::AddBuildingHeatExchange:
    case SimMessageHash::ModifyBuildingHeatExchange:
    case SimMessageHash::ModifyBuildingEnergy:
    case SimMessageHash::RemoveBuildingHeatExchange:
    case SimMessageHash::AddBuildingToBuildingHeatExchange:
    case SimMessageHash::AddInContactBuildingToBuildingToBuildingHeatExchange:
    case SimMessageHash::RemoveBuildingInContactFromBuildingToBuildingHeatExchange:
    case SimMessageHash::RemoveBuildingToBuildingHeatExchange:
    // g->chunks
    case SimMessageHash::AddElementChunk:
    case SimMessageHash::MoveElementChunk:
    case SimMessageHash::SetElementChunkData:
    case SimMessageHash::ModifyElementChunkEnergy:
    case SimMessageHash::ModifyChunkTemperatureAdjuster:
    case SimMessageHash::RemoveElementChunk:
    // g->flow
    case SimMessageHash::AddElementConsumer:
    case SimMessageHash::SetElementConsumerData:
    case SimMessageHash::RemoveElementConsumer:
    case SimMessageHash::AddElementEmitter:
    case SimMessageHash::ModifyElementEmitter:
    case SimMessageHash::RemoveElementEmitter:
    case SimMessageHash::AddDiseaseEmitter:
    case SimMessageHash::ModifyDiseaseEmitter:
    case SimMessageHash::RemoveDiseaseEmitter:
    // g->radiation, and the five params Allocate resets alongside it
    case SimMessageHash::AddRadiationEmitter:
    case SimMessageHash::ModifyRadiationEmitter:
    case SimMessageHash::RemoveRadiationEmitter:
    case SimMessageHash::RadiationParamsModification:
      return true;
    default:
      return false;
  }
}

class Session {
 public:
  // Set before Boot to see the sim's own log lines; off by default so a snapshot run
  // produces only its own output.
  bool verbose = false;

  // Re-send the corpus's component registrations after every Restore that reallocates.
  // Off by default: it changes what a restored world contains, so it is measured with a
  // flag rather than turned on by assertion. See ReplayRegistrations below.
  bool replay_registrations = false;

  bool Boot(const char* dll, const char* corpus) {
    if (!simhost::Bind(dll)) return false;
    if (!simhost::LoadTables(corpus, &tables_)) {
      out::Printf("could not lift element/disease tables from %s\n", corpus);
      return false;
    }
    std::vector<CorpusRecord> records;
    std::string why;
    if (!ReadCorpus(corpus, &records, &why)) {
      out::Printf("could not read corpus %s (%s)\n", corpus, why.c_str());
      return false;
    }
    records_read_ = records.size();

    simhost::sim_initialize(verbose ? &simhost::DefaultMessageHandler : &SilentHandler);

    // The last record that (re)defines the world, and the last one that publishes a frame. A
    // corpus whose world was replaced after its last published frame boots onto a
    // GameDataUpdate that describes nothing: see boot_unpublished() below.
    size_t last_world = SIZE_MAX, last_frame = SIZE_MAX, at = 0;
    for (const CorpusRecord& r : records) {
      if (r.id == static_cast<int32_t>(SimMessageHash::AllocateCells) ||
          r.id == static_cast<int32_t>(SimMessageHash::SimData_InitializeFromCells) ||
          r.id == static_cast<int32_t>(SimMessageHash::Load)) {
        last_world = at;
      }
      ++at;
      // AllocateCells and SimData_InitializeFromCells both open with int32 width,
      // int32 height. A main-game load sends the former, a worldgen settle sim only
      // the latter, so take the dimensions from whichever arrives.
      if (r.id == static_cast<int32_t>(SimMessageHash::AllocateCells)) {
        wall_clock_seeded_ = true;
      }
      if ((r.id == static_cast<int32_t>(SimMessageHash::AllocateCells) ||
           r.id == static_cast<int32_t>(SimMessageHash::SimData_InitializeFromCells)) &&
          r.payload.size() >= 8) {
        memcpy(&width_, r.payload.data(), 4);
        memcpy(&height_, r.payload.data() + 4, 4);
      }
      // Kept before the send rather than after, so the retained copy is the record as the
      // corpus wrote it: no handler mutates the caller's buffer, but a record that the sim
      // refuses is still a record the game sent, and a replay that silently dropped it would
      // reproduce a different registration state than the boot did.
      if (IsRegistrationMessage(r.id)) registrations_.push_back(r);
      const uint8_t* p = r.payload.empty() ? nullptr : r.payload.data();
      void* result = (r.count > 1)
                         ? SendMany(r.id, r.length, r.count, p)
                         : simhost::sim_handle_message(r.id, r.length, p);
      if (r.id == static_cast<int32_t>(SimMessageHash::Start) ||
          r.id == static_cast<int32_t>(SimMessageHash::PrepareGameData)) {
        if (!result) {
          out::Printf("corpus record seq=%d id=%d returned null; sim refused it\n", r.seq,
                 r.id);
          return false;
        }
        latest_ = static_cast<const GameDataUpdate*>(result);
        last_frame = at - 1;
      }
    }
    boot_unpublished_ =
        last_world != SIZE_MAX && (last_frame == SIZE_MAX || last_frame < last_world);
    booted_ = latest_ != nullptr && width_ > 0 && height_ > 0;
    if (!booted_) {
      out::Printf("corpus replayed but produced no GameDataUpdate (%dx%d)\n", width_,
             height_);
    }
    return booted_;
  }

  // Pin the random stream (oni-sim-replacement ext message "ONI<"). Send it after Boot and
  // before the first Tick that should be reproducible. It is a queued message, so it is
  // drained at the top of the next frame, ahead of that frame's physics.
  //
  // Without it a LOADED world is not reproducible: AllocateCells seeds the stream from
  // the wall clock. See wall_clock_seeded().
  bool SetRandomState(uint32_t state) {
    if (!booted_) return false;
    // REFUSED rather than sent into the dark. SIM_HandleMessage answers nothing about a
    // message id it does not know -- Klei's own DLL drops ext::kSetRandomState on the floor
    // and returns exactly as it does for one it handled. This used to return true there, so
    // `--seed 7` against a stock SimDLL printed "pinned random stream to 0x00000007" over a
    // stream that was still seeded from the wall clock, and two such runs then diverged
    // while claiming to be pinned. Found by test.sh, which measured the divergence.
    //
    // The proxy is the DEBUG EXPORT, not the message: SIM_DebugRandomState and
    // kSetRandomState are both oni-sim-replacement's, neither is Klei's, and no DLL in this
    // project carries one without the other. Testing the export is possible; testing the
    // message is not, because a queued message cannot be read back before the next frame.
    if (!HasRandomState()) return false;
    oni_sim::ext::SetRandomStateMessage m{};
    m.state = state;
    simhost::sim_handle_message(oni_sim::ext::kSetRandomState, sizeof(m),
                                reinterpret_cast<const uint8_t*>(&m));
    pending_rng_ = state;
    rng_pending_ = true;
    return true;
  }

  // The random state as it WILL be when the next frame's physics runs. RandomState() reads the
  // sim's stream now, and straight after a SetRandomState -- every --seed, every timeline start
  // and every seek, since Restore sends one -- that is the transient state the Load or
  // AllocateCells left, which differs run to run (it is the wall clock). The message is queued
  // and lands ahead of the next frame's physics, so until a frame runs, the value about to be
  // in effect is the one that was sent. The HUD shows this; the sim never reads it.
  uint32_t ShownRandomState() const { return rng_pending_ ? pending_rng_ : RandomState(); }

  // Whether this DLL has the random-stream surface at all. Offline only: attached there is
  // no DLL in this process to ask.
  bool HasRandomState() const {
    if (attached_) return false;
    if (!debug_random_state_) {
      debug_random_state_ = reinterpret_cast<uint32_t (*)()>(
          GetProcAddress(simhost::g_sim, "SIM_DebugRandomState"));
    }
    return debug_random_state_ != nullptr;
  }

  // Re-send the corpus's component registrations, in the order the corpus sent them.
  //
  // `AllocateCells` clears `g->buildings`, `g->chunks`, `g->flow` and `g->radiation`, and no
  // save blob carries them back -- the GAME re-registers every handle it holds after a load,
  // and this program is not the game. Without this a restored world has no buildings, no
  // element chunks, no emitters and no consumers, which is why a restored run does not
  // reproduce a pristine run.
  //
  // Order matters and the whole subsequence is replayed, not just the Add records. `Move` and
  // `Set` carry where a chunk ended up and what it held; replaying only the Adds would put
  // every chunk back at the position it was first registered with. Handles line up because
  // `CompactedVector::Add` allocates them deterministically from a cleared table
  // (`sim/buildings.h`) -- slot 0 upward, versions at 0 -- so the handles baked into the
  // recorded `Modify` and `Remove` payloads still resolve.
  //
  // These are queued messages, so they drain at the top of the next frame, ahead of that
  // frame's physics, exactly like the extension messages Restore sends beside them.
  //
  // What this does NOT restore is the registries' evolving state. A chunk's temperature moves
  // every substep (`sim/chunks.h`, the ChunkExchange write-back), so a replay lands boot-era
  // temperatures on a world whose cells are at the checkpoint. That is why this is a flag and
  // not the default.
  size_t ReplayRegistrations() {
    if (!booted_) return 0;
    for (const CorpusRecord& r : registrations_) {
      const uint8_t* p = r.payload.empty() ? nullptr : r.payload.data();
      if (r.count > 1) {
        SendMany(r.id, r.length, r.count, p);
      } else {
        simhost::sim_handle_message(r.id, r.length, p);
      }
    }
    return registrations_.size();
  }

  // One sim step. dt is elapsed seconds; the game sends 0.2 and the sim turns that into
  // max(1, (int)((dt + carry) * 5.0)) substeps, so 0.2 is exactly one substep.
  bool Tick(float dt = 0.2f) {
    if (!booted_) return false;
    rng_pending_ = false;  // this frame drains the queued SetRandomState (ShownRandomState)
    NewGameFrame frame{};
    frame.elapsedSeconds = dt;
    frame.minX = 0;
    frame.minY = 0;
    frame.maxX = width_;
    frame.maxY = height_ - 1;
    frame.currentSunlightIntensity = 0.0f;
    frame.currentCosmicRadiationIntensity = 0.0f;
    simhost::sim_handle_message(
        static_cast<int32_t>(SimMessageHash::SimFrameManager_NewGameFrame),
        sizeof(NewGameFrame), reinterpret_cast<const uint8_t*>(&frame));
    // PrepareGameData's payload IS the visibility grid, one byte per cell
    // (sim/simdll.cpp:3002 -- "SimData::visibleGrid"; driver.cpp:413 sends the same
    // thing). Passing 0/nullptr is not "no opinion": simdll.cpp:3006 assigns from the
    // payload unconditionally, so an empty one CLEARS the grid and the sim then runs
    // with g->visible.data() == nullptr. Doing that was measured to make the whole run
    // non-deterministic -- 10 ticks on the reference corpus produced a different total
    // mass across invocations, spread over ~30 kg -- while driver.exe, which does send
    // the mask, is bit-stable over the same DLL. Everything visible, as driver.exe does.
    if (visible_.size() != static_cast<size_t>(cells())) {
      visible_.assign(static_cast<size_t>(cells()), 1);
    }
    void* upd = simhost::sim_handle_message(
        static_cast<int32_t>(SimMessageHash::PrepareGameData),
        static_cast<int>(visible_.size()), visible_.data());
    if (!upd) return false;
    latest_ = static_cast<const GameDataUpdate*>(upd);
    ++ticks_;
    return true;
  }

  // ------------------------------------------------------------ checkpoints
  //
  // A checkpoint is the save blob PLUS the random stream's position, because the blob
  // alone is not a complete state: `rng_` appears nowhere in `sim/saveblob.h`.
  // Restoring only the blob leaves the stream wherever the run
  // had pushed it, so re-simulating from the restore draws different numbers than the
  // original run did and the timeline changes under the scrubber.
  //
  // What makes this work at all is that `Load` restores in place: it decodes into the
  // existing world and does NOT go through `AllocateCells`, which is the message that
  // seeds from the wall clock (the AllocateCells case in sim/simdll.cpp vs the Load case below
  // it). A restore does not re-enter through AllocateCells and is not re-seeded; the handler
  // says so, and a restored run measured against a pristine one confirms it.
  struct Checkpoint {
    std::vector<uint8_t> blob;
    uint32_t rng = 0;
    // The SimData scheduling counters. The blob does not carry them and both AllocateCells
    // and Load reset them, so without these a restored world runs its physics in a
    // different order than the run it came from. That measurement is what put
    // ext::kSetSchedulingState into the sim.
    oni_sim::ext::SetSchedulingStateMessage sched{};
    bool sched_valid = false;
    // World::stable_ticks_, one byte a PADDED cell: how long each unstable solid has left
    // before it falls. No save format field holds it and every Allocate resets the array to
    // the reroll sentinel, so without this a restored world drops sand on a different tick
    // than the run did -- and draws from the random stream while doing it, since the reroll
    // is the only draw the unstable path makes. Measured without it:
    // 2,234 cells and +20.392861 kg one frame past a checkpoint, on a seed that has an
    // unstable solid in reach. Empty on a DLL without SIM_DebugStableTicks.
    std::vector<uint8_t> stable;
    // World::disease_accum_ and World::disease_infest_, the fifth thing no blob carries: the
    // growth remainder each cell has banked toward its next whole disease unit, and how long
    // it has been infested. Klei's SaveDisease is a hash and a count, so FromBlob zeroes both
    // on every load. Without them a replay runs its disease growth from zero while the run it
    // is replaying does not -- 26 of 35 replayed ticks wrong on the reference corpus, in the
    // disease arrays alone, with the random stream in step and the total mass bit-identical.
    // Invisible until the timeline's digest was widened past elementIdx/mass/temperature.
    // Empty on a DLL without SIM_DebugDiseaseGrowth.
    std::vector<float> disease_accum;
    std::vector<uint8_t> disease_infest;
    // The four component registries, whole: buildings, element chunks, the element/disease
    // flow components, the radiation emitters. The sixth thing, and the one that stops a
    // restored world from being a world with no colony in it. AllocateCells clears all four
    // and no blob carries any of them, and re-sending the corpus's own registration records
    // is NOT the same thing -- a registration is a handle on a body that then evolves every
    // substep, so replaying the boot records restores the topology at boot values and a
    // checkpoint comes back stale by its own age. The measurement that showed it had one
    // row that gives it away: a SHORTER checkpoint interval made it worse.
    // Opaque here on purpose -- the sim writes it and the sim parses it. Empty on a DLL
    // without SIM_DebugRegistryState.
    std::vector<uint8_t> registry;
    // The per-cell extension properties a save blob does not carry: the kRehydrated ones,
    // whose owner promised to re-push them after every load, and the kCheckpointOnly ones,
    // which are an explicit statement that nobody will. The SEVENTH thing
    // (ext::kSetExtCellState / SIM_DebugExtCellState).
    //
    // THIS PROGRAM IS THE REASON IT EXISTS. A mod re-pushes its own kRehydrated properties
    // after a load and never notices the gap; a tool driving the DLL has no mods loaded, so
    // there is no owner present to keep that promise and `kRehydrated` is indistinguishable
    // from "lost". And the values are not inert -- the sim's conduction kernel binds the
    // thermal-mass bonus inside its pair loop -- so an absent property does not read as
    // missing, it silently produces a different temperature. That is the divergence signature
    // all six components above were found by.
    //
    // Opaque here, like `registry`: the sim writes it and the sim parses it. Empty on a DLL
    // without SIM_DebugExtCellState. Never empty on one WITH it -- the export always returns
    // at least a header, precisely so this field can tell the two cases apart.
    std::vector<uint8_t> ext_cells;
    // World::radiation_, whole: the EIGHTH thing (ext::kSetCellRadiation). The blob does carry
    // radiation, but the sim's Load clears it in every Vacuum and Void cell, as Klei's does, and
    // a running world has radiation there. Measured: once the sim took on that
    // rewrite, --timeline-test 30 failed 2 of 4 exact stops and 17 of 26 replayed ticks in the
    // radiation array alone. Empty on a DLL without SIM_DebugCellRadiation.
    std::vector<float> radiation;
    // The game's visibility mask, three buffers deep: the NINTH thing
    // (ext::kSetVisibilityState). A frame reads the mask sent two PrepareGameData calls earlier
    // and falling liquid refuses a cell nobody can see; AllocateCells and Load zero all three
    // buffers, so without this a replay's first frames refuse the hand-offs its run made.
    // Measured on a DLL without it: --timeline-test 30 failed 17 of 26 replayed ticks, up to
    // 1,114 kg heavier. Opaque, like `registry`: the getter's output is the
    // message's payload. Empty on a DLL without SIM_DebugVisibilityState.
    std::vector<uint8_t> visibility;
    int ticks = 0;
    bool valid() const { return !blob.empty(); }
    size_t bytes() const {
      return blob.size() + stable.size() + disease_accum.size() * sizeof(float) +
             disease_infest.size() + registry.size() + ext_cells.size() +
             radiation.size() * sizeof(float) + visibility.size();
    }
  };

  // x and y are the blob's recorded world offset only -- ToBlob always writes the whole
  // allocated grid (sim/world.h) -- so 0,0 is right for a single-world capture.
  Checkpoint Capture() const {
    Checkpoint cp;
    if (!booted_) return cp;
    cp.blob = simhost::Save(0, 0);
    cp.rng = RandomState();
    cp.sched_valid = SchedulingState(&cp.sched);
    cp.stable = StableTicks();
    DiseaseGrowth(&cp.disease_accum, &cp.disease_infest);
    cp.registry = RegistryState();
    cp.ext_cells = ExtCellState();
    CellRadiation(&cp.radiation);
    cp.visibility = VisibilityState();
    cp.ticks = ticks_;
    return cp;
  }

  // `realloc_first` sends AllocateCells before the blob, which is the order the GAME
  // uses: a save load is always Alloc-then-Load, never a bare Load into a world that has
  // been running. AllocateCells is also the only message that resets the SimData
  // scheduling state the sim documents as surviving a load -- displace_rotation to 0,
  // pressure_dir to -1 -- and it re-seeds the stream, which is why SetRandomState below
  // has to come after it rather than before.
  //
  // The cost is that it also clears buildings, chunks, flow and radiation registrations.
  // For a corpus the visualizer replayed once that is acceptable; for a live attach it
  // would not be.
  bool Restore(const Checkpoint& cp, bool realloc_first = true) {
    if (!booted_ || !cp.valid()) return false;
    if (realloc_first) {
      int32_t dims[2] = {width_, height_};
      simhost::sim_handle_message(static_cast<int32_t>(SimMessageHash::AllocateCells),
                                  sizeof(dims),
                                  reinterpret_cast<const uint8_t*>(dims));
    }
    // BEFORE the Load, and the only restore message that is. Every Load ends with Klei's
    // load-time state transition, which moves a cell outside its element's range one step --
    // right for a save, wrong for a checkpoint, because a run publishes such cells (a
    // supercooled drop that landed this substep freezes on the next). Measured on a live
    // corpus: the tick-21 exact stop restored two cells of liquid CO2 as solid 1.5 K warmer,
    // and 8 of 26 replayed ticks diverged from there. ext::kSetLoadIsRestore makes this one
    // Load skip that pass; the sim clears it again whatever the Load does. A DLL that does not
    // publish it gets nothing sent, and a restore through it keeps that divergence.
    if (RestoreModeSupported()) {
      oni_sim::ext::SetLoadIsRestoreMessage m{};
      m.restore = 1;
      simhost::sim_handle_message(oni_sim::ext::kSetLoadIsRestore, static_cast<int>(sizeof(m)),
                                  reinterpret_cast<const uint8_t*>(&m));
    }
    void* result = simhost::sim_handle_message(
        static_cast<int32_t>(SimMessageHash::Load), static_cast<int>(cp.blob.size()),
        cp.blob.data());
    if (!result) return false;  // the sim turns a rejected blob into a null return
    latest_ = static_cast<const GameDataUpdate*>(result);
    // Only on the realloc path, and that is not a detail: a bare Load clears nothing, so the
    // registries still hold everything the boot put in them. Re-sending here would register
    // every building and chunk a SECOND time, doubling the colony's heat exchange rather than
    // restoring it.
    //
    // And only when the checkpoint does NOT carry the registries, for the same reason at one
    // remove. Both land as queued messages on the same frame, and the sim drains the component
    // registrations AFTER ext::kSetRegistryState -- so the replayed Adds would pile the boot
    // colony on top of the restored one. Measured rather than reasoned about: running both at
    // once fails with the registry digest differing while mass and the random stream are
    // identical, which is what a doubled registry looks like before it has moved any heat.
    // The flag is the fallback for a DLL without SIM_DebugRegistryState, not a second opinion.
    if (realloc_first && replay_registrations && cp.registry.empty()) ReplayRegistrations();
    if (!SetRandomState(cp.rng)) return false;
    // Sent verbatim, skip included. Measured with --timeline-diff, which reads the counters
    // back after each frame of a restore: the message lands on the FIRST frame after this
    // one, ahead of that frame's physics (+0 frames reads skip 1 and rotation 0, straight
    // out of the Load; +1 reads skip 0 and the rotation one past the captured value). So
    // the captured skip of 0 overwrites the skip the Load inserted and that frame steps
    // physics, which is what makes `restore + 1 frame` the tick after the checkpoint's.
    // Forcing skip = 1 here instead would make that frame skip and drop one physics frame
    // out of every replay -- which is exactly what an earlier cut of this did, and it cost
    // 26 of 35 replayed ticks.
    if (cp.sched_valid && !SetSchedulingState(cp.sched)) return false;
    // Same position, same reason, and after the Load for the same reason: it is sized
    // against the world that is loaded when it drains.
    if (!cp.stable.empty() && !SetStableTicks(cp.stable)) return false;
    // Same position and the same reason again: queued, and sized against the world that is
    // loaded when it drains.
    if (!cp.disease_accum.empty() &&
        !SetDiseaseGrowth(cp.disease_accum, cp.disease_infest)) {
      return false;
    }
    // Last, and only on the realloc path for the same reason ReplayRegistrations is: a bare
    // Load clears nothing, so the registries still hold what the run put in them and writing
    // over them with a checkpoint's copy would be a change rather than a restore.
    if (realloc_first && !cp.registry.empty() && !SetRegistryState(cp.registry)) return false;
    // NOT gated on `realloc_first`, and that asymmetry is the whole point rather than an
    // oversight. The registries survive a bare Load because only AllocateCells clears them --
    // but `World::FromBlob` calls `Allocate` itself, so EVERY load resizes and re-defaults
    // every extension property, realloc path or not. Gating this the way the registries are
    // gated would leave the bare-Load path silently restoring six components out of seven,
    // which is exactly the shape of the scheduling and registry gaps described above.
    //
    // After the Load for the same reason the three above are: it is sized against the world
    // that is loaded when it drains.
    if (!cp.ext_cells.empty() && !SetExtCellState(cp.ext_cells)) return false;
    // Not gated on `realloc_first` either: it is Load itself that clears the radiation, on
    // either path.
    if (!cp.radiation.empty() && !SetCellRadiation(cp.radiation)) return false;
    // Not gated either, and immediate like radiation: every AllocateCells and Load zeroes the
    // mask, and the next PrepareGameData writes into one of the buffers this restores.
    if (!cp.visibility.empty() && !SetVisibilityState(cp.visibility)) return false;
    ticks_ = cp.ticks;
    return true;
  }

  // Republishes the cell arrays without advancing physics: a zero-length frame. After a
  // Restore the world is correct but the GameDataUpdate the visualizer reads is still
  // the one from before the restore, so a seek that replays no ticks would draw and
  // digest the state it came from. dt = 0 accumulates no substeps, so displace_rotation
  // and pressure_dir (sim/simdll.cpp:2051, 2255 -- both inside the substep loop) do not
  // move, which is what makes this a publish rather than a step.
  bool Publish() {
    const int t = ticks_;
    const bool ok = Tick(0.0f);
    ticks_ = t;
    return ok;
  }

  void Shutdown() {
    if (simhost::sim_shutdown) simhost::sim_shutdown();
    booted_ = false;
    latest_ = nullptr;
  }

  // True when the world arrived through AllocateCells, which is how a main-game LOAD
  // boots. That path seeds the random stream from the wall clock rather than from the
  // world seed (sim/simdll.cpp:2827), so the same corpus replayed twice shuffles its
  // gas differently and the run is NOT reproducible. It is faithful Klei behaviour and
  // deliberately not "fixed" -- see the comment at that line -- but it is fatal to any
  // check that compares two runs, so it is reported rather than left to be discovered.
  // A worldgen corpus, which boots through SimData_InitializeFromCells, carries a real
  // world seed and IS reproducible.
  bool wall_clock_seeded() const { return wall_clock_seeded_; }
  // True when the corpus's last world-defining record (AllocateCells, a worldgen
  // SimData_InitializeFromCells, or Load) is not followed by any frame. The frame this
  // session then holds was published for an EARLIER world, so until one tick has run its
  // arrays describe nothing: measured on a corpus recorded from a new game, where the shim
  // captured the worldgen session's boot and the colony's Load arrived only as a later
  // sample -- at tick 0, 144,832 of 256,944 cells held element indices past the table and
  // 28,818 a NaN mass; after one tick, none. The corpus records the first sim session of the
  // process, so a corpus meant to show a colony has to be recorded by loading its save as
  // the first world after the game starts.
  bool boot_unpublished() const { return boot_unpublished_; }

  // The sim's live LCG state. Not part of Klei's ABI; oni-sim-replacement exports it for
  // exactly this kind of question. Zero if the DLL does not have it.
  uint32_t RandomState() const {
    // There is no sim to ask when the world is arriving over a socket, and no managed
    // route exposes the game's stream position either. Zero, and the HUD says so.
    if (!HasRandomState()) return 0u;
    return debug_random_state_();
  }

  // The getter half of ext::kSetSchedulingState. Resolved here rather than through
  // simhost.h for the same reason SIM_HandleMessages and SIM_DebugRandomState are: the
  // shared header binds Klei's ABI, and this is not part of it.
  bool SchedulingState(oni_sim::ext::SetSchedulingStateMessage* out) const {
    if (!debug_sched_state_) {
      debug_sched_state_ = reinterpret_cast<bool (*)(oni_sim::ext::SetSchedulingStateMessage*)>(
          GetProcAddress(simhost::g_sim, "SIM_DebugSchedulingState"));
    }
    return debug_sched_state_ ? debug_sched_state_(out) : false;
  }

  // Queued, like every extension message: it lands at the top of the next frame, ahead of
  // that frame's physics, which is exactly the frame that has to run under it.
  bool SetSchedulingState(const oni_sim::ext::SetSchedulingStateMessage& m) {
    if (!booted_) return false;
    simhost::sim_handle_message(oni_sim::ext::kSetSchedulingState, sizeof(m),
                                reinterpret_cast<const uint8_t*>(&m));
    return true;
  }

  // The getter half of ext::kSetStableTicks. Sized by the DLL rather than derived here:
  // the array is one byte per PADDED cell, and padding is the sim's business, not the
  // host's. Empty on a DLL that does not export it, which is what makes carrying the
  // countdowns optional the same way carrying the scheduling counters is.
  std::vector<uint8_t> StableTicks() const {
    if (!debug_stable_ticks_) {
      debug_stable_ticks_ = reinterpret_cast<int32_t (*)(uint8_t*, int32_t)>(
          GetProcAddress(simhost::g_sim, "SIM_DebugStableTicks"));
    }
    if (!debug_stable_ticks_) return {};
    const int32_t n = debug_stable_ticks_(nullptr, 0);
    if (n <= 0) return {};
    std::vector<uint8_t> v(static_cast<size_t>(n));
    if (debug_stable_ticks_(v.data(), n) != n) return {};
    return v;
  }

  // The only variable-length message the sim takes: a 4-byte count followed by that many
  // bytes. Built into one buffer because SIM_HandleMessage takes one pointer and one
  // length, and the sim checks both halves against each other on the way in.
  bool SetStableTicks(const std::vector<uint8_t>& ticks) {
    if (!booted_ || ticks.empty()) return false;
    oni_sim::ext::SetStableTicksMessage head{};
    head.count = static_cast<int32_t>(ticks.size());
    std::vector<uint8_t> payload(sizeof(head) + ticks.size());
    memcpy(payload.data(), &head, sizeof(head));
    memcpy(payload.data() + sizeof(head), ticks.data(), ticks.size());
    simhost::sim_handle_message(oni_sim::ext::kSetStableTicks,
                                static_cast<int>(payload.size()), payload.data());
    return true;
  }

  // The getter half of ext::kSetRegistryState: all four component registries -- buildings,
  // element chunks, the element/disease flow components, the radiation emitters -- as one
  // opaque blob the sim serializes and the sim parses. Nothing here reads inside it, and that
  // is the point: the alternative shape had this program recomputing chunk heat capacities
  // and building registration fields out of the element table to rebuild the registries with
  // vanilla messages, which is mirroring the sim's bookkeeping in the tool that is supposed
  // to be checking the sim.
  //
  // The size is not derivable from the world's dimensions -- it depends on how much the game
  // registered -- so the first call is the only way to learn it. Empty on a DLL without the
  // export, which is what keeps carrying the registries optional the way the other components
  // are.
  std::vector<uint8_t> RegistryState() const {
    if (!debug_registry_state_) {
      debug_registry_state_ = reinterpret_cast<int32_t (*)(uint8_t*, int32_t)>(
          GetProcAddress(simhost::g_sim, "SIM_DebugRegistryState"));
    }
    if (!debug_registry_state_) return {};
    const int32_t n = debug_registry_state_(nullptr, 0);
    if (n <= 0) return {};
    std::vector<uint8_t> v(static_cast<size_t>(n));
    if (debug_registry_state_(v.data(), n) != n) return {};
    return v;
  }

  // The payload IS the blob -- no header struct in front of it, because the blob carries its
  // own magic, version and lengths and the sim validates all of them before it moves anything
  // into place. Queued like every extension message.
  bool SetRegistryState(const std::vector<uint8_t>& blob) {
    if (!booted_ || blob.empty()) return false;
    simhost::sim_handle_message(oni_sim::ext::kSetRegistryState,
                                static_cast<int>(blob.size()), blob.data());
    return true;
  }

  // The getter half of ext::kSetExtCellState: the per-cell extension properties no save blob
  // carries, as one opaque blob the sim serializes and the sim parses. Same two-call sizing as
  // RegistryState above, and for the same reason -- the length depends on what has been
  // registered and written, not on the world's dimensions.
  //
  // A DLL that HAS the export never returns zero: a world with nothing to carry still hands
  // back a header. So an empty vector here means "this DLL predates the seventh component",
  // which is a different thing from "there was nothing to carry" and has to stay
  // distinguishable -- a caller that conflated them would silently carry six.
  std::vector<uint8_t> ExtCellState() const {
    if (!debug_ext_cell_state_) {
      debug_ext_cell_state_ = reinterpret_cast<int32_t (*)(uint8_t*, int32_t)>(
          GetProcAddress(simhost::g_sim, "SIM_DebugExtCellState"));
    }
    if (!debug_ext_cell_state_) return {};
    const int32_t n = debug_ext_cell_state_(nullptr, 0);
    if (n <= 0) return {};
    std::vector<uint8_t> v(static_cast<size_t>(n));
    if (debug_ext_cell_state_(v.data(), n) != n) return {};
    return v;
  }

  // EVERY registered extension property, all three persistence classes -- the inspection
  // twin of the export above, and read-only: there is no message that accepts this blob, and
  // `ext_state::Load` refuses a kSaved record by name, so it cannot be applied as a
  // checkpoint even by mistake. Empty on a DLL that predates it, which is why the caller
  // falls back rather than treating that as an error.
  std::vector<uint8_t> ExtCellStateAll() const {
    if (!debug_ext_cell_state_all_) {
      debug_ext_cell_state_all_ = reinterpret_cast<int32_t (*)(uint8_t*, int32_t)>(
          GetProcAddress(simhost::g_sim, "SIM_DebugExtCellStateAll"));
    }
    if (!debug_ext_cell_state_all_) return {};
    const int32_t n = debug_ext_cell_state_all_(nullptr, 0);
    if (n <= 0) return {};
    std::vector<uint8_t> v(static_cast<size_t>(n));
    if (debug_ext_cell_state_all_(v.data(), n) != n) return {};
    return v;
  }

  // The payload IS the blob, exactly as SetRegistryState works. Applied whole or not at all by
  // the sim, which checks its own magic, version and record shapes first.
  bool SetExtCellState(const std::vector<uint8_t>& blob) {
    if (!booted_ || blob.empty()) return false;
    simhost::sim_handle_message(oni_sim::ext::kSetExtCellState,
                                static_cast<int>(blob.size()), blob.data());
    return true;
  }

  // The same blob, DECODED, for everything that wants to look at a property rather than
  // carry one. `Checkpoint` keeps treating it as opaque bytes on purpose -- a checkpoint
  // must round-trip a property this build knows nothing about -- so this is a second,
  // read-only view of the same export and not a replacement for that one.
  //
  // REFRESHED EXPLICITLY, never on demand from an accessor. The export copies every
  // written property out of the sim (about 1 MB on the reference corpus once --ext-probe
  // has run), so the window pulls it when the displayed frame changes and not once per
  // redraw, and a const readout like `SampleCell` reads whatever the last refresh left.
  // That is why `ExtProps` is const and this is not.
  const extprops::Table& ExtProps() const { return ext_props_; }

  // WHICH extension property the ext view is showing. It lives here, on the object every
  // renderer already takes, rather than inside `View` -- the enum names the eight KINDS of
  // view, and the set of extension properties is the sim's to decide and can change without
  // this program being rebuilt. Set from --view ext:<name>[c]; empty otherwise.
  void SetExtView(const std::string& name, int component) {
    ext_view_ = name;
    ext_view_comp_ = component;
  }
  const std::string& ext_view_name() const { return ext_view_; }
  int ext_view_comp() const { return ext_view_comp_; }

  // ------------------------------------------------------- registry 2, as a picture
  //
  // WHICH per-element attribute the attr view is showing, and the same argument as
  // `SetExtView` for why it lives here: the enum names the KINDS of view, and the set of
  // attributes is the sim's to decide. Set from --view attr:<name>[c]; empty otherwise.
  //
  // The difference from registry 1 is what the value is a property OF. An extension
  // property has a value per CELL, so the ext view is a straight read of a per-cell array.
  // An attribute has a value per ELEMENT, so this view is that value looked up through the
  // cell's element -- one indirection, resolved once per refresh into `attr_value_` rather
  // than walked per cell. What it draws is therefore never finer than the element map, and
  // that is the honest picture: the data really is that coarse.
  void SetAttrView(const std::string& name, int component) {
    attr_view_ = name;
    attr_view_comp_ = component;
    ResolveAttrView();
  }
  const std::string& attr_view_name() const { return attr_view_; }
  int attr_view_comp() const { return attr_view_comp_; }

  // The cached registry-2 table. Refreshed EXPLICITLY, exactly as `ExtProps` is and for a
  // related reason: attached, reading it is an HTTP round trip to the game, and a const
  // accessor that made one per redraw would put the game's frame time in this program's
  // paint loop. `ElementAttributes()` above is the fetch; this is the last result of one.
  const extattrs::Table& ExtAttrs() const { return ext_attrs_; }

  // Unlike `RefreshExtProps` there is no --ext gate on the attached path, and the asymmetry
  // is a cost difference rather than an oversight: /extcells makes the game wait for its sim
  // thread and serialise every written property -- 12.7 MB on the reference world -- while
  // /extelements serialises a registry the ABI caps at 64 attributes, each held by a handful
  // of elements. A few kilobytes of JSON does not need to be asked for twice.
  bool RefreshExtAttrs() {
    ext_attrs_ = ElementAttributes(/*with_values=*/true);
    ResolveAttrView();
    return ext_attrs_.exported;
  }

  // The selected attribute for one ELEMENT-TABLE ROW, which is what a cell's `elementIdx`
  // is. `AttrHas` false means that element carries no value, which is a REAL ANSWER and not
  // a zero: extattrs.h's header opens on why. The attr view draws it as backdrop for that
  // reason, and this is the only place the two can be told apart.
  bool AttrHas(uint16_t idx) const {
    return idx < attr_has_.size() && attr_has_[idx] != 0;
  }
  float AttrAt(uint16_t idx) const {
    return idx < attr_value_.size() ? attr_value_[idx] : 0.0f;
  }
  // Elements of the CURRENT table carrying a value for the selected attribute and component.
  int attr_elements() const { return attr_elements_; }
  // Values held for an id hash this table has no row for. Not an error -- keys are ids so a
  // value survives a table reload -- but no cell can ever show one, so a view that is all
  // backdrop with `attr_off_table() > 0` has a different explanation from one without.
  int attr_off_table() const { return attr_off_table_; }
  // The span of the values that are reachable. The attr view ranges over THESE and not over
  // the cells; views.h's `ScanRange` says why a percentile over cells is the wrong band here.
  float attr_lo() const { return attr_lo_; }
  float attr_hi() const { return attr_hi_; }

  // Whether an ATTACHED session should pull `/extcells` when it refreshes. Off by default,
  // and there is no equivalent switch offline, because the two paths cost different things:
  // offline the export is a memcpy inside this process, and attached it makes the game wait
  // for its sim thread and serialise every written property -- about 13 MB on a world with an
  // active gas mixture, on top of the frame `/grid` already moves. So the live readout is
  // asked for (`--ext`) rather than assumed, and the flag is implied by every switch that
  // cannot work without it.
  void set_ext_live(bool on) { ext_live_ = on; }
  bool ext_live() const { return ext_live_; }

  // Take the extension registry off the PUBLISHED path instead of the inspection export --
  // `/extcells?published=1`. Attached only; offline there is
  // no barrier to avoid, because the export is a memcpy inside this process.
  //
  // `props` is a comma-separated list of property names, or empty to mean "whatever the game
  // is already publishing". Empty is the safe default in both directions: it subscribes
  // nothing, so a bare `--ext-published` cannot make the game start copying the world, and it
  // still shows what another mod has subscribed for its own reasons.
  //
  // Implies `ext_live`. The two flags name the same appetite -- "fetch extension data" -- and
  // differ only in which route answers, so making the caller pass both would be a way to get
  // a silently empty readout.
  void set_ext_published(bool on, const std::string& props) {
    ext_published_ = on;
    ext_published_props_ = props;
    if (on) ext_live_ = true;
  }
  bool ext_published() const { return ext_published_; }
  const std::string& ext_published_props() const { return ext_published_props_; }

  // What `props=` should carry when the caller named nothing: the property being VIEWED, if
  // one is. A session rendering `--view ext:sim.gas_mass` wants that property published and
  // wants nothing else copied every frame, and making the caller repeat the name on a second
  // flag would be a way to typo one of them.
  std::string ExtPublishedQuery() const {
    if (!ext_published_props_.empty()) return ext_published_props_;
    return ext_view_;
  }

  // The raw blob the next refresh would read, whichever path the Session is on: the
  // inspection export offline, `/extcells` attached, and empty when neither can answer.
  //
  // Exists for --ext-blob. A stand-in for the game (tools/attach-stub.py) has to serve the
  // sim's OWN bytes back, because a fixture re-encoded by this program's reader would be
  // testing the reader against itself and would agree with any bug it has.
  std::vector<uint8_t> ExtCellStateBlob() const {
    if (attached_) {
      if (!ext_live_) return {};
      std::vector<uint8_t> blob;
      std::string err;
      if (ext_published_) {
        if (!FetchPublishedBlob(&blob, &err)) return {};
        return blob;
      }
      if (!attach_.FetchExtCells(&blob, &err)) return {};
      return blob;
    }
    std::vector<uint8_t> blob = ExtCellStateAll();
    if (!blob.empty()) return blob;
    return ExtCellState();
  }

  // One published fetch, with the retry the subscription makes necessary.
  //
  // A property nobody has asked for is in no frame yet, so the route's first answer to a first
  // request is "subscribed, ask again next tick" -- `kPublishCellProperty` is queued, like
  // every other cell message. Treating that as a failure would mean a one-shot `--snapshot`
  // could never succeed on the property it was pointed at, so this waits instead.
  //
  // Bounded, and bounded by TIME rather than by ticks, because the thing being waited on is a
  // sim that may be paused: a game sitting on a pause screen publishes nothing and would
  // otherwise hold this here forever. Two seconds is about ten ticks at the sim's usual rate,
  // and a run that has not been served by then gets the route's own sentence back rather than
  // a timeout of this program's invention.
  bool FetchPublishedBlob(std::vector<uint8_t>* blob, std::string* err) const {
    const std::string props = ExtPublishedQuery();
    for (int attempt = 0; attempt < kPublishedRetries; ++attempt) {
      bool pending = false;
      if (attach_.FetchPublishedExtCells(props, blob, &pending, err)) return true;
      if (!pending) return false;
      Sleep(kPublishedRetryMs);
    }
    *err = "the game subscribed " + (props.empty() ? std::string("the requested properties")
                                                   : props) +
           " but had not published them " +
           std::to_string(kPublishedRetries * kPublishedRetryMs / 1000) +
           "s later -- is the sim paused?";
    return false;
  }

  static constexpr int kPublishedRetries = 20;
  static constexpr int kPublishedRetryMs = 100;

  // ---------------------------------------------------------------------- event streams

  // Which streams the Session watches, as a comma-separated list for the route. Empty means
  // "watch nothing", and that is the default in both directions for the same reason
  // `ext_published_props_` is: subscribing is something the GAME pays for, so it happens
  // because a flag asked, never because a program was started.
  void set_event_streams(const std::string& streams) { event_streams_ = streams; }
  const std::string& event_streams() const { return event_streams_; }
  bool watching_events() const { return !event_streams_.empty(); }

  // The log, oldest first. Kept in the Session rather than in the pane that draws it, because
  // the pane is redrawn on every frame and the log is not: a refusal is a fact about a tick
  // that happened, and a readout that only showed what arrived since the last repaint would
  // show an empty pane to anyone who blinked.
  const std::vector<extevents::Refusal>& refusals() const { return refusals_; }
  int32_t events_lost() const { return events_lost_; }
  int32_t events_dropped() const { return events_dropped_; }
  const std::string& events_error() const { return events_error_; }

  // How many refusals the log holds before it starts forgetting. A cap this program can hit is
  // better than one it cannot: the pane shows the newest and a run left overnight against a
  // misbehaving mod should not grow without bound. Dropping the OLDEST matches the route's own
  // ring for the same reason -- what a watcher wants is what is happening now, and the count
  // of what fell off is carried separately so the gap is never silent.
  static constexpr size_t kMaxLoggedRefusals = 2000;

  // One drain. Returns false only for a real failure -- a reply with no batches is success and
  // the ordinary case, because the stream's whole subject is things that usually do not happen.
  //
  // The subscribe-and-retry is NOT waited out here, unlike `FetchPublishedBlob`. That one is
  // called from a one-shot path where returning empty means the snapshot fails; this one is
  // called every frame from a live session, so the honest answer to "not collecting yet" is to
  // say so and try again on the next frame that comes anyway. Blocking the render loop for two
  // seconds to avoid one empty pane would be the worse trade.
  bool PollEvents() {
    events_error_.clear();
    if (!attached_ || event_streams_.empty()) return false;
    std::vector<uint8_t> blob;
    bool pending = false;
    std::string err;
    if (!attach_.FetchExtEvents(event_streams_, &blob, &pending, &err)) {
      events_error_ = pending ? "subscribing; collecting from the next tick" : err;
      return false;
    }
    extevents::Reply reply;
    if (!extevents::Parse(blob, &reply, &err)) {
      events_error_ = err;
      return false;
    }
    events_lost_ += reply.lost;
    for (const extevents::Batch& b : reply.batches) events_dropped_ += b.dropped;
    const std::vector<extevents::Refusal> decoded = extevents::DecodeRefusals(reply);
    refusals_.insert(refusals_.end(), decoded.begin(), decoded.end());
    if (refusals_.size() > kMaxLoggedRefusals) {
      const size_t excess = refusals_.size() - kMaxLoggedRefusals;
      refusals_.erase(refusals_.begin(), refusals_.begin() + static_cast<long>(excess));
      events_forgotten_ += static_cast<int32_t>(excess);
    }
    return true;
  }

  int32_t events_forgotten() const { return events_forgotten_; }

  bool RefreshExtProps() {
    if (attached_) {
      ext_props_ = extprops::Table();
      if (!ext_live_) {
        ext_props_.error =
            "attached: extension properties are not being fetched. /extcells costs the game "
            "a sim-thread wait and a full serialisation, so it is opt-in -- pass --ext";
        return false;
      }
      // The route hands over exactly what SIM_DebugExtCellStateAll produced, so this is the
      // same blob the offline path decodes and `all_classes` is true for the same reason.
      std::vector<uint8_t> blob;
      std::string err;
      const bool ok = ext_published_ ? FetchPublishedBlob(&blob, &err)
                                     : attach_.FetchExtCells(&blob, &err);
      if (!ok) {
        ext_props_.error = err;
        return false;
      }
      const std::string source =
          ext_published_ ? "/extcells?published=1" : "/extcells";
      // `all_classes` is true on BOTH routes and for the same reason: neither is a checkpoint.
      // The published blob carries whatever is subscribed regardless of persistence class,
      // exactly as the inspection blob carries whatever is written regardless of it.
      const bool decoded =
          extprops::Decode(blob, width_, height_, /*all_classes=*/true, &ext_props_);
      ext_props_.source = source;
      return decoded;
    }
    // The inspection export first, the checkpoint's as a fallback. They differ by exactly the
    // kSaved class -- the gas mixture and the promoted-room flag, which is to say everything
    // the sim itself writes -- so an older DLL still gives a readout, with those missing and
    // with `all_classes` false so every listing can say which it is looking at.
    std::vector<uint8_t> blob = ExtCellStateAll();
    if (!blob.empty()) {
      const bool decoded =
          extprops::Decode(blob, width_, height_, /*all_classes=*/true, &ext_props_);
      ext_props_.source = "SIM_DebugExtCellStateAll";
      return decoded;
    }
    const bool decoded = extprops::Decode(ExtCellState(), width_, height_,
                                          /*all_classes=*/false, &ext_props_);
    ext_props_.source = "SIM_DebugExtCellState";
    return decoded;
  }

  // -------------------------------------------------------- the gas mixture
  //
  // Moves part of every gas cell in a rectangle out of vanilla's single-element PhaseEntry
  // and into the volume-fractions mixture layer (oni-sim-replacement's gas_mixture.h and
  // gas_rooms.h). It exists for one reason: `vf_active` is set only by a world that has
  // been sent ext::kInjectGasSpecies, or that loads a blob already carrying mixture data
  // (sim/simdll.cpp, ActivateVolumeFractions). With no way to send that message, the
  // mixing kernels -- and every piece of state they own, including World::gas_sleeping_
  // and the sim's own per-cell sleep counter, neither of which a save blob carries -- were
  // unreachable from this program and therefore untested by the timeline.
  //
  // Conserving by construction AND by element: each cell donates `fraction` of its OWN
  // vanilla mass, as its OWN element, at its OWN temperature, paired one for one with an
  // ext::kRemoveVanillaMass for the same amount at the same cell -- the conserving pair
  // kRemoveVanillaMass's own doc comment defines. Nothing appears, nothing changes species,
  // and the temperature blend ApplyInjectGasSpecies performs is exact because the incoming
  // temperature is the cell's own.
  //
  // `fraction` must be below 1. A cell left with no vanilla mass at all is zeroed to 0 K by
  // ZeroMasslessCells -- which runs every substep regardless of vf_active -- and MixPair
  // then silently no-ops on it (`avg_t <= 0`). That is the KNOWN GAP recorded at
  // sim/simdll.cpp's volume-fractions block, not something this can work around; leaving
  // real mass on both sides keeps both layers live.
  //
  // Coordinates are IMAGE coordinates, y measured from the top, the same convention --probe
  // and --crop use, so a region picked out of a snapshot is typed in as it was seen.
  //
  // Both messages are queued, so this drains them with two zero-length frames before
  // returning: the queue rotation costs two frames (see Timeline::Start), and a caller that
  // captures a checkpoint straight afterwards has to be capturing a world the injection has
  // actually reached rather than one it is still on its way to.
  int InjectGasRect(int px, int py, int pw, int ph, float fraction) {
    if (!booted_ || !latest_ || !(fraction > 0.0f) || !(fraction < 1.0f)) return 0;
    std::vector<uint8_t> seen(static_cast<size_t>(tables_.count), 0);
    int sent = 0;
    int x0 = width_, y0 = height_, x1 = -1, y1 = -1;
    for (int j = 0; j < ph; ++j) {
      for (int i = 0; i < pw; ++i) {
        const int x = px + i, y_img = py + j;
        if (x < 0 || y_img < 0 || x >= width_ || y_img >= height_) continue;
        const int y = height_ - 1 - y_img;
        const int cell = y * width_ + x;
        const uint16_t idx = latest_->elementIdx[cell];
        // Phase 1 is gas (views.h paints the same four). The mixture layer is
        // atmosphere-only by its own scope note in gas_mixture_abi.h -- a liquid or a solid
        // has no business in it, and InjectSpecies would take the mass regardless.
        if (tables_.Phase(idx) != 1) continue;
        const float mass = latest_->mass[cell] * fraction;
        if (!(mass > 0.0f)) continue;
        // Sent in this order for readability only. The sim drains by category in a fixed
        // order of its own (sim/simdll.cpp, DrainQueue), and inject happens to run before
        // remove there; the pair conserves either way, because remove is clamped to what
        // the vanilla cell actually holds and inject never reads it back.
        oni_sim::ext::InjectGasSpeciesMessage in{};
        in.cellIdx = cell;
        in.speciesIdx = idx;
        in.massKg = mass;
        in.temperatureK = latest_->temperature[cell];
        simhost::sim_handle_message(oni_sim::ext::kInjectGasSpecies, sizeof(in),
                                    reinterpret_cast<const uint8_t*>(&in));
        oni_sim::ext::RemoveVanillaMassMessage rm{};
        rm.cellIdx = cell;
        rm.massKg = mass;
        simhost::sim_handle_message(oni_sim::ext::kRemoveVanillaMass, sizeof(rm),
                                    reinterpret_cast<const uint8_t*>(&rm));
        if (idx < seen.size() && !seen[idx]) {
          seen[idx] = 1;
          inject_species_.push_back(idx);
        }
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
        ++sent;
      }
    }
    if (sent == 0) return 0;
    // The digest window, in CELL coordinates. Grown past the cells that were seeded because
    // mixing does not stay where it was put: MixPair moves mass along every pair in a room,
    // so a diffusion front leaves the seeded rectangle within a few ticks. A margin rather
    // than the whole world because SIM_DebugGasMass costs a WaitIdle per call, and a digest
    // over every cell and every species would put ~800k of them in front of every tick of a
    // sweep. Big enough for the fronts a few hundred ticks of rate-0.2 mixing produce; a run
    // that needs more should say so rather than have this quietly guessed larger.
    const int kMargin = 16;
    inject_x0_ = x0 - kMargin < 0 ? 0 : x0 - kMargin;
    inject_y0_ = y0 - kMargin < 0 ? 0 : y0 - kMargin;
    inject_x1_ = x1 + kMargin >= width_ ? width_ - 1 : x1 + kMargin;
    inject_y1_ = y1 + kMargin >= height_ ? height_ - 1 : y1 + kMargin;
    // -1 rather than 0: the messages WERE sent, so "seeded nothing" would be the wrong
    // thing for a caller to report.
    if (!Publish() || !Publish()) return -1;
    return sent;
  }

  // Writes a per-cell thermal-mass bonus into a rectangle: the probe that makes the SEVENTH
  // checkpoint component load-bearing in a run this program can actually take.
  //
  // WHY A PROBE IS NEEDED AT ALL. `--timeline-test` on an ordinary corpus passes whether or
  // not the extension properties are carried, because nothing in a corpus writes one -- every
  // property sits at its default, the checkpoint blob is a bare 12-byte header, and the digest
  // agrees on both sides for the wrong reason. That is the same shape as the two milestones
  // this program spent reporting "every tick reproduces" while hashing three of the twelve
  // published arrays. A carrier nothing exercises is a carrier nothing tests.
  //
  // `sim.thermal_mass_bonus` is the right property to write because it is not inert: the sim's
  // conduction kernel binds it INSIDE its pair loop, so a checkpoint that fails to carry it
  // does not read as missing -- it produces a different temperature a few ticks later, in an
  // array that looks unrelated. Dropping the carry and re-running this is how the component was
  // shown to fire on purpose rather than merely to be present.
  //
  // Queued like the injection above, and drained with the same two zero-length frames, so a
  // caller capturing a checkpoint straight afterwards captures a world the writes have reached.
  int WriteThermalBonusRect(int px, int py, int pw, int ph, float joules_per_k) {
    if (!booted_ || !latest_) return 0;
    int sent = 0;
    for (int j = 0; j < ph; ++j) {
      for (int i = 0; i < pw; ++i) {
        const int x = px + i, y_img = py + j;
        if (x < 0 || y_img < 0 || x >= width_ || y_img >= height_) continue;
        const int y = height_ - 1 - y_img;
        oni_sim::SetCellFloatValueMessage m{};
        m.cellIdx = y * width_ + x;
        m.value = joules_per_k;
        simhost::sim_handle_message(oni_sim::ext::kSetCellThermalMassBonus, sizeof(m),
                                    reinterpret_cast<const uint8_t*>(&m));
        ++sent;
      }
    }
    if (sent == 0) return 0;
    if (!Publish() || !Publish()) return -1;
    return sent;
  }

  // The mixture mass of one species in one game cell. Zero for a world that never activated
  // volume fractions, for a cell that does not hold that species, and on a DLL without the
  // export -- all three are the same answer to this question and none of them is an error.
  float GasMass(int32_t cell, int32_t species) const {
    if (!debug_gas_mass_) {
      debug_gas_mass_ = reinterpret_cast<float (*)(int32_t, int32_t)>(
          GetProcAddress(simhost::g_sim, "SIM_DebugGasMass"));
    }
    return debug_gas_mass_ ? debug_gas_mass_(cell, species) : 0.0f;
  }

  // World::gas_sleeping_, one byte per PADDED cell: 1 where the mixing layer has parked a
  // cell, 0 where it is awake. Same two-call sizing as StableTicks above, and read-only for
  // the same reason the sim exports no setter for it -- a restored world forgets every one of
  // these bits and reproduces its run anyway, which is the measurement this accessor exists
  // to keep checkable. Empty on a DLL without SIM_DebugGasSleeping.
  std::vector<uint8_t> GasSleepingCells() const {
    if (!debug_gas_sleeping_) {
      debug_gas_sleeping_ = reinterpret_cast<int32_t (*)(uint8_t*, int32_t)>(
          GetProcAddress(simhost::g_sim, "SIM_DebugGasSleeping"));
    }
    if (!debug_gas_sleeping_) return {};
    const int32_t n = debug_gas_sleeping_(nullptr, 0);
    if (n <= 0) return {};
    std::vector<uint8_t> v(static_cast<size_t>(n));
    if (debug_gas_sleeping_(v.data(), n) != n) return {};
    return v;
  }

  // How many of those are parked right now. -1 when the DLL cannot say.
  int SleepingCount() const {
    const std::vector<uint8_t> v = GasSleepingCells();
    if (v.empty()) return -1;
    int n = 0;
    for (uint8_t b : v) {
      if (b) ++n;
    }
    return n;
  }

  // The window InjectGasRect seeded, grown by its margin, and the species it put there.
  // Empty until something is injected, which is what makes every mixture-aware check below
  // cost nothing on an ordinary run.
  bool has_mixture() const { return !inject_species_.empty(); }
  const std::vector<uint16_t>& injected_species() const { return inject_species_; }
  int mixture_x0() const { return inject_x0_; }
  int mixture_y0() const { return inject_y0_; }
  int mixture_x1() const { return inject_x1_; }
  int mixture_y1() const { return inject_y1_; }

  // The getter half of ext::kSetDiseaseGrowth. Both arrays in one call because they are one
  // state -- an infestation age without the growth remainder that produced it is not a state.
  // Leaves both empty on a DLL without the export, which is what makes carrying them optional
  // the same way the scheduling counters and the countdowns are.
  bool DiseaseGrowth(std::vector<float>* accum, std::vector<uint8_t>* infest) const {
    accum->clear();
    infest->clear();
    if (!debug_disease_growth_) {
      debug_disease_growth_ = reinterpret_cast<int32_t (*)(float*, uint8_t*, int32_t)>(
          GetProcAddress(simhost::g_sim, "SIM_DebugDiseaseGrowth"));
    }
    if (!debug_disease_growth_) return false;
    const int32_t n = debug_disease_growth_(nullptr, nullptr, 0);
    if (n <= 0) return false;
    accum->resize(static_cast<size_t>(n));
    infest->resize(static_cast<size_t>(n));
    if (debug_disease_growth_(accum->data(), infest->data(), n) != n) {
      accum->clear();
      infest->clear();
      return false;
    }
    return true;
  }

  // The getter half of ext::kSetCellRadiation. Leaves `out` empty on a DLL without the export.
  bool CellRadiation(std::vector<float>* out) const {
    out->clear();
    if (!debug_cell_radiation_) {
      debug_cell_radiation_ = reinterpret_cast<int32_t (*)(float*, int32_t)>(
          GetProcAddress(simhost::g_sim, "SIM_DebugCellRadiation"));
    }
    if (!debug_cell_radiation_) return false;
    const int32_t n = debug_cell_radiation_(nullptr, 0);
    if (n <= 0) return false;
    out->resize(static_cast<size_t>(n));
    if (debug_cell_radiation_(out->data(), n) != n) {
      out->clear();
      return false;
    }
    return true;
  }

  // Whether the DLL takes ext::kSetLoadIsRestore. It has no getter, so the answer comes from
  // the message table the DLL publishes (SIM_ExtMessageCount / SIM_ExtMessageDescribe), looked
  // up by wire id once. A DLL without the table -- the game's own -- takes none of these.
  bool RestoreModeSupported() const {
    if (restore_mode_ >= 0) return restore_mode_ == 1;
    restore_mode_ = 0;
    auto count = reinterpret_cast<int32_t (*)()>(
        GetProcAddress(simhost::g_sim, "SIM_ExtMessageCount"));
    auto describe = reinterpret_cast<int32_t (*)(int32_t, oni_sim::ext::ExtMessageDesc*)>(
        GetProcAddress(simhost::g_sim, "SIM_ExtMessageDescribe"));
    if (!count || !describe) return false;
    const int32_t n = count();
    for (int32_t i = 0; i < n; ++i) {
      oni_sim::ext::ExtMessageDesc d{};
      if (describe(i, &d) && d.id == oni_sim::ext::kSetLoadIsRestore) {
        restore_mode_ = 1;
        break;
      }
    }
    return restore_mode_ == 1;
  }

  // The getter half of ext::kSetVisibilityState: the message's whole payload. Empty on a DLL
  // without the export.
  std::vector<uint8_t> VisibilityState() const {
    std::vector<uint8_t> out;
    if (!debug_visibility_state_) {
      debug_visibility_state_ = reinterpret_cast<int32_t (*)(uint8_t*, int32_t)>(
          GetProcAddress(simhost::g_sim, "SIM_DebugVisibilityState"));
    }
    if (!debug_visibility_state_) return out;
    const int32_t n = debug_visibility_state_(nullptr, 0);
    if (n <= 0) return out;
    out.resize(static_cast<size_t>(n));
    if (debug_visibility_state_(out.data(), n) != n) out.clear();
    return out;
  }

  bool SetVisibilityState(const std::vector<uint8_t>& payload) {
    if (!booted_ || payload.empty()) return false;
    simhost::sim_handle_message(oni_sim::ext::kSetVisibilityState,
                                static_cast<int>(payload.size()), payload.data());
    return true;
  }

  bool SetCellRadiation(const std::vector<float>& values) {
    if (!booted_ || values.empty()) return false;
    oni_sim::ext::SetCellRadiationMessage head{};
    head.count = static_cast<int32_t>(values.size());
    std::vector<uint8_t> payload(sizeof(head) + values.size() * sizeof(float));
    memcpy(payload.data(), &head, sizeof(head));
    memcpy(payload.data() + sizeof(head), values.data(), values.size() * sizeof(float));
    simhost::sim_handle_message(oni_sim::ext::kSetCellRadiation,
                                static_cast<int>(payload.size()), payload.data());
    return true;
  }

  // int32 count, then that many floats, then that many bytes -- one buffer, because
  // SIM_HandleMessage takes one pointer and the sim checks the length against the count.
  bool SetDiseaseGrowth(const std::vector<float>& accum,
                        const std::vector<uint8_t>& infest) {
    if (!booted_ || accum.empty() || accum.size() != infest.size()) return false;
    oni_sim::ext::SetDiseaseGrowthMessage head{};
    head.count = static_cast<int32_t>(accum.size());
    std::vector<uint8_t> payload(sizeof(head) + accum.size() * sizeof(float) + infest.size());
    memcpy(payload.data(), &head, sizeof(head));
    memcpy(payload.data() + sizeof(head), accum.data(), accum.size() * sizeof(float));
    memcpy(payload.data() + sizeof(head) + accum.size() * sizeof(float), infest.data(),
           infest.size());
    simhost::sim_handle_message(oni_sim::ext::kSetDiseaseGrowth,
                                static_cast<int>(payload.size()), payload.data());
    return true;
  }

  // ------------------------------------------------------------------ live attach
  //
  // The other half of this program's two modes. Offline, Session OWNS the simulation:
  // it binds SimDLL, replays a corpus, and every array it draws is one the sim just
  // wrote. Attached, it owns nothing -- a running Oxygen Not Included does, and
  // `oni-framework-api`'s bulk routes hand its arrays over a socket. See attach.h.
  //
  // What makes this a small change rather than a second program is that `latest_` is
  // the only thing the renderer ever reads. Point it at a decoded reply instead of at
  // PrepareGameData's return and every view, probe, stat and readout follows.
  //
  // `booted_` deliberately stays FALSE here. Every method that sends the sim a message
  // -- Tick, Checkpoint, Restore, SetRandomState, InjectGasRect, ReplayRegistrations --
  // already opens by testing it, so attaching makes all of them refuse without a single
  // new guard. That is a property worth stating out loud: the sim-driving surface is
  // closed by construction, not by remembering to close it.
  bool Attach(const std::string& endpoint, int timeout_ms, std::string* err) {
    if (booted_) {
      *err = "already driving a sim offline";
      return false;
    }
    if (!attach_.Open(endpoint, timeout_ms, err)) return false;
    width_ = attach_.width();
    height_ = attach_.height();
    // Copied rather than delegated so tables() has one implementation and every caller
    // keeps reading the same reference it always did.
    tables_ = attach_.tables();
    attached_ = true;
    return true;
  }

  // One live frame. The counterpart of Tick: it advances nothing, it just asks what the
  // game has now. A failure leaves the previous frame standing (attach.h validates the
  // whole reply before it publishes a pointer), so a dropped poll blanks nothing.
  bool Poll(const attach::SurfaceSet& want, std::string* err) {
    if (!attached_) {
      *err = "not attached";
      return false;
    }
    if (!attach_.Poll(want, err)) return false;
    latest_ = attach_.frame();
    ++ticks_;
    return true;
  }

  bool attached() const { return attached_; }
  const attach::Source& source() const { return attach_; }

  bool booted() const { return booted_; }
  int width() const { return width_; }
  int height() const { return height_; }
  int cells() const { return width_ * height_; }
  int ticks() const { return ticks_; }
  // The tick counter is bookkeeping, not sim state -- the sim does not have one.
  // The timeline needs to burn a frame after a restore without that frame counting
  // as progress (see Timeline::settle), which is the only reason this is settable.
  void set_ticks(int t) { ticks_ = t; }
  size_t records_read() const { return records_read_; }
  size_t registrations_held() const { return registrations_.size(); }
  const GameDataUpdate* frame() const { return latest_; }
  const simhost::Tables& tables() const { return tables_; }

  // REGISTRY 2, ENUMERATED. Offline this is the only route there is: `/extelements` needs a
  // running game, and the corpus carries no attributes at all -- they are content data a mod
  // pushes after every load, never anything the sim saves. So an offline listing shows what
  // THIS SimDLL registers for itself, which is exactly `sim.molecular_mass` and its seeded
  // diatomic corrections on a build nobody has modded.
  //
  // The element NAMES are missing here and that is not an oversight: the sim's element table
  // carries the hash and no name, so offline the hash is the identity. `/elements` is where a
  // readable name comes from, and only an attached session has one.
  extattrs::Table ElementAttributes(bool with_values = true) const {
    extattrs::Table t;
    // ATTACHED, THE SIM IS IN ANOTHER PROCESS, so the exports below are not reachable and the
    // route is the only path. One accessor over both, because everything upstream of here --
    // the printer, the panel -- should not have to know which kind of session it has, exactly
    // as `ExtProps` already works for registry 1.
    if (attached_) {
      std::string err;
      if (!attach_.FetchElementAttributes(with_values, &t, &err)) {
        t = extattrs::Table();
        t.source = "/extelements";
        t.error = err;
      }
      return t;
    }
    t.source = "SIM exports";
    if (!booted_) {
      t.error = "no sim is booted, so there are no registries to enumerate";
      return t;
    }
    if (!ext_attr_count_) {
      ext_attr_count_ = reinterpret_cast<int32_t (*)()>(
          GetProcAddress(simhost::g_sim, "SIM_ExtElementAttributeCount"));
      ext_attr_describe_ =
          reinterpret_cast<int32_t (*)(int32_t, oni_sim::ext::ExtElementAttributeDesc*)>(
              GetProcAddress(simhost::g_sim, "SIM_ExtElementAttributeDescribe"));
      ext_attr_keys_ = reinterpret_cast<int32_t (*)(int32_t, int32_t*, int32_t)>(
          GetProcAddress(simhost::g_sim, "SIM_ExtElementAttributeKeys"));
      ext_attr_read_ = reinterpret_cast<int32_t (*)(int32_t, int32_t, int32_t, uint32_t*)>(
          GetProcAddress(simhost::g_sim, "SIM_ExtElementAttribute"));
    }
    if (!ext_attr_count_ || !ext_attr_describe_ || !ext_attr_keys_ || !ext_attr_read_) {
      t.error = "this SimDLL cannot enumerate element attributes -- it is either stock or "
                "predates the element attribute registry's enumeration exports";
      return t;
    }

    t.exported = true;
    t.with_values = with_values;
    const int32_t n = ext_attr_count_();
    for (int32_t i = 0; i < n; ++i) {
      oni_sim::ext::ExtElementAttributeDesc d{};
      if (!ext_attr_describe_(i, &d)) continue;
      extattrs::Attribute a;
      a.idx = d.declaredIdx;
      a.name.assign(d.name, strnlen(d.name, sizeof(d.name)));
      a.type = d.type;
      a.arity = d.arity;
      a.stride = d.stride;
      a.value_count = d.valueCount;
      a.first_party = d.firstParty != 0;
      a.writes = d.writes;
      if (with_values) {
        // Two calls, and the second return value is the one that matters: the export copies
        // at most `max` and still reports the true total, so an attribute that grew between
        // them is truncated here rather than overflowing the buffer.
        const int32_t total = ext_attr_keys_(i, nullptr, 0);
        if (total > 0) {
          std::vector<int32_t> keys(static_cast<size_t>(total));
          const int32_t reported = ext_attr_keys_(i, keys.data(), total);
          const int32_t have = reported < total ? reported : total;
          for (int32_t k = 0; k < have; ++k) {
            extattrs::Value v;
            v.id_hash = keys[static_cast<size_t>(k)];
            v.in_table = tables_.IndexOf(v.id_hash) >= 0;
            for (int32_t c = 0; c < a.arity; ++c) {
              uint32_t bits = 0;
              if (ext_attr_read_(i, v.id_hash, c, &bits)) v.bits.push_back(bits);
            }
            a.values.push_back(v);
          }
        }
      }
      t.attrs.push_back(a);
    }
    return t;
  }

 private:
  // See ShownRandomState().
  uint32_t pending_rng_ = 0;
  bool rng_pending_ = false;
  static int SilentHandler(int, void*) { return 0; }

  // Flattens the selected attribute onto the element table, so the attr view is one array
  // read per cell instead of a search of the registry per cell. Called from both places that
  // can invalidate it -- a new selection, and a new fetch -- because either alone leaves the
  // arrays describing a state that is no longer the one being drawn.
  //
  // A value whose element is not in this table is COUNTED and not stored: there is no row to
  // store it against, and dropping it silently would leave "the registry has four values and
  // the picture is empty" with no explanation.
  void ResolveAttrView() {
    const size_t n = tables_.count > 0 ? static_cast<size_t>(tables_.count) : 0;
    attr_has_.assign(n, 0);
    attr_value_.assign(n, 0.0f);
    attr_elements_ = 0;
    attr_off_table_ = 0;
    attr_lo_ = 0.0f;
    attr_hi_ = 0.0f;
    if (attr_view_.empty()) return;
    const extattrs::Attribute* a = ext_attrs_.Find(attr_view_);
    if (a == nullptr) return;
    if (attr_view_comp_ < 0 || attr_view_comp_ >= a->arity) return;
    const size_t comp = static_cast<size_t>(attr_view_comp_);
    bool any = false;
    for (const extattrs::Value& v : a->values) {
      const int32_t row = tables_.IndexOf(v.id_hash);
      if (row < 0) {
        ++attr_off_table_;
        continue;
      }
      // Short of `arity` only when a component read failed on the way in. Skipped rather
      // than defaulted: a default here would put a made-up number on the ramp.
      if (comp >= v.bits.size()) continue;
      const float x = static_cast<float>(extattrs::ScalarOf(a->type, v.bits[comp]));
      if (static_cast<size_t>(row) >= n) continue;
      attr_has_[static_cast<size_t>(row)] = 1;
      attr_value_[static_cast<size_t>(row)] = x;
      ++attr_elements_;
      if (!any || x < attr_lo_) attr_lo_ = x;
      if (!any || x > attr_hi_) attr_hi_ = x;
      any = true;
    }
  }

  // SIM_HandleMessages is the batched entry point; simhost.h binds only the singular
  // one, so it is resolved here rather than widening that shared header.
  void* SendMany(int id, int length, int count, const uint8_t* p) {
    if (!sim_handle_messages_) {
      sim_handle_messages_ = reinterpret_cast<void* (*)(int, int, int, const uint8_t*)>(
          GetProcAddress(simhost::g_sim, "SIM_HandleMessages"));
    }
    if (!sim_handle_messages_) return nullptr;
    return sim_handle_messages_(id, length, count, p);
  }

  simhost::Tables tables_;
  const GameDataUpdate* latest_ = nullptr;
  void* (*sim_handle_messages_)(int, int, int, const uint8_t*) = nullptr;
  int width_ = 0, height_ = 0;
  int ticks_ = 0;
  std::vector<uint8_t> visible_;
  size_t records_read_ = 0;
  // The corpus's own registration records, kept in arrival order for ReplayRegistrations.
  std::vector<CorpusRecord> registrations_;
  bool booted_ = false;
  bool attached_ = false;
  extprops::Table ext_props_;
  bool ext_live_ = false;
  std::string event_streams_;
  std::vector<extevents::Refusal> refusals_;
  int32_t events_lost_ = 0;       // whole batches the ROUTE dropped
  int32_t events_dropped_ = 0;    // records the SIM dropped at its per-frame cap
  int32_t events_forgotten_ = 0;  // records this log dropped off its own front
  std::string events_error_;
  bool ext_published_ = false;
  std::string ext_published_props_;
  std::string ext_view_;
  int ext_view_comp_ = 0;
  extattrs::Table ext_attrs_;
  std::string attr_view_;
  int attr_view_comp_ = 0;
  // Indexed by element-table ROW, so a cell's `elementIdx` indexes them directly.
  std::vector<uint8_t> attr_has_;
  std::vector<float> attr_value_;
  int attr_elements_ = 0;
  int attr_off_table_ = 0;
  float attr_lo_ = 0.0f;
  float attr_hi_ = 0.0f;
  attach::Source attach_;
  bool wall_clock_seeded_ = false;
  bool boot_unpublished_ = false;
  mutable uint32_t (*debug_random_state_)() = nullptr;
  mutable bool (*debug_sched_state_)(oni_sim::ext::SetSchedulingStateMessage*) = nullptr;
  mutable int32_t (*debug_stable_ticks_)(uint8_t*, int32_t) = nullptr;
  mutable float (*debug_gas_mass_)(int32_t, int32_t) = nullptr;
  mutable int32_t (*debug_gas_sleeping_)(uint8_t*, int32_t) = nullptr;
  mutable int32_t (*debug_disease_growth_)(float*, uint8_t*, int32_t) = nullptr;
  mutable int32_t (*debug_cell_radiation_)(float*, int32_t) = nullptr;
  mutable int32_t (*debug_visibility_state_)(uint8_t*, int32_t) = nullptr;
  // RestoreModeSupported's answer: -1 not asked yet, 0 no, 1 yes.
  mutable int restore_mode_ = -1;
  mutable int32_t (*debug_registry_state_)(uint8_t*, int32_t) = nullptr;
  mutable int32_t (*debug_ext_cell_state_)(uint8_t*, int32_t) = nullptr;
  mutable int32_t (*debug_ext_cell_state_all_)(uint8_t*, int32_t) = nullptr;
  // Registry 2's enumeration. Resolved lazily like every other optional export above: an
  // older SimDLL has the read side and not these, and that has to
  // degrade to "cannot enumerate" rather than to a crash.
  mutable int32_t (*ext_attr_count_)() = nullptr;
  mutable int32_t (*ext_attr_describe_)(int32_t, oni_sim::ext::ExtElementAttributeDesc*) = nullptr;
  mutable int32_t (*ext_attr_keys_)(int32_t, int32_t*, int32_t) = nullptr;
  mutable int32_t (*ext_attr_read_)(int32_t, int32_t, int32_t, uint32_t*) = nullptr;
  std::vector<uint16_t> inject_species_;
  int inject_x0_ = 0, inject_y0_ = 0, inject_x1_ = -1, inject_y1_ = -1;
};

}  // namespace simviz
