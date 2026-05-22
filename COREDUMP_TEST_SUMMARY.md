# HTTP Client Coredump Unit Test - Summary

## What Was Created

A comprehensive unit test suite to reproduce the client coredump issue in the
bmcweb HTTP client.

### Files Created

1. **`test/http/http_client_coredump_test.cpp`** (308 lines)
   - Complete GTest-based unit test suite
   - 5 different test cases targeting various aspects of the bug
   - Uses Google Test framework for proper test structure

2. **`test/http/HTTP_CLIENT_COREDUMP_TEST_README.md`** (130 lines)
   - Comprehensive documentation of the test suite
   - Explains the bug, test cases, and expected results
   - Includes instructions for building and running tests

### Files Modified

1. **`meson.build`**
   - Added `test/http/http_client_coredump_test.cpp` to the `srcfiles_unittest`
     list
   - Test will now be built and run as part of the standard test suite

## The Bug Being Tested

The coredump occurs due to a **use-after-free** bug in
[`ConnectionInfo`](http/http_client.hpp:128):

### Root Cause

When `ConnectionInfo` starts async operations (DNS resolve, TCP connect, SSL
handshake, HTTP read/write), it uses raw `this` pointers in callbacks via
`std::bind_front()`. If the `ConnectionInfo` shared_ptr is destroyed while these
async operations are pending, the callbacks fire with a dangling `this` pointer,
causing:

- Segmentation faults
- `std::bad_function_call` exceptions
- Heap-use-after-free (detected by AddressSanitizer)

### Vulnerable Code Locations

All async callbacks in [`http/http_client.hpp`](http/http_client.hpp):

- **Line 169-170**: `afterResolve` - DNS resolution callback
- **Line 194-195**: `afterConnect` - TCP connection callback
- **Line 242-243**: `afterSslHandshake` - SSL handshake callback
- **Line 282-283**: `afterWrite` - HTTP write callback
- **Line 335-336**: `afterRead` - HTTP read callback
- **Line 413**: User callback invocation accessing `connPolicy` member

## Test Strategy

The test suite uses 5 different approaches to trigger the race condition:

### Test 1: DestroyDuringAsyncResolve

- **Target**: DNS resolution phase
- **Method**: Destroy `ConnectionInfo` immediately after starting
  `async_resolve()`
- **Why effective**: DNS resolution has the longest window for the race
  condition

### Test 2: DestroyDuringConnect

- **Target**: TCP connection phase
- **Method**: Destroy during connection to a closed port
- **Why effective**: Connection timeout creates delay for destruction timing

### Test 3: RapidCreateDestroy

- **Target**: All phases
- **Method**: Rapidly create and destroy 10 connections
- **Why effective**: Increases probability of hitting exact timing window

### Test 4: DestroyWithMemberAccessInCallback

- **Target**: Member variable access in callbacks
- **Method**: Callback that accesses response object
- **Why effective**: Tests the specific code path at line 390
  (`connPolicy->invalidResp()`)

### Test 5: MultipleConnectionsStressTest

- **Target**: Concurrent operations
- **Method**: 5 concurrent connections destroyed simultaneously
- **Why effective**: Multiple operations increase crash probability

## How to Use

### Building the Test

```bash
# Standard build
meson setup build
cd build
ninja http_client_coredump_test

# With sanitizers (RECOMMENDED)
meson setup build -Db_sanitize=address,undefined
cd build
ninja http_client_coredump_test
```

### Running the Test

```bash
# Run the test
./http_client_coredump_test

# Run with verbose output
./http_client_coredump_test --gtest_verbose

# Run specific test
./http_client_coredump_test --gtest_filter=HttpClientCoredumpTest.DestroyDuringAsyncResolve
```

### Expected Results

#### With Unfixed Code (Current State)

- **Likely outcome**: Segmentation fault or crash
- **AddressSanitizer**: Will report heap-use-after-free
- **Behavior**: May be intermittent due to race condition timing
- **Error messages**: May see `std::bad_function_call` exceptions

#### With Fixed Code (After Applying Fix)

- **Outcome**: All tests pass
- **AddressSanitizer**: No errors
- **Behavior**: Consistent, no crashes
- **Callbacks**: Either don't fire (if destroyed) or fire safely

## The Fix (For Reference)

The fix requires changing all async callbacks to use `weak_from_this()`:

```cpp
// BEFORE (BUGGY):
resolver.async_resolve(host, port,
    std::bind_front(&ConnectionInfo::afterResolve, this, shared_from_this()));

// AFTER (FIXED):
resolver.async_resolve(host, port,
    std::bind_front(&ConnectionInfo::afterResolve, weak_from_this(), shared_from_this()));

// Callback signature changes:
void afterResolve(const std::weak_ptr<ConnectionInfo>& weakSelf,
                  const std::shared_ptr<ConnectionInfo>& /*self*/,
                  const boost::system::error_code& ec,
                  const Resolver::results_type& endpointList)
{
    std::shared_ptr<ConnectionInfo> self = weakSelf.lock();
    if (!self) {
        return; // Object destroyed, safely exit
    }
    // Now safe to access members through 'self'
}
```

This pattern must be applied to:

- `afterResolve()`
- `afterConnect()`
- `afterSslHandshake()`
- `afterWrite()`
- `afterRead()`
- `afterSslShutdown()`
- `onTimerDone()`

## Integration with CI/CD

The test is integrated into the meson build system and will:

- Be built automatically when tests are enabled
- Run as part of `ninja test` or `meson test`
- Be included in CI/CD pipelines that run the test suite
- Work with sanitizers for enhanced bug detection

## Verification

To verify the test is properly integrated:

```bash
# List all tests
meson test --list

# Should show:
# http_client_coredump_test

# Run just this test
meson test http_client_coredump_test
```

## Related Documentation

- [`test/http/HTTP_CLIENT_COREDUMP_TEST_README.md`](test/http/HTTP_CLIENT_COREDUMP_TEST_README.md) -
  Detailed test documentation
- [`http/http_client.hpp`](http/http_client.hpp) - HTTP client implementation
  with the bug
- [`src/http_client_connectioninfo_destruction_test.cpp`](src/http_client_connectioninfo_destruction_test.cpp) -
  Standalone test variant

## Branch Information

- **Branch**: `1110-test-reproduction-client-coredump-caused`
- **Purpose**: Reproduce and document the client coredump issue
- **Status**: Test created, ready for validation

## Next Steps

1. Build the test with sanitizers enabled
2. Run the test to confirm it reproduces the coredump
3. Apply the fix (weak_ptr pattern) to http_client.hpp
4. Re-run the test to verify the fix
5. Commit both the test and the fix together
