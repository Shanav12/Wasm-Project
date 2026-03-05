// sender.cpp — Ships a Wasm function binary to a receiver node for execution
//
// Usage:
//   ./sender --program factorial --arg 10 [--host 127.0.0.1] [--port 8080]
//   ./sender --program fib --arg 20
//   ./sender --program prime --arg 97

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <map>

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

// Program name -> { wasm filename, exported function name }
struct ProgramInfo {
    std::string filename;
    std::string export_name;
};

static const std::map<std::string, ProgramInfo> PROGRAMS = {
    {"factorial", {"programs/factorial.wasm", "factorial"}},
    {"fib",       {"programs/fib.wasm",       "fib"}},
    {"prime",     {"programs/prime.wasm",      "isPrime"}},
};

// ---- Helpers ----

std::vector<uint8_t> read_file(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        fprintf(stderr, "[sender] Cannot open file: %s\n", path.c_str());
        return {};
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::vector<uint8_t> data(size);
    fread(data.data(), 1, size, f);
    fclose(f);
    return data;
}

bool write_exact(int fd, const void* buf, size_t n) {
    size_t total = 0;
    while (total < n) {
        ssize_t w = write(fd, (const char*)buf + total, n - total);
        if (w <= 0) return false;
        total += w;
    }
    return true;
}

bool write_u32(int fd, uint32_t val) {
    uint32_t net = htonl(val);
    return write_exact(fd, &net, 4);
}

bool read_u32(int fd, uint32_t* out) {
    uint32_t net;
    size_t total = 0;
    while (total < 4) {
        ssize_t r = read(fd, (char*)&net + total, 4 - total);
        if (r <= 0) return false;
        total += r;
    }
    *out = ntohl(net);
    return true;
}

void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s --program <factorial|fib|prime> --arg <int> "
                    "[--host <addr>] [--port <port>]\n", prog);
}

int main(int argc, char* argv[]) {
    std::string program;
    int32_t arg = 0;
    std::string host = "127.0.0.1";
    uint16_t port = 8080;
    bool has_program = false;
    bool has_arg = false;

    // Parse CLI arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--program") == 0 && i + 1 < argc) {
            program = argv[++i];
            has_program = true;
        } else if (strcmp(argv[i], "--arg") == 0 && i + 1 < argc) {
            arg = atoi(argv[++i]);
            has_arg = true;
        } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = (uint16_t)atoi(argv[++i]);
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!has_program || !has_arg) {
        print_usage(argv[0]);
        return 1;
    }

    // Look up the program
    auto it = PROGRAMS.find(program);
    if (it == PROGRAMS.end()) {
        fprintf(stderr, "[sender] Unknown program: '%s'\n", program.c_str());
        fprintf(stderr, "[sender] Available: factorial, fib, prime\n");
        return 1;
    }

    const ProgramInfo& info = it->second;

    // Read the wasm binary
    std::vector<uint8_t> wasm_bytes = read_file(info.filename);
    if (wasm_bytes.empty()) {
        return 1;
    }

    printf("[sender] Loaded %s (%zu bytes)\n", info.filename.c_str(), wasm_bytes.size());
    printf("[sender] Shipping to %s:%d — calling %s(%d)\n",
           host.c_str(), port, info.export_name.c_str(), arg);

    auto t_start = std::chrono::steady_clock::now();

    // Connect to receiver
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr) <= 0) {
        // Try hostname resolution
        struct hostent* he = gethostbyname(host.c_str());
        if (!he) {
            fprintf(stderr, "[sender] Cannot resolve host: %s\n", host.c_str());
            close(sock);
            return 1;
        }
        memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    auto t_connected = std::chrono::steady_clock::now();

    // Send: function name length + function name
    const std::string& func_name = info.export_name;
    if (!write_u32(sock, (uint32_t)func_name.size()) ||
        !write_exact(sock, func_name.c_str(), func_name.size())) {
        fprintf(stderr, "[sender] Failed to send function name\n");
        close(sock);
        return 1;
    }

    // Send: argument
    if (!write_u32(sock, (uint32_t)arg)) {
        fprintf(stderr, "[sender] Failed to send argument\n");
        close(sock);
        return 1;
    }

    // Send: wasm binary length + wasm binary
    if (!write_u32(sock, (uint32_t)wasm_bytes.size()) ||
        !write_exact(sock, wasm_bytes.data(), wasm_bytes.size())) {
        fprintf(stderr, "[sender] Failed to send wasm binary\n");
        close(sock);
        return 1;
    }

    auto t_sent = std::chrono::steady_clock::now();

    // Read result
    uint32_t raw_result;
    if (!read_u32(sock, &raw_result)) {
        fprintf(stderr, "[sender] Failed to read result\n");
        close(sock);
        return 1;
    }

    auto t_done = std::chrono::steady_clock::now();

    int32_t result = (int32_t)raw_result;

    if (raw_result == 0xDEADBEEF) {
        fprintf(stderr, "[sender] Receiver reported an execution error\n");
        close(sock);
        return 1;
    }

    double connect_ms = std::chrono::duration<double, std::milli>(t_connected - t_start).count();
    double send_ms    = std::chrono::duration<double, std::milli>(t_sent - t_connected).count();
    double exec_ms    = std::chrono::duration<double, std::milli>(t_done - t_sent).count();
    double total_ms   = std::chrono::duration<double, std::milli>(t_done - t_start).count();

    printf("\n[sender] Result: %s(%d) = %d\n", info.export_name.c_str(), arg, result);
    printf("[sender] Timing: connect=%.3f ms, send=%.3f ms, remote_exec=%.3f ms, total=%.3f ms\n",
           connect_ms, send_ms, exec_ms, total_ms);
    printf("[sender] Bytes shipped: %zu\n", wasm_bytes.size());

    close(sock);
    return 0;
}
