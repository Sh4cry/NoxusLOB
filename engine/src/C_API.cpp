#include "C_API.h"
#include "OrderBook.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <new>

using namespace noxus;

static_assert(sizeof(CTrade) == 33, "CTrade layout must match the ctypes packed structure");
static_assert(sizeof(CLevel) == 12, "CLevel layout must match the ctypes packed structure");

namespace {
void copyTrade(const Trade& source, CTrade& target) {
    target.makerOrderId = source.makerOrderId;
    target.takerOrderId = source.takerOrderId;
    target.takerSide = static_cast<uint8_t>(source.takerSide);
    target.price = source.price;
    target.quantity = source.quantity;
    target.timestamp = source.timestamp;
}

void zeroResult(COrderResult* result) noexcept {
    if (result) {
        result->status = NOXUS_ORDER_REJECTED_INVALID_INPUT;
        result->resting = 0;
        result->reserved = 0;
        result->unfilledQty = 0;
        result->trades = nullptr;
        result->tradeCount = 0;
    }
}
}

extern "C" {

void* noxus_create_book(uint32_t capacity) {
    try {
        return new OrderBook(capacity > 0 ? capacity : 100000);
    } catch (...) {
        return nullptr;
    }
}

void noxus_destroy_book(void* bookPtr) {
    try {
        delete static_cast<OrderBook*>(bookPtr);
    } catch (...) {
    }
}

void noxus_clear_book(void* bookPtr) {
    try {
        if (bookPtr) static_cast<OrderBook*>(bookPtr)->clear();
    } catch (...) {
    }
}

int noxus_submit_order(
    void* bookPtr,
    uint64_t id,
    uint8_t side,
    uint8_t type,
    uint32_t price,
    uint32_t qty,
    uint64_t ts,
    COrderResult* outResult
) {
    zeroResult(outResult);
    if (!bookPtr || !outResult) return -1;

    try {
        auto* book = static_cast<OrderBook*>(bookPtr);
        const size_t outputCapacity = std::min<size_t>(book->orderCount(), qty);
        std::unique_ptr<CTrade[]> trades;
        if (outputCapacity > 0) {
            trades.reset(new (std::nothrow) CTrade[outputCapacity]);
            if (!trades) return -2;
        }

        auto result = book->submitOrder(
            id, static_cast<Side>(side), static_cast<OrderType>(type), price, qty, ts);
        if (result.trades.size() > std::numeric_limits<uint32_t>::max()) return -3;
        const auto count = static_cast<uint32_t>(result.trades.size());
        if (count > outputCapacity) return -3;
        for (uint32_t i = 0; i < count; ++i) copyTrade(result.trades[i], trades[i]);

        outResult->status = static_cast<uint8_t>(result.status);
        outResult->resting = result.resting ? 1 : 0;
        outResult->unfilledQty = result.unfilledQty;
        outResult->trades = trades.release();
        outResult->tradeCount = count;
        return 0;
    } catch (...) {
        zeroResult(outResult);
        return -3;
    }
}

void noxus_free_order_result(COrderResult* result) {
    if (!result) return;
    delete[] result->trades;
    zeroResult(result);
}

int noxus_add_order(
    void* bookPtr,
    uint64_t id,
    uint8_t side,
    uint8_t type,
    uint32_t price,
    uint32_t qty,
    uint64_t ts,
    CTrade* outTrades,
    uint32_t maxTrades,
    uint32_t* outTradeCount
) {
    COrderResult result{};
    const int apiResult = noxus_submit_order(bookPtr, id, side, type, price, qty, ts, &result);
    if (apiResult != 0) {
        if (outTradeCount) *outTradeCount = 0;
        return apiResult;
    }

    const uint32_t copied = outTrades ? std::min(result.tradeCount, maxTrades) : 0;
    if (outTradeCount) *outTradeCount = copied;
    if (outTrades && copied > 0) {
        std::copy_n(result.trades, copied, outTrades);
    }
    const int total = static_cast<int>(std::min<uint32_t>(
        result.tradeCount, static_cast<uint32_t>(std::numeric_limits<int>::max())));
    noxus_free_order_result(&result);
    return total;
}

bool noxus_cancel_order(void* bookPtr, uint64_t id) {
    try {
        return bookPtr && static_cast<OrderBook*>(bookPtr)->cancelOrder(id);
    } catch (...) {
        return false;
    }
}

bool noxus_modify_order(void* bookPtr, uint64_t id, uint32_t newQty) {
    try {
        return bookPtr && static_cast<OrderBook*>(bookPtr)->modifyOrder(id, newQty);
    } catch (...) {
        return false;
    }
}

bool noxus_has_order(void* bookPtr, uint64_t id) {
    try {
        return bookPtr && static_cast<OrderBook*>(bookPtr)->hasOrder(id);
    } catch (...) {
        return false;
    }
}

uint32_t noxus_order_count(void* bookPtr) {
    try {
        if (!bookPtr) return 0;
        const size_t count = static_cast<OrderBook*>(bookPtr)->orderCount();
        return static_cast<uint32_t>(std::min<size_t>(count, std::numeric_limits<uint32_t>::max()));
    } catch (...) {
        return 0;
    }
}

bool noxus_get_bbo(void* bookPtr, uint32_t* outBid, uint32_t* outAsk) {
    try {
        if (!bookPtr) return false;
        auto* book = static_cast<OrderBook*>(bookPtr);
        const auto bid = book->getBestBid();
        const auto ask = book->getBestAsk();
        if (outBid) *outBid = bid.value_or(0);
        if (outAsk) *outAsk = ask.value_or(0);
        return bid.has_value() || ask.has_value();
    } catch (...) {
        return false;
    }
}

void noxus_get_l2_depth(
    void* bookPtr,
    uint32_t maxLevels,
    CLevel* outBids,
    uint32_t* outBidCount,
    CLevel* outAsks,
    uint32_t* outAskCount
) {
    if (outBidCount) *outBidCount = 0;
    if (outAskCount) *outAskCount = 0;
    try {
        if (!bookPtr) return;
        auto snapshot = static_cast<OrderBook*>(bookPtr)->getL2Snapshot(maxLevels);
        if (outBids && outBidCount) {
            const auto count = static_cast<uint32_t>(std::min<size_t>(maxLevels, snapshot.bids.size()));
            *outBidCount = count;
            for (uint32_t i = 0; i < count; ++i) {
                outBids[i] = {snapshot.bids[i].price, snapshot.bids[i].quantity, snapshot.bids[i].orderCount};
            }
        }
        if (outAsks && outAskCount) {
            const auto count = static_cast<uint32_t>(std::min<size_t>(maxLevels, snapshot.asks.size()));
            *outAskCount = count;
            for (uint32_t i = 0; i < count; ++i) {
                outAsks[i] = {snapshot.asks[i].price, snapshot.asks[i].quantity, snapshot.asks[i].orderCount};
            }
        }
    } catch (...) {
        if (outBidCount) *outBidCount = 0;
        if (outAskCount) *outAskCount = 0;
    }
}

void* nexus_create_book(uint32_t capacity) { return noxus_create_book(capacity); }
void nexus_destroy_book(void* bookPtr) { noxus_destroy_book(bookPtr); }
void nexus_clear_book(void* bookPtr) { noxus_clear_book(bookPtr); }
int nexus_submit_order(void* bookPtr, uint64_t id, uint8_t side, uint8_t type, uint32_t price, uint32_t qty, uint64_t ts, COrderResult* outResult) {
    return noxus_submit_order(bookPtr, id, side, type, price, qty, ts, outResult);
}
void nexus_free_order_result(COrderResult* result) { noxus_free_order_result(result); }
int nexus_add_order(void* bookPtr, uint64_t id, uint8_t side, uint8_t type, uint32_t price, uint32_t qty, uint64_t ts, CTrade* outTrades, uint32_t maxTrades, uint32_t* outTradeCount) {
    return noxus_add_order(bookPtr, id, side, type, price, qty, ts, outTrades, maxTrades, outTradeCount);
}
bool nexus_cancel_order(void* bookPtr, uint64_t id) { return noxus_cancel_order(bookPtr, id); }
bool nexus_modify_order(void* bookPtr, uint64_t id, uint32_t newQty) { return noxus_modify_order(bookPtr, id, newQty); }
bool nexus_has_order(void* bookPtr, uint64_t id) { return noxus_has_order(bookPtr, id); }
uint32_t nexus_order_count(void* bookPtr) { return noxus_order_count(bookPtr); }
bool nexus_get_bbo(void* bookPtr, uint32_t* outBid, uint32_t* outAsk) { return noxus_get_bbo(bookPtr, outBid, outAsk); }
void nexus_get_l2_depth(void* bookPtr, uint32_t maxLevels, CLevel* outBids, uint32_t* outBidCount, CLevel* outAsks, uint32_t* outAskCount) {
    noxus_get_l2_depth(bookPtr, maxLevels, outBids, outBidCount, outAsks, outAskCount);
}

} // extern "C"
