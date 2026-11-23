#ifndef DARKBASE_TEST_DATABASE_TEST_H
#define DARKBASE_TEST_DATABASE_TEST_H

#include "vex.h"

/**
 * Minimal row fixture for DatabaseResult tests.
 * @field v Test payload value.
 */
typedef struct Row2 {
    u32 v;
} Row2;

/**
 * Commit observer probe used to trigger a controlled crash at the root redo boundary.
 * @field commits Count of observed commit callbacks.
 */
typedef struct DatabaseCrashProbe {
    u32 commits;
} DatabaseCrashProbe;

/**
 * Reflected row fixture for Database tests.
 * @field id Row identifier.
 * @field health Signed integer health value for range and index queries.
 */
typedef struct Row {
    pointer id;
    i32 health;
} Row;

#endif
