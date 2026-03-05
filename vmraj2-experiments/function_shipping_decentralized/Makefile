# Makefile for wasm-shipping prototype
#
# Prerequisites:
#   1. Install wasi-sdk to ~/wasi-sdk (for compiling Wasm functions)
#   2. Install wasmtime C API (libwasmtime, headers)

# ---- Toolchains ----
CXX        = g++
WASI_CXX   = /opt/wasi-sdk-30.0-x86_64-macos/bin/clang++

# ---- Wasmtime C API paths ----
# Adjust if you installed wasmtime-c-api elsewhere
WASMTIME_DIR = /opt/wasmtime-v42.0.1-aarch64-macos-c-api
WASMTIME_INC = $(WASMTIME_DIR)/include
WASMTIME_LIB = $(WASMTIME_DIR)/lib

# ---- Flags ----
CXXFLAGS   = -std=c++17 -O2 -Wall
WASI_FLAGS = -fno-exceptions -O2 -Wl,--no-entry

# ---- Targets ----
WASM_FUNCS = programs/factorial.wasm programs/fib.wasm programs/prime.wasm
BINARIES   = receiver sender node

.PHONY: all clean wasm native

all: wasm node

# ---- Wasm serverless functions ----
wasm: $(WASM_FUNCS)

programs/factorial.wasm: programs/factorial.cpp
	$(WASI_CXX) $(WASI_FLAGS) -o $@ $<

programs/fib.wasm: programs/fib.cpp
	$(WASI_CXX) $(WASI_FLAGS) -o $@ $<

programs/prime.wasm: programs/prime.cpp
	$(WASI_CXX) $(WASI_FLAGS) -o $@ $<

# ---- P2P node ----
# Requires httplib.h and json.hpp placed in include/
# See README for download instructions.
node: node.cpp
	$(CXX) $(CXXFLAGS) -I$(WASMTIME_INC) -Iinclude -o $@ $< \
		-L$(WASMTIME_LIB) -lwasmtime -lpthread -ldl -lm \
		-Wl,-rpath,$(WASMTIME_LIB)

# ---- Cleanup ----
clean:
	rm -f $(BINARIES) $(WASM_FUNCS)
