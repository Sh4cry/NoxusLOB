import asyncio
import time
import json
import os
from typing import Set, List, Dict, Any, Optional, Literal
from contextlib import asynccontextmanager

from fastapi import FastAPI, WebSocket, WebSocketDisconnect, HTTPException, Query
from fastapi.middleware.cors import CORSMiddleware
from fastapi.staticfiles import StaticFiles

from app.engine_bridge import EngineBridge
from app.market_maker import MarkovMarketMaker
from app.models.avellaneda_stoikov import AvellanedaStoikovModel
from app.models.data_replayer import HistoricalDataReplayer
from app.agent.autonomous_feeder import AutonomousFeederAgent
from app.schemas import (
    OrderRequest,
    OrderResponse,
    L2SnapshotModel,
    StressTestRequest,
    StressTestResult,
)

# Global Engine Instance
engine = EngineBridge()

# Trade Buffer
recent_trades: List[Dict[str, Any]] = []

def handle_trade(trade: Dict[str, Any]):
    recent_trades.insert(0, trade)
    if len(recent_trades) > 100:
        recent_trades.pop()

# Instantiate Models
markov_bot = MarkovMarketMaker(engine, base_price=100.0, tick_size=0.05)
markov_bot.register_trade_callback(handle_trade)

as_bot = AvellanedaStoikovModel(engine, gamma=0.1, kappa=1.5, tick_size=0.05)
as_bot.register_trade_callback(handle_trade)

data_replayer = HistoricalDataReplayer(engine)
data_replayer.register_trade_callback(handle_trade)

feeder_agent = AutonomousFeederAgent(engine)

# Load sample data if available
sample_csv = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "data", "sample_btc_ticks.csv"))
if os.path.exists(sample_csv):
    try:
        data_replayer.load_csv(sample_csv)
    except Exception as e:
        print(f"[Warning] Could not load sample CSV: {e}")

# Active Simulation Mode: "MARKOV", "AVELLANEDA_STOIKOV", "DATA_REPLAY"
active_model_mode = "AVELLANEDA_STOIKOV"
sim_is_running = True

# WebSocket connection manager
class ConnectionManager:
    def __init__(self):
        self.active_connections: Set[WebSocket] = set()

    async def connect(self, websocket: WebSocket):
        await websocket.accept()
        self.active_connections.add(websocket)

    def disconnect(self, websocket: WebSocket):
        self.active_connections.discard(websocket)

    async def broadcast_json(self, data: dict):
        if not self.active_connections:
            return
        dead_connections = set()
        for conn in self.active_connections:
            try:
                await conn.send_json(data)
            except Exception:
                dead_connections.add(conn)
        for dead in dead_connections:
            self.active_connections.discard(dead)

manager = ConnectionManager()

# Background Simulation Step Loop
async def simulation_loop():
    global sim_is_running, active_model_mode
    while True:
        try:
            if sim_is_running:
                if active_model_mode == "MARKOV":
                    await markov_bot.step()
                elif active_model_mode == "AVELLANEDA_STOIKOV":
                    await as_bot.step()
                elif active_model_mode == "DATA_REPLAY":
                    await data_replayer.step()
        except Exception as e:
            print(f"[Sim Error] {e}")
        await asyncio.sleep(0.06) # ~16 Hz execution rate

# Background WebSocket Broadcast Loop (20 updates/sec)
async def market_data_broadcaster():
    while True:
        try:
            if manager.active_connections:
                l2 = engine.get_l2_depth(depth=15)
                model_meta = {}
                if active_model_mode == "AVELLANEDA_STOIKOV":
                    model_meta = as_bot.get_metrics()
                elif active_model_mode == "DATA_REPLAY":
                    model_meta = data_replayer.get_progress()
                else:
                    model_meta = {
                        "model_name": "Markov Chain (Microstructure)",
                        "markov_state": markov_bot.current_state,
                        "state_name": ["Low Depth", "Medium Depth", "High Depth"][markov_bot.current_state]
                    }

                payload = {
                    "type": "SNAPSHOT",
                    "data": l2,
                    "recent_trades": recent_trades[:25],
                    "order_count": engine.order_count(),
                    "sim_state": {
                        "running": sim_is_running,
                        "active_mode": active_model_mode,
                        "metadata": model_meta,
                    },
                    "backend": "C++20 (Low-Latency)" if engine.is_cpp_backend else "Python",
                }
                await manager.broadcast_json(payload)
        except Exception as e:
            print(f"[Broadcaster Error] {e}")
        await asyncio.sleep(0.05) # 50ms = 20 Hz update rate

@asynccontextmanager
async def lifespan(app: FastAPI):
    # Initialize initial state
    await as_bot.initialize_book()
    sim_task = asyncio.create_task(simulation_loop())
    broadcaster_task = asyncio.create_task(market_data_broadcaster())
    yield
    sim_task.cancel()
    broadcaster_task.cancel()

app = FastAPI(
    title="NoxusLOB Real-Time Matching Engine Gateway",
    description="Low-latency institutional limit order book matching engine with adaptive quantitative models",
    version="1.1.0",
    lifespan=lifespan,
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

@app.get("/api/v1/health")
def get_health():
    return {
        "status": "healthy",
        "engine_backend": "C++20 Native" if engine.is_cpp_backend else "Python Fallback",
        "active_model": active_model_mode,
        "orders_in_book": engine.order_count(),
        "timestamp_ns": time.time_ns(),
    }

@app.get("/api/v1/book/depth", response_model=L2SnapshotModel)
def get_depth(depth: int = Query(15, ge=1, le=50)):
    return engine.get_l2_depth(depth=depth)

@app.get("/api/v1/models")
def get_models():
    return {
        "active_mode": active_model_mode,
        "available_models": [
            {
                "id": "AVELLANEDA_STOIKOV",
                "name": "Avellaneda-Stoikov (Adaptive)",
                "description": "Optimal market making adapting quotes to rolling volatility and inventory risk."
            },
            {
                "id": "MARKOV",
                "name": "Markov Chain (Microstructure)",
                "description": "3-state discrete Markov chain modeling regime transitions and queue depth."
            },
            {
                "id": "DATA_REPLAY",
                "name": "Historical Tick Replayer",
                "description": "Replays arbitrary CSV market tick data through the C++ matching engine."
            }
        ]
    }

@app.post("/api/v1/models/select")
async def select_model(mode: Literal["AVELLANEDA_STOIKOV", "MARKOV", "DATA_REPLAY"] = Query(...)):
    global active_model_mode
    active_model_mode = mode
    if mode == "AVELLANEDA_STOIKOV":
        await as_bot.initialize_book()
    elif mode == "MARKOV":
        await markov_bot.initialize_book()
    elif mode == "DATA_REPLAY":
        engine.clear()
    return {"status": "success", "active_mode": active_model_mode}

@app.post("/api/v1/data/load")
def load_custom_data(file_path: str):
    if not os.path.exists(file_path):
        raise HTTPException(status_code=404, detail="File not found")
    count = data_replayer.load_csv(file_path)
    return {"status": "loaded", "ticks_count": count, "file": file_path}

# --- Background Autonomous Screen & Web Agent Endpoints ---
@app.get("/api/v1/agent/status")
def get_agent_status():
    return feeder_agent.get_status()

@app.post("/api/v1/agent/start")
async def start_agent(mode: Literal["SCREEN_UIA", "WEB_FEED"] = Query("WEB_FEED"), symbol: str = "BTCUSDT", interval_sec: float = 1.0):
    feeder_agent.start(mode=mode, symbol=symbol, interval_sec=interval_sec)
    return {"status": "started", "config": feeder_agent.get_status()}

@app.post("/api/v1/agent/stop")
async def stop_agent():
    feeder_agent.stop()
    return {"status": "stopped"}

@app.post("/api/v1/agent/scan-screen")
async def scan_screen_now():
    data = await feeder_agent.poll_once()
    return data

@app.post("/api/v1/orders", response_model=OrderResponse)
def place_order(order: OrderRequest):
    result = engine.submit_order(
        order_id=order.id,
        side=order.side.value,
        order_type=order.order_type.value,
        price_dollars=order.price,
        quantity=order.quantity,
    )
    order_id = result["id"]
    trades = result["trades"]
    for tr in trades:
        handle_trade(tr)

    return OrderResponse(
        id=order_id,
        status=("FILLED" if trades else "PLACED") if result["accepted"] else "REJECTED",
        engine_status=result["status"],
        trades_count=len(trades),
        trades=trades,
        resting=result["resting"],
        unfilled_quantity=result["unfilled_quantity"],
    )

@app.delete("/api/v1/orders/{order_id}")
def cancel_order(order_id: int):
    success = engine.cancel_order(order_id)
    if not success:
        raise HTTPException(status_code=404, detail="Order not found or already filled/cancelled")
    return {"order_id": order_id, "status": "CANCELLED"}

@app.post("/api/v1/simulation/start")
def start_simulation():
    global sim_is_running
    sim_is_running = True
    return {"status": "started"}

@app.post("/api/v1/simulation/stop")
def stop_simulation():
    global sim_is_running
    sim_is_running = False
    return {"status": "stopped"}

@app.post("/api/v1/stress-test", response_model=StressTestResult)
def run_stress_test(req: StressTestRequest):
    count = req.order_count
    import numpy as np

    latencies_us = []
    total_trades = 0
    start_time = time.perf_counter()

    for i in range(1, count + 1):
        side = "BUY" if (i % 2 == 0) else "SELL"
        price = 100.0 + (i % 20) * 0.05
        t0 = time.perf_counter_ns()
        _, trades = engine.add_order(
            order_id=10000000 + i,
            side=side,
            order_type="LIMIT",
            price_dollars=price,
            quantity=10,
        )
        t1 = time.perf_counter_ns()
        latencies_us.append((t1 - t0) / 1000.0)
        total_trades += len(trades)

    elapsed = time.perf_counter() - start_time
    latencies_us.sort()

    return StressTestResult(
        total_orders=count,
        total_trades=total_trades,
        elapsed_seconds=round(elapsed, 4),
        throughput_ops_sec=round(count / elapsed, 1),
        p50_us=round(float(np.percentile(latencies_us, 50)), 2),
        p95_us=round(float(np.percentile(latencies_us, 95)), 2),
        p99_us=round(float(np.percentile(latencies_us, 99)), 2),
    )

@app.websocket("/ws/market-data")
async def websocket_endpoint(websocket: WebSocket):
    await manager.connect(websocket)
    try:
        while True:
            text = await websocket.receive_text()
            data = json.loads(text)
            if data.get("action") == "PING":
                await websocket.send_json({"type": "PONG"})
    except WebSocketDisconnect:
        manager.disconnect(websocket)
    except Exception:
        manager.disconnect(websocket)

dist_dir = os.path.join(os.path.dirname(__file__), "..", "..", "web", "dist")
if os.path.exists(dist_dir):
    app.mount("/", StaticFiles(directory=dist_dir, html=True), name="static")
