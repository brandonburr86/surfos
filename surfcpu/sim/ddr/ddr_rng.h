/*
SurfCPU DDR simulation random numbers
--------------------
File: ddr_rng.h Date: 10/15/26 (SurfCPU)
--------------------
A small seeded generator (splitmix64) for the DDR model and testbench, so a seed gives the
same run on every machine and library (std:: distributions are not portable), and the hash
that defines the contents of never-written DDR.
*/
#pragma once
#include <cstdint>

namespace surfddr {

inline uint64_t mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

class Rng {
public:
    explicit Rng(uint64_t seed = 1) : state_(seed * 0x9e3779b97f4a7c15ULL + 0x1234567ULL) {}
    uint64_t next() { state_ += 0x9e3779b97f4a7c15ULL; return mix64(state_); }
    uint32_t u32() { return static_cast<uint32_t>(next() >> 32); }
    // uniform in [lo, hi] (inclusive); the tiny modulo bias does not matter here
    uint64_t range(uint64_t lo, uint64_t hi) { return lo + next() % (hi - lo + 1); }
    bool percent(int p) { return static_cast<int>(next() % 100) < p; }
private:
    uint64_t state_;
};

// Byte k of never-written DDR (k = byte offset inside the 256 MiB window). The model and the
// testbench's reference memory both use it, so a read of an untouched location is checked too.
inline uint8_t initial_byte(uint32_t offset) {
    return static_cast<uint8_t>(mix64(0x5eed0000ULL ^ offset) >> 24);
}

}  // namespace surfddr
