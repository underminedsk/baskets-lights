// Clock synchronization core — the resilience heart of the installation.
//
// Deliberately dependency-free (no Arduino, no ESP-NOW, no globals): the
// conductor's clock, the performer's offset, free-run behavior, and drop
// detection are all expressed as plain functions over a SyncState struct. That
// makes the hard part of the project — the part the brief calls "the real risk"
// — readable in isolation and testable on a host without hardware.
//
// All times are int64 microseconds (esp_timer_get_time on-device), never 32-bit
// millis, so there is no wraparound over a multi-day run.
#pragma once

#include <stdint.h>

struct SyncState {
  int64_t  offset_us;       // conductor_clock - local_clock
  int64_t  last_beacon_us;  // local time the last beacon was accepted
  bool     locked;          // have we ever locked to a beacon?
  uint32_t last_seq;        // sequence number of the last accepted beacon
  uint32_t beacons_rx;      // total beacons accepted
  uint32_t seq_gaps;        // beacons that arrived missing/out-of-order
};

// Returned from syncOnBeacon so callers can log per-beacon outcomes.
struct BeaconOutcome {
  bool gap;  // this beacon was not the expected next-in-sequence
};

inline void syncInit(SyncState& s) {
  s.offset_us = 0;
  s.last_beacon_us = 0;
  s.locked = false;
  s.last_seq = 0;
  s.beacons_rx = 0;
  s.seq_gaps = 0;
}

// Apply a received beacon.
//   epoch_us       conductor's clock stamped into the beacon at send time
//   seq            the beacon's sequence number
//   local_recv_us  this node's local clock when the beacon arrived
//
// The offset is chosen so that (local + offset) reproduces the conductor's clock.
// Transmission delay is sub-millisecond and intentionally ignored — fine for the
// slow visual patterns this drives.
inline BeaconOutcome syncOnBeacon(SyncState& s, int64_t epoch_us, uint32_t seq,
                                  int64_t local_recv_us) {
  BeaconOutcome out{false};

  // Gap detection before we adopt the new seq. uint32 arithmetic wraps cleanly,
  // so last_seq + 1 stays correct across the 2^32 rollover.
  if (s.locked && seq != (uint32_t)(s.last_seq + 1)) {
    s.seq_gaps++;
    out.gap = true;
  }

  s.offset_us = epoch_us - local_recv_us;
  s.last_beacon_us = local_recv_us;
  s.last_seq = seq;
  s.beacons_rx++;
  s.locked = true;
  return out;
}

// Synced ("conductor") time as seen by this node. Before the first lock the
// offset is 0, so this simply reads the local clock. After a lock it keeps
// returning sensible values even with no further beacons — that *is* free-run:
// the node coasts on its last known offset rather than blanking.
inline int64_t syncedTime(const SyncState& s, int64_t local_now_us) {
  return local_now_us + s.offset_us;
}

// Microseconds since the last accepted beacon. Negative sentinel if never locked.
inline int64_t beaconAge(const SyncState& s, int64_t local_now_us) {
  return s.locked ? (local_now_us - s.last_beacon_us) : -1;
}

// "Stale" means free-running on an old offset (or never locked). It does NOT mean
// stop rendering — the node keeps coasting. This is purely a diagnostics signal.
inline bool syncIsStale(const SyncState& s, int64_t local_now_us,
                         int64_t stale_us) {
  return !s.locked || (local_now_us - s.last_beacon_us) > stale_us;
}
