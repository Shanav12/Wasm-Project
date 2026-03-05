// fib.cpp
__attribute__((export_name("fib")))
int fib(int n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}
