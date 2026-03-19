#!/bin/bash
set -e

echo "Running tests..."

# Ensure we're running from the root of the project
cd "$(dirname "$0")/.."

export EVENT_NOEPOLL=1

# Create a sample input
echo -e "apple\nbanana\napple\norange\nbanana\napple" > tests/data/test_input.txt

# --- Test 1: grep ---
# Expected grep output: 3 lines of 'apple'
./mapred -m "grep apple" -c 2 tests/data/test_input.txt > tests/data/test_result_grep.txt

# Sort output to avoid non-deterministic ordering
sort tests/data/test_result_grep.txt > tests/data/test_result_grep_sorted.txt

cat << 'INNER_EOF' > tests/data/expected_grep.txt
apple
apple
apple
INNER_EOF

if diff -q tests/data/test_result_grep_sorted.txt tests/data/expected_grep.txt; then
    echo "Grep Test passed!"
else
    echo "Grep Test failed!"
    exit 1
fi

# --- Test 2: wc ---
./mapred -m "wc -l" -c 2 tests/data/test_input.txt > tests/data/test_result_wc.txt

# Sum the results from the child processes
awk '{s+=$1} END {print s}' tests/data/test_result_wc.txt > tests/data/test_result_wc_sum.txt

echo 6 > tests/data/expected_wc.txt

if diff -q tests/data/test_result_wc_sum.txt tests/data/expected_wc.txt; then
    echo "Word count test passed!"
else
    echo "Word count test failed!"
    exit 1
fi

echo "All tests completed successfully!"
