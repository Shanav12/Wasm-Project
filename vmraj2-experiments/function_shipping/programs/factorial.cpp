// factorial.cpp
__attribute__((export_name("factorial")))
int factorial(int n) {
	int acc = 1;
	for (int i = 1; i <= n; ++i) {
		acc *= i;
	}

	return acc;
}
