# src/DRISE (distributed RISE client/server) -- FROZEN (legacy, unsupported)

Status since 2026-10-09 (legacy-deprecation Phase 1,
[docs/LEGACY_DEPRECATION_ASSESSMENT.md](../../docs/LEGACY_DEPRECATION_ASSESSMENT.md) §13):
**frozen**.  Owner ruling #15: kept, frozen.  The owner intends a future state-of-the-art distributed renderer; this code is the historical starting point, not a supported surface.  build/make/rise/Makefile still has `drise_server` / `drise_client` targets (not part of `all`).

- It stays in the tree as it is.  Removal is **on hold** by owner ruling
  (2026-10-09); nothing here is deleted in Phase 1.
- It receives no fixes and no maintenance.  It is not built or tested by
  any gate, and it may no longer build or work against the current library.
- Do not file debt-ledger rows against it, and do not extend it.  Changes to
  `src/Library` do not need to keep it compiling.
