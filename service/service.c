#include "service/service.h"
#include "annotation/annotation.h"

#include "database/database.h"
#include "exception/throw.h"
#include "io/file.h"
#include "nio/mem.h"
#include "reflection/struct.h"
#include "security/base32.h"
#include "security/password.h"
#include "security/secure_random.h"
#include "security/totp.h"
#include "time/datetime.h"

#include <stdio.h>
#include <string.h>

#define HEADLESS_ADMIN_NAME "admin"
#define HEADLESS_ADMIN_PASSWORD "Password123!"
#define HEADLESS_ADMIN_SERVICE "vexgraph.darkbase.headless"
#define HEADLESS_ADMIN_ACCOUNT "admin"
#define HEADLESS_ADMIN_SECRET_BYTES 20u
#define HEADLESS_PASSWORD_BYTES 32u
#define HEADLESS_SALT_BYTES 16u

;;DEFINITION
/** HeadlessService is the R4 lifetime and admission owner for the R3 Database.
 * It registers its private admin row before loading the file, owns the Database
 * handle, and keeps every data request closed until the sole admin has logged in
 * with password + TOTP, replaced the one-time bootstrap password, and explicitly
 * enabled requests. The TOTP replay step is persisted through Database_save
 * before login succeeds; a failed save rolls the in-memory step back. Secure
 * TOTP bytes cross only the injected secret-store seam, which must be backed by
 * an OS credential store and has no plaintext fallback. The legacy snapshot is
 * currently the engine's only persistence API; WAL-backed mutation audit and
 * CRUD dispatch are separate unfinished integrations. */
;;OVERVIEW
/** ========================================================================
 * CLASS: HeadlessService (service/service.c)
 * STRUCT FIELDS (Mirroring service/service.c)
 *   Database *database            service-owned R3 store
 *   Struct *adminSchema           service-owned schema, kept alive through DB free
 *   char databasePath[1024]       private file path copied at construction
 *   HeadlessSecretStoreFn store   borrowed secure-secret write callback
 *   HeadlessSecretLoadFn load     borrowed secure-secret read callback
 *   HeadlessSecretRemoveFn remove borrowed secure-secret delete callback
 *   void *context                 borrowed secure-store callback context
 *   bool started                  database and admin record admitted
 *   bool adminAuthenticated       current process-local admin session
 *   bool acceptRequests           explicit runtime admission gate
 *   uint32_t passwordIterations   configured PBKDF2 work factor
 * PRIVATE HELPERS
 *   HeadlessAdminRecord { uint8_t passwordSalt[16]; uint8_t passwordHash[32];
 *     uint32_t iterations; uint32_t passwordChangeRequired;
 *     uint32_t acceptRequestsPreference; uint64_t lastTotpStep; }
 *   persistRecord(self, row, replacement) save with in-memory rollback
 *   adminRecord(self)                  private row lookup
 *   verifyPasswordTotp(...)            cold credential verification
 *   bootstrap(...)                     initial admin + one-time TOTP secret
 *   cleanup(self)                      reverse-order partial-start teardown
 * FUNCTION REGISTRY (exports)
 *   Constructors: HeadlessService_0(), HeadlessService_1(databasePath)
 *   Lifecycle: HeadlessService_start, HeadlessService_free
 *   Authentication: HeadlessService_adminLogin, adminLoginAt,
 *     HeadlessService_changeAdminPassword, changeAdminPasswordAt
 *   Configuration: HeadlessService_setPasswordIterations,
 *     HeadlessService_getPasswordIterations
 *   Admission: HeadlessService_setAcceptRequests, isAcceptingRequests,
 *     isRequestAllowed
 *   Projections: HeadlessService_toString, toStringStruct
 * CONTRACT: all calls are externally serialized. Callback context outlives the
 * service. The database and schema are service-owned; request admission starts
 * closed on every process start. Passwords and TOTP secrets never appear in
 * projections or diagnostics. ======================================================================== */

typedef struct HeadlessAdminRecord {
    uint8_t passwordSalt[HEADLESS_SALT_BYTES]; // PBKDF2 salt
    uint8_t passwordHash[HEADLESS_PASSWORD_BYTES]; // PBKDF2-SHA256 result
    uint32_t iterations; // derivation cost recorded with the hash
    uint32_t passwordChangeRequired; // one-time bootstrap barrier
    uint32_t acceptRequestsPreference; // durable explicit enable preference
    uint64_t lastTotpStep; // durable replay rejection watermark
} HeadlessAdminRecord;

struct HeadlessService {
    Database *database;
    Struct *adminSchema;
    char databasePath[FILE_PATH_MAX];
    HeadlessSecretStoreFn store;
    HeadlessSecretLoadFn load;
    HeadlessSecretRemoveFn remove;
    ;;WHAT("void")
    void *context;
    bool started;
    bool adminAuthenticated;
    bool acceptRequests;
    uint32_t passwordIterations;
};

static bool persistRecord(HeadlessService *self, HeadlessAdminRecord *record,
                          const HeadlessAdminRecord *replacement) {
    HeadlessAdminRecord previous = *record;
    *record = *replacement;
    if (Database_save((*self).database, (*self).databasePath) == DATABASE_OK)
        return true;
    *record = previous;
    return false;
}

static HeadlessAdminRecord *adminRecord(HeadlessService *self) {
    if (self == nullptr || !(*self).started || (*self).database == nullptr)
        return nullptr;
    return (HeadlessAdminRecord*) Database_row((*self).database, HEADLESS_ADMIN_NAME, 0u);
}

static bool verifyPasswordTotp(HeadlessService *self, const char *password,
                               uint32_t code, uint64_t unixTimeSeconds,
                               HeadlessAdminRecord *record, uint64_t *outAcceptedStep) {
    if (self == nullptr || password == nullptr || record == nullptr || outAcceptedStep == nullptr)
        return false;
    uint8_t secret[HEADLESS_ADMIN_SECRET_BYTES];
    size_t secretLength = 0u;
    if ((*self).load == nullptr || !(*self).load((*self).context, HEADLESS_ADMIN_SERVICE,
            HEADLESS_ADMIN_ACCOUNT, secret, sizeof secret, &secretLength) ||
        secretLength != sizeof secret) {
        memset(secret, 0, sizeof secret);
        return false;
    }
    bool passwordValid = Password_verifySha256(password, (*record).passwordSalt,
        sizeof (*record).passwordSalt, (*record).iterations, (*record).passwordHash,
        sizeof (*record).passwordHash);
    uint64_t accepted = (*record).lastTotpStep;
    bool totpValid = Totp_verify(secret, secretLength, code, unixTimeSeconds,
        TOTP_DIGITS_DEFAULT, TOTP_STEP_SECONDS_DEFAULT, TOTP_WINDOW_DEFAULT, &accepted);
    memset(secret, 0, sizeof secret);
    if (!passwordValid || !totpValid)
        return false;
    *outAcceptedStep = accepted;
    return true;
}

static void cleanup(HeadlessService *self) {
    if (self == nullptr)
        return;
    (*self).acceptRequests = false;
    (*self).adminAuthenticated = false;
    (*self).started = false;
    if ((*self).database != nullptr) {
        Database_free((*self).database);
        (*self).database = nullptr;
    }
    if ((*self).adminSchema != nullptr) {
        Struct_free((*self).adminSchema);
        (*self).adminSchema = nullptr;
    }
}

static bool bootstrap(HeadlessService *self, char *outSetupSecret,
                      size_t secretCapacity, size_t *outSecretLength) {
    if (outSecretLength != nullptr)
        *outSecretLength = 0u;
    if (outSetupSecret == nullptr || outSecretLength == nullptr ||
        secretCapacity < Base32_encodedLength(HEADLESS_ADMIN_SECRET_BYTES) + 1u ||
        (*self).store == nullptr || (*self).remove == nullptr) {
        THROW("HeadlessService_start: bootstrap output or secret-store contract invalid");
        return false;
    }

    uint8_t secret[HEADLESS_ADMIN_SECRET_BYTES];
    uint8_t salt[HEADLESS_SALT_BYTES];
    char encoded[40];
    bool truncated = false;
    SecureRandom *random = SecureRandom_0();
    bool generated = random != nullptr && SecureRandom_bytes(random, secret, sizeof secret) &&
        Password_generateSalt(salt, sizeof salt);
    SecureRandom_free(random);
    if (!generated || !Base32_encode(secret, sizeof secret, encoded, sizeof encoded, &truncated)) {
        memset(secret, 0, sizeof secret);
        memset(salt, 0, sizeof salt);
        memset(encoded, 0, sizeof encoded);
        THROW("HeadlessService_start: secure bootstrap material unavailable");
        return false;
    }

    HeadlessAdminRecord record = {0};
    memcpy(record.passwordSalt, salt, sizeof salt);
    record.iterations = (*self).passwordIterations;
    record.passwordChangeRequired = 1u;
    if (!Password_deriveSha256(HEADLESS_ADMIN_PASSWORD, record.passwordSalt,
            sizeof record.passwordSalt, record.iterations, record.passwordHash,
            sizeof record.passwordHash)) {
        memset(secret, 0, sizeof secret);
        memset(salt, 0, sizeof salt);
        memset(encoded, 0, sizeof encoded);
        memset(&record, 0, sizeof record);
        THROW("HeadlessService_start: bootstrap password derivation failed");
        return false;
    }
    if (!(*self).store((*self).context, HEADLESS_ADMIN_SERVICE, HEADLESS_ADMIN_ACCOUNT,
                       secret, sizeof secret)) {
        memset(secret, 0, sizeof secret);
        memset(salt, 0, sizeof salt);
        memset(encoded, 0, sizeof encoded);
        memset(&record, 0, sizeof record);
        THROW("HeadlessService_start: secure secret store refused bootstrap TOTP");
        return false;
    }
    memset(secret, 0, sizeof secret);
    if (Database_insertOwned((*self).database, HEADLESS_ADMIN_NAME, &record,
                             sizeof record) != 0 ||
        Database_save((*self).database, (*self).databasePath) != DATABASE_OK) {
        if (!(*self).remove((*self).context, HEADLESS_ADMIN_SERVICE,
                            HEADLESS_ADMIN_ACCOUNT))
            THROW("HeadlessService_start: unresolved bootstrap secret cleanup failure");
        memset(salt, 0, sizeof salt);
        memset(encoded, 0, sizeof encoded);
        memset(&record, 0, sizeof record);
        THROW("HeadlessService_start: initial database publication failed");
        return false;
    }
    size_t encodedLength = strlen(encoded);
    memcpy(outSetupSecret, encoded, encodedLength + 1u);
    *outSecretLength = encodedLength;
    memset(encoded, 0, sizeof encoded);
    memset(salt, 0, sizeof salt);
    memset(&record, 0, sizeof record);
    return true;
}

HeadlessService *HeadlessService_0(void) {
    HeadlessService *self = (HeadlessService*) Memory_alloc(0u, sizeof(HeadlessService));
    if (self == nullptr)
        return nullptr;
    memset(self, 0, sizeof *self);
    (*self).passwordIterations = PASSWORD_ITERATIONS_DEFAULT;
    return self;
}

HeadlessService *HeadlessService_1(const char *databasePath) {
    if (databasePath == nullptr || strlen(databasePath) == 0u ||
        strlen(databasePath) >= FILE_PATH_MAX) {
        THROW("HeadlessService: invalid database path");
        return nullptr;
    }
    HeadlessService *self = HeadlessService_0();
    if (self == nullptr)
        return nullptr;
    memcpy((*self).databasePath, databasePath, strlen(databasePath) + 1u);
    return self;
}

void HeadlessService_free(HeadlessService *self) {
    if (self == nullptr)
        return;
    cleanup(self);
    Memory_free(self);
}

bool HeadlessService_start(HeadlessService *self, HeadlessSecretStoreFn store,
                           HeadlessSecretLoadFn load, HeadlessSecretRemoveFn remove,
                            void *context, char *outSetupSecret, size_t secretCapacity,
                           size_t *outSecretLength) {
    if (outSecretLength != nullptr)
        *outSecretLength = 0u;
    if (self == nullptr || (*self).started || (*self).databasePath[0] == '\0' ||
        store == nullptr || load == nullptr || remove == nullptr) {
        THROW("HeadlessService_start: invalid or already-started service");
        return false;
    }
    (*self).store = store;
    (*self).load = load;
    (*self).remove = remove;
    (*self).context = context;
    (*self).adminSchema = Struct(HEADLESS_ADMIN_NAME);
    if ((*self).adminSchema == nullptr) {
        cleanup(self);
        THROW("HeadlessService_start: admin schema allocation failed");
        return false;
    }
    Struct_setSize((*self).adminSchema, (uint32_t) sizeof(HeadlessAdminRecord));
    (*self).database = Database();
    if ((*self).database == nullptr || Database_define((*self).database,
            (*self).adminSchema) < 0) {
        cleanup(self);
        THROW("HeadlessService_start: database/schema initialization failed");
        return false;
    }
    // Enable private lookups while startup validates or creates the admin row.
    (*self).started = true;
    bool exists = File_exists((*self).databasePath);
    if (exists) {
        if (Database_load((*self).database, (*self).databasePath) != DATABASE_OK ||
            Database_count((*self).database, HEADLESS_ADMIN_NAME) != 1u) {
            cleanup(self);
            THROW("HeadlessService_start: existing database rejected or lacks admin record");
            return false;
        }
        uint8_t secret[HEADLESS_ADMIN_SECRET_BYTES];
        size_t secretLength = 0u;
        bool loaded = load(context, HEADLESS_ADMIN_SERVICE, HEADLESS_ADMIN_ACCOUNT,
                           secret, sizeof secret, &secretLength);
        memset(secret, 0, sizeof secret);
        if (!loaded || secretLength != sizeof secret) {
            cleanup(self);
            THROW("HeadlessService_start: admin TOTP secret unavailable");
            return false;
        }
    } else if (!bootstrap(self, outSetupSecret, secretCapacity, outSecretLength)) {
        cleanup(self);
        return false;
    }
    (*self).adminAuthenticated = false;
    (*self).acceptRequests = false;
    return true;
}

bool HeadlessService_adminLoginAt(HeadlessService *self, const char *username,
                                  const char *password, uint32_t totpCode,
                                  uint64_t unixTimeSeconds) {
    HeadlessAdminRecord *record = adminRecord(self);
    if (record == nullptr || username == nullptr || strcmp(username, HEADLESS_ADMIN_NAME) != 0) {
        THROW("HeadlessService_adminLogin: unknown admin or service not started");
        return false;
    }
    uint64_t accepted = 0u;
    if (!verifyPasswordTotp(self, password, totpCode, unixTimeSeconds, record, &accepted)) {
        THROW("HeadlessService_adminLogin: credentials rejected");
        return false;
    }
    HeadlessAdminRecord replacement = *record;
    replacement.lastTotpStep = accepted;
    if (!persistRecord(self, record, &replacement)) {
        THROW("HeadlessService_adminLogin: replay watermark was not persisted");
        return false;
    }
    (*self).adminAuthenticated = true;
    return true;
}

bool HeadlessService_adminLogin(HeadlessService *self, const char *username,
                               const char *password, uint32_t totpCode) {
    DateTime now = {0};
    DateTime_set(&now);
    int64_t millis = DateTime_epochMillis(&now);
    if (millis < 0)
        return false;
    return HeadlessService_adminLoginAt(self, username, password, totpCode,
                                         (uint64_t) millis / 1000u);
}

bool HeadlessService_changeAdminPasswordAt(HeadlessService *self,
                                           const char *currentPassword,
                                           const char *newPassword, uint32_t totpCode,
                                           uint64_t unixTimeSeconds) {
    HeadlessAdminRecord *record = adminRecord(self);
    if (record == nullptr || !(*self).adminAuthenticated || currentPassword == nullptr ||
        newPassword == nullptr || strlen(newPassword) < 12u) {
        THROW("HeadlessService_changeAdminPassword: invalid state or password policy");
        return false;
    }
    uint64_t accepted = 0u;
    if (!verifyPasswordTotp(self, currentPassword, totpCode, unixTimeSeconds,
                            record, &accepted)) {
        THROW("HeadlessService_changeAdminPassword: credentials rejected");
        return false;
    }
    HeadlessAdminRecord replacement = *record;
    if (!Password_generateSalt(replacement.passwordSalt,
                               sizeof replacement.passwordSalt) ||
        !Password_deriveSha256(newPassword, replacement.passwordSalt,
            sizeof replacement.passwordSalt, (*self).passwordIterations,
            replacement.passwordHash, sizeof replacement.passwordHash)) {
        memset(&replacement, 0, sizeof replacement);
        THROW("HeadlessService_changeAdminPassword: password derivation failed");
        return false;
    }
    replacement.iterations = (*self).passwordIterations;
    replacement.passwordChangeRequired = 0u;
    replacement.lastTotpStep = accepted;
    bool persisted = persistRecord(self, record, &replacement);
    memset(&replacement, 0, sizeof replacement);
    if (!persisted) {
        THROW("HeadlessService_changeAdminPassword: persistence failed");
        return false;
    }
    return true;
}

bool HeadlessService_changeAdminPassword(HeadlessService *self,
                                        const char *currentPassword,
                                        const char *newPassword, uint32_t totpCode) {
    DateTime now = {0};
    DateTime_set(&now);
    int64_t millis = DateTime_epochMillis(&now);
    if (millis < 0)
        return false;
    return HeadlessService_changeAdminPasswordAt(self, currentPassword, newPassword,
        totpCode, (uint64_t) millis / 1000u);
}

bool HeadlessService_setPasswordIterations(HeadlessService *self, uint32_t iterations) {
    if (self == nullptr || (*self).started || iterations < PASSWORD_ITERATIONS_MIN) {
        THROW("HeadlessService_setPasswordIterations: invalid service state or below policy minimum");
        return false;
    }
    (*self).passwordIterations = iterations;
    return true;
}

uint32_t HeadlessService_getPasswordIterations(const HeadlessService *self) {
    return self == nullptr ? 0u : (*self).passwordIterations;
}

bool HeadlessService_setAcceptRequests(HeadlessService *self, bool accept) {
    HeadlessAdminRecord *record = adminRecord(self);
    if (record == nullptr) {
        THROW("HeadlessService_setAcceptRequests: service not started");
        return false;
    }
    if (accept && (!(*self).adminAuthenticated || (*record).passwordChangeRequired != 0u)) {
        THROW("HeadlessService_setAcceptRequests: admin login and password change required");
        return false;
    }
    if (!accept)
        (*self).acceptRequests = false;
    if (accept == ((*record).acceptRequestsPreference != 0u)) {
        (*self).acceptRequests = accept;
        return true;
    }
    HeadlessAdminRecord replacement = *record;
    replacement.acceptRequestsPreference = accept ? 1u : 0u;
    if (!persistRecord(self, record, &replacement)) {
        THROW("HeadlessService_setAcceptRequests: gate state persistence failed");
        return false;
    }
    (*self).acceptRequests = accept;
    return true;
}

bool HeadlessService_isAcceptingRequests(const HeadlessService *self) {
    return self != nullptr && (*self).started && (*self).acceptRequests;
}

bool HeadlessService_isRequestAllowed(const HeadlessService *self) {
    return HeadlessService_isAcceptingRequests(self);
}

bool HeadlessService_toString(const HeadlessService *self, char *dest, size_t cap,
                              bool *outTruncated) {
    if (outTruncated != nullptr)
        *outTruncated = false;
    if (dest == nullptr || cap == 0u)
        return false;
    const char *value = self == nullptr ? "nullptr" :
        ((*self).started ? "HeadlessService{started}" : "HeadlessService{closed}");
    size_t length = strlen(value);
    size_t copy = length < cap - 1u ? length : cap - 1u;
    memcpy(dest, value, copy);
    dest[copy] = '\0';
    if (outTruncated != nullptr)
        *outTruncated = copy != length;
    return copy == length;
}

bool HeadlessService_toStringStruct(const HeadlessService *self, char *dest, size_t cap,
                                    bool *outTruncated) {
    if (outTruncated != nullptr)
        *outTruncated = false;
    if (dest == nullptr || cap == 0u)
        return false;
    int result = self == nullptr ? snprintf(dest, cap, "nullptr") : snprintf(dest, cap,
        "HeadlessService{database:%s,adminSchema:%s,databasePathLength:%zu,store:%s,load:%s,remove:%s,context:%s,started:%s,adminAuthenticated:%s,acceptRequests:%s,passwordIterations:%u}",
        (*self).database == nullptr ? "nullptr" : "Database",
        (*self).adminSchema == nullptr ? "nullptr" : "Struct",
        strlen((*self).databasePath), (*self).store == nullptr ? "nullptr" : "function",
        (*self).load == nullptr ? "nullptr" : "function",
        (*self).remove == nullptr ? "nullptr" : "function",
        (*self).context == nullptr ? "nullptr" : "present",
        (*self).started ? "true" : "false",
        (*self).adminAuthenticated ? "true" : "false",
        (*self).acceptRequests ? "true" : "false", (*self).passwordIterations);
    bool ok = result >= 0 && (size_t) result < cap;
    if (!ok && outTruncated != nullptr)
        *outTruncated = true;
    if (!ok)
        dest[cap - 1u] = '\0';
    return ok;
}
