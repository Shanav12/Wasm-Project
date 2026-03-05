// receiver.cpp — Edge node that receives and executes Wasm function binaries
//
// Protocol (over TCP):
//   1. Client sends 4 bytes: length of function name (uint32_t, network byte order)
//   2. Client sends function name (e.g., "fib", "factorial", "isPrime")
//   3. Client sends 4 bytes: function argument (int32_t, network byte order)
//   4. Client sends 4 bytes: length of wasm binary (uint32_t, network byte order)
//   5. Client sends wasm binary bytes
//
// The receiver loads the wasm module, looks up the exported function by name,
// calls it with the argument, and sends back the 4-byte result.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <chrono>

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <wasmtime.h>
#include <wasm.h>
#include <wasi.h>

// ---- Networking helpers ----

// Read exactly n bytes from a file descriptor
bool read_exact(int fd, void* buf, size_t n) {
    size_t total = 0;
    while (total < n) {
        ssize_t r = read(fd, (char*)buf + total, n - total);
        if (r <= 0) return false;
        total += r;
    }
    return true;
}

// Read a uint32 in network byte order
bool read_u32(int fd, uint32_t* out) {
    uint32_t net;
    if (!read_exact(fd, &net, 4)) return false;
    *out = ntohl(net);
    return true;
}

// Write a uint32 in network byte order
bool write_u32(int fd, uint32_t val) {
    uint32_t net = htonl(val);
    return write(fd, &net, 4) == 4;
}

// ---- Wasmtime execution ----

// Load a wasm binary, call the named function with one i32 arg, return the result.
// Returns true on success, sets *result.
bool execute_wasm(const std::vector<uint8_t>& wasm_bytes,
                  const std::string& func_name,
                  int32_t arg,
                  int32_t* result)
{
    wasm_engine_t* engine = wasm_engine_new();
    if (!engine) {
        fprintf(stderr, "[receiver] Failed to create wasm engine\n");
        return false;
    }

    wasmtime_store_t* store = wasmtime_store_new(engine, nullptr, nullptr);
    wasmtime_context_t* context = wasmtime_store_context(store);

    // Compile the module
    auto t_compile_start = std::chrono::steady_clock::now();

    wasmtime_module_t* module = nullptr;
    wasmtime_error_t* error = wasmtime_module_new(
        engine, wasm_bytes.data(), wasm_bytes.size(), &module);

    auto t_compile_end = std::chrono::steady_clock::now();
    double compile_ms = std::chrono::duration<double, std::milli>(
        t_compile_end - t_compile_start).count();

    if (error || !module) {
        fprintf(stderr, "[receiver] Failed to compile wasm module\n");
        if (error) {
            wasm_message_t msg;
            wasmtime_error_message(error, &msg);
            fprintf(stderr, "[receiver] Compile error: %.*s\n", (int)msg.size, msg.data);
            wasm_byte_vec_delete(&msg);
            wasmtime_error_delete(error);
        }
        wasmtime_store_delete(store);
        wasm_engine_delete(engine);
        return false;
    }

    printf("[receiver] Module compiled in %.3f ms (%zu bytes)\n",
           compile_ms, wasm_bytes.size());

    // Create a linker and define WASI imports
    wasmtime_linker_t* linker = wasmtime_linker_new(engine);

    error = wasmtime_linker_define_wasi(linker);
    if (error) {
        fprintf(stderr, "[receiver] Failed to define WASI in linker\n");
        wasm_message_t msg;
        wasmtime_error_message(error, &msg);
        fprintf(stderr, "[receiver] WASI error: %.*s\n", (int)msg.size, msg.data);
        wasm_byte_vec_delete(&msg);
        wasmtime_error_delete(error);
        wasmtime_linker_delete(linker);
        wasmtime_module_delete(module);
        wasmtime_store_delete(store);
        wasm_engine_delete(engine);
        return false;
    }

    // Configure WASI on the store context
    wasi_config_t* wasi_config = wasi_config_new();
    wasi_config_inherit_stdout(wasi_config);
    wasi_config_inherit_stderr(wasi_config);

    error = wasmtime_context_set_wasi(context, wasi_config);
    if (error) {
        fprintf(stderr, "[receiver] Failed to set WASI context\n");
        wasm_message_t msg;
        wasmtime_error_message(error, &msg);
        fprintf(stderr, "[receiver] WASI context error: %.*s\n", (int)msg.size, msg.data);
        wasm_byte_vec_delete(&msg);
        wasmtime_error_delete(error);
        wasmtime_linker_delete(linker);
        wasmtime_module_delete(module);
        wasmtime_store_delete(store);
        wasm_engine_delete(engine);
        return false;
    }

    // Instantiate the module through the linker
    auto t_inst_start = std::chrono::steady_clock::now();

    wasmtime_instance_t instance;
    wasm_trap_t* trap = nullptr;
    error = wasmtime_linker_instantiate(linker, context, module, &instance, &trap);

    auto t_inst_end = std::chrono::steady_clock::now();
    double inst_ms = std::chrono::duration<double, std::milli>(
        t_inst_end - t_inst_start).count();

    if (error || trap) {
        fprintf(stderr, "[receiver] Failed to instantiate module\n");
        if (error) {
            wasm_message_t msg;
            wasmtime_error_message(error, &msg);
            fprintf(stderr, "[receiver] Instantiation error: %.*s\n", (int)msg.size, msg.data);
            wasm_byte_vec_delete(&msg);
            wasmtime_error_delete(error);
        }
        if (trap) {
            wasm_message_t msg;
            wasm_trap_message(trap, &msg);
            fprintf(stderr, "[receiver] Instantiation trap: %.*s\n", (int)msg.size, msg.data);
            wasm_byte_vec_delete(&msg);
            wasm_trap_delete(trap);
        }
        wasmtime_linker_delete(linker);
        wasmtime_module_delete(module);
        wasmtime_store_delete(store);
        wasm_engine_delete(engine);
        return false;
    }

    printf("[receiver] Module instantiated in %.3f ms\n", inst_ms);

    // Look up the exported function
    wasmtime_extern_t func_extern;
    bool found = wasmtime_instance_export_get(
        context, &instance, func_name.c_str(), func_name.size(), &func_extern);

    if (!found || func_extern.kind != WASMTIME_EXTERN_FUNC) {
        fprintf(stderr, "[receiver] Export '%s' not found or not a function\n",
                func_name.c_str());
        wasmtime_linker_delete(linker);
        wasmtime_module_delete(module);
        wasmtime_store_delete(store);
        wasm_engine_delete(engine);
        return false;
    }

    // Call the function with one i32 argument
    auto t_exec_start = std::chrono::steady_clock::now();

    wasmtime_val_t params[1];
    params[0].kind = WASMTIME_I32;
    params[0].of.i32 = arg;

    wasmtime_val_t results[1];

    error = wasmtime_func_call(context, &func_extern.of.func, params, 1, results, 1, &trap);

    auto t_exec_end = std::chrono::steady_clock::now();
    double exec_ms = std::chrono::duration<double, std::milli>(
        t_exec_end - t_exec_start).count();

    if (error || trap) {
        fprintf(stderr, "[receiver] Function call failed\n");
        if (error) {
            wasm_message_t msg;
            wasmtime_error_message(error, &msg);
            fprintf(stderr, "[receiver] Call error: %.*s\n", (int)msg.size, msg.data);
            wasm_byte_vec_delete(&msg);
            wasmtime_error_delete(error);
        }
        if (trap) {
            wasm_message_t msg;
            wasm_trap_message(trap, &msg);
            fprintf(stderr, "[receiver] Call trap: %.*s\n", (int)msg.size, msg.data);
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

    printf("[receiver] %s(%d) = %d  (executed in %.3f ms)\n",
           func_name.c_str(), arg, *result, exec_ms);
    printf("[receiver] Total: compile=%.3f ms, instantiate=%.3f ms, execute=%.3f ms\n",
           compile_ms, inst_ms, exec_ms);

    // Cleanup
    wasmtime_linker_delete(linker);
    wasmtime_module_delete(module);
    wasmtime_store_delete(store);
    wasm_engine_delete(engine);
    return true;
}

// ---- Main server loop ----

int main(int argc, char* argv[]) {
    uint16_t port = 8080;
    if (argc > 1) {
        port = (uint16_t)atoi(argv[1]);
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(server_fd, 5) < 0) {
        perror("listen");
        return 1;
    }

    printf("[receiver] Listening on port %d...\n", port);

    while (true) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        printf("\n[receiver] Connection from %s:%d\n",
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        auto t_total_start = std::chrono::steady_clock::now();

        // 1. Read function name
        uint32_t name_len;
        if (!read_u32(client_fd, &name_len) || name_len > 256) {
            fprintf(stderr, "[receiver] Bad function name length\n");
            close(client_fd);
            continue;
        }

        std::string func_name(name_len, '\0');
        if (!read_exact(client_fd, &func_name[0], name_len)) {
            fprintf(stderr, "[receiver] Failed to read function name\n");
            close(client_fd);
            continue;
        }

        // 2. Read function argument
        uint32_t raw_arg;
        if (!read_u32(client_fd, &raw_arg)) {
            fprintf(stderr, "[receiver] Failed to read argument\n");
            close(client_fd);
            continue;
        }
        int32_t arg = (int32_t)raw_arg;

        // 3. Read wasm binary
        uint32_t wasm_len;
        if (!read_u32(client_fd, &wasm_len) || wasm_len > 10 * 1024 * 1024) {
            fprintf(stderr, "[receiver] Bad wasm binary length: %u\n", wasm_len);
            close(client_fd);
            continue;
        }

        std::vector<uint8_t> wasm_bytes(wasm_len);
        if (!read_exact(client_fd, wasm_bytes.data(), wasm_len)) {
            fprintf(stderr, "[receiver] Failed to read wasm binary\n");
            close(client_fd);
            continue;
        }

        printf("[receiver] Received function '%s' with arg=%d (%u bytes of wasm)\n",
               func_name.c_str(), arg, wasm_len);

        // 4. Execute
        int32_t result = 0;
        if (execute_wasm(wasm_bytes, func_name, arg, &result)) {
            write_u32(client_fd, (uint32_t)result);
        } else {
            // Send error marker
            write_u32(client_fd, 0xDEADBEEF);
        }

        auto t_total_end = std::chrono::steady_clock::now();
        double total_ms = std::chrono::duration<double, std::milli>(
            t_total_end - t_total_start).count();
        printf("[receiver] Total request time: %.3f ms\n", total_ms);

        close(client_fd);
    }

    close(server_fd);
    return 0;
}
