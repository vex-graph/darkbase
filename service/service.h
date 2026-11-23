#ifndef DARKBASE_HEADLESS_SERVICE_SERVICE_H
#define DARKBASE_HEADLESS_SERVICE_SERVICE_H

#include "vex.h"

#define HEADLESS_PASSWORD_BYTES 32u
#define HEADLESS_SALT_BYTES 16u

typedef struct HeadlessService HeadlessService;

/**
 * Persisted admin credential and admission state record stored in darkbase.
 *
 * Fields:
 *   passwordSalt             : PBKDF2 salt bytes
 *   passwordHash             : PBKDF2-SHA256 derived password hash
 *   iterations               : PBKDF2 derivation work factor
 *   passwordChangeRequired   : non-zero until bootstrap password is replaced
 *   acceptRequestsPreference : durable request-admission gate preference
 *   lastTotpStep             : durable TOTP replay-rejection watermark
 */
typedef struct HeadlessAdminRecord {
    u8 passwordSalt[HEADLESS_SALT_BYTES];
    u8 passwordHash[HEADLESS_PASSWORD_BYTES];
    u32 iterations;
    u32 passwordChangeRequired;
    u32 acceptRequestsPreference;
    u64 lastTotpStep;
} HeadlessAdminRecord;

typedef bool (*HeadlessSecretStoreFn)(any context, string service,
                                      string account, const u8 *secret,
                                      bytes secretLength);
typedef bool (*HeadlessSecretLoadFn)(any context, string service,
                                     string account, u8 *dest,
                                     bytes capacity, bytes *outLength);
typedef bool (*HeadlessSecretRemoveFn)(any context, string service,
                                       string account);

HeadlessService *HeadlessService_0();
HeadlessService *HeadlessService_1(string databasePath);
#define HeadlessService(...) CONSTRUCTOR_DISPATCH(HeadlessService, __VA_ARGS__)
#define HeadlessService_zero() HeadlessService_0()

fn HeadlessService_free(HeadlessService *self);

/**
 * Opens the service-owned database, registering its private admin row schema
 * before load. A missing file bootstraps the one-time admin/Password123! account
 * and returns its base32 TOTP secret exactly once through outSetupSecret.
 * Secret callbacks must use secure OS storage (Keychain on Apple); no plaintext
 * fallback is permitted. The callbacks and context are borrowed until free.
 */
bool HeadlessService_start(HeadlessService *self, HeadlessSecretStoreFn store,
                           HeadlessSecretLoadFn load, HeadlessSecretRemoveFn remove,
                            any context, utf8 *outSetupSecret,
                            bytes secretCapacity,
                           bytes *outSecretLength);

/**
 * Authenticate the sole admin with password and current six-digit TOTP.
 * Successful TOTP steps are durably saved before this returns true.
 */
bool HeadlessService_adminLogin(HeadlessService *self, string username,
                               string password, u32 totpCode);
/** Deterministic clock seam for owner tests; production uses adminLogin. */
bool HeadlessService_adminLoginAt(HeadlessService *self, string username,
                                  string password, u32 totpCode,
                                  u64 unixTimeSeconds);

/**
 * Change the bootstrap password after login. The new password and cleared
 * forced-change state are persisted before success is returned.
 */
bool HeadlessService_changeAdminPassword(HeadlessService *self,
                                        string currentPassword,
                                        string newPassword, u32 totpCode);
bool HeadlessService_changeAdminPasswordAt(HeadlessService *self,
                                           string currentPassword,
                                           string newPassword, u32 totpCode,
                                           u64 unixTimeSeconds);

/**
 * Configure the PBKDF2 work factor before start. Values below the R2 policy
 * minimum reject; the default is PASSWORD_ITERATIONS_DEFAULT.
 */
bool HeadlessService_setPasswordIterations(HeadlessService *self, u32 iterations);
u32 HeadlessService_getPasswordIterations(const HeadlessService *self);

/**
 * Enabling requires an authenticated admin and completed bootstrap password
 * change. Enable is persisted before the in-memory request gate opens; disable
 * closes the gate immediately and then persists the disabled state.
 */
bool HeadlessService_setAcceptRequests(HeadlessService *self, bool accept);
bool HeadlessService_isAcceptingRequests(const HeadlessService *self);
bool HeadlessService_isRequestAllowed(const HeadlessService *self);

bool HeadlessService_toString(const HeadlessService *self, utf8 *dest, bytes cap,
                              bool *outTruncated);
bool HeadlessService_toStringStruct(const HeadlessService *self, utf8 *dest, bytes cap,
                                    bool *outTruncated);

#endif
