#ifndef DARKBASE_HEADLESS_SERVICE_SERVICE_H
#define DARKBASE_HEADLESS_SERVICE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "c23/constructor.h"

typedef struct HeadlessService HeadlessService;

typedef bool (*HeadlessSecretStoreFn)(void *context, const char *service,
                                      const char *account, const uint8_t *secret,
                                      size_t secretLength);
typedef bool (*HeadlessSecretLoadFn)(void *context, const char *service,
                                     const char *account, uint8_t *dest,
                                     size_t capacity, size_t *outLength);
typedef bool (*HeadlessSecretRemoveFn)(void *context, const char *service,
                                       const char *account);

HeadlessService *HeadlessService_0(void);
HeadlessService *HeadlessService_1(const char *databasePath);
#define HeadlessService(...) CONSTRUCTOR_DISPATCH(HeadlessService, __VA_ARGS__)
#define HeadlessService_zero() HeadlessService_0()

void HeadlessService_free(HeadlessService *self);

// Opens the service-owned database, registering its private admin row schema
// before load. A missing file bootstraps the one-time admin/Password123! account
// and returns its base32 TOTP secret exactly once through outSetupSecret.
// Secret callbacks must use secure OS storage (Keychain on Apple); no plaintext
// fallback is permitted. The callbacks and context are borrowed until free.
bool HeadlessService_start(HeadlessService *self, HeadlessSecretStoreFn store,
                           HeadlessSecretLoadFn load, HeadlessSecretRemoveFn remove,
                            void *context, char *outSetupSecret,
                            size_t secretCapacity,
                           size_t *outSecretLength);

// Authenticate the sole admin with password and current six-digit TOTP.
// Successful TOTP steps are durably saved before this returns true.
bool HeadlessService_adminLogin(HeadlessService *self, const char *username,
                               const char *password, uint32_t totpCode);
// Deterministic clock seam for owner tests; production uses adminLogin.
bool HeadlessService_adminLoginAt(HeadlessService *self, const char *username,
                                  const char *password, uint32_t totpCode,
                                  uint64_t unixTimeSeconds);

// Change the bootstrap password after login. The new password and cleared
// forced-change state are persisted before success is returned.
bool HeadlessService_changeAdminPassword(HeadlessService *self,
                                        const char *currentPassword,
                                        const char *newPassword, uint32_t totpCode);
bool HeadlessService_changeAdminPasswordAt(HeadlessService *self,
                                           const char *currentPassword,
                                           const char *newPassword, uint32_t totpCode,
                                           uint64_t unixTimeSeconds);

// Configure the PBKDF2 work factor before start. Values below the R2 policy
// minimum reject; the default is PASSWORD_ITERATIONS_DEFAULT.
bool HeadlessService_setPasswordIterations(HeadlessService *self, uint32_t iterations);
uint32_t HeadlessService_getPasswordIterations(const HeadlessService *self);

// Enabling requires an authenticated admin and completed bootstrap password
// change. Enable is persisted before the in-memory request gate opens; disable
// closes the gate immediately and then persists the disabled state.
bool HeadlessService_setAcceptRequests(HeadlessService *self, bool accept);
bool HeadlessService_isAcceptingRequests(const HeadlessService *self);
bool HeadlessService_isRequestAllowed(const HeadlessService *self);

bool HeadlessService_toString(const HeadlessService *self, char *dest, size_t cap,
                              bool *outTruncated);
bool HeadlessService_toStringStruct(const HeadlessService *self, char *dest, size_t cap,
                                    bool *outTruncated);

#endif
