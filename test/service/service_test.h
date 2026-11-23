#ifndef DARKBASE_TEST_SERVICE_TEST_H
#define DARKBASE_TEST_SERVICE_TEST_H

#include "vex.h"

/**
 * In-memory secret store fixture for HeadlessService tests.
 * @field secret Stored 20-byte TOTP secret.
 * @field present True when a secret has been stored.
 */
typedef struct TestSecretContext {
    u8 secret[20];
    bool present;
} TestSecretContext;

#endif
