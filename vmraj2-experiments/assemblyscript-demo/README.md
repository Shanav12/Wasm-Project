Here is a fibonacci program to test webassembly.

It uses assemblyscript (typescript + webassembly) + wasmtime as a runtime.

To compile:
```
asc fib.ts -o fib.wasm --runtime stub
```
Assemblyscript assumes you are using a browser-based runtime, by passing in a "stub" runtime, it compiles without that assumption.

To run:
```
wasmtime run --invoke fib fib.wasm 10
```
