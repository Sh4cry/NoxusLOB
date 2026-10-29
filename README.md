# NoxusLOB

NoxusLOB is a C++20 price-time-priority limit order book with a C ABI and Python bridge. This repository is an educational matching-engine prototype; it does not connect to an exchange or model network execution latency.

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

The result also carries all fills, the unfilled quantity, and whether the order rests. The existing `addOrder` method remains as a compatibility wrapper that returns only fills.

The order pool preallocates and recycles order nodes, and the per-price intrusive queues support constant-time unlinking. Price levels use ordered maps and order lookup uses a hash map; these containers and returned trade vectors may allocate. The pool therefore reduces order-node allocation churn, but the whole matching path is not allocation-free.

## C ABI and Python

New callers should use `noxus_submit_order` (or its `nexus_*` alias). It returns a `COrderResult` containing every fill. The caller must release its trade array with `noxus_free_order_result`.

The legacy `noxus_add_order` interface is retained. Its return value is the total number of fills and `outTradeCount` is the number copied to the supplied buffer; callers can detect truncation when those values differ. The Python bridge uses the full-result API and exposes the outcome status. Its Python fallback follows the same order rules.

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

## Benchmark

The benchmark pre-generates one deterministic 500,000-operation workload (seed 1337), warms up the engine, then runs it five times. The mix covers resting limits, cancellations, quantity modifications, crossing limits, and market sweeps. It reports throughput and per-operation p50, p90, p99, and p99.9 latency. It measures direct C++ `OrderBook` calls; it excludes Python and FFI overhead.

Reproduce the measured build and workload with:

```bash
g++ -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -I engine/include engine/src/OrderBook.cpp engine/benchmarks/main_benchmark.cpp -o build/benchmarks
./build/benchmarks 500000 5
```

### Recorded run

| Run | Throughput | p50 | p90 | p99 | p99.9 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 5.29 M ops/s | 0.20 µs | 0.30 µs | 0.60 µs | 1.40 µs |
| 2 | 5.12 M ops/s | 0.20 µs | 0.30 µs | 0.60 µs | 4.30 µs |
| 3 | 5.73 M ops/s | 0.20 µs | 0.30 µs | 0.70 µs | 1.70 µs |
| 4 | 5.25 M ops/s | 0.20 µs | 0.30 µs | 0.60 µs | 1.10 µs |
| 5 | 5.82 M ops/s | 0.20 µs | 0.30 µs | 0.50 µs | 0.70 µs |

Mean throughput was **5.44 ± 0.31 M ops/s**. Mean p50 was **0.20 ± 0.00 µs** and mean p99 was **0.60 ± 0.07 µs**. All 83,544 generated cancellations succeeded in the first run.

Recorded environment: Windows x64, GCC 14.2.0, C++20, `-O3 -DNDEBUG`, Intel Core Ultra 7 256V. These are local measurements, not a performance guarantee; use the same workload and machine when comparing changes.

## Project layout

- `engine/`: C++ order book, C ABI, tests, and benchmark.
- `server/`: FastAPI gateway, Python bridge, and simulation models.
- `web/`: React trading dashboard.
