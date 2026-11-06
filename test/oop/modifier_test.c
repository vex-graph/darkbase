// Owner: oop/modifier.c — the frozen field-modifier keyword set.
//
// Proves the three axes validate, the 80 declaration presets and the short
// forms agree with the orthogonal constructor, the terminal parser accepts the
// same words the C schema uses (free order, defaults by absence) and rejects
// unknown/duplicate/overlong input with exactly one cold diagnostic, and both
// projections round-trip. A Modifier is a value record: nothing is allocated,
// so there is no lifetime to leak.
#include "oop/modifier.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// --- A captured cold diagnostic must be exactly one "[vex]" line. -----------
// A function type is spelled `typedef Modifier Name(void);` — never
// `Modifier (*Name)(void)` — because the constructor macro `Modifier(...)`
// would otherwise expand on the `Modifier (` sequence.
typedef Modifier ModifierProbe(void);

static size_t captureProbe(ModifierProbe probe, char *out, size_t cap) {
    FILE *capture = tmpfile();
    assert(capture != nullptr);
    assert(fflush(stderr) == 0);
    int saved = dup(STDERR_FILENO);
    assert(saved >= 0 && dup2(fileno(capture), STDERR_FILENO) >= 0);
    (void) probe();
    assert(fflush(stderr) == 0);
    assert(dup2(saved, STDERR_FILENO) >= 0);
    close(saved);
    rewind(capture);
    size_t n = fread(out, 1u, cap - 1u, capture);
    out[n] = '\0';
    fclose(capture);
    return n;
}

static size_t lineCount(const char *text) {
    size_t lines = 0u;
    for (const char *p = text; *p != '\0'; ++p)
        if (*p == '\n')
            ++lines;
    return lines;
}

static Modifier probeNull(void) { return Modifier_parse(nullptr); }
static Modifier probeUnknown(void) { return Modifier_parse("bogus"); }
static Modifier probeDuplicate(void) { return Modifier_parse("public public"); }
static Modifier probeOverlong(void) {
    return Modifier_parse("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"); // 32 bytes
}
static Modifier probeApplyUnknown(void) {
    return Modifier_applyToken(Modifier_0(), "bogus");
}
static Modifier probeQuiet(void) { return Modifier_parse("public hashed nullable"); }

static void checkReject(ModifierProbe probe, ModifierCode expected, const char *fragment) {
    char text[512];
    size_t n = captureProbe(probe, text, sizeof text);
    assert(n > 0u);
    assert(lineCount(text) == 1u);
    assert(strncmp(text, "[vex] ", 6u) == 0);
    assert(strstr(text, fragment) != nullptr);
    (void) expected;
}

static void checkQuiet(ModifierProbe probe) {
    char text[512];
    size_t n = captureProbe(probe, text, sizeof text);
    assert(n == 0u);
}

int main(void) {
    // --- Defaults and constructors -----------------------------------------
    Modifier def = Modifier_0();
    assert(Modifier_equals(def, MODIFIER_DEFAULT));
    assert(Modifier_equals(Modifier(), def));
    assert(Modifier_isPrivate(def) && !Modifier_isPublic(def));
    assert(!Modifier_isProtected(def) && !Modifier_isTransient(def) && !Modifier_isShared(def));
    assert(Modifier_code(def) == MODIFIER_OK);

    Modifier three = Modifier(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_HASHED,
                              MODIFIER_NULLABLE);
    assert(Modifier_getAccess(three) == MODIFIER_ACCESS_PUBLIC);
    assert(Modifier_getProtection(three) == MODIFIER_PROTECTION_HASHED);
    assert(Modifier_getNullability(three) == MODIFIER_NULLABLE);

    Modifier five = Modifier(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_SERVER_SEALED,
                             MODIFIER_NONNULL, true, true);
    assert(Modifier_getNullability(five) == MODIFIER_NONNULL);
    assert(Modifier_isTransient(five) && Modifier_isShared(five));
    assert(Modifier_isAdmissible(five));

    // --- Presets: full and short forms agree -------------------------------
    assert(Modifier_equals(MODIFIER_PUBLIC_HASHED, MODIFIER_PUBLIC_HASHED_NULLABLE));
    assert(Modifier_equals(MODIFIER_PUBLIC_HASHED, three));
    assert(Modifier_equals(MODIFIER_PUBLIC_SERVER_SEALED_NONNULL_TRANSIENT_STATIC, five));
    assert(Modifier_equals(MODIFIER_PRIVATE, MODIFIER_PRIVATE_PLAIN_NULLABLE));
    assert(Modifier_equals(MODIFIER_PUBLIC, MODIFIER_PUBLIC_PLAIN_NULLABLE));
    Modifier extremes = MODIFIER_PRIVATE_TRUNCATED_NONNULL_TRANSIENT_STATIC;
    assert(Modifier_isPrivate(extremes) && Modifier_isLossy(extremes));
    assert(Modifier_getNullability(extremes) == MODIFIER_NONNULL);
    assert(Modifier_isTransient(extremes) && Modifier_isShared(extremes));

    // --- Validation names the offending axis -------------------------------
    Modifier badAccess = def;
    badAccess.access = 9u;
    assert(Modifier_validate(badAccess) == MODIFIER_BAD_ACCESS);
    assert(!Modifier_isAdmissible(badAccess));
    Modifier badProtection = def;
    badProtection.protection = 9u;
    assert(Modifier_validate(badProtection) == MODIFIER_BAD_PROTECTION);
    Modifier badNullability = def;
    badNullability.nullability = 9u;
    assert(Modifier_validate(badNullability) == MODIFIER_BAD_NULLABILITY);
    assert(Modifier_validate(def) == MODIFIER_OK);

    // --- Predicates and warnings -------------------------------------------
    assert(Modifier_isOneWay(MODIFIER_PUBLIC_HASHED));
    assert(Modifier_isReversible(MODIFIER_PRIVATE_CLIENT_SEALED));
    assert(Modifier_isReversible(MODIFIER_PRIVATE_SERVER_SEALED));
    assert(Modifier_isLossy(MODIFIER_PUBLIC_TRUNCATED));
    assert(Modifier_isProtected(MODIFIER_PUBLIC_TRUNCATED));
    assert(!Modifier_isProtected(def));
    assert(!Modifier_isOneWay(MODIFIER_PRIVATE_CLIENT_SEALED));

    assert(Modifier_warnings(MODIFIER_PUBLIC_HASHED) &
           (uint32_t) MODIFIER_WARNING_PUBLIC_VERIFIER);
    assert(Modifier_warnings(MODIFIER_PRIVATE_HASHED_NULLABLE_TRANSIENT) &
           (uint32_t) MODIFIER_WARNING_TRANSIENT_PROTECTED);
    assert(Modifier_warnings(MODIFIER_PUBLIC_PLAIN_NULLABLE_TRANSIENT_STATIC) &
           (uint32_t) MODIFIER_WARNING_SHARED_TRANSIENT);
    assert(Modifier_warnings(MODIFIER_PRIVATE_TRUNCATED) &
           (uint32_t) MODIFIER_WARNING_LOSSY);
    assert(Modifier_warnings(def) == MODIFIER_WARNING_NONE);

    // --- Names --------------------------------------------------------------
    assert(strcmp(Modifier_accessName(MODIFIER_ACCESS_PUBLIC), "public") == 0);
    assert(strcmp(Modifier_protectionName(MODIFIER_PROTECTION_CLIENT_SEALED),
                  "client-sealed") == 0);
    assert(strcmp(Modifier_nullabilityName(MODIFIER_NONNULL), "nonnull") == 0);
    assert(strcmp(Modifier_codeName(MODIFIER_DUPLICATE), "DUPLICATE") == 0);
    assert(strcmp(Modifier_warningName(MODIFIER_WARNING_LOSSY), "lossy") == 0);
    assert(strcmp(Modifier_accessName((ModifierAccess) 9), "unknown") == 0);

    // --- Parser: the terminal path meets the C schema ----------------------
    assert(Modifier_equals(Modifier_parse(""), def));
    assert(Modifier_equals(Modifier_parse("   \t\r\n "), def));
    assert(Modifier_equals(Modifier_parse("public hashed nullable"), three));
    assert(Modifier_equals(Modifier_parse("nullable public hashed"), three)); // order free
    assert(Modifier_equals(Modifier_parse("public server-sealed nonnull transient static"), five));
    assert(Modifier_equals(Modifier_parse("private truncated nonnull transient static"), extremes));
    assert(Modifier_code(Modifier_parse("public hashed")) == MODIFIER_OK);

    Modifier bad = Modifier_parse("bogus");
    assert(Modifier_code(bad) == MODIFIER_BAD_TOKEN);
    assert(Modifier_equals(bad, def)); // rejection leaves the safe default
    assert(Modifier_code(Modifier_parse("public public")) == MODIFIER_DUPLICATE);
    assert(Modifier_code(Modifier_parse(nullptr)) == MODIFIER_INVALID);
    assert(Modifier_code(Modifier_applyToken(Modifier_0(), "bogus")) == MODIFIER_BAD_TOKEN);
    assert(Modifier_equals(Modifier_applyToken(Modifier_0(), "public"), MODIFIER_PUBLIC));

    checkReject(probeNull, MODIFIER_INVALID, "null modifier text");
    checkReject(probeUnknown, MODIFIER_BAD_TOKEN, "unknown modifier token");
    checkReject(probeDuplicate, MODIFIER_DUPLICATE, "named twice");
    checkReject(probeOverlong, MODIFIER_BAD_TOKEN, "over 31 bytes");
    checkReject(probeApplyUnknown, MODIFIER_BAD_TOKEN, "unknown modifier token");
    checkQuiet(probeQuiet);

    // --- Projections --------------------------------------------------------
    char text[256];
    bool cut = true;
    assert(Modifier_toString(three, text, sizeof text, &cut) && !cut);
    assert(strcmp(text, "public:hashed:nullable") == 0);
    assert(Modifier_toString(five, text, sizeof text, &cut) && !cut);
    assert(strcmp(text, "public:server-sealed:nonnull:transient:static") == 0);
    assert(Modifier_toString(def, text, sizeof text, &cut) && !cut);
    assert(strcmp(text, "private:plain:nullable") == 0);
    assert(!Modifier_toString(five, text, 5u, &cut) && cut); // bounded, never silent
    assert(!Modifier_toString(five, nullptr, 0u, &cut) && cut);

    assert(Modifier_toStringStruct(five, text, sizeof text, &cut) && !cut);
    assert(strstr(text, "access=public") != nullptr);
    assert(strstr(text, "protection=server-sealed") != nullptr);
    assert(strstr(text, "nullability=nonnull") != nullptr);
    assert(strstr(text, "transient=1") != nullptr);
    assert(strstr(text, "shared=1") != nullptr);
    assert(strstr(text, "lastCode=OK") != nullptr);
    assert(!Modifier_toStringStruct(five, text, 4u, &cut) && cut);

    printf("modifier_test: OK\n");
    return 0;
}
