---
name: abi-preserving-api-evolution
description: |
  Discipline for evolving public APIs without breaking out-of-tree
  callers.  Use when: changing a signature in RISE_API.h, adding a
  virtual to an abstract interface, adding an overload to a class
  that already has overloads of that name, or modifying any type
  whose layout is read by external code.  Covers three distinct
  layers — exported function signatures, virtual-interface vtables,
  and derived-class name hiding — each with its own failure mode.
---

# ABI-Preserving API Evolution

## When To Use

- Changing a function signature in `src/Library/RISE_API.h` (the
  documented public construction boundary).
- Adding a virtual method to an abstract interface (anything in
  `src/Library/Interfaces/`).
- Changing a virtual method's signature or removing one.
- Adding an overload to a class that already has an overload of that
  same name (derived or base).
- Changing the size, alignment, or field layout of a type that is
  referenced by pointer or reference from external code.
- Any time you think "this is just an internal refactor" but the
  affected type is reachable through `RISE_API.h`, `IJob.h`, or a
  header under `Interfaces/`.

## When NOT To Use

- Purely internal code that is not exposed through any public header
  and whose users all live in the RISE source tree.
- Adding a new function / method that does not collide with an
  existing name.
- Changing `.cpp` file content without touching any `.h`.

## The Three Layers

API compatibility has three distinct failure modes, each with its own
fix.  Identify which layer your change affects before proceeding.

### Layer 1 — Exported function signature

**The symbol name/signature changes → old-linker-linked clients fail
to link.**

C-style exported functions in `RISE_API.h` are mangled by the C++
compiler into symbols like
`_Z35RISE_API_CreateMLTRasterizerjjjjddPKcbbb` (roughly).  Change the
arg list and the mangled symbol changes — old object files calling
the old symbol no longer resolve.

### Layer 2 — Virtual-interface vtable layout

**Slot indices change → virtual calls dispatch to the wrong method.**

When an abstract class is compiled, each virtual method gets a slot
index.  Callers and subclasses compiled against that header emit
instructions that look up "slot N of the vtable" and call it.  If a
new version reshuffles the slots (including APPENDING, if the caller
later tries to call the new slot on an old-vtable object), everything
indexed past the insertion point dispatches to the wrong function.

### Layer 3 — Derived-class name hiding

**Same method name in derived class → base overloads become
invisible to concrete-type callers.**

C++ rule: declaring a function named `foo` in a derived class HIDES
every base-class `foo` overload from unqualified lookup on a
derived-type pointer.  Even if the base's overload compiles and is
accessible, `derived.foo(legacy_args)` fails to find it unless
`using Base::foo;` unhides it.

## Procedure

### Step 1 — Identify the layer

Ask which of the three layers your change touches.  Many real changes
touch two or three at once.

| Scenario | Layer |
|---|---|
| Adding params to `RISE_API_CreateFooRasterizer` | Layer 1 |
| Adding a virtual to `ICamera` | Layer 2 |
| Adding virtual `SetFoo` to `Job` when `IJob::SetFoo` already exists | Layer 2 + 3 |
| Renaming a virtual | Layer 2 |
| Reordering members of a struct in a public header | Layer 1 (struct layout) |

### Step 2 — Fix Layer 1 by adding a new name, not mutating the old

If you want new capability in a function that already has an
exported symbol:

**DO**: add a new function with a distinct name (convention:
`*WithFilter`, `*Ex`, `*V2`, or similar) that takes the new signature.
Have in-tree callers move to the new name.  Restore the old name as a
thin wrapper that forwards to the new with defaulted/null values for
the new parameters.

**DO NOT**: change the parameter list of the existing exported
function.  Even when "nobody is using the old name in this repo,"
out-of-tree clients linked against the pre-change library still call
the old mangled symbol.

Example from RISE: `RISE_API_CreateMLTRasterizer` added a pixel
sampler + filter pair.  Resolution:

```cpp
// New name, new signature, new symbol.
bool RISE_API_CreateMLTRasterizerWithFilter(..., ISampling2D*, IPixelFilter*);

// Old name, old signature, old symbol — preserved as a wrapper.
bool RISE_API_CreateMLTRasterizer(...)  // unchanged arg list
{
    return RISE_API_CreateMLTRasterizerWithFilter(..., 0, 0);
}
```

External code linked against the pre-change library keeps linking.
Internal code and new external clients use the extended variant.

### Step 3 — Fix Layer 2 by NOT putting it on the virtual surface

The strongest guarantee for vtable compatibility is: do not change
the vtable.  At all.

**DO**: add the new capability as a non-virtual method on the CONCRETE
class that needs it, and have callers reach it via `dynamic_cast` at
the call site.  Other concrete classes are untouched.  The vtable of
the abstract interface is byte-for-byte identical.

**DO NOT**: add a new virtual to the abstract interface — even at the
end.  Appending preserves slot indices for existing virtuals, which
protects OLD-caller → OLD-implementation and NEW-caller →
NEW-implementation.  It does NOT protect NEW-caller → OLD-implementation:
an out-of-tree subclass compiled against the pre-change header has
no entry in its vtable for the new slot, so a call into that slot
dispatches past the end of the table into undefined memory.

**DO NOT**: mid-insert a new virtual.  That reshuffles every slot
after the insertion point and breaks everything.

Example from RISE: MLT needed to pass a PSSMLT-driven lens sample
into the camera.  The attempt to add
`ICamera::GenerateRayWithLensSample` — even appended at the end of
the interface — was vetoed because MLT code dispatching through
`ICamera*` would crash on out-of-tree camera objects that lacked the
appended slot.  Resolution:

```cpp
// ICamera.h — unchanged, zero new virtuals.

// ThinLensCamera.h — non-virtual method specific to this concrete
// class.  NOT on any vtable.
bool GenerateRayWithLensSample(rc, ray, ptOnScreen, lensSample) const;

// MLTRasterizer.cpp — static helper, dynamic_cast at call site.
static bool GenerateCameraRayWithLensSample(
    const ICamera& camera, ...lensSample) {
    if (const ThinLensCamera* tl = dynamic_cast<const ThinLensCamera*>(&camera)) {
        return tl->GenerateRayWithLensSample(...);
    }
    return camera.GenerateRay(...);  // unchanged virtual.
}
```

Cost: one pointer comparison per dispatch.  Benefit: ICamera's
vtable is byte-identical across versions; every out-of-tree camera
implementation keeps working.

### Step 4 — Fix Layer 3 with `using Base::method;`

When a derived class introduces an overload of a name that already
exists in the base, add `using Base::method;` in the derived class's
public section to unhide the inherited overloads.

Example from RISE: `Job` overrode `SetMLTRasterizer` with the new
filter-aware signature, which hid `IJob::SetMLTRasterizer`'s legacy
inline overload.  External code holding a concrete `Job*` could no
longer call the legacy signature even though it still existed on
IJob.  Resolution:

```cpp
class Job : public IJob {
public:
    // Unhide legacy overloads from the base.
    using IJob::SetMLTRasterizer;
    using IJob::SetMLTSpectralRasterizer;

    // New filter-aware overrides.
    bool SetMLTRasterizer(..., const char* pixelFilter, ...) override;
    bool SetMLTSpectralRasterizer(..., const char* pixelFilter, ...) override;
};
```

With the `using`, `job->SetMLTRasterizer(legacy_args)` resolves to
IJob's legacy inline, which forwards to the virtual filter-aware
override.  Without it, the call fails to compile.

### Step 5 — Verify

For each layer you touched, verify:

- Layer 1: the old exported symbol name is still present in the
  built library (`nm -g librise.a | grep <symbolprefix>`).  Build
  a minimal external harness against the old header and link.
- Layer 2: the abstract interface's list of virtuals is unchanged
  relative to the prior revision.  Diff the header.
- Layer 3: write a temporary test that calls both the legacy and
  new overloads on a concrete-type pointer; both must compile.

If any check fails, go back to the procedure — do not ship
"probably fine."

## Anti-patterns

### "Nobody uses the old signature in this repo, just change it"

`RISE_API.h` is the external construction boundary.  Out-of-tree
code is invisible from inside the repo.  The fact that no in-tree
caller uses the old symbol means nothing.

### "Appending a virtual is safe because existing slots don't move"

True for old-caller → old-impl.  False for new-caller → old-impl.
The appended slot does not exist in the old vtable, so a call into
it runs off the end.  The only way to safely add a virtual is to
introduce a NEW abstract base class (or not add one at all).

### "I'll just cast around the const"

Different skill — see
[const-correctness-over-escape-hatches](const-correctness-over-escape-hatches.md).

### Using a default argument to "preserve" the old signature

Default arguments are a compile-time convenience, not an ABI
mechanism.  A function with a new default argument gets a new
mangled symbol.  Old call sites compiled against the old header do
not pass the default and do not find the new symbol.  Not
equivalent to a legacy wrapper.

### Silent layout changes to structs in public headers

Adding a field to a struct declared in a public header changes its
size.  External code holding an array of that struct breaks.  If the
struct is visible through `RISE_API.h` or an interface header, treat
it like a public API — evolve it with care (pImpl, opaque handles,
or a new struct name).

## Concrete Example (From The Repo — Full Trail)

The MLT pixel-filter work in April 2026 went through five rounds
before all three layers were correctly handled.  Abbreviated:

- **Initial change**: added `ISampling2D*` / `IPixelFilter*` to
  `RISE_API_CreateMLTRasterizer` signature.  **Layer 1 broken.**
  Out-of-tree clients no longer link.
- **Round 2 fix**: added `RISE_API_CreateMLTRasterizerWithFilter`
  as the new name, preserved the old symbol as a wrapper.
- **Round 2 added**: virtual `ICamera::GenerateRayWithLensSample`
  mid-interface.  **Layer 2 broken.**  Reshuffled slots.
- **Round 3 fix**: moved virtual to end of `ICamera`.  Still
  **Layer 2 broken** — new-caller → old-impl can still run off
  the vtable.
- **Round 4 fix**: removed the virtual entirely.  Added a
  non-virtual `ThinLensCamera::GenerateRayWithLensSample`, routed
  MLT through a `dynamic_cast` helper.  Layer 2 now truly
  preserved.
- **Meanwhile**: added filter-aware override on `Job`, which hid
  `IJob`'s legacy overload.  **Layer 3 broken.**
- **Round 4 fix**: added `using IJob::SetMLTRasterizer;` and
  `using IJob::SetMLTSpectralRasterizer;` to unhide.

Applying this skill from the start would have compressed those five
rounds into one.

## Stop Rule

The skill's work is done when, for each layer the change touched:

- A minimal out-of-tree harness compiled against the pre-change
  header would still link and run correctly against the new
  library.
- The corresponding verification in Step 5 passes.

If you cannot state this with confidence, the skill is not done.
