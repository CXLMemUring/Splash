#ifndef HW_COHERENCY_TESTS_H
#define HW_COHERENCY_TESTS_H

#include "litmus_framework.h"

// Store Buffer Test (SB)
// Tests if stores can be reordered or buffered
// P0: x=1, r0=y  |  P1: y=1, r1=x
// Forbidden outcome: r0=0, r1=0
class StoreBufferTest : public LitmusTest {
public:
    using LitmusTest::LitmusTest;
    std::string get_name() const override { return "Store Buffer (SB)"; }
    std::string get_description() const override {
        return "Tests store buffering - detects if stores can be reordered";
    }
    void run_process0() override;
    void run_process1() override;
    bool check_violation() override;
};

// Load Buffer Test (LB)
// Tests if loads can be reordered
// P0: r0=x, y=1  |  P1: r1=y, x=1
// Forbidden outcome: r0=1, r1=1
class LoadBufferTest : public LitmusTest {
public:
    using LitmusTest::LitmusTest;
    std::string get_name() const override { return "Load Buffer (LB)"; }
    std::string get_description() const override {
        return "Tests load buffering - detects if loads can be reordered before stores";
    }
    void run_process0() override;
    void run_process1() override;
    bool check_violation() override;
};

// Message Passing Test (MP)
// Tests if message passing semantics are preserved
// P0: x=1, y=1  |  P1: r0=y, r1=x
// Forbidden outcome: r0=1, r1=0 (saw message but not data)
class MessagePassingTest : public LitmusTest {
public:
    using LitmusTest::LitmusTest;
    std::string get_name() const override { return "Message Passing (MP)"; }
    std::string get_description() const override {
        return "Tests message passing - ensures data is visible when flag is set";
    }
    void run_process0() override;
    void run_process1() override;
    bool check_violation() override;
};

// Write-to-Read Causality Test (WRC)
// Tests causality of writes across multiple processors
// P0: x=1  |  P1: r0=x, y=1  |  P2: r1=y, r2=x
// This is simplified for 2 processes
class WriteCausalityTest : public LitmusTest {
public:
    using LitmusTest::LitmusTest;
    std::string get_name() const override { return "Write Causality (WRC-simplified)"; }
    std::string get_description() const override {
        return "Tests write causality across CXL fabric";
    }
    void run_process0() override;
    void run_process1() override;
    bool check_violation() override;
};

// Independent Reads of Independent Writes (IRIW)
// Tests if two processors see writes in different orders
// P0: x=1  |  P1: y=1  |  P2: r0=x, r1=y  |  P3: r2=y, r3=x
// Simplified for 2 processes
class IRIWTest : public LitmusTest {
public:
    using LitmusTest::LitmusTest;
    std::string get_name() const override { return "IRIW (2-process variant)"; }
    std::string get_description() const override {
        return "Tests if writes are observed in consistent order";
    }
    void run_process0() override;
    void run_process1() override;
    bool check_violation() override;
};

// Read-Read Coherence (CoRR)
// Tests if two consecutive reads see values in order
// P0: x=1, x=2  |  P1: r0=x, r1=x
// Forbidden outcome: r0=2, r1=1
class ReadReadCoherenceTest : public LitmusTest {
public:
    using LitmusTest::LitmusTest;
    std::string get_name() const override { return "Read-Read Coherence (CoRR)"; }
    std::string get_description() const override {
        return "Tests if consecutive reads observe values in order";
    }
    void run_process0() override;
    void run_process1() override;
    bool check_violation() override;
};

// Write-Write Coherence (CoWW)
// Tests if writes to same location are ordered
// P0: x=1  |  P1: x=2  |  P2: r0=x, r1=x
// For 2 processes: simplified version
class WriteWriteCoherenceTest : public LitmusTest {
public:
    using LitmusTest::LitmusTest;
    std::string get_name() const override { return "Write-Write Coherence (CoWW)"; }
    std::string get_description() const override {
        return "Tests if writes to same location are globally ordered";
    }
    void run_process0() override;
    void run_process1() override;
    bool check_violation() override;
};

// Read-Write Coherence (CoRW)
// Tests read-after-write coherence
// P0: r0=x, x=1  |  P1: r1=x
class ReadWriteCoherenceTest : public LitmusTest {
public:
    using LitmusTest::LitmusTest;
    std::string get_name() const override { return "Read-Write Coherence (CoRW)"; }
    std::string get_description() const override {
        return "Tests if reads see the most recent writes";
    }
    void run_process0() override;
    void run_process1() override;
    bool check_violation() override;
};

// CXL-specific: HDM-H (Hardware Coherent) Cache Line Sharing
// Tests cache line sharing across CXL fabric with hardware coherence
class CXLCacheLineSharingTest : public LitmusTest {
public:
    using LitmusTest::LitmusTest;
    std::string get_name() const override { return "CXL Cache Line Sharing"; }
    std::string get_description() const override {
        return "Tests cache line sharing across CXL.cache protocol";
    }
    void run_process0() override;
    void run_process1() override;
    bool check_violation() override;
};

// CXL-specific: Memory Ordering with CXL.mem
// Tests memory ordering guarantees with CXL.mem transactions
class CXLMemOrderingTest : public LitmusTest {
public:
    using LitmusTest::LitmusTest;
    std::string get_name() const override { return "CXL.mem Ordering"; }
    std::string get_description() const override {
        return "Tests memory ordering with CXL.mem protocol";
    }
    void run_process0() override;
    void run_process1() override;
    bool check_violation() override;
};

#endif // HW_COHERENCY_TESTS_H
