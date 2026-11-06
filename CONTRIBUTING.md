# Contributions & Engineering Manifesto (darkbase)

This project is a strictly solo development process conducted in tight pair-programming partnership with an AI coding assistant.

It serves as an architectural manifesto for a **Level 3 Database Engine**: native vex relational storage, zero-allocation table queries, B-tree indexes, and lockless persistence in pure C23.

---

## 1. The AI-First Architecture Manifesto & Boilerplate Defense

This codebase strictly enforces the verbose, explicit boilerplate required across the `vexgraph` ecosystem:
- Strict prohibition of arrow syntax (`p->field` is banned; only explicit `(*p).field` is permitted).
- Single Class Per File (the Java Law: one public `typedef struct` per `.h`/`.c` pair).
- Arity-overloaded explicit constructor dispatch macros (`Class_0()`, `Class_1()`).
- Complete, symmetric getters and setters for all struct fields.
- Strict dest-last parameter ordering `(a, b, dest)`.
- Two-layer member access cap (`(*layer1).layer2` maximum).
- Exhaustive `;;OVERVIEW` blueprints mirrored at the top of every implementation file.

---

## 2. Sanity Warning for External Contributors

> [!WARNING]
> **SANITY NOTICE FOR EXTERNAL CONTRIBUTORS**
> This repository is not designed for traditional C conveniences, casual hacking, or stylistic shortcuts. It is an unapologetic, machine-verifiable manifesto of AI-augmented systems architecture.
>
> If you do not approve of this architecture or cannot find peace with this philosophy, consider leaving this repository for your own sanity.
>
> We do not accept Pull Requests, issues, or unsolicited stylistic refactors attempting to re-introduce `->`, combine multiple database classes into one file, or bypass explicit getters/setters. Upstream is maintained exclusively by the author and the AI agent.

---

## 3. Workspace Constitution & Repo-Local Preferences

All architectural rules and style invariants are governed by the central constitution:

- **[Compressed law list](../../../metadata/gists/preferences/gist.md)** and [published preferences](https://gist.github.com/vex-graph/4132a6c45cb6d3797c3e8eff2e94035a); read only relevant laws with `./tools/prefs`.
- **[darkbase-preferences.md](darkbase-preferences.md)** (repo-local mirror binding darkbase)

Under the Living Documentation Law, update affected contracts in the same cycle.
Universal changes are published to the existing Gist and byte-verified; repo-local
documentation is committed locally under the Git Workflow Law. Never auto-push.
Darkbase owns R3 database semantics/persistence, not R2 allocation/storage.
Either Vexspoke CPU computation/behavior or Relational Engine storage/native C
search public contracts may be borrowed; this blueprint proves no integration.
