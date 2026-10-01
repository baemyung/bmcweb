// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
//
// Proves the difference between:
//
//   UNSAFE:  std::bind_front(&Self::cb, this, shared_from_this())
//   SAFE:    std::bind_front(&Self::cb, shared_from_this())
//
// --- The problem ---
//
// std::bind_front stores its first argument as the "call object":
//
//   std::bind_front(&Self::cb, X, ...)
//     → on invocation calls (X.*&Self::cb)(...)
//
// When X is `this` (a raw pointer) the functor calls the method through an
// unowned pointer.  The extra `shared_from_this()` argument is stored as the
// *first method parameter* — it keeps the object alive in memory, but the
// mechanism for the actual call is the raw pointer.
//
// When X is `shared_from_this()` (a shared_ptr) the functor dereferences the
// shared_ptr to reach the object.  The shared_ptr IS the call target; it owns
// the object for the lifetime of the functor.
//
// The unsafe pattern works by accident as long as the functor — and thus its
// bound shared_ptr copy — outlives every call.  But:
//
//  • The raw `this` carries no ownership: it can be copied/stored without
//    extending lifetime.
//  • Moving the functor can leave the raw pointer dangling if the shared_ptr
//    copy is not moved together with it (e.g. stored in a separate member).
//  • Every callback method that uses this pattern must carry a spurious
//    leading `const shared_ptr<Self>&` parameter solely to extend lifetime —
//    responsibility that belongs to the call-target slot of bind_front.
//
// --- Tests ---
//
//  1. SafePattern   — bind_front built with shared_from_this() as call target.
//  2. UnsafePattern — bind_front built with `this` as call target and
//                     shared_from_this() as param.
//  3. UnsafePattern_RawPtrOutlivesSharedPtr
//                   — shows that the raw `this` becomes dangling the moment
//                     the functor (the sole shared_ptr holder) is destroyed,
//                     while the safe functor would prevent that destruction.
//  4. UnsafePattern_DoubleCompletion
//                   — models the production crash observed in http_client.hpp:
//                     two queued functors (simulating two async completions on
//                     the same stream) both target the same raw `this` and
//                     share mutable state.  The first invocation modifies that
//                     state (nulls the callback); the second invocation then
//                     calls the now-null callback → std::bad_function_call.
//                     Because both functors go through raw `this` rather than
//                     a single shared_ptr call target, there is no ownership
//                     token that would serialise or prevent the second call.

#include <functional>
#include <memory>

#include <gtest/gtest.h>

namespace
{

// ============================================================================
// Subject class – models the bmcweb ConnectionInfo / websocket async pattern.
// ============================================================================

struct AsyncWorker : std::enable_shared_from_this<AsyncWorker>
{
    // Tracks whether the destructor has been called.
    bool& destroyed;
    int result = 0;

    // Models ConnectionInfo::callback — mutable shared state accessed by both
    // the async completion handler and by the pool-management path (sendNext).
    std::function<void()> callback;

    // Set to true by unsafeCompletion/safeCompletion when callback is null.
    // Lets tests assert on the null-callback condition without crashing.
    bool callbackWasNull = false;

    explicit AsyncWorker(bool& destroyedFlag) : destroyed(destroyedFlag) {}
    AsyncWorker(const AsyncWorker&) = delete;
    AsyncWorker& operator=(const AsyncWorker&) = delete;
    AsyncWorker(AsyncWorker&&) = delete;
    AsyncWorker& operator=(AsyncWorker&&) = delete;
    ~AsyncWorker()
    {
        destroyed = true;
    }

    // -----------------------------------------------------------------------
    // Safe callback: no leading shared_ptr parameter.
    // Paired with:  std::bind_front(&AsyncWorker::safeCallback,
    //                               shared_from_this())
    // -----------------------------------------------------------------------
    void safeCallback(int value)
    {
        result = value * 2;
    }

    // -----------------------------------------------------------------------
    // Unsafe callback: leading shared_ptr parameter solely to extend lifetime.
    // Paired with:  std::bind_front(&AsyncWorker::unsafeCallback,
    //                               this, shared_from_this())
    // -----------------------------------------------------------------------
    void unsafeCallback(const std::shared_ptr<AsyncWorker>& /*self*/, int value)
    {
        result = value * 2;
    }

    // -----------------------------------------------------------------------
    // Models ConnectionInfo::afterRead — unsafe variant.
    // `this` is the raw call target; the leading shared_ptr is the spurious
    // lifetime-extension parameter.
    // Paired with:
    //   std::bind_front(&AsyncWorker::unsafeCompletion, this,
    //   shared_from_this())
    //
    // Mirrors the production fix (commit 1b196a8): guards with `if (callback)`
    // so a null callback does not crash.  Records the null condition in
    // callbackWasNull so tests can assert on it.
    // -----------------------------------------------------------------------
    void unsafeCompletion(const std::shared_ptr<AsyncWorker>& /*self*/)
    {
        if (callback)
        {
            callback();
        }
        else
        {
            callbackWasNull = true;
        }
    }

    // -----------------------------------------------------------------------
    // Models ConnectionInfo::afterRead — safe variant.
    // shared_from_this() is the call target; no leading shared_ptr parameter.
    // Paired with:
    //   std::bind_front(&AsyncWorker::safeCompletion, shared_from_this())
    // -----------------------------------------------------------------------
    void safeCompletion()
    {
        if (callback)
        {
            callback();
        }
        else
        {
            callbackWasNull = true;
        }
    }
};

// ============================================================================
// Test 1 — Safe pattern: shared_ptr as call target
// ============================================================================
//
// bind_front stores shared_from_this() in the call-target slot.  Dropping the
// external shared_ptr leaves the functor as the sole owner; the object stays
// alive and the callback fires correctly.
//
TEST(BindFrontThisPattern, SafePattern_SharedPtrIsCallTarget)
{
    bool destroyed = false;

    std::function<void(int)> fn;
    {
        auto obj = std::make_shared<AsyncWorker>(destroyed);

        // shared_from_this() occupies the call-target slot of bind_front.
        // Equivalent to the lambda: [self = shared_from_this()](int v)
        //                           { self->safeCallback(v); }
        fn = std::bind_front(&AsyncWorker::safeCallback,
                             obj->shared_from_this());

        // Drop the only external reference.
        obj.reset();

        // The functor's shared_ptr copy is now the SOLE owner.
        EXPECT_FALSE(destroyed)
            << "Object must still be alive; functor's shared_ptr holds it";
    }

    // Call the functor — object is alive because the shared_ptr inside the
    // functor owns it.
    fn(21);

    // Destroy the functor — shared_ptr refcount → 0 → object destroyed.
    fn = nullptr;
    EXPECT_TRUE(destroyed)
        << "Object must be destroyed after the functor (sole owner) is gone";
}

// ============================================================================
// Test 2 — Unsafe pattern: raw `this` as call target, shared_ptr as parameter
// ============================================================================
//
// bind_front stores `this` in the call-target slot and shared_from_this() as
// the first bound argument (passed as the first method parameter).  The object
// is kept alive by the bound shared_ptr — but only for as long as the functor
// lives.
//
// This works by accident: the bound shared_ptr happens to be the last ref,
// so `this` remains valid while the functor exists.  The danger lies in the
// indirection: the raw pointer and its "guardian" shared_ptr are two separate
// values inside the functor with no enforced coupling.
//
TEST(BindFrontThisPattern, UnsafePattern_RawThisIsCallTarget)
{
    bool destroyed = false;

    std::function<void(int)> fn;
    {
        auto obj = std::make_shared<AsyncWorker>(destroyed);

        // `this` occupies the call-target slot; invocation does:
        //   (this->*&AsyncWorker::unsafeCallback)(shared_ptr_copy, value)
        fn = std::bind_front(
            &AsyncWorker::unsafeCallback,
            obj.get(),                // raw pointer — no ownership
            obj->shared_from_this()); // kept as method parameter

        // Drop the only external reference.
        obj.reset();

        // Object is still alive — but ONLY because the functor holds the
        // shared_ptr that was bound as a method parameter.
        EXPECT_FALSE(destroyed)
            << "Object survives only because the bound shared_ptr copy holds it";
    }

    // Call while functor is alive — works because the bound shared_ptr
    // (method parameter) still holds the object.
    fn(21);

    // Destroy the functor — the shared_ptr bound as a parameter drops → 0 refs.
    fn = nullptr;
    EXPECT_TRUE(destroyed)
        << "Object destroyed when functor's bound shared_ptr parameter drops";

    // KEY DIFFERENCE vs safe pattern:
    //   - Safe:   shared_ptr IS the call target → dereference is
    //   ownership-safe.
    //   - Unsafe: raw `this` IS the call target → validity depends on an
    //             unrelated shared_ptr stored elsewhere in the same functor.
    //             No compile-time enforcement of this coupling.
}

// ============================================================================
// Test 3 — Unsafe pattern: raw `this` becomes dangling when functor is gone
// ============================================================================
//
// Demonstrates the lifetime hazard directly: if the functor is destroyed while
// a raw copy of `this` is still held outside it, that pointer is dangling.
//
// With the safe pattern the object cannot be destroyed while the functor
// exists (the shared_ptr prevents it), so a pointer derived from the
// functor's call target can never outlive the object it belongs to.
//
TEST(BindFrontThisPattern, UnsafePattern_RawPtrOutlivesSharedPtr)
{
    bool destroyed = false;
    AsyncWorker* rawPtr = nullptr; // raw pointer captured outside

    {
        auto obj = std::make_shared<AsyncWorker>(destroyed);
        rawPtr = obj.get(); // store raw pointer independently — unsafe

        {
            // Build the unsafe functor inside a tighter scope.
            std::function<void(int)> fn =
                std::bind_front(&AsyncWorker::unsafeCallback, obj.get(),
                                obj->shared_from_this());

            // Drop the external shared_ptr.  Object alive only via functor.
            obj.reset();
            EXPECT_FALSE(destroyed);

            // fn is destroyed here — its bound shared_ptr is the last ref.
        }
        // After fn's scope: shared_ptr in fn drops → object is destroyed.
        EXPECT_TRUE(destroyed)
            << "Functor gone → bound shared_ptr dropped → object destroyed";
    }

    // rawPtr now points to freed memory.  Accessing it is undefined behaviour.
    // The test proves the invariant: after the unsafe functor is destroyed,
    // any raw `this` pointer stored outside the functor is dangling.
    // With the SAFE pattern, only the functor holds the call target as a
    // shared_ptr, so no raw pointer can outlive it without explicit extraction.
    EXPECT_TRUE(destroyed); // confirms rawPtr is dangling — do NOT dereference
    (void)rawPtr;           // suppress unused-variable warning
}

// ============================================================================
// Test 4 — Callback signature reveals the pattern in use
// ============================================================================
//
// The unsafe pattern forces every callback to carry a spurious leading
// `const shared_ptr<Self>&` parameter whose only purpose is lifetime
// extension — a responsibility that belongs to the call-target slot of
// bind_front.  The safe pattern requires no such parameter.
//
TEST(BindFrontThisPattern, CallbackSignature_SafeHasNoLeadingSharedPtr)
{
    bool destroyed = false;
    auto obj = std::make_shared<AsyncWorker>(destroyed);

    // Safe: callback signature is void(int) — no shared_ptr parameter.
    std::function<void(int)> safeFn =
        std::bind_front(&AsyncWorker::safeCallback, obj->shared_from_this());

    // Unsafe: callback method has signature void(const shared_ptr<Self>&, int).
    // bind_front pre-binds the shared_ptr so the resulting std::function
    // signature is still void(int) — but the underlying method is burdened
    // with a parameter it only uses to keep itself alive.
    std::function<void(int)> unsafeFn = std::bind_front(
        &AsyncWorker::unsafeCallback, obj.get(), obj->shared_from_this());

    safeFn(10);
    EXPECT_EQ(obj->result, 20);

    unsafeFn(15);
    EXPECT_EQ(obj->result, 30);
}

// ============================================================================
// Test 5 — Unsafe pattern: double-completion corrupts shared mutable state
// ============================================================================
//
// Models the production crash observed in http_client.hpp (commit 1b196a8):
//
//   recvMessage() posts:
//     async_read(..., bind_front(&afterRead, this, shared_from_this()))
//
// Under certain SSL error conditions the underlying stream delivers TWO
// completion events for that single async_read (e.g. asio.ssl error followed
// immediately by stream_truncated).  In single-threaded Boost.Asio these two
// completions are queued and dispatched sequentially — one after the other —
// each invoking afterRead through the same raw `this`.
//
// The first invocation succeeds and triggers sendNext(), which sets
//   conn->callback = nullptr
// to allow the response handler (which may hold a shared_ptr to an AsyncResp)
// to be released.
//
// The second invocation then reaches:
//   callback(parser->keep_alive(), connId, res)
// where callback is now null.  In production this throws std::bad_function_call
// (or SIGSEGV depending on platform/sanitizer settings) → process terminates.
//
// The production fix (commit 1b196a8) adds `if (callback)` before every call
// site — mirrored here in unsafeCompletion so the test does not crash.
// The test instead asserts that the second completion observes a null callback,
// which is the invariant the production fix relies on.
//
// The unsafe pattern is the enabler: because `this` is the raw call target,
// there is no single ownership token representing "the active async operation".
// Two independent functors can both be queued against the same object with no
// compile-time or run-time guard preventing the second from observing state
// already mutated by the first.
//
// The safe pattern does NOT prevent the double-completion either — two
// shared_ptr-targeted functors would still both fire.  But it removes the
// spurious `const shared_ptr<Self>&` parameter that gives a false sense of
// ownership, making the real hazard (shared mutable state with no serialisation
// guard) easier to see and reason about.
//
TEST(BindFrontThisPattern, UnsafePattern_DoubleCompletion_NullsCallback)
{
    bool destroyed = false;
    auto obj = std::make_shared<AsyncWorker>(destroyed);

    // Keep a non-owning observer and a weak_ptr before dropping the
    // external shared_ptr, so we can inspect the object afterwards.
    AsyncWorker* raw = obj.get();
    std::weak_ptr<AsyncWorker> weak = obj;

    // Set up the shared mutable state — models ConnectionInfo::callback.
    // The first completion should invoke it; the second should find it null.
    // Capture weak rather than obj so the lambda does not dangle after
    // obj.reset() below.
    int callCount = 0;
    obj->callback = [&callCount, weak]() {
        callCount++;
        // Models sendNext(): null out the callback after the first successful
        // completion so the response handler (AsyncResp) can be released.
        if (auto self = weak.lock())
        {
            self->callback = nullptr;
        }
    };

    // Simulate two async completion events queued for the same async_read,
    // each carrying its own functor built with the unsafe pattern.
    // In production these arrive as: ssl_error → stream_truncated.
    std::function<void()> completion1 = std::bind_front(
        &AsyncWorker::unsafeCompletion, obj.get(), obj->shared_from_this());
    std::function<void()> completion2 = std::bind_front(
        &AsyncWorker::unsafeCompletion, obj.get(), obj->shared_from_this());

    // Drop the external reference — object stays alive via the two bound
    // shared_ptr copies inside the functors, exactly as in production where
    // the ConnectionPool holds the shared_ptr but the async op also holds one.
    obj.reset();
    EXPECT_FALSE(destroyed) << "Object must survive: two functors hold it";

    // First completion fires — callback is invoked and then nulled.
    completion1();
    EXPECT_EQ(callCount, 1) << "First completion must invoke callback once";
    EXPECT_FALSE(raw->callbackWasNull)
        << "First completion must have found callback non-null";

    // Second completion fires — callback is now null.
    // In production this is the crash point (std::bad_function_call / SIGSEGV).
    // The `if (callback)` guard in unsafeCompletion — mirroring commit 1b196a8
    // — prevents the crash here; callbackWasNull is set instead.
    completion2();
    EXPECT_EQ(callCount, 1)
        << "Second completion must NOT invoke callback again";
    EXPECT_TRUE(raw->callbackWasNull)
        << "Second completion must have observed a null callback";
}

// ============================================================================
// Test 6 — Safe pattern: double-completion with shared mutable state
// ============================================================================
//
// Demonstrates that switching to the safe pattern alone does NOT prevent the
// double-completion from reaching the null callback — the same null-callback
// condition occurs.
//
// The safe pattern removes the misleading `const shared_ptr<Self>&` parameter
// and makes ownership explicit, but the real fix for double-completion is a
// guard at the call site (e.g. `if (callback) { callback(...); }`).
//
// This test documents that truth: safe functor construction and null-checking
// the callback are two independent, complementary fixes.
//
TEST(BindFrontThisPattern, SafePattern_DoubleCompletion_StillNullsCallback)
{
    bool destroyed = false;
    auto obj = std::make_shared<AsyncWorker>(destroyed);

    AsyncWorker* raw = obj.get();
    std::weak_ptr<AsyncWorker> weak = obj;

    int callCount = 0;
    obj->callback = [&callCount, weak]() {
        callCount++;
        // Same sendNext()-style null as in Test 5.
        if (auto self = weak.lock())
        {
            self->callback = nullptr;
        }
    };

    // Two functors built with the SAFE pattern.
    std::function<void()> completion1 =
        std::bind_front(&AsyncWorker::safeCompletion, obj->shared_from_this());
    std::function<void()> completion2 =
        std::bind_front(&AsyncWorker::safeCompletion, obj->shared_from_this());

    obj.reset();
    EXPECT_FALSE(destroyed) << "Object must survive: two functors hold it";

    // First completion — callback fires and is nulled.
    completion1();
    EXPECT_EQ(callCount, 1);
    EXPECT_FALSE(raw->callbackWasNull)
        << "First completion must have found callback non-null";

    // Second completion — callback is null; the `if (callback)` guard skips
    // the call.  Safe pattern and unsafe pattern behave identically here:
    // both require the explicit null-check; neither prevents the double-fire.
    completion2();
    EXPECT_EQ(callCount, 1)
        << "Second completion must NOT invoke callback again; "
           "safe functor alone does not guard against double-completion";
    EXPECT_TRUE(raw->callbackWasNull)
        << "Second completion must have observed a null callback";
}

} // anonymous namespace
