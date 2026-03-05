Prototype of function shipping system for serverless edge networks using WebAssembly.

## Dependencies

### WASI SDK (for compiling .wasm functions)
```
# Download from https://github.com/WebAssembly/wasi-sdk/releases
# Extract to ~/wasi-sdk
```

### Wasmtime C API
```
# Download from https://github.com/bytecodealliance/wasmtime/releases
# Extract to ~/wasmtime-c-api  (needs include/ and lib/ subdirectories)
```

### cpp-httplib (single header, for node.cpp)
```
mkdir -p include
curl -sL https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h \
     -o include/httplib.h
```

### nlohmann/json (single header, for node.cpp)
```
curl -sL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp \
     -o include/json.hpp
```

---

## Compiling a single WASM function

```
wasiclang -fno-exceptions -O2 -Wl,--no-entry -o fib.wasm fib.cpp
wasmtime --invoke fib fib.wasm 10
```

`wasiclang` is an alias to the path of the wasi-sdk clang++ compiler.
`-fno-exceptions` is required because wasi-sdk doesn't support C++ exceptions.

---

## P2P node (recommended)

`node.cpp` is a single binary that acts as both a function host and an invoker.
Each node knows the network-wide function directory and will fetch WASM from the
owning peer when it needs to execute a function it doesn't host locally.

**Hardcoded layout (two nodes on localhost):**
| Node | Port | Hosts |
|------|------|-------|
| A | 8080 | factorial, fib |
| B | 8081 | prime |

### Build
```
make
```

### Run
```
# Terminal 1 — Node A
./node 8080

# Terminal 2 — Node B
./node 8081
```

### HTTP API

Invoke a function (node resolves location automatically):
```
curl -X POST http://localhost:8080/invoke \
  -H "Content-Type: application/json" \
  -d '{"function": "factorial", "arg": 10}'

# {"result":3628800,"source":"local","function":"factorial","arg":10,...}

curl -X POST http://localhost:8080/invoke \
  -H "Content-Type: application/json" \
  -d '{"function": "prime", "arg": 17}'

# {"result":1,"source":"remote","function":"prime","arg":17,...}
# Node A fetches prime.wasm from Node B, executes locally, returns result.
```

List functions known to a node:
```
curl http://localhost:8080/functions
# {"hosted":["factorial","fib"],"known":["factorial","fib","prime"]}
```

---

## Legacy sender/receiver (for reference)

```
make native

./receiver 8080            # on one machine
./sender --program fib --arg 30 --host 192.168.1.50 --port 8080  # on another
```
