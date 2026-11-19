# matching-engine

A price-time priority limit order book and matching engine in C++20, with a Nasdaq TotalView-ITCH 5.0 replayer.

- **Orders:** limit (GTC), IOC, market, cancel, partial cancel, and cancel/replace.
- **Correctness:** every operation is checked against a brute-force model. The book can never cross, and shares are never created or lost. Three deliberately broken builds of the engine prove that these tests can catch real bugs.
- **Real data:** replays a full Nasdaq trading day (368M ITCH messages). On 99.91% of 8.1M executions, the engine picked the same order Nasdaq actually executed.
- **Latency:** p50, p99 and p99.9 for each operation type. The histogram uses constant memory, so a full trading day fits.

No dependencies beyond the standard library and CMake.

```bash
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
./build/bench_matching
```

## Design

```
                 ┌──────────────── OrderBook (one per instrument, single-threaded) ───────────────┐
 limit/IOC/mkt → │ match(): walk the best opposite level, oldest order first, until filled or no cross│
 cancel/reduce → │ index_: unordered_map<OrderId, Order*>  → O(1) lookup                              │
                 │ bids_:  std::map<Price, Level, greater>   asks_: std::map<Price, Level, less>      │
                 │ Level:  intrusive doubly linked FIFO of Order {id, price, qty, prev, next, level}  │
                 │ pool_:  std::deque<Order> + free list → stable addresses, no malloc per order      │
                 └──────────────────────────────────────────────────────────────────────────────────┘
```

- **Prices** are integer ticks (`int64_t`). There is no floating point anywhere near money.
- **Priority:** best price first, then arrival time. `reduce` (a partial cancel or execution) keeps the order's queue position. `replace` loses it, which is Nasdaq's rule for `U` messages.
- **Cancel** is O(1): one hash lookup and one unlink. The level is erased when its last order leaves.
- **Matching** pops orders from the head of the level and reuses their slots, so the hot path doesn't allocate once the pool has warmed up.
- **One thread per book**, by design. Real matching engines shard instruments across cores instead of locking a book.
- **Result accounting:** every call returns `{filled, rested, cancelled}`, and those always add up to the order's quantity. The conservation test is built on this.

## Testing: tests that prove they can fail

| Test | What it checks |
|---|---|
| `unit` | Hand-checked scenarios: price before time, sweeping several levels, IOC and market remainders, rejects, priority after reduce and replace, slot reuse |
| `property` | 300 seeds × 2,000 random operations (600k ops). After **every** op: results, trades and the full depth of book are identical to a brute-force model; `check_invariants()` passes (never crossed or locked, links consistent, level totals right); and `submitted == resting + 2·traded + cancelled` |
| `itch` | Encode/decode round trip for every message type, including reads split across buffer boundaries down to 1 byte at a time; truncated files and short messages are reported instead of read past; a 200k-message synthetic day replays with zero unknown refs and 100% FIFO agreement |
| `mutant1_is_caught` | Engine built with LIFO instead of FIFO in a level → the property test fails at step 5 ("trades differ") |
| `mutant2_is_caught` | Engine that drops one share on each partial fill → fails at step 1 ("invariant violated") |
| `mutant3_is_caught` | Engine that won't match at exactly the limit price → fails at step 13 ("result differs") |

The mutants are the same `src/order_book.cpp` compiled with `-DLOB_MUTANT=N`, and CTest expects them to fail (`WILL_FAIL`). If someone weakens the property test so that a mutant slips through, CI goes red.

The whole suite also passes under AddressSanitizer and UBSan:

```bash
cmake -S . -B build-asan -DLOB_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build-asan -j
ctest --test-dir build-asan
```

## Performance

Measured on an Apple M2 (8 cores), Apple clang 15, `-O3`, single thread, nothing pinned.

### Matching engine (`bench_matching`, 10M ops)

The order stream is recorded against a live book, so every cancel names an order that is really resting. It's then replayed into a fresh book. The mix is 40% cancel, 5% reduce, 40% passive limit, 5% crossing limit, 4% IOC and 1% market.

**Throughput:** **7.1M ops/s**. That's 10M ops in 1.41 s, producing 3.48M trades, with no timer in the loop.

| op | n | p50 | p99 | p99.9 | mean |
|---|---:|---:|---:|---:|---:|
| all ops | 10,000,000 | 42 ns | 625 ns | 1,041 ns | 144 ns |
| limit, passive | 4,824,264 | 42 ns | 125 ns | 292 ns | 46 ns |
| limit, crossing | 1,663,525 | 84 ns | 833 ns | 1,209 ns | 177 ns |
| IOC | 517,206 | 42 ns | 708 ns | 1,083 ns | 103 ns |
| market | 129,487 | 42 ns | 458 ns | 709 ns | 70 ns |
| cancel | 2,547,805 | 333 ns | 625 ns | 875 ns | 311 ns |
| reduce | 317,713 | 209 ns | 459 ns | 625 ns | 209 ns |

### ITCH replay: real Nasdaq day (2019-01-30, every symbol)

The input is `01302019.NASDAQ_ITCH50.gz`, streamed through `gunzip -c`.

| | full day | AAPL only |
|---|---:|---:|
| ITCH messages | 368,366,634 | (same file, filtered) |
| Book ops applied | 363,118,215 | 1,656,597 |
| Wall time, including gunzip and parsing | 149 s (2.5M msg/s, timed run) | 19.6 s (18.8M msg/s, untimed) |
| Book op latency | p50 250 ns · p99 1,459 ns · p99.9 3,000 ns · mean 338 ns | not timed |
| Malformed / rejected | 0 / 0 | 0 / 0 |
| FIFO agreement on `E` executions | **99.913%** (8,089,927 / 8,096,983) | 98.960% (88,174 / 89,101) |
| Adds that traded in our engine | 97 | 0 |
| Unknown refs | 137 | 0 |
| Invariants at end of day | OK | OK (book empty after the close) |

How to read this:

- **The decoder holds up.** It handled all 368M messages with nothing malformed and nothing rejected.
- **The book mirrors Nasdaq's.** Only 97 adds out of 164.7M crossed our book, and only 137 messages referenced an order we no longer had. My unverified guess: these come from periods when Nasdaq accepts orders without matching them, such as halts and IPO quote-only windows, where the real book can cross until a cross auction clears it. Our engine matches them right away instead. That eats orders Nasdaq still considers live, which then show up later as unknown refs.
- **The FIFO gap is real exchange behaviour.** About 0.09% of executions hit an order other than the oldest order at the best price. That's consistent with reserve-order refills and other priority rules that pure price-time ordering doesn't model.
- **Real-day latency is higher than the synthetic one.** Mean 338 ns vs 204 ns, because a real day has more symbols and a bigger working set, and so more cache misses.
- **gunzip isn't the bottleneck.** The untimed AAPL pass read the same file at 18.8M msg/s.

### ITCH replay (synthetic day: 20M messages, 200 symbols)

| | |
|---|---|
| Throughput, including parsing | 4.5M msg/s |
| Book op latency | p50 166 ns · p99 625 ns · p99.9 834 ns · mean 204 ns |
| Unknown refs / rejects / unexpected trades | 0 / 0 / 0 |
| FIFO agreement | 100% (1,519,120 / 1,519,120) |

### Reading these numbers honestly

- **Timer resolution.** On Apple Silicon `steady_clock` ticks every 41.67 ns, so values cluster at 42 and 84, and "p50 = 42 ns" means "one tick or less". On x86 Linux, swap in `rdtsc` to get sub-ns resolution. The mean is the most trustworthy figure for the fast operations.
- **Timer overhead** is included in every sample. Timing an empty block reads 0 ns at p50 and 42 ns at p99. Throughput is measured in a separate run with no timer at all.
- **Cancels are the slowest operation**, and I think memory is the reason, not the algorithm. A cancel touches a hash bucket, a hash node and the order itself, each of them cold in a book of about 850k orders. The next steps would be an open-addressing hash table (one fewer pointer chase) and flat price-level arrays instead of `std::map`. Both are hypotheses until profiled.
- **Not measured:** network I/O, kernel bypass, or multi-core sharding. This measures the book itself.

## Replaying real Nasdaq data

Nasdaq publishes full-day ITCH 5.0 sample files at <https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/>. Each file is about 4–5 GB gzipped and roughly 10 GB uncompressed.

```bash
# stream it straight from the .gz, no need to unpack
gunzip -c 01302019.NASDAQ_ITCH50.gz | ./build/itch_replay -
# one symbol, with its top of book at end of day
gunzip -c 01302019.NASDAQ_ITCH50.gz | ./build/itch_replay - --symbol AAPL
```

The replayer feeds `A`/`F` adds, `E`/`C` executions, `X` partial cancels, `D` deletes and `U` replaces into one `OrderBook` per instrument, then reports these checks:

- **engine trades:** how many times our matching engine traded on an add. Nasdaq's displayed book never crosses, so a book that mirrors it correctly should report 0.
- **FIFO agrees:** for each `E` execution, whether Nasdaq executed the same order our price-time priority would pick next.
- **unknown refs:** how many times ITCH referenced an order our book doesn't have.

Real data has hidden, reserve and cross executions that this model doesn't represent, so FIFO agreement there is below 100%: 99.913% on 2019-01-30 (see Performance). The gap measures how far real Nasdaq differs from pure price-time priority.

`gen_itch` writes a synthetic file in the same format, so all of the above runs without the download:

```bash
./build/gen_itch sim.itch 20000000 200
./build/itch_replay sim.itch --symbol SYM0042
```

## Layout

```
include/lob/order_book.hpp   engine API
src/order_book.cpp           engine (and the LOB_MUTANT hooks)
include/lob/itch.hpp         ITCH 5.0 decoder, streaming reader, encoder
include/lob/replay.hpp       ITCH → OrderBook, with the agreement checks
include/lob/itch_sim.hpp     synthetic ITCH generator
include/lob/latency.hpp      constant-memory latency histogram
tools/                       itch_replay, gen_itch
bench/                       bench_matching
tests/                       unit, property (+ mutants), itch
```
