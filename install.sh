#!/bin/bash

VMS=(
  "sp26-cs525-1801.cs.illinois.edu"
  "sp26-cs525-1802.cs.illinois.edu"
  "sp26-cs525-1803.cs.illinois.edu"
  "sp26-cs525-1804.cs.illinois.edu"
  "sp26-cs525-1805.cs.illinois.edu"
  "sp26-cs525-1806.cs.illinois.edu"
  "sp26-cs525-1807.cs.illinois.edu"
  "sp26-cs525-1808.cs.illinois.edu"
  "sp26-cs525-1809.cs.illinois.edu"
  "sp26-cs525-1810.cs.illinois.edu"
  "sp26-cs525-1811.cs.illinois.edu"
  "sp26-cs525-1812.cs.illinois.edu"
  "sp26-cs525-1813.cs.illinois.edu"
  "sp26-cs525-1814.cs.illinois.edu"
  "sp26-cs525-1815.cs.illinois.edu"
  "sp26-cs525-1816.cs.illinois.edu"
  "sp26-cs525-1817.cs.illinois.edu"
  "sp26-cs525-1818.cs.illinois.edu"
  "sp26-cs525-1819.cs.illinois.edu"
  "sp26-cs525-1820.cs.illinois.edu"
)

USER="svbagga2"



for vm in "${VMS[@]}"; do
  echo "Setting up host: $vm"
  ssh "$USER@$vm" 'bash -s' <<'EOF' &
    set -e
    echo "Running on $(hostname)"

    # wasi-sdk
    if [ ! -d ~/wasi-sdk ]; then
        wget -q https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-25/wasi-sdk-25.0-x86_64-linux.tar.gz
        tar -xzf wasi-sdk-25.0-x86_64-linux.tar.gz -C ~/
        mv ~/wasi-sdk-25.0-x86_64-linux ~/wasi-sdk
        echo "Done installing wasi-sdk!"
    fi

    # wasmtime
    if [ ! -d ~/wasmtime-c-api ]; then
        wget -q https://github.com/bytecodealliance/wasmtime/releases/download/v30.0.0/wasmtime-v30.0.0-x86_64-linux-c-api.tar.xz
        tar -xJf wasmtime-v30.0.0-x86_64-linux-c-api.tar.xz -C ~/
        mv ~/wasmtime-v30.0.0-x86_64-linux-c-api ~/wasmtime-c-api
        echo "Done installing wasmtime!"
    fi

    # Set env vars
    export WASMTIME_HOME=~/wasmtime-c-api
    export LD_LIBRARY_PATH=$WASMTIME_HOME/lib:$LD_LIBRARY_PATH

    # Write to bashrc
    grep -qxF 'export WASMTIME_HOME=~/wasmtime-c-api' ~/.bashrc || echo 'export WASMTIME_HOME=~/wasmtime-c-api' >> ~/.bashrc
    grep -qxF 'export LD_LIBRARY_PATH=$WASMTIME_HOME/lib:$LD_LIBRARY_PATH' ~/.bashrc || echo 'export LD_LIBRARY_PATH=$WASMTIME_HOME/lib:$LD_LIBRARY_PATH' >> ~/.bashrc
    grep -qxF 'alias wasiclang="$HOME/wasi-sdk/bin/clang++ --sysroot=$HOME/wasi-sdk/share/wasi-sysroot"' ~/.bashrc || echo 'alias wasiclang="$HOME/wasi-sdk/bin/clang++ --sysroot=$HOME/wasi-sdk/share/wasi-sysroot"' >> ~/.bashrc

    # Rust
    if [ ! -d ~/.cargo ]; then
        curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --no-modify-path
        echo '. "$HOME/.cargo/env"' >> ~/.bashrc
    fi

    echo "All done on $(hostname)!"
EOF
done
wait