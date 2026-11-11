#include "protocol/headless_protocol.h"
#include "annotation/annotation.h"
#include "exception/throw.h"
#include <stdio.h>
#include <string.h>

;;DEFINITION
/** HeadlessProtocol is the R4 wire envelope, independent of sockets and store
 * layout. A host feeds bounded receive slices into one externally serialized
 * owner per connection. Caller-owned backing holds exactly one admitted frame;
 * no allocation, callback, socket, wait, password check or database action occurs.
 * The host retains unconsumed stream tails until the response is sent and the
 * frame released. Syntax rejection latches failure: close the stream, never scan
 * attacker bytes for resynchronization. Reset is an explicit new stream epoch.
 * A valid envelope is NOT authentication or permission to execute its operation.
 * All wire integers are explicit little-endian bytes, never cast packed structs. */
;;OVERVIEW
/** ========================================================================
 * CLASS: HeadlessProtocol (protocol/headless_protocol.c)
 * STRUCT FIELDS (Mirroring protocol/headless_protocol.h)
 *   u8 *buffer           borrowed frame backing, never formatted
 *   bytes capacity           admitted backing extent
 *   bytes used               initialized bytes in current frame
 *   bytes expected           total length decoded from prefix, zero until known
 *   u64 exhaustionCount  saturated capacity-refusal count this reset epoch
 *   u32 code             latest typed local result
 *   bool ready                full validated envelope admitted
 *   bool failed               stream cannot continue without explicit reset
 * FUNCTION REGISTRY — exports:
 * CONSTRUCTORS:
 *   HeadlessProtocol HeadlessProtocol_0(void)
 *   HeadlessProtocol HeadlessProtocol_2(u8 *buffer, bytes capacity)
 * CORE / LIFECYCLE:
 *   bool HeadlessProtocol_feed(HeadlessProtocol *self, const u8 *bytes,
 *                              bytes length, bytes *outConsumed)
 *   bool HeadlessProtocol_finish(HeadlessProtocol *self)
 *   bool HeadlessProtocol_release(HeadlessProtocol *self)
 *   void HeadlessProtocol_reset(HeadlessProtocol *self)
 * QUERIES / BORROWED VIEWS:
 *   bool HeadlessProtocol_isReady(const HeadlessProtocol *self)
 *   bool HeadlessProtocol_isFailed(const HeadlessProtocol *self)
 *   u32 HeadlessProtocol_getCode(const HeadlessProtocol *self)
 *   u64 HeadlessProtocol_getExhaustionCount(const HeadlessProtocol *self)
 *   bytes HeadlessProtocol_getCapacity(const HeadlessProtocol *self)
 *   bytes HeadlessProtocol_getUsed(const HeadlessProtocol *self)
 *   bytes HeadlessProtocol_getExpected(const HeadlessProtocol *self)
 *   u16 HeadlessProtocol_getKind(const HeadlessProtocol *self)
 *   u16 HeadlessProtocol_getOperation(const HeadlessProtocol *self)
 *   u16 HeadlessProtocol_getStatus(const HeadlessProtocol *self)
 *   u32 HeadlessProtocol_getRequestId(const HeadlessProtocol *self)
 *   const u8 *HeadlessProtocol_getPayload(const HeadlessProtocol *self)
 *   bytes HeadlessProtocol_getPayloadLength(const HeadlessProtocol *self)
 * ENCODING / PROJECTIONS:
 *   bool HeadlessProtocol_encode(u16 kind, u16 operation,
 *       u16 status, u32 requestId, const u8 *payload, bytes length,
 *       u8 *dest, bytes capacity, bytes *outLength, bool *outTruncated)
 *   bool HeadlessProtocol_toString(const HeadlessProtocol *self, char *dest,
 *                                  bytes capacity, bool *outTruncated)
 *   bool HeadlessProtocol_toStringStruct(const HeadlessProtocol *self, char *dest,
 *                                        bytes capacity, bool *outTruncated)
 * PRIVATE FUNCTIONS:
 *   read16/read32, write16/write32 — bytewise unaligned wire access
 *   validHeader — v1 kind/status/identity invariant, unknown operations opaque
 *   reject — typed cold refusal, terminal latch when requested
 *   project — bounded redacted state formatting
 * CONTRACT: live/disjoint spans and external serialization are preconditions;
 * invalid API inputs preserve frame, wire errors fail closed. No arbitrary field
 * setters: lifecycle alone changes initialized bytes and borrowed views under
 * the Conflict Triage Law and Single Class Per File Law (Java Law). Projections
 * expose counts/flags only, never bytes, identities, credentials or addresses.
 * ======================================================================== */

;;INTENTION("Per the Conflict Triage Law and Single Class Per File Law (Java Law), derived frame counts/backing/readiness change only through constructor/feed/release/reset; setters cannot forge initialized bytes or payload lifetime.")

static u16 read16(const u8 *p) {
    return (u16) ((u16) p[0] | ((u16) p[1] << 8u));
}

static u32 read32(const u8 *p) {
    return (u32) p[0] | ((u32) p[1] << 8u) |
        ((u32) p[2] << 16u) | ((u32) p[3] << 24u);
}

static void write16(u16 value, u8 *p) {
    p[0] = (u8) value;
    p[1] = (u8) (value >> 8u);
}

static void write32(u32 value, u8 *p) {
    for (unsigned i = 0u; i < 4u; ++i)
        p[i] = (u8) (value >> (i * 8u));
}

static bool validHeader(u16 kind, u16 operation, u16 status, u32 id) {
    return id != 0u && operation != 0u &&
        ((kind == HEADLESS_WIRE_REQUEST && status == HEADLESS_WIRE_OK) ||
         (kind == HEADLESS_WIRE_RESPONSE && status <= HEADLESS_WIRE_INTERNAL_ERROR));
}

static bool reject(HeadlessProtocol *self, u32 code, bool terminal) {
    if (self != nullptr) {
        (*self).code = code;
        (*self).failed = (*self).failed || terminal;
        if (code == HEADLESS_PROTOCOL_CAPACITY) {
            if ((*self).exhaustionCount != UINT64_MAX)
                ++(*self).exhaustionCount;
            if ((*self).exhaustionCount > 1u)
                return false;
            THROW("HeadlessProtocol: frame request %zu exceeds backing bound %zu",
                  (*self).expected, (*self).capacity);
            return false;
        }
    }
    THROW("HeadlessProtocol: rejected code %u", code);
    return false;
}

HeadlessProtocol HeadlessProtocol_0(void) {
    return (HeadlessProtocol) {0};
}

HeadlessProtocol HeadlessProtocol_2(u8 *buffer, bytes capacity) {
    HeadlessProtocol self = HeadlessProtocol_0();
    if (buffer == nullptr || capacity < HEADLESS_WIRE_HEADER_BYTES ||
        capacity > HEADLESS_WIRE_MAX_BYTES) {
        reject(&self, HEADLESS_PROTOCOL_INVALID, true);
        return self;
    }
    self.buffer = buffer;
    self.capacity = capacity;
    return self;
}

bool HeadlessProtocol_feed(HeadlessProtocol *self, const u8 *bytes,
                           bytes length, bytes *outConsumed) {
    if (outConsumed != nullptr)
        *outConsumed = 0u;
    if (self == nullptr || outConsumed == nullptr || (bytes == nullptr && length != 0u))
        return reject(self, HEADLESS_PROTOCOL_INVALID, false);
    if ((*self).failed) {
        if ((*self).code == HEADLESS_PROTOCOL_CAPACITY)
            return reject(self, HEADLESS_PROTOCOL_CAPACITY, true);
        return false; // original terminal rejection already reported
    }
    if ((*self).buffer == nullptr)
        return reject(self, HEADLESS_PROTOCOL_CLOSED, false);
    if ((*self).ready)
        return reject(self, HEADLESS_PROTOCOL_BUSY, false);
    while (*outConsumed < length) {
        bytes target = (*self).expected == 0u ? HEADLESS_WIRE_PREFIX_BYTES : (*self).expected;
        bytes copy = target - (*self).used;
        bytes remaining = length - *outConsumed;
        if (copy > remaining)
            copy = remaining;
        memcpy((*self).buffer + (*self).used, bytes + *outConsumed, copy);
        (*self).used += copy;
        *outConsumed += copy;
        if ((*self).expected == 0u && (*self).used == HEADLESS_WIRE_PREFIX_BYTES) {
            u32 body = read32((*self).buffer);
            if (body < HEADLESS_WIRE_HEADER_BYTES - HEADLESS_WIRE_PREFIX_BYTES ||
                body > HEADLESS_WIRE_MAX_BYTES - HEADLESS_WIRE_PREFIX_BYTES)
                return reject(self, HEADLESS_PROTOCOL_LENGTH, true);
            (*self).expected = (bytes) body + HEADLESS_WIRE_PREFIX_BYTES;
            if ((*self).expected > (*self).capacity)
                return reject(self, HEADLESS_PROTOCOL_CAPACITY, true);
        }
        if ((*self).expected != 0u && (*self).used == (*self).expected) {
            const u8 *header = (*self).buffer;
            if (read16(header + 4u) != HEADLESS_WIRE_VERSION)
                return reject(self, HEADLESS_PROTOCOL_VERSION, true);
            if (!validHeader(read16(header + 6u), read16(header + 8u),
                             read16(header + 10u), read32(header + 12u)))
                return reject(self, HEADLESS_PROTOCOL_HEADER, true);
            (*self).ready = true;
            break;
        }
    }
    (*self).code = HEADLESS_PROTOCOL_OK;
    return true;
}

bool HeadlessProtocol_finish(HeadlessProtocol *self) {
    if (self == nullptr || (*self).buffer == nullptr)
        return reject(self, HEADLESS_PROTOCOL_CLOSED, false);
    if ((*self).failed)
        return false;
    if ((*self).used != 0u && !(*self).ready)
        return reject(self, HEADLESS_PROTOCOL_TRUNCATED, true);
    return true;
}

void HeadlessProtocol_reset(HeadlessProtocol *self) {
    if (self == nullptr)
        return;
    if ((*self).used != 0u)
        memset((*self).buffer, 0, (*self).used);
    (*self).used = 0u;
    (*self).expected = 0u;
    (*self).exhaustionCount = 0u;
    (*self).code = HEADLESS_PROTOCOL_OK;
    (*self).ready = false;
    (*self).failed = false;
}

bool HeadlessProtocol_release(HeadlessProtocol *self) {
    if (!HeadlessProtocol_isReady(self))
        return reject(self, HEADLESS_PROTOCOL_INVALID, false);
    u64 count = (*self).exhaustionCount;
    HeadlessProtocol_reset(self);
    (*self).exhaustionCount = count;
    return true;
}

bool HeadlessProtocol_isReady(const HeadlessProtocol *self) {
    return self != nullptr && (*self).ready;
}

bool HeadlessProtocol_isFailed(const HeadlessProtocol *self) {
    return self != nullptr && (*self).failed;
}

u32 HeadlessProtocol_getCode(const HeadlessProtocol *self) {
    return self == nullptr ? HEADLESS_PROTOCOL_INVALID : (*self).code;
}

u64 HeadlessProtocol_getExhaustionCount(const HeadlessProtocol *self) {
    return self == nullptr ? 0u : (*self).exhaustionCount;
}

bytes HeadlessProtocol_getCapacity(const HeadlessProtocol *self) {
    return self == nullptr ? 0u : (*self).capacity;
}

bytes HeadlessProtocol_getUsed(const HeadlessProtocol *self) {
    return self == nullptr ? 0u : (*self).used;
}

bytes HeadlessProtocol_getExpected(const HeadlessProtocol *self) {
    return self == nullptr ? 0u : (*self).expected;
}

u16 HeadlessProtocol_getKind(const HeadlessProtocol *self) {
    return HeadlessProtocol_isReady(self) ? read16((*self).buffer + 6u) : 0u;
}

u16 HeadlessProtocol_getOperation(const HeadlessProtocol *self) {
    return HeadlessProtocol_isReady(self) ? read16((*self).buffer + 8u) : 0u;
}

u16 HeadlessProtocol_getStatus(const HeadlessProtocol *self) {
    return HeadlessProtocol_isReady(self) ? read16((*self).buffer + 10u) : 0u;
}

u32 HeadlessProtocol_getRequestId(const HeadlessProtocol *self) {
    return HeadlessProtocol_isReady(self) ? read32((*self).buffer + 12u) : 0u;
}

const u8 *HeadlessProtocol_getPayload(const HeadlessProtocol *self) {
    return HeadlessProtocol_isReady(self) ? (*self).buffer + HEADLESS_WIRE_HEADER_BYTES : nullptr;
}

bytes HeadlessProtocol_getPayloadLength(const HeadlessProtocol *self) {
    return HeadlessProtocol_isReady(self) ? (*self).used - HEADLESS_WIRE_HEADER_BYTES : 0u;
}

bool HeadlessProtocol_encode(u16 kind, u16 operation, u16 status,
    u32 requestId, const u8 *payload, bytes length,
    u8 *dest, bytes capacity, bytes *outLength, bool *outTruncated) {
    if (outLength != nullptr)
        *outLength = 0u;
    if (outTruncated != nullptr)
        *outTruncated = false;
    if (dest == nullptr || outLength == nullptr || outTruncated == nullptr ||
        (payload == nullptr && length != 0u) ||
        length > HEADLESS_WIRE_MAX_BYTES - HEADLESS_WIRE_HEADER_BYTES ||
        !validHeader(kind, operation, status, requestId))
        return reject(nullptr, HEADLESS_PROTOCOL_INVALID, false);
    bytes total = HEADLESS_WIRE_HEADER_BYTES + length;
    if (capacity < total) {
        *outTruncated = true;
        return reject(nullptr, HEADLESS_PROTOCOL_CAPACITY, false);
    }
    write32((u32) (total - HEADLESS_WIRE_PREFIX_BYTES), dest);
    write16(HEADLESS_WIRE_VERSION, dest + 4u);
    write16(kind, dest + 6u);
    write16(operation, dest + 8u);
    write16(status, dest + 10u);
    write32(requestId, dest + 12u);
    if (length != 0u)
        memcpy(dest + HEADLESS_WIRE_HEADER_BYTES, payload, length);
    *outLength = total;
    return true;
}

static bool project(const HeadlessProtocol *self, bool structure, char *dest,
                    bytes capacity, bool *outTruncated) {
    if (outTruncated != nullptr)
        *outTruncated = false;
    if (dest == nullptr || capacity == 0u) {
        if (outTruncated != nullptr)
            *outTruncated = true;
        return false;
    }
    int length;
    if (self == nullptr)
        length = snprintf(dest, capacity, "nullptr");
    else if (structure)
        length = snprintf(dest, capacity,
            "HeadlessProtocol{buffer:<redacted>,capacity:%zu,used:%zu,expected:%zu,exhaustionCount:%llu,code:%u,ready:%u,failed:%u}",
            (*self).capacity, (*self).used, (*self).expected,
            (unsigned long long) (*self).exhaustionCount, (*self).code,
            (unsigned) (*self).ready, (unsigned) (*self).failed);
    else
        length = snprintf(dest, capacity, "HeadlessProtocol{ready:%u,failed:%u}",
                           (unsigned) (*self).ready, (unsigned) (*self).failed);
    bool complete = length >= 0 && (bytes) length < capacity;
    if (outTruncated != nullptr)
        *outTruncated = !complete;
    return complete;
}

bool HeadlessProtocol_toString(const HeadlessProtocol *self, char *dest,
                               bytes capacity, bool *outTruncated) {
    return project(self, false, dest, capacity, outTruncated);
}

bool HeadlessProtocol_toStringStruct(const HeadlessProtocol *self, char *dest,
                                     bytes capacity, bool *outTruncated) {
    return project(self, true, dest, capacity, outTruncated);
}
