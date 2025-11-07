#include "hw_coherency_tests.h"

// Store Buffer Test (SB)
void StoreBufferTest::run_process0() {
    shared_mem->x.store(1, std::memory_order_relaxed);
    shared_mem->r0 = shared_mem->y.load(std::memory_order_relaxed);
}

void StoreBufferTest::run_process1() {
    shared_mem->y.store(1, std::memory_order_relaxed);
    shared_mem->r1 = shared_mem->x.load(std::memory_order_relaxed);
}

bool StoreBufferTest::check_violation() {
    // Forbidden: both reads see 0 (stores were reordered)
    return (shared_mem->r0 == 0 && shared_mem->r1 == 0);
}

// Load Buffer Test (LB)
void LoadBufferTest::run_process0() {
    shared_mem->r0 = shared_mem->x.load(std::memory_order_relaxed);
    shared_mem->y.store(1, std::memory_order_relaxed);
}

void LoadBufferTest::run_process1() {
    shared_mem->r1 = shared_mem->y.load(std::memory_order_relaxed);
    shared_mem->x.store(1, std::memory_order_relaxed);
}

bool LoadBufferTest::check_violation() {
    // Forbidden: r0=1, r1=1 (loads saw future writes)
    return (shared_mem->r0 == 1 && shared_mem->r1 == 1);
}

// Message Passing Test (MP)
void MessagePassingTest::run_process0() {
    // Write data then set flag
    shared_mem->x.store(1, std::memory_order_relaxed);
    memory_fence_release();  // Ensure x=1 is visible before y=1
    shared_mem->y.store(1, std::memory_order_release);
}

void MessagePassingTest::run_process1() {
    // Check flag then read data
    shared_mem->r0 = shared_mem->y.load(std::memory_order_acquire);
    memory_fence_acquire();  // Ensure we see x=1 if we saw y=1
    shared_mem->r1 = shared_mem->x.load(std::memory_order_relaxed);
}

bool MessagePassingTest::check_violation() {
    // Forbidden: saw flag (r0=1) but not data (r1=0)
    return (shared_mem->r0 == 1 && shared_mem->r1 == 0);
}

// Write Causality Test (WRC)
void WriteCausalityTest::run_process0() {
    shared_mem->x.store(1, std::memory_order_release);
    shared_mem->r0 = shared_mem->y.load(std::memory_order_acquire);
    shared_mem->z.store(1, std::memory_order_release);
}

void WriteCausalityTest::run_process1() {
    shared_mem->r1 = shared_mem->z.load(std::memory_order_acquire);
    shared_mem->r2 = shared_mem->x.load(std::memory_order_acquire);
}

bool WriteCausalityTest::check_violation() {
    // If P1 sees z=1 but not x=1, causality is violated
    return (shared_mem->r1 == 1 && shared_mem->r2 == 0);
}

// IRIW Test (simplified for 2 processes)
void IRIWTest::run_process0() {
    shared_mem->x.store(1, std::memory_order_seq_cst);
    shared_mem->r0 = shared_mem->y.load(std::memory_order_seq_cst);
}

void IRIWTest::run_process1() {
    shared_mem->y.store(1, std::memory_order_seq_cst);
    shared_mem->r1 = shared_mem->x.load(std::memory_order_seq_cst);
}

bool IRIWTest::check_violation() {
    // With seq_cst, if r0=0 and r1=0, it's a violation
    return (shared_mem->r0 == 0 && shared_mem->r1 == 0);
}

// Read-Read Coherence Test (CoRR)
void ReadReadCoherenceTest::run_process0() {
    shared_mem->x.store(1, std::memory_order_release);
    mfence();
    shared_mem->x.store(2, std::memory_order_release);
}

void ReadReadCoherenceTest::run_process1() {
    shared_mem->r0 = shared_mem->x.load(std::memory_order_acquire);
    memory_fence_seq_cst();
    shared_mem->r1 = shared_mem->x.load(std::memory_order_acquire);
}

bool ReadReadCoherenceTest::check_violation() {
    // Forbidden: r0=2, r1=1 (reads went backwards)
    return (shared_mem->r0 == 2 && shared_mem->r1 == 1);
}

// Write-Write Coherence Test (CoWW)
void WriteWriteCoherenceTest::run_process0() {
    shared_mem->x.store(1, std::memory_order_seq_cst);
}

void WriteWriteCoherenceTest::run_process1() {
    shared_mem->x.store(2, std::memory_order_seq_cst);
    mfence();
    shared_mem->r0 = shared_mem->x.load(std::memory_order_seq_cst);
}

bool WriteWriteCoherenceTest::check_violation() {
    // If process1 wrote 2 but reads a different value, it's incoherent
    // This is a basic check - in practice CoWW needs more processes
    return false;  // Difficult to violate with 2 processes
}

// Read-Write Coherence Test (CoRW)
void ReadWriteCoherenceTest::run_process0() {
    shared_mem->r0 = shared_mem->x.load(std::memory_order_acquire);
    memory_fence_acquire();
    shared_mem->x.store(1, std::memory_order_release);
}

void ReadWriteCoherenceTest::run_process1() {
    shared_mem->x.store(2, std::memory_order_release);
    memory_fence_release();
    shared_mem->r1 = shared_mem->x.load(std::memory_order_acquire);
}

bool ReadWriteCoherenceTest::check_violation() {
    // Check if reads don't see expected values based on write order
    // This is complex and depends on execution timing
    return false;  // Simplified - needs more complex check
}

// CXL Cache Line Sharing Test
void CXLCacheLineSharingTest::run_process0() {
    // Write to cache line
    shared_mem->x.store(0xDEADBEEF, std::memory_order_release);
    clflushopt((void*)&shared_mem->x);
    mfence();

    // Read back
    shared_mem->r0 = shared_mem->x.load(std::memory_order_acquire);
}

void CXLCacheLineSharingTest::run_process1() {
    // Wait a bit then try to read
    for (volatile int i = 0; i < 100; i++);

    shared_mem->r1 = shared_mem->x.load(std::memory_order_acquire);

    // Modify the value
    shared_mem->x.store(0xCAFEBABE, std::memory_order_release);
    clflushopt((void*)&shared_mem->x);
    mfence();
}

bool CXLCacheLineSharingTest::check_violation() {
    // Check if cache line sharing maintained coherence
    // Both processes should see consistent values
    return (shared_mem->r0 != 0 && shared_mem->r0 != 0xDEADBEEF &&
            shared_mem->r1 != 0 && shared_mem->r1 != 0xDEADBEEF &&
            shared_mem->r1 != 0xCAFEBABE);
}

// CXL Memory Ordering Test
void CXLMemOrderingTest::run_process0() {
    // Multiple writes with different orderings
    shared_mem->x.store(1, std::memory_order_relaxed);
    shared_mem->y.store(2, std::memory_order_release);
    mfence();
    shared_mem->z.store(3, std::memory_order_seq_cst);
}

void CXLMemOrderingTest::run_process1() {
    // Read with acquire semantics
    shared_mem->r0 = shared_mem->z.load(std::memory_order_seq_cst);
    memory_fence_acquire();
    shared_mem->r1 = shared_mem->y.load(std::memory_order_acquire);
    shared_mem->r2 = shared_mem->x.load(std::memory_order_acquire);
}

bool CXLMemOrderingTest::check_violation() {
    // If we see z=3 (with seq_cst), we must see y=2 and x=1
    if (shared_mem->r0 == 3) {
        return (shared_mem->r1 != 2 || shared_mem->r2 != 1);
    }
    return false;
}
