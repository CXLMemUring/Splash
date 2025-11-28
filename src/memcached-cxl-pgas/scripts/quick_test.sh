#!/bin/bash
# Quick two-node test with proper timing
cd /root/splash/src/memcached-cxl-pgas/build

# Kill any existing processes and wait
sudo pkill -9 -f pgas_two_node_test 2>/dev/null || true
sleep 3  # Wait longer for ports to be released

echo "=== Starting Two-Node Test ==="
echo "Starting Node 0 (will wait for Node 1 to connect)..."
sudo ./pgas_two_node_test -c ../config/node0.conf -i 100 > /tmp/node0.log 2>&1 &
PID0=$!

# Wait 1 second for Node 0 to start listening
sleep 1

echo "Starting Node 1 (will connect to Node 0)..."
sudo ./pgas_two_node_test -c ../config/node1.conf -i 100 > /tmp/node1.log 2>&1 &
PID1=$!

echo "Node 0 PID: $PID0"
echo "Node 1 PID: $PID1"
echo ""
echo "Waiting for tests to complete (max 60 seconds)..."

# Wait for both processes with timeout
timeout 60 bash -c "wait $PID0 2>/dev/null; wait $PID1 2>/dev/null" || {
    echo "Timeout! Killing processes..."
    kill $PID0 $PID1 2>/dev/null
}

echo ""
echo "========================================"
echo "  NODE 0 OUTPUT"
echo "========================================"
cat /tmp/node0.log

echo ""
echo "========================================"
echo "  NODE 1 OUTPUT"
echo "========================================"
cat /tmp/node1.log
