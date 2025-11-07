#ifndef LITMUS_FRAMEWORK_H
#define LITMUS_FRAMEWORK_H

#include <mpi.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <chrono>

// Constants for CXL memory regions
#define CXL_MEMORY_SIZE (1ULL << 30)  // 1GB
#define CACHE_LINE_SIZE 64
#define TEST_ITERATIONS 1000000

// Memory ordering types
enum class MemoryOrder {
    RELAXED,
    ACQUIRE,
    RELEASE,
    ACQ_REL,
    SEQ_CST
};

// Test result structure
struct TestResult {
    std::string test_name;
    uint64_t total_iterations;
    uint64_t violations;
    double violation_rate;
    double avg_latency_us;
    bool passed;
    std::string description;
};

// Shared memory structure for litmus tests
struct LitmusSharedMem {
    // Aligned to cache line to avoid false sharing
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> x;
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> y;
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> z;
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> w;

    // Results from each thread/process
    alignas(CACHE_LINE_SIZE) uint64_t r0;
    alignas(CACHE_LINE_SIZE) uint64_t r1;
    alignas(CACHE_LINE_SIZE) uint64_t r2;
    alignas(CACHE_LINE_SIZE) uint64_t r3;

    // Synchronization
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> barrier;
    alignas(CACHE_LINE_SIZE) std::atomic<bool> start_flag;

    void reset() {
        x.store(0, std::memory_order_relaxed);
        y.store(0, std::memory_order_relaxed);
        z.store(0, std::memory_order_relaxed);
        w.store(0, std::memory_order_relaxed);
        r0 = 0;
        r1 = 0;
        r2 = 0;
        r3 = 0;
        barrier.store(0, std::memory_order_relaxed);
        start_flag.store(false, std::memory_order_relaxed);
    }
};

// Base class for litmus tests
class LitmusTest {
protected:
    int rank;
    int world_size;
    LitmusSharedMem* shared_mem;
    MPI_Win win;
    uint64_t iterations;

    // Helper functions
    void barrier_sync();
    void flush_cache();
    std::atomic<uint64_t>& to_atomic(std::memory_order order);

public:
    LitmusTest(int rank, int world_size, uint64_t iterations = TEST_ITERATIONS);
    virtual ~LitmusTest();

    // Setup shared memory using MPI RMA
    void setup_shared_memory();
    void cleanup_shared_memory();

    // Virtual functions for test implementation
    virtual std::string get_name() const = 0;
    virtual std::string get_description() const = 0;
    virtual void run_process0() = 0;
    virtual void run_process1() = 0;
    virtual bool check_violation() = 0;

    // Execute the test
    TestResult execute();
};

// Utility functions
void print_test_header(const std::string& category);
void print_test_result(const TestResult& result);
void print_summary(const std::vector<TestResult>& results);

// Memory barrier helpers
inline void memory_fence_seq_cst() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

inline void memory_fence_acquire() {
    std::atomic_thread_fence(std::memory_order_acquire);
}

inline void memory_fence_release() {
    std::atomic_thread_fence(std::memory_order_release);
}

// CPU cache line flush (x86-64 specific)
inline void clflush(volatile void* p) {
    asm volatile("clflush (%0)" :: "r"(p) : "memory");
}

inline void clflushopt(volatile void* p) {
    asm volatile(".byte 0x66; clflush (%0)" :: "r"(p) : "memory");
}

inline void mfence() {
    asm volatile("mfence" ::: "memory");
}

inline void sfence() {
    asm volatile("sfence" ::: "memory");
}

inline void lfence() {
    asm volatile("lfence" ::: "memory");
}

#endif // LITMUS_FRAMEWORK_H
