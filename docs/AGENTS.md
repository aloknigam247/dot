# Documentation

## Index

- [Design language](design/DESIGN.md): shared Surfaces rules, metrics, profiles, palette, and change control.
- [Implementation target](design/implementation.md): approved dot composition and native color bindings.
- [Reference images](design/images): visual specifications linked from the design documents.

## Editing

Keep shared rules in `DESIGN.md` and product-specific mappings in `implementation.md`; link rather than duplicate.
Reference images describe the approved target, not proof that the native applications implement it.
Verify implementation claims against the [Android](../android/AGENTS.md) or [desktop](../desktop/AGENTS.md) entrypoints.
Follow the design language's change-control policy before changing its rules or profiles.

Build prerequisites belong in [SETUP.md](../SETUP.md); repository layout belongs in the root
[Layout](../AGENTS.md#layout) table.
