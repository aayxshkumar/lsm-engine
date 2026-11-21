# lsm-engine

A log-structured merge-tree key-value store in C++20: write-ahead log, memtable, SSTables with bloom filters and checksums, a manifest, and compaction. It's tested by crashing it. A simulated file system cuts the power at a random point (mid-append, mid-flush, mid-compaction, even mid-recovery), and a model checker verifies that what comes back is exactly what the durability contract promises.

```
$ ./build/lsm_crash --seed 7
seed 7: PASS
  6 crashes (6 during recovery), 9 recoveries found a torn WAL tail
  4130 writes acknowledged, 716 reads checked
  92 flushes, 27 compactions, 7251 file-system mutations

$ ./build/lsm_crash --seeds 2000 --quiet
2000/2000 seeds passed (15.8 s wall)
```

No dependencies beyond googletest. Built on Apple clang 17 and GCC 13.

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/lsm_tests
./build/lsm_crash --seeds 1000
./build/lsm_bench --n 200000
python3 tools/mutants.py      # plants known bugs, reports what catches each
```

googletest 1.15.2 comes from `third_party/` if the tarball is there (offline machines), otherwise from GitHub; both are pinned to one SHA-256. Sanitizers: `-DLSM_SANITIZERS=address,undefined`.

## Design

```
 put/del/batch ──► WAL (000N.log) ──► memtable
                                        │ full
                                        ▼ flush
                               L0 tables (may overlap; searched newest first)
                                        │ 4 tables (configurable)
                                        ▼ compaction: merge all of L0 + L1
                               L1 tables (sorted, disjoint)
 MANIFEST: live tables, next file number, last sequence, oldest live log
```

- **WAL record:** `crc32c | len | payload`, one append per batch, so a crash tears at most one record. Replay stops at the first short or bad record.
- **Never append to a replayed log.** Recovery writes what it replayed to an L0 table, starts a fresh log and deletes the old ones. Writing after a torn tail would put new records behind garbage, where replay can never reach them.
- **SSTable:** data blocks, a bloom filter (10 bits/key, about 1% false positives) and an index, each with a CRC-32C trailer, plus a checksummed footer. One version per key, since there are no snapshots yet.
- **Manifest:** rewritten in full on every change: written to `MANIFEST.tmp`, synced, then renamed over `MANIFEST`. Files the manifest doesn't list are deleted at open.
- **Flush order:** create the new log → write and sync the table → write the manifest (which now points at the new log) → delete the old log. Crash at any point and recovery either replays the old log or uses the table, never neither.
- **Sequence numbers** order versions; internal key order is (user key ↑, sequence ↓), so a merge sees the newest version of each key first.
- **Single-threaded:** flush and compaction run inline in the write that triggers them.

## The durability contract

- A write with `WriteOptions{.sync = true}` is durable when it returns, and so is every write before it.
- Writes without sync may be lost. But recovery always keeps a **prefix** of the writes in the order they were made (never a later write without an earlier one), and a `WriteBatch` is all or nothing.

## How it's tested

`SimFs` is an in-memory file system. Each file tracks how many of its bytes have been synced. `crash()` simulates power loss: each file keeps its synced bytes plus a random prefix of the rest. A crash point can be armed on the n-th mutating call (append, sync, create, rename, remove), and an append that hits it writes a random part of its data first.

`run_crash_test` (in `src/crash_harness.cpp`) runs rounds of random puts, deletes, batches and reads against a `std::map` model, crashes, recovers, and checks:

| Check | Fails if |
|---|---|
| Durability | a write acknowledged before a sync write, a flush or a successful open is missing |
| Prefix | the recovered contents don't equal the model after *some* prefix of the attempted batches |
| Atomicity | the recovered sequence number lands inside a batch |
| Reads | any `get` between crashes disagrees with the model, or `get` disagrees with `scan` after recovery |
| No crash leaks | a recovery throws (corrupt manifest, unreadable table) |

Crashes also land inside recovery itself (30% of opens), and the next recovery has to cope with that.

## What catches what

`tools/mutants.py` plants each bug in a scratch copy and runs both the unit tests and 1000 crash-sweep seeds:

| Mutant | Bug | Unit tests | Crash sweep |
|---|---|---|---|
| wal-no-sync | sync write returns without syncing the WAL | 2 fail | 1000/1000 |
| manifest-no-sync | MANIFEST.tmp renamed without being synced | 4 fail | 1000/1000 |
| table-no-sync | table added to the manifest unsynced | 2 fail | 1000/1000 |
| log-deleted-early | old WAL deleted before the manifest that replaces it | 2 fail | 298/1000 |
| flush-drops-deletes | flush drops tombstones; old L1 values come back | 5 fail | 1000/1000 |
| l0-oldest-first | get searches L0 oldest first | 3 fail | 1000/1000 |
| batch-not-atomic | batch logged as one record per op | 2 fail | 921/1000 |
| wal-skips-crc | WAL replay ignores checksums | 1 fail | **0/1000** |
| block-skips-crc | table blocks read without checking their checksum | 1 fail | **0/1000** |

The last two show the limit of the crash model. `SimFs` only ever loses bytes from the end of a file; it never returns wrong bytes. So the sweep can't tell whether checksums are checked at all, and only hand-written corruption tests catch those two.

There's also a gap no mutant can show yet. **The engine never fsyncs a directory**, and `SimFs` treats create, rename and remove as durable the moment they happen. On Linux (ext4, xfs) a newly created WAL or table, or the rename of `MANIFEST`, can be lost in a crash unless the directory is fsynced. So the engine almost certainly has a real durability bug that its own harness can't see. Fixing the model to expose it, then fixing the engine, is the first item below.

## Performance (Apple M4, macOS, APFS, Release, 200k keys, 100-byte values, no sync)

| Workload | ops/s | µs/op |
|---|---|---|
| fillseq | 332,544 | 3.01 |
| fillrandom | 211,713 | 4.72 |
| readrandom | 106,581 | 9.38 |
| readmissing (bloom filter) | 4,460,909 | 0.22 |
| scan | 1,838,338 | 0.54 per row |

Write amplification 3.63. Point reads are slow because there's no block cache: every `get` reads and decodes a whole 4 KB block. Write amplification grows with the database, because each compaction rewrites all of L1.

## Not handled yet

Roughly in order:

1. **Directory durability.** Make `SimFs` lose unsynced directory entries (creates, renames, removes) on crash, watch the sweep fail, then add `sync_dir` to the file system interface and call it in the right places.
2. **A nastier crash model:** unsynced pages lost out of order, zero-filled or garbage tails, bit flips in synced data, failing `fsync` (and what to do after one, given fsyncgate). The two checksum mutants should be caught by the sweep after this.
3. **Leveled compaction:** L0…Ln with a size ratio and per-file overlap picking, so compaction cost stops scaling with the whole database. Measure write amplification before and after.
4. **Block cache** (LRU, sharded) and binary search inside blocks with restart points.
5. **Snapshots and MVCC:** keep multiple versions in tables, iterators that stay stable across flushes and compactions.
6. **Concurrency:** background flush and compaction threads, an immutable memtable, a concurrent skiplist memtable, readers that don't block writers. Checked with ThreadSanitizer and the crash harness.
7. **Group commit**, so concurrent sync writes share one fsync.
8. **Compression** (a self-written LZ-style codec, since the machine is offline) and prefix-compressed keys.
9. **Benchmarks against a baseline** with `fio`-style reporting: p50/p99/p99.9 latency, not just averages.

## Layout

```
include/lsm/   coding, crc32c, rng, fs (+ PosixFs), sim_fs, format, wal, iterator,
               memtable, bloom, sstable, manifest, db, crash_harness
src/           implementations
tools/         lsm_crash (crash sweeps), lsm_bench (throughput), mutants.py
tests/         unit tests per component, a model-based DB test, crash tests
```

## References

- Patrick O'Neil et al., *The Log-Structured Merge-Tree*, 1996.
- LevelDB's table and log formats (this is an independent implementation with its own formats).
- Thanumalayan Sankaranarayana Pillai et al., *All File Systems Are Not Created Equal* (ALICE), OSDI 2014.
- Rebello et al., *Can Applications Recover from fsync Failures?*, USENIX ATC 2020.

MIT licence.
