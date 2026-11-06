// Owner test for the HeadlessService startup, admin gate and recovery contract.

#include "annotation/annotation.h"

#include "service/service.h"
#include "security/password.h"
#include "security/totp.h"
#include "time/datetime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

;;OVERVIEW
/**
 * ============================================================================
 * MODULE: HeadlessServiceTest (darkbase-headless/test/service/service_test.c)
 * CONTRACT: the R4 service owns the database lifetime; startup always closes
 * request admission. First boot emits one TOTP setup secret and creates the
 * one-time admin. Password + TOTP login is replay-protected, bootstrap password
 * replacement is mandatory, and explicit enable is persisted before admission.
 * STRUCT FIELDS: none — procedural owner harness.
 * FUNCTION REGISTRY: main, secretStore, secretLoad, secretRemove, check.
 * ============================================================================
 */

typedef struct TestSecretContext {
    uint8_t secret[20];
    bool present;
} TestSecretContext;

static uint32_t checks;
static uint32_t failures;

#define CHECK(value) do { \
    checks++; \
    if (!(value)) { \
        failures++; \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); \
    } \
} while (false)

static bool secretStore(void *opaqueContext, const char *service,
                        const char *account, const uint8_t *secret, size_t length) {
    TestSecretContext *context = (TestSecretContext*) opaqueContext;
    if (context == nullptr || service == nullptr || account == nullptr || secret == nullptr ||
        length != sizeof (*context).secret)
        return false;
    memcpy((*context).secret, secret, length);
    (*context).present = true;
    return true;
}

static bool secretLoad(void *opaqueContext, const char *service,
                       const char *account, uint8_t *dest, size_t capacity,
                       size_t *outLength) {
    TestSecretContext *context = (TestSecretContext*) opaqueContext;
    if (outLength != nullptr)
        *outLength = 0u;
    if (context == nullptr || service == nullptr || account == nullptr || dest == nullptr ||
        outLength == nullptr || !(*context).present || capacity < sizeof (*context).secret)
        return false;
    memcpy(dest, (*context).secret, sizeof (*context).secret);
    *outLength = sizeof (*context).secret;
    return true;
}

static bool secretRemove(void *opaqueContext, const char *service,
                         const char *account) {
    TestSecretContext *context = (TestSecretContext*) opaqueContext;
    if (context == nullptr || service == nullptr || account == nullptr)
        return false;
    memset((*context).secret, 0, sizeof (*context).secret);
    (*context).present = false;
    return true;
}

static uint32_t codeAt(const TestSecretContext *context, uint64_t seconds) {
    return Totp_code((*context).secret, sizeof (*context).secret, seconds,
                     TOTP_DIGITS_DEFAULT, TOTP_STEP_SECONDS_DEFAULT);
}

int main(void) {
    CHECK(HeadlessService_getPasswordIterations(nullptr) == 0u);
    CHECK(!HeadlessService_setPasswordIterations(nullptr, PASSWORD_ITERATIONS_MIN));
    HeadlessService *emptyService = HeadlessService();
    CHECK(emptyService != nullptr);
    if (emptyService != nullptr) {
        CHECK(!HeadlessService_isAcceptingRequests(emptyService));
        CHECK(!HeadlessService_isRequestAllowed(emptyService));
        CHECK(HeadlessService_getPasswordIterations(emptyService) == PASSWORD_ITERATIONS_DEFAULT);
        char projection[256];
        bool truncated = true;
        CHECK(HeadlessService_toString(nullptr, projection, sizeof projection, &truncated));
        CHECK(!truncated && strcmp(projection, "nullptr") == 0);
        CHECK(HeadlessService_toString(emptyService, projection, sizeof projection, &truncated));
        CHECK(!truncated && strstr(projection, "HeadlessService{closed}") != nullptr);
        CHECK(HeadlessService_toStringStruct(emptyService, projection, sizeof projection,
                                             &truncated));
        CHECK(!truncated && strstr(projection, "passwordIterations:") != nullptr);
        CHECK(!HeadlessService_toString(emptyService, projection, 1u, &truncated));
        CHECK(truncated);
        HeadlessService_free(emptyService);
    }
    CHECK(HeadlessService_1(nullptr) == nullptr);

    char directory[] = "/tmp/darkbase-headless-XXXXXX";
    CHECK(mkdtemp(directory) != nullptr);
    if (failures != 0u)
        return EXIT_FAILURE;
    char databasePath[256];
    int pathLength = snprintf(databasePath, sizeof databasePath, "%s/admin.vexdb", directory);
    CHECK(pathLength > 0 && (size_t) pathLength < sizeof databasePath);
    if (pathLength <= 0 || (size_t) pathLength >= sizeof databasePath)
        return EXIT_FAILURE;

    TestSecretContext secrets = {0};
    HeadlessService *service = HeadlessService(databasePath);
    CHECK(service != nullptr);
    if (service == nullptr)
        return EXIT_FAILURE;
    CHECK(HeadlessService_getPasswordIterations(service) == PASSWORD_ITERATIONS_DEFAULT);
    CHECK(!HeadlessService_setPasswordIterations(service, PASSWORD_ITERATIONS_MIN - 1u));
    CHECK(HeadlessService_setPasswordIterations(service, PASSWORD_ITERATIONS_MIN));
    CHECK(HeadlessService_getPasswordIterations(service) == PASSWORD_ITERATIONS_MIN);
    char setupSecret[64];
    size_t setupLength = 0u;
    CHECK(!HeadlessService_start(service, nullptr, nullptr, nullptr, nullptr,
                                 setupSecret, sizeof setupSecret, &setupLength));
    CHECK(setupLength == 0u);
    CHECK(HeadlessService_start(service, secretStore, secretLoad, secretRemove,
                                &secrets, setupSecret, sizeof setupSecret,
                                &setupLength));
    CHECK(setupLength == 32u && setupSecret[setupLength] == '\0');
    CHECK(secrets.present);
    CHECK(!HeadlessService_isRequestAllowed(service));
    CHECK(!HeadlessService_setAcceptRequests(service, true));

    uint64_t firstTime = 600u;
    CHECK(HeadlessService_adminLoginAt(service, "not-admin", "Password123!",
                                       codeAt(&secrets, firstTime), firstTime) == false);
    CHECK(HeadlessService_adminLoginAt(service, "admin", "wrong-password",
                                       codeAt(&secrets, firstTime), firstTime) == false);
    CHECK(HeadlessService_adminLoginAt(service, "admin", "Password123!",
                                       codeAt(&secrets, firstTime), firstTime));
    CHECK(!HeadlessService_setAcceptRequests(service, true));

    uint64_t nextTime = 630u;
    CHECK(HeadlessService_changeAdminPasswordAt(service, "Password123!", "correct horse battery",
        codeAt(&secrets, nextTime), nextTime));
    CHECK(HeadlessService_setAcceptRequests(service, true));
    CHECK(HeadlessService_isRequestAllowed(service));
    CHECK(!HeadlessService_adminLoginAt(service, "admin", "correct horse battery",
        codeAt(&secrets, nextTime), nextTime));
    CHECK(HeadlessService_setAcceptRequests(service, false));
    CHECK(!HeadlessService_isRequestAllowed(service));
    HeadlessService_free(service);

    service = HeadlessService(databasePath);
    CHECK(service != nullptr);
    if (service != nullptr) {
        CHECK(HeadlessService_setPasswordIterations(service, PASSWORD_ITERATIONS_MIN));
        setupLength = SIZE_MAX;
        CHECK(HeadlessService_start(service, secretStore, secretLoad, secretRemove,
                                    &secrets, setupSecret, sizeof setupSecret,
                                    &setupLength));
        CHECK(setupLength == 0u);
        CHECK(!HeadlessService_isRequestAllowed(service));
        CHECK(!HeadlessService_setAcceptRequests(service, true));
        uint64_t laterTime = 660u;
        CHECK(HeadlessService_adminLoginAt(service, "admin", "correct horse battery",
            codeAt(&secrets, laterTime), laterTime));
        CHECK(HeadlessService_setAcceptRequests(service, true));
        DateTime now = {0};
        DateTime_set(&now);
        int64_t nowMillis = DateTime_epochMillis(&now);
        CHECK(nowMillis >= 0);
        if (nowMillis >= 0) {
            uint64_t nowSeconds = (uint64_t) nowMillis / 1000u;
            CHECK(HeadlessService_adminLogin(service, "admin", "correct horse battery",
                                              codeAt(&secrets, nowSeconds)));
            CHECK(!HeadlessService_changeAdminPassword(service, "correct horse battery",
                "correct horse battery staple", codeAt(&secrets, nowSeconds)));
        }
        HeadlessService_free(service);
    }

    memset(&secrets, 0, sizeof secrets);
    unlink(databasePath);
    rmdir(directory);
    printf("HeadlessService: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? EXIT_SUCCESS : EXIT_FAILURE;
}
