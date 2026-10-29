#!/usr/bin/env python3
"""
NoxusLOB Master Launcher & Benchmark Utility
-------------------------------------------
Usage:
  python run.py             # Start the full trading platform (Engine + Gateway + UI)
  python run.py --bench     # Run the high-resolution C++20 latency benchmark
  python run.py --test      # Run all C++ and Python unit test suites
"""

import sys
import os
import subprocess
import argparse
import webbrowser
import time

PROJECT_ROOT = os.path.dirname(os.path.abspath(__file__))
ENGINE_DIR = os.path.join(PROJECT_ROOT, "engine")
SERVER_DIR = os.path.join(PROJECT_ROOT, "server")
WEB_DIR = os.path.join(PROJECT_ROOT, "web")

def find_gxx():
    candidates = [
        "g++",
        r"C:\msys64\ucrt64\bin\g++.exe",
        r"C:\msys64\mingw64\bin\g++.exe",
    ]
    for c in candidates:
        try:
            res = subprocess.run([c, "--version"], capture_output=True, text=True)
            if res.returncode == 0:
                return c
        except Exception:
            continue
    return None

def build_engine(force=False):
    build_dir = os.path.join(ENGINE_DIR, "build")
    os.makedirs(build_dir, exist_ok=True)
    prefix = "" if sys.platform == "win32" else "lib"
    extension = ".dll" if sys.platform == "win32" else (".dylib" if sys.platform == "darwin" else ".so")
    suffix = f"_test_{os.getpid()}" if force else ""
    dll_name = f"{prefix}noxus_engine{suffix}{extension}"
    dll_path = os.path.join(build_dir, dll_name)
    if os.path.exists(dll_path) and not force:
        os.environ["NOXUS_ENGINE_LIBRARY"] = dll_path
        return True

    gxx = find_gxx()
    if not gxx:
        print("[!] Warning: g++ compiler not found in PATH or standard MSYS2 locations.")
        print("[*] NoxusLOB will run with the Python fallback.")
        return False

    print(f"[*] Compiling C++20 engine with {gxx}...")
    cmd = [
        gxx,
        "-std=c++20",
        "-O3",
        "-dynamiclib" if sys.platform == "darwin" else "-shared",
        *(["-static", "-static-libgcc", "-static-libstdc++"] if sys.platform == "win32" else ["-fPIC"]),
        "-I", os.path.join(ENGINE_DIR, "include"),
        os.path.join(ENGINE_DIR, "src", "OrderBook.cpp"),
        os.path.join(ENGINE_DIR, "src", "C_API.cpp"),
        "-o", dll_path,
    ]
    res = subprocess.run(cmd)
    if res.returncode == 0:
        print(f"[+] Engine DLL compiled successfully: {dll_path}")
        os.environ["NOXUS_ENGINE_LIBRARY"] = dll_path
        return True
    else:
        print("[!] Engine compilation failed. Falling back to Python backend.")
        return False

def run_tests():
    print("=" * 60)
    print("  RUNNING NOXUSLOB COMPLETE TEST SUITE")
    print("=" * 60)

    # 1. C++ Matching Engine Tests
    gxx = find_gxx()
    if gxx:
        test_exe = os.path.join(ENGINE_DIR, "tests", "test_matching.exe")
        cmd = [
            gxx, "-std=c++20", "-O2", "-DNDEBUG", "-Wall", "-Wextra", "-Wpedantic",
            "-I", os.path.join(ENGINE_DIR, "include"),
            os.path.join(ENGINE_DIR, "src", "OrderBook.cpp"),
            os.path.join(ENGINE_DIR, "src", "C_API.cpp"),
            os.path.join(ENGINE_DIR, "tests", "test_matching.cpp"),
            "-o", test_exe
        ]
        subprocess.run(cmd, check=True)
        subprocess.run([test_exe], check=True)

    # 2. Python Gateway Tests
    print("\n--- Python Gateway & WebSocket Tests ---")
    if not build_engine(force=True):
        raise RuntimeError("The C++ engine must build successfully before gateway tests can run.")
    try:
        subprocess.run([sys.executable, os.path.join(SERVER_DIR, "tests", "run_tests.py"), "-v"], check=True)
    finally:
        # The test subprocess has released the temporary library by this point.
        library = os.environ.pop("NOXUS_ENGINE_LIBRARY", None)
        if library:
            try:
                os.remove(library)
            except OSError:
                pass

def run_benchmark(orders=500000, runs=5):
    gxx = find_gxx()
    if not gxx:
        print("[!] Benchmark requires g++ compiler.")
        return
    bench_exe = os.path.join(ENGINE_DIR, "benchmarks", "main_benchmark.exe")
    cmd = [
        gxx, "-std=c++20", "-O3", "-DNDEBUG",
        "-I", os.path.join(ENGINE_DIR, "include"),
        os.path.join(ENGINE_DIR, "src", "OrderBook.cpp"),
        os.path.join(ENGINE_DIR, "benchmarks", "main_benchmark.cpp"),
        "-o", bench_exe
    ]
    subprocess.run(cmd, check=True)
    subprocess.run([bench_exe, str(orders), str(runs)], check=True)

def start_server():
    build_engine()
    sys.path.insert(0, SERVER_DIR)

    import uvicorn
    print("\n" + "=" * 60)
    print("  NOXUSLOB TRADING PLATFORM READY")
    print("  Terminal URL: http://localhost:8000")
    print("  API Docs    : http://localhost:8000/docs")
    print("  WebSocket   : ws://localhost:8000/ws/market-data")
    print("=" * 60 + "\n")

    # Open browser after 1 second delay
    def open_browser():
        time.sleep(1.2)
        webbrowser.open("http://localhost:8000")

    import threading
    threading.Thread(target=open_browser, daemon=True).start()

    from app.main import app
    uvicorn.run(app, host="127.0.0.1", port=8000, log_level="info")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="NoxusLOB Master Launcher")
    parser.add_argument("--test", action="store_true", help="Run unit tests")
    parser.add_argument("--bench", action="store_true", help="Run C++ latency benchmark")
    parser.add_argument("--orders", type=int, default=500000, help="Order count for benchmark")
    parser.add_argument("--runs", type=int, default=5, help="Repeated benchmark runs")
    args = parser.parse_args()

    if args.test:
        run_tests()
    elif args.bench:
        run_benchmark(args.orders, args.runs)
    else:
        start_server()
