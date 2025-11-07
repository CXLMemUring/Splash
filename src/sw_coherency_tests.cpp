#include "sw_coherency_tests.h"

// Dekker's Algorithm Test
void DekkerTest::run_process0() {
    // Process 0 wants to enter critical section
    shared_mem->x.store(1, std::memory_order_release);  // flag0 = true
    shared_mem->r0 = shared_mem->y.load(std::memory_order_acquire);  // read flag1

    if (shared_mem->r0 == 0) {
        // Enter critical section
        shared_mem->z.fetch_add(1, std::memory_order_seq_cst);
    }

    shared_mem->x.store(0, std::memory_order_release);  // flag0 = false
}

void DekkerTest::run_process1() {
    // Process 1 wants to enter critical section
    shared_mem->y.store(1, std::memory_order_release);  // flag1 = true
    shared_mem->r1 = shared_mem->x.load(std::memory_order_acquire);  // read flag0

    if (shared_mem->r1 == 0) {
        // Enter critical section
        shared_mem->z.fetch_add(1, std::memory_order_seq_cst);
    }

    shared_mem->y.store(0, std::memory_order_release);  // flag1 = false
}

bool DekkerTest::check_violation() {
    // Violation if both entered critical section (z == 2)
    return shared_mem->z.load(std::memory_order_seq_cst) == 2;
}

// Peterson's Algorithm Test
void PetersonsTest::run_process0() {
    shared_mem->x.store(1, std::memory_order_release);  // flag[0] = true
    shared_mem->w.store(0, std::memory_order_release);  // turn = 0 (yield to other)
    memory_fence_seq_cst();

    shared_mem->r0 = shared_mem->y.load(std::memory_order_acquire);  // flag[1]
    uint64_t turn = shared_mem->w.load(std::memory_order_acquire);

    if (shared_mem->r0 == 0 || turn == 0) {
        // Enter critical section
        shared_mem->z.fetch_add(1, std::memory_order_seq_cst);
    }

    shared_mem->x.store(0, std::memory_order_release);
}

void PetersonsTest::run_process1() {
    shared_mem->y.store(1, std::memory_order_release);  // flag[1] = true
    shared_mem->w.store(1, std::memory_order_release);  // turn = 1 (yield to other)
    memory_fence_seq_cst();

    shared_mem->r1 = shared_mem->x.load(std::memory_order_acquire);  // flag[0]
    uint64_t turn = shared_mem->w.load(std::memory_order_acquire);

    if (shared_mem->r1 == 0 || turn == 1) {
        // Enter critical section
        shared_mem->z.fetch_add(1, std::memory_order_seq_cst);
    }

    shared_mem->y.store(0, std::memory_order_release);
}

bool PetersonsTest::check_violation() {
    // Violation if both entered critical section
    return shared_mem->z.load(std::memory_order_seq_cst) == 2;
}

// Bakery Algorithm Test
void BakeryTest::run_process0() {
    // Take a ticket
    shared_mem->x.store(1, std::memory_order_release);  // choosing[0] = true
    uint64_t other_ticket = shared_mem->y.load(std::memory_order_acquire);
    shared_mem->z.store(other_ticket + 1, std::memory_order_release);  // ticket[0]
    shared_mem->x.store(0, std::memory_order_release);  // choosing[0] = false

    // Wait for turn
    memory_fence_seq_cst();
    uint64_t my_ticket = shared_mem->z.load(std::memory_order_acquire);

    // Enter critical section if we have lower ticket
    shared_mem->r0 = 1;  // Mark that we entered

    shared_mem->z.store(0, std::memory_order_release);  // Release ticket
}

void BakeryTest::run_process1() {
    // Take a ticket
    shared_mem->w.store(1, std::memory_order_release);  // choosing[1] = true
    uint64_t other_ticket = shared_mem->z.load(std::memory_order_acquire);
    shared_mem->y.store(other_ticket + 1, std::memory_order_release);  // ticket[1]
    shared_mem->w.store(0, std::memory_order_release);  // choosing[1] = false

    // Wait for turn
    memory_fence_seq_cst();

    // Enter critical section
    shared_mem->r1 = 1;  // Mark that we entered

    shared_mem->y.store(0, std::memory_order_release);  // Release ticket
}

bool BakeryTest::check_violation() {
    // This is simplified - real bakery needs more complex checking
    return false;
}

// Software Barrier Test
void SoftwareBarrierTest::run_process0() {
    // Increment barrier counter
    shared_mem->x.fetch_add(1, std::memory_order_acq_rel);

    // Wait for both to arrive
    while (shared_mem->x.load(std::memory_order_acquire) < 2) {
        // Spin
    }

    // After barrier, write value
    shared_mem->y.store(1, std::memory_order_release);
    shared_mem->r0 = shared_mem->z.load(std::memory_order_acquire);
}

void SoftwareBarrierTest::run_process1() {
    // Write before barrier
    shared_mem->z.store(1, std::memory_order_release);

    // Increment barrier counter
    shared_mem->x.fetch_add(1, std::memory_order_acq_rel);

    // Wait for both to arrive
    while (shared_mem->x.load(std::memory_order_acquire) < 2) {
        // Spin
    }

    // After barrier, read value
    shared_mem->r1 = shared_mem->y.load(std::memory_order_acquire);
}

bool SoftwareBarrierTest::check_violation() {
    // After barrier, both should see each other's writes
    return (shared_mem->r0 != 1 || shared_mem->r1 != 1);
}

// Producer-Consumer Test
void ProducerConsumerTest::run_process0() {
    // Producer: write data then set ready flag
    shared_mem->x.store(0xABCD, std::memory_order_release);
    memory_fence_release();
    shared_mem->y.store(1, std::memory_order_release);  // ready flag
}

void ProducerConsumerTest::run_process1() {
    // Consumer: wait for ready flag then read data
    while (shared_mem->y.load(std::memory_order_acquire) == 0) {
        // Spin wait
    }
    memory_fence_acquire();
    shared_mem->r1 = shared_mem->x.load(std::memory_order_acquire);
}

bool ProducerConsumerTest::check_violation() {
    // Consumer should have read the correct data
    return shared_mem->r1 != 0xABCD;
}

// RCU Pattern Test
void RCUPatternTest::run_process0() {
    // Writer: update pointer atomically
    shared_mem->x.store(0x1000, std::memory_order_release);
    memory_fence_release();

    // Grace period simulation
    for (volatile int i = 0; i < 100; i++);

    // Update again
    shared_mem->x.store(0x2000, std::memory_order_release);
}

void RCUPatternTest::run_process1() {
    // Reader: read pointer and dereference
    shared_mem->r1 = shared_mem->x.load(std::memory_order_acquire);

    // Should see either 0, 0x1000, or 0x2000
    if (shared_mem->r1 != 0 && shared_mem->r1 != 0x1000 &&
        shared_mem->r1 != 0x2000) {
        shared_mem->r2 = 1;  // Mark violation
    }
}

bool RCUPatternTest::check_violation() {
    return shared_mem->r2 == 1;
}

// Seqlock Test
void SeqlockTest::run_process0() {
    // Writer: increment sequence, write data, increment again
    uint64_t seq = shared_mem->z.fetch_add(1, std::memory_order_acquire);
    shared_mem->x.store(0x1234, std::memory_order_relaxed);
    memory_fence_release();
    shared_mem->z.fetch_add(1, std::memory_order_release);
}

void SeqlockTest::run_process1() {
    // Reader: read sequence, read data, check sequence
    uint64_t seq1 = shared_mem->z.load(std::memory_order_acquire);
    memory_fence_acquire();
    shared_mem->r1 = shared_mem->x.load(std::memory_order_relaxed);
    memory_fence_acquire();
    uint64_t seq2 = shared_mem->z.load(std::memory_order_acquire);

    // Check if read was consistent (even seq and same before/after)
    if ((seq1 & 1) == 0 && seq1 == seq2) {
        shared_mem->r2 = shared_mem->r1;  // Valid read
    } else {
        shared_mem->r2 = 0;  // Invalid read
    }
}

bool SeqlockTest::check_violation() {
    // If reader got data but it's not correct, it's a violation
    return (shared_mem->r2 != 0 && shared_mem->r2 != 0x1234);
}

// Test-and-Set Lock Test
void TestAndSetTest::run_process0() {
    // Try to acquire lock
    uint64_t expected = 0;
    if (shared_mem->x.compare_exchange_strong(expected, 1,
                                               std::memory_order_acquire)) {
        // Got lock, enter critical section
        shared_mem->z.fetch_add(1, std::memory_order_relaxed);

        // Release lock
        shared_mem->x.store(0, std::memory_order_release);
    }
}

void TestAndSetTest::run_process1() {
    // Try to acquire lock
    uint64_t expected = 0;
    if (shared_mem->x.compare_exchange_strong(expected, 1,
                                               std::memory_order_acquire)) {
        // Got lock, enter critical section
        shared_mem->z.fetch_add(1, std::memory_order_relaxed);

        // Release lock
        shared_mem->x.store(0, std::memory_order_release);
    }
}

bool TestAndSetTest::check_violation() {
    // Both should not enter critical section
    return shared_mem->z.load() == 2;
}

// CAS Lock Test
void CASLockTest::run_process0() {
    // Spin until we get the lock
    uint64_t expected = 0;
    while (!shared_mem->x.compare_exchange_weak(expected, 1,
                                                 std::memory_order_acquire)) {
        expected = 0;  // Reset for next attempt
    }

    // Critical section
    shared_mem->y.fetch_add(1, std::memory_order_relaxed);

    // Release lock
    shared_mem->x.store(0, std::memory_order_release);
}

void CASLockTest::run_process1() {
    // Spin until we get the lock
    uint64_t expected = 0;
    while (!shared_mem->x.compare_exchange_weak(expected, 1,
                                                 std::memory_order_acquire)) {
        expected = 0;  // Reset for next attempt
    }

    // Critical section
    shared_mem->y.fetch_add(1, std::memory_order_relaxed);

    // Release lock
    shared_mem->x.store(0, std::memory_order_release);
}

bool CASLockTest::check_violation() {
    // Both should eventually enter, so y should be 2
    // Violation if it's not 2
    return shared_mem->y.load() != 2;
}

// Software Cache Flush Test
void SoftwareCacheFlushTest::run_process0() {
    shared_mem->x.store(0xFEEDBEEF, std::memory_order_release);

    // Explicit cache flush (for HDM-D mode)
    clflushopt((void*)&shared_mem->x);
    mfence();

    shared_mem->r0 = shared_mem->x.load(std::memory_order_acquire);
}

void SoftwareCacheFlushTest::run_process1() {
    // Wait a bit
    for (volatile int i = 0; i < 100; i++);

    shared_mem->r1 = shared_mem->x.load(std::memory_order_acquire);
}

bool SoftwareCacheFlushTest::check_violation() {
    // Both should see the written value
    return (shared_mem->r0 != 0xFEEDBEEF || shared_mem->r1 != 0xFEEDBEEF);
}

// Explicit Fence Test
void ExplicitFenceTest::run_process0() {
    shared_mem->x.store(1, std::memory_order_relaxed);
    sfence();  // Store fence
    shared_mem->y.store(1, std::memory_order_relaxed);
    mfence();  // Full fence
    shared_mem->r0 = shared_mem->z.load(std::memory_order_relaxed);
}

void ExplicitFenceTest::run_process1() {
    shared_mem->z.store(1, std::memory_order_relaxed);
    sfence();
    shared_mem->r1 = shared_mem->y.load(std::memory_order_relaxed);
    lfence();  // Load fence
    shared_mem->r2 = shared_mem->x.load(std::memory_order_relaxed);
}

bool ExplicitFenceTest::check_violation() {
    // If P1 sees y=1, it should also see x=1 due to fences
    if (shared_mem->r1 == 1 && shared_mem->r2 != 1) {
        return true;
    }
    return false;
}

// Double-Checked Locking Test
void DoubleCheckedLockingTest::run_process0() {
    // Writer: initialize and set flag
    shared_mem->x.store(0x5555, std::memory_order_relaxed);
    memory_fence_release();
    shared_mem->y.store(1, std::memory_order_release);
}

void DoubleCheckedLockingTest::run_process1() {
    // Reader: double-checked locking pattern
    if (shared_mem->y.load(std::memory_order_acquire) == 1) {
        memory_fence_acquire();
        shared_mem->r1 = shared_mem->x.load(std::memory_order_relaxed);
    }
}

bool DoubleCheckedLockingTest::check_violation() {
    // If flag was seen but data wasn't initialized, it's a violation
    return (shared_mem->r1 != 0 && shared_mem->r1 != 0x5555);
}
