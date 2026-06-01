# HTTP Client Coredump Test Status

## Current Status: ✅ TEST WORKING CORRECTLY

The test is **successfully reproducing the bug** as intended.

## Test Results

### With Current Code (Unfixed)

```text
4/65 bmcweb:http_client_coredump_test    FAIL    0.25s   killed by signal 11 SIGSEGV
[ RUN      ] HttpClientCoredumpTest.DestroyClientDuringAsyncResolve
```

**Result**: SIGSEGV (Segmentation Fault) ✅ **EXPECTED - Bug detected!**

## Why The Test Crashes

The test crashes because the bug still exists in the current code:

**File**: `http/http_client.hpp`  
**Line 178**: Still uses raw `this` pointer:

```cpp
resolver.async_resolve(
    host.encoded_host_address(), host.port(),
    std::bind_front(&ConnectionInfo::afterResolve, this,  // ← BUG: raw 'this'
                    shared_from_this()));
```

## The Fix (Not Yet Applied)

The fix from <https://gerrit.openbmc.org/c/openbmc/bmcweb/+/90541> needs to be
applied.

It should change line 178 to use `weak_from_this()`:

```cpp
resolver.async_resolve(
    host.encoded_host_address(), host.port(),
    std::bind_front(&ConnectionInfo::afterResolve, weak_from_this(),  // ← FIXED
                    shared_from_this()));
```

And update the callback signature:

```cpp
void afterResolve(const std::weak_ptr<ConnectionInfo>& weakSelf,
                  const std::shared_ptr<ConnectionInfo>& /*self*/,
                  const boost::system::error_code& ec,
                  const Resolver::results_type& endpointList)
{
    std::shared_ptr<ConnectionInfo> self = weakSelf.lock();
    if (!self) {
        return;  // Object destroyed, safely exit
    }
    // ... rest of implementation
}
```

## Next Steps

1. **Apply the fix** from the Gerrit review to `http/http_client.hpp`
2. **Re-run the test** - it should pass all 5 test cases
3. **Verify** no SIGSEGV occurs

## Test File

**Location**: `test/http/http_client_coredump_test.cpp`  
**Branch**: `master-test-reproduction-client-coredump-caused`  
**Commits**:

- 969d1ece - Initial test
- 4c03e6b8 - Fixed to use HttpClient API
- 3aa42f8e - Optimized to prevent timeout

## Conclusion

The test is **working perfectly**! It:

- ✅ Compiles successfully
- ✅ Detects the bug (crashes with SIGSEGV)
- ✅ Completes quickly (0.25s)
- ✅ Ready to verify the fix

Once the fix is applied, the test should pass without crashes.
