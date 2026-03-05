// node.cpp — P2P edge node for serverless WASM function execution
//
// Each node knows the network-wide function directory (hardcoded).
// It hosts a subset of functions as the source of truth.
// For functions it doesn't host, it fetches the WASM binary from the
// owning peer over HTTP, executes it locally, and returns the result.
//
// Usage: ./node <port>
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
//   Node B  port 8081  hosts: prime

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <chrono>
#include <map>
#include <fstream>

#include "httplib.h"
#include "json.hpp"

#include <wasmtime.h>
#include <wasm.h>
#include <wasi.h>

using json = nlohmann::json;

// ---- Network-wide function directory (hardcoded) -------------------------
//
// wasm_path    path to the .wasm file on the owning node's filesystem
// export_name  exported symbol name inside the WASM module
// owner_host   IP/hostname of the node that is the source of truth
// owner_port   HTTP port of the owning node
//
// A node considers itself the owner when owner_port == MY_PORT.

struct FunctionEntry {
    std::string wasm_path;
    std::string export_name;
    std::string owner_host;
    int         owner_port;
};

static const std::map<std::string, FunctionEntry> FUNCTION_DIRECTORY = {
    {"factorial", {"programs/factorial.wasm", "factorial", "127.0.0.1", 8080}},
    {"fib",       {"programs/fib.wasm",       "fib",       "127.0.0.1", 8080}},
    {"prime",     {"programs/prime.wasm",      "isPrime",   "127.0.0.1", 8081}},
};

static int MY_PORT = -1;  // set once in main() before server starts

// ---- File I/O ---------------------------------------------------------------

static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

// ---- WASM execution ---------------------------------------------------------
//
// Compiles and executes a WASM binary, calling `func_name` with one i32
// argument. Returns true on success and writes the i32 result to *result.

static bool execute_wasm(const std::vector<uint8_t>& wasm_bytes,
                         const std::string& func_name,
                         int32_t arg,
                         int32_t* result)
{
    wasm_engine_t* engine = wasm_engine_new();
    if (!engine) {
        fprintf(stderr, "[node:%d] Failed to create wasm engine\n", MY_PORT);
        return false;
    }

    wasmtime_store_t*   store   = wasmtime_store_new(engine, nullptr, nullptr);
    wasmtime_context_t* context = wasmtime_store_context(store);

    // Compile
    auto t0 = std::chrono::steady_clock::now();
    wasmtime_module_t*  module = nullptr;
    wasmtime_error_t*   error  = wasmtime_module_new(
        engine, wasm_bytes.data(), wasm_bytes.size(), &module);
    double compile_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();

    if (error || !module) {
        if (error) {
            wasm_message_t msg;
            wasmtime_error_message(error, &msg);
            fprintf(stderr, "[node:%d] Compile error: %.*s\n",
                    MY_PORT, (int)msg.size, msg.data);
            wasm_byte_vec_delete(&msg);
            wasmtime_error_delete(error);
        }
        wasmtime_store_delete(store);
        wasm_engine_delete(engine);
        return false;
    }

    // WASI setup
    wasmtime_linker_t* linker = wasmtime_linker_new(engine);
    error = wasmtime_linker_define_wasi(linker);
    if (error) {
        wasm_message_t msg;
        wasmtime_error_message(error, &msg);
        fprintf(stderr, "[node:%d] WASI linker error: %.*s\n",
                MY_PORT, (int)msg.size, msg.data);
        wasm_byte_vec_delete(&msg);
        wasmtime_error_delete(error);
        wasmtime_linker_delete(linker);
        wasmtime_module_delete(module);
        wasmtime_store_delete(store);
        wasm_engine_delete(engine);
        return false;
    }

    wasi_config_t* wasi_config = wasi_config_new();
    wasi_config_inherit_stdout(wasi_config);
    wasi_config_inherit_stderr(wasi_config);
    error = wasmtime_context_set_wasi(context, wasi_config);
    if (error) {
        wasm_message_t msg;
        wasmtime_error_message(error, &msg);
        fprintf(stderr, "[node:%d] WASI context error: %.*s\n",
                MY_PORT, (int)msg.size, msg.data);
        wasm_byte_vec_delete(&msg);
        wasmtime_error_delete(error);
        wasmtime_linker_delete(linker);
        wasmtime_module_delete(module);
        wasmtime_store_delete(store);
        wasm_engine_delete(engine);
        return false;
    }

    // Instantiate
    auto t1 = std::chrono::steady_clock::now();
    wasmtime_instance_t instance;
    wasm_trap_t*        trap  = nullptr;
    error = wasmtime_linker_instantiate(linker, context, module, &instance, &trap);
    double inst_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t1).count();

    if (error || trap) {
        if (error) {
            wasm_message_t msg;
            wasmtime_error_message(error, &msg);
            fprintf(stderr, "[node:%d] Instantiation error: %.*s\n",
                    MY_PORT, (int)msg.size, msg.data);
            wasm_byte_vec_delete(&msg);
            wasmtime_error_delete(error);
        }
        if (trap) {
            wasm_message_t msg;
            wasm_trap_message(trap, &msg);
            fprintf(stderr, "[node:%d] Instantiation trap: %.*s\n",
                    MY_PORT, (int)msg.size, msg.data);
            wasm_byte_vec_delete(&msg);
            wasm_trap_delete(trap);
        }
        wasmtime_linker_delete(linker);
        wasmtime_module_delete(module);
        wasmtime_store_delete(store);
        wasm_engine_delete(engine);
        return false;
    }

    // Look up the exported function
    wasmtime_extern_t func_extern;
    bool found = wasmtime_instance_export_get(
        context, &instance, func_name.c_str(), func_name.size(), &func_extern);

    if (!found || func_extern.kind != WASMTIME_EXTERN_FUNC) {
        fprintf(stderr, "[node:%d] Export '%s' not found\n",
                MY_PORT, func_name.c_str());
        wasmtime_linker_delete(linker);
        wasmtime_module_delete(module);
        wasmtime_store_delete(store);
        wasm_engine_delete(engine);
        return false;
    }

    // Call
    auto t2 = std::chrono::steady_clock::now();
    wasmtime_val_t params[1];
    params[0].kind    = WASMTIME_I32;
    params[0].of.i32  = arg;
    wasmtime_val_t results[1];
    error = wasmtime_func_call(
        context, &func_extern.of.func, params, 1, results, 1, &trap);
    double exec_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t2).count();

    if (error || trap) {
        if (error) {
            wasm_message_t msg;
            wasmtime_error_message(error, &msg);
            fprintf(stderr, "[node:%d] Call error: %.*s\n",
                    MY_PORT, (int)msg.size, msg.data);
            wasm_byte_vec_delete(&msg);
            wasmtime_error_delete(error);
        }
        if (trap) {
            wasm_message_t msg;
            wasm_trap_message(trap, &msg);
            fprintf(stderr, "[node:%d] Call trap: %.*s\n",
                    MY_PORT, (int)msg.size, msg.data);
            wasm_byte_vec_delete(&msg);
            wasm_trap_delete(trap);
        }
        wasmtime_linker_delete(linker);
        wasmtime_module_delete(module);
        wasmtime_store_delete(store);
        wasm_engine_delete(engine);
        return false;
    }

    *result = results[0].of.i32;
    printf("[node:%d] %s(%d) = %d  (compile=%.2f ms, inst=%.2f ms, exec=%.2f ms)\n",
           MY_PORT, func_name.c_str(), arg, *result, compile_ms, inst_ms, exec_ms);

    wasmtime_linker_delete(linker);
    wasmtime_module_delete(module);
    wasmtime_store_delete(store);
    wasm_engine_delete(engine);
    return true;
}

// ---- P2P fetch --------------------------------------------------------------
//
// Fetches the raw WASM binary for `func_name` from the owning peer node.

static std::vector<uint8_t> fetch_wasm_from_peer(const std::string& host,
                                                   int port,
                                                   const std::string& func_name)
{
    httplib::Client client(host, port);
    client.set_connection_timeout(5, 0);

    printf("[node:%d] Fetching '%s' from %s:%d\n",
           MY_PORT, func_name.c_str(), host.c_str(), port);

    auto res = client.Get("/functions/" + func_name + "/binary");
    if (!res || res->status != 200) {
        fprintf(stderr, "[node:%d] Failed to fetch '%s' from %s:%d (status=%d)\n",
                MY_PORT, func_name.c_str(), host.c_str(), port,
                res ? res->status : -1);
        return {};
    }

    const std::string& body = res->body;
    printf("[node:%d] Received %zu bytes\n", MY_PORT, body.size());
    return std::vector<uint8_t>(body.begin(), body.end());
}

// ---- HTTP handlers ----------------------------------------------------------

// POST /invoke
//   {"function": "factorial", "arg": 10}
static void handle_invoke(const httplib::Request& req, httplib::Response& res) {
    json body;
    try {
        body = json::parse(req.body);
    } catch (...) {
        res.status = 400;
        res.set_content(json{{"error", "invalid JSON"}}.dump(), "application/json");
        return;
    }

    if (!body.contains("function") || !body.contains("arg")) {
        res.status = 400;
        res.set_content(
            json{{"error", "request must contain 'function' and 'arg'"}}.dump(),
            "application/json");
        return;
    }

    std::string func_name;
    int32_t     arg;
    try {
        func_name = body["function"].get<std::string>();
        arg       = body["arg"].get<int32_t>();
    } catch (...) {
        res.status = 400;
        res.set_content(json{{"error", "invalid field types"}}.dump(), "application/json");
        return;
    }

    auto it = FUNCTION_DIRECTORY.find(func_name);
    if (it == FUNCTION_DIRECTORY.end()) {
        res.status = 404;
        res.set_content(
            json{{"error", "unknown function: " + func_name}}.dump(),
            "application/json");
        return;
    }

    const FunctionEntry& entry    = it->second;
    bool                 is_local = (entry.owner_port == MY_PORT);

    auto t_start = std::chrono::steady_clock::now();

    // Get the WASM binary — read from disk if local, fetch from peer otherwise
    std::vector<uint8_t> wasm_bytes;
    if (is_local) {
        wasm_bytes = read_file(entry.wasm_path);
        if (wasm_bytes.empty()) {
            res.status = 500;
            res.set_content(
                json{{"error", "failed to read local wasm file"}}.dump(),
                "application/json");
            return;
        }
    } else {
        wasm_bytes = fetch_wasm_from_peer(
            entry.owner_host, entry.owner_port, func_name);
        if (wasm_bytes.empty()) {
            res.status = 502;
            res.set_content(
                json{{"error", "failed to fetch wasm from peer"}}.dump(),
                "application/json");
            return;
        }
    }

    double fetch_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t_start).count();

    int32_t result = 0;
    if (!execute_wasm(wasm_bytes, entry.export_name, arg, &result)) {
        res.status = 500;
        res.set_content(
            json{{"error", "wasm execution failed"}}.dump(),
            "application/json");
        return;
    }

    double total_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t_start).count();

    json resp;
    resp["result"]   = result;
    resp["source"]   = is_local ? "local" : "remote";
    resp["function"] = func_name;
    resp["arg"]      = arg;
    resp["timing_ms"] = {
        {"fetch",  is_local ? 0.0 : fetch_ms},
        {"total",  total_ms}
    };
    res.set_content(resp.dump(), "application/json");
}

// GET /functions
static void handle_list_functions(const httplib::Request&, httplib::Response& res) {
    json hosted = json::array();
    json known  = json::array();

    for (const auto& [name, entry] : FUNCTION_DIRECTORY) {
        known.push_back(name);
        if (entry.owner_port == MY_PORT)
            hosted.push_back(name);
    }

    json resp;
    resp["hosted"] = hosted;
    resp["known"]  = known;
    res.set_content(resp.dump(2), "application/json");
}

// GET /functions/<name>/binary  — P2P: serve raw WASM bytes to a peer
static void handle_get_binary(const httplib::Request& req, httplib::Response& res) {
    std::string func_name = req.matches[1];

    auto it = FUNCTION_DIRECTORY.find(func_name);
    if (it == FUNCTION_DIRECTORY.end()) {
        res.status = 404;
        res.set_content(
            json{{"error", "unknown function"}}.dump(), "application/json");
        return;
    }

    if (it->second.owner_port != MY_PORT) {
        res.status = 403;
        res.set_content(
            json{{"error", "function not hosted on this node"}}.dump(),
            "application/json");
        return;
    }

    auto wasm_bytes = read_file(it->second.wasm_path);
    if (wasm_bytes.empty()) {
        res.status = 500;
        res.set_content(
            json{{"error", "failed to read wasm file"}}.dump(), "application/json");
        return;
    }

    printf("[node:%d] Serving %zu bytes of '%s' to peer\n",
           MY_PORT, wasm_bytes.size(), func_name.c_str());
    res.set_content(
        std::string(wasm_bytes.begin(), wasm_bytes.end()),
        "application/octet-stream");
}

// ---- Main -------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    MY_PORT = atoi(argv[1]);

    printf("[node:%d] Network function directory:\n", MY_PORT);
    for (const auto& [name, entry] : FUNCTION_DIRECTORY) {
        const char* tag = (entry.owner_port == MY_PORT) ? "  <-- hosted here" : "";
        printf("[node:%d]   %-12s  owner=%s:%d%s\n",
               MY_PORT, name.c_str(), entry.owner_host.c_str(), entry.owner_port, tag);
    }
    printf("\n");

    httplib::Server svr;

    svr.Post("/invoke",                          handle_invoke);
    svr.Get ("/functions",                       handle_list_functions);
    svr.Get (R"(/functions/([^/]+)/binary)",     handle_get_binary);

    printf("[node:%d] Listening on http://0.0.0.0:%d\n\n", MY_PORT, MY_PORT);
    svr.listen("0.0.0.0", MY_PORT);

    return 0;
}
