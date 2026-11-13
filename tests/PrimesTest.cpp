#include <iostream>
#include <vector>
#include <cassert>
#include <cmath>
#include "../src/Library/Utilities/Primes.h"
#include "../src/Library/Sampling/HaltonPoints.h"

// Helper to check if a number is prime
bool IsPrime(unsigned int number) {
    if (number <= 1) return false;
    if (number <= 3) return true;
    if (number % 2 == 0 || number % 3 == 0) return false;
    for (unsigned int i = 5; i * i <= number; i += 6) {
        if (number % i == 0 || number % (i + 2) == 0)
            return false;
    }
    return true;
}

void TestGeneratePrimesBasic() {
    std::cout << "Running TestGeneratePrimesBasic..." << std::endl;
    std::vector<unsigned int> primes;
    unsigned int n = 20;

    RISE::Primes::GeneratePrimes(n, primes);

    std::cout << "Primes up to " << n << ": ";
    for (size_t i = 0; i < primes.size(); ++i) {
        std::cout << primes[i] << " ";
    }
    std::cout << std::endl;

    assert(primes.size() == n);
    assert(primes[0] == 2);
    assert(primes[1] == 3);
    assert(primes[2] == 5);
    assert(primes[4] == 11);
    assert(primes[19] == 71); 

    std::cout << "TestGeneratePrimesBasic passed!" << std::endl;
}

void TestEdgeCases() {
    std::cout << "Running TestEdgeCases..." << std::endl;
    std::vector<unsigned int> primes;
    
    // Case n=0: The function contract says n >= 2, but let's see if it crashes.
    // Implementation pushes 2 and 3 unconditionally.
    primes.clear();
    RISE::Primes::GeneratePrimes(0, primes);
    assert(primes.size() >= 2); 
    assert(primes[0] == 2);
    assert(primes[1] == 3);

    // Case n=1
    primes.clear();
    RISE::Primes::GeneratePrimes(1, primes);
    assert(primes.size() >= 2);
    assert(primes[0] == 2);
    assert(primes[1] == 3);

    // Case n=2
    primes.clear();
    RISE::Primes::GeneratePrimes(2, primes);
    assert(primes.size() == 2);
    assert(primes[0] == 2);
    assert(primes[1] == 3);

    std::cout << "TestEdgeCases passed!" << std::endl;
}

void TestLargeSequence() {
    std::cout << "Running TestLargeSequence..." << std::endl;
    std::vector<unsigned int> primes;
    unsigned int n = 10000; // Generate 10,000 primes

    RISE::Primes::GeneratePrimes(n, primes);

    assert(primes.size() == n);
    assert(primes[0] == 2);
    
    // The 10,000th prime is 104729
    assert(primes[n-1] == 104729);

    // Verify all generated numbers are actually prime
    for (size_t i = 0; i < primes.size(); ++i) {
        if (!IsPrime(primes[i])) {
            std::cerr << "Error: " << primes[i] << " is not prime!" << std::endl;
            assert(false);
        }
    }

    // Verify ordering
    for (size_t i = 1; i < primes.size(); ++i) {
        assert(primes[i] > primes[i-1]);
    }

    std::cout << "TestLargeSequence passed!" << std::endl;
}

static bool TestHaltonSequences()
{
    struct Sample {
        unsigned int dimension, index;
        double expected;
    };
    const Sample known[] = {
        { 0, 1, 0x1p+3 },
        { 1, 1, 0x1.5555555555555p-2 },
        { 2, 5, 0x1.eb851eb851ebap-4 },
        { 7, 19, 0x1.f33c46ea04412p-6 },
        { 64, 2345, 0x1.9d387df3362d4p-2 },
        { 255, 4095, 0x1.30a8e69a153f9p-2 },
    };
    RISE::MultiHalton first, second;
    for (const auto& sample : known) {
        if (std::abs(first.halton(sample.dimension, sample.index) - sample.expected) > 1e-15) {
            std::cerr << "Halton baseline mismatch at dimension " << sample.dimension
                      << ", index " << sample.index << '\n';
            return false;
        }
    }
    for (unsigned int dimension = 0; dimension < RISE::QMC_NUM_PRIMES; ++dimension) {
        first.Reset();
        const double start = first.next_halton(dimension);
        const double next = first.next_halton(dimension);
        if (start != first.mod1(first.halton(dimension, 0)) ||
            next != first.mod1(first.halton(dimension, 1)) ||
            start != second.next_halton(dimension) ||
            next != second.next_halton(dimension)) {
            std::cerr << "Halton sampler state mismatch at dimension " << dimension << '\n';
            return false;
        }
    }
    second.Reset();
    const double start = second.next_halton(7);
    second.next_halton(7);
    second.Reset();
    if (second.next_halton(7) != start ||
        first.next_halton(255) != first.mod1(first.halton(255, 2))) {
        std::cerr << "Halton reset did not replay one sampler independently\n";
        return false;
    }
    return true;
}

int main() {
    TestGeneratePrimesBasic();
    TestEdgeCases();
    TestLargeSequence();
    return TestHaltonSequences() ? 0 : 1;
}
