#include "page/page.h"
#include <stdio.h>
#include <string.h>
#include "annotation/annotation.h"
#include "oop/darkbase_type.h"
#include "exception/throw.h"
#include "nio/mem.h"
#include "util/crc32.h"

;;DEFINITION
/** The page is where a store's durability, its checksumming and its addressing all
 * meet, so it is deliberately the smallest thing that can be written, sealed and
 * verified as one unit. Nothing here knows what a record is: the table that owns the
 * page decides how to lay its payload out, and the page's job is to make one page of
 * bytes either provably intact or provably not.
 *
 * The seal covers the header (excluding the seal field itself) and the in-use
 * payload, which deliberately includes the page's own ID and the store's page size.
 * That is what makes a misplaced page detectable: the caller compares its admitted
 * ID with the expected offset. An ID changed without resealing, or bytes read back
 * under a different page size, fail verification instead of handing the caller
 * plausible garbage.
 *
 * The page owns no memory. It borrows storage from the caller — an arena block, a
 * mapping window, or a stack buffer in a test — so a page can be sealed straight
 * where it lives rather than being copied somewhere to be checked. Computing the
 * checksum inline is not an optimisation to be deferred: the seal has to exist before
 * the bytes are made durable, so a background worker could only seal after the fact.
 *
 * A sealed page is immutable until reset. There is no in-place record edit, because
 * an edit after sealing would either invalidate the seal or force a re-seal of bytes
 * that are already durable. Binding does not touch existing bytes. Cold load first
 * verifies their stored length and seal, then adopts header state atomically under
 * caller-owned serialization; a refusal leaves cached state and bytes intact. */
;;OVERVIEW
/** ========================================================================
 * CLASS: Page (page/page.c)
 * STRUCT FIELDS (Mirroring page/page.h)
 *   u8 *bytes      borrowed page storage, `size` bytes (never owned)
 *   u32 size       the store's page size, power of two (sealed in)
 *   u32 id         this page's number within the file (sealed in)
 *   u32 kind       PageKind
 *   u32 used       admitted in-use payload bytes
 *   i32 lastCode    last PageCode refusal
 *   bool sealed         the seal field is valid
 *   bool dirty          caller changed bytes since the last seal
 *
 * LAYOUT (byte-native, memory order, little-endian host floor)
 *   [0]  u32 magic      PAGE_MAGIC
 *   [4]  u32 id
 *   [8]  u32 kind
 *   [12] u32 used       in-use payload bytes
 *   [16] u32 size       the store's page size
 *   [20] u32 reserved
 *   [24] u32 reserved
 *   [28] u32 seal       CRC32 over bytes [0..28) and the used payload
 *   [32..] payload
 *
 * FUNCTION REGISTRY (exports)
 *   Constructors / lifecycle:
 *     Page *Page_0(void)
 *     Page *Page_2(u8 *bytes, u32 size)
 *     void Page_free(Page *self)
 *     bool Page_sizeValid(u32 size)
 *   Writing / admission:
 *     PageCode Page_format(Page *self, u32 id, u32 kind)
 *     PageCode Page_reset(Page *self, u32 kind)
 *     PageCode Page_append(Page *self, const u8 *src, u32 length)
 *     PageCode Page_seal(Page *self)
 *     PageCode Page_verify(const Page *self)
 *     PageCode Page_load(Page *self)
 *   Accessors:
 *     PageCode Page_code(const Page *self)
 *     u8 *Page_payload(Page *self)
 *     const u8 *Page_payloadOf(const Page *self)
 *     u32 Page_used(const Page *self)
 *     u32 Page_capacity(const Page *self)
 *     u32 Page_freeSpace(const Page *self)
 *     u32 Page_id(const Page *self)
 *     u32 Page_kind(const Page *self)
 *     u32 Page_size(const Page *self)
 *     u32 Page_sealValue(const Page *self)
 *     bool Page_isSealed(const Page *self)
 *     bool Page_isDirty(const Page *self)
 *     void Page_markClean(Page *self)
 *   Naming / projections:
 *     const char *Page_codeName(PageCode code)
 *     const char *Page_kindName(u32 kind)
 *     bool Page_toString(const Page *self, char *dest, bytes capacity,
 *                        bool *outTruncated)
 *     bool Page_toStringStruct(const Page *self, char *dest, bytes capacity,
 *                              bool *outTruncated)
 * PRIVATE HELPERS
 *   static void putU32(u8 *out, u32 value) — memory-order encoder
 *   static u32 getU32(const u8 *in) — memory-order decoder
 *   static u8 *payloadBytes(const Page *self) — borrowed payload area
 *   static u32 computeSeal(const Page *self, u32 used) — bounded CRC
 *   static void writeHeader(Page *self) — initialize an unsealed header
 *   static Page *allocPage(void) — allocate only the Page owner
 *   static bool writeNull(char *dest, bytes capacity, bool *outTruncated)
 *     — bounded null-owner projection
 * CONTRACT: null storage, an illegal size, a bad argument, an overflowing append, a
 * mutation of a sealed page and a damaged page are each refused through THROW with
 * the page left as it was found. `Page_verify` never crashes on arbitrary bytes: a
 * wrong magic, size, bounds, kind or reserved word is INVALID, a wrong checksum is
 * BAD_SEAL. Verification uses stored used, never the cached length. Load preserves
 * all cached fields on failure; on success it adopts id/kind/used, marks sealed and
 * clean and clears lastCode. Caller checks ID/location and excludes concurrent
 * mutation/destruction; borrowed storage must remain live and fully sized.
 * ======================================================================== */

static void putU32(u8 *out, u32 value) {
    out[0] = (u8) value;
    out[1] = (u8) (value >> 8);
    out[2] = (u8) (value >> 16);
    out[3] = (u8) (value >> 24);
}

static u32 getU32(const u8 *in) {
    return (u32) in[0] | ((u32) in[1] << 8) | ((u32) in[2] << 16) |
           ((u32) in[3] << 24);
}

bool Page_sizeValid(u32 size) {
    if (size < PAGE_SIZE_MIN || size > PAGE_SIZE_MAX)
        return false;
    return (size & (size - 1u)) == 0u;   // a power of two
}

static u8 *payloadBytes(const Page *self) {
    return (*self).bytes + PAGE_HEADER_BYTES;
}

static u32 computeSeal(const Page *self, u32 used) {
    u32 crc = Crc32_update(CRC32_SEED, (*self).bytes, PAGE_SEAL_OFFSET);
    return Crc32_finalize(Crc32_update(crc, payloadBytes(self), used));
}

static void writeHeader(Page *self) {
    putU32((*self).bytes + 0u, PAGE_MAGIC);
    putU32((*self).bytes + 4u, (*self).id);
    putU32((*self).bytes + 8u, (*self).kind);
    putU32((*self).bytes + 12u, 0u);          // used: set by append
    putU32((*self).bytes + 16u, (*self).size);
    putU32((*self).bytes + 20u, 0u);
    putU32((*self).bytes + 24u, 0u);
    putU32((*self).bytes + PAGE_SEAL_OFFSET, 0u);   // unsealed
}

static Page *allocPage(void) {
    Page *self = (Page*) Memory_alloc(TYPE_DB_PAGE_SINGLETON, sizeof(Page));
    if (self == nullptr) {
        THROW("Page: allocation refused");
        return nullptr;
    }
    memset(self, 0, sizeof(*self));
    return self;
}

Page *Page_0(void) { return allocPage(); }

Page *Page_2(u8 *bytes, u32 size) {
    if (bytes == nullptr || !Page_sizeValid(size)) {
        THROW("Page_2: null storage or an illegal page size");
        return nullptr;
    }
    Page *self = allocPage();
    if (self == nullptr)
        return nullptr;
    (*self).bytes = bytes;
    (*self).size = size;
    return self;
}

void Page_free(Page *self) {
    if (self != nullptr)
        Memory_free(self);
}

PageCode Page_format(Page *self, u32 id, u32 kind) {
    if (self == nullptr || (*self).bytes == nullptr) {
        THROW("Page_format: no storage is bound");
        return PAGE_INVALID;
    }
    if (kind > PAGE_KIND_CATALOG) {
        THROW("Page_format: unknown page kind");
        (*self).lastCode = PAGE_INVALID;
        return PAGE_INVALID;
    }
    (*self).id = id;
    (*self).kind = kind;
    (*self).used = 0u;
    (*self).sealed = false;
    (*self).dirty = true;
    memset((*self).bytes, 0, (*self).size);
    writeHeader(self);
    (*self).lastCode = PAGE_OK;
    return PAGE_OK;
}

PageCode Page_reset(Page *self, u32 kind) {
    if (self == nullptr || (*self).bytes == nullptr) {
        THROW("Page_reset: no storage is bound");
        return PAGE_INVALID;
    }
    return Page_format(self, (*self).id, kind);
}

PageCode Page_append(Page *self, const u8 *src, u32 length) {
    if (self == nullptr || (*self).bytes == nullptr) {
        THROW("Page_append: no storage is bound");
        return PAGE_INVALID;
    }
    if ((src == nullptr && length != 0u)) {
        THROW("Page_append: null source with a nonzero length");
        (*self).lastCode = PAGE_INVALID;
        return PAGE_INVALID;
    }
    if ((*self).sealed) {
        THROW("Page_append: the page is sealed and immutable");
        (*self).lastCode = PAGE_SEALED;
        return PAGE_SEALED;
    }
    u32 capacity = (*self).size - PAGE_HEADER_BYTES;
    // Checked before a single byte is copied, so a refusal is never a partial append.
    if (length > capacity - (*self).used) {
        THROW("Page_append: the payload does not fit this page");
        (*self).lastCode = PAGE_FULL;
        return PAGE_FULL;
    }
    if (length > 0u)
        memcpy(payloadBytes(self) + (*self).used, src, length);
    (*self).used += length;
    (*self).dirty = true;
    putU32((*self).bytes + 12u, (*self).used);
    (*self).lastCode = PAGE_OK;
    return PAGE_OK;
}

PageCode Page_seal(Page *self) {
    if (self == nullptr || (*self).bytes == nullptr) {
        THROW("Page_seal: no storage is bound");
        return PAGE_INVALID;
    }
    putU32((*self).bytes + PAGE_SEAL_OFFSET, computeSeal(self, (*self).used));
    (*self).sealed = true;
    (*self).dirty = false;
    (*self).lastCode = PAGE_OK;
    return PAGE_OK;
}

PageCode Page_verify(const Page *self) {
    if (self == nullptr || (*self).bytes == nullptr || !Page_sizeValid((*self).size)) {
        THROW("Page_verify: no storage is bound");
        return PAGE_INVALID;
    }
    const u8 *bytes = (*self).bytes;
    if (getU32(bytes + 0u) != PAGE_MAGIC) {
        // Not a page at all: refuse rather than interpret arbitrary bytes.
        THROW("Page_verify: the page magic does not match");
        return PAGE_INVALID;
    }
    u32 storedSize = getU32(bytes + 16u);
    if (storedSize != (*self).size || !Page_sizeValid(storedSize)) {
        // A mismatched size means this is not the page the store thought it read.
        THROW("Page_verify: the page size does not match this store");
        return PAGE_INVALID;
    }
    u32 used = getU32(bytes + 12u);
    if (used > (*self).size - PAGE_HEADER_BYTES) {
        THROW("Page_verify: the recorded payload length cannot fit the page");
        return PAGE_INVALID;
    }
    if (getU32(bytes + 8u) > PAGE_KIND_CATALOG ||
        getU32(bytes + 20u) != 0u || getU32(bytes + 24u) != 0u) {
        THROW("Page_verify: unknown kind or nonzero reserved header words");
        return PAGE_INVALID;
    }
    if (getU32(bytes + PAGE_SEAL_OFFSET) != computeSeal(self, used)) {
        THROW("Page_verify: the page seal does not match its bytes");
        return PAGE_BAD_SEAL;
    }
    return PAGE_OK;
}

PageCode Page_load(Page *self) {
    PageCode code = Page_verify(self);
    if (code != PAGE_OK)
        return code;
    const u8 *bytes = (*self).bytes;
    (*self).id = getU32(bytes + 4u);
    (*self).kind = getU32(bytes + 8u);
    (*self).used = getU32(bytes + 12u);
    (*self).sealed = true;
    (*self).dirty = false;
    (*self).lastCode = PAGE_OK;
    return PAGE_OK;
}

PageCode Page_code(const Page *self) { return self == nullptr ? PAGE_INVALID : (PageCode) (*self).lastCode; }
u8 *Page_payload(Page *self) { return self == nullptr || (*self).bytes == nullptr ? nullptr : payloadBytes(self); }
const u8 *Page_payloadOf(const Page *self) { return self == nullptr || (*self).bytes == nullptr ? nullptr : payloadBytes(self); }
u32 Page_used(const Page *self) { return self == nullptr ? 0u : (*self).used; }
u32 Page_capacity(const Page *self) { return self == nullptr ? 0u : (*self).size - PAGE_HEADER_BYTES; }
u32 Page_freeSpace(const Page *self) { return self == nullptr ? 0u : Page_capacity(self) - (*self).used; }
u32 Page_id(const Page *self) { return self == nullptr ? 0u : (*self).id; }
u32 Page_kind(const Page *self) { return self == nullptr ? 0u : (*self).kind; }
u32 Page_size(const Page *self) { return self == nullptr ? 0u : (*self).size; }
u32 Page_sealValue(const Page *self) {
    return self == nullptr || (*self).bytes == nullptr ? 0u : getU32((*self).bytes + PAGE_SEAL_OFFSET);
}
bool Page_isSealed(const Page *self) { return self != nullptr && (*self).sealed; }
bool Page_isDirty(const Page *self) { return self != nullptr && (*self).dirty; }
void Page_markClean(Page *self) { if (self != nullptr) (*self).dirty = false; }

const char *Page_codeName(PageCode code) {
    switch (code) {
        case PAGE_OK:         return "ok";
        case PAGE_INVALID:    return "invalid";
        case PAGE_BAD_SEAL:   return "bad-seal";
        case PAGE_FULL:       return "full";
        case PAGE_NOT_SEALED: return "not-sealed";
        case PAGE_SEALED:     return "sealed";
        default:              return "unknown";
    }
}

const char *Page_kindName(u32 kind) {
    switch (kind) {
        case PAGE_KIND_FREE:     return "free";
        case PAGE_KIND_LEAF:     return "leaf";
        case PAGE_KIND_BRANCH:   return "branch";
        case PAGE_KIND_OVERFLOW: return "overflow";
        case PAGE_KIND_CATALOG:  return "catalog";
        default:                 return "unknown";
    }
}

static bool writeNull(char *dest, bytes capacity, bool *outTruncated) {
    const char *text = "nullptr";
    bytes length = strlen(text);
    bool cut = length + 1u > capacity;
    bytes copy = cut ? capacity - 1u : length;
    if (capacity > 0u) {
        memcpy(dest, text, copy);
        dest[copy] = '\0';
    }
    if (outTruncated != nullptr)
        *outTruncated = cut;
    return !cut;
}

bool Page_toString(const Page *self, char *dest, bytes capacity, bool *outTruncated) {
    if (dest == nullptr || capacity == 0u) {
        if (outTruncated != nullptr)
            *outTruncated = true;
        return false;
    }

    if (self == nullptr)
        return writeNull(dest, capacity, outTruncated);
    int written = snprintf(dest, capacity, "Page(id=%u, kind=%s, used=%u/%u, sealed=%d)",
                           Page_id(self), Page_kindName(Page_kind(self)), Page_used(self),
                           Page_capacity(self), Page_isSealed(self) ? 1 : 0);
    bool cut = written < 0 || (bytes) written >= capacity;
    if (outTruncated != nullptr)
        *outTruncated = cut;
    return !cut;
}

bool Page_toStringStruct(const Page *self, char *dest, bytes capacity, bool *outTruncated) {
    if (dest == nullptr || capacity == 0u) {
        if (outTruncated != nullptr)
            *outTruncated = true;
        return false;
    }

    if (self == nullptr)
        return writeNull(dest, capacity, outTruncated);
    int written = snprintf(dest, capacity,
                           "Page{bytes=%p, size=%u, id=%u, kind=%s, used=%u, lastCode=%s, "
                           "sealed=%d, dirty=%d, free=%u, seal=%u}",
                           (void*) (*self).bytes, Page_size(self), Page_id(self),
                           Page_kindName(Page_kind(self)), Page_used(self),
                           Page_codeName(Page_code(self)), Page_isSealed(self) ? 1 : 0,
                           Page_isDirty(self) ? 1 : 0, Page_freeSpace(self), Page_sealValue(self));
    bool cut = written < 0 || (bytes) written >= capacity;
    if (outTruncated != nullptr)
        *outTruncated = cut;
    return !cut;
}
