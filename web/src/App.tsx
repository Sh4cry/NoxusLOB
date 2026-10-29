import React, { useState, useEffect, useRef, useCallback } from 'react';
import { Header } from './components/Header';
import { OrderBookLadder } from './components/OrderBookLadder';
import { DepthChart } from './components/DepthChart';
import { TradeTape } from './components/TradeTape';
import { LatencyStats } from './components/LatencyStats';
import { OrderControls } from './components/OrderControls';
import { L2Snapshot, Trade, SimState, MarketDataMessage, StressTestMetrics } from './types';
import { Terminal, BookOpen, ExternalLink, Code2 } from 'lucide-react';

export const App: React.FC = () => {
  const [snapshot, setSnapshot] = useState<L2Snapshot | null>(null);
  const [trades, setTrades] = useState<Trade[]>([]);
  const [orderCount, setOrderCount] = useState<number>(0);
  const [simState, setSimState] = useState<SimState | null>(null);
  const [backend, setBackend] = useState<string>('C++20 (Low-Latency)');
  const [isConnected, setIsConnected] = useState<boolean>(false);
  const [selectedPrice, setSelectedPrice] = useState<number | null>(null);
  const [stressMetrics, setStressMetrics] = useState<StressTestMetrics | null>(null);
  const [activeTab, setActiveTab] = useState<'terminal' | 'architecture'>('terminal');

  const wsRef = useRef<WebSocket | null>(null);
  const reconnectTimer = useRef<number | null>(null);

  const connectWebSocket = useCallback(() => {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const host = window.location.host;
    // Default to port 8000 if running on Vite dev server (port 3000)
    const wsUrl = host.includes(':3000')
      ? `${protocol}//127.0.0.1:8000/ws/market-data`
      : `${protocol}//${host}/ws/market-data`;

    try {
      const ws = new WebSocket(wsUrl);
      wsRef.current = ws;

      ws.onopen = () => {
        setIsConnected(true);
        console.log('[NoxusLOB] WebSocket Connected');
      };

      ws.onmessage = (event) => {
        try {
          const msg: MarketDataMessage = JSON.parse(event.data);
          if (msg.type === 'SNAPSHOT') {
            setSnapshot(msg.data);
            setTrades(msg.recent_trades || []);
            setOrderCount(msg.order_count || 0);
            setSimState(msg.sim_state);
            setBackend(msg.backend);
          }
        } catch (e) {
          console.error('[WS Parse Error]', e);
        }
      };

      ws.onclose = () => {
        setIsConnected(false);
        console.log('[NoxusLOB] WebSocket Closed. Reconnecting in 2s...');
        reconnectTimer.current = window.setTimeout(connectWebSocket, 2000);
      };

      ws.onerror = (err) => {
        console.error('[WS Error]', err);
        ws.close();
      };
    } catch (e) {
      console.error('[WS Init Error]', e);
      reconnectTimer.current = window.setTimeout(connectWebSocket, 2000);
    }
  }, []);

  useEffect(() => {
    connectWebSocket();
    return () => {
      if (wsRef.current) wsRef.current.close();
      if (reconnectTimer.current) clearTimeout(reconnectTimer.current);
    };
  }, [connectWebSocket]);

  const handleToggleSim = async (run: boolean) => {
    try {
      await fetch(`/api/v1/simulation/${run ? 'start' : 'stop'}`, { method: 'POST' });
    } catch (err) {
      console.error(err);
    }
  };

  const handleSelectModel = async (mode: 'AVELLANEDA_STOIKOV' | 'MARKOV' | 'DATA_REPLAY') => {
    try {
      await fetch(`/api/v1/models/select?mode=${mode}`, { method: 'POST' });
    } catch (err) {
      console.error(err);
    }
  };

  return (
    <div className="min-h-screen bg-background text-slate-100 flex flex-col font-sans">
      <Header
        snapshot={snapshot}
        simState={simState}
        backend={backend}
        orderCount={orderCount}
        isConnected={isConnected}
        onSelectModel={handleSelectModel}
      />

      {/* Navigation Sub-bar */}
      <div className="px-6 py-2 border-b border-surfaceBorder/60 bg-surface/40 flex items-center justify-between text-xs">
        <div className="flex items-center gap-2">
          <button
            onClick={() => setActiveTab('terminal')}
            className={`flex items-center gap-1.5 px-3 py-1 rounded-md font-semibold transition-colors ${
              activeTab === 'terminal'
                ? 'bg-brandCyan/10 text-brandCyan border border-brandCyan/30'
                : 'text-slate-400 hover:text-white'
            }`}
          >
            <Terminal className="w-3.5 h-3.5" />
            <span>Trading Terminal</span>
          </button>
          <button
            onClick={() => setActiveTab('architecture')}
            className={`flex items-center gap-1.5 px-3 py-1 rounded-md font-semibold transition-colors ${
              activeTab === 'architecture'
                ? 'bg-brandCyan/10 text-brandCyan border border-brandCyan/30'
                : 'text-slate-400 hover:text-white'
            }`}
          >
            <BookOpen className="w-3.5 h-3.5" />
            <span>Architecture & CV Showcase</span>
          </button>
        </div>

        <div className="hidden sm:flex items-center gap-3 text-slate-400 font-mono text-[11px]">
          <span>FIFO Price-Time Priority</span>
          <span>•</span>
          <span>Pre-Allocated Order Slab Pools</span>
          <span>•</span>
          <span className="text-emerald-400">~3.5M Ops/Sec</span>
        </div>
      </div>

      {activeTab === 'terminal' ? (
        <main className="flex-1 p-4 lg:p-6 flex flex-col gap-4 max-w-[1700px] w-full mx-auto">
          {/* Latency & Telemetry Row */}
          <LatencyStats metrics={stressMetrics} />

          {/* Core 3-Column Trading Grid */}
          <div className="grid grid-cols-1 md:grid-cols-12 gap-4 flex-1 items-stretch">
            {/* Left Column: Order Controls & Trade Feed (4 cols) */}
            <div className="md:col-span-4 flex flex-col gap-4">
              <OrderControls
                selectedPrice={selectedPrice}
                simRunning={simState?.running ?? true}
                onToggleSim={handleToggleSim}
                onUpdateMetrics={setStressMetrics}
              />
              <div className="h-[320px] lg:h-[380px]">
                <TradeTape trades={trades} />
              </div>
            </div>

            {/* Middle Column: L2 Depth Ladder (4 cols) */}
            <div className="md:col-span-4 h-[650px] lg:h-auto">
              <OrderBookLadder
                snapshot={snapshot}
                onSelectPrice={setSelectedPrice}
              />
            </div>

            {/* Right Column: Canvas Depth Chart & Insights (4 cols) */}
            <div className="md:col-span-4 flex flex-col gap-4">
              <div className="h-[340px] lg:h-[380px]">
                <DepthChart snapshot={snapshot} />
              </div>

              {/* HFT Microstructure Info Card */}
              <div className="bg-surface rounded-xl border border-surfaceBorder p-4 flex flex-col gap-2.5 shadow-lg">
                <div className="flex items-center justify-between text-xs font-semibold text-slate-300">
                  <div className="flex items-center gap-1.5">
                    <Code2 className="w-4 h-4 text-brandCyan" />
                    <span>Microstructure Simulation</span>
                  </div>
                  <span className="text-[10px] font-mono text-slate-500">Markov Chain</span>
                </div>
                <p className="text-xs text-slate-400 leading-relaxed">
                  Real-time order flow is generated using a 3-state Markov chain tracking bid/ask queue transitions, cancellation waves, and institutional liquidity replenishment with sub-millisecond tick precision.
                </p>
                <div className="grid grid-cols-3 gap-2 mt-1 pt-2 border-t border-surfaceBorder/60 text-center font-mono">
                  <div className="bg-background/60 p-2 rounded border border-surfaceBorder">
                    <div className="text-[10px] text-slate-500">State 0</div>
                    <div className="text-xs font-bold text-amber-400">Low Depth</div>
                  </div>
                  <div className="bg-background/60 p-2 rounded border border-surfaceBorder">
                    <div className="text-[10px] text-slate-500">State 1</div>
                    <div className="text-xs font-bold text-emerald-400">Med Depth</div>
                  </div>
                  <div className="bg-background/60 p-2 rounded border border-surfaceBorder">
                    <div className="text-[10px] text-slate-500">State 2</div>
                    <div className="text-xs font-bold text-blue-400">High Depth</div>
                  </div>
                </div>
              </div>
            </div>
          </div>
        </main>
      ) : (
        /* Architecture & CV Documentation View */
        <main className="flex-1 p-6 max-w-5xl w-full mx-auto space-y-6">
          <div className="bg-surface rounded-xl border border-surfaceBorder p-6 shadow-lg space-y-4">
            <h2 className="text-xl font-bold text-white flex items-center gap-2">
              <Code2 className="w-6 h-6 text-brandCyan" />
              NoxusLOB Engineering Design & Architecture
            </h2>
            <p className="text-slate-300 text-sm leading-relaxed">
              NoxusLOB is designed from the ground up for high-frequency trading and low-latency financial systems engineering. It showcases key computer science and systems fundamentals: cache locality, memory pools, intrusive data structures, lock-free patterns, and FFI inter-process communication.
            </p>

            <div className="grid grid-cols-1 md:grid-cols-2 gap-4 mt-4">
              <div className="bg-background p-4 rounded-lg border border-surfaceBorder space-y-2">
                <h3 className="font-bold text-sm text-brandCyan">1. O(1) Intrusive Queue Detachment</h3>
                <p className="text-xs text-slate-400 leading-relaxed">
                  Orders store intrusive doubly linked list pointers (<code className="text-slate-200">prev</code>, <code className="text-slate-200">next</code>). Detaching an order during cancellations or fills requires zero pointer traversals and zero node allocations, keeping operations completely <code className="text-brandCyan">O(1)</code>.
                </p>
              </div>

              <div className="bg-background p-4 rounded-lg border border-surfaceBorder space-y-2">
                <h3 className="font-bold text-sm text-emerald-400">2. Pre-Allocated Order Slab Pool</h3>
                <p className="text-xs text-slate-400 leading-relaxed">
                  Pre-allocates contiguous slabs of <code className="text-slate-200">Order</code> objects to eliminate dynamic node allocation churn (<code className="text-slate-200">malloc()</code>/<code className="text-slate-200">free()</code>) on the trading critical path, minimizing memory fragmentation and allocator contention.
                </p>
              </div>

              <div className="bg-background p-4 rounded-lg border border-surfaceBorder space-y-2">
                <h3 className="font-bold text-sm text-indigo-400">3. C-ABI & Direct FFI Bridge</h3>
                <p className="text-xs text-slate-400 leading-relaxed">
                  Exported as a C-ABI shared library with explicit order outcomes and complete fill lists, enabling Python simulation scripts and WebSockets to interact with the C++ engine.
                </p>
              </div>

              <div className="bg-background p-4 rounded-lg border border-surfaceBorder space-y-2">
                <h3 className="font-bold text-sm text-amber-400">4. Markov-Modulated Microstructure</h3>
                <p className="text-xs text-slate-400 leading-relaxed">
                  Simulates realistic order arrival distributions, cancellation cascades, and liquidity replenishment using a 3-state discrete Markov chain transition matrix.
                </p>
              </div>
            </div>

            {/* Resume Bullet Points Showcase */}
            <div className="bg-background/90 p-5 rounded-lg border border-surfaceBorder mt-6 space-y-3">
              <h3 className="text-sm font-bold text-white uppercase tracking-wider flex items-center gap-2">
                <span>Copy-Paste Ready CV / Resume Bullet Points</span>
                <span className="text-[10px] bg-brandCyan/10 text-brandCyan px-2 py-0.5 rounded font-mono">FOR RECRUITERS</span>
              </h3>
              <ul className="text-xs text-slate-300 space-y-2 list-disc list-inside">
                <li>
                  <strong className="text-white">Matching Engine (C++20):</strong> Implemented price-time priority, maker-price execution, and limit, market, IOC, and FOK orders. A seeded reference test compares fills and book state after every operation; the CLI reproduces the core benchmark.
                </li>
                <li>
                  <strong className="text-white">Order-Node Memory Pool:</strong> Reused preallocated order nodes with ownership and double-free checks. Intrusive price queues provide constant-time unlinking after the order and price level are located.
                </li>
                <li>
                  <strong className="text-white">Full-Stack & Real-Time Telemetry:</strong> Built an event-driven market data gateway in FastAPI/WebSockets streaming L2 book updates at 20Hz to a high-framerate React/Vite trading terminal featuring interactive depth charts and microsecond latency profiling.
                </li>
                <li>
                  <strong className="text-white">Quantitative Microstructure Modeling:</strong> Designed synthetic market makers implementing the Avellaneda-Stoikov inventory model and a 3-state discrete-time Markov chain to simulate realistic order arrival and spread dynamics.
                </li>
              </ul>
            </div>
          </div>
        </main>
      )}
    </div>
  );
};
