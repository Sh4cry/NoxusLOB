import React from 'react';
import { Gauge, Clock, ShieldCheck, Zap } from 'lucide-react';
import { StressTestMetrics } from '../types';

interface LatencyStatsProps {
  metrics: StressTestMetrics | null;
}

export const LatencyStats: React.FC<LatencyStatsProps> = ({ metrics }) => {
  const p50 = metrics ? metrics.p50_us.toFixed(2) : '--';
  const p95 = metrics ? metrics.p95_us.toFixed(2) : '--';
  const p99 = metrics ? metrics.p99_us.toFixed(2) : '--';
  const throughput = metrics 
    ? (metrics.throughput_ops_sec / 1e6).toFixed(2) + 'M' 
    : '--';

  return (
    <div className="bg-surface rounded-xl border border-surfaceBorder p-4 flex flex-col gap-3 shadow-lg">
      <div className="flex items-center justify-between">
        <div className="flex items-center gap-2">
          <Gauge className="w-4 h-4 text-brandCyan" />
          <h2 className="text-xs font-semibold uppercase tracking-wider text-slate-300">
            Gateway Latency & Throughput
          </h2>
        </div>
        <div className="flex items-center gap-1.5 text-[11px] text-emerald-400 bg-emerald-500/10 px-2 py-0.5 rounded border border-emerald-500/20 font-mono">
          <ShieldCheck className="w-3.5 h-3.5" />
          <span>{metrics ? 'Measured gateway sample' : 'Run a stress test'}</span>
        </div>
      </div>

      <div className="grid grid-cols-2 sm:grid-cols-4 gap-3">
        {/* Throughput */}
        <div className="bg-background/70 border border-surfaceBorder/80 rounded-lg p-3 flex flex-col">
          <div className="text-[11px] text-slate-400 font-medium flex items-center gap-1">
            <Zap className="w-3 h-3 text-amber-400" />
            Throughput
          </div>
          <div className="text-lg font-bold font-mono text-slate-100 mt-1">
            {throughput} <span className="text-xs font-normal text-slate-400">ops/sec</span>
          </div>
          <div className="text-[10px] text-slate-500 mt-0.5">Includes Python and FFI</div>
        </div>

        {/* p50 Median */}
        <div className="bg-background/70 border border-surfaceBorder/80 rounded-lg p-3 flex flex-col">
          <div className="text-[11px] text-slate-400 font-medium flex items-center gap-1">
            <Clock className="w-3 h-3 text-emerald-400" />
            p50 (Median)
          </div>
          <div className="text-lg font-bold font-mono text-emerald-400 mt-1">
            {p50} <span className="text-xs font-normal text-slate-400">µs</span>
          </div>
          <div className="text-[10px] text-slate-500 mt-0.5">Median gateway operation</div>
        </div>

        {/* p95 */}
        <div className="bg-background/70 border border-surfaceBorder/80 rounded-lg p-3 flex flex-col">
          <div className="text-[11px] text-slate-400 font-medium flex items-center gap-1">
            <Clock className="w-3 h-3 text-brandCyan" />
            p95 Latency
          </div>
          <div className="text-lg font-bold font-mono text-brandCyan mt-1">
            {p95} <span className="text-xs font-normal text-slate-400">µs</span>
          </div>
          <div className="text-[10px] text-slate-500 mt-0.5">High-percentile execution</div>
        </div>

        {/* p99 Tail */}
        <div className="bg-background/70 border border-surfaceBorder/80 rounded-lg p-3 flex flex-col">
          <div className="text-[11px] text-slate-400 font-medium flex items-center gap-1">
            <Clock className="w-3 h-3 text-indigo-400" />
            p99 Tail
          </div>
          <div className="text-lg font-bold font-mono text-indigo-400 mt-1">
            {p99} <span className="text-xs font-normal text-slate-400">µs</span>
          </div>
          <div className="text-[10px] text-slate-500 mt-0.5">Observed 99th percentile</div>
        </div>
      </div>
    </div>
  );
};
