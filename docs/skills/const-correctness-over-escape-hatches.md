---
name: const-correctness-over-escape-hatches
description: |
  Decision tree for "I need to mutate X inside a const method."  Use
  BEFORE reaching for `mutable`, `const_cast`, or dropping `const`
  from a method signature.  Forces the real design question — is the
  method conceptually const, or is the const a lie — instead of
  silently papering over the conflict with a language escape hatch.
---

# Const-Correctness Over Escape Hatches

## When To Use

- You are writing a method marked `const` and you realise you need
  to write to a member.
- You are tempted to add `mutable` to a member so a `const` method
  can touch it.
- You are tempted to add `const_cast` anywhere.
- You are tempted to drop `const` from a method that was `const`
  before.
- Someone else's code already uses `mutable` / `const_cast` and you
  are wondering whether to follow the pattern.

## When NOT To Use

- The member is a mutex / atomic counter / lazily-initialised cache
  whose mutation genuinely does not change observable state.
  (`mutable` is legitimate here; see "Legitimate `mutable`" below.)
- You are calling a third-party API that incorrectly takes a
  non-const pointer for read-only work.  `const_cast` at the
  boundary is sometimes the only option; document it.

## The Core Question

Before doing anything, answer this:

> Is this mutation observable to callers?

"Observable" means: would two callers, using the same sequence of
operations on the same object, see different results because of
this mutation?

- **No** — the mutation is internal bookkeeping, state that a
  caller cannot distinguish from "it was always that way."
  `mutable` is the right tool IF the storage is per-object and
  thread-safe.
- **Yes** — the mutation IS part of the object's observable state.
  The method is not conceptually const; stop trying to make it
  const.

Most cases where people reach for `mutable` or `const_cast` are the
second kind — the method is not really const and the escape hatch is
hiding a design bug.

## Decision Tree

```
You are inside a const method and want to mutate member X.
    │
    ├── Is the mutation observable to callers?
    │       YES → The method is NOT const.  Drop the `const`.
    │              Do not use `mutable`, do not `const_cast`.
    │              If the method is on an interface whose const-ness
    │              you cannot change, add a new non-const method
    │              with a different name instead of corrupting the
    │              existing contract.
    │
    │       NO →  Continue below.
    │
    ├── Is X thread-shared state?
    │       YES → `mutable` WITHOUT a lock is a race hazard.  Use
    │              `mutable std::atomic<T>` for a counter, `mutable
    │              std::mutex` + guarded member for complex state,
    │              or move the state out of the object entirely
    │              (pass it in, or thread-local it).
    │
    │       NO →  Continue below.
    │
    ├── Is X computed purely from the object's logical state?
    │       YES → `mutable` is legitimate as a cache.  Document the
    │              invariant: "cache of pure function f(*this);
    │              never observed externally."
    │
    │       NO →  Stop.  The mutation depends on something other than
    │              the logical state.  Figure out what it depends on
    │              and either pass that dependency in as a parameter,
    │              or accept that the method is not const.
    │
    └── Is X actually a dependency injection problem in disguise?
            YES → Stop using a member.  Pass the dependency as a
                   parameter or accept a strategy / callback object.
                   The "need to mutate a member" disappears.
```

## Legitimate `mutable`

`mutable` is the right answer in a small set of real cases:

1. **Mutex / lock for concurrent access** to otherwise-const data.
   `mutable std::mutex mu_;` in a thread-safe const method is
   standard.

2. **Lazily-initialised cache** of a value that is a pure function
   of the object's logical state.  Two callers using the same
   operation sequence see the same result; the cache is invisible
   to them except as a performance optimization.

3. **Reference counts** and similar bookkeeping whose mutation
   cannot be observed via the object's public interface.
   `Implementation::Reference` in RISE is an example — `addref()`
   and `release()` mutate internal counts but the object's
   logical state is unchanged.

4. **Per-object RNG state** for a const method that performs
   stochastic reads (e.g. `CanonicalRandom()` on a
   `mutable MersenneTwister mt`).  The RNG stream is not part of
   observable state; two callers get the "same result" in the
   sense that their statistical expectations match.

Each legitimate use has the same shape: the mutation is per-object,
bounded in scope, and invisible in the contract.

## Illegitimate `mutable` / `const_cast`

Common anti-patterns that should be fixed, not excused:

### "I need to update a cache across objects"

Shared state across objects in a const method is not a cache — it's
observable mutation of shared global state.  If `a.doFoo()` affects
what `b.doFoo()` returns later, neither method is const.

### "The API forces const but my implementation needs to mutate"

You do not have a const-correctness problem; you have an interface
design problem.  Either:

- Rethink whether the interface should be const (often no).
- Add a non-const method for the mutating operation.
- Split the interface into const and non-const halves.

`const_cast`-ing away the const is lying to every caller that
relies on the const contract — including the compiler's aliasing
optimizer.  It is undefined behavior if the original object was
declared const.

### "I want to avoid reallocating this vector so I keep it as a `mutable` scratch buffer"

This is sometimes OK as a cache (see legitimate use #2) BUT it is a
thread-safety landmine if the object is ever shared across threads.
The better solution is usually to hoist the scratch buffer out of
the object — into the caller's stack, into a thread-local, or into a
per-call parameter.  If you keep it `mutable`, document the
thread-affinity invariant and consider asserting it.

### "Small `const_cast` at one call site"

There are no small `const_cast`s.  Each one is either provably safe
(in which case it is unnecessary — refactor) or a latent UB bug (in
which case it is unsafe).  The only legitimate use is at a poorly-
designed third-party API boundary, and even then it needs a comment
explaining why the underlying object is actually non-const in
practice.

### "Drop the const, less typing"

Removing `const` propagates outward — every caller now also has to
be non-const, every reference has to be non-const, const-correct
callers can no longer use your API.  Once `const` is gone it is
expensive to put back.  Be very sure before dropping it.

## Concrete Examples (From The Repo)

### Legitimate `mutable`: `RandomNumberGenerator`

```cpp
class RandomNumberGenerator {
    mutable MersenneTwister mt;   // OK — stream state is per-object,
                                  //      per-thread in practice,
                                  //      and CanonicalRandom is
                                  //      "const" in the sense that
                                  //      the RNG's abstract spec
                                  //      allows the stream to advance.
public:
    inline double CanonicalRandom() const {
        return mt.genrand_res53();  // mutates mt via mutable.
    }
};
```

This is legitimate because the stream state is bookkeeping the
caller cannot distinguish from "this particular RNG happens to be
at this point in its sequence."  The invariant is per-object, per-
thread.  If the RNG were shared across threads the mutation would
race; in RISE it isn't.

### Would-be illegitimate: lens sample in the camera

Early attempts to add lens-sample support to MLT considered
seeding a hidden RNG inside the RuntimeContext so the camera's
`rc.random.CanonicalRandom()` calls would return the PSSMLT lens
sample.  This would have required mutating RuntimeContext inside
`ThinLensCamera::GenerateRay(const ...) const` — either by
`const_cast`-ing `rc` or by making something inside `rc` `mutable`.

Applying the decision tree: the mutation IS observable (the RNG
stream state afterward differs based on lens injection) → drop the
const? No, `GenerateRay` really is const on the camera → this is a
dependency injection problem → pass the lens sample in as a
parameter.

Resolution: added
`ThinLensCamera::GenerateRayWithLensSample(rc, ray, ptOnScreen,
lensSample) const` as a separate non-virtual method that takes the
lens sample explicitly.  No mutable, no const_cast, no lying.

### Illegitimate (hypothetical, don't do it)

```cpp
class Camera {
    mutable Matrix4 cachedInverseWorldMatrix;
    mutable bool    cacheValid = false;
    mutable std::vector<Ray> scratchRays;  // DANGER
public:
    bool GenerateRay(const RuntimeContext& rc, Ray& r, const Point2& p) const {
        if (!cacheValid) {
            cachedInverseWorldMatrix = ComputeInverse(worldMatrix);
            cacheValid = true;
        }
        scratchRays.clear();
        scratchRays.push_back(Ray(...));   // mutate scratchRays
        // ... use scratchRays ...
    }
};
```

The `cachedInverseWorldMatrix` / `cacheValid` pair is fine
(legitimate cache).  But `scratchRays` is a landmine: if two
threads call `GenerateRay` on the same Camera simultaneously, the
`clear()` / `push_back()` race.  A renderer using shared-Camera
concurrency (RISE does) would silently corrupt rays.

Fix: hoist `scratchRays` out of the object.  Either (a) local
variable inside `GenerateRay`, (b) thread-local, or (c) passed in.
The const-correctness of the method is preserved and the race goes
away.

## Anti-patterns (Summary)

- **`mutable` as a thought-terminator.**  If the answer to "why is
  this mutable" is "because the method is const and I need to
  mutate," the decision tree was skipped.
- **`const_cast` with a comment promising it's safe.**  Write the
  proof in code instead by refactoring the caller's type.
- **Dropping `const` to avoid thinking.**  Propagates non-const
  outward and is expensive to undo.
- **Mutating shared state from a const method.**  By definition the
  method is not const.  The compiler cannot catch this because it
  cannot see "shared" — it's your responsibility.

## Stop Rule

The skill's work is done when either:

- You have applied the decision tree and chosen the appropriate
  path: either `const` dropped / a new non-const method added, or
  `mutable` used for a verified-legitimate case with a
  documentation comment, or a dependency lifted out of the member.
- You have determined the escape hatch IS legitimate and added a
  code comment citing which of the four legitimate uses applies
  and why.

A `mutable` or `const_cast` in the codebase without an accompanying
comment explaining its legitimacy is technical debt and should be
revisited when next touched.
