import unittest
import sys
import os
import httpx
from unittest.mock import patch
from concurrent.futures import ThreadPoolExecutor

# Add server directory to path
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from app.main import app
from app.engine_bridge import EngineBridge

class TestGateway(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        transport = httpx.ASGITransport(app=app)
        self.client = httpx.AsyncClient(transport=transport, base_url="http://testserver")

    async def asyncTearDown(self):
        await self.client.aclose()

    async def test_health(self):
        res = await self.client.get("/api/v1/health")
        self.assertEqual(res.status_code, 200)
        data = res.json()
        self.assertEqual(data["status"], "healthy")
        self.assertIn("engine_backend", data)
        print("\n[TEST] Health check endpoint: PASSED")

    async def test_place_and_cancel_order(self):
        order_payload = {
            "side": "BUY",
            "order_type": "LIMIT",
            "price": 95.50,
            "quantity": 25
        }
        res = await self.client.post("/api/v1/orders", json=order_payload)
        self.assertEqual(res.status_code, 200)
        data = res.json()
        order_id = data["id"]
        self.assertGreater(order_id, 0)
        self.assertIn(data["status"], ["PLACED", "FILLED"])
        self.assertIn(data["engine_status"], ["ACCEPTED_NO_FILL", "ACCEPTED_WITH_FILLS"])
        self.assertTrue(data["resting"])

        cancel_res = await self.client.delete(f"/api/v1/orders/{order_id}")
        self.assertEqual(cancel_res.status_code, 200)
        self.assertEqual(cancel_res.json()["status"], "CANCELLED")
        print("[TEST] Place & cancel order: PASSED")

    async def test_get_l2_depth(self):
        res = await self.client.get("/api/v1/book/depth?depth=10")
        self.assertEqual(res.status_code, 200)
        data = res.json()
        self.assertIn("bids", data)
        self.assertIn("asks", data)
        self.assertEqual(data["symbol"], "APEX/USD")
        print("[TEST] L2 depth query: PASSED")

    async def test_models_selection(self):
        res = await self.client.get("/api/v1/models")
        self.assertEqual(res.status_code, 200)
        self.assertIn("available_models", res.json())

        # Select Avellaneda-Stoikov
        res_sel = await self.client.post("/api/v1/models/select?mode=AVELLANEDA_STOIKOV")
        self.assertEqual(res_sel.status_code, 200)
        self.assertEqual(res_sel.json()["active_mode"], "AVELLANEDA_STOIKOV")
        print("[TEST] Model selection API: PASSED")

    async def test_agent_controls(self):
        res = await self.client.get("/api/v1/agent/status")
        self.assertEqual(res.status_code, 200)
        self.assertIn("is_running", res.json())

        # Keep this control-plane test deterministic and independent of live feeds.
        with patch("app.main.feeder_agent.start") as start_agent, \
             patch("app.main.feeder_agent.stop") as stop_agent:
            res_start = await self.client.post("/api/v1/agent/start?mode=WEB_FEED&symbol=BTCUSDT")
            self.assertEqual(res_start.status_code, 200)
            self.assertEqual(res_start.json()["status"], "started")
            start_agent.assert_called_once()

            res_stop = await self.client.post("/api/v1/agent/stop")
            self.assertEqual(res_stop.status_code, 200)
            self.assertEqual(res_stop.json()["status"], "stopped")
            stop_agent.assert_called_once()
        print("[TEST] Background Autonomous Agent controls: PASSED")

    async def test_stress_test_endpoint(self):
        res = await self.client.post("/api/v1/stress-test", json={"order_count": 1000})
        self.assertEqual(res.status_code, 200)
        data = res.json()
        self.assertEqual(data["total_orders"], 1000)
        self.assertGreater(data["throughput_ops_sec"], 0)
        print(f"[TEST] Stress test API (1,000 orders): PASSED -> {data['throughput_ops_sec']:.0f} ops/sec, p50={data['p50_us']}µs")

    def test_python_fallback_order_semantics(self):
        bridge = EngineBridge(force_python=True)
        result = bridge.submit_order(1, "SELL", "LIMIT", 100.00, 5)
        self.assertEqual(result["status"], "ACCEPTED_NO_FILL")
        self.assertTrue(result["resting"])
        duplicate = bridge.submit_order(1, "BUY", "LIMIT", 100.00, 1)
        self.assertEqual(duplicate["status"], "REJECTED_DUPLICATE_ID")
        fok = bridge.submit_order(2, "BUY", "FOK", 100.00, 6)
        self.assertEqual(fok["status"], "REJECTED_FOK_UNFILLED")
        self.assertEqual(bridge.order_count(), 1)
        fill = bridge.submit_order(3, "BUY", "IOC", 100.00, 3)
        self.assertEqual(fill["status"], "ACCEPTED_WITH_FILLS")
        self.assertEqual(fill["trades"][0]["quantity"], 3)
        self.assertFalse(fill["resting"])
        self.assertEqual(bridge.get_l2_depth()["asks"][0]["quantity"], 2)
        print("[TEST] Python fallback typed outcomes and FOK/IOC semantics: PASSED")

    def test_native_bridge_reports_more_than_64_fills(self):
        bridge = EngineBridge()
        self.assertTrue(bridge.is_cpp_backend, "Build the native C++ library before running this suite")
        for order_id in range(1, 71):
            result = bridge.submit_order(order_id, "SELL", "LIMIT", 100.00, 1)
            self.assertEqual(result["status"], "ACCEPTED_NO_FILL")
        result = bridge.submit_order(1000, "BUY", "MARKET", 0, 70)
        self.assertEqual(result["status"], "ACCEPTED_WITH_FILLS")
        self.assertEqual(len(result["trades"]), 70)
        self.assertEqual(sum(trade["quantity"] for trade in result["trades"]), 70)
        print("[TEST] Native bridge returns all 70 fills: PASSED")

    def test_python_and_cpp_bridge_match_results(self):
        native = EngineBridge()
        self.assertTrue(native.is_cpp_backend, "Build the native C++ library before running this suite")
        fallback = EngineBridge(force_python=True)

        def run_trace(bridge):
            first = bridge.submit_order(1, "SELL", "LIMIT", 100.00, 3)
            second = bridge.submit_order(2, "SELL", "LIMIT", 100.00, 4)
            duplicate = bridge.submit_order(1, "BUY", "LIMIT", 100.00, 1)
            rejected_fok = bridge.submit_order(3, "BUY", "FOK", 100.00, 8)
            self.assertTrue(bridge.modify_order(1, 5))
            taker = bridge.submit_order(4, "BUY", "IOC", 100.00, 12)
            return [
                (result["status"], result["resting"], result["unfilled_quantity"],
                 [(t["maker_order_id"], t["price"], t["quantity"]) for t in result["trades"]])
                for result in (first, second, duplicate, rejected_fok, taker)
            ]

        self.assertEqual(run_trace(native), run_trace(fallback))
        print("[TEST] Python fallback and C++ bridge semantics match: PASSED")

    def test_invalid_bridge_quantities_do_not_wrap(self):
        native = EngineBridge()
        self.assertTrue(native.is_cpp_backend)
        for bridge in (native, EngineBridge(force_python=True)):
            bridge.submit_order(1, "BUY", "LIMIT", 100.00, 3)
            for new_qty in (-1, 0, 4294967296, 1.5):
                self.assertFalse(bridge.modify_order(1, new_qty))
            for order_id in (-1, 0, 18446744073709551616):
                self.assertFalse(bridge.cancel_order(order_id))
            self.assertEqual(bridge.get_l2_depth()["bids"][0]["quantity"], 3)
            self.assertEqual(bridge.submit_order(2, "SELL", "IOC", 100.00, 1.5)["status"],
                             "REJECTED_INVALID_INPUT")

    def test_bridge_serializes_concurrent_callers(self):
        native = EngineBridge()
        self.assertTrue(native.is_cpp_backend)
        for bridge in (native, EngineBridge(force_python=True)):
            with ThreadPoolExecutor(max_workers=4) as workers:
                results = list(workers.map(
                    lambda order_id: bridge.submit_order(order_id, "BUY", "LIMIT", 100.00, 1),
                    range(1, 201)))
                self.assertTrue(all(result["accepted"] for result in results))
                self.assertEqual(bridge.order_count(), 200)
                self.assertEqual(bridge.get_l2_depth()["bids"][0]["quantity"], 200)
                self.assertTrue(all(workers.map(bridge.cancel_order, range(1, 201))))
            self.assertEqual(bridge.order_count(), 0)

    def test_bridge_volume_capacity_matches_native_and_fallback(self):
        native = EngineBridge()
        self.assertTrue(native.is_cpp_backend)
        for bridge in (native, EngineBridge(force_python=True)):
            self.assertTrue(bridge.submit_order(1, "BUY", "LIMIT", 99, 4294967295)["accepted"])
            self.assertEqual(bridge.submit_order(2, "BUY", "LIMIT", 98, 1)["status"], "REJECTED_CAPACITY")
            self.assertFalse(bridge.modify_order(1, 4294967296))
            bridge.submit_order(3, "SELL", "LIMIT", 100, 1)
            self.assertEqual(bridge.submit_order(4, "BUY", "LIMIT", 100, 2)["status"], "REJECTED_CAPACITY")
            self.assertEqual(bridge.get_l2_depth()["asks"][0]["quantity"], 1)
            self.assertEqual(bridge.get_l2_depth()["bids"][0]["quantity"], 4294967295)

if __name__ == "__main__":
    unittest.main()
