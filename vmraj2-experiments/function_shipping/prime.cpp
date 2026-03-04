#include <cmath>

__attribute__((export_name("isPrime")))
int isPrime(int n) {
    if (n <= 1) return 0;
    int stoppingPoint = (int)sqrt(n);
    for (int i = 2; i <= stoppingPoint; ++i) {
        if (n % i == 0) {
            return 0;
        }
    }
    return 1;
}
