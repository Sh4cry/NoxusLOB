#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#define NOXUS_EXPORT __declspec(dllexport)
#else
#define NOXUS_EXPORT __attribute__((visibility("default")))
#endif
#define NEXUS_EXPORT NOXUS_EXPORT

#pragma pack(push, 1)
typedef struct {
    uint64_t makerOrderId;
    uint64_t takerOrderId;
    uint8_t takerSide;
    uint32_t price;
    uint32_t quantity;
    uint64_t timestamp;
} CTrade;

typedef struct {
    uint32_t price;
    uint32_t quantity;
    uint32_t orderCount;
} CLevel;
#pragma pack(pop)

typedef struct {
    uint8_t status;
    uint8_t resting;
    uint16_t reserved;
    uint32_t unfilledQty;
    CTrade* trades;
    uint32_t tradeCount;
} COrderResult;

enum {
    NOXUS_ORDER_ACCEPTED_NO_FILL = 0,
    NOXUS_ORDER_ACCEPTED_WITH_FILLS = 1,
    NOXUS_ORDER_REJECTED_INVALID_INPUT = 2,
    NOXUS_ORDER_REJECTED_DUPLICATE_ID = 3,
    NOXUS_ORDER_REJECTED_FOK_UNFILLED = 4,
    NOXUS_ORDER_REJECTED_CAPACITY = 5
};

// Primary Noxus C API
NOXUS_EXPORT void* noxus_create_book(uint32_t capacity);
NOXUS_EXPORT void noxus_destroy_book(void* bookPtr);
NOXUS_EXPORT void noxus_clear_book(void* bookPtr);
// Result owns its trade array until noxus_free_order_result is called.
// Returns 0 on API success, -1 for invalid arguments, -2 for allocation failure,
// and -3 for an unexpected C++ exception.
NOXUS_EXPORT int noxus_submit_order(
    void* bookPtr,
    uint64_t id,
    uint8_t side,
    uint8_t type,
    uint32_t price,
    uint32_t qty,
    uint64_t ts,
    COrderResult* outResult
);
NOXUS_EXPORT void noxus_free_order_result(COrderResult* result);

// Compatibility API. The return value is the total number of fills produced;
// outTradeCount is the number copied into outTrades. A smaller copied count
// explicitly indicates truncation. Prefer noxus_submit_order for full results.
NOXUS_EXPORT int noxus_add_order(
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
);

NOXUS_EXPORT bool noxus_cancel_order(void* bookPtr, uint64_t id);
NOXUS_EXPORT bool noxus_modify_order(void* bookPtr, uint64_t id, uint32_t newQty);
NOXUS_EXPORT bool noxus_has_order(void* bookPtr, uint64_t id);

NOXUS_EXPORT uint32_t noxus_order_count(void* bookPtr);
NOXUS_EXPORT bool noxus_get_bbo(void* bookPtr, uint32_t* outBid, uint32_t* outAsk);

NOXUS_EXPORT void noxus_get_l2_depth(
    void* bookPtr,
    uint32_t maxLevels,
    CLevel* outBids,
    uint32_t* outBidCount,
    CLevel* outAsks,
    uint32_t* outAskCount
);

// Backward Compatibility Aliases
NEXUS_EXPORT void* nexus_create_book(uint32_t capacity);
NEXUS_EXPORT void nexus_destroy_book(void* bookPtr);
NEXUS_EXPORT void nexus_clear_book(void* bookPtr);
NEXUS_EXPORT int nexus_submit_order(
    void* bookPtr,
    uint64_t id,
    uint8_t side,
    uint8_t type,
    uint32_t price,
    uint32_t qty,
    uint64_t ts,
    COrderResult* outResult
);
NEXUS_EXPORT void nexus_free_order_result(COrderResult* result);

NEXUS_EXPORT int nexus_add_order(
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
);

NEXUS_EXPORT bool nexus_cancel_order(void* bookPtr, uint64_t id);
NEXUS_EXPORT bool nexus_modify_order(void* bookPtr, uint64_t id, uint32_t newQty);
NEXUS_EXPORT bool nexus_has_order(void* bookPtr, uint64_t id);

NEXUS_EXPORT uint32_t nexus_order_count(void* bookPtr);
NEXUS_EXPORT bool nexus_get_bbo(void* bookPtr, uint32_t* outBid, uint32_t* outAsk);

NEXUS_EXPORT void nexus_get_l2_depth(
    void* bookPtr,
    uint32_t maxLevels,
    CLevel* outBids,
    uint32_t* outBidCount,
    CLevel* outAsks,
    uint32_t* outAskCount
);

#ifdef __cplusplus
}
#endif
