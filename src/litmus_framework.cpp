#include "litmus_framework.h"
#include <iostream>
#include <iomanip>
#include <cstring>
#include <unistd.h>

LitmusTest::LitmusTest(int rank, int world_size, uint64_t iterations)
    : rank(rank), world_size(world_size), shared_mem(nullptr),
      win(MPI_WIN_NULL), iterations(iterations) {
}

LitmusTest::~LitmusTest() {
    cleanup_shared_memory();
}

void LitmusTest::setup_shared_memory() {
    MPI_Aint size = (rank == 0) ? sizeof(LitmusSharedMem) : 0;

    // Create MPI window for one-sided communication
    MPI_Win_allocate(size, 1, MPI_INFO_NULL, MPI_COMM_WORLD,
                     &shared_mem, &win);

    if (rank == 0) {
        shared_mem->reset();
    }

    // Ensure memory is initialized before proceeding
    MPI_Win_fence(0, win);
    MPI_Barrier(MPI_COMM_WORLD);
}

void LitmusTest::cleanup_shared_memory() {
    if (win != MPI_WIN_NULL) {
        MPI_Win_free(&win);
        win = MPI_WIN_NULL;
        shared_mem = nullptr;
    }
}

void LitmusTest::barrier_sync() {
    MPI_Barrier(MPI_COMM_WORLD);
}

void LitmusTest::flush_cache() {
    if (shared_mem) {
        clflushopt((void*)&shared_mem->x);
        clflushopt((void*)&shared_mem->y);
        clflushopt((void*)&shared_mem->z);
        clflushopt((void*)&shared_mem->w);
        mfence();
    }
}

TestResult LitmusTest::execute() {
    TestResult result;
    result.test_name = get_name();
    result.description = get_description();
    result.total_iterations = iterations;
    result.violations = 0;

    // Setup shared memory
    setup_shared_memory();

    auto start_time = std::chrono::high_resolution_clock::now();

    // Run the test multiple iterations
    for (uint64_t i = 0; i < iterations; i++) {
        // Reset shared memory state
        if (rank == 0) {
            shared_mem->reset();
        }

        MPI_Win_fence(0, win);
        barrier_sync();

        // Both processes execute their test code
        if (rank == 0) {
            run_process0();
        } else if (rank == 1) {
            run_process1();
        }

        // Synchronize and check for violations
        MPI_Win_fence(0, win);
        barrier_sync();

        if (rank == 0) {
            if (check_violation()) {
                result.violations++;
            }
        }

        barrier_sync();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
        end_time - start_time);

    result.avg_latency_us = duration.count() / (double)iterations;
    result.violation_rate = (double)result.violations / (double)iterations;
    result.passed = (result.violations == 0);

    cleanup_shared_memory();

    return result;
}

void print_test_header(const std::string& category) {
    std::cout << "\n" << std::string(80, '=') << "\n";
    std::cout << "  " << category << "\n";
    std::cout << std::string(80, '=') << "\n";
}

void print_test_result(const TestResult& result) {
    std::cout << "\nTest: " << result.test_name << "\n";
    std::cout << "Description: " << result.description << "\n";
    std::cout << "Iterations: " << result.total_iterations << "\n";
    std::cout << "Violations: " << result.violations << "\n";
    std::cout << "Violation Rate: " << std::fixed << std::setprecision(6)
              << (result.violation_rate * 100) << "%\n";
    std::cout << "Avg Latency: " << std::fixed << std::setprecision(3)
              << result.avg_latency_us << " μs\n";
    std::cout << "Status: " << (result.passed ? "PASSED ✓" : "FAILED ✗") << "\n";
    std::cout << std::string(80, '-') << "\n";
}

void print_summary(const std::vector<TestResult>& results) {
    int passed = 0;
    int failed = 0;
    uint64_t total_violations = 0;

    for (const auto& result : results) {
        if (result.passed) passed++;
        else failed++;
        total_violations += result.violations;
    }

    std::cout << "\n" << std::string(80, '=') << "\n";
    std::cout << "  TEST SUMMARY\n";
    std::cout << std::string(80, '=') << "\n";
    std::cout << "Total Tests: " << results.size() << "\n";
    std::cout << "Passed: " << passed << "\n";
    std::cout << "Failed: " << failed << "\n";
    std::cout << "Total Violations: " << total_violations << "\n";
    std::cout << std::string(80, '=') << "\n";
}
