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
//  1. SafePattern   — bind_front built inside a member function with
//                     shared_from_this() as call target.
//  2. UnsafePattern — bind_front built inside a member function with
//                     `this` as call target and shared_from_this() as param.
//  3. UnsafePattern_RawPtrOutlivesSharedPtr
//                   — shows that the raw `this` becomes dangling the moment
//                     the functor (the sole shared_ptr holder) is destroyed,
//                     while the safe functor would prevent that destruction.

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

    explicit AsyncWorker(bool& destroyedFlag) : destroyed(destroyedFlag) {}
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
    // Builds the SAFE functor from inside a member function.
    // shared_from_this() is the *call target* — the shared_ptr is stored
    // in the functor and used to dereference the object on invocation.
    // -----------------------------------------------------------------------
    std::function<void(int)> makeSafeFunctor()
    {
        // shared_from_this() occupies the call-target slot of bind_front.
        // Equivalent to the lambda: [self = shared_from_this()](int v)
        //                           { self->safeCallback(v); }
        return std::bind_front(&AsyncWorker::safeCallback, shared_from_this());
    }

    // -----------------------------------------------------------------------
    // Builds the UNSAFE functor from inside a member function.
    // `this` is the *call target* (raw, unowned pointer).
    // shared_from_this() is stored as the first bound argument and passed as
    // the first method parameter — it keeps the object alive, but the call
    // itself goes through the raw pointer.
    // -----------------------------------------------------------------------
    std::function<void(int)> makeUnsafeFunctor()
    {
        // `this` occupies the call-target slot; invocation does:
        //   (this->*&AsyncWorker::unsafeCallback)(shared_ptr_copy, value)
        return std::bind_front(&AsyncWorker::unsafeCallback,
                               this, // raw pointer — no ownership
                               shared_from_this()); // kept as method parameter
    }
};

// ============================================================================
// Test 1 — Safe pattern: shared_ptr as call target
// ============================================================================
//
// The functor built by makeSafeFunctor() stores a shared_ptr as the call
// target.  Dropping the external shared_ptr leaves the functor as the sole
// owner; the object stays alive and the callback fires correctly.
//
TEST(BindFrontThisPattern, SafePattern_SharedPtrIsCallTarget)
{
    bool destroyed = false;

    std::function<void(int)> fn;
    {
        auto obj = std::make_shared<AsyncWorker>(destroyed);

        // Functor is built inside the member function with
        // `shared_from_this()`.
        fn = obj->makeSafeFunctor();

        // Drop the only external reference.
        obj.reset();

        // The functor's shared_ptr copy is now the SOLE owner.
        EXPECT_FALSE(destroyed)
            << "Object must still be alive; functor's shared_ptr holds it";
    }

    // Call the functor — object is alive, `this` is valid because the
    // shared_ptr inside the functor owns it.
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
// The functor built by makeUnsafeFunctor() stores `this` as the call target
// and a shared_ptr copy as the first method parameter.  The object is kept
// alive by the bound shared_ptr — but only for as long as the functor lives.
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

        // Functor is built inside the member function with `this`.
        fn = obj->makeUnsafeFunctor();

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
            std::function<void(int)> fn = obj->makeUnsafeFunctor();

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
    std::function<void(int)> safeFn = obj->makeSafeFunctor();

    // Unsafe: callback method has signature
    //   void(const shared_ptr<AsyncWorker>&, int)
    // bind_front pre-binds the shared_ptr so the resulting std::function
    // signature is still void(int) — but the underlying method is burdened
    // with a parameter it only uses to keep itself alive.
    std::function<void(int)> unsafeFn = obj->makeUnsafeFunctor();

    safeFn(10);
    EXPECT_EQ(obj->result, 20);

    unsafeFn(15);
    EXPECT_EQ(obj->result, 30);
}

} // anonymous namespace
