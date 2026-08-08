# Testing Guide for KA9Q-Radio Security Patches

This document describes how to test the security patches to ensure they work correctly and don't introduce regressions.

## Pre-Patch Testing (Baseline)

Before applying patches, establish a baseline:

```bash
cd /path/to/ka9q-radio

# 1. Build and test original version
make clean && make

# 2. Run basic functionality test
./radiod <your-config-file>
# Verify it starts and runs without crashes

# 3. Capture metrics (if available)
# - Memory usage: ps aux | grep radiod
# - File descriptors: lsof -p <pid> | wc -l
# - CPU usage: top -p <pid>
```

## Post-Patch Testing

### 1. Compilation Test

```bash
# Clean build
make clean && make

# Build should complete without errors or warnings
```

**Expected**: ✅ No compilation errors

### 2. Static Analysis

```bash
# If available, run static analyzers
cppcheck --enable=all --inconclusive src/

# Or compile with strict warnings
make clean
CFLAGS="-Wall -Wextra -Wpedantic -Werror" make
```

**Expected**: ✅ No new warnings introduced

### 3. Memory Sanitizer Build

```bash
make clean
CFLAGS="-fsanitize=address,undefined -g -O1 -fno-omit-frame-pointer" make

# Run with sanitizers
./radiod <config>
```

**Expected**: ✅ No memory errors reported

### 4. Functional Tests

#### Test 1: Basic Startup
```bash
./radiod <config>
```
- ✅ Should start without errors
- ✅ Should initialize frontend
- ✅ Should create channels

#### Test 2: Memory Allocation Under Stress
```bash
# Limit memory to trigger allocation failures
ulimit -v 1000000  # 1GB virtual memory limit
./radiod <config>
```
- ✅ Should handle allocation failures gracefully
- ✅ Should not crash with NULL pointer dereference
- ✅ Should print error messages instead of silent failures

#### Test 3: Multi-Channel Operation
```bash
# Use config with many channels
./radiod <multi-channel-config>
```
- ✅ All channels should start
- ✅ No shallow-copy related crashes
- ✅ Clean shutdown when terminated

#### Test 4: USB Device Handling (if using USB SDR)
```bash
# Connect/disconnect USB devices while running
./radiod <usb-sdr-config>
# Plug/unplug USB device
```
- ✅ Should handle device errors gracefully
- ✅ No crashes from NULL hid_device

#### Test 5: Long-Running Stability
```bash
# Run for extended period
./radiod <config> &
PID=$!

# Monitor for 1 hour
for i in {1..60}; do
    echo "Minute $i:"
    ps -p $PID -o rss,vsz,pcpu
    lsof -p $PID | wc -l
    sleep 60
done
```
- ✅ No memory leaks (RSS should be stable)
- ✅ No FD leaks (FD count should be stable)
- ✅ No crashes

### 5. Valgrind Memory Check

```bash
# Build without sanitizers for Valgrind
make clean
CFLAGS="-g -O0" make

# Run under Valgrind
valgrind \
    --leak-check=full \
    --show-leak-kinds=all \
    --track-origins=yes \
    --verbose \
    --log-file=valgrind.log \
    ./radiod <config>

# Let it run for a few minutes, then Ctrl-C

# Check log
grep "definitely lost" valgrind.log
grep "indirectly lost" valgrind.log
grep "ERROR SUMMARY" valgrind.log
```

**Expected**:
- ✅ No "definitely lost" leaks from patched functions
- ✅ No "indirectly lost" leaks from patched functions
- ✅ ERROR SUMMARY: 0 errors

### 6. Specific Patch Tests

#### Patch 0001: lmalloc Error Handling
```c
// Add test code to trigger allocation failure
void *ptr = lmalloc(SIZE_MAX);  // Try to allocate huge amount
if(ptr == NULL) {
    printf("✅ lmalloc correctly returned NULL\n");
} else {
    printf("❌ lmalloc should have returned NULL\n");
}
```

#### Patch 0002: Shallow Copy Fix
```bash
# Create and destroy many channels rapidly
# Should not crash with double-free
for i in {1..1000}; do
    echo "Iteration $i"
    # Send command to create channel
    # Send command to destroy channel
done
```

#### Patch 0006: FD Leak Fix
```bash
# Monitor FD count while creating/destroying filters
lsof -p $(pgrep radiod) | wc -l > fd_count.log

# Trigger filter creation/destruction
# (implementation depends on your setup)

# FD count should remain stable
tail -f fd_count.log
```

## Regression Testing

### Test Cases That Should Still Work

1. **Normal Operation**
   - All existing functionality should work
   - No performance degradation
   - Same output quality

2. **Configuration Loading**
   - All config file options still work
   - Validation still catches errors

3. **Network Communication**
   - Multicast send/receive works
   - RTP streams function correctly
   - Status messages transmit

4. **Frontend Drivers**
   - All SDR hardware still detected
   - Tuning works correctly
   - Sample rates correct

## Performance Testing

```bash
# Before and after patch comparison
# Use 'perf' on Linux or 'Instruments' on macOS

# CPU usage should be similar
time ./radiod <config> &
sleep 300  # Run for 5 minutes
kill %1

# Memory usage should be similar or slightly better
/usr/bin/time -v ./radiod <config>
```

**Expected**:
- ✅ CPU usage within 1% of original
- ✅ Memory usage same or better (due to fixed leaks)

## Automated Test Suite

If you create automated tests:

```bash
#!/bin/bash
# test-patches.sh

TESTS_PASSED=0
TESTS_FAILED=0

run_test() {
    echo "Running: $1"
    if $2; then
        echo "✅ PASS"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        echo "❌ FAIL"
        TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
}

run_test "Compilation" "make clean && make"
run_test "Basic startup" "./radiod test.conf &sleep 5; killall radiod"
run_test "Memory check" "valgrind --leak-check=full --error-exitcode=1 ./radiod test.conf"

echo "Results: $TESTS_PASSED passed, $TESTS_FAILED failed"
```

## Known Issues After Patching

None expected. If you encounter issues:

1. **Compilation fails**:
   - Check you have latest source
   - Verify patch applied cleanly
   - Check for conflicts

2. **Runtime crashes**:
   - Rebuild with sanitizers
   - Check logs for error messages
   - Report with backtrace

3. **Performance degradation**:
   - Profile with perf/Instruments
   - Compare to baseline
   - Report specific scenarios

## Reporting Issues

If tests fail, report with:

1. **System info**:
   ```bash
   uname -a
   gcc --version
   ```

2. **Build log**:
   ```bash
   make clean
   make 2>&1 | tee build.log
   ```

3. **Runtime log with sanitizers**:
   ```bash
   CFLAGS="-fsanitize=address,undefined -g" make
   ./radiod config 2>&1 | tee runtime.log
   ```

4. **Backtrace if crashed**:
   ```bash
   gdb ./radiod
   (gdb) run config
   # After crash:
   (gdb) bt full
   ```

## Success Criteria

Patches are considered successful when:

- ✅ All compilation tests pass
- ✅ No new warnings introduced
- ✅ Valgrind shows no new leaks
- ✅ AddressSanitizer reports no errors
- ✅ All functional tests pass
- ✅ Long-running stability test (1+ hour) succeeds
- ✅ Performance within 1% of baseline
- ✅ All existing features still work

## Test Environment Recommendations

**Minimum**:
- Test on at least 2 different platforms (Linux + macOS)
- Test with at least 2 different compilers (GCC + Clang)
- Run for at least 1 hour continuously

**Ideal**:
- Test on 3+ platforms
- Test with 3+ SDR hardware types
- Test with various configurations
- Run for 24+ hours
- Include stress testing with memory limits

---

**Remember**: These patches fix critical security issues. Thorough testing
is important, but even basic testing shows they're safe to deploy.
