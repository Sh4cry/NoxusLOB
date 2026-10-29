#include "OrderBook.hpp"
#include <algorithm>
#include <limits>

namespace noxus {

OrderBook::OrderBook(size_t initialPoolCapacity)
    : m_pool(initialPoolCapacity) {
    m_orderIndex.reserve(initialPoolCapacity);
}

OrderBook::~OrderBook() {
    clear();
}

void OrderBook::clear() {
    for (auto& [id, order] : m_orderIndex) {
        m_pool.deallocate(order);
    }
    m_orderIndex.clear();
    m_bids.clear();
    m_asks.clear();
    m_bidVolume = 0;
    m_askVolume = 0;
}

bool OrderBook::hasOrder(OrderId id) const noexcept {
    return m_orderIndex.find(id) != m_orderIndex.end();
}

const Order* OrderBook::getOrder(OrderId id) const noexcept {
    auto it = m_orderIndex.find(id);
    return (it != m_orderIndex.end()) ? it->second : nullptr;
}

std::optional<Price> OrderBook::getBestBid() const noexcept {
    if (m_bids.empty()) return std::nullopt;
    return m_bids.begin()->first;
}

std::optional<Price> OrderBook::getBestAsk() const noexcept {
    if (m_asks.empty()) return std::nullopt;
    return m_asks.begin()->first;
}

std::optional<Price> OrderBook::getSpread() const noexcept {
    if (m_bids.empty() || m_asks.empty()) return std::nullopt;
    Price bestBid = m_bids.begin()->first;
    Price bestAsk = m_asks.begin()->first;
    if (bestAsk >= bestBid) {
        return bestAsk - bestBid;
    }
    return 0; // Cross book edge-case
}

std::optional<double> OrderBook::getMidPrice() const noexcept {
    if (m_bids.empty() || m_asks.empty()) return std::nullopt;
    return (static_cast<double>(m_bids.begin()->first) + static_cast<double>(m_asks.begin()->first)) / 2.0;
}

Quantity OrderBook::getBidVolume() const noexcept {
    return m_bidVolume;
}

Quantity OrderBook::getAskVolume() const noexcept {
    return m_askVolume;
}

OrderResult OrderBook::submitOrder(OrderId id, Side side, OrderType type, Price price, Quantity qty, Timestamp ts) {
    const bool validSide = side == Side::BUY || side == Side::SELL;
    const bool validType = type == OrderType::LIMIT || type == OrderType::MARKET ||
                           type == OrderType::IOC || type == OrderType::FOK;
    if (!validSide || !validType || id == 0 || qty == 0 ||
        (type != OrderType::MARKET && price == 0)) {
        return {OrderStatus::REJECTED_INVALID_INPUT, {}, qty, false};
    }
    if (hasOrder(id)) {
        return {OrderStatus::REJECTED_DUPLICATE_ID, {}, qty, false};
    }

    if (ts == 0) {
        ts = currentTimestampNs();
    }

    // Preflight liquidity, volume capacity and fill storage before changing the book.
    size_t maxTradeCount = 0;
    uint64_t availableVolume = 0;
    if (side == Side::BUY) {
        for (const auto& [askPrice, level] : m_asks) {
            if (type != OrderType::MARKET && askPrice > price) break;
            maxTradeCount += level.orderCount();
            availableVolume += level.totalQuantity();
            if (availableVolume >= qty) break;
        }
    } else {
        for (const auto& [bidPrice, level] : m_bids) {
            if (type != OrderType::MARKET && bidPrice < price) break;
            maxTradeCount += level.orderCount();
            availableVolume += level.totalQuantity();
            if (availableVolume >= qty) break;
        }
    }
    if (type == OrderType::FOK && availableVolume < qty) {
        return {OrderStatus::REJECTED_FOK_UNFILLED, {}, qty, false};
    }
    const auto expectedRemainder = static_cast<Quantity>(qty - std::min<uint64_t>(availableVolume, qty));
    const Quantity ownVolume = side == Side::BUY ? m_bidVolume : m_askVolume;
    if (type == OrderType::LIMIT && expectedRemainder > std::numeric_limits<Quantity>::max() - ownVolume) {
        return {OrderStatus::REJECTED_CAPACITY, {}, qty, false};
    }
    std::vector<Trade> trades;
    trades.reserve(std::min<size_t>(maxTradeCount, qty));

    // Allocate and initialize only after preflight allocations have succeeded.
    Order* order = m_pool.allocate();
    order->id = id;
    order->price = price;
    order->initialQty = qty;
    order->remainingQty = qty;
    order->side = side;
    order->type = type;
    order->timestamp = ts;

    try {
        if (side == Side::BUY) {
            matchBuyOrder(order, trades);
        } else {
            matchSellOrder(order, trades);
        }
    } catch (...) {
        if (order->poolInUse) m_pool.deallocate(order);
        throw;
    }

    Quantity filledQty = 0;
    for (const Trade& trade : trades) {
        filledQty += trade.quantity;
    }
    const bool resting = hasOrder(id);
    return {
        trades.empty() ? OrderStatus::ACCEPTED_NO_FILL : OrderStatus::ACCEPTED_WITH_FILLS,
        std::move(trades),
        static_cast<Quantity>(qty - filledQty),
        resting
    };
}

std::vector<Trade> OrderBook::addOrder(OrderId id, Side side, OrderType type, Price price, Quantity qty, Timestamp ts) {
    return submitOrder(id, side, type, price, qty, ts).trades;
}

void OrderBook::matchBuyOrder(Order* order, std::vector<Trade>& trades) {
    while (!m_asks.empty() && order->remainingQty > 0) {
        auto askIt = m_asks.begin();
        Price bestAsk = askIt->first;

        // If LIMIT order and bestAsk exceeds limit price, stop matching
        if (order->type != OrderType::MARKET && bestAsk > order->price) {
            break;
        }

        PriceLevel& level = askIt->second;
        while (!level.isEmpty() && order->remainingQty > 0) {
            Order* maker = level.head();
            Quantity fillQty = std::min(order->remainingQty, maker->remainingQty);

            Trade trade{
                .makerOrderId = maker->id,
                .takerOrderId = order->id,
                .takerSide = Side::BUY,
                .price = bestAsk,
                .quantity = fillQty,
                .timestamp = order->timestamp
            };
            trades.push_back(trade);

            order->remainingQty -= fillQty;
            maker->remainingQty -= fillQty;
            level.decreaseQuantity(fillQty);
            m_askVolume -= fillQty;

            if (maker->isFilled()) {
                m_orderIndex.erase(maker->id);
                level.remove(maker);
                m_pool.deallocate(maker);
            }
        }

        if (level.isEmpty()) {
            m_asks.erase(askIt);
        }
    }

    if (order->remainingQty > 0) {
        if (order->type == OrderType::LIMIT) {
            // Resting limit order added to book
            addOrderToBook(order);
        } else {
            // MARKET or IOC remainder is cancelled
            m_pool.deallocate(order);
        }
    } else {
        // Taker fully filled
        m_pool.deallocate(order);
    }

}

void OrderBook::matchSellOrder(Order* order, std::vector<Trade>& trades) {
    while (!m_bids.empty() && order->remainingQty > 0) {
        auto bidIt = m_bids.begin();
        Price bestBid = bidIt->first;

        // If LIMIT order and bestBid is below limit price, stop matching
        if (order->type != OrderType::MARKET && bestBid < order->price) {
            break;
        }

        PriceLevel& level = bidIt->second;
        while (!level.isEmpty() && order->remainingQty > 0) {
            Order* maker = level.head();
            Quantity fillQty = std::min(order->remainingQty, maker->remainingQty);

            Trade trade{
                .makerOrderId = maker->id,
                .takerOrderId = order->id,
                .takerSide = Side::SELL,
                .price = bestBid,
                .quantity = fillQty,
                .timestamp = order->timestamp
            };
            trades.push_back(trade);

            order->remainingQty -= fillQty;
            maker->remainingQty -= fillQty;
            level.decreaseQuantity(fillQty);
            m_bidVolume -= fillQty;

            if (maker->isFilled()) {
                m_orderIndex.erase(maker->id);
                level.remove(maker);
                m_pool.deallocate(maker);
            }
        }

        if (level.isEmpty()) {
            m_bids.erase(bidIt);
        }
    }

    if (order->remainingQty > 0) {
        if (order->type == OrderType::LIMIT) {
            // Resting limit order added to book
            addOrderToBook(order);
        } else {
            // MARKET or IOC remainder is cancelled
            m_pool.deallocate(order);
        }
    } else {
        // Taker fully filled
        m_pool.deallocate(order);
    }

}

void OrderBook::addOrderToBook(Order* order) {
    auto appendToSide = [this, order](auto& side) {
        auto level = side.find(order->price);
        const bool createdLevel = level == side.end();
        if (createdLevel) level = side.emplace(order->price, PriceLevel(order->price)).first;
        try {
            const auto [index, inserted] = m_orderIndex.emplace(order->id, order);
            (void)index;
            if (!inserted) {
                if (createdLevel) side.erase(level);
                return false;
            }
            level->second.append(order);
            auto& volume = order->side == Side::BUY ? m_bidVolume : m_askVolume;
            volume += order->remainingQty;
            return true;
        } catch (...) {
            if (createdLevel) side.erase(level);
            throw;
        }
    };
    if (order->side == Side::BUY) {
        if (!appendToSide(m_bids)) m_pool.deallocate(order);
    } else if (!appendToSide(m_asks)) {
        m_pool.deallocate(order);
    }
}

void OrderBook::removeOrderFromBook(Order* order) {
    if (order->side == Side::BUY) {
        auto it = m_bids.find(order->price);
        if (it != m_bids.end()) {
            it->second.remove(order);
            if (it->second.isEmpty()) {
                m_bids.erase(it);
            }
        }
    } else {
        auto it = m_asks.find(order->price);
        if (it != m_asks.end()) {
            it->second.remove(order);
            if (it->second.isEmpty()) {
                m_asks.erase(it);
            }
        }
    }
}

bool OrderBook::cancelOrder(OrderId id) {
    auto it = m_orderIndex.find(id);
    if (it == m_orderIndex.end()) {
        return false;
    }

    Order* order = it->second;
    auto& volume = order->side == Side::BUY ? m_bidVolume : m_askVolume;
    volume -= order->remainingQty;
    removeOrderFromBook(order);
    m_orderIndex.erase(it);
    m_pool.deallocate(order);
    return true;
}

bool OrderBook::modifyOrder(OrderId id, Quantity newQty) {
    auto it = m_orderIndex.find(id);
    if (it == m_orderIndex.end() || newQty == 0) {
        return false;
    }

    Order* order = it->second;
    if (newQty < order->remainingQty) {
        // Reducing quantity retains queue priority!
        Quantity diff = order->remainingQty - newQty;
        auto& volume = order->side == Side::BUY ? m_bidVolume : m_askVolume;
        volume -= diff;
        order->remainingQty = newQty;
        if (order->side == Side::BUY) {
            m_bids[order->price].decreaseQuantity(diff);
        } else {
            m_asks[order->price].decreaseQuantity(diff);
        }
        return true;
    } else if (newQty > order->remainingQty) {
        auto& volume = order->side == Side::BUY ? m_bidVolume : m_askVolume;
        const Quantity increase = newQty - order->remainingQty;
        if (increase > std::numeric_limits<Quantity>::max() - volume) return false;
        // The unchanged price remains uncrossed. Requeue without allocating a new node.
        PriceLevel& level = order->side == Side::BUY ? m_bids.at(order->price) : m_asks.at(order->price);
        level.remove(order);
        order->remainingQty = newQty;
        order->initialQty = newQty;
        order->timestamp = currentTimestampNs();
        level.append(order);
        volume += increase;
        return true;
    }

    return true; // No change
}

L2Snapshot OrderBook::getL2Snapshot(size_t depth) const {
    L2Snapshot snapshot;
    snapshot.timestamp = currentTimestampNs();
    snapshot.bids.reserve(std::min(depth, m_bids.size()));
    snapshot.asks.reserve(std::min(depth, m_asks.size()));

    size_t count = 0;
    for (const auto& [price, level] : m_bids) {
        if (count++ >= depth) break;
        snapshot.bids.push_back(LevelSummary{
            .price = price,
            .quantity = level.totalQuantity(),
            .orderCount = level.orderCount()
        });
    }

    count = 0;
    for (const auto& [price, level] : m_asks) {
        if (count++ >= depth) break;
        snapshot.asks.push_back(LevelSummary{
            .price = price,
            .quantity = level.totalQuantity(),
            .orderCount = level.orderCount()
        });
    }

    return snapshot;
}

} // namespace noxus
