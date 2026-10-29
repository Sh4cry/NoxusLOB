#pragma once

#include "Types.hpp"
#include "Order.hpp"
#include "PriceLevel.hpp"
#include "MemoryPool.hpp"
#include <map>
#include <unordered_map>
#include <vector>
#include <optional>
#include <functional>

namespace noxus {

class OrderBook {
public:
    explicit OrderBook(size_t initialPoolCapacity = 100000);
    ~OrderBook();

    // Core Matching Engine API
    OrderResult submitOrder(OrderId id, Side side, OrderType type, Price price, Quantity qty, Timestamp ts = 0);
    // Compatibility wrapper. Use submitOrder when rejection status matters.
    std::vector<Trade> addOrder(OrderId id, Side side, OrderType type, Price price, Quantity qty, Timestamp ts = 0);
    bool cancelOrder(OrderId id);
    bool modifyOrder(OrderId id, Quantity newQty);

    // Order Queries
    bool hasOrder(OrderId id) const noexcept;
    const Order* getOrder(OrderId id) const noexcept;
    size_t orderCount() const noexcept { return m_orderIndex.size(); }

    // Market State Queries
    std::optional<Price> getBestBid() const noexcept;
    std::optional<Price> getBestAsk() const noexcept;
    std::optional<Price> getSpread() const noexcept;
    std::optional<double> getMidPrice() const noexcept;

    Quantity getBidVolume() const noexcept;
    Quantity getAskVolume() const noexcept;

    // L2 Depth Snapshot (e.g. top 10/20 levels)
    L2Snapshot getL2Snapshot(size_t depth = 15) const;

    // Reset book & pool
    void clear();

    // Pool capacity query
    size_t poolActiveCount() const noexcept { return m_pool.activeCount(); }

private:
    // Internal matching routines
    void matchBuyOrder(Order* order, std::vector<Trade>& trades);
    void matchSellOrder(Order* order, std::vector<Trade>& trades);

    void addOrderToBook(Order* order);
    void removeOrderFromBook(Order* order);

    // Book storage:
    // Bids sorted descending (highest bid at begin())
    std::map<Price, PriceLevel, std::greater<Price>> m_bids;
    // Asks sorted ascending (lowest ask at begin())
    std::map<Price, PriceLevel, std::less<Price>> m_asks;

    // O(1) order lookup
    std::unordered_map<OrderId, Order*> m_orderIndex;
    Quantity m_bidVolume{0};
    Quantity m_askVolume{0};

    // Recycled order-node pool; grows when its preallocated capacity is exhausted.
    OrderPool m_pool;
};

} // namespace noxus
