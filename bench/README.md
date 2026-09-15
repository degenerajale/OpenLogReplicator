# Parser throughput benchmark (runs A and B)

Answers two questions in one afternoon, on your hardware, with your redo:

1. **Can OLR consume redo faster than the database produces it?** (MB/s from run A vs your peak)
2. **Where does the parser thread's time go?** (run A vs run B splits scan/decode from
   transaction handling + build, which decides whether any optimization can help — see
   `specs/olr-parallelism-analysis.md` section 8)

Nothing here touches the database or the production OLR. Batch mode reads files and exits.

## What you need on the bench host

Ideally the DB host itself, or hardware matching it (same CPU, same storage path for the
archives) — the numbers are only meaningful on the hardware they'll run on.

1. **Archived redo logs from a real peak window** — 10–20 GB is enough. Copy them to a
   directory, e.g. `/olr-bench/archives`. They must be a contiguous range of sequences.
   From the database:

       SELECT sequence#, name, blocks*block_size/1048576 AS mb, first_time
         FROM v$archived_log WHERE first_time > SYSDATE - 1 ORDER BY sequence#;

2. **A copy of the production OLR checkpoint directory.** Batch mode needs the schema and
   refuses to start without it (error 10052). Copy — do not point at — the directory named
   by `state.path` in your production config (default `checkpoint/`):

       cp -r /path/to/production/checkpoint /olr-bench/state-copy

   The schema in it must match the archives' object ids. If DDL on your replicated tables
   happened between the checkpoint and the archive window, take a checkpoint copy from
   before the window instead.

3. **The OLR binary** with its libraries on the loader path (Instant Client is not needed
   for batch mode, but the binary is linked against it):

       export LD_LIBRARY_PATH=/path/to/instantclient:/path/to/instantclient/libaio:/path/to/librdkafka/lib:/path/to/prometheus/lib

## Configure

Edit both `olr-bench-A.json` and `olr-bench-B.json`:

- `"name"` — your database name, exactly as in the checkpoint file names (`<name>-chkpt-*.json`).
- `"state" → "path"` — the checkpoint copy.
- `"redo-log"` — the archive directory.
- `"format"` — make it match production (`message`, `scn-type`, anything else you set), so
  the build cost is representative.

In **A**, put your real `filter.table` list. In **B**, leave the `NOSUCHOWNER.NOSUCHTABLE`
entry — a filter that matches nothing, so OLR scans and decodes everything but builds nothing.

`start-scn` stays unset. With a checkpoint present and no `start-scn`, OLR treats every
transaction in the archives as new and builds it, which is what run A measures.

## Run

    ./run-bench.sh olr-bench-A.json /olr-bench/archives /path/to/OpenLogReplicator
    ./run-bench.sh olr-bench-B.json /olr-bench/archives /path/to/OpenLogReplicator

Each prints wall time, MB/s, the CPU% of the busiest threads, and the counters from the
end of the run. Run each twice; the first pass may be paying for cold page cache.

## Reading the result

**Throughput.** Run A's MB/s against your peak redo rate. Comfortably above it: the CPU
side is not your problem. Below it: read on.

**Which resource binds.** The top thread's CPU%:

- near 100% → CPU-bound. Clock speed and per-record work are the levers.
- well below 100% while MB/s is low → I/O-bound. The parser is waiting for the reader;
  storage latency is the lever (see `DIRECT_DISABLE`, flag 8, and analysis section 7.D).

**Where the CPU goes.** With both runs on the same archives:

    scan/decode fraction   = B / A          (time the filter cannot remove)
    handling+build fraction = (A - B) / A   (time proportional to matched rows)

If B is most of A, you are scan-bound: the early-exit change (analysis 7.E) and parallel
decode (7.B) are the options that apply; parallel build (7.C) and multi-instance (7.F) are not.

**Sanity check on A.** `dml_ops out` must be > 0 and `transactions commit out` should be
large. If both are 0, the run built nothing — usually the schema copy isn't being read
(wrong `name` or `path`) or the filter names don't match the checkpoint's schema. Run B
is *expected* to show `dml_ops out` = 0.

## Going deeper

To see which functions the parser thread spends its time in, run A under `perf`:

    perf record -g --call-graph dwarf -o perf-A.data -- /path/to/OpenLogReplicator -f olr-bench-A.json
    perf report -i perf-A.data --sort symbol | head -40

`OpCode*::process*`, `ktb*`, `kdo*` are decode; `memcpy` under `Parser::parse` is the block
walk; `appendToTransaction`/`findTransaction` are transaction handling; `Builder::*` is
serialization; `pthread_mutex_lock` under `checkTableDict` is the per-vector dictionary lock.

---

## Reader CPU benchmark (`reader-bench.cpp`)

Answers "did `read-parallel: 1` get slower?" locally, without the DB host. This box's disk is a
WSL virtual disk served from the Windows page cache, so throughput is meaningless — but with
I/O latency near zero, what remains is the reader's own per-request CPU work, which is exactly
what the parallel-reader change could have altered. It drives the real `Reader::mainLoop()`
with a parser-like consumer over a synthetic file of checksum-valid blocks and prints CPU
seconds per GB.

Build (a Release tree must exist; see the top of this file):

    cmake --build bench-build -j
    bench/build-bench.sh bench/reader-bench.cpp bench-build reader-bench-new -DHAS_READ_PARALLEL

Baseline (no `read-parallel`): a worktree at the commit before the reader change, built the
same way, with the bench compiled from that tree and **without** `-DHAS_READ_PARALLEL`:

    git worktree add /tmp/base <pre-reader-commit>
    cmake -S /tmp/base -B /tmp/base/base-build -DCMAKE_BUILD_TYPE=Release ...
    cmake --build /tmp/base/base-build -j
    cp bench/reader-bench.cpp /tmp/base/bench/
    bench/build-bench.sh /tmp/base/bench/reader-bench.cpp /tmp/base/base-build reader-bench-base

Fixture and runs:

    bench-build/reader-bench-new --make /home/user/olr-bench.redo 4096
    for i in 1 2 3 4 5; do
        /tmp/base/base-build/reader-bench-base /home/user/olr-bench.redo 1 1
        bench-build/reader-bench-new          /home/user/olr-bench.redo 1 1
    done
    # then direct 0; then reader-bench-new at N=4 and N=8

### Result — 4 GB fixture, WSL host, 2026-09-15

CPU seconds per GB, alternating runs. `direct=1` bypasses the Linux page cache; the
`direct=0` first pass is a cold-cache outlier and is excluded.

| binary | N | direct | wall med | MB/s med | CPU s/GB med | CPU s/GB min–max |
|--------|---|--------|----------|----------|--------------|------------------|
| baseline | 1 | 1 | 3.881 s | 1055 | 0.202 | 0.194–0.205 |
| new      | 1 | 1 | 3.840 s | 1067 | 0.201 | 0.199–0.205 |
| baseline | 1 | 0 | 0.476 s | 8614 | 0.129 | 0.129–0.130 |
| new      | 1 | 0 | 0.484 s | 8475 | 0.132 | 0.131–0.133 |
| new      | 4 | 1 | 2.623 s | 1561 | 0.246 | 0.241–0.248 |
| new      | 8 | 1 | 2.621 s | 1563 | 0.247 | 0.245–0.248 |

**Verdict:** N=1 is inside the run-to-run spread (−0.5 % direct, +1.9 % buffered, both under
the ~3 % WSL noise band), so the single-read path has not regressed. The pool costs ~23 % more
CPU per GB at N=4/8 but reads ~35 % faster from cache. These are not throughput predictions; the
real-latency / `iostat` acceptance run is `specs/olr-parallel-reader-fix-plan.md` step 4.
