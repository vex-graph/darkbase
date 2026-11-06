#ifndef DARKBASE_OOP_MODIFIER_H
#define DARKBASE_OOP_MODIFIER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "c23/constructor.h"

// oop/modifier.h — the frozen field-modifier keyword set.
//
// A Modifier is the schema-time declaration of how ONE field is accessed,
// protected, null-checked and stored. It is a small VALUE RECORD: it owns no
// memory, borrows nothing, and is copied around by value. It is the Java-like
// keyword set for a vex database field:
//
//     public|private
//     hashed|client-sealed|server-sealed|truncated      (plain by absence)
//     nullable|nonnull                                  (nullable by default)
//     transient                                         (not persisted)
//     static                                            (shared across rows)
//
// THE SET IS FROZEN. Like Java's public/private/static/final/transient, these
// keywords do not grow casually: adding one later breaks every existing schema
// that used the word as a name. Everything that is NOT here — key, unique,
// indexed, default, computed, lifecycle, retention, audit mode, schema version
// — is an extensible ATTRIBUTE, not a modifier. That split is what keeps the
// keyword set stable.
//
// INTENTIONAL(vex): the modifier keyword set is closed on purpose. A field's
// access, protection, nullability and storage are declared once and stay; the
// ecosystem's flexibility comes from schema VERSIONING and field VISIBILITY,
// never from editing a live field's declaration. You do not change a data
// type just because it is that — you version it or you hide it. Adding a
// keyword later is a breaking change the same way a new Java keyword is, so
// the set is shut now and everything else is an extensible ATTRIBUTE.
//
// WHAT EACH AXIS MEANS
//   access       private = the processor/private API only (default);
//                public  = reachable through the public API.
//   protection   plain          = stored as-is (default);
//                hashed         = one-way verifier; never returned as plaintext;
//                client-sealed  = reversible; the CLIENT holds the key, so the
//                                 server stores ciphertext it cannot read;
//                server-sealed  = reversible; the PROCESSOR holds the key and
//                                 decrypts on demand;
//                truncated      = irreversible loss; explicit opt-in only, and
//                                 the cut must be reported (never silent).
//   nullability  nullable = the value may be absent (default);
//                nonnull  = the value must be present.
//   transient    the field is NOT persisted: it lives in memory only (a live
//                presence table, a session registry, a cache). Save/load/export
//                skip it; it is per-process and lost on restart.
//   shared       the field is shared across rows ("static"): its value lives in
//                the schema/catalog or a singleton, not in each row.
//
// DECLARATION-SITE SHORTHAND (the 80 presets)
// Every combination has one MODIFIER_* macro, each expanding to a Modifier
// value. The naming is uniform:
//
//     MODIFIER_<ACCESS>_<PROTECTION>_<NULLABILITY>[_TRANSIENT][_STATIC]
//
//   MODIFIER_PUBLIC_HASHED_NULLABLE
//   MODIFIER_PRIVATE_CLIENT_SEALED_NONNULL_TRANSIENT
//   MODIFIER_PUBLIC_SERVER_SEALED_NULLABLE_STATIC
//
// The ten short forms MODIFIER_<ACCESS>_<PROTECTION> mean the NULLABLE variant,
// because nullable is the default. MODIFIER_DEFAULT is private/plain/nullable.
// The macros are declaration sugar, not the internal representation: the value
// is still the orthogonal struct below, so `Modifier_validate` can reject a
// forged axis and `Modifier_toString` can print the canonical token.
//
// THREAD AFFINITY: a Modifier is immutable once constructed and copied by
// value, so any thread may read a copy without synchronization. It is
// schema-definition-time data (cold path); it is never part of a row value.
// Changing a modifier is a SCHEMA CHANGE — a versioned migration — never a
// runtime field mutation. There is deliberately no Modifier_setProtection: the
// field declaration carries the modifier, and the schema layer decides when a
// declaration may change.

typedef enum ModifierAccess {
    MODIFIER_ACCESS_PRIVATE = 0, // processor/private API only (default)
    MODIFIER_ACCESS_PUBLIC = 1,  // reachable through the public API
} ModifierAccess;

typedef enum ModifierProtection {
    MODIFIER_PROTECTION_PLAIN = 0,         // stored as-is (default)
    MODIFIER_PROTECTION_HASHED = 1,        // one-way verifier
    MODIFIER_PROTECTION_CLIENT_SEALED = 2, // reversible; the client holds the key
    MODIFIER_PROTECTION_SERVER_SEALED = 3, // reversible; the processor holds the key
    MODIFIER_PROTECTION_TRUNCATED = 4,     // irreversible loss (explicit opt-in)
} ModifierProtection;

typedef enum ModifierNullability {
    MODIFIER_NULLABLE = 0, // default
    MODIFIER_NONNULL = 1,
} ModifierNullability;

// Cold rejection codes. MODIFIER_OK is success; a Modifier carries its last
// code so a value-returning constructor or parse can report without a
// destination parameter.
typedef enum ModifierCode {
    MODIFIER_OK = 0,
    MODIFIER_INVALID = 1,         // null self or an illegal argument
    MODIFIER_BAD_ACCESS = 2,      // access outside the enum
    MODIFIER_BAD_PROTECTION = 3,  // protection outside the enum
    MODIFIER_BAD_NULLABILITY = 4, // nullability outside the enum
    MODIFIER_BAD_TOKEN = 5,       // unknown, empty or overlong modifier token
    MODIFIER_DUPLICATE = 6,       // the same axis was named twice
} ModifierCode;

// Policy advisories, NOT rejections. A public content hash is legitimate, so
// these are flags the schema layer may surface; they never make a Modifier
// structurally invalid.
typedef enum ModifierWarning {
    MODIFIER_WARNING_NONE = 0u,
    MODIFIER_WARNING_PUBLIC_VERIFIER = 1u << 0,     // public + hashed
    MODIFIER_WARNING_TRANSIENT_PROTECTED = 1u << 1, // transient + protected
    MODIFIER_WARNING_SHARED_TRANSIENT = 1u << 2,    // static + transient
    MODIFIER_WARNING_LOSSY = 1u << 3,               // truncated: irreversible
} ModifierWarning;

// STRUCT FIELDS (mirrored by the ;;OVERVIEW in oop/modifier.c, in this order)
typedef struct Modifier {
    uint32_t access;      // ModifierAccess
    uint32_t protection;  // ModifierProtection
    uint32_t nullability; // ModifierNullability
    bool transient;       // not persisted: in-memory only
    bool shared;          // "static": shared across rows
    int32_t lastCode;     // last ModifierCode
} Modifier;

#define MODIFIER_TOKEN_MAX   32u // one token, including its NUL
#define MODIFIER_TOKEN_LIMIT  8u // more names than axes is a schema error

// --- Declaration-site presets ---------------------------------------------
// MODIFIER_PRESET_ is the internal builder; every MODIFIER_* below is one
// combination. All 80 are present; the ten short forms imply NULLABLE.
#define MODIFIER_PRESET_(ACCESS, PROTECTION, NULLABILITY, TRANSIENT, SHARED) \
    ((Modifier){ .access = (ACCESS), .protection = (PROTECTION), \
                 .nullability = (NULLABILITY), .transient = (TRANSIENT), \
                 .shared = (SHARED), .lastCode = MODIFIER_OK })

#define MODIFIER_PRIVATE_PLAIN_NULLABLE MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_PLAIN, MODIFIER_NULLABLE, false, false)
#define MODIFIER_PRIVATE_PLAIN_NULLABLE_TRANSIENT MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_PLAIN, MODIFIER_NULLABLE, true, false)
#define MODIFIER_PRIVATE_PLAIN_NULLABLE_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_PLAIN, MODIFIER_NULLABLE, false, true)
#define MODIFIER_PRIVATE_PLAIN_NULLABLE_TRANSIENT_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_PLAIN, MODIFIER_NULLABLE, true, true)
#define MODIFIER_PRIVATE_PLAIN_NONNULL MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_PLAIN, MODIFIER_NONNULL, false, false)
#define MODIFIER_PRIVATE_PLAIN_NONNULL_TRANSIENT MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_PLAIN, MODIFIER_NONNULL, true, false)
#define MODIFIER_PRIVATE_PLAIN_NONNULL_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_PLAIN, MODIFIER_NONNULL, false, true)
#define MODIFIER_PRIVATE_PLAIN_NONNULL_TRANSIENT_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_PLAIN, MODIFIER_NONNULL, true, true)
#define MODIFIER_PRIVATE_HASHED_NULLABLE MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_HASHED, MODIFIER_NULLABLE, false, false)
#define MODIFIER_PRIVATE_HASHED_NULLABLE_TRANSIENT MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_HASHED, MODIFIER_NULLABLE, true, false)
#define MODIFIER_PRIVATE_HASHED_NULLABLE_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_HASHED, MODIFIER_NULLABLE, false, true)
#define MODIFIER_PRIVATE_HASHED_NULLABLE_TRANSIENT_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_HASHED, MODIFIER_NULLABLE, true, true)
#define MODIFIER_PRIVATE_HASHED_NONNULL MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_HASHED, MODIFIER_NONNULL, false, false)
#define MODIFIER_PRIVATE_HASHED_NONNULL_TRANSIENT MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_HASHED, MODIFIER_NONNULL, true, false)
#define MODIFIER_PRIVATE_HASHED_NONNULL_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_HASHED, MODIFIER_NONNULL, false, true)
#define MODIFIER_PRIVATE_HASHED_NONNULL_TRANSIENT_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_HASHED, MODIFIER_NONNULL, true, true)
#define MODIFIER_PRIVATE_CLIENT_SEALED_NULLABLE MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_CLIENT_SEALED, MODIFIER_NULLABLE, false, false)
#define MODIFIER_PRIVATE_CLIENT_SEALED_NULLABLE_TRANSIENT MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_CLIENT_SEALED, MODIFIER_NULLABLE, true, false)
#define MODIFIER_PRIVATE_CLIENT_SEALED_NULLABLE_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_CLIENT_SEALED, MODIFIER_NULLABLE, false, true)
#define MODIFIER_PRIVATE_CLIENT_SEALED_NULLABLE_TRANSIENT_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_CLIENT_SEALED, MODIFIER_NULLABLE, true, true)
#define MODIFIER_PRIVATE_CLIENT_SEALED_NONNULL MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_CLIENT_SEALED, MODIFIER_NONNULL, false, false)
#define MODIFIER_PRIVATE_CLIENT_SEALED_NONNULL_TRANSIENT MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_CLIENT_SEALED, MODIFIER_NONNULL, true, false)
#define MODIFIER_PRIVATE_CLIENT_SEALED_NONNULL_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_CLIENT_SEALED, MODIFIER_NONNULL, false, true)
#define MODIFIER_PRIVATE_CLIENT_SEALED_NONNULL_TRANSIENT_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_CLIENT_SEALED, MODIFIER_NONNULL, true, true)
#define MODIFIER_PRIVATE_SERVER_SEALED_NULLABLE MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_SERVER_SEALED, MODIFIER_NULLABLE, false, false)
#define MODIFIER_PRIVATE_SERVER_SEALED_NULLABLE_TRANSIENT MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_SERVER_SEALED, MODIFIER_NULLABLE, true, false)
#define MODIFIER_PRIVATE_SERVER_SEALED_NULLABLE_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_SERVER_SEALED, MODIFIER_NULLABLE, false, true)
#define MODIFIER_PRIVATE_SERVER_SEALED_NULLABLE_TRANSIENT_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_SERVER_SEALED, MODIFIER_NULLABLE, true, true)
#define MODIFIER_PRIVATE_SERVER_SEALED_NONNULL MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_SERVER_SEALED, MODIFIER_NONNULL, false, false)
#define MODIFIER_PRIVATE_SERVER_SEALED_NONNULL_TRANSIENT MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_SERVER_SEALED, MODIFIER_NONNULL, true, false)
#define MODIFIER_PRIVATE_SERVER_SEALED_NONNULL_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_SERVER_SEALED, MODIFIER_NONNULL, false, true)
#define MODIFIER_PRIVATE_SERVER_SEALED_NONNULL_TRANSIENT_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_SERVER_SEALED, MODIFIER_NONNULL, true, true)
#define MODIFIER_PRIVATE_TRUNCATED_NULLABLE MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_TRUNCATED, MODIFIER_NULLABLE, false, false)
#define MODIFIER_PRIVATE_TRUNCATED_NULLABLE_TRANSIENT MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_TRUNCATED, MODIFIER_NULLABLE, true, false)
#define MODIFIER_PRIVATE_TRUNCATED_NULLABLE_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_TRUNCATED, MODIFIER_NULLABLE, false, true)
#define MODIFIER_PRIVATE_TRUNCATED_NULLABLE_TRANSIENT_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_TRUNCATED, MODIFIER_NULLABLE, true, true)
#define MODIFIER_PRIVATE_TRUNCATED_NONNULL MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_TRUNCATED, MODIFIER_NONNULL, false, false)
#define MODIFIER_PRIVATE_TRUNCATED_NONNULL_TRANSIENT MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_TRUNCATED, MODIFIER_NONNULL, true, false)
#define MODIFIER_PRIVATE_TRUNCATED_NONNULL_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_TRUNCATED, MODIFIER_NONNULL, false, true)
#define MODIFIER_PRIVATE_TRUNCATED_NONNULL_TRANSIENT_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PRIVATE, MODIFIER_PROTECTION_TRUNCATED, MODIFIER_NONNULL, true, true)
#define MODIFIER_PUBLIC_PLAIN_NULLABLE MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_PLAIN, MODIFIER_NULLABLE, false, false)
#define MODIFIER_PUBLIC_PLAIN_NULLABLE_TRANSIENT MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_PLAIN, MODIFIER_NULLABLE, true, false)
#define MODIFIER_PUBLIC_PLAIN_NULLABLE_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_PLAIN, MODIFIER_NULLABLE, false, true)
#define MODIFIER_PUBLIC_PLAIN_NULLABLE_TRANSIENT_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_PLAIN, MODIFIER_NULLABLE, true, true)
#define MODIFIER_PUBLIC_PLAIN_NONNULL MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_PLAIN, MODIFIER_NONNULL, false, false)
#define MODIFIER_PUBLIC_PLAIN_NONNULL_TRANSIENT MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_PLAIN, MODIFIER_NONNULL, true, false)
#define MODIFIER_PUBLIC_PLAIN_NONNULL_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_PLAIN, MODIFIER_NONNULL, false, true)
#define MODIFIER_PUBLIC_PLAIN_NONNULL_TRANSIENT_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_PLAIN, MODIFIER_NONNULL, true, true)
#define MODIFIER_PUBLIC_HASHED_NULLABLE MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_HASHED, MODIFIER_NULLABLE, false, false)
#define MODIFIER_PUBLIC_HASHED_NULLABLE_TRANSIENT MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_HASHED, MODIFIER_NULLABLE, true, false)
#define MODIFIER_PUBLIC_HASHED_NULLABLE_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_HASHED, MODIFIER_NULLABLE, false, true)
#define MODIFIER_PUBLIC_HASHED_NULLABLE_TRANSIENT_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_HASHED, MODIFIER_NULLABLE, true, true)
#define MODIFIER_PUBLIC_HASHED_NONNULL MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_HASHED, MODIFIER_NONNULL, false, false)
#define MODIFIER_PUBLIC_HASHED_NONNULL_TRANSIENT MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_HASHED, MODIFIER_NONNULL, true, false)
#define MODIFIER_PUBLIC_HASHED_NONNULL_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_HASHED, MODIFIER_NONNULL, false, true)
#define MODIFIER_PUBLIC_HASHED_NONNULL_TRANSIENT_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_HASHED, MODIFIER_NONNULL, true, true)
#define MODIFIER_PUBLIC_CLIENT_SEALED_NULLABLE MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_CLIENT_SEALED, MODIFIER_NULLABLE, false, false)
#define MODIFIER_PUBLIC_CLIENT_SEALED_NULLABLE_TRANSIENT MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_CLIENT_SEALED, MODIFIER_NULLABLE, true, false)
#define MODIFIER_PUBLIC_CLIENT_SEALED_NULLABLE_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_CLIENT_SEALED, MODIFIER_NULLABLE, false, true)
#define MODIFIER_PUBLIC_CLIENT_SEALED_NULLABLE_TRANSIENT_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_CLIENT_SEALED, MODIFIER_NULLABLE, true, true)
#define MODIFIER_PUBLIC_CLIENT_SEALED_NONNULL MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_CLIENT_SEALED, MODIFIER_NONNULL, false, false)
#define MODIFIER_PUBLIC_CLIENT_SEALED_NONNULL_TRANSIENT MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_CLIENT_SEALED, MODIFIER_NONNULL, true, false)
#define MODIFIER_PUBLIC_CLIENT_SEALED_NONNULL_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_CLIENT_SEALED, MODIFIER_NONNULL, false, true)
#define MODIFIER_PUBLIC_CLIENT_SEALED_NONNULL_TRANSIENT_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_CLIENT_SEALED, MODIFIER_NONNULL, true, true)
#define MODIFIER_PUBLIC_SERVER_SEALED_NULLABLE MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_SERVER_SEALED, MODIFIER_NULLABLE, false, false)
#define MODIFIER_PUBLIC_SERVER_SEALED_NULLABLE_TRANSIENT MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_SERVER_SEALED, MODIFIER_NULLABLE, true, false)
#define MODIFIER_PUBLIC_SERVER_SEALED_NULLABLE_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_SERVER_SEALED, MODIFIER_NULLABLE, false, true)
#define MODIFIER_PUBLIC_SERVER_SEALED_NULLABLE_TRANSIENT_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_SERVER_SEALED, MODIFIER_NULLABLE, true, true)
#define MODIFIER_PUBLIC_SERVER_SEALED_NONNULL MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_SERVER_SEALED, MODIFIER_NONNULL, false, false)
#define MODIFIER_PUBLIC_SERVER_SEALED_NONNULL_TRANSIENT MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_SERVER_SEALED, MODIFIER_NONNULL, true, false)
#define MODIFIER_PUBLIC_SERVER_SEALED_NONNULL_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_SERVER_SEALED, MODIFIER_NONNULL, false, true)
#define MODIFIER_PUBLIC_SERVER_SEALED_NONNULL_TRANSIENT_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_SERVER_SEALED, MODIFIER_NONNULL, true, true)
#define MODIFIER_PUBLIC_TRUNCATED_NULLABLE MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_TRUNCATED, MODIFIER_NULLABLE, false, false)
#define MODIFIER_PUBLIC_TRUNCATED_NULLABLE_TRANSIENT MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_TRUNCATED, MODIFIER_NULLABLE, true, false)
#define MODIFIER_PUBLIC_TRUNCATED_NULLABLE_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_TRUNCATED, MODIFIER_NULLABLE, false, true)
#define MODIFIER_PUBLIC_TRUNCATED_NULLABLE_TRANSIENT_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_TRUNCATED, MODIFIER_NULLABLE, true, true)
#define MODIFIER_PUBLIC_TRUNCATED_NONNULL MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_TRUNCATED, MODIFIER_NONNULL, false, false)
#define MODIFIER_PUBLIC_TRUNCATED_NONNULL_TRANSIENT MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_TRUNCATED, MODIFIER_NONNULL, true, false)
#define MODIFIER_PUBLIC_TRUNCATED_NONNULL_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_TRUNCATED, MODIFIER_NONNULL, false, true)
#define MODIFIER_PUBLIC_TRUNCATED_NONNULL_TRANSIENT_STATIC MODIFIER_PRESET_(MODIFIER_ACCESS_PUBLIC, MODIFIER_PROTECTION_TRUNCATED, MODIFIER_NONNULL, true, true)

// Short forms: the ten access/protection pairs at their default nullability,
// the two bare accesses at plain/nullable, and the zero value.
#define MODIFIER_DEFAULT MODIFIER_PRIVATE_PLAIN_NULLABLE
#define MODIFIER_PRIVATE MODIFIER_PRIVATE_PLAIN_NULLABLE
#define MODIFIER_PUBLIC MODIFIER_PUBLIC_PLAIN_NULLABLE
#define MODIFIER_PRIVATE_PLAIN MODIFIER_PRIVATE_PLAIN_NULLABLE
#define MODIFIER_PRIVATE_HASHED MODIFIER_PRIVATE_HASHED_NULLABLE
#define MODIFIER_PRIVATE_CLIENT_SEALED MODIFIER_PRIVATE_CLIENT_SEALED_NULLABLE
#define MODIFIER_PRIVATE_SERVER_SEALED MODIFIER_PRIVATE_SERVER_SEALED_NULLABLE
#define MODIFIER_PRIVATE_TRUNCATED MODIFIER_PRIVATE_TRUNCATED_NULLABLE
#define MODIFIER_PUBLIC_PLAIN MODIFIER_PUBLIC_PLAIN_NULLABLE
#define MODIFIER_PUBLIC_HASHED MODIFIER_PUBLIC_HASHED_NULLABLE
#define MODIFIER_PUBLIC_CLIENT_SEALED MODIFIER_PUBLIC_CLIENT_SEALED_NULLABLE
#define MODIFIER_PUBLIC_SERVER_SEALED MODIFIER_PUBLIC_SERVER_SEALED_NULLABLE
#define MODIFIER_PUBLIC_TRUNCATED MODIFIER_PUBLIC_TRUNCATED_NULLABLE

// --- Constructors ----------------------------------------------------------
// Arity-dispatched value constructors; the presets above are the usual entry.
Modifier Modifier_0(void);
Modifier Modifier_3(ModifierAccess access, ModifierProtection protection,
                    ModifierNullability nullability);
Modifier Modifier_5(ModifierAccess access, ModifierProtection protection,
                    ModifierNullability nullability, bool transient, bool shared);
// NOTE: like Page(...) and Database(...), this function-like macro expands on
// any `Modifier (` sequence, so a function-pointer declaration must go through
// a typedef: `typedef Modifier ModifierFn(void); ModifierFn *fn;`.
#define Modifier(...) CONSTRUCTOR_DISPATCH(Modifier, __VA_ARGS__)
#define Modifier_zero() Modifier_0()

// --- Validation ------------------------------------------------------------
// Structural validation: are the three axes inside their enums? Returns
// MODIFIER_OK or the offending axis. Never mutates self, never THROWs.
ModifierCode Modifier_validate(Modifier self);
// True when Modifier_validate is MODIFIER_OK.
bool Modifier_isAdmissible(Modifier self);
// Policy advisories (a bitmask of ModifierWarning); never a rejection.
uint32_t Modifier_warnings(Modifier self);

// --- Accessors (a value record; read-only) ---------------------------------
ModifierAccess Modifier_getAccess(Modifier self);
ModifierProtection Modifier_getProtection(Modifier self);
ModifierNullability Modifier_getNullability(Modifier self);
bool Modifier_isTransient(Modifier self);
bool Modifier_isShared(Modifier self);
ModifierCode Modifier_code(Modifier self);

// --- Predicates ------------------------------------------------------------
bool Modifier_isPublic(Modifier self);
bool Modifier_isPrivate(Modifier self);
bool Modifier_isProtected(Modifier self); // protection is not plain
bool Modifier_isReversible(Modifier self); // client-sealed or server-sealed
bool Modifier_isOneWay(Modifier self);     // hashed
bool Modifier_isLossy(Modifier self);      // truncated
bool Modifier_equals(Modifier a, Modifier b); // axes and flags; not lastCode

// --- Naming ----------------------------------------------------------------
const char *Modifier_accessName(ModifierAccess access);
const char *Modifier_protectionName(ModifierProtection protection);
const char *Modifier_nullabilityName(ModifierNullability nullability);
const char *Modifier_codeName(ModifierCode code);
const char *Modifier_warningName(uint32_t warning);

// --- Terminal path ---------------------------------------------------------
// Parse a whitespace-separated modifier list, e.g.
// "public client-sealed nonnull transient". Order is free; naming one axis
// twice rejects as MODIFIER_DUPLICATE; an unknown/overlong token rejects as
// MODIFIER_BAD_TOKEN with one THROW. An empty list is the default Modifier.
Modifier Modifier_parse(const char *text);
// Apply ONE token to a base value; last-wins for that axis. Unknown token
// rejects as MODIFIER_BAD_TOKEN with one THROW.
Modifier Modifier_applyToken(Modifier self, const char *token);

// --- Projections (the toString Law; dest-last per this repo's convention) ---
// Canonical token, e.g. "public:client-sealed:nonnull:transient:static".
bool Modifier_toString(Modifier self, char *dest, size_t capacity, bool *outTruncated);
// By-name dump mirroring STRUCT FIELDS, one layer only.
bool Modifier_toStringStruct(Modifier self, char *dest, size_t capacity, bool *outTruncated);

#endif
