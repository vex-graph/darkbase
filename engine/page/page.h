#ifndef DARKBASE_PAGE_PAGE_H
#define DARKBASE_PAGE_PAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// page/page.h — the page: the unit of durability, checksumming and addressing.
//
// A store file is a sequence of fixed-size pages. Nothing is read or written in
// units smaller than one page, so a page is also the unit of EVERYTHING that has to
// agree: the seal, the write, and the recovery decision.
//
// The seal is a CRC32 over the header (excluding the seal field) plus the in-use
// payload. It deliberately covers the page's own ID and size, so a page whose
// ID changed without resealing, or which is read back as a different size, fails
// verification instead of being silently accepted as valid data. The caller checks
// the admitted ID against its expected file location; Page has no offset context.
//
// Sizing: the page size is chosen once by the store, is a power of two within
// [PAGE_SIZE_MIN, PAGE_SIZE_MAX], and is recorded in the page header so a mismatch
// is detected rather than assumed away. The page owns NO memory: it borrows storage
// the caller supplies (arena, mapping, or a stack buffer in a test).
//
// Not offered: variable page sizes inside one file, in-place record edits (a sealed
// page is immutable until it is reset), compression, encryption, or any claim about
// what the payload means. Addressing a record inside a page belongs to the table
// that owns the page, not to the page.

#define PAGE_MAGIC          0x47505856u   /* the bytes 'V','X','P','G' in memory order */
#define PAGE_SIZE_DEFAULT   4096u
#define PAGE_SIZE_MIN        512u
#define PAGE_SIZE_MAX      65536u
// magic, id, kind, used, size, two reserved words, then the seal LAST so the sealed
// span is contiguous from byte zero: header[0..28) plus the payload.
#define PAGE_HEADER_BYTES     32u
#define PAGE_SEAL_OFFSET      28u

typedef enum PageCode {
    PAGE_OK = 0,
    PAGE_INVALID = 1,     // null storage, an illegal size, or a bad argument
    PAGE_BAD_SEAL = 2,    // the checksum does not match: the page is damaged
    PAGE_FULL = 3,        // the payload would not fit; nothing was appended
    PAGE_NOT_SEALED = 4,  // the operation needs a sealed page
    PAGE_SEALED = 5,      // the page is sealed and therefore immutable
} PageCode;

typedef enum PageKind {
    PAGE_KIND_FREE = 0,      // released: reusable by the free list
    PAGE_KIND_LEAF = 1,      // holds records
    PAGE_KIND_BRANCH = 2,    // holds child page ids
    PAGE_KIND_OVERFLOW = 3,  // the tail of a value too long for its row
    PAGE_KIND_CATALOG = 4,   // schema and identity rows
} PageKind;

typedef struct Page {
    uint8_t *bytes;      // borrowed page storage (PAGE size bytes)
    uint32_t size;       // the store's page size, power of two
    uint32_t id;         // this page's number within the file (sealed in)
    uint32_t kind;       // PageKind
    uint32_t used;       // in-use payload bytes (mirrors the header field)
    int32_t lastCode;    // last PageCode refusal
    bool sealed;         // the seal field is valid
    bool dirty;          // the caller has changed bytes since the last seal
} Page;

// Is this a legal page size for a store to choose?
bool Page_sizeValid(uint32_t size);

// Arity constructors: Page_0 has no storage; Page_2 binds borrowed storage of
// `size` bytes without changing them or admitting their header. Null on refusal.
// The caller retains the full live buffer and externally serializes all access.
Page *Page_0(void);
Page *Page_2(uint8_t *bytes, uint32_t size);
#define Page(...) CONSTRUCTOR_DISPATCH(Page, __VA_ARGS__)
#define Page_zero() Page_0()

void Page_free(Page *self);

// Start a page in place: zero it, write its header, clear the payload. Unsealed.
PageCode Page_format(Page *self, uint32_t id, uint32_t kind);
// Reuse a page: same id, new kind, payload cleared, unsealed.
PageCode Page_reset(Page *self, uint32_t kind);
// Append payload bytes. Refuses with PAGE_FULL when they do not fit, and in that
// case appends NOTHING — there is no partial append.
PageCode Page_append(Page *self, const uint8_t *src, uint32_t length);
// Store the seal. A sealed page is immutable until it is reset.
PageCode Page_seal(Page *self);
// Cold byte verification, independent of cached id/kind/used/sealed/dirty. INVALID
// for illegal binding size, magic, stored size, used, kind or nonzero reserved
// words; BAD_SEAL for a checksum mismatch. Never mutates bytes or cached state.
PageCode Page_verify(const Page *self);
// Cold admission of existing bytes: verify first, then adopt stored id/kind/used,
// set sealed=true, dirty=false and lastCode=PAGE_OK. On refusal return the code
// without changing ANY cached state (including lastCode) or bytes. Caller checks
// the admitted ID against the expected file location before using the payload.
PageCode Page_load(Page *self);

PageCode    Page_code(const Page *self);
uint8_t    *Page_payload(Page *self);           // borrowed, Page_used bytes long
const uint8_t *Page_payloadOf(const Page *self);
uint32_t    Page_used(const Page *self);
uint32_t    Page_capacity(const Page *self);    // size minus the header
uint32_t    Page_freeSpace(const Page *self);
uint32_t    Page_id(const Page *self);
uint32_t    Page_kind(const Page *self);
uint32_t    Page_size(const Page *self);
uint32_t    Page_sealValue(const Page *self);   // 0 while unsealed
bool        Page_isSealed(const Page *self);
bool        Page_isDirty(const Page *self);
void        Page_markClean(Page *self);         // the caller flushed it

const char *Page_codeName(PageCode code);
const char *Page_kindName(uint32_t kind);

bool Page_toString(const Page *self, char *dest, size_t capacity, bool *outTruncated);
bool Page_toStringStruct(const Page *self, char *dest, size_t capacity, bool *outTruncated);

#endif
