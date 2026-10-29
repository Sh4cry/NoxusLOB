cmake_minimum_required(VERSION 3.20)
project(noxus_lob VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep optimization decisions with CMAKE_BUILD_TYPE so Debug and Release
# exercise different configurations as documented.
if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

option(NOXUS_ENABLE_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(NOXUS_ENABLE_SANITIZERS AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

include_directories(include)

# Shared Library (for Python / C FFI bindings)
add_library(noxus_engine SHARED
    src/OrderBook.cpp
    src/C_API.cpp
)

# Static Library
add_library(noxus_engine_static STATIC
    src/OrderBook.cpp
)

# Unit Tests Executable
add_executable(test_matching
    tests/test_matching.cpp
    src/C_API.cpp
)
target_link_libraries(test_matching PRIVATE noxus_engine_static)

# Benchmark Executable
add_executable(benchmarks
    benchmarks/main_benchmark.cpp
)
target_link_libraries(benchmarks PRIVATE noxus_engine_static)

enable_testing()
add_test(NAME MatchingTests COMMAND test_matching)
