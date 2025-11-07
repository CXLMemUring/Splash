#ifndef SW_COHERENCY_TESTS_H
#define SW_COHERENCY_TESTS_H

#include "litmus_framework.h"

// Dekker's Algorithm Test
// Tests software mutual exclusion algorithm
// Classic test for memory ordering in concurrent systems
class DekkerTest : public LitmusTest {
public:
    using LitmusTest::LitmusTest;
    std::string get_name() const override { return "Dekker's Algorithm"; }
    std::string get_description() const override {
        return "Tests Dekker's mutual exclusion - both should not enter critical section";
    }
    void run_process0() override;
    void run_process1() override;
    bool check_violation() override;
};

// Peterson's Algorithm Test
// Another classic mutual exclusion algorithm
class PetersonsTest : public LitmusTest {
public:
    using LitmusTest::LitmusTest;
    std::string get_name() const override { return "Peterson's Algorithm"; }
    std::string get_description() const override {
        return "Tests Peterson's lock - mutual exclusion should be preserved";
    }
    void run_process0() override;
    void run_process1() override;
    bool check_violation() override;
};

// Lamport's Bakery Algorithm (simplified 2-process)
class BakeryTest : public LitmusTest {
public:
    using LitmusTest::LitmusTest;
    std::string get_name() const override { return "Lamport's Bakery (2-process)"; }
    std::string get_description() const override {
        return "Tests Bakery algorithm - ensures FIFO mutual exclusion";
    }
    void run_process0() override;
    void run_process1() override;
    bool check_violation() override;
};

// Software Barrier Test
// Tests if software barriers work correctly across CXL
class SoftwareBarrierTest : public LitmusTest {
public:
    using LitmusTest::LitmusTest;
    std::string get_name() const override { return "Software Barrier"; }
    std::string get_description() const override {
        return "Tests software barrier synchronization across CXL fabric";
    }
    void run_process0() override;
    void run_process1() override;
    bool check_violation() override;
};

// Producer-Consumer Test
// Tests software-based producer-consumer queue
class ProducerConsumerTest : public LitmusTest {
public:
    using LitmusTest::LitmusTest;
    std::string get_name() const override { return "Producer-Consumer"; }
    std::string get_description() const override {
        return "Tests single-producer single-consumer queue correctness";
    }
    void run_process0() override;
    void run_process1() override;
    bool check_violation() override;
};

// Read-Copy-Update (RCU) Pattern Test
// Tests RCU-like pattern for lock-free reads
class RCUPatternTest : public LitmusTest {
public:
    using LitmusTest::LitmusTest;
    std::string get_name() const override { return "RCU Pattern"; }
    std::string get_description() const override {
        return "Tests RCU-like pattern for concurrent reads and updates";
    }
    void run_process0() override;
    void run_process1() override;
    bool check_violation() override;
};

// Seqlock Test
// Tests sequence lock pattern
class SeqlockTest : public LitmusTest {
public:
    using LitmusTest::LitmusTest;
    std::string get_name() const override { return "Sequence Lock"; }
    std::string get_description() const override {
        return "Tests seqlock pattern for writer priority";
    }
    void run_process0() override;
    void run_process1() override;
    bool check_violation() override;
};

// Test-and-Set Lock Test
class TestAndSetTest : public LitmusTest {
public:
    using LitmusTest::LitmusTest;
    std::string get_name() const override { return "Test-and-Set Lock"; }
    std::string get_description() const override {
        return "Tests atomic test-and-set based spinlock";
    }
    void run_process0() override;
    void run_process1() override;
    bool check_violation() override;
};

// Compare-and-Swap (CAS) Lock Test
class CASLockTest : public LitmusTest {
public:
    using LitmusTest::LitmusTest;
    std::string get_name() const override { return "CAS Lock"; }
    std::string get_description() const override {
        return "Tests compare-and-swap based lock";
    }
    void run_process0() override;
    void run_process1() override;
    bool check_violation() override;
};

// CXL-specific: Software Cache Flush Coherence
// Tests if explicit cache flushes maintain coherence
class SoftwareCacheFlushTest : public LitmusTest {
public:
    using LitmusTest::LitmusTest;
    std::string get_name() const override { return "Software Cache Flush"; }
    std::string get_description() const override {
        return "Tests software-initiated cache flushes for HDM-D (Device Coherent) mode";
    }
    void run_process0() override;
    void run_process1() override;
    bool check_violation() override;
};

// CXL-specific: Explicit Memory Fence Test
// Tests memory fence effectiveness across CXL
class ExplicitFenceTest : public LitmusTest {
public:
    using LitmusTest::LitmusTest;
    std::string get_name() const override { return "Explicit Memory Fence"; }
    std::string get_description() const override {
        return "Tests explicit memory fences (mfence/sfence/lfence) across CXL";
    }
    void run_process0() override;
    void run_process1() override;
    bool check_violation() override;
};

// Double-Checked Locking Test
// Tests the broken double-checked locking pattern
class DoubleCheckedLockingTest : public LitmusTest {
public:
    using LitmusTest::LitmusTest;
    std::string get_name() const override { return "Double-Checked Locking"; }
    std::string get_description() const override {
        return "Tests double-checked locking pattern (should detect if broken)";
    }
    void run_process0() override;
    void run_process1() override;
    bool check_violation() override;
};

#endif // SW_COHERENCY_TESTS_H
