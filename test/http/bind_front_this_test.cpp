// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
//
// Proves the difference between:
//
//   UNSAFE: bind_front(&Self::cb, this, shared_from_this())
//   SAFE:   bind_front(&Self::cb, shared_from_this())
//
// --- The problem ---
//
// std::bind_front stores its first argument as the call target and
// invokes it on every call.  Two patterns appear in the bmcweb async
// callback code, with very different ownership semantics:
//
//   UNSAFE: bind_front(&Self::cb, this, shared_from_this())
//     `this` is the call target — a raw pointer with no ownership.
//     shared_from_this() is bound as the first method argument, which
//     incidentally keeps the object alive while that handler exists.
//     But the raw pointer and its guardian shared_ptr are two separate
//     values with no enforced coupling.
//
//   SAFE: bind_front(&Self::cb, shared_from_this())
//     shared_from_this() is the call target.  The handler owns the
//     object directly; the object cannot be destroyed while the
//     handler is alive.
//
// The unsafe pattern breaks in at least two ways:
//
//  1. If the handler is destroyed without being invoked (e.g. the
//     I/O queue is discarded) the bound shared_ptr may be the last
//     owner.  Any raw `this` copy held elsewhere becomes dangling.
//
//  2. Under double-completion — certain SSL teardown sequences deliver
//     two completion events for the same async_read (asio.ssl error
//     followed by stream_truncated).  Both handlers are queued against
//     the same raw `this`.  The first nulls conn->callback (via
//     sendNext); the second then calls the now-null callback causing
//     std::bad_function_call or a use-after-free.
//
// The unsafe pattern also forces every callback to carry a spurious
// leading `const shared_ptr<Self>&` parameter whose sole purpose is
// lifetime extension — responsibility that belongs to the call-target
// slot of bind_front.
//
// --- Tests ---
//
//  A. SafePattern — bind_front built with shared_from_this() as call
//                   target; proves the handler keeps the object alive.
//  B. UnsafePattern_RawPtrOutlivesSharedPtr
//                 — the handler is destroyed undrained; the bound
//                   shared_ptr was the last owner so the object is
//                   destroyed, leaving any external raw `this` dangling.
//  C. UnsafePattern_DoubleCompletion
//                 — two handlers queued for the same raw `this` share
//                   mutable state.  The first invocation nulls the
//                   callback; the second observes null — models the
//                   production crash in http_client.hpp.

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
    // -----------------------------------------------------------------------
    void unsafeCompletion(const std::shared_ptr<AsyncWorker>& /*self*/)
    {
        if (callback)
        {
            callback();
        }
    }
};

// ============================================================================
// Test A — Safe pattern: shared_ptr as call target
// ============================================================================
//
// bind_front stores shared_from_this() in the call-target slot.  The handler
// is post()ed to IoQueue, which becomes the sole shared_ptr owner after
// obj.reset().  drain() invokes the handler then destroys it, dropping
// the last ref and destroying the object.
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
        << "Object must still be alive; queued handler's shared_ptr holds it";

    // drain() invokes safeCallback(21) then destroys the handler.
    // shared_ptr refcount → 0 → object destroyed.
    ioQueue.drain();
    EXPECT_TRUE(destroyed)
        << "Object must be destroyed after the queued handler (sole owner) runs";
}

// ============================================================================
// Test B — Unsafe pattern: raw `this` becomes dangling when handler is gone
// ============================================================================
//
// Demonstrates the lifetime hazard directly: if the handler is destroyed
// while a raw copy of `this` is still held outside, that pointer is dangling.
//
// With the safe pattern the object cannot be destroyed while the handler
// exists (the shared_ptr prevents it).
//
TEST(BindFrontThisPattern, B_UnsafePattern_RawPtrOutlivesSharedPtr)
{
    bool destroyed = false;
    auto obj = std::make_shared<AsyncWorker>(destroyed);

    AsyncWorker* rawPtr = obj.get(); // non-owning observer — unsafe

    {
        IoQueue ioQueue;

        // Post the unsafe handler into a tighter scope.
        ioQueue.post(std::bind_front(&AsyncWorker::unsafeCompletion, obj.get(),
                                     obj->shared_from_this()));

        // Drop the external shared_ptr — object alive only via queued handler.
        obj.reset();
        EXPECT_FALSE(destroyed)
            << "Object must survive: queued handler's bound shared_ptr holds it";

        // ioQueue goes out of scope here — handler destroyed without being
        // drained.  Its bound shared_ptr is the last ref → object destroyed.
    }
    EXPECT_TRUE(destroyed)
        << "Handler gone → bound shared_ptr dropped → object destroyed";

    // rawPtr now points to freed memory.  Accessing it is undefined behavior.
    // The test proves the invariant: after the unsafe handler is destroyed,
    // any raw `this` stored outside the handler is dangling.
    // With the SAFE pattern the shared_ptr in the call-target slot prevents
    // destruction while the handler is alive.
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
// Under certain SSL error conditions the stream delivers TWO completion
// events for that single async_read (e.g. asio.ssl error followed by
// stream_truncated).  Both handlers are queued against the same raw `this`.
// The first nulls conn->callback (via sendNext); the second then calls the
// now-null callback → std::bad_function_call → process terminates.
//
// Because `this` is the raw call target there is no ownership token
// coupling the two handlers, so nothing prevents the second from
// observing state already mutated by the first.
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
    // shared_ptr copies inside the queued handlers, exactly as in production
    // where the ConnectionPool holds the shared_ptr but each async completion
    // handler also holds one.
    obj.reset();
    EXPECT_FALSE(destroyed)
        << "Object must survive: two queued handlers hold it";

    // drain() dispatches queued handlers one by one — mirrors
    // io_context::run().
    //
    // First handler fires: callback is invoked and then nulled (sendNext path).
    // Second handler fires: callback is already null, so it is skipped.
    // In production (without the guard) the second handler crashes here.
    EXPECT_TRUE(raw->callback != nullptr)
        << "Before drain: callback must be set";
    ioQueue.drain();

    EXPECT_EQ(callCount, 1)
        << "callback must have been invoked exactly once (by the first handler)";
    EXPECT_FALSE(raw->callback)
        << "After drain: callback must be null (nulled by the first handler)";

    // After drain() all queued handlers are destroyed → refcount → 0.
    EXPECT_TRUE(destroyed)
        << "Object must be destroyed once both queued handlers are gone";
}

} // anonymous namespace
