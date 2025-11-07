#include "litmus_framework.h"
#include "hw_coherency_tests.h"
#include "sw_coherency_tests.h"
#include <iostream>
#include <vector>
#include <memory>
#include <cstring>

void print_usage(const char* prog_name) {
    std::cout << "Usage: mpirun -np 2 " << prog_name << " [options]\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --help              Show this help message\n";
    std::cout << "  -i, --iterations N      Number of iterations per test (default: 1000000)\n";
    std::cout << "  -t, --test TYPE         Run specific test type: hw, sw, or all (default: all)\n";
    std::cout << "  --hw-only               Run only hardware coherency tests\n";
    std::cout << "  --sw-only               Run only software coherency tests\n";
    std::cout << "\nNote: This program requires exactly 2 MPI processes.\n";
}

void run_hardware_tests(int rank, int size, uint64_t iterations,
                       std::vector<TestResult>& results) {
    print_test_header("HARDWARE CACHE COHERENCY TESTS");

    std::vector<std::unique_ptr<LitmusTest>> hw_tests;

    hw_tests.push_back(std::make_unique<StoreBufferTest>(rank, size, iterations));
    hw_tests.push_back(std::make_unique<LoadBufferTest>(rank, size, iterations));
    hw_tests.push_back(std::make_unique<MessagePassingTest>(rank, size, iterations));
    hw_tests.push_back(std::make_unique<WriteCausalityTest>(rank, size, iterations));
    hw_tests.push_back(std::make_unique<IRIWTest>(rank, size, iterations));
    hw_tests.push_back(std::make_unique<ReadReadCoherenceTest>(rank, size, iterations));
    hw_tests.push_back(std::make_unique<WriteWriteCoherenceTest>(rank, size, iterations));
    hw_tests.push_back(std::make_unique<ReadWriteCoherenceTest>(rank, size, iterations));
    hw_tests.push_back(std::make_unique<CXLCacheLineSharingTest>(rank, size, iterations));
    hw_tests.push_back(std::make_unique<CXLMemOrderingTest>(rank, size, iterations));

    for (auto& test : hw_tests) {
        if (rank == 0) {
            std::cout << "\nRunning: " << test->get_name() << "..." << std::flush;
        }

        TestResult result = test->execute();

        if (rank == 0) {
            std::cout << " Done\n";
            print_test_result(result);
            results.push_back(result);
        }
    }
}

void run_software_tests(int rank, int size, uint64_t iterations,
                       std::vector<TestResult>& results) {
    print_test_header("SOFTWARE CACHE COHERENCY TESTS");

    std::vector<std::unique_ptr<LitmusTest>> sw_tests;

    sw_tests.push_back(std::make_unique<DekkerTest>(rank, size, iterations));
    sw_tests.push_back(std::make_unique<PetersonsTest>(rank, size, iterations));
    sw_tests.push_back(std::make_unique<BakeryTest>(rank, size, iterations));
    sw_tests.push_back(std::make_unique<SoftwareBarrierTest>(rank, size, iterations));
    sw_tests.push_back(std::make_unique<ProducerConsumerTest>(rank, size, iterations));
    sw_tests.push_back(std::make_unique<RCUPatternTest>(rank, size, iterations));
    sw_tests.push_back(std::make_unique<SeqlockTest>(rank, size, iterations));
    sw_tests.push_back(std::make_unique<TestAndSetTest>(rank, size, iterations));
    sw_tests.push_back(std::make_unique<CASLockTest>(rank, size, iterations));
    sw_tests.push_back(std::make_unique<SoftwareCacheFlushTest>(rank, size, iterations));
    sw_tests.push_back(std::make_unique<ExplicitFenceTest>(rank, size, iterations));
    sw_tests.push_back(std::make_unique<DoubleCheckedLockingTest>(rank, size, iterations));

    for (auto& test : sw_tests) {
        if (rank == 0) {
            std::cout << "\nRunning: " << test->get_name() << "..." << std::flush;
        }

        TestResult result = test->execute();

        if (rank == 0) {
            std::cout << " Done\n";
            print_test_result(result);
            results.push_back(result);
        }
    }
}

int main(int argc, char* argv[]) {
    // Initialize MPI
    MPI_Init(&argc, &argv);

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    // Validate we have exactly 2 processes
    if (size != 2) {
        if (rank == 0) {
            std::cerr << "Error: This program requires exactly 2 MPI processes.\n";
            std::cerr << "Usage: mpirun -np 2 " << argv[0] << "\n";
        }
        MPI_Finalize();
        return 1;
    }

    // Parse command line arguments
    uint64_t iterations = TEST_ITERATIONS;
    bool run_hw = true;
    bool run_sw = true;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            if (rank == 0) {
                print_usage(argv[0]);
            }
            MPI_Finalize();
            return 0;
        } else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--iterations") == 0) {
            if (i + 1 < argc) {
                iterations = std::stoull(argv[++i]);
            }
        } else if (strcmp(argv[i], "--hw-only") == 0) {
            run_hw = true;
            run_sw = false;
        } else if (strcmp(argv[i], "--sw-only") == 0) {
            run_hw = false;
            run_sw = true;
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--test") == 0) {
            if (i + 1 < argc) {
                std::string test_type = argv[++i];
                if (test_type == "hw") {
                    run_hw = true;
                    run_sw = false;
                } else if (test_type == "sw") {
                    run_hw = false;
                    run_sw = true;
                } else if (test_type == "all") {
                    run_hw = true;
                    run_sw = true;
                }
            }
        }
    }

    std::vector<TestResult> results;

    if (rank == 0) {
        std::cout << "\n";
        std::cout << "╔════════════════════════════════════════════════════════════════════════════╗\n";
        std::cout << "║                    CXL CACHE COHERENCY LITMUS TESTS                        ║\n";
        std::cout << "╚════════════════════════════════════════════════════════════════════════════╝\n";
        std::cout << "\nTest Configuration:\n";
        std::cout << "  MPI Processes: " << size << "\n";
        std::cout << "  Iterations per test: " << iterations << "\n";
        std::cout << "  Test Types: ";
        if (run_hw && run_sw) std::cout << "Hardware + Software\n";
        else if (run_hw) std::cout << "Hardware Only\n";
        else std::cout << "Software Only\n";
        std::cout << "\n";
    }

    MPI_Barrier(MPI_COMM_WORLD);

    // Run tests
    try {
        if (run_hw) {
            run_hardware_tests(rank, size, iterations, results);
        }

        if (run_sw) {
            run_software_tests(rank, size, iterations, results);
        }

        // Print summary
        if (rank == 0) {
            print_summary(results);
        }

    } catch (const std::exception& e) {
        if (rank == 0) {
            std::cerr << "Error: " << e.what() << "\n";
        }
        MPI_Finalize();
        return 1;
    }

    MPI_Finalize();
    return 0;
}
