Prototype of function shipping system

To compile program:
```
wasiclang -fno-exceptions -O2 -Wl,--no-entry -o fib.wasm fib.cpp
wasmtime --invoke fib fib.wasm 10
```

`wasiclang` is an alias to the path of the wasi-sdk clang++ compiler. Kinda hacky but works for now.

`-fno-exceptions` is present because wasi-sdk doesn't support C++ exceptions.
