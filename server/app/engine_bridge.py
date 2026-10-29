import os
import sys
import ctypes
import shutil
import threading
from functools import wraps
from typing import List, Tuple, Optional, Dict, Any
import time

class CTrade(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("makerOrderId", ctypes.c_uint64),
        ("takerOrderId", ctypes.c_uint64),
        ("takerSide", ctypes.c_uint8),
        ("price", ctypes.c_uint32),
        ("quantity", ctypes.c_uint32),
        ("timestamp", ctypes.c_uint64),
    ]

class CLevel(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("price", ctypes.c_uint32),
        ("quantity", ctypes.c_uint32),
        ("orderCount", ctypes.c_uint32),
    ]

class COrderResult(ctypes.Structure):
    _fields_ = [
        ("status", ctypes.c_uint8),
        ("resting", ctypes.c_uint8),
        ("reserved", ctypes.c_uint16),
        ("unfilledQty", ctypes.c_uint32),
        ("trades", ctypes.POINTER(CTrade)),
        ("tradeCount", ctypes.c_uint32),
    ]

ORDER_STATUS = {
    0: "ACCEPTED_NO_FILL",
    1: "ACCEPTED_WITH_FILLS",
    2: "REJECTED_INVALID_INPUT",
    3: "REJECTED_DUPLICATE_ID",
    4: "REJECTED_FOK_UNFILLED",
    5: "REJECTED_CAPACITY",
}

def _serialized(method):
    @wraps(method)
    def call(self, *args, **kwargs):
        with self._lock:
            return method(self, *args, **kwargs)
    return call

class EngineBridge:
    def __init__(self, dll_path: Optional[str] = None, force_python: bool = False):
        self.is_cpp_backend = False
        self._handle = None
        self._next_order_id = 1000
        self._dll_directory_handles = []
        self._lock = threading.RLock()

        # Try to locate the compiled C++ DLL
        search_paths = [
            dll_path,
            os.environ.get("NOXUS_ENGINE_LIBRARY"),
            os.path.join(os.path.dirname(__file__), "..", "..", "engine", "build", "noxus_engine.dll"),
            os.path.join(os.path.dirname(__file__), "..", "..", "engine", "noxus_engine.dll"),
            os.path.join(os.path.dirname(__file__), "..", "..", "engine", "libnoxus_engine.so"),
            os.path.join(os.path.dirname(__file__), "..", "..", "engine", "libnoxus_engine.dylib"),
            os.path.join(os.path.dirname(__file__), "..", "..", "engine", "nexus_engine.dll"),
            os.path.join(os.path.dirname(__file__), "..", "..", "engine", "build", "nexus_engine.dll"),
            os.path.join(os.path.dirname(__file__), "..", "..", "engine", "libnexus_engine.so"),
            os.path.join(os.path.dirname(__file__), "..", "..", "engine", "libnexus_engine.dylib"),
        ]

        lib_file = None
        for p in search_paths:
            if not force_python and p and os.path.exists(p):
                lib_file = os.path.abspath(p)
                break

        if lib_file:
            try:
                if os.name == "nt" and hasattr(os, "add_dll_directory"):
                    dll_dirs = {os.path.dirname(lib_file)}
                    compiler = shutil.which("g++")
                    if compiler:
                        dll_dirs.add(os.path.dirname(os.path.abspath(compiler)))
                    for directory in dll_dirs:
                        if directory and os.path.isdir(directory):
                            self._dll_directory_handles.append(os.add_dll_directory(directory))
                self.lib = ctypes.CDLL(lib_file)
                self._setup_ffi()
                self._handle = getattr(self.lib, "noxus_create_book", self.lib.nexus_create_book)(200000)
                if not self._handle:
                    raise RuntimeError("C++ engine could not allocate an order book")
                self.is_cpp_backend = True
                print(f"[EngineBridge] Successfully loaded C++ Low-Latency Engine: {lib_file}")
            except Exception as e:
                print(f"[EngineBridge] Failed to load DLL ({e}), using Python fallback.")
                self._init_python_fallback()
        else:
            print("[EngineBridge] C++ binary not found, using Python fallback.")
            self._init_python_fallback()

    def _setup_ffi(self):
        # Map both noxus_* and nexus_* prefixes
        for name in [
            "create_book", "destroy_book", "clear_book", "add_order",
            "submit_order", "free_order_result",
            "cancel_order", "modify_order", "has_order", "order_count",
            "get_bbo", "get_l2_depth"
        ]:
            fn = getattr(self.lib, f"noxus_{name}", getattr(self.lib, f"nexus_{name}", None))
            if fn:
                setattr(self.lib, f"noxus_{name}", fn)
                setattr(self.lib, f"nexus_{name}", fn)

        self.lib.nexus_create_book.argtypes = [ctypes.c_uint32]
        self.lib.nexus_create_book.restype = ctypes.c_void_p

        self.lib.nexus_destroy_book.argtypes = [ctypes.c_void_p]
        self.lib.nexus_destroy_book.restype = None

        self.lib.nexus_clear_book.argtypes = [ctypes.c_void_p]
        self.lib.nexus_clear_book.restype = None

        self.lib.nexus_add_order.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint64,
            ctypes.c_uint8,
            ctypes.c_uint8,
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_uint64,
            ctypes.POINTER(CTrade),
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint32),
        ]
        self.lib.nexus_add_order.restype = ctypes.c_int

        self.lib.nexus_submit_order.argtypes = [
            ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint8, ctypes.c_uint8,
            ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint64,
            ctypes.POINTER(COrderResult),
        ]
        self.lib.nexus_submit_order.restype = ctypes.c_int
        self.lib.nexus_free_order_result.argtypes = [ctypes.POINTER(COrderResult)]
        self.lib.nexus_free_order_result.restype = None

        self.lib.nexus_cancel_order.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
        self.lib.nexus_cancel_order.restype = ctypes.c_bool

        self.lib.nexus_modify_order.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint32]
        self.lib.nexus_modify_order.restype = ctypes.c_bool

        self.lib.nexus_has_order.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
        self.lib.nexus_has_order.restype = ctypes.c_bool

        self.lib.nexus_order_count.argtypes = [ctypes.c_void_p]
        self.lib.nexus_order_count.restype = ctypes.c_uint32

        self.lib.nexus_get_bbo.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(ctypes.c_uint32),
        ]
        self.lib.nexus_get_bbo.restype = ctypes.c_bool

        self.lib.nexus_get_l2_depth.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.POINTER(CLevel),
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(CLevel),
            ctypes.POINTER(ctypes.c_uint32),
        ]
        self.lib.nexus_get_l2_depth.restype = None

    def __del__(self):
        if self.is_cpp_backend and self._handle and hasattr(self, 'lib'):
            self.lib.nexus_destroy_book(self._handle)
            self._handle = None

    @_serialized
    def get_next_order_id(self) -> int:
        self._next_order_id += 1
        return self._next_order_id

    @_serialized
    def submit_order(
        self,
        order_id: Optional[int],
        side: str,
        order_type: str,
        price_dollars: float,
        quantity: int,
    ) -> Dict[str, Any]:
        try:
            order_id = int(order_id) if order_id is not None else 0
            if order_id <= 0:
                order_id = self.get_next_order_id()
            if not 0 < order_id <= 18446744073709551615:
                raise ValueError("order ID is outside the unsigned 64-bit range")
        except (TypeError, ValueError, OverflowError):
            return {"id": order_id, "status": "REJECTED_INVALID_INPUT", "accepted": False,
                    "resting": False, "unfilled_quantity": 0, "trades": []}
        try:
            price_value = float(price_dollars)
            quantity_value = int(quantity)
            if float(quantity) != quantity_value:
                raise ValueError("quantity must be an integer")
            if not (0 <= price_value <= 42949672.95) or not (0 <= quantity_value <= 4294967295):
                raise ValueError("price or quantity is outside the C++ engine range")
            price_cents = int(round(price_value * 100))
        except (TypeError, ValueError, OverflowError):
            return {"id": order_id, "status": "REJECTED_INVALID_INPUT", "accepted": False,
                    "resting": False, "unfilled_quantity": 0, "trades": []}

        side_code = {"BUY": 0, "SELL": 1}.get(str(side).upper(), 255)
        type_map = {"LIMIT": 0, "MARKET": 1, "IOC": 2, "FOK": 3}
        type_code = type_map.get(str(order_type).upper(), 255)

        if self.is_cpp_backend:
            result = COrderResult()
            api_status = self.lib.nexus_submit_order(
                self._handle,
                ctypes.c_uint64(order_id),
                ctypes.c_uint8(side_code),
                ctypes.c_uint8(type_code),
                ctypes.c_uint32(price_cents),
                ctypes.c_uint32(quantity_value),
                ctypes.c_uint64(time.time_ns()),
                ctypes.byref(result),
            )
            if api_status != 0:
                raise RuntimeError(f"C++ engine submission failed (error {api_status})")
            try:
                trades_list = [
                    {
                        "maker_order_id": result.trades[i].makerOrderId,
                        "taker_order_id": result.trades[i].takerOrderId,
                        "taker_side": "BUY" if result.trades[i].takerSide == 0 else "SELL",
                        "price": round(result.trades[i].price / 100.0, 2),
                        "quantity": result.trades[i].quantity,
                        "timestamp_ns": result.trades[i].timestamp,
                    }
                    for i in range(result.tradeCount)
                ]
                return {
                    "id": order_id,
                    "status": ORDER_STATUS.get(result.status, "ENGINE_ERROR"),
                    "accepted": result.status in (0, 1),
                    "resting": bool(result.resting),
                    "unfilled_quantity": result.unfilledQty,
                    "trades": trades_list,
                }
            finally:
                self.lib.nexus_free_order_result(ctypes.byref(result))

        return self._py_submit_order(order_id, side_code, type_code, price_cents, quantity_value)

    def add_order(
        self,
        order_id: Optional[int],
        side: str,
        order_type: str,
        price_dollars: float,
        quantity: int,
    ) -> Tuple[int, List[Dict[str, Any]]]:
        """Compatibility wrapper returning only the assigned ID and fills."""
        if order_id is None or order_id <= 0:
            order_id = self.get_next_order_id()
        result = self.submit_order(order_id, side, order_type, price_dollars, quantity)
        return result["id"], result["trades"]

    @_serialized
    def cancel_order(self, order_id: int) -> bool:
        if not isinstance(order_id, int) or not 0 < order_id <= 18446744073709551615:
            return False
        if self.is_cpp_backend:
            return bool(self.lib.nexus_cancel_order(self._handle, ctypes.c_uint64(order_id)))
        return self._py_cancel_order(order_id)

    @_serialized
    def modify_order(self, order_id: int, new_qty: int) -> bool:
        if (not isinstance(order_id, int) or not 0 < order_id <= 18446744073709551615 or
                not isinstance(new_qty, int) or not 0 < new_qty <= 4294967295):
            return False
        if self.is_cpp_backend:
            return bool(self.lib.nexus_modify_order(self._handle, ctypes.c_uint64(order_id), ctypes.c_uint32(new_qty)))
        return self._py_modify_order(order_id, new_qty)

    @_serialized
    def get_bbo(self) -> Tuple[Optional[float], Optional[float]]:
        if self.is_cpp_backend:
            bid_c = ctypes.c_uint32(0)
            ask_c = ctypes.c_uint32(0)
            has_bbo = self.lib.nexus_get_bbo(self._handle, ctypes.byref(bid_c), ctypes.byref(ask_c))
            best_bid = round(bid_c.value / 100.0, 2) if bid_c.value > 0 else None
            best_ask = round(ask_c.value / 100.0, 2) if ask_c.value > 0 else None
            return best_bid, best_ask
        return self._py_get_bbo()

    @_serialized
    def get_l2_depth(self, depth: int = 15) -> Dict[str, Any]:
        if self.is_cpp_backend:
            bids_arr = (CLevel * depth)()
            asks_arr = (CLevel * depth)()
            num_bids = ctypes.c_uint32(0)
            num_asks = ctypes.c_uint32(0)

            self.lib.nexus_get_l2_depth(
                self._handle,
                ctypes.c_uint32(depth),
                bids_arr,
                ctypes.byref(num_bids),
                asks_arr,
                ctypes.byref(num_asks),
            )

            bids = [
                {
                    "price": round(bids_arr[i].price / 100.0, 2),
                    "quantity": bids_arr[i].quantity,
                    "order_count": bids_arr[i].orderCount,
                }
                for i in range(num_bids.value)
            ]
            asks = [
                {
                    "price": round(asks_arr[i].price / 100.0, 2),
                    "quantity": asks_arr[i].quantity,
                    "order_count": asks_arr[i].orderCount,
                }
                for i in range(num_asks.value)
            ]

            best_bid = bids[0]["price"] if bids else None
            best_ask = asks[0]["price"] if asks else None
            spread = round(best_ask - best_bid, 2) if (best_bid and best_ask) else None
            mid = round((best_bid + best_ask) / 2.0, 2) if (best_bid and best_ask) else None

            return {
                "timestamp_ns": time.time_ns(),
                "symbol": "APEX/USD",
                "bids": bids,
                "asks": asks,
                "best_bid": best_bid,
                "best_ask": best_ask,
                "spread": spread,
                "mid_price": mid,
            }
        return self._py_get_l2_depth(depth)

    @_serialized
    def order_count(self) -> int:
        if self.is_cpp_backend:
            return int(self.lib.nexus_order_count(self._handle))
        return len(self._py_orders)

    @_serialized
    def clear(self):
        if self.is_cpp_backend:
            self.lib.nexus_clear_book(self._handle)
        else:
            self._py_orders.clear()
            self._py_bids.clear()
            self._py_asks.clear()

    # --- Python Fallback Implementation ---
    def _init_python_fallback(self):
        self._py_orders = {}
        self._py_bids = {} # price -> [orders]
        self._py_asks = {} # price -> [orders]

    def _py_submit_order(self, order_id, side_code, type_code, price_cents, quantity):
        invalid = {
            "id": order_id, "status": "REJECTED_INVALID_INPUT", "accepted": False,
            "resting": False, "unfilled_quantity": quantity, "trades": [],
        }
        if (order_id <= 0 or side_code not in (0, 1) or type_code not in range(4) or
                quantity <= 0 or (type_code != 1 and price_cents <= 0)):
            return invalid
        if order_id in self._py_orders:
            return {**invalid, "status": "REJECTED_DUPLICATE_ID"}

        opposite = self._py_asks if side_code == 0 else self._py_bids
        eligible_prices = sorted(opposite, reverse=(side_code == 1))
        if type_code in (0, 3):
            available = 0
            for level_price in eligible_prices:
                if side_code == 0 and level_price > price_cents:
                    break
                if side_code == 1 and level_price < price_cents:
                    break
                available += sum(order["qty"] for order in opposite[level_price])
                if available >= quantity:
                    break
            if type_code == 3 and available < quantity:
                return {**invalid, "status": "REJECTED_FOK_UNFILLED"}
            if type_code == 0:
                own_volume = sum(order["qty"] for order in self._py_orders.values()
                                 if order["side"] == side_code)
                if own_volume + max(0, quantity - available) > 4294967295:
                    return {**invalid, "status": "REJECTED_CAPACITY"}

        trades = []
        now_ns = time.time_ns()
        remaining = quantity
        for level_price in eligible_prices:
            if side_code == 0 and type_code != 1 and level_price > price_cents:
                break
            if side_code == 1 and type_code != 1 and level_price < price_cents:
                break

            queue = opposite[level_price]
            remaining_orders = []
            for maker in queue:
                if remaining == 0:
                    remaining_orders.append(maker)
                    continue
                fill_qty = min(remaining, maker["qty"])
                trades.append({
                    "maker_order_id": maker["id"],
                    "taker_order_id": order_id,
                    "taker_side": "BUY" if side_code == 0 else "SELL",
                    "price": round(level_price / 100.0, 2),
                    "quantity": fill_qty,
                    "timestamp_ns": now_ns,
                })
                remaining -= fill_qty
                maker["qty"] -= fill_qty
                if maker["qty"]:
                    remaining_orders.append(maker)
                else:
                    self._py_orders.pop(maker["id"], None)
            if remaining_orders:
                opposite[level_price] = remaining_orders
            else:
                opposite.pop(level_price, None)
            if remaining == 0:
                break

        resting = False
        if remaining and type_code == 0:
            order = {"id": order_id, "side": side_code, "price": price_cents, "qty": remaining}
            self._py_orders[order_id] = order
            own_side = self._py_bids if side_code == 0 else self._py_asks
            own_side.setdefault(price_cents, []).append(order)
            resting = True

        return {
            "id": order_id,
            "status": "ACCEPTED_WITH_FILLS" if trades else "ACCEPTED_NO_FILL",
            "accepted": True,
            "resting": resting,
            "unfilled_quantity": remaining,
            "trades": trades,
        }

    def _py_cancel_order(self, order_id):
        if order_id not in self._py_orders:
            return False
        ord_info = self._py_orders.pop(order_id)
        book = self._py_bids if ord_info["side"] == 0 else self._py_asks
        p = ord_info["price"]
        if p in book:
            book[p] = [o for o in book[p] if o["id"] != order_id]
            if not book[p]:
                del book[p]
        return True

    def _py_modify_order(self, order_id, new_qty):
        order = self._py_orders.get(order_id)
        if order is None or new_qty <= 0 or new_qty > 4294967295:
            return False
        own_volume = sum(resting["qty"] for resting in self._py_orders.values()
                         if resting["side"] == order["side"])
        if own_volume - order["qty"] + new_qty > 4294967295:
            return False
        if new_qty <= order["qty"]:
            order["qty"] = new_qty
            return True

        side_code = order["side"]
        price_cents = order["price"]
        self._py_cancel_order(order_id)
        result = self._py_submit_order(order_id, side_code, 0, price_cents, new_qty)
        return result["accepted"]

    def _py_get_bbo(self):
        best_bid = max(self._py_bids.keys()) / 100.0 if self._py_bids else None
        best_ask = min(self._py_asks.keys()) / 100.0 if self._py_asks else None
        return best_bid, best_ask

    def _py_get_l2_depth(self, depth):
        bids = []
        for p in sorted(self._py_bids.keys(), reverse=True)[:depth]:
            bids.append({
                "price": round(p / 100.0, 2),
                "quantity": sum(o["qty"] for o in self._py_bids[p]),
                "order_count": len(self._py_bids[p]),
            })
        asks = []
        for p in sorted(self._py_asks.keys())[:depth]:
            asks.append({
                "price": round(p / 100.0, 2),
                "quantity": sum(o["qty"] for o in self._py_asks[p]),
                "order_count": len(self._py_asks[p]),
            })
        best_bid = bids[0]["price"] if bids else None
        best_ask = asks[0]["price"] if asks else None
        spread = round(best_ask - best_bid, 2) if (best_bid and best_ask) else None
        mid = round((best_bid + best_ask) / 2.0, 2) if (best_bid and best_ask) else None
        return {
            "timestamp_ns": time.time_ns(),
            "symbol": "APEX/USD",
            "bids": bids,
            "asks": asks,
            "best_bid": best_bid,
            "best_ask": best_ask,
            "spread": spread,
            "mid_price": mid,
        }
