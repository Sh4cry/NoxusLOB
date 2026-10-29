#pragma once

#include "Types.hpp"

namespace noxus {

struct Order {
    OrderId id{0};
    Price price{0};
    Quantity initialQty{0};
    Quantity remainingQty{0};
    Side side{Side::BUY};
    OrderType type{OrderType::LIMIT};
    Timestamp timestamp{0};

    // Intrusive doubly linked list pointers for O(1) queue insertion and unlinking
    Order* prev{nullptr};
    Order* next{nullptr};
    bool poolInUse{false};
    const void* poolOwner{nullptr};

    void reset() {
        id = 0;
        price = 0;
        initialQty = 0;
        remainingQty = 0;
        side = Side::BUY;
        type = OrderType::LIMIT;
        timestamp = 0;
        prev = nullptr;
        next = nullptr;
        poolInUse = false;
        poolOwner = nullptr;
    }

    bool isFilled() const {
        return remainingQty == 0;
    }
};

} // namespace noxus
