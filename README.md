# NoxusLOB

NoxusLOB is a C++20 price-time-priority limit order book with a C ABI and Python bridge. The engine matches in-memory orders for simulation and systems research.

## Matching rules

- **Price-time priority:** the best price executes first; orders at the same price execute in arrival order.
- **Maker-price execution:** each fill uses the resting maker order's price.
- **Limit:** trades against crossing liquidity, then rests any remainder.
- **Market:** sweeps the opposite book without a price limit; any unfilled remainder is canceled.
- **IOC:** trades only at or better than its limit price; any remainder is canceled.
- **FOK:** accepted only if the full quantity is available at its limit price; otherwise it is rejected without changing the book.
- **Modify:** reducing quantity keeps queue priority; increasing it removes and re-adds the order at the back of its price queue.
- **Price representation:** the C++ engine stores integer price units. The current Python gateway converts dollar prices to cents.

Timestamps are trade metadata; FIFO priority comes from the order's arrival in the engine.

## C++ API and storage

Use `OrderBook::submitOrder` when the caller needs an explicit outcome. `OrderResult` reports one of:

- `ACCEPTED_NO_FILL`
- `ACCEPTED_WITH_FILLS`
- `REJECTED_INVALID_INPUT`
- `REJECTED_DUPLICATE_ID`
- `REJECTED_FOK_UNFILLED`
- `REJECTED_CAPACITY`

The result also carries all fills, the unfilled quantity, and whether the order rests. The existing `addOrder` method remains as a compatibility wrapper that returns only fills.

To preserve the existing 32-bit quantity interfaces, total resting quantity on each side is limited to 4,294,967,295 units. A limit order that would exceed that total is rejected before any fills; a quantity increase that would exceed it returns `false` without changing the order. Side totals are maintained incrementally and checked against the reference book in the tests.

The order pool preallocates and recycles order nodes, and the per-price intrusive queues support constant-time unlinking. Price levels use ordered maps and order lookup uses a hash map; these containers and returned trade vectors may allocate. The pool therefore reduces order-node allocation churn, but the whole matching path is not allocation-free.

## C ABI and Python

New callers should use `noxus_submit_order` (or its `nexus_*` alias). It returns a `COrderResult` containing every fill. The caller must release its trade array with `noxus_free_order_result`.

The legacy `noxus_add_order` interface is retained. Its return value is the total number of fills and `outTradeCount` is the number copied to the supplied buffer; callers can detect truncation when those values differ. The Python bridge uses the full-result API and exposes the outcome status. Its Python fallback follows the same order rules.

The C++ and C APIs require serial access to each book. The Python bridge serializes calls with a lock so gateway worker threads and simulation tasks can share one book. Gateway stress-test timings include Python, locking, and FFI work; the dashboard displays these measured samples only after a stress test.

## Build and test

### CMake

```bash
cmake -B engine/build -S engine -DCMAKE_BUILD_TYPE=Release
cmake --build engine/build --config Release
ctest --test-dir engine/build --output-on-failure
```

To run the Debug configuration, use `-DCMAKE_BUILD_TYPE=Debug`. The test checks remain enabled in both configurations.

### Direct GCC build

```bash
mkdir -p build
g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -I engine/include engine/src/OrderBook.cpp engine/src/C_API.cpp engine/tests/test_matching.cpp -o build/test_matching
./build/test_matching
```

The C++ tests cover order semantics, pool double-free protection and reuse, C ABI result ownership, and a seeded 5,000-operation differential run against a deliberately simple reference matcher. On a failure, the test reports the fixed seed and failing check.

CI runs Debug and Release engine tests, an AddressSanitizer/UndefinedBehaviorSanitizer build, the Python gateway suite, and the web production build.

### Python gateway and dashboard

Install `server/requirements.txt`, then run `python run.py --test` for the combined C++ and API suite. It builds a fresh native library for the tests and verifies native/fallback parity. Run `npm ci` and `npm run build` from `web/` to build the dashboard, then use `python run.py` from the repository root to launch the application. See [the verification report](docs/verification.md) for the recorded checks and their scope.

## Benchmark

The benchmark pre-generates one deterministic 500,000-operation workload (seed 1337), warms up the engine, then runs it five times. The mix covers resting limits, cancellations, quantity modifications, crossing limits, and market sweeps. Throughput uses the entire workload; a separate pass samples the first 250,000 operations for p50, p90, p99, and p99.9 latency using a monotonic clock. It measures direct C++ `OrderBook` calls; it excludes Python and FFI overhead. Clock-query overhead and ordinary desktop scheduling affect the latency samples.

Reproduce the measured build and workload with:

```bash
mkdir -p build
g++ -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -I engine/include engine/src/OrderBook.cpp engine/benchmarks/main_benchmark.cpp -o build/benchmarks
./build/benchmarks 500000 5
```

### Recorded run

| Run | Throughput | p50 | p90 | p99 | p99.9 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 7.18 M ops/s | 0.10 µs | 0.20 µs | 0.40 µs | 0.70 µs |
| 2 | 6.26 M ops/s | 0.20 µs | 0.30 µs | 0.50 µs | 1.20 µs |
| 3 | 6.11 M ops/s | 0.20 µs | 0.30 µs | 0.60 µs | 1.60 µs |
| 4 | 6.42 M ops/s | 0.20 µs | 0.30 µs | 0.50 µs | 1.00 µs |
| 5 | 6.02 M ops/s | 0.10 µs | 0.20 µs | 0.40 µs | 0.70 µs |

Mean throughput was **6.40 ± 0.46 M ops/s**. Mean p50 was **0.16 ± 0.05 µs** and mean p99 was **0.48 ± 0.08 µs**. All 83,544 generated cancellations succeeded in the first run. Tail measurements vary across the five runs and are reported alongside throughput.

Recorded environment: Windows x64, GCC 14.2.0, C++20, `-O3 -DNDEBUG -Wall -Wextra -Wpedantic`, Intel Core Ultra 7 256V. Use the same workload and machine when comparing changes; this workload differs from the repository's earlier benchmark.

## Project layout

- `engine/`: C++ order book, C ABI, tests, and benchmark.
- `server/`: FastAPI gateway, Python bridge, and simulation models.
- `web/`: React trading dashboard.
