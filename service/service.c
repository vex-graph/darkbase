#include "vex.h"
#include "service/service.h"

#include "database/database.h"
#include "io/file/file.h"
#include "nio/mem.h"
#include "reflection/struct.h"
#include "security/base32.h"
#include "security/password.h"
#include "security/secure_random.h"
#include "security/totp.h"
#include "time/datetime.h"

#include <stdio.h>
#include <string.h>
#include "re/primitive/char_sequence.h"
#include "nio/darkbase_memory.h"

#define HEADLESS_ADMIN_NAME "admin"
#define HEADLESS_ADMIN_PASSWORD "Password123!"
#define HEADLESS_ADMIN_SERVICE "vexgraph.darkbase.headless"
#define HEADLESS_ADMIN_ACCOUNT "admin"
#define HEADLESS_ADMIN_SECRET_BYTES 20u

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
 *   utf8 databasePath[1024]       private file path copied at construction
 *   HeadlessSecretStoreFn store   borrowed secure-secret write callback
 *   HeadlessSecretLoadFn load     borrowed secure-secret read callback
 *   HeadlessSecretRemoveFn remove borrowed secure-secret delete callback
 *   any context                 borrowed secure-store callback context
 *   bool started                  database and admin record admitted
 *   bool adminAuthenticated       current process-local admin session
 *   bool acceptRequests           explicit runtime admission gate
 *   u32 passwordIterations   configured PBKDF2 work factor
 * PRIVATE HELPERS
 *   HeadlessAdminRecord { u8 passwordSalt[16]; u8 passwordHash[32];
 *     u32 iterations; u32 passwordChangeRequired;
 *     u32 acceptRequestsPreference; u64 lastTotpStep; }
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


struct HeadlessService {
    Database *database;
    Struct *adminSchema;
    utf8 databasePath[FILE_PATH_MAX];
    HeadlessSecretStoreFn store;
    HeadlessSecretLoadFn load;
    HeadlessSecretRemoveFn remove;
    ;;WHAT("fn")
    any context;
    bool started;
    bool adminAuthenticated;
    bool acceptRequests;
    u32 passwordIterations;
};

;;GETTER
static inline const u8 *HeadlessAdminRecord_passwordSalt(const HeadlessAdminRecord *r)
{
    return (*r).passwordSalt;
}
;;GETTER
static inline const u8 *HeadlessAdminRecord_passwordHash(const HeadlessAdminRecord *r)
{
    return (*r).passwordHash;
}
;;GETTER
static inline u32 HeadlessAdminRecord_iterations(const HeadlessAdminRecord *r)
{
    return (*r).iterations;
}
;;GETTER
static inline u32 HeadlessAdminRecord_passwordChangeRequired(const HeadlessAdminRecord *r) {
    return (*r).passwordChangeRequired;
}
;;GETTER
static inline u32 HeadlessAdminRecord_acceptRequestsPreference(const HeadlessAdminRecord *r) {
    return (*r).acceptRequestsPreference;
}
;;GETTER
static inline u64 HeadlessAdminRecord_lastTotpStep(const HeadlessAdminRecord *r)
{
    return (*r).lastTotpStep;
}
;;GETTER
static inline Database *HeadlessService_database(const HeadlessService *self)
{
    return (*self).database;
}
;;SETTER
static inline fn HeadlessService_setDatabase(HeadlessService *self, Database *db)
{
    (*self).database = db;
}
;;GETTER
static inline Struct *HeadlessService_adminSchema(const HeadlessService *self)
{
    return (*self).adminSchema;
}
;;SETTER
static inline fn HeadlessService_setAdminSchema(HeadlessService *self, Struct *s)
{
    (*self).adminSchema = s;
}
;;GETTER
static inline const utf8 *HeadlessService_databasePath(const HeadlessService *self)
{
    return (*self).databasePath;
}
;;GETTER
static inline utf8 *HeadlessService_databasePathMut(HeadlessService *self)
{
    return (*self).databasePath;
}
;;GETTER
static inline HeadlessSecretStoreFn HeadlessService_store(const HeadlessService *self)
{
    return (*self).store;
}
;;SETTER
static inline fn HeadlessService_setStore(HeadlessService *self, HeadlessSecretStoreFn s)
{
    (*self).store = s;
}
;;GETTER
static inline HeadlessSecretLoadFn HeadlessService_load(const HeadlessService *self)
{
    return (*self).load;
}
;;SETTER
static inline fn HeadlessService_setLoad(HeadlessService *self, HeadlessSecretLoadFn l)
{
    (*self).load = l;
}
;;GETTER
static inline HeadlessSecretRemoveFn HeadlessService_remove(const HeadlessService *self)
{
    return (*self).remove;
}
;;SETTER
static inline fn HeadlessService_setRemove(HeadlessService *self, HeadlessSecretRemoveFn r)
{
    (*self).remove = r;
}
;;GETTER
static inline any HeadlessService_context(const HeadlessService *self)
{
    return (*self).context;
}
;;SETTER
static inline fn HeadlessService_setContext(HeadlessService *self, any c)
{
    (*self).context = c;
}
;;GETTER
static inline bool HeadlessService_started(const HeadlessService *self)
{
    return (*self).started;
}
;;SETTER
static inline fn HeadlessService_setStarted(HeadlessService *self, bool s)
{
    (*self).started = s;
}
;;GETTER
static inline bool HeadlessService_adminAuthenticated(const HeadlessService *self) {
    return (*self).adminAuthenticated;
}
;;SETTER
static inline fn HeadlessService_setAdminAuthenticated(HeadlessService *self, bool a) {
    (*self).adminAuthenticated = a;
}
;;GETTER
static inline bool HeadlessService_acceptRequests(const HeadlessService *self)
{
    return (*self).acceptRequests;
}
;;SETTER
static inline fn HeadlessService_setAcceptRequestsField(HeadlessService *self, bool a)
{
    (*self).acceptRequests = a;
}
;;GETTER
static inline u32 HeadlessService_passwordIterations(const HeadlessService *self) {
    return (*self).passwordIterations;
}
;;SETTER
static inline fn HeadlessService_setPasswordIterationsField(HeadlessService *self, u32 it) {
    (*self).passwordIterations = it;
}

static bool persistRecord(HeadlessService *self, HeadlessAdminRecord *record,
                          const HeadlessAdminRecord *replacement) {
    HeadlessAdminRecord previous = *record;
    *record = *replacement;
    if (Database_save(HeadlessService_database(self), HeadlessService_databasePath(self)) == DATABASE_OK)
        return true;
    *record = previous;
    return false;
}

static HeadlessAdminRecord *adminRecord(HeadlessService *self) {
    if (self == nullptr || !HeadlessService_started(self) || HeadlessService_database(self) == nullptr)
        return nullptr;
    return (HeadlessAdminRecord*) Database_row(HeadlessService_database(self), HEADLESS_ADMIN_NAME, 0u);
}

static bool verifyPasswordTotp(HeadlessService *self, const utf8 *password,
                               u32 code, u64 unixTimeSeconds,
                               HeadlessAdminRecord *record, u64 *outAcceptedStep) {
    if (self == nullptr || password == nullptr || record == nullptr || outAcceptedStep == nullptr)
        return false;
    u8 secret[HEADLESS_ADMIN_SECRET_BYTES];
    bytes secretLength = 0u;
    HeadlessSecretLoadFn loadFn = HeadlessService_load(self);
    if (loadFn == nullptr || !loadFn(HeadlessService_context(self), HEADLESS_ADMIN_SERVICE,
            HEADLESS_ADMIN_ACCOUNT, secret, sizeof secret, &secretLength) ||
        secretLength != sizeof secret) {
        memset(secret, 0, sizeof secret);
        return false;
    }
    bool passwordValid = Password_verifySha256(password, HeadlessAdminRecord_passwordSalt(record),
        sizeof(((HeadlessAdminRecord) {0}).passwordSalt), HeadlessAdminRecord_iterations(record),
        HeadlessAdminRecord_passwordHash(record), sizeof(((HeadlessAdminRecord) {0}).passwordHash));
    u64 accepted = HeadlessAdminRecord_lastTotpStep(record);
    bool totpValid = Totp_verify(secret, secretLength, code, unixTimeSeconds,
        TOTP_DIGITS_DEFAULT, TOTP_STEP_SECONDS_DEFAULT, TOTP_WINDOW_DEFAULT, &accepted);
    memset(secret, 0, sizeof secret);
    if (!passwordValid || !totpValid)
        return false;
    *outAcceptedStep = accepted;
    return true;
}

static fn cleanup(HeadlessService *self) {
    if (self == nullptr)
        return;
    HeadlessService_setAcceptRequestsField(self, false);
    HeadlessService_setAdminAuthenticated(self, false);
    HeadlessService_setStarted(self, false);
    Database *db = HeadlessService_database(self);
    if (db != nullptr) {
        Database_free(db);
        HeadlessService_setDatabase(self, nullptr);
    }
    Struct *schema = HeadlessService_adminSchema(self);
    if (schema != nullptr) {
        Struct_free(schema);
        HeadlessService_setAdminSchema(self, nullptr);
    }
}

static bool bootstrap(HeadlessService *self, utf8 *outSetupSecret,
                      bytes secretCapacity, bytes *outSecretLength) {
    if (outSecretLength != nullptr)
        *outSecretLength = 0u;
    HeadlessSecretStoreFn storeFn = HeadlessService_store(self);
    HeadlessSecretRemoveFn removeFn = HeadlessService_remove(self);
    if (outSetupSecret == nullptr || outSecretLength == nullptr ||
        secretCapacity < Base32_encodedLength(HEADLESS_ADMIN_SECRET_BYTES) + 1u ||
        storeFn == nullptr || removeFn == nullptr) {
        THROW("HeadlessService_start: bootstrap output or secret-store contract invalid");
        return false;
    }

    u8 secret[HEADLESS_ADMIN_SECRET_BYTES];
    u8 salt[HEADLESS_SALT_BYTES];
    utf8 encoded[40];
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
    record.iterations = HeadlessService_passwordIterations(self);
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
    if (!storeFn(HeadlessService_context(self), HEADLESS_ADMIN_SERVICE, HEADLESS_ADMIN_ACCOUNT,
                 secret, sizeof secret)) {
        memset(secret, 0, sizeof secret);
        memset(salt, 0, sizeof salt);
        memset(encoded, 0, sizeof encoded);
        memset(&record, 0, sizeof record);
        THROW("HeadlessService_start: secure secret store refused bootstrap TOTP");
        return false;
    }
    memset(secret, 0, sizeof secret);
    Database *db = HeadlessService_database(self);
    if (Database_insertOwned(db, HEADLESS_ADMIN_NAME, &record, sizeof record) != 0 ||
        Database_save(db, HeadlessService_databasePath(self)) != DATABASE_OK) {
        if (!removeFn(HeadlessService_context(self), HEADLESS_ADMIN_SERVICE,
                      HEADLESS_ADMIN_ACCOUNT))
            THROW("HeadlessService_start: unresolved bootstrap secret cleanup failure");
        memset(salt, 0, sizeof salt);
        memset(encoded, 0, sizeof encoded);
        memset(&record, 0, sizeof record);
        THROW("HeadlessService_start: initial database publication failed");
        return false;
    }
    bytes encodedLength = strlen(encoded);
    memcpy(outSetupSecret, encoded, encodedLength + 1u);
    *outSecretLength = encodedLength;
    memset(encoded, 0, sizeof encoded);
    memset(salt, 0, sizeof salt);
    memset(&record, 0, sizeof record);
    return true;
}

HeadlessService *HeadlessService_0() {
    HeadlessService *self = (HeadlessService*) Memory_alloc(0u, Darkbase_arena(), sizeof(HeadlessService));
    if (self == nullptr)
        return nullptr;
    memset(self, 0, sizeof *self);
    HeadlessService_setPasswordIterationsField(self, PASSWORD_ITERATIONS_DEFAULT);
    return self;
}

HeadlessService *HeadlessService_1(const utf8 *databasePath) {
    if (databasePath == nullptr || strlen(databasePath) == 0u ||
        strlen(databasePath) >= FILE_PATH_MAX) {
        THROW("HeadlessService: invalid database path");
        return nullptr;
    }
    HeadlessService *self = HeadlessService_0();
    if (self == nullptr)
        return nullptr;
    memcpy(HeadlessService_databasePathMut(self), databasePath, strlen(databasePath) + 1u);
    return self;
}

fn HeadlessService_free(HeadlessService *self) {
    if (self == nullptr)
        return;
    cleanup(self);
    Memory_free(self);
}

bool HeadlessService_start(HeadlessService *self, HeadlessSecretStoreFn store,
                           HeadlessSecretLoadFn load, HeadlessSecretRemoveFn remove,
                            any context, utf8 *outSetupSecret, bytes secretCapacity,
                           bytes *outSecretLength) {
    if (outSecretLength != nullptr)
        *outSecretLength = 0u;
    if (self == nullptr || HeadlessService_started(self) ||
        HeadlessService_databasePath(self)[0] == '\0' ||
        store == nullptr || load == nullptr || remove == nullptr) {
        THROW("HeadlessService_start: invalid or already-started service");
        return false;
    }
    HeadlessService_setStore(self, store);
    HeadlessService_setLoad(self, load);
    HeadlessService_setRemove(self, remove);
    HeadlessService_setContext(self, context);
    Struct *schema = Struct(HEADLESS_ADMIN_NAME);
    HeadlessService_setAdminSchema(self, schema);
    if (schema == nullptr) {
        cleanup(self);
        THROW("HeadlessService_start: admin schema allocation failed");
        return false;
    }
    Struct_setSize(schema, (u32) sizeof(HeadlessAdminRecord));
    Database *db = Database();
    HeadlessService_setDatabase(self, db);
    if (db == nullptr || Database_define(db, schema) < 0) {
        cleanup(self);
        THROW("HeadlessService_start: database/schema initialization failed");
        return false;
    }
    HeadlessService_setStarted(self, true);
    const utf8 *dbPath = HeadlessService_databasePath(self);
    bool exists = File_exists(dbPath);
    if (exists) {
        if (Database_load(db, dbPath) != DATABASE_OK ||
            Database_count(db, HEADLESS_ADMIN_NAME) != 1u) {
            cleanup(self);
            THROW("HeadlessService_start: existing database rejected or lacks admin record");
            return false;
        }
        u8 secret[HEADLESS_ADMIN_SECRET_BYTES];
        bytes secretLength = 0u;
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
    HeadlessService_setAdminAuthenticated(self, false);
    HeadlessService_setAcceptRequestsField(self, false);
    return true;
}

bool HeadlessService_adminLoginAt(HeadlessService *self, const utf8 *username,
                                  const utf8 *password, u32 totpCode,
                                  u64 unixTimeSeconds) {
    HeadlessAdminRecord *record = adminRecord(self);
    if (record == nullptr || username == nullptr ||
        !CharSequence_equals(CharSequence_cstr(username), CharSequence_cstr(HEADLESS_ADMIN_NAME))) {
        THROW("HeadlessService_adminLogin: unknown admin or service not started");
        return false;
    }
    u64 accepted = 0u;
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
    HeadlessService_setAdminAuthenticated(self, true);
    return true;
}

bool HeadlessService_adminLogin(HeadlessService *self, const utf8 *username,
                               const utf8 *password, u32 totpCode) {
    DateTime now = {0};
    DateTime_set(&now);
    i64 millis = DateTime_epochMillis(&now);
    if (millis < 0)
        return false;
    return HeadlessService_adminLoginAt(self, username, password, totpCode,
                                         (u64) millis / 1000u);
}

bool HeadlessService_changeAdminPasswordAt(HeadlessService *self,
                                           const utf8 *currentPassword,
                                           const utf8 *newPassword, u32 totpCode,
                                           u64 unixTimeSeconds) {
    HeadlessAdminRecord *record = adminRecord(self);
    if (record == nullptr || !HeadlessService_adminAuthenticated(self) ||
        currentPassword == nullptr || newPassword == nullptr || strlen(newPassword) < 12u) {
        THROW("HeadlessService_changeAdminPassword: invalid state or password policy");
        return false;
    }
    u64 accepted = 0u;
    if (!verifyPasswordTotp(self, currentPassword, totpCode, unixTimeSeconds,
                            record, &accepted)) {
        THROW("HeadlessService_changeAdminPassword: credentials rejected");
        return false;
    }
    u32 iters = HeadlessService_passwordIterations(self);
    HeadlessAdminRecord replacement = *record;
    if (!Password_generateSalt(replacement.passwordSalt,
                               sizeof replacement.passwordSalt) ||
        !Password_deriveSha256(newPassword, replacement.passwordSalt,
            sizeof replacement.passwordSalt, iters,
            replacement.passwordHash, sizeof replacement.passwordHash)) {
        memset(&replacement, 0, sizeof replacement);
        THROW("HeadlessService_changeAdminPassword: password derivation failed");
        return false;
    }
    replacement.iterations = iters;
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
                                        const utf8 *currentPassword,
                                        const utf8 *newPassword, u32 totpCode) {
    DateTime now = {0};
    DateTime_set(&now);
    i64 millis = DateTime_epochMillis(&now);
    if (millis < 0)
        return false;
    return HeadlessService_changeAdminPasswordAt(self, currentPassword, newPassword,
        totpCode, (u64) millis / 1000u);
}

bool HeadlessService_setPasswordIterations(HeadlessService *self, u32 iterations) {
    if (self == nullptr || HeadlessService_started(self) || iterations < PASSWORD_ITERATIONS_MIN) {
        THROW("HeadlessService_setPasswordIterations: invalid service state or below policy minimum");
        return false;
    }
    HeadlessService_setPasswordIterationsField(self, iterations);
    return true;
}

u32 HeadlessService_getPasswordIterations(const HeadlessService *self) {
    return self == nullptr ? 0u : HeadlessService_passwordIterations(self);
}

bool HeadlessService_setAcceptRequests(HeadlessService *self, bool accept) {
    HeadlessAdminRecord *record = adminRecord(self);
    if (record == nullptr) {
        THROW("HeadlessService_setAcceptRequests: service not started");
        return false;
    }
    if (accept && (!HeadlessService_adminAuthenticated(self) ||
                   HeadlessAdminRecord_passwordChangeRequired(record) != 0u)) {
        THROW("HeadlessService_setAcceptRequests: admin login and password change required");
        return false;
    }
    if (!accept)
        HeadlessService_setAcceptRequestsField(self, false);
    if (accept == (HeadlessAdminRecord_acceptRequestsPreference(record) != 0u)) {
        HeadlessService_setAcceptRequestsField(self, accept);
        return true;
    }
    HeadlessAdminRecord replacement = *record;
    replacement.acceptRequestsPreference = accept ? 1u : 0u;
    if (!persistRecord(self, record, &replacement)) {
        THROW("HeadlessService_setAcceptRequests: gate state persistence failed");
        return false;
    }
    HeadlessService_setAcceptRequestsField(self, accept);
    return true;
}

bool HeadlessService_isAcceptingRequests(const HeadlessService *self) {
    return self != nullptr && HeadlessService_started(self) && HeadlessService_acceptRequests(self);
}

bool HeadlessService_isRequestAllowed(const HeadlessService *self) {
    return HeadlessService_isAcceptingRequests(self);
}

bool HeadlessService_toString(const HeadlessService *self, utf8 *dest, bytes cap,
                              bool *outTruncated) {
    if (outTruncated != nullptr)
        *outTruncated = false;
    if (dest == nullptr || cap == 0u)
        return false;
    const utf8 *value = self == nullptr ? "nullptr" :
        (HeadlessService_started(self) ? "HeadlessService{started}" : "HeadlessService{closed}");
    bytes length = strlen(value);
    bytes copy = length < cap - 1u ? length : cap - 1u;
    memcpy(dest, value, copy);
    dest[copy] = '\0';
    if (outTruncated != nullptr)
        *outTruncated = copy != length;
    return copy == length;
}

bool HeadlessService_toStringStruct(const HeadlessService *self, utf8 *dest, bytes cap,
                                    bool *outTruncated) {
    if (outTruncated != nullptr)
        *outTruncated = false;
    if (dest == nullptr || cap == 0u)
        return false;
    i32 result = self == nullptr ? snprintf(dest, cap, "nullptr") : snprintf(dest, cap,
        "HeadlessService{database:%s,adminSchema:%s,databasePathLength:%zu,store:%s,load:%s,remove:%s,context:%s,started:%s,adminAuthenticated:%s,acceptRequests:%s,passwordIterations:%u}",
        HeadlessService_database(self) == nullptr ? "nullptr" : "Database",
        HeadlessService_adminSchema(self) == nullptr ? "nullptr" : "Struct",
        strlen(HeadlessService_databasePath(self)), HeadlessService_store(self) == nullptr ? "nullptr" : "function",
        HeadlessService_load(self) == nullptr ? "nullptr" : "function",
        HeadlessService_remove(self) == nullptr ? "nullptr" : "function",
        HeadlessService_context(self) == nullptr ? "nullptr" : "present",
        HeadlessService_started(self) ? "true" : "false",
        HeadlessService_adminAuthenticated(self) ? "true" : "false",
        HeadlessService_acceptRequests(self) ? "true" : "false",
        HeadlessService_passwordIterations(self));
    bool ok = result >= 0 && (bytes) result < cap;
    if (!ok && outTruncated != nullptr)
        *outTruncated = true;
    if (!ok)
        dest[cap - 1u] = '\0';
    return ok;
}
