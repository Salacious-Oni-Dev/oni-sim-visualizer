// timeline.h -- the scrub timeline: a sparse ring of checkpoints plus deterministic
// replay, so any tick that has already been simulated can be gone back to.
//
// The obvious implementation -- keep a checkpoint per tick -- is not affordable. A
// checkpoint of the reference world is 9,325,037 bytes (8.89 MB, 36.3 bytes per cell
// over 256,944 cells), so a hundred ticks of history would be 889 MB. What is
// affordable is a checkpoint every N ticks plus replay: restore the nearest checkpoint
// at or before the target and re-run the difference. That costs memory of
//
//     keep * blob_bytes
//
// and time of at most `interval` ticks per seek, and it is only correct because a replay
// reproduces the run it is replaying -- which is measured, and which took four changes in
// the sim to be true at all. A save blob carries the world and nothing else: not the random
// stream, not the SimData scheduling counters, not the per-cell countdowns that decide when
// an unstable solid falls, not the per-cell disease growth remainders, and not the four
// component registries, all of which AllocateCells and Load reset, and not the per-cell
// extension properties, which every Load re-defaults because `World::FromBlob` allocates.
// A CHECKPOINT IS NINE THINGS -- blob, ext::kSetRandomState, ext::kSetSchedulingState,
// ext::kSetStableTicks, ext::kSetDiseaseGrowth, ext::kSetRegistryState, ext::kSetExtCellState,
// ext::kSetCellRadiation, ext::kSetVisibilityState -- and any eight of them leave a replay that
// can diverge; each was added after a replay was measured diverging without it.
// Session::Checkpoint carries all nine. The authoritative
// list lives in oni-sim-replacement's abi/sim_abi_ext.h at kSetExtCellState and nowhere else,
// because this count has already been stale twice.
//
// The last two were each found by widening the test rather than by looking harder at the sim.
// For two milestones this program reported "every tick reproduces bit for bit" while hashing
// three of the twelve arrays GameDataUpdate publishes; the sixth then hid behind the fact
// that nothing published the registries at all, and it took hashing the registry blob itself
// to see a divergence at the frame it happened rather than as a temperature reading several
// ticks later. A bar is only worth what it covers.
//
// The digest now covers the five PROPERTY TEXTURES as well. They are derived
// from the grid rather than stored, and hashing them found on the first run that an exact
// stop publishes an EMPTY flow texture: the frame that primes the pipeline after a Load runs
// no physics, and flow records what a frame did rather than what the world holds. That one is
// a reading provenance and is classified as such rather than excused silently.
//
// Two things follow from the restore gaps above and are structural here rather than optional:
//
//   1. The timeline's tick 0 is CAPTURED AND THEN IMMEDIATELY RESTORED (Start below).
//      AllocateCells clears the building, element-chunk, flow and radiation
//      registrations the corpus sent once at boot, and no blob carries them back, so a
//      restored world is not the same world as a pristine boot. Making the displayed
//      run itself a restored one is what makes every later seek land on the same run
//      rather than on a slightly different one.
//
//      Re-sending those registrations is NOT what fixed this, and it was measured: a registration is a handle on a body whose temperature then evolves
//      every substep, so replaying the corpus's records restores the topology at boot
//      values and a checkpoint comes back stale by its own age. The registries are carried
//      whole instead (ext::kSetRegistryState, the sixth component), so a restored world now
//      contains the colony rather than an empty grid.
//
//      Restoring the anchor stays, and for a reason that outlived the registration gap: the
//      boot queue's messages -- `--seed` among them -- take two frames to rotate, so a
//      pristine run's first ticks run under state the anchor has already drained. Making the
//      displayed run a restored one is what puts every seek on the same run as the frames
//      before it.
//
//   2. dt is recorded once. Replay only reproduces the original if it is re-run with
//      the step it was run with, so changing dt mid-session invalidates history rather
//      than quietly producing a different past.
//
//   3. Every tick number here is an ABSOLUTE session tick, and the timeline does not
//      begin at 0. `Start` captures the anchor wherever the session already stands, so a
//      run that ticked N frames first (`--ticks N`) has an anchor at N and reaches
//      nothing before it. `base()` is that floor. This was tick 0 by assumption until it
//      was tested with `--ticks N` in front of a seek: the caller's relative tick and the
//      timeline's absolute one are the same number only when N is 0, and with N > 0 every
//      seek landed N ticks early -- or, once N exceeded the checkpoint interval, failed
//      outright with "no checkpoint at or before that tick".
#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "session.h"

namespace simviz {

class Timeline {
 public:
  // `keep` is the number of checkpoints held. Below three there is nothing to thin --
  // the anchor, one middle and the newest -- so that is the floor.
  void Configure(int interval, int keep, float dt) {
    interval_ = interval > 0 ? interval : 1;
    keep_ = keep > 3 ? keep : 3;
    dt_ = dt;
  }

  // Captures tick 0 and restores it, so the run that is displayed is a restored run.
  // See note 1 at the top of this file: without this the very first seek would move to
  // a world that has lost its registrations, and every frame after it would differ from
  // the frames before it for a reason that has nothing to do with the sim.
  bool Start(Session& s, std::string* err) {
    cps_.clear();
    base_ = s.ticks();
    head_ = base_;
    branched_ = false;
    seeks_ = replayed_ = thins_ = 0;
    // Drain whatever the boot sequence queued before reading the state to capture. Every
    // extension message is queued, and the rotation costs TWO frames, not one: a message
    // sent now sits in `queue`, the next frame swaps it into `active`, and the frame after
    // that drains it. `--seed N` is such a message, so capturing before those two frames
    // records the wall-clock state the corpus's own AllocateCells left behind, and pins
    // the whole timeline to a value that changes every run. That is exactly what it did,
    // and it looked like "taking a checkpoint perturbs the sim" until two runs of one
    // unchanged command were compared against each other. A zero-length frame runs no
    // substep (sim/simdll.cpp, the `elapsed <= 0` branch), so this drains without
    // stepping anything.
    if (!s.Publish() || !s.Publish()) {
      *err = "the sim refused a zero-length frame while draining the boot queue";
      return false;
    }
    Session::Checkpoint cp = s.Capture();
    if (!cp.valid()) {
      *err = "SIM_BeginSave returned nothing";
      return false;
    }
    if (!s.Restore(cp)) {
      *err = "the sim refused the anchor blob on Load";
      return false;
    }
    cps_.push_back(std::move(cp));
    return true;
  }

  // Called after every tick the window advances. Extends the head and captures when the
  // tick lands on the interval. Only new territory is captured: replaying over ground
  // that already has a checkpoint must not churn the ring.
  void Note(Session& s) {
    const int t = s.ticks();
    if (t <= head_) return;
    head_ = t;
    // The schedule is measured from the anchor, not from tick 0, so "a checkpoint every
    // N ticks" means the same thing whatever tick the timeline was started on.
    if (interval_ <= 0 || (t - base_) % interval_ != 0) return;
    if (!cps_.empty() && cps_.back().ticks >= t) return;
    Session::Checkpoint cp = s.Capture();
    if (!cp.valid()) return;  // a refused save costs resolution, not correctness
    cps_.push_back(std::move(cp));
    while (static_cast<int>(cps_.size()) > keep_) Thin();
  }

  // Moves the session to `target`, restoring and replaying as needed. Seeking forward
  // from where the session already is does not restore at all -- ticking on is both
  // cheaper and exactly what the sim would have done anyway.
  bool Seek(Session& s, int target, std::string* err) {
    if (target < base_) target = base_;
    if (target > head_) target = head_;
    if (target == s.ticks()) return true;
    ++seeks_;

    int from = s.ticks();
    bool restored = false;
    // A checkpoint captured after tick T reproduces DISPLAYED tick T + 1, not tick T.
    // Measured, not assumed: over a 40-tick sweep, every seek that restored and ran exactly
    // one frame reproduced the run to the last bit, and every seek that ran zero frames
    // landed one tick late.
    //
    // The REASON was wrong here until it was measured. This said the frame after a
    // restore runs no physics, because AllocateCells sets skip_physics_frames = 1. It does
    // not: the queued ext::kSetSchedulingState carries the CAPTURED skip of 0 and is drained
    // at the top of that same frame, ahead of StepPhysics, so the Load's skip is overwritten
    // before the guard is ever tested and the frame runs a full substep. `--timeline-diff`
    // reads the counters back frame by frame and says so outright: at +0 frames the rotation
    // is the Load's 0, and at +1 it is 2 -- the captured 1, plus one substep on top.
    //
    // What is actually true is the weaker statement, and it is enough: the PUBLISHED arrays
    // do not show that substep yet, so tick 0 and tick 1 hash identically and a seek to 0 is
    // served by the anchor like a seek to 1. `--digest` at 0, 1 and 2 ticks gives one mass
    // hash for the first two and a different one for the third.
    const int reachable = target < base_ + 1 ? base_ + 1 : target;
    const Session::Checkpoint* best = nullptr;
    for (const Session::Checkpoint& cp : cps_) {
      if (cp.ticks + 1 <= reachable && (!best || cp.ticks > best->ticks)) best = &cp;
    }
    // Restore only when going backwards, or when a checkpoint is closer to the target
    // than the current position is.
    if (target < from || (best && best->ticks + 1 > from)) {
      if (!best) {
        *err = "no checkpoint at or before that tick";
        return false;
      }
      if (!s.Restore(*best)) {
        *err = "restore failed";
        return false;
      }
      // The skipped frame. It is the restore's second half rather than a step, so the
      // tick counter is set to what that frame displays instead of being incremented.
      if (!s.Tick(dt_)) {
        *err = "the sim refused the frame that publishes a restore";
        return false;
      }
      from = best->ticks + 1;
      s.set_ticks(from);
      restored = true;
      if (target < from) {
        s.set_ticks(target);  // the anchor case above
        branched_ = false;    // the anchor is an exact stop, and so is the tick it shows
        return true;
      }
    }
    // Replayed physics reproduces the original run only if the checkpoint carried the
    // SimData scheduling counters with it (ext::kSetSchedulingState). With them, every
    // tick of the run is reachable exactly. Without them -- a stock SimDLL, or a custom
    // one built before that message existed -- a replay is a branch that diverges from
    // the run, and it says so rather than presenting a branch as history.
    // Measured both ways.
    if (target > from) {
      branched_ = !(best && best->sched_valid && !best->stable.empty() &&
                    !best->disease_accum.empty() && !best->registry.empty() &&
                    !best->ext_cells.empty() && !best->radiation.empty());
    }
    else if (restored) branched_ = false;
    for (int t = from; t < target; ++t) {
      if (!s.Tick(dt_)) {
        *err = "tick failed during replay";
        return false;
      }
      ++replayed_;
    }
    return true;
  }

  // Halves the ring and doubles the spacing. The alternative -- evicting the oldest --
  // would make the timeline unable to reach the beginning of the run, which is the one
  // place a debugger is always asked to go. This keeps full-span coverage and pays for
  // it in resolution: the history gets coarser, never shorter.
  void Thin() {
    if (cps_.size() < 3) return;
    std::vector<Session::Checkpoint> out;
    out.reserve(cps_.size() / 2 + 1);
    for (size_t i = 0; i < cps_.size(); i += 2) out.push_back(cps_[i]);
    // Every-other drops the newest whenever the count is even; the newest is the one
    // seek uses most, so it is put back.
    if (out.back().ticks != cps_.back().ticks) out.push_back(cps_.back());
    cps_.swap(out);
    interval_ *= 2;
    ++thins_;
  }

  // True when the displayed state is the run that was actually simulated, rather than a
  // replay that diverged from it. Cleared by a seek that lands on an exact stop, set by
  // any seek that has to replay, and carried through live ticks -- ticking on from a
  // branch continues the branch.
  bool branched() const { return branched_; }

  // True when a seek to `tick` needs no replayed physics: one frame past a checkpoint,
  // or tick 0, which the anchor publishes. Every tick is reproducible when the
  // checkpoints carry scheduling state (see `carries_schedule`); these are the ones that
  // are reproducible either way, and the ones a seek reaches without replaying.
  bool exact_stop(int tick) const {
    for (const Session::Checkpoint& cp : cps_) {
      if (cp.ticks + 1 == tick) return true;
    }
    return !cps_.empty() && tick == base_ && cps_.front().ticks == base_;
  }

  // Whether the DLL under the Session hands back its scheduling counters, and whether it
  // hands back the unstable-solid countdowns. Both are needed before "every tick
  // reproduces" is claimable; with only the first, a replay reproduces every tick that has
  // no falling solid in reach and quietly diverges on the ticks that do, which is worse
  // than not reproducing at all because it looks like it worked.
  bool carries_schedule() const { return !cps_.empty() && cps_.front().sched_valid; }
  bool carries_countdowns() const { return !cps_.empty() && !cps_.front().stable.empty(); }
  bool carries_disease() const {
    return !cps_.empty() && !cps_.front().disease_accum.empty();
  }
  bool carries_registries() const {
    return !cps_.empty() && !cps_.front().registry.empty();
  }
  // The seventh component. A DLL that has the export always returns at least a header, so an
  // empty field here means the DLL predates it -- never "there was nothing to carry".
  bool carries_ext_cells() const {
    return !cps_.empty() && !cps_.front().ext_cells.empty();
  }
  // The eighth: the radiation field Load clears in Vacuum and Void cells.
  bool carries_visibility() const {
    return !cps_.empty() && !cps_.front().visibility.empty();
  }
  bool carries_radiation() const {
    return !cps_.empty() && !cps_.front().radiation.empty();
  }

  // The checkpoint a seek to `tick` would restore, or null when the ring has none at or
  // before it. Exposed for --timeline-diff, which has to be able to restore the same
  // checkpoint by hand and watch what the sim does with it frame by frame.
  const Session::Checkpoint* checkpoint_serving(int tick) const {
    const int reachable = tick < base_ + 1 ? base_ + 1 : tick;
    const Session::Checkpoint* best = nullptr;
    for (const Session::Checkpoint& cp : cps_) {
      if (cp.ticks + 1 <= reachable && (!best || cp.ticks > best->ticks)) best = &cp;
    }
    return best;
  }

  // The earliest tick the timeline can reach: the tick the anchor was captured on, which
  // is 0 only when nothing ran before Start.
  int base() const { return base_; }
  int head() const { return head_; }
  int interval() const { return interval_; }
  int keep() const { return keep_; }
  float dt() const { return dt_; }
  int seeks() const { return seeks_; }
  int replayed_ticks() const { return replayed_; }
  int thins() const { return thins_; }
  size_t count() const { return cps_.size(); }

  size_t bytes() const {
    size_t n = 0;
    for (const Session::Checkpoint& cp : cps_) n += cp.bytes();
    return n;
  }

  // One frame past each checkpoint: the ticks a seek reaches with no replay at all.
  // Drawn on the scrub bar because the gaps between them are what a seek costs in
  // replayed frames -- and, on a DLL that does not carry scheduling state, the only
  // ticks that reproduce the run.
  std::vector<int> checkpoint_ticks() const {
    std::vector<int> v;
    v.reserve(cps_.size());
    for (const Session::Checkpoint& cp : cps_) v.push_back(cp.ticks + 1);
    return v;
  }

 private:
  std::vector<Session::Checkpoint> cps_;
  int interval_ = 10;
  int keep_ = 8;
  int base_ = 0;
  int head_ = 0;
  int seeks_ = 0, replayed_ = 0, thins_ = 0;
  bool branched_ = false;
  float dt_ = 0.2f;
};

}  // namespace simviz
