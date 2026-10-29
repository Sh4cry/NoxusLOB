# Matching-engine verification

Recorded on 2026-10-29 for the `engine-core-quality` branch.

## What the engine does

The engine matches in-memory buy and sell orders using the best available price, then FIFO arrival order within a price. Fills execute at the resting maker's price. Limit orders may leave a resting remainder; market and IOC orders cancel their remainder; FOK orders check eligible liquidity before executing anything. Prices use integer units, with the Python gateway converting dollars to cents. Size reductions retain queue priority, while size increases move an order to the back of its existing price queue.

The implementation exposes explicit acceptance/rejection results, rejects side-volume overflow before mutation, recycles order nodes with ownership and double-free checks, and returns complete fill lists through the new C ABI. Existing C++, C, and Python call signatures remain available. The gateway preserves its existing `status` field and adds `engine_status`, `resting`, and `unfilled_quantity`. A bridge lock serializes access to each C++ book from Python worker threads.

## Local test results

| Check | Result | Evidence and scope |
| --- | --- | --- |
| Direct GCC Debug build (`-O0 -g`) | Passed | All 17 engine test groups. |
| Direct GCC Release build (`-O3 -DNDEBUG`) | Passed | The same 17 groups; checks remain active with `NDEBUG`. |
| Seeded differential test | Passed | 5,000 mixed operations, seed 336338638163. An independent vector-based matcher chooses makers by scanning price and arrival sequence. Trades, outcomes, active orders, side volume, price-level totals, counts, and pool activity are compared after each operation. |
| Pool and quantity boundaries | Passed | Foreign-pool/double-free rejection, growth, reuse, and 32-bit side-volume capacity. Capacity rejection preserves liquidity even when the incoming limit could partially execute. |
| C ABI boundaries | Passed | Null arguments, invalid enums, owned result release, all 80 fills returned, and explicit total-versus-copied counts for a legacy buffer holding only 64 of 70 fills. |
| `python run.py --test` | Passed | Rebuilt the native library from current sources, reran the C++ checks, then passed 12 API/bridge tests without skipped native tests. |
| Python/API regressions | Passed | Health, depth, order submission/cancellation, model selection, a 1,000-order stress request, native/fallback parity, FOK/IOC behavior, 70-fill delivery, numeric range rejection, capacity parity, and four concurrent callers. Agent control requests use mocked start/stop methods to avoid live-feed dependencies. |
| `npm run build` in `web/` | Passed | TypeScript compilation and Vite production build. This is a build check, not a visual browser review. |

The API checks used an in-process ASGI transport. On this Windows environment, Python's event loop required local socket access outside the restricted execution context. The combined runner uses a unique temporary native library and a separate test process so an already-loaded Windows DLL does not prevent rebuilding current sources.

## Benchmark

The recorded command was:

```bash
mkdir -p build
g++ -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -I engine/include engine/src/OrderBook.cpp engine/benchmarks/main_benchmark.cpp -o build/benchmarks
./build/benchmarks 500000 5
```

On Windows x64, GCC 14.2.0 and an Intel Core Ultra 7 256V, the final workload measured **6.40 ± 0.46 million operations/second**, mean p50 **0.16 µs**, and mean p99 **0.48 µs** over five runs. The workload contained 249,955 passive additions, 83,544 cancellations, 41,624 modifications, 83,397 crossing limits, and 41,480 market sweeps. All generated cancellations succeeded; 225,576 trade records were produced per throughput run.

Generation and warm-up are outside the timers. Throughput measures all 500,000 operations; a separate pass times the first 250,000 operations using `steady_clock`. Engine timestamp and result-container work is included. Python, locking, FFI conversion, network activity, and dashboard rendering are excluded. The README records each run, including p99.9 variation. These results use a different operation mix from earlier repository figures and should not be treated as a direct speedup comparison.

## Environment limits and CI

Local tools: GCC 14.2.0, Python 3.14.6, Node 24.14.0, npm 11.10.1. CMake was unavailable locally, so Debug/Release checks used the direct GCC commands above with the corresponding flags. The local MSYS2 compiler could not link AddressSanitizer/UndefinedBehaviorSanitizer because `libasan` and `libubsan` were absent. The GitHub workflow configures Linux CMake Debug, Release, and sanitizer jobs plus Python and web checks; their results are reported by GitHub Actions.

The matching core expects serial access per book. The tests establish the listed behavior on deterministic workloads and boundary scenarios; they do not establish exchange equivalence, network execution latency, or production readiness.
