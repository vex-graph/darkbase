#include "oop/modifier.h"

#include <stdio.h>
#include <string.h>

#include "annotation/annotation.h"
#include "exception/throw.h"

;;DEFINITION
/** A Modifier is the frozen keyword set of a vex database field: how it is
 * accessed (public/private), how it is protected (plain/hashed/client-sealed/
 * server-sealed/truncated), whether it may be absent (nullable/nonnull), and
 * where it lives (stored, transient, or shared/static). It exists so a field
 * declaration reads like a Java field and the store has ONE place to ask
 * "what did the schema author mean?" — instead of every query re-deciding
 * access and every caller re-deciding crypto.
 *
 * It is deliberately a VALUE RECORD. It owns no memory, borrows nothing and
 * allocates nothing, so a schema can hold an array of them and a caller can
 * pass one by value. That is also why there is no registry id: like
 * DatabaseFieldSpan, it is a described value, not an allocated owner.
 *
 * INTENTIONAL(vex): the keyword set is closed. Flexibility lives in schema
 * VERSIONING and field VISIBILITY — a field is hidden or a new version is
 * published, never re-declared in place. You do not change a data type just
 * because it is that; the whole point of freezing the words is that an old row
 * written under one declaration keeps meaning the same thing forever. A new
 * keyword later is a breaking change the way a new Java keyword is, which is
 * exactly why nothing else is allowed in this set: key, unique, indexed,
 * default, computed, lifecycle, retention and audit mode are extensible
 * ATTRIBUTES, not modifiers.
 *
 * The three axes are orthogonal, so the only structurally illegal state is an
 * axis outside its enum; Modifier_validate catches that and names the axis.
 * Combinations that are merely questionable — a public verifier, a protected
 * in-memory value, a shared transient — are WARNINGS, not rejections, because
 * a public content hash is legitimate and the schema author decides.
 *
 * The 80 MODIFIER_* presets are declaration sugar over this record. The
 * terminal path parses the same words with Modifier_parse, so the C schema and
 * the terminal schema meet in one value and one validator. Parsing is cold and
 * THROWs once per rejection; reading a copy is lock-free because a Modifier is
 * immutable once constructed. */
;;OVERVIEW
/** ========================================================================
 * CLASS: Modifier (oop/modifier.c)
 * STRUCT FIELDS (Mirroring oop/modifier.h)
 *   uint32_t access       ModifierAccess: private (default) or public
 *   uint32_t protection   ModifierProtection: plain/hashed/client-sealed/
 *                         server-sealed/truncated
 *   uint32_t nullability  ModifierNullability: nullable (default) or nonnull
 *   bool     transient    not persisted: in-memory only
 *   bool     shared       "static": shared across rows
 *   int32_t  lastCode     last ModifierCode
 *
 * TOKEN GRAMMAR (Modifier_parse / Modifier_toString)
 *   <access> <protection> <nullability> [transient] [static]
 *   private | public
 *   plain | hashed | client-sealed | server-sealed | truncated
 *   nullable | nonnull
 * Order is free; naming one axis twice rejects; absence is the default.
 *
 * FUNCTION REGISTRY (exports)
 *   Constructors:
 *     Modifier Modifier_0(void)
 *     Modifier Modifier_3(access, protection, nullability)
 *     Modifier Modifier_5(access, protection, nullability, transient, shared)
 *     #define Modifier(...)   arity dispatch
 *     #define Modifier_zero() Modifier_0()
 *   Validation:
 *     ModifierCode Modifier_validate(Modifier self)
 *     bool         Modifier_isAdmissible(Modifier self)
 *     uint32_t     Modifier_warnings(Modifier self)
 *   Accessors:
 *     ModifierAccess      Modifier_getAccess(Modifier self)
 *     ModifierProtection  Modifier_getProtection(Modifier self)
 *     ModifierNullability Modifier_getNullability(Modifier self)
 *     bool                Modifier_isTransient(Modifier self)
 *     bool                Modifier_isShared(Modifier self)
 *     ModifierCode        Modifier_code(Modifier self)
 *   Predicates:
 *     bool Modifier_isPublic/isPrivate/isProtected/isReversible/isOneWay/isLossy
 *     bool Modifier_equals(Modifier a, Modifier b.sh)
 *   Naming:
 *     const char *Modifier_accessName/protectionName/nullabilityName/codeName
 *     const char *Modifier_warningName(uint32_t warning)
 *   Terminal path:
 *     Modifier Modifier_parse(const char *text)
 *     Modifier Modifier_applyToken(Modifier self, const char *token)
 *   Projections:
 *     bool Modifier_toString(Modifier self, char *dest, size_t capacity,
 *                            bool *outTruncated)
 *     bool Modifier_toStringStruct(Modifier self, char *dest, size_t capacity,
 *                                  bool *outTruncated)
 * PRIVATE HELPERS
 *   ModifierAxis { NONE, ACCESS, PROTECTION, NULLABILITY, TRANSIENT, SHARED }
 *   classify(token)            which axis a token names, or NONE
 *   applyAxis(self, axis, token)  set one axis; no validation, no THROW
 *
 * CONTRACT: null self / an out-of-range axis returns the safe default (the
 * zero Modifier) with a ModifierCode, never a crash. Modifier_parse and
 * Modifier_applyToken THROW once on a bad token; the value functions do not
 * log. A Modifier is immutable once built and safe to copy across threads.
 * ======================================================================== */

;;INTENTION("darkbase's established projection convention is dest-last (Page_toString, Database_toString, Wal_toString all take a destination). Following it here keeps the new class consistent with every existing owner; the universal Value Return Law migration for this repository is a separate, repo-wide change and is not silently waived by this file.")

typedef enum ModifierAxis {
    MODIFIER_AXIS_NONE = 0,
    MODIFIER_AXIS_ACCESS = 1,
    MODIFIER_AXIS_PROTECTION = 2,
    MODIFIER_AXIS_NULLABILITY = 3,
    MODIFIER_AXIS_TRANSIENT = 4,
    MODIFIER_AXIS_SHARED = 5,
} ModifierAxis;

static ModifierAxis classify(const char *token) {
    if (token == nullptr || token[0] == '\0')
        return MODIFIER_AXIS_NONE;
    if (strcmp(token, "private") == 0 || strcmp(token, "public") == 0)
        return MODIFIER_AXIS_ACCESS;
    if (strcmp(token, "plain") == 0 || strcmp(token, "hashed") == 0 ||
        strcmp(token, "client-sealed") == 0 || strcmp(token, "server-sealed") == 0 ||
        strcmp(token, "truncated") == 0)
        return MODIFIER_AXIS_PROTECTION;
    if (strcmp(token, "nullable") == 0 || strcmp(token, "nonnull") == 0)
        return MODIFIER_AXIS_NULLABILITY;
    if (strcmp(token, "transient") == 0)
        return MODIFIER_AXIS_TRANSIENT;
    if (strcmp(token, "static") == 0)
        return MODIFIER_AXIS_SHARED;
    return MODIFIER_AXIS_NONE;
}

static Modifier applyAxis(Modifier self, ModifierAxis axis, const char *token) {
    switch (axis) {
        case MODIFIER_AXIS_ACCESS:
            self.access = strcmp(token, "public") == 0
                ? (uint32_t) MODIFIER_ACCESS_PUBLIC
                : (uint32_t) MODIFIER_ACCESS_PRIVATE;
            break;
        case MODIFIER_AXIS_PROTECTION:
            if (strcmp(token, "hashed") == 0)
                self.protection = (uint32_t) MODIFIER_PROTECTION_HASHED;
            else if (strcmp(token, "client-sealed") == 0)
                self.protection = (uint32_t) MODIFIER_PROTECTION_CLIENT_SEALED;
            else if (strcmp(token, "server-sealed") == 0)
                self.protection = (uint32_t) MODIFIER_PROTECTION_SERVER_SEALED;
            else if (strcmp(token, "truncated") == 0)
                self.protection = (uint32_t) MODIFIER_PROTECTION_TRUNCATED;
            else
                self.protection = (uint32_t) MODIFIER_PROTECTION_PLAIN;
            break;
        case MODIFIER_AXIS_NULLABILITY:
            self.nullability = strcmp(token, "nonnull") == 0
                ? (uint32_t) MODIFIER_NONNULL
                : (uint32_t) MODIFIER_NULLABLE;
            break;
        case MODIFIER_AXIS_TRANSIENT:
            self.transient = true;
            break;
        case MODIFIER_AXIS_SHARED:
            self.shared = true;
            break;
        case MODIFIER_AXIS_NONE:
        default:
            break;
    }
    return self;
}

Modifier Modifier_0(void) {
    Modifier self = {
        .access = (uint32_t) MODIFIER_ACCESS_PRIVATE,
        .protection = (uint32_t) MODIFIER_PROTECTION_PLAIN,
        .nullability = (uint32_t) MODIFIER_NULLABLE,
        .transient = false,
        .shared = false,
        .lastCode = MODIFIER_OK,
    };
    return self;
}

Modifier Modifier_3(ModifierAccess access, ModifierProtection protection,
                    ModifierNullability nullability) {
    Modifier self = Modifier_0();
    self.access = (uint32_t) access;
    self.protection = (uint32_t) protection;
    self.nullability = (uint32_t) nullability;
    self.lastCode = Modifier_validate(self);
    return self;
}

Modifier Modifier_5(ModifierAccess access, ModifierProtection protection,
                    ModifierNullability nullability, bool transient, bool shared) {
    Modifier self = Modifier_3(access, protection, nullability);
    self.transient = transient;
    self.shared = shared;
    return self;
}

ModifierCode Modifier_validate(Modifier self) {
    if (self.access > (uint32_t) MODIFIER_ACCESS_PUBLIC)
        return MODIFIER_BAD_ACCESS;
    if (self.protection > (uint32_t) MODIFIER_PROTECTION_TRUNCATED)
        return MODIFIER_BAD_PROTECTION;
    if (self.nullability > (uint32_t) MODIFIER_NONNULL)
        return MODIFIER_BAD_NULLABILITY;
    return MODIFIER_OK;
}

bool Modifier_isAdmissible(Modifier self) {
    return Modifier_validate(self) == MODIFIER_OK;
}

uint32_t Modifier_warnings(Modifier self) {
    uint32_t flags = MODIFIER_WARNING_NONE;
    if (self.access == (uint32_t) MODIFIER_ACCESS_PUBLIC &&
        self.protection == (uint32_t) MODIFIER_PROTECTION_HASHED)
        flags |= (uint32_t) MODIFIER_WARNING_PUBLIC_VERIFIER;
    if (self.transient && self.protection != (uint32_t) MODIFIER_PROTECTION_PLAIN)
        flags |= (uint32_t) MODIFIER_WARNING_TRANSIENT_PROTECTED;
    if (self.shared && self.transient)
        flags |= (uint32_t) MODIFIER_WARNING_SHARED_TRANSIENT;
    if (self.protection == (uint32_t) MODIFIER_PROTECTION_TRUNCATED)
        flags |= (uint32_t) MODIFIER_WARNING_LOSSY;
    return flags;
}

ModifierAccess Modifier_getAccess(Modifier self) {
    return (ModifierAccess) self.access;
}

ModifierProtection Modifier_getProtection(Modifier self) {
    return (ModifierProtection) self.protection;
}

ModifierNullability Modifier_getNullability(Modifier self) {
    return (ModifierNullability) self.nullability;
}

bool Modifier_isTransient(Modifier self) {
    return self.transient;
}

bool Modifier_isShared(Modifier self) {
    return self.shared;
}

ModifierCode Modifier_code(Modifier self) {
    return (ModifierCode) self.lastCode;
}

bool Modifier_isPublic(Modifier self) {
    return self.access == (uint32_t) MODIFIER_ACCESS_PUBLIC;
}

bool Modifier_isPrivate(Modifier self) {
    return self.access == (uint32_t) MODIFIER_ACCESS_PRIVATE;
}

bool Modifier_isProtected(Modifier self) {
    return self.protection != (uint32_t) MODIFIER_PROTECTION_PLAIN;
}

bool Modifier_isReversible(Modifier self) {
    return self.protection == (uint32_t) MODIFIER_PROTECTION_CLIENT_SEALED ||
           self.protection == (uint32_t) MODIFIER_PROTECTION_SERVER_SEALED;
}

bool Modifier_isOneWay(Modifier self) {
    return self.protection == (uint32_t) MODIFIER_PROTECTION_HASHED;
}

bool Modifier_isLossy(Modifier self) {
    return self.protection == (uint32_t) MODIFIER_PROTECTION_TRUNCATED;
}

bool Modifier_equals(Modifier a, Modifier b) {
    return a.access == b.access && a.protection == b.protection &&
           a.nullability == b.nullability && a.transient == b.transient &&
           a.shared == b.shared;
}

const char *Modifier_accessName(ModifierAccess access) {
    switch (access) {
        case MODIFIER_ACCESS_PRIVATE: return "private";
        case MODIFIER_ACCESS_PUBLIC: return "public";
        default: return "unknown";
    }
}

const char *Modifier_protectionName(ModifierProtection protection) {
    switch (protection) {
        case MODIFIER_PROTECTION_PLAIN: return "plain";
        case MODIFIER_PROTECTION_HASHED: return "hashed";
        case MODIFIER_PROTECTION_CLIENT_SEALED: return "client-sealed";
        case MODIFIER_PROTECTION_SERVER_SEALED: return "server-sealed";
        case MODIFIER_PROTECTION_TRUNCATED: return "truncated";
        default: return "unknown";
    }
}

const char *Modifier_nullabilityName(ModifierNullability nullability) {
    switch (nullability) {
        case MODIFIER_NULLABLE: return "nullable";
        case MODIFIER_NONNULL: return "nonnull";
        default: return "unknown";
    }
}

const char *Modifier_codeName(ModifierCode code) {
    switch (code) {
        case MODIFIER_OK: return "OK";
        case MODIFIER_INVALID: return "INVALID";
        case MODIFIER_BAD_ACCESS: return "BAD_ACCESS";
        case MODIFIER_BAD_PROTECTION: return "BAD_PROTECTION";
        case MODIFIER_BAD_NULLABILITY: return "BAD_NULLABILITY";
        case MODIFIER_BAD_TOKEN: return "BAD_TOKEN";
        case MODIFIER_DUPLICATE: return "DUPLICATE";
        default: return "UNKNOWN";
    }
}

const char *Modifier_warningName(uint32_t warning) {
    switch (warning) {
        case MODIFIER_WARNING_NONE: return "none";
        case MODIFIER_WARNING_PUBLIC_VERIFIER: return "public-verifier";
        case MODIFIER_WARNING_TRANSIENT_PROTECTED: return "transient-protected";
        case MODIFIER_WARNING_SHARED_TRANSIENT: return "shared-transient";
        case MODIFIER_WARNING_LOSSY: return "lossy";
        default: return "unknown";
    }
}

Modifier Modifier_applyToken(Modifier self, const char *token) {
    ModifierAxis axis = classify(token);
    if (axis == MODIFIER_AXIS_NONE) {
        self.lastCode = MODIFIER_BAD_TOKEN;
        THROW("Modifier_applyToken: unknown modifier token \"%s\"",
              token == nullptr ? "nullptr" : token);
        return self;
    }
    self = applyAxis(self, axis, token);
    self.lastCode = MODIFIER_OK;
    return self;
}

Modifier Modifier_parse(const char *text) {
    Modifier self = Modifier_0();
    if (text == nullptr) {
        self.lastCode = MODIFIER_INVALID;
        THROW("Modifier_parse: null modifier text");
        return self;
    }
    bool seen[5] = { false, false, false, false, false };
    size_t count = 0u;
    const char *cursor = text;
    while (*cursor != '\0') {
        cursor += strspn(cursor, " \t\r\n");
        if (*cursor == '\0')
            break;
        size_t length = strcspn(cursor, " \t\r\n");
        if (length == 0u || length >= MODIFIER_TOKEN_MAX) {
            self.lastCode = MODIFIER_BAD_TOKEN;
            THROW("Modifier_parse: modifier token over %u bytes",
                  (unsigned) MODIFIER_TOKEN_MAX - 1u);
            return self;
        }
        char token[MODIFIER_TOKEN_MAX];
        memcpy(token, cursor, length);
        token[length] = '\0';
        ModifierAxis axis = classify(token);
        if (axis == MODIFIER_AXIS_NONE) {
            self.lastCode = MODIFIER_BAD_TOKEN;
            THROW("Modifier_parse: unknown modifier token \"%s\"", token);
            return self;
        }
        size_t index = (size_t) axis - 1u;
        if (seen[index]) {
            self.lastCode = MODIFIER_DUPLICATE;
            THROW("Modifier_parse: modifier axis named twice (\"%s\")", token);
            return self;
        }
        seen[index] = true;
        self = applyAxis(self, axis, token);
        ++count;
        if (count > (size_t) MODIFIER_TOKEN_LIMIT) {
            self.lastCode = MODIFIER_BAD_TOKEN;
            THROW("Modifier_parse: more than %u modifier tokens",
                  (unsigned) MODIFIER_TOKEN_LIMIT);
            return self;
        }
        cursor += length;
    }
    self.lastCode = MODIFIER_OK;
    return self;
}

bool Modifier_toString(Modifier self, char *dest, size_t capacity, bool *outTruncated) {
    if (dest == nullptr || capacity == 0u) {
        if (outTruncated != nullptr)
            *outTruncated = true;
        return false;
    }
    int written = snprintf(dest, capacity, "%s:%s:%s%s%s",
        Modifier_accessName((ModifierAccess) self.access),
        Modifier_protectionName((ModifierProtection) self.protection),
        Modifier_nullabilityName((ModifierNullability) self.nullability),
        self.transient ? ":transient" : "",
        self.shared ? ":static" : "");
    bool cut = written < 0 || (size_t) written >= capacity;
    if (outTruncated != nullptr)
        *outTruncated = cut;
    return !cut;
}

bool Modifier_toStringStruct(Modifier self, char *dest, size_t capacity,
                             bool *outTruncated) {
    if (dest == nullptr || capacity == 0u) {
        if (outTruncated != nullptr)
            *outTruncated = true;
        return false;
    }
    int written = snprintf(dest, capacity,
        "Modifier{access=%s, protection=%s, nullability=%s, transient=%d, "
        "shared=%d, lastCode=%s}",
        Modifier_accessName((ModifierAccess) self.access),
        Modifier_protectionName((ModifierProtection) self.protection),
        Modifier_nullabilityName((ModifierNullability) self.nullability),
        self.transient ? 1 : 0, self.shared ? 1 : 0,
        Modifier_codeName((ModifierCode) self.lastCode));
    bool cut = written < 0 || (size_t) written >= capacity;
    if (outTruncated != nullptr)
        *outTruncated = cut;
    return !cut;
}
