# src/3DSMax (3ds Max plugin) -- FROZEN (legacy, unsupported)

Status since 2026-10-09 (legacy-deprecation Phase 1,
[docs/LEGACY_DEPRECATION_ASSESSMENT.md](../../docs/LEGACY_DEPRECATION_ASSESSMENT.md) §13):
**frozen**.  Owner ruling #15: nobody uses it.  It calls legacy IJob entry points (several of them now frozen or deprecated chunk types) and is not built here.

- It stays in the tree as it is.  Removal is **on hold** by owner ruling
  (2026-10-09); nothing here is deleted in Phase 1.
- It receives no fixes and no maintenance.  It is not built or tested by
  any gate, and it may no longer build or work against the current library.
- Do not file debt-ledger rows against it, and do not extend it.  Changes to
  `src/Library` do not need to keep it compiling.
