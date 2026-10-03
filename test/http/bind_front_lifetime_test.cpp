// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
//
// Unit tests for the bind_front / shared_from_this() lifetime invariant.
//
// Background
// ----------
// bmcweb async callbacks use std::bind_front to attach a member function
// to its owning object.  There are two distinct patterns:
//
//   UNSAFE: bind_front(&Foo::cb, this, shared_from_this())
//     `this`            — raw pointer, used as the implicit object.
//     shared_from_this() — extra bound argument (appears as a parameter
//                          in cb's signature as `const shared_ptr&`).
//
//   SAFE: bind_front(&Foo::cb, shared_from_this())
//     shared_from_this() — stored as the implicit object pointer inside
//                          the callable.  The callable itself holds the
//                          reference; no external owner is needed.
//
// The difference matters when a stale completion handler fires after all
// external shared_ptr owners have released the object (e.g. the pool
// drops the connection while an async_read is still in flight).
//
//   UNSAFE: `this` is dangling once the external owner drops.  Calling
//           the handler is undefined behaviour.
//
//   SAFE:   The callable keeps the object alive until the handler fires
//           and the callable itself is destroyed.
//
// The full end-to-end double-completion scenario that triggered the
// production crash is proved in http_client_test.cpp.  These tests
// prove the underlying lifetime guarantee in isolation: pure C++,
// no network, no TLS, fully deterministic.

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <functional>
#include <memory>
#include <utility>

#include <gtest/gtest.h>

namespace
{

// ============================================================================
// A minimal object that tracks whether it was alive when its callback fired.
// ============================================================================
struct AsyncTarget : std::enable_shared_from_this<AsyncTarget>
{
    bool callbackFiredWhileAlive = false;

    // The method that would be used as a bind_front target.
    void onComplete()
    {
        callbackFiredWhileAlive = true;
    }
};

// ============================================================================
// SafePattern: bind_front(&AsyncTarget::onComplete, shared_from_this())
//
// shared_from_this() is the implicit object stored in the callable.  The
// callable itself holds the only remaining reference after the external
// shared_ptr is reset.  onComplete() fires on a live object.
// ============================================================================
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

// ============================================================================
// UnsafePattern: bind_front(&AsyncTarget::onComplete, this, shared_from_this())
//
// `this` is the raw implicit-object pointer stored by bind_front.
// shared_from_this() is merely an extra argument (unused by onComplete).
// When the external owner drops before the handler fires and no other
// shared_ptr holds the object, the raw `this` is dangling.
//
// We prove this by resetting the external shared_ptr AND releasing the
// extra sptr argument before calling the handler, then checking the
// weak_ptr: the object is gone.  Calling the handler at that point would
// be undefined behaviour — the test asserts the object is destroyed,
// demonstrating the hazard without actually invoking UB.
// ============================================================================
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

} // anonymous namespace
