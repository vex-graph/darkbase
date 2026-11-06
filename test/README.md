# darkbase-engine/test

Owner-test partition for `../../suite/ecosystem/darkbase-engine`, mirroring its production
source tree (the Test Tree Mirror Law). A production unit at
`../../suite/ecosystem/darkbase-engine/src/<dir>/<unit>.c` owns
`darkbase-engine/test/<dir>/<unit>_test.c`.

Current owner tests:
- `database/database_test.c` — L2 `Database` entity registry and live-row binding.
- `database/database_result_test.c` — the dest-last `DatabaseResult` cursor.
- `page/page_test.c` — sealed frame layout, stored-byte validation and `Page_load`.
- `wal/wal_test.c` — standalone and embedded WAL, commit marker, torn prefix,
  bounds/exhaustion and rejection preservation.
- `util/crc32_test.c` — standard CRC32 and incremental equivalence.
- `store/store_test.c` — one-file real mapping/WAL/Page integration, live full
  pages, release/reuse/reopen, both capacity bounds, corruption and child-exit
  recovery. The independent runner additionally injects five commit-stage IO failures.

Run through the workspace `b` graph: `./tools/b test database`.

Independent durable owners: `python3 darkbase-engine/test/store/run.py`.
This builds fresh Rust/native dependencies and executes Page/Wal/Store optimized
and C ASan/UBSan under bounded subprocess watchdogs. No cached native archive is
used. Rust sanitizer instrumentation, real power-loss and other-platform proof
remain gaps; the installed Rust standard library warns about macOS 27 versus
the 14.0 C deployment target. The graph currently stops at a workspace-wide
post-migration ratchet failure before these targets run. No baseline is weakened.

No durable relational Database adapter, catalog, indexes, embedded checkpoint
recycling, multi-page transactions or reactive-program proof is implied.
