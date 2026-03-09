// node_rust.rs — P2P edge node for serverless WASM function execution (Rust)
//
// Rust rewrite of node.cpp. Same API, same hardcoded network layout.
//
// Usage: cargo run -- <port>
//
// Dependencies (Cargo.toml):
//   axum          = "0.7"
//   tokio         = { version = "1", features = ["full"] }
//   reqwest       = { version = "0.12", features = ["json"] }
//   serde_json    = "1"
//   wasmtime      = "28"
//   wasmtime-wasi = "28"
//   anyhow        = "1"
//
// User-facing HTTP API:
//   POST /invoke
//     Body:     {"function": "<name>", "arg": <int>}
//     Response: {"result": <int>, "source": "local"|"remote",
//                "function": "...", "arg": ..., "timing_ms": {...}}
//
//   GET /functions
//     Response: {"hosted": [...], "known": [...]}
//
// P2P HTTP API (node-to-node only):
//   GET /functions/<name>/binary
//     Response: raw WASM bytes (application/octet-stream)
//
// Hardcoded network layout (two nodes on localhost):
//   Node A  port 8080  hosts: factorial, fib
//   Node B  port 8080  hosts: prime   (on a different host)

use anyhow::{anyhow, Result};
use axum::{
    extract::{Path, State},
    http::{header, StatusCode},
    response::{IntoResponse, Response},
    routing::{get, post},
    Json, Router,
};
use reqwest::Client;
use serde_json::{json, Value};
use std::{collections::HashMap, sync::Arc, time::Instant};
use wasmtime::*;
use wasmtime_wasi::preview1::WasiP1Ctx;

// ---- Network-wide function directory (hardcoded) ---------------------------

#[derive(Clone)]
struct FunctionEntry {
    wasm_path: String,
    export_name: String,
    owner_host: String,
    owner_port: u16,
}

fn function_directory() -> HashMap<String, FunctionEntry> {
    [
        (
            "factorial",
            FunctionEntry {
                wasm_path: "programs/factorial.wasm".into(),
                export_name: "factorial".into(),
                owner_host: "sp26-cs525-1801.cs.illinois.edu".into(),
                owner_port: 8080,
            },
        ),
        (
            "fib",
            FunctionEntry {
                wasm_path: "programs/fib.wasm".into(),
                export_name: "fib".into(),
                owner_host: "sp26-cs525-1801.cs.illinois.edu".into(),
                owner_port: 8080,
            },
        ),
        (
            "prime",
            FunctionEntry {
                wasm_path: "programs/prime.wasm".into(),
                export_name: "isPrime".into(),
                owner_host: "sp26-cs525-1802.cs.illinois.edu".into(),
                owner_port: 8080,
            },
        ),
    ]
    .into_iter()
    .map(|(k, v)| (k.to_string(), v))
    .collect()
}

// ---- Shared server state ----------------------------------------------------

#[derive(Clone)]
struct AppState {
    engine: Arc<Engine>,
    my_port: u16,
    my_host: String,
    functions: Arc<HashMap<String, FunctionEntry>>,
    http_client: Client,
}

// ---- WASM execution ---------------------------------------------------------
//
// Compiles and executes a WASM binary, calling `func_name(arg: i32) -> i32`.
// Returns (result, compile_ms, instantiate_ms, exec_ms) on success.
//
// This is intentionally synchronous — call it inside tokio::task::spawn_blocking.

fn execute_wasm(
    engine: Arc<Engine>,
    wasm_bytes: &[u8],
    func_name: &str,
    arg: i32,
    my_port: u16,
) -> Result<(i32, f64, f64, f64)> {
    // Linker with WASI preview1 support
    let mut linker: Linker<WasiP1Ctx> = Linker::new(&engine);
    wasmtime_wasi::preview1::add_to_linker_sync(&mut linker, |t| t)?;

    // Compile
    let t0 = Instant::now();
    let module = Module::from_binary(&engine, wasm_bytes)?;
    let compile_ms = t0.elapsed().as_secs_f64() * 1000.0;

    // WASI context + Store
    let wasi = wasmtime_wasi::WasiCtxBuilder::new()
        .inherit_stdout()
        .inherit_stderr()
        .build_p1();
    let mut store = Store::new(&engine, wasi);

    // Instantiate
    let t1 = Instant::now();
    let instance = linker.instantiate(&mut store, &module)?;
    let inst_ms = t1.elapsed().as_secs_f64() * 1000.0;

    // Look up the exported function
    let func = instance
        .get_typed_func::<i32, i32>(&mut store, func_name)
        .map_err(|e| anyhow!("export '{}' not found: {}", func_name, e))?;

    // Call
    let t2 = Instant::now();
    let result = func.call(&mut store, arg)?;
    let exec_ms = t2.elapsed().as_secs_f64() * 1000.0;

    println!(
        "[node:{}] {}({}) = {}  (compile={:.2} ms, inst={:.2} ms, exec={:.2} ms)",
        my_port, func_name, arg, result, compile_ms, inst_ms, exec_ms
    );

    Ok((result, compile_ms, inst_ms, exec_ms))
}

// ---- HTTP handlers ----------------------------------------------------------

// POST /invoke  {"function": "...", "arg": <int>}
async fn handle_invoke(State(state): State<AppState>, body: String) -> Response {
    let body: Value = match serde_json::from_str(&body) {
        Ok(v) => v,
        Err(_) => {
            return (
                StatusCode::BAD_REQUEST,
                Json(json!({"error": "invalid JSON"})),
            )
                .into_response()
        }
    };

    let func_name = match body.get("function").and_then(Value::as_str) {
        Some(s) => s.to_string(),
        None => {
            return (
                StatusCode::BAD_REQUEST,
                Json(json!({"error": "request must contain 'function' and 'arg'"})),
            )
                .into_response()
        }
    };

    let arg = match body.get("arg").and_then(Value::as_i64) {
        Some(n) => n as i32,
        None => {
            return (
                StatusCode::BAD_REQUEST,
                Json(json!({"error": "request must contain 'function' and 'arg'"})),
            )
                .into_response()
        }
    };

    let entry = match state.functions.get(&func_name) {
        Some(e) => e.clone(),
        None => {
            return (
                StatusCode::NOT_FOUND,
                Json(json!({"error": format!("unknown function: {}", func_name)})),
            )
                .into_response()
        }
    };

    let is_local = entry.owner_host == state.my_host && entry.owner_port == state.my_port;
    let t_start = Instant::now();

    // Get WASM bytes — read from disk if local, fetch from peer otherwise
    let wasm_bytes: Vec<u8> = if is_local {
        match std::fs::read(&entry.wasm_path) {
            Ok(b) => b,
            Err(_) => {
                return (
                    StatusCode::INTERNAL_SERVER_ERROR,
                    Json(json!({"error": "failed to read local wasm file"})),
                )
                    .into_response()
            }
        }
    } else {
        let url = format!(
            "http://{}:{}/functions/{}/binary",
            entry.owner_host, entry.owner_port, func_name
        );
        println!(
            "[node:{}] Fetching '{}' from {}:{}",
            state.my_port, func_name, entry.owner_host, entry.owner_port
        );
        match state.http_client.get(&url).send().await {
            Ok(resp) if resp.status().is_success() => match resp.bytes().await {
                Ok(b) => {
                    println!("[node:{}] Received {} bytes", state.my_port, b.len());
                    b.to_vec()
                }
                Err(e) => {
                    eprintln!("[node:{}] Failed to read peer response: {}", state.my_port, e);
                    return (
                        StatusCode::BAD_GATEWAY,
                        Json(json!({"error": "failed to fetch wasm from peer"})),
                    )
                        .into_response()
                }
            },
            Ok(resp) => {
                eprintln!(
                    "[node:{}] Failed to fetch '{}' from {}:{} (status={})",
                    state.my_port, func_name, entry.owner_host, entry.owner_port, resp.status()
                );
                return (
                    StatusCode::BAD_GATEWAY,
                    Json(json!({"error": "failed to fetch wasm from peer"})),
                )
                    .into_response()
            }
            Err(e) => {
                eprintln!(
                    "[node:{}] Failed to connect to peer for '{}': {}",
                    state.my_port, func_name, e
                );
                return (
                    StatusCode::BAD_GATEWAY,
                    Json(json!({"error": "failed to fetch wasm from peer"})),
                )
                    .into_response()
            }
        }
    };

    let fetch_ms = t_start.elapsed().as_secs_f64() * 1000.0;

    // Run WASM on a blocking thread (compilation is CPU-bound)
    let engine = state.engine;
    let export_name = entry.export_name.clone();
    let my_port = state.my_port;
    let exec_result =
        tokio::task::spawn_blocking(move || execute_wasm(engine, &wasm_bytes, &export_name, arg, my_port))
            .await;

    let (result, compile_ms, inst_ms, exec_ms) = match exec_result {
        Ok(Ok(r)) => r,
        Ok(Err(e)) => {
            eprintln!("[node:{}] WASM execution error: {}", state.my_port, e);
            return (
                StatusCode::INTERNAL_SERVER_ERROR,
                Json(json!({"error": "wasm execution failed"})),
            )
                .into_response()
        }
        Err(e) => {
            eprintln!("[node:{}] Task join error: {}", state.my_port, e);
            return (
                StatusCode::INTERNAL_SERVER_ERROR,
                Json(json!({"error": "wasm execution failed"})),
            )
                .into_response()
        }
    };

    let total_ms = t_start.elapsed().as_secs_f64() * 1000.0;

    Json(json!({
        "result": result,
        "source": if is_local { "local" } else { "remote" },
        "function": func_name,
        "arg": arg,
        "timing_ms": {
            "fetch": if is_local { 0.0 } else { fetch_ms },
            "compile": compile_ms,
            "instantiate": inst_ms,
            "exec": exec_ms,
            "total": total_ms,
        }
    }))
    .into_response()
}

// GET /functions
async fn handle_list_functions(State(state): State<AppState>) -> impl IntoResponse {
    let mut hosted: Vec<&str> = Vec::new();
    let mut known: Vec<&str> = Vec::new();

    for (name, entry) in state.functions.iter() {
        known.push(name);
        if entry.owner_port == state.my_port {
            hosted.push(name);
        }
    }

    Json(json!({ "hosted": hosted, "known": known }))
}

// GET /functions/:name/binary  — P2P: serve raw WASM bytes to a peer
async fn handle_get_binary(
    State(state): State<AppState>,
    Path(func_name): Path<String>,
) -> Response {
    let entry = match state.functions.get(&func_name) {
        Some(e) => e.clone(),
        None => {
            return (
                StatusCode::NOT_FOUND,
                Json(json!({"error": "unknown function"})),
            )
                .into_response()
        }
    };

    if entry.owner_host != state.my_host || entry.owner_port != state.my_port {
        return (
            StatusCode::FORBIDDEN,
            Json(json!({"error": "function not hosted on this node"})),
        )
            .into_response();
    }

    match std::fs::read(&entry.wasm_path) {
        Ok(bytes) => {
            println!(
                "[node:{}] Serving {} bytes of '{}' to peer",
                state.my_port,
                bytes.len(),
                func_name
            );
            ([(header::CONTENT_TYPE, "application/octet-stream")], bytes).into_response()
        }
        Err(_) => (
            StatusCode::INTERNAL_SERVER_ERROR,
            Json(json!({"error": "failed to read wasm file"})),
        )
            .into_response(),
    }
}

// ---- Main -------------------------------------------------------------------

#[tokio::main]
async fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        eprintln!("Usage: {} <port>", args[0]);
        std::process::exit(1);
    }

    let my_port: u16 = args[1].parse().expect("port must be a valid u16");
    let my_host = "sp26-cs525-1801.cs.illinois.edu".to_string();
    let functions = Arc::new(function_directory());

    println!("[node:{}] Network function directory:", my_port);
    let mut names: Vec<&String> = functions.keys().collect();
    names.sort();
    for name in &names {
        let entry = &functions[*name];
        let tag = if entry.owner_port == my_port { "  <-- hosted here" } else { "" };
        println!(
            "[node:{}]   {:<12}  owner={}:{}{}",
            my_port, name, entry.owner_host, entry.owner_port, tag
        );
    }
    println!();

    let mut config = Config::new();
    config.strategy(Strategy::Winch);
    let engine = Engine::default();
    let state = AppState {
        engine: Arc::new(engine),
        my_port,
        my_host,
        functions,
        http_client: Client::builder()
            .timeout(std::time::Duration::from_secs(5))
            .build()
            .unwrap(),
    };

    let app = Router::new()
        .route("/invoke", post(handle_invoke))
        .route("/functions", get(handle_list_functions))
        .route("/functions/:name/binary", get(handle_get_binary))
        .with_state(state);

    let addr = format!("0.0.0.0:{}", my_port);
    let listener = tokio::net::TcpListener::bind(&addr).await.unwrap();
    println!("[node:{}] Listening on http://{}\n", my_port, addr);
    axum::serve(listener, app).await.unwrap();
}
