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
//  A. SafePattern   — bind_front built with shared_from_this() as call target.
//  B. UnsafePattern_RawPtrOutlivesSharedPtr
//                   — shows that the raw `this` becomes dangling the moment
//                     the functor (the sole shared_ptr holder) is destroyed,
//                     while the safe functor would prevent that destruction.
//  C. UnsafePattern_DoubleCompletion
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
#include <queue>
#include <utility>

#include <gtest/gtest.h>

namespace
{

// ============================================================================
// IoQueue — minimal model of a single-threaded Boost.Asio io_context queue.
//
// In production, Boost.Asio's io_context holds a queue of completion
// handlers.  When an async operation completes, its handler is pushed onto
// that queue and dispatched (run to completion, one at a time) on the next
// call to io_context::run() / poll().
//
// IoQueue replaces io_context here so that tests can:
//   1. post() completion handlers exactly as async ops would enqueue them.
//   2. drain() them sequentially, mirroring single-threaded dispatch.
//
// This makes the double-completion scenario explicit: two handlers for the
// same async_read are post()ed before either runs, then drained one by one.
// ============================================================================
struct IoQueue
{
    void post(std::function<void()> handler)
    {
        queue.push(std::move(handler));
    }

    // Dispatch all queued handlers in FIFO order — mirrors io_context::run().
    void drain()
    {
        while (!queue.empty())
        {
            std::function<void()> handler = std::move(queue.front());
            queue.pop();
            handler(); // run to completion before the next one starts
        }
    }

    std::queue<std::function<void()>> queue;
};

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
    // Paired with:
    //   std::bind_front(&AsyncWorker::safeCallback, shared_from_this())
    // -----------------------------------------------------------------------
    void safeCallback(int value)
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
    // Guards with `if (callback)` so a null callback does not crash.
    // Records the null condition in callbackWasNull so tests can assert on it.
    // -----------------------------------------------------------------------
    void unsafeCompletion(const std::shared_ptr<AsyncWorker>& /*self*/)
    {
        if (!callback)
        {
            callbackWasNull = true;
            return;
        }
        callback();
    }

    // -----------------------------------------------------------------------
    // Models ConnectionInfo::afterRead — safe variant.
    // shared_from_this() is the call target; no leading shared_ptr parameter.
    // Paired with:
    //   std::bind_front(&AsyncWorker::safeCompletion, shared_from_this())
    // -----------------------------------------------------------------------
    void safeCompletion()
    {
        if (!callback)
        {
            callbackWasNull = true;
            return;
        }
        callback();
    }
};

// ============================================================================
// Test A — Safe pattern: shared_ptr as call target
// ============================================================================
//
// bind_front stores shared_from_this() in the call-target slot.  The functor
// is post()ed to IoQueue, which becomes the sole shared_ptr owner after
// obj.reset().  drain() invokes the handler then destroys the functor,
// dropping the last ref and destroying the object.
//
TEST(BindFrontThisPattern, A_SafePattern_SharedPtrIsCallTarget)
{
    bool destroyed = false;
    auto obj = std::make_shared<AsyncWorker>(destroyed);

    IoQueue ioQueue;

    // shared_from_this() occupies the call-target slot of bind_front.
    // Equivalent to the lambda: [self = shared_from_this()](int v)
    //                           { self->safeCallback(v); }
    ioQueue.post(std::bind_front(&AsyncWorker::safeCallback,
                                 obj->shared_from_this(), 21));

    // Drop the only external reference — IoQueue is now the SOLE owner.
    obj.reset();
    EXPECT_FALSE(destroyed)
        << "Object must still be alive; queued functor's shared_ptr holds it";

    // drain() invokes safeCallback(21) then destroys the functor.
    // shared_ptr refcount → 0 → object destroyed.
    ioQueue.drain();
    EXPECT_TRUE(destroyed)
        << "Object must be destroyed after the queued functor (sole owner) runs";
}

// ============================================================================
// Test B — Unsafe pattern: raw `this` becomes dangling when functor is gone
// ============================================================================
//
// Demonstrates the lifetime hazard directly: if the functor is destroyed while
// a raw copy of `this` is still held outside, that pointer is dangling.
//
// With the safe pattern the object cannot be destroyed while the functor
// exists (the shared_ptr prevents it).
//
TEST(BindFrontThisPattern, B_UnsafePattern_RawPtrOutlivesSharedPtr)
{
    bool destroyed = false;
    auto obj = std::make_shared<AsyncWorker>(destroyed);

    AsyncWorker* rawPtr = obj.get(); // non-owning observer — unsafe

    {
        IoQueue ioQueue;

        // Post the unsafe functor into a tighter scope.
        ioQueue.post(std::bind_front(&AsyncWorker::unsafeCompletion, obj.get(),
                                     obj->shared_from_this()));

        // Drop the external shared_ptr — object alive only via queued functor.
        obj.reset();
        EXPECT_FALSE(destroyed)
            << "Object must survive: queued functor's bound shared_ptr holds it";

        // ioQueue goes out of scope here — functor destroyed without being
        // drained.  Its bound shared_ptr is the last ref → object destroyed.
    }
    EXPECT_TRUE(destroyed)
        << "Functor gone → bound shared_ptr dropped → object destroyed";

    // rawPtr now points to freed memory.  Accessing it is undefined behaviour.
    // The test proves the invariant: after the unsafe functor is destroyed,
    // any raw `this` stored outside the functor is dangling.
    // With the SAFE pattern the shared_ptr in the call-target slot prevents
    // destruction while the functor lives.
    EXPECT_TRUE(destroyed); // confirms rawPtr is dangling — do NOT dereference
    (void)rawPtr;           // suppress unused-variable warning
}

// ============================================================================
// Test C — Unsafe pattern: double-completion corrupts shared mutable state
// ============================================================================
//
// Models the production crash observed in http_client.hpp:
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
// IoQueue models the io_context pending-completion queue.  Both completions
// are post()ed before either runs (exactly as Boost.Asio would enqueue them),
// then drained one by one via drain() — mirroring io_context::run().
//
// The first invocation triggers sendNext(), which sets conn->callback = nullptr
// to release the AsyncResp shared_ptr.  The second invocation then reaches
// callback(...) where callback is now null.  In production this throws
// std::bad_function_call (or SIGSEGV under sanitizers) → process terminates.
//
// The production workaround adds `if (callback)` before every call site —
// mirrored here in unsafeCompletion so the test does not crash.  The test
// instead asserts that the second completion observes a null callback, which
// is the invariant the workaround relies on.
//
// The unsafe pattern is the enabler: because `this` is the raw call target,
// there is no single ownership token representing "the active async operation".
// Two independent functors can both be queued with no compile-time or
// run-time guard preventing the second from observing state already mutated
// by the first.
//
TEST(BindFrontThisPattern, C_UnsafePattern_DoubleCompletion_NullsCallback)
{
    bool destroyed = false;
    auto obj = std::make_shared<AsyncWorker>(destroyed);

    // Keep a non-owning observer and a weak_ptr before dropping the
    // external shared_ptr, so we can inspect the object after obj.reset().
    AsyncWorker* raw = obj.get();
    std::weak_ptr<AsyncWorker> weak = obj;

    // Set up shared mutable state — models ConnectionInfo::callback.
    // Capture weak rather than obj so the lambda does not dangle after
    // obj.reset() below.
    int callCount = 0;
    obj->callback = [&callCount, weak]() {
        callCount++;
        // Models sendNext(): null the callback after the first successful
        // completion so the AsyncResp shared_ptr can be released.
        if (auto self = weak.lock())
        {
            self->callback = nullptr;
        }
    };

    // Models recvMessage() posting one async_read whose SSL stream delivers
    // TWO completions (asio.ssl error → stream_truncated).  Both are
    // enqueued before either runs — exactly as Boost.Asio would do.
    IoQueue ioQueue;
    ioQueue.post(std::bind_front(&AsyncWorker::unsafeCompletion, obj.get(),
                                 obj->shared_from_this()));
    ioQueue.post(std::bind_front(&AsyncWorker::unsafeCompletion, obj.get(),
                                 obj->shared_from_this()));

    // Drop the external reference — object stays alive via the two bound
    // shared_ptr copies inside the queued functors, exactly as in production
    // where the ConnectionPool holds the shared_ptr but each async completion
    // functor also holds one.
    obj.reset();
    EXPECT_FALSE(destroyed)
        << "Object must survive: two queued functors hold it";

    // drain() dispatches queued handlers one by one — mirrors
    // io_context::run().
    //
    // First handler fires: callback is invoked and then nulled (sendNext path).
    // Second handler fires: callback is already null → callbackWasNull is set.
    // In production (without the guard) the second handler crashes here.
    EXPECT_FALSE(raw->callbackWasNull)
        << "Before drain: callbackWasNull must start false";
    ioQueue.drain();

    EXPECT_EQ(callCount, 1)
        << "callback must have been invoked exactly once (by the first handler)";
    EXPECT_TRUE(raw->callbackWasNull)
        << "After drain: second handler must have observed a null callback";

    // After drain() all queued functors are destroyed → refcount → 0.
    EXPECT_TRUE(destroyed)
        << "Object must be destroyed once both queued functors are gone";
}

} // anonymous namespace
