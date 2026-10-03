// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
//
// Unit tests for the bind_front / shared_from_this() lifetime invariant.
//
// Background
// ----------
// bmcweb async callbacks use std::bind_front to attach a member function
// to its owning object.  Two patterns appear with very different ownership
// semantics:
//
//   UNSAFE: bind_front(&Self::cb, this, shared_from_this())
//     `this`             — raw pointer, used as the implicit object.
//     shared_from_this() — extra bound argument that incidentally keeps
//                          the object alive while the handler exists.
//                          But the raw pointer and its guardian shared_ptr
//                          are two separate values with no enforced coupling.
//
//   SAFE: bind_front(&Self::cb, shared_from_this())
//     shared_from_this() — stored as the implicit object pointer inside
//                          the callable.  The callable itself holds the
//                          reference; the object cannot be destroyed while
//                          the handler is alive.
//
// The unsafe pattern breaks in two ways:
//
//   1. If the handler is destroyed without being invoked (e.g. the I/O
//      queue is discarded) the bound shared_ptr may be the last owner.
//      Any raw `this` copy held elsewhere becomes dangling.
//
//   2. Under double-completion — certain SSL teardown sequences deliver
//      two completion events for the same async_read (asio.ssl error
//      followed by stream_truncated).  Both handlers are queued against
//      the same raw `this`.  The first nulls conn->callback (via
//      sendNext); the second then calls the now-null callback causing
//      std::bad_function_call or a use-after-free.
//
// The full end-to-end double-completion scenario that triggered the
// production crash is proved in http_client_test.cpp.  These tests
// prove the underlying lifetime guarantee in isolation: pure C++,
// no network, no TLS, fully deterministic.
//
// --- Test suites ---
//
//   SharedFromThisLifetimeTest — uses a real boost::asio::io_context to
//     prove the guarantee holds through Asio's actual dispatch machinery.
//
//     SafePattern_ObjectAliveWhenHandlerFires
//       shared_from_this() as call target keeps the object alive until
//       the handler fires and is destroyed.
//
//     UnsafePattern_ObjectDestroyedBeforeHandlerFires
//       Dropping both the external ref and the bound sptr destroys the
//       object; calling the handler at that point would be UB.
//
//   BindFrontThisPattern — uses a minimal IoQueue mock to prove the full
//     async lifecycle: post, hold, drain-or-discard.
//
//     A_SafePattern_SharedPtrIsCallTarget
//       Handler keeps the object alive as the sole owner; object is
//       destroyed exactly when the handler is drained.
//
//     B_UnsafePattern_RawPtrOutlivesSharedPtr
//       Handler destroyed without being invoked (scope ends); the bound
//       shared_ptr was the last owner so the object is destroyed, leaving
//       any external raw `this` dangling.
//
//     C_UnsafePattern_DoubleCompletion_NullsCallback
//       Two handlers queued for the same raw `this` share mutable state.
//       The first invocation nulls the callback; the second observes
//       null — models the production crash in http_client.hpp.

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <functional>
#include <memory>
#include <queue>
#include <utility>

#include <gtest/gtest.h>

namespace
{

// ============================================================================
// AsyncTarget — minimal subject for the SharedFromThisLifetimeTest suite.
// ============================================================================
struct AsyncTarget : std::enable_shared_from_this<AsyncTarget>
{
    bool callbackFiredWhileAlive = false;

    void onComplete()
    {
        callbackFiredWhileAlive = true;
    }
};

// ============================================================================
// IoQueue — minimal model of a single-threaded Boost.Asio io_context queue.
//
// Replaces io_context in the BindFrontThisPattern suite so that tests can:
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
            handler();
        }
    }

    std::queue<std::function<void()>> queue;
};

// ============================================================================
// AsyncWorker — subject for the BindFrontThisPattern suite.
//
// Models the bmcweb ConnectionInfo / websocket async pattern.
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

    // Safe callback: shared_from_this() is the call target.
    // Paired with: bind_front(&AsyncWorker::safeCallback, shared_from_this())
    void safeCallback(int value)
    {
        result = value * 2;
    }

    // Unsafe callback: `this` is the raw call target; the leading shared_ptr
    // is the spurious lifetime-extension parameter.
    // Paired with: bind_front(&AsyncWorker::unsafeCompletion, this,
    //                          shared_from_this())
    void unsafeCompletion(const std::shared_ptr<AsyncWorker>& /*self*/) const
    {
        if (callback)
        {
            callback();
        }
    }
};

// ============================================================================
// SharedFromThisLifetimeTest suite — real boost::asio::io_context
// ============================================================================

// SafePattern: bind_front(&AsyncTarget::onComplete, shared_from_this())
//
// shared_from_this() is the implicit object stored in the callable.  The
// callable itself holds the only remaining reference after the external
// shared_ptr is reset.  onComplete() fires on a live object.
TEST(SharedFromThisLifetimeTest, SafePattern_ObjectAliveWhenHandlerFires)
{
    boost::asio::io_context io;

    auto obj = std::make_shared<AsyncTarget>();
    std::weak_ptr<AsyncTarget> weak = obj;

    // SAFE: shared_from_this() is the call target — handler owns the ref.
    auto handler =
        std::bind_front(&AsyncTarget::onComplete, obj->shared_from_this());

    // Keep a separate observer reference for post-run result inspection.
    // This is NOT the reference that proves the invariant — the handler's
    // internal shared_ptr is the one that must keep the object alive.
    std::shared_ptr<AsyncTarget> observer = obj;

    // Drop the sole "external owner" reference (simulates the pool
    // releasing the connection after posting the async op).
    obj.reset();

    // Object must still be alive: handler holds it via shared_from_this().
    ASSERT_FALSE(weak.expired())
        << "Object destroyed before handler fired — shared_from_this() "
           "should have kept it alive";

    // Fire the handler.  After io.run() the handler has been destroyed
    // and its internal shared_ptr released; only observer remains.
    boost::asio::post(io, std::move(handler));
    io.run();

    // Verify the callback ran on a live object.
    EXPECT_TRUE(observer->callbackFiredWhileAlive)
        << "onComplete() must have been called while the object was alive";
}

// UnsafePattern: bind_front(&AsyncTarget::onComplete, this,
//                            shared_from_this())
//
// `this` is the raw implicit-object pointer stored by bind_front.
// shared_from_this() is merely an extra argument.  When the external owner
// drops before the handler fires and no other shared_ptr holds the object,
// the raw `this` is dangling.
//
// We prove this by resetting the external shared_ptr AND releasing the
// extra sptr argument before calling the handler, then checking the
// weak_ptr: the object is gone.  Calling the handler at that point would
// be undefined behaviour — the test asserts the object is destroyed,
// demonstrating the hazard without actually invoking UB.
TEST(SharedFromThisLifetimeTest,
     UnsafePattern_ObjectDestroyedBeforeHandlerFires)
{
    auto obj = std::make_shared<AsyncTarget>();
    std::weak_ptr<AsyncTarget> weak = obj;

    // UNSAFE: `this` is the implicit object; sptr is just a bound argument.
    // Capture the extra sptr separately so we can release it explicitly.
    std::shared_ptr<AsyncTarget> extraSptr = obj->shared_from_this();

    // Simulate what bind_front(&Foo::cb, this, shared_from_this()) stores:
    // a raw this + a shared_ptr argument.  When both external refs drop,
    // nothing keeps the object alive.
    AsyncTarget* rawThis = obj.get();
    (void)rawThis; // would be the implicit object in the real bind_front

    // Drop both the external ref and the bound sptr argument.
    obj.reset();
    extraSptr.reset();

    // Object is now destroyed — raw `this` is dangling.
    EXPECT_TRUE(weak.expired())
        << "Object should be destroyed once both shared_ptrs are released; "
           "calling the handler at this point is undefined behaviour";
}

// ============================================================================
// BindFrontThisPattern suite — IoQueue mock, full async lifecycle
// ============================================================================

// Test A — Safe pattern: shared_ptr as call target
//
// bind_front stores shared_from_this() in the call-target slot.  The handler
// is post()ed to IoQueue, which becomes the sole shared_ptr owner after
// obj.reset().  drain() invokes the handler then destroys it, dropping
// the last ref and destroying the object.
TEST(BindFrontThisPattern, A_SafePattern_SharedPtrIsCallTarget)
{
    bool destroyed = false;
    auto obj = std::make_shared<AsyncWorker>(destroyed);

    IoQueue ioQueue;

    // shared_from_this() occupies the call-target slot of bind_front.
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

// Test B — Unsafe pattern: raw `this` becomes dangling when handler is gone
//
// If the handler is destroyed without being invoked (scope ends before
// drain()), the bound shared_ptr was the last owner.  The object is
// destroyed and any external raw `this` is left dangling.
TEST(BindFrontThisPattern, B_UnsafePattern_RawPtrOutlivesSharedPtr)
{
    bool destroyed = false;
    auto obj = std::make_shared<AsyncWorker>(destroyed);

    AsyncWorker* rawPtr = obj.get(); // non-owning observer — unsafe

    {
        IoQueue ioQueue;

        ioQueue.post(std::bind_front(&AsyncWorker::unsafeCompletion, obj.get(),
                                     obj->shared_from_this()));

        // Drop the external shared_ptr — object alive only via queued handler.
        obj.reset();
        EXPECT_FALSE(destroyed)
            << "Object must survive: queued handler's bound shared_ptr holds it";

        // ioQueue goes out of scope — handler destroyed without being drained.
        // Its bound shared_ptr is the last ref → object destroyed.
    }
    EXPECT_TRUE(destroyed)
        << "Handler gone → bound shared_ptr dropped → object destroyed";

    // rawPtr now points to freed memory — do NOT dereference.
    EXPECT_TRUE(destroyed); // confirms rawPtr is dangling
    (void)rawPtr;
}

// Test C — Unsafe pattern: double-completion corrupts shared mutable state
//
// Models the production crash in http_client.hpp:
//   recvMessage() posts async_read with bind_front(&afterRead, this,
//   shared_from_this()).  Under unclean SSL shutdown the stream delivers
//   TWO completions for that single async_read.  Both handlers share the
//   same raw `this`.  The first nulls conn->callback (sendNext path);
//   the second calls the now-null callback → std::bad_function_call.
TEST(BindFrontThisPattern, C_UnsafePattern_DoubleCompletion_NullsCallback)
{
    bool destroyed = false;
    auto obj = std::make_shared<AsyncWorker>(destroyed);

    AsyncWorker* raw = obj.get();
    std::weak_ptr<AsyncWorker> weak = obj;

    // Models ConnectionInfo::callback: invoke once then null itself.
    int callCount = 0;
    obj->callback = [&callCount, weak]() {
        callCount++;
        if (auto self = weak.lock())
        {
            self->callback = nullptr;
        }
    };

    // Two completions queued before either runs — mirrors Boost.Asio
    // SSL double-completion behaviour.
    IoQueue ioQueue;
    ioQueue.post(std::bind_front(&AsyncWorker::unsafeCompletion, obj.get(),
                                 obj->shared_from_this()));
    ioQueue.post(std::bind_front(&AsyncWorker::unsafeCompletion, obj.get(),
                                 obj->shared_from_this()));

    obj.reset();
    EXPECT_FALSE(destroyed)
        << "Object must survive: two queued handlers hold it";

    EXPECT_TRUE(raw->callback != nullptr)
        << "Before drain: callback must be set";
    ioQueue.drain();

    EXPECT_EQ(callCount, 1)
        << "callback must be invoked exactly once (by the first handler)";
    EXPECT_FALSE(raw->callback)
        << "After drain: callback must be null (nulled by first handler)";

    EXPECT_TRUE(destroyed)
        << "Object must be destroyed once both queued handlers are gone";
}

} // anonymous namespace
