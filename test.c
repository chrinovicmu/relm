#include <stdio.h>
#include <stdint.h>
#include <time.h>

int main() {
    uint64_t state = time(NULL);        // seed with current time
    volatile uint64_t sum = 0;          // prevent optimization away
    const long long iterations = 100000000LL;  // 100 million iterations

    clock_t start = clock();

    for (long long i = 0; i < iterations; ++i) {
        // Good LCG parameters → high-entropy bits
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;

        // This branch is very hard for predictors to learn
        if (state & (1ULL << 31)) {          // take bit 31
            sum += i ^ state;                // some work
        } else {
            sum += (i * 17) ^ (state >> 32);
        }
    }

    clock_t end = clock();
    double seconds = (double)(end - start) / CLOCKS_PER_SEC;

    printf("Iterations: %lld\n", iterations);
    printf("Final sum: %llu\n", sum);
    printf("Time: %.3f seconds\n", seconds);
    printf("Misprediction pressure: HIGH (branch depends on LCG high bit)\n");

    return 0;
}

