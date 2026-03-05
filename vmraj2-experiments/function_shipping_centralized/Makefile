# Makefile for wasm-shipping prototype
#
# Prerequisites:
#   1. Install wasi-sdk to ~/wasi-sdk (for compiling Wasm functions)
#   2. Install wasmtime C API (libwasmtime, headers)

# ---- Toolchains ----
CXX        = g++
WASI_CXX   = $(HOME)/wasi-sdk/bin/clang++

# ---- Wasmtime C API paths ----
# Adjust if you installed wasmtime-c-api elsewhere
WASMTIME_DIR = $(HOME)/wasmtime-c-api
WASMTIME_INC = $(WASMTIME_DIR)/include
WASMTIME_LIB = $(WASMTIME_DIR)/lib

# ---- Flags ----
CXXFLAGS   = -std=c++17 -O2 -Wall
WASI_FLAGS = -fno-exceptions -O2 -Wl,--no-entry

# ---- Targets ----
WASM_FUNCS = programs/factorial.wasm programs/fib.wasm programs/prime.wasm
BINARIES   = receiver sender

.PHONY: all clean wasm native

all: wasm native

# ---- Wasm serverless functions ----
wasm: $(WASM_FUNCS)

programs/factorial.wasm: programs/factorial.cpp
	$(WASI_CXX) $(WASI_FLAGS) -o $@ $<

programs/fib.wasm: programs/fib.cpp
	$(WASI_CXX) $(WASI_FLAGS) -o $@ $<

programs/prime.wasm: programs/prime.cpp
	$(WASI_CXX) $(WASI_FLAGS) -o $@ $<

# ---- Native binaries (receiver and sender) ----
native: $(BINARIES)

receiver: receiver.cpp
	$(CXX) $(CXXFLAGS) -I$(WASMTIME_INC) -o $@ $< \
		-L$(WASMTIME_LIB) -lwasmtime -lpthread -ldl -lm \
		-Wl,-rpath,$(WASMTIME_LIB)

sender: sender.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

# ---- Cleanup ----
clean:
	rm -f $(BINARIES) $(WASM_FUNCS)
