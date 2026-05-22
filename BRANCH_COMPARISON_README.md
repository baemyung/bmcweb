# HTTP Client Coredump Test Branches - Upstream bmcweb

This document describes two branches created for the upstream bmcweb repository to demonstrate the HTTP client coredump vulnerability and its fix.

## Repository

**GitHub:** https://github.com/baemyung/bmcweb.git  
**Base Branch:** master

## Branch Overview

### 1. `test-reproduction-client-coredump-caused`
**Purpose:** Demonstrates the vulnerable code that causes coredumps

**Key Characteristics:**
- Contains the aggressive HTTP client test program
- Uses the **ORIGINAL** vulnerable `http/http_client.hpp` (without defensive fixes)
- **Expected Behavior:** Will crash with SIGABRT/SIGSEGV when run

**Latest Commit:** 8325e0d3 (Add aggressive HTTP client crash test)

### 2. `test-reproduction-client-coredump-fixed`
**Purpose:** Demonstrates the fixed code that prevents coredumps

**Key Characteristics:**
- Contains the aggressive HTTP client test program
- Uses the **FIXED** `http/http_client.hpp` with defensive checks
- **Expected Behavior:** Completes successfully without crashes

**Latest Commit:** d92f20d9 (Fix HTTP client coredump vulnerability with defensive checks)

## The HTTP Client Vulnerability

The upstream bmcweb master branch has the same vulnerability as the IBM branches:

### Vulnerable Code Patterns:

1. **In `sendNext()` function (~line 720):**
```cpp
void sendNext(bool keepAlive, uint32_t connId)
{
    auto conn = connections[connId];  // No bounds checking!
    // ...
}
```

2. **In `afterSendData()` function (~line 839):**
```cpp
static void afterSendData(const std::weak_ptr<ConnectionPool>& weakSelf,
                          const std::function<void(Response&)>& resHandler,
                          bool keepAlive, uint32_t connId, Response& res)
{
    resHandler(res);  // Called BEFORE validation!
    
    std::shared_ptr<ConnectionPool> self = weakSelf.lock();
    if (!self) {
        // Too late - callback already executed
        return;
    }
}
```

## The Fix

### Defensive Checks Added (in -fixed branch only):

1. **Bounds checking in `sendNext()`:**
```cpp
void sendNext(bool keepAlive, uint32_t connId)
{
    // Defensive check: validate connId is within bounds
    if (connId >= connections.size())
    {
        BMCWEB_LOG_ERROR("Invalid connId: {} (size: {})", connId,
                         connections.size());
        return;
    }

    auto conn = connections[connId];

    // Defensive check: validate connection pointer is not null
    if (!conn)
    {
        BMCWEB_LOG_ERROR("Connection at index {} is null", connId);
        return;
    }
    // ... rest of function
}
```

2. **Reordered validation in `afterSendData()`:**
```cpp
static void afterSendData(const std::weak_ptr<ConnectionPool>& weakSelf,
                          const std::function<void(Response&)>& resHandler,
                          bool keepAlive, uint32_t connId, Response& res)
{
    // Defensive check: validate ConnectionPool is still alive BEFORE
    // executing callback
    std::shared_ptr<ConnectionPool> self = weakSelf.lock();
    if (!self)
    {
        BMCWEB_LOG_CRITICAL("Failed to capture connection");
        return;
    }

    // Now safe to execute callback
    resHandler(res);
    // ... rest of function
}
```

## Testing Instructions

### Build and Test:

#### Test the VULNERABLE branch (should crash):
```bash
cd /path/to/bmcweb
git fetch origin
git checkout test-reproduction-client-coredump-caused

# Build
meson setup build
ninja -C build

# Run test
./build/http_client_aggressive_test

# Expected: SIGABRT or SIGSEGV crashes
```

#### Test the FIXED branch (should complete successfully):
```bash
cd /path/to/bmcweb
git fetch origin
git checkout test-reproduction-client-coredump-fixed

# Build
meson setup build
ninja -C build

# Run test
./build/http_client_aggressive_test

# Expected: Clean completion with "created 1000, destroyed 1000 clients"
```

## Commit History Comparison

### Branch: test-reproduction-client-coredump-caused
```
8325e0d3 Add aggressive HTTP client crash test (vulnerable version)
df2c4aac Move to Redfish 2026.1
[... upstream commits ...]
```

### Branch: test-reproduction-client-coredump-fixed
```
d92f20d9 Fix HTTP client coredump vulnerability with defensive checks  <-- KEY FIX
8325e0d3 Add aggressive HTTP client crash test (vulnerable version)
df2c4aac Move to Redfish 2026.1
[... upstream commits ...]
```

## Key Files

Both branches contain:
- `src/http_client_aggressive_test.cpp` - The aggressive test program
- `meson.build` - Build configuration for the test

Only the `-fixed` branch has the defensive fixes in:
- `http/http_client.hpp` - HTTP client implementation with defensive checks

## Relationship to IBM Branches

This vulnerability exists in:
- **Upstream bmcweb** (master branch) - Demonstrated by these branches
- **IBM bmcweb 1120** - Fixed in branches `1120-test-reproduction-client-coredump-caused/fixed`
- **IBM bmcweb 1110** - Same vulnerability exists

The fix is identical across all versions, making it suitable for:
- Upstream contribution
- Backporting to stable branches
- IBM internal branches

## Summary

These two branches provide a clear before/after comparison for the upstream bmcweb repository:
- **-caused**: Reproduces the vulnerability (crashes expected)
- **-fixed**: Demonstrates the fix (clean completion expected)

The same aggressive test program is used in both branches, making it easy to verify that the HTTP client fixes resolve the coredump issue in the upstream codebase.