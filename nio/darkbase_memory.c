#include "vex.h"
#include "nio/darkbase_memory.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: DarkbaseMemory (darkbase/nio/darkbase_memory.c)
 * ============================================================================
 * Per-library MemoryArena owner for Darkbase, backed by Relational Engine's
 * MemoryPool. Starts at zero OS pages and grows on demand until OS OOM.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * MODULE: DarkbaseMemory (nio/darkbase_memory)
 * ============================================================================
 * Lazily creates and shuts down Darkbase's dedicated MemoryArena.
 * ============================================================================
 */

static MemoryArena *s_darkbaseArena = nullptr;

/** Lazily create and return Darkbase's page-pooled MemoryArena. */
MemoryArena *Darkbase_arena() {
    if (s_darkbaseArena == nullptr)
        s_darkbaseArena = MemoryArena_createPages(0);
    return s_darkbaseArena;
}

/** Destroy Darkbase's MemoryArena and return all pages to MemoryPool. */
fn Darkbase_arenaShutdown() {
    if (s_darkbaseArena != nullptr) {
        MemoryArena_destroy(s_darkbaseArena);
        s_darkbaseArena = nullptr;
    }
}
