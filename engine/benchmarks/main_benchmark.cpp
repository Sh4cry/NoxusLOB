#include "OrderBook.hpp"
#include <iostream>
#include <vector>
#include <chrono>
#include <random>
#include <algorithm>
#include <array>
#include <numeric>
#include <iomanip>
#include <cmath>

using namespace noxus;

enum class BenchmarkOpType : uint8_t {
    ADD_LIMIT = 0,
    CANCEL = 1,
    CROSSING_LIMIT = 2,
    MARKET_SWEEP = 3,
    MODIFY = 4
};

struct PreGeneratedOp {
    BenchmarkOpType opType{BenchmarkOpType::ADD_LIMIT};
    OrderId id{0};
    Side side{Side::BUY};
    OrderType orderType{OrderType::LIMIT};
    Price price{0};
    Quantity qty{0};
    OrderId cancelId{0};
};

struct SingleRunResult {
    size_t totalOperations{0};
    size_t totalTrades{0};
    size_t cancelsAttempted{0};
    size_t cancelsSucceeded{0};
    double throughputDurationSec{0.0};
    double throughputOpsPerSec{0.0};
    double p50Us{0.0};
    double p90Us{0.0};
    double p99Us{0.0};
    double p999Us{0.0};
    double maxUs{0.0};
    double minUs{0.0};
    double meanUs{0.0};
};

struct MultiRunSummary {
    std::vector<SingleRunResult> runs;
    double meanThroughput{0.0};
    double stdDevThroughput{0.0};
    double meanP50{0.0};
    double stdDevP50{0.0};
    double meanP99{0.0};
    double stdDevP99{0.0};
    double meanTrades{0.0};
    double meanCancelSuccessRate{0.0};
};

// Phase 1: Pre-generate entire workload in memory (untimed)
// Uses a shadow OrderBook simulator so that every generated cancellation targets
// an order that is GENUINELY resting in the book at that point in time.
std::vector<PreGeneratedOp> generateWorkload(size_t orderCount) {
    std::vector<PreGeneratedOp> ops;
    ops.reserve(orderCount);

    std::mt19937_64 rng(1337); // Deterministic seed
    std::uniform_int_distribution<uint32_t> sideDist(0, 1);
    std::uniform_int_distribution<uint32_t> opDist(0, 11); // 50% add, 17% cancel, 8% modify, 17% cross, 8% sweep
    std::uniform_int_distribution<uint32_t> priceOffsetDist(1, 15);
    std::uniform_int_distribution<uint32_t> qtyDist(1, 50);

    Price currentMid = 10000; // $100.00 base
    OrderBook shadowBook(orderCount);
    std::vector<OrderId> restingOrderIds;
    restingOrderIds.reserve(orderCount);

    for (OrderId id = 1; id <= orderCount; ++id) {
        uint32_t op = opDist(rng);
        Side side = sideDist(rng) == 0 ? Side::BUY : Side::SELL;
        Quantity qty = qtyDist(rng);

        PreGeneratedOp item;
        item.id = id;
        item.side = side;
        item.qty = qty;

        if (op < 6 || restingOrderIds.empty()) {
            // Passive Limit Quote (Liquidity Maker)
            item.opType = BenchmarkOpType::ADD_LIMIT;
            item.orderType = OrderType::LIMIT;
            item.price = (side == Side::BUY) ? (currentMid - priceOffsetDist(rng))
                                             : (currentMid + priceOffsetDist(rng));
            shadowBook.addOrder(id, side, OrderType::LIMIT, item.price, qty);
            restingOrderIds.push_back(id);
        } else if (op < 8) {
            // Cancellation of an order verified to be resting in the book
            item.opType = BenchmarkOpType::CANCEL;
            size_t idx = rng() % restingOrderIds.size();
            item.cancelId = restingOrderIds[idx];
            restingOrderIds[idx] = restingOrderIds.back();
            restingOrderIds.pop_back();
            shadowBook.cancelOrder(item.cancelId);
        } else if (op == 8) {
            item.opType = BenchmarkOpType::MODIFY;
            const size_t idx = rng() % restingOrderIds.size();
            item.cancelId = restingOrderIds[idx];
            shadowBook.modifyOrder(item.cancelId, qty);
        } else if (op < 11) {
            // Aggressive Crossing Limit Order (Liquidity Taker)
            item.opType = BenchmarkOpType::CROSSING_LIMIT;
            item.orderType = OrderType::LIMIT;
            item.price = (side == Side::BUY) ? (currentMid + priceOffsetDist(rng))
                                             : (currentMid - priceOffsetDist(rng));
            auto trades = shadowBook.addOrder(id, side, OrderType::LIMIT, item.price, qty);
            // Prune fully-filled makers from resting list
            for (const auto& tr : trades) {
                if (!shadowBook.hasOrder(tr.makerOrderId)) {
                    auto it = std::find(restingOrderIds.begin(), restingOrderIds.end(), tr.makerOrderId);
                    if (it != restingOrderIds.end()) {
                        *it = restingOrderIds.back();
                        restingOrderIds.pop_back();
                    }
                }
            }
            if (shadowBook.hasOrder(id)) {
                restingOrderIds.push_back(id);
            }
        } else {
            // Immediate Market Sweep
            item.opType = BenchmarkOpType::MARKET_SWEEP;
            item.orderType = OrderType::MARKET;
            item.price = 0;
            auto trades = shadowBook.addOrder(id, side, OrderType::MARKET, 0, qty);
            for (const auto& tr : trades) {
                if (!shadowBook.hasOrder(tr.makerOrderId)) {
                    auto it = std::find(restingOrderIds.begin(), restingOrderIds.end(), tr.makerOrderId);
                    if (it != restingOrderIds.end()) {
                        *it = restingOrderIds.back();
                        restingOrderIds.pop_back();
                    }
                }
            }
        }

        // Mid-price tracking
        auto bboBid = shadowBook.getBestBid();
        auto bboAsk = shadowBook.getBestAsk();
        if (bboBid && bboAsk) {
            currentMid = (*bboBid + *bboAsk) / 2;
        } else if (id % 10 == 0) {
            int drift = static_cast<int>(rng() % 5) - 2;
            if (static_cast<int>(currentMid) + drift > 1000) {
                currentMid += drift;
            }
        }

        ops.push_back(item);
    }

    return ops;
}

// Phase 2: Warm up CPU cache & branch predictors
void warmupEngine(const std::vector<PreGeneratedOp>& ops, size_t warmupCount) {
    OrderBook warmupBook(warmupCount);
    for (size_t i = 0; i < warmupCount && i < ops.size(); ++i) {
        const auto& op = ops[i];
        if (op.opType == BenchmarkOpType::CANCEL) {
            warmupBook.cancelOrder(op.cancelId);
        } else if (op.opType == BenchmarkOpType::MODIFY) {
            warmupBook.modifyOrder(op.cancelId, op.qty);
        } else {
            warmupBook.addOrder(op.id, op.side, op.orderType, op.price, op.qty);
        }
    }
}

// Phase 3 & 4: Execute a single isolated benchmark pass
SingleRunResult runSingleBenchmark(const std::vector<PreGeneratedOp>& workload) {
    size_t orderCount = workload.size();

    // 1. Throughput Run (Pure engine matching, 0 clock queries in loop)
    OrderBook throughputBook(orderCount);
    size_t totalTrades = 0;
    size_t cancelsAttempted = 0;
    size_t cancelsSucceeded = 0;

    auto tStart = std::chrono::steady_clock::now();
    for (const auto& op : workload) {
        if (op.opType == BenchmarkOpType::CANCEL) {
            ++cancelsAttempted;
            if (throughputBook.cancelOrder(op.cancelId)) {
                ++cancelsSucceeded;
            }
        } else if (op.opType == BenchmarkOpType::MODIFY) {
            throughputBook.modifyOrder(op.cancelId, op.qty);
        } else {
            auto trades = throughputBook.addOrder(op.id, op.side, op.orderType, op.price, op.qty);
            totalTrades += trades.size();
        }
    }
    auto tEnd = std::chrono::steady_clock::now();
    double totalSec = std::chrono::duration<double>(tEnd - tStart).count();

    // 2. Latency Profiling Run (pre-allocated measurement array)
    OrderBook latencyBook(orderCount);
    size_t latencySamples = std::min<size_t>(orderCount, 250000);
    std::vector<double> latenciesUs(latencySamples, 0.0);

    for (size_t i = 0; i < latencySamples; ++i) {
        const auto& op = workload[i];
        auto op0 = std::chrono::steady_clock::now();
        if (op.opType == BenchmarkOpType::CANCEL) {
            latencyBook.cancelOrder(op.cancelId);
        } else if (op.opType == BenchmarkOpType::MODIFY) {
            latencyBook.modifyOrder(op.cancelId, op.qty);
        } else {
            latencyBook.addOrder(op.id, op.side, op.orderType, op.price, op.qty);
        }
        auto op1 = std::chrono::steady_clock::now();
        latenciesUs[i] = std::chrono::duration<double, std::micro>(op1 - op0).count();
    }

    std::sort(latenciesUs.begin(), latenciesUs.end());
    double sum = std::accumulate(latenciesUs.begin(), latenciesUs.end(), 0.0);

    SingleRunResult res;
    res.totalOperations = orderCount;
    res.totalTrades = totalTrades;
    res.cancelsAttempted = cancelsAttempted;
    res.cancelsSucceeded = cancelsSucceeded;
    res.throughputDurationSec = totalSec;
    res.throughputOpsPerSec = orderCount / totalSec;
    res.minUs = latenciesUs.front();
    res.maxUs = latenciesUs.back();
    res.meanUs = sum / latencySamples;
    res.p50Us = latenciesUs[latencySamples * 50 / 100];
    res.p90Us = latenciesUs[latencySamples * 90 / 100];
    res.p99Us = latenciesUs[latencySamples * 99 / 100];
    res.p999Us = latenciesUs[latencySamples * 999 / 1000];

    return res;
}

// Multi-run evaluation to measure variance across repeated runs
MultiRunSummary runMultiBenchmark(size_t orderCount, size_t numRuns = 5) {
    auto workload = generateWorkload(orderCount);
    warmupEngine(workload, std::min<size_t>(10000, orderCount / 10));

    MultiRunSummary summary;
    summary.runs.reserve(numRuns);

    std::vector<double> throughList;
    std::vector<double> p50List;
    std::vector<double> p99List;

    for (size_t r = 0; r < numRuns; ++r) {
        auto runRes = runSingleBenchmark(workload);
        summary.runs.push_back(runRes);
        throughList.push_back(runRes.throughputOpsPerSec);
        p50List.push_back(runRes.p50Us);
        p99List.push_back(runRes.p99Us);
    }

    auto calcStats = [](const std::vector<double>& v) -> std::pair<double, double> {
        double mean = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
        double sqSum = 0.0;
        for (double x : v) sqSum += (x - mean) * (x - mean);
        double stdDev = (v.size() > 1) ? std::sqrt(sqSum / (v.size() - 1)) : 0.0;
        return {mean, stdDev};
    };

    auto [mTh, sTh] = calcStats(throughList);
    auto [mP50, sP50] = calcStats(p50List);
    auto [mP99, sP99] = calcStats(p99List);

    summary.meanThroughput = mTh;
    summary.stdDevThroughput = sTh;
    summary.meanP50 = mP50;
    summary.stdDevP50 = sP50;
    summary.meanP99 = mP99;
    summary.stdDevP99 = sP99;
    summary.meanTrades = summary.runs[0].totalTrades;
    summary.meanCancelSuccessRate = (summary.runs[0].cancelsAttempted > 0)
        ? (100.0 * summary.runs[0].cancelsSucceeded / summary.runs[0].cancelsAttempted)
        : 100.0;

    return summary;
}

int main(int argc, char* argv[]) {
    size_t orderCount = 500000;
    size_t numRuns = 5;

    if (argc > 1) {
        orderCount = std::stoull(argv[1]);
    }
    if (argc > 2) {
        numRuns = std::stoull(argv[2]);
    }
    if (orderCount == 0 || numRuns == 0) {
        std::cerr << "Workload size and run count must both be greater than zero.\n";
        return 2;
    }

    std::cout << "===========================================================\n";
    std::cout << "       NoxusLOB Isolated C++20 Benchmark Suite            \n";
    std::cout << "===========================================================\n";
    std::cout << "[*] Workload Size  : " << orderCount << " operations / run\n";
    std::cout << "[*] Repeated Runs  : " << numRuns << " consecutive iterations\n";
    std::cout << "[*] Workload Setup : Shadow-book pre-generated in RAM (0 RNG in hot loop)\n";
    std::cout << "[*] Workload Seed  : 1337 (50% add / 17% cancel / 8% modify / 17% cross / 8% sweep)\n";
    std::cout << "[*] Measurement    : Direct C++ OrderBook API; ctypes/FFI excluded\n";
    std::cout << "[*] Cache Warm-Up  : 10,000 operations cache prime\n";
#if defined(__clang__)
    std::cout << "[*] Compiler       : Clang " << __clang_version__ << "\n";
#elif defined(__GNUC__)
    std::cout << "[*] Compiler       : GCC " << __VERSION__ << "\n";
#elif defined(_MSC_VER)
    std::cout << "[*] Compiler       : MSVC " << _MSC_VER << "\n";
#endif
#if defined(_WIN32)
    std::cout << "[*] Platform       : Windows\n";
#elif defined(__linux__)
    std::cout << "[*] Platform       : Linux\n";
#elif defined(__APPLE__)
    std::cout << "[*] Platform       : macOS\n";
#endif
#if defined(NDEBUG)
    std::cout << "[*] Build Mode     : Release (NDEBUG)\n";
#else
    std::cout << "[*] Build Mode     : assertions enabled\n";
#endif
    std::cout << "[*] C++ Standard   : C++" << (__cplusplus / 100 % 100) << "\n";
    std::cout << "===========================================================\n";
    std::cout << "Executing " << numRuns << " benchmark runs...\n";

    auto summary = runMultiBenchmark(orderCount, numRuns);

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n--- REPEATED RUNS SUMMARY ---\n";
    for (size_t i = 0; i < summary.runs.size(); ++i) {
        const auto& r = summary.runs[i];
        std::cout << "  Run #" << (i + 1) << ": " << (r.throughputOpsPerSec / 1e6) << "M ops/s | "
                  << "p50: " << r.p50Us << " µs | "
                  << "p90: " << r.p90Us << " µs | "
                  << "p99: " << r.p99Us << " µs | "
                  << "p99.9: " << r.p999Us << " µs\n";
    }

    const auto& best = summary.runs[0];
    std::cout << "\n--- WORKLOAD BREAKDOWN (Run #1) ---\n";
    std::cout << "Total Operations : " << best.totalOperations << "\n";
    std::cout << "Fills / Trades   : " << best.totalTrades << "\n";
    std::cout << "Cancels Attempted: " << best.cancelsAttempted << "\n";
    std::cout << "Cancels Succeeded: " << best.cancelsSucceeded << " ("
              << summary.meanCancelSuccessRate << "% verified resting cancellations)\n";
    std::array<size_t, 5> operationCounts{};
    for (const auto& op : generateWorkload(orderCount)) {
        ++operationCounts[static_cast<size_t>(op.opType)];
    }
    std::cout << "Operation Mix     : add=" << operationCounts[0]
              << ", cancel=" << operationCounts[1]
              << ", cross=" << operationCounts[2]
              << ", sweep=" << operationCounts[3]
              << ", modify=" << operationCounts[4] << "\n";

    std::cout << "\n--- STATISTICAL AGGREGATES (" << numRuns << " Runs) ---\n";
    std::cout << "Throughput       : " << (summary.meanThroughput / 1e6) << " M ops/sec "
              << "(± " << (summary.stdDevThroughput / 1e6) << " M ops/s)\n";
    std::cout << "Median Latency   : " << summary.meanP50 << " µs (± " << summary.stdDevP50 << " µs)\n";
    std::cout << "99th Percentile  : " << summary.meanP99 << " µs (± " << summary.stdDevP99 << " µs)\n";
    std::cout << "===========================================================\n";

    return 0;
}
