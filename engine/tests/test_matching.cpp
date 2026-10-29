#include "OrderBook.hpp"
#include "C_API.h"
#include <algorithm>
#include <iostream>
#include <map>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>

using namespace noxus;

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string("CHECK failed at ") + __FILE__ + ":" + std::to_string(__LINE__) + ": " #condition); \
} while (false)

void testBasicInsertion() {
    std::cout << "[TEST] Running testBasicInsertion... ";
    OrderBook book;
    auto trades = book.addOrder(1, Side::BUY, OrderType::LIMIT, 10000, 50);
    CHECK(trades.empty());
    CHECK(book.orderCount() == 1);
    CHECK(book.hasOrder(1));
    CHECK(book.getBestBid().value() == 10000);
    CHECK(!book.getBestAsk().has_value());
    CHECK(book.getBidVolume() == 50);
    std::cout << "PASSED\n";
}

void testExactMatch() {
    std::cout << "[TEST] Running testExactMatch... ";
    OrderBook book;
    // Maker Sell: 100 units at $100.00 (10000)
    book.addOrder(1, Side::SELL, OrderType::LIMIT, 10000, 100);
    CHECK(book.getBestAsk().value() == 10000);

    // Taker Buy: 100 units at $100.00
    auto trades = book.addOrder(2, Side::BUY, OrderType::LIMIT, 10000, 100);
    CHECK(trades.size() == 1);
    CHECK(trades[0].makerOrderId == 1);
    CHECK(trades[0].takerOrderId == 2);
    CHECK(trades[0].price == 10000);
    CHECK(trades[0].quantity == 100);
    CHECK(book.orderCount() == 0);
    CHECK(!book.getBestBid().has_value());
    CHECK(!book.getBestAsk().has_value());
    std::cout << "PASSED\n";
}

void testPriceTimePriorityFIFO() {
    std::cout << "[TEST] Running testPriceTimePriorityFIFO... ";
    OrderBook book;
    // Two makers at same price: $50.00
    book.addOrder(101, Side::SELL, OrderType::LIMIT, 5000, 40); // First
    book.addOrder(102, Side::SELL, OrderType::LIMIT, 5000, 60); // Second

    // Taker buys 50: Should fully fill 101 (40) and partially fill 102 (10)
    auto trades = book.addOrder(201, Side::BUY, OrderType::LIMIT, 5000, 50);
    CHECK(trades.size() == 2);
    CHECK(trades[0].makerOrderId == 101);
    CHECK(trades[0].quantity == 40);
    CHECK(trades[1].makerOrderId == 102);
    CHECK(trades[1].quantity == 10);

    // Remaining in book should be 102 with 50 units
    CHECK(book.orderCount() == 1);
    CHECK(book.hasOrder(102));
    CHECK(book.getOrder(102)->remainingQty == 50);
    std::cout << "PASSED\n";
}

void testPriceImprovement() {
    std::cout << "[TEST] Running testPriceImprovement... ";
    OrderBook book;
    // Maker Sell at $100.00
    book.addOrder(1, Side::SELL, OrderType::LIMIT, 10000, 10);
    // Taker aggressively bids $105.00 -> should execute at maker's price ($100.00)
    auto trades = book.addOrder(2, Side::BUY, OrderType::LIMIT, 10500, 10);
    CHECK(trades.size() == 1);
    CHECK(trades[0].price == 10000);
    CHECK(book.orderCount() == 0);
    std::cout << "PASSED\n";
}

void testOrderCancellation() {
    std::cout << "[TEST] Running testOrderCancellation... ";
    OrderBook book;
    book.addOrder(1, Side::BUY, OrderType::LIMIT, 20000, 30);
    book.addOrder(2, Side::BUY, OrderType::LIMIT, 19900, 50);
    CHECK(book.orderCount() == 2);
    CHECK(book.getBestBid().value() == 20000);

    bool cancelled = book.cancelOrder(1);
    CHECK(cancelled);
    CHECK(book.orderCount() == 1);
    CHECK(!book.hasOrder(1));
    CHECK(book.getBestBid().value() == 19900);

    bool cancelNonExistent = book.cancelOrder(999);
    CHECK(!cancelNonExistent);
    std::cout << "PASSED\n";
}

void testOrderModification() {
    std::cout << "[TEST] Running testOrderModification... ";
    OrderBook book;
    book.addOrder(1, Side::BUY, OrderType::LIMIT, 30000, 100);
    
    // Reduce size: keeps priority, reduces book volume
    bool modSuccess = book.modifyOrder(1, 60);
    CHECK(modSuccess);
    CHECK(book.getOrder(1)->remainingQty == 60);
    CHECK(book.getBidVolume() == 60);
    std::cout << "PASSED\n";
}

void testMarketOrder() {
    std::cout << "[TEST] Running testMarketOrder... ";
    OrderBook book;
    book.addOrder(1, Side::SELL, OrderType::LIMIT, 1000, 20);
    book.addOrder(2, Side::SELL, OrderType::LIMIT, 1010, 30);

    // Market Buy for 40 units: sweeps level 1000 (20) and level 1010 (20)
    auto trades = book.addOrder(3, Side::BUY, OrderType::MARKET, 0, 40);
    CHECK(trades.size() == 2);
    CHECK(trades[0].price == 1000 && trades[0].quantity == 20);
    CHECK(trades[1].price == 1010 && trades[1].quantity == 20);
    CHECK(book.orderCount() == 1);
    CHECK(book.getOrder(2)->remainingQty == 10);
    std::cout << "PASSED\n";
}

void testFillOrKill() {
    std::cout << "[TEST] Running testFillOrKill... ";
    OrderBook book;
    book.addOrder(1, Side::SELL, OrderType::LIMIT, 5000, 30);

    // FOK for 50 units (insufficient liquidity) -> killed immediately
    auto trades1 = book.addOrder(2, Side::BUY, OrderType::FOK, 5000, 50);
    CHECK(trades1.empty());
    CHECK(book.orderCount() == 1); // Order 1 unaffected

    // FOK for 30 units (exact match available) -> fills
    auto trades2 = book.addOrder(3, Side::BUY, OrderType::FOK, 5000, 30);
    CHECK(trades2.size() == 1);
    CHECK(book.orderCount() == 0);
    std::cout << "PASSED\n";
}

void testImmediateOrCancel() {
    std::cout << "[TEST] Running testImmediateOrCancel... ";
    OrderBook book;
    // Maker Sell: 25 units at $100.00
    book.addOrder(1, Side::SELL, OrderType::LIMIT, 10000, 25);
    CHECK(book.orderCount() == 1);

    // Taker IOC Buy for 60 units at $100.00:
    // Should partially fill 25 units, cancel the remaining 35 units, and NOT rest in book
    auto trades = book.addOrder(2, Side::BUY, OrderType::IOC, 10000, 60);
    CHECK(trades.size() == 1);
    CHECK(trades[0].quantity == 25);
    CHECK(book.orderCount() == 0); // Maker filled, IOC remainder cancelled
    CHECK(!book.hasOrder(2));     // Taker was not placed on book
    std::cout << "PASSED\n";
}

void testDuplicateOrderIdRejection() {
    std::cout << "[TEST] Running testDuplicateOrderIdRejection... ";
    OrderBook book;
    auto trades1 = book.addOrder(42, Side::BUY, OrderType::LIMIT, 10000, 50);
    CHECK(trades1.empty());
    CHECK(book.orderCount() == 1);

    // Attempt to insert duplicate order ID 42 -> must be rejected cleanly
    auto trades2 = book.addOrder(42, Side::SELL, OrderType::LIMIT, 10000, 50);
    CHECK(trades2.empty());
    CHECK(book.orderCount() == 1);
    CHECK(book.getOrder(42)->side == Side::BUY); // Original order preserved intact
    std::cout << "PASSED\n";
}

void testInvalidZeroQuantity() {
    std::cout << "[TEST] Running testInvalidZeroQuantity... ";
    OrderBook book;
    // Inserting order with quantity = 0 -> must be rejected
    auto trades = book.addOrder(99, Side::BUY, OrderType::LIMIT, 10000, 0);
    CHECK(trades.empty());
    CHECK(book.orderCount() == 0);
    CHECK(!book.hasOrder(99));
    std::cout << "PASSED\n";
}

void testL2DepthSnapshotInvariants() {
    std::cout << "[TEST] Running testL2DepthSnapshotInvariants... ";
    OrderBook book;
    book.addOrder(1, Side::BUY, OrderType::LIMIT, 10000, 15);
    book.addOrder(2, Side::BUY, OrderType::LIMIT, 10000, 25); // Aggregated at level 10000 -> 40 qty, 2 orders
    book.addOrder(3, Side::BUY, OrderType::LIMIT, 9950, 60);

    book.addOrder(4, Side::SELL, OrderType::LIMIT, 10050, 10);
    book.addOrder(5, Side::SELL, OrderType::LIMIT, 10100, 50);

    CHECK(book.getBidVolume() == 100);
    CHECK(book.getAskVolume() == 60);

    auto snap = book.getL2Snapshot(5);
    CHECK(snap.bids.size() == 2);
    CHECK(snap.asks.size() == 2);

    // Verify bid price descending
    CHECK(snap.bids[0].price == 10000);
    CHECK(snap.bids[0].quantity == 40);
    CHECK(snap.bids[0].orderCount == 2);
    CHECK(snap.bids[1].price == 9950);
    CHECK(snap.bids[1].quantity == 60);

    // Verify ask price ascending
    CHECK(snap.asks[0].price == 10050);
    CHECK(snap.asks[0].quantity == 10);
    CHECK(snap.asks[1].price == 10100);
    CHECK(snap.asks[1].quantity == 50);

    std::cout << "PASSED\n";
}

void testTypedOrderResults() {
    OrderBook book(2);
    auto resting = book.submitOrder(1, Side::BUY, OrderType::LIMIT, 10000, 10, 10);
    CHECK(resting.status == OrderStatus::ACCEPTED_NO_FILL);
    CHECK(resting.accepted());
    CHECK(resting.resting);
    CHECK(resting.unfilledQty == 10);
    CHECK(resting.trades.empty());

    auto duplicate = book.submitOrder(1, Side::SELL, OrderType::LIMIT, 10000, 1, 11);
    CHECK(duplicate.status == OrderStatus::REJECTED_DUPLICATE_ID);
    CHECK(!duplicate.accepted());
    CHECK(book.orderCount() == 1);

    auto invalid = book.submitOrder(2, static_cast<Side>(9), OrderType::LIMIT, 10000, 1, 12);
    CHECK(invalid.status == OrderStatus::REJECTED_INVALID_INPUT);
    CHECK(book.submitOrder(2, Side::BUY, static_cast<OrderType>(9), 10000, 1).status == OrderStatus::REJECTED_INVALID_INPUT);
    CHECK(book.submitOrder(2, Side::BUY, OrderType::LIMIT, 0, 1).status == OrderStatus::REJECTED_INVALID_INPUT);
    CHECK(book.submitOrder(0, Side::BUY, OrderType::LIMIT, 10000, 1).status == OrderStatus::REJECTED_INVALID_INPUT);
    CHECK(book.orderCount() == 1);

    auto fok = book.submitOrder(3, Side::SELL, OrderType::FOK, 10000, 11, 13);
    CHECK(fok.status == OrderStatus::REJECTED_FOK_UNFILLED);
    CHECK(book.orderCount() == 1);

    auto fill = book.submitOrder(4, Side::SELL, OrderType::IOC, 10000, 6, 14);
    CHECK(fill.status == OrderStatus::ACCEPTED_WITH_FILLS);
    CHECK(fill.trades.size() == 1);
    CHECK(fill.unfilledQty == 0);
    CHECK(!fill.resting);
    CHECK(book.getOrder(1)->remainingQty == 4);
    std::cout << "[TEST] Typed order results... PASSED\n";
}

void testPoolRejectsDoubleDeallocate() {
    OrderPool pool(1);
    OrderPool otherPool(1);
    Order* first = pool.allocate();
    CHECK(pool.activeCount() == 1);
    CHECK(!otherPool.deallocate(first));
    CHECK(pool.activeCount() == 1);
    CHECK(pool.deallocate(first));
    CHECK(!pool.deallocate(first));
    CHECK(pool.activeCount() == 0);
    Order* reused = pool.allocate();
    CHECK(reused == first);
    CHECK(pool.activeCount() == 1);
    CHECK(pool.deallocate(reused));
    CHECK(pool.activeCount() == 0);
    MemoryPool<Order, 2> growing(1);
    Order* one = growing.allocate();
    Order* two = growing.allocate();
    Order* three = growing.allocate();
    CHECK(one != two && two != three && one != three);
    CHECK(growing.totalCapacity() == 3);
    CHECK(growing.activeCount() == 3);
    CHECK(growing.deallocate(one));
    CHECK(growing.deallocate(two));
    CHECK(growing.deallocate(three));
    CHECK(growing.activeCount() == 0);
    std::cout << "[TEST] Pool ownership, double-free, growth and reuse... PASSED\n";
}

void testVolumeCapacityPreservesBook() {
    const Quantity maximum = std::numeric_limits<Quantity>::max();
    OrderBook book(8);
    CHECK(book.submitOrder(1, Side::BUY, OrderType::LIMIT, 9900, maximum - 1).accepted());
    CHECK(book.submitOrder(2, Side::BUY, OrderType::LIMIT, 9900, 1).accepted());
    CHECK(book.getBidVolume() == maximum);
    CHECK(book.getL2Snapshot(1).bids[0].quantity == maximum);
    CHECK(book.submitOrder(3, Side::BUY, OrderType::LIMIT, 9800, 1).status == OrderStatus::REJECTED_CAPACITY);
    CHECK(!book.modifyOrder(2, 2));
    CHECK(book.getOrder(2)->remainingQty == 1);
    CHECK(book.submitOrder(4, Side::SELL, OrderType::LIMIT, 10000, 1).accepted());
    // Even a partially marketable limit must be rejected before executing any fill.
    CHECK(book.submitOrder(5, Side::BUY, OrderType::LIMIT, 10000, 2).status == OrderStatus::REJECTED_CAPACITY);
    CHECK(book.getOrder(4)->remainingQty == 1);
    CHECK(book.submitOrder(6, Side::BUY, OrderType::LIMIT, 10000, 1).trades.size() == 1);
    CHECK(book.getAskVolume() == 0);
    auto sweep = book.submitOrder(7, Side::SELL, OrderType::MARKET, 0, maximum);
    CHECK(sweep.unfilledQty == 0 && sweep.trades.size() == 2);
    CHECK(book.getBidVolume() == 0);
    CHECK(book.orderCount() == 0);
    CHECK(book.poolActiveCount() == 0);
    CHECK(book.submitOrder(8, Side::SELL, OrderType::LIMIT, 10000, maximum).accepted());
    CHECK(book.submitOrder(9, Side::SELL, OrderType::LIMIT, 10100, 1).status == OrderStatus::REJECTED_CAPACITY);
    book.clear();
    CHECK(book.getBidVolume() == 0 && book.getAskVolume() == 0);
    std::cout << "[TEST] Volume capacity and rejection before mutation... PASSED\n";
}

class ReferenceBook {
public:
    struct RefOrder {
        OrderId id;
        Side side;
        Price price;
        Quantity qty;
        uint64_t sequence;
    };

    OrderResult submit(OrderId id, Side side, OrderType type, Price price, Quantity qty, Timestamp ts) {
        if (id == 0 || qty == 0 || (side != Side::BUY && side != Side::SELL) ||
            (type != OrderType::LIMIT && type != OrderType::MARKET &&
             type != OrderType::IOC && type != OrderType::FOK) ||
            (type != OrderType::MARKET && price == 0)) {
            return {OrderStatus::REJECTED_INVALID_INPUT, {}, qty, false};
        }
        if (find(id) != m_orders.end()) {
            return {OrderStatus::REJECTED_DUPLICATE_ID, {}, qty, false};
        }

        if (type == OrderType::FOK) {
            uint64_t available = 0;
            for (const RefOrder& maker : m_orders) {
                if (maker.side == side) continue;
                const bool crosses = side == Side::BUY ? maker.price <= price : maker.price >= price;
                if (crosses) available += maker.qty;
            }
            if (available < qty) return {OrderStatus::REJECTED_FOK_UNFILLED, {}, qty, false};
        }

        Quantity remaining = qty;
        std::vector<Trade> trades;
        while (remaining > 0) {
            auto makerIt = bestMaker(side, type, price);
            if (makerIt == m_orders.end()) break;
            const Quantity fill = std::min(remaining, makerIt->qty);
            trades.push_back({makerIt->id, id, side, makerIt->price, fill, ts});
            remaining -= fill;
            makerIt->qty -= fill;
            if (makerIt->qty == 0) m_orders.erase(makerIt);
        }

        bool resting = false;
        if (remaining > 0 && type == OrderType::LIMIT) {
            m_orders.push_back({id, side, price, remaining, m_nextSequence++});
            resting = true;
        }
        return {trades.empty() ? OrderStatus::ACCEPTED_NO_FILL : OrderStatus::ACCEPTED_WITH_FILLS,
                std::move(trades), remaining, resting};
    }

    bool cancel(OrderId id) {
        auto it = find(id);
        if (it == m_orders.end()) return false;
        m_orders.erase(it);
        return true;
    }

    bool modify(OrderId id, Quantity newQty) {
        auto it = find(id);
        if (it == m_orders.end() || newQty == 0) return false;
        if (newQty > it->qty) {
            RefOrder updated = *it;
            m_orders.erase(it);
            updated.qty = newQty;
            updated.sequence = m_nextSequence++;
            m_orders.push_back(updated);
        } else {
            it->qty = newQty;
        }
        return true;
    }

    const std::vector<RefOrder>& orders() const { return m_orders; }

    std::vector<LevelSummary> levels(Side side) const {
        std::map<Price, std::pair<uint64_t, uint32_t>> aggregated;
        for (const RefOrder& order : m_orders) {
            if (order.side == side) {
                auto& level = aggregated[order.price];
                level.first += order.qty;
                ++level.second;
            }
        }
        std::vector<LevelSummary> output;
        for (const auto& [price, summary] : aggregated) {
            output.push_back({price, static_cast<Quantity>(summary.first), summary.second});
        }
        if (side == Side::BUY) std::reverse(output.begin(), output.end());
        return output;
    }

private:
    std::vector<RefOrder>::iterator find(OrderId id) {
        return std::find_if(m_orders.begin(), m_orders.end(), [id](const RefOrder& o) { return o.id == id; });
    }

    std::vector<RefOrder>::iterator bestMaker(Side takerSide, OrderType type, Price limit) {
        auto best = m_orders.end();
        for (auto it = m_orders.begin(); it != m_orders.end(); ++it) {
            if (it->side == takerSide) continue;
            if (type != OrderType::MARKET &&
                (takerSide == Side::BUY ? it->price > limit : it->price < limit)) continue;
            if (best == m_orders.end() ||
                (takerSide == Side::BUY ? it->price < best->price : it->price > best->price) ||
                (it->price == best->price && it->sequence < best->sequence)) {
                best = it;
            }
        }
        return best;
    }

    std::vector<RefOrder> m_orders;
    uint64_t m_nextSequence{1};
};

void compareBooks(const OrderBook& book, const ReferenceBook& reference, OrderId lastId) {
    CHECK(book.orderCount() == reference.orders().size());
    CHECK(book.poolActiveCount() == book.orderCount());
    Quantity bidVolume = 0;
    Quantity askVolume = 0;
    for (const auto& order : reference.orders()) {
        CHECK(book.hasOrder(order.id));
        const Order* actual = book.getOrder(order.id);
        CHECK(actual != nullptr);
        CHECK(actual->side == order.side);
        CHECK(actual->price == order.price);
        CHECK(actual->remainingQty == order.qty);
        if (order.side == Side::BUY) bidVolume += order.qty;
        else askVolume += order.qty;
    }
    CHECK(book.getBidVolume() == bidVolume);
    CHECK(book.getAskVolume() == askVolume);

    const auto actual = book.getL2Snapshot(100);
    const auto expectedBids = reference.levels(Side::BUY);
    const auto expectedAsks = reference.levels(Side::SELL);
    CHECK(actual.bids.size() == expectedBids.size());
    CHECK(actual.asks.size() == expectedAsks.size());
    for (size_t i = 0; i < expectedBids.size(); ++i) {
        CHECK(actual.bids[i].price == expectedBids[i].price);
        CHECK(actual.bids[i].quantity == expectedBids[i].quantity);
        CHECK(actual.bids[i].orderCount == expectedBids[i].orderCount);
    }
    for (size_t i = 0; i < expectedAsks.size(); ++i) {
        CHECK(actual.asks[i].price == expectedAsks[i].price);
        CHECK(actual.asks[i].quantity == expectedAsks[i].quantity);
        CHECK(actual.asks[i].orderCount == expectedAsks[i].orderCount);
    }
    if (lastId != 0 && !reference.orders().empty()) CHECK(book.hasOrder(lastId) ==
        (std::find_if(reference.orders().begin(), reference.orders().end(),
            [lastId](const ReferenceBook::RefOrder& order) { return order.id == lastId; }) != reference.orders().end()));
}

void compareResults(const OrderResult& actual, const OrderResult& expected) {
    CHECK(actual.status == expected.status);
    CHECK(actual.unfilledQty == expected.unfilledQty);
    CHECK(actual.resting == expected.resting);
    CHECK(actual.trades.size() == expected.trades.size());
    for (size_t i = 0; i < actual.trades.size(); ++i) {
        CHECK(actual.trades[i].makerOrderId == expected.trades[i].makerOrderId);
        CHECK(actual.trades[i].takerOrderId == expected.trades[i].takerOrderId);
        CHECK(actual.trades[i].takerSide == expected.trades[i].takerSide);
        CHECK(actual.trades[i].price == expected.trades[i].price);
        CHECK(actual.trades[i].quantity == expected.trades[i].quantity);
    }
}

void testSeededReferenceSequences() {
    constexpr uint64_t seed = 0x4E4F585553ULL;
    std::mt19937_64 rng(seed);
    OrderBook book(8);
    ReferenceBook reference;
    OrderId nextId = 1;
    size_t currentStep = 0;

    try {
        for (size_t step = 0; step < 5000; ++step) {
            currentStep = step;
            const uint64_t action = rng() % 10;
            if (action < 7) {
                const Side side = (rng() & 1) ? Side::BUY : Side::SELL;
                const OrderType type = static_cast<OrderType>(rng() % 4);
                const Price price = static_cast<Price>(995 + (rng() % 11));
                const Quantity quantity = static_cast<Quantity>(1 + (rng() % 20));
                const OrderId id = (step % 31 == 0 && nextId > 1) ? 1 + (rng() % (nextId - 1)) : nextId++;
                const Timestamp ts = step + 1;
                compareResults(book.submitOrder(id, side, type, price, quantity, ts),
                               reference.submit(id, side, type, price, quantity, ts));
                compareBooks(book, reference, id);
            } else if (action < 9) {
                const OrderId id = rng() % nextId;
                CHECK(book.cancelOrder(id) == reference.cancel(id));
                compareBooks(book, reference, id);
            } else {
                const OrderId id = rng() % nextId;
                const Quantity quantity = static_cast<Quantity>(rng() % 25);
                CHECK(book.modifyOrder(id, quantity) == reference.modify(id, quantity));
                compareBooks(book, reference, id);
            }
        }
    } catch (const std::exception& error) {
        throw std::runtime_error("seeded differential sequence failed (seed=" +
            std::to_string(seed) + ", operation=" + std::to_string(currentStep) + "): " + error.what());
    }
    std::cout << "[TEST] Seeded reference sequences (5000 ops, seed " << seed << ")... PASSED\n";
}

void testCAbiReportsAllFills() {
    COrderResult nullResult{};
    CHECK(noxus_submit_order(nullptr, 1, 0, 0, 10000, 1, 1, &nullResult) == -1);
    CHECK(nullResult.trades == nullptr && nullResult.tradeCount == 0);
    CHECK(!noxus_cancel_order(nullptr, 1));
    uint32_t nullBidCount = 99;
    uint32_t nullAskCount = 99;
    noxus_get_l2_depth(nullptr, 10, nullptr, &nullBidCount, nullptr, &nullAskCount);
    CHECK(nullBidCount == 0 && nullAskCount == 0);
    void* handle = noxus_create_book(128);
    CHECK(handle != nullptr);
    CHECK(noxus_submit_order(handle, 1, 0, 0, 10000, 1, 1, nullptr) == -1);
    CHECK(noxus_order_count(handle) == 0);
    for (uint64_t i = 1; i <= 80; ++i) {
        COrderResult maker{};
        CHECK(noxus_submit_order(handle, i, 1, 0, 10000, 1, i, &maker) == 0);
        CHECK(maker.status == NOXUS_ORDER_ACCEPTED_NO_FILL);
        CHECK(maker.resting == 1);
        noxus_free_order_result(&maker);
    }
    COrderResult taker{};
    CHECK(noxus_submit_order(handle, 1000, 0, 1, 0, 80, 1000, &taker) == 0);
    CHECK(taker.status == NOXUS_ORDER_ACCEPTED_WITH_FILLS);
    CHECK(taker.tradeCount == 80);
    CHECK(taker.unfilledQty == 0);
    noxus_free_order_result(&taker);
    CHECK(noxus_order_count(handle) == 0);

    COrderResult invalid{};
    CHECK(noxus_submit_order(handle, 1001, 9, 0, 10000, 1, 1001, &invalid) == 0);
    CHECK(invalid.status == NOXUS_ORDER_REJECTED_INVALID_INPUT);
    noxus_free_order_result(&invalid);
    noxus_destroy_book(handle);

    handle = noxus_create_book(128);
    for (uint64_t i = 1; i <= 70; ++i) {
        COrderResult maker{};
        CHECK(noxus_submit_order(handle, i, 1, 0, 10000, 1, i, &maker) == 0);
        noxus_free_order_result(&maker);
    }
    CTrade legacyTrades[64]{};
    uint32_t copied = 0;
    const int total = noxus_add_order(handle, 1000, 0, 1, 0, 70, 1000, legacyTrades, 64, &copied);
    CHECK(total == 70);
    CHECK(copied == 64);
    noxus_destroy_book(handle);
    std::cout << "[TEST] C ABI full-fill ownership and legacy truncation reporting... PASSED\n";
}

int main() {
    try {
    std::cout << "==========================================\n";
    std::cout << "   NoxusLOB Matching Engine Unit Tests    \n";
    std::cout << "==========================================\n";

    testBasicInsertion();
    testExactMatch();
    testPriceTimePriorityFIFO();
    testPriceImprovement();
    testOrderCancellation();
    testOrderModification();
    testMarketOrder();
    testFillOrKill();
    testImmediateOrCancel();
    testDuplicateOrderIdRejection();
    testInvalidZeroQuantity();
    testL2DepthSnapshotInvariants();
    testTypedOrderResults();
    testPoolRejectsDoubleDeallocate();
    testVolumeCapacityPreservesBook();
    testSeededReferenceSequences();
    testCAbiReportsAllFills();
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    std::cout << "==========================================\n";
    std::cout << "   ALL MATCHING ENGINE TESTS PASSED!      \n";
    std::cout << "==========================================\n";
    return 0;
}
