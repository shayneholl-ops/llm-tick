# Domain Docs

How the engineering skills should consume this repo's domain documentation when
exploring the codebase.

**Layout: single-context.**

```
/
├── CONTEXT.md          ← glossary: Board, Panel, Usage feed, Server, Ledger,
│                          Standby, Provisioning, Setup AP, Provisioned,
│                          Verify-then-reboot, Factory default
├── docs/adr/           ← decisions, newest last
│   └── 0001-explicit-provisioning-trigger.md
├── docs/spec/          ← specs, one per feature
│   └── f2-web-provisioning.md
└── src/
```

## Before exploring, read these

- **`CONTEXT.md`** at the repo root. Use its vocabulary in issue titles, refactor
  proposals, hypotheses and test names. In particular: **Board** (the whole device)
  vs **Panel** (the glass); **Server** (the machine running `server.py`) vs **Ledger**
  (the token log it reads); **Standby** (a state inferred from staleness, not a mode).
- **`docs/adr/`**: read ADRs touching the area you are about to work in.
- **`docs/spec/`**: read the spec for the feature you are working on.

If any of these don't exist, **proceed silently** — don't flag their absence or propose
creating them upfront. `/domain-modeling` creates them lazily when terms or decisions
actually get resolved.

## Flag ADR conflicts

If your output contradicts an existing ADR, surface it explicitly rather than silently
overriding:

> _Contradicts ADR-0001 (explicit provisioning trigger), but worth reopening because…_
