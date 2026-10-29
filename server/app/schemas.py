from pydantic import BaseModel, Field
from typing import List, Optional
from enum import Enum

class OrderSide(str, Enum):
    BUY = "BUY"
    SELL = "SELL"

class OrderType(str, Enum):
    LIMIT = "LIMIT"
    MARKET = "MARKET"
    IOC = "IOC"
    FOK = "FOK"

class OrderRequest(BaseModel):
    id: Optional[int] = None
    side: OrderSide
    order_type: OrderType = OrderType.LIMIT
    price: float = Field(..., description="Price in dollars, e.g. 100.50")
    quantity: int = Field(..., gt=0, description="Quantity in units")

class OrderResponse(BaseModel):
    id: int
    status: str
    trades_count: int
    trades: List[dict]
    engine_status: str = "ACCEPTED_NO_FILL"
    resting: bool = False
    unfilled_quantity: int = 0

class LevelSummaryModel(BaseModel):
    price: float
    quantity: int
    order_count: int

class L2SnapshotModel(BaseModel):
    timestamp_ns: int
    symbol: str = "APEX/USD"
    bids: List[LevelSummaryModel]
    asks: List[LevelSummaryModel]
    best_bid: Optional[float] = None
    best_ask: Optional[float] = None
    spread: Optional[float] = None
    mid_price: Optional[float] = None

class TradeModel(BaseModel):
    maker_order_id: int
    taker_order_id: int
    taker_side: str
    price: float
    quantity: int
    timestamp_ns: int

class StressTestRequest(BaseModel):
    order_count: int = Field(default=10000, le=500000)

class StressTestResult(BaseModel):
    total_orders: int
    total_trades: int
    elapsed_seconds: float
    throughput_ops_sec: float
    p50_us: float
    p95_us: float
    p99_us: float
