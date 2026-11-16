#include "vex.h"
#include "protocol/headless_protocol.h"
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
 *   HeadlessProtocol HeadlessProtocol_0()
 *   HeadlessProtocol HeadlessProtocol_2(u8 *buffer, bytes capacity)
 * CORE / LIFECYCLE:
 *   bool HeadlessProtocol_feed(HeadlessProtocol *self, const u8 *bytes,
 *                              bytes length, bytes *outConsumed)
 *   bool HeadlessProtocol_finish(HeadlessProtocol *self)
 *   bool HeadlessProtocol_release(HeadlessProtocol *self)
 *   fn HeadlessProtocol_reset(HeadlessProtocol *self)
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

;;GETTER
static inline u8 *HeadlessProtocol_buffer(const HeadlessProtocol *self) { return (*self).buffer; }
;;GETTER
static inline bytes HeadlessProtocol_capacity(const HeadlessProtocol *self) { return (*self).capacity; }
;;GETTER
static inline bytes HeadlessProtocol_used(const HeadlessProtocol *self) { return (*self).used; }
;;SETTER
static inline fn HeadlessProtocol_setUsed(HeadlessProtocol *self, bytes used) { (*self).used = used; }
;;GETTER
static inline bytes HeadlessProtocol_expected(const HeadlessProtocol *self) { return (*self).expected; }
;;SETTER
static inline fn HeadlessProtocol_setExpected(HeadlessProtocol *self, bytes expected) { (*self).expected = expected; }
;;GETTER
static inline u64 HeadlessProtocol_exhaustionCount(const HeadlessProtocol *self) { return (*self).exhaustionCount; }
;;SETTER
static inline fn HeadlessProtocol_setExhaustionCount(HeadlessProtocol *self, u64 count) { (*self).exhaustionCount = count; }
;;GETTER
static inline u32 HeadlessProtocol_code(const HeadlessProtocol *self) { return (*self).code; }
;;SETTER
static inline fn HeadlessProtocol_setCode(HeadlessProtocol *self, u32 code) { (*self).code = code; }
;;GETTER
static inline bool HeadlessProtocol_ready(const HeadlessProtocol *self) { return (*self).ready; }
;;SETTER
static inline fn HeadlessProtocol_setReady(HeadlessProtocol *self, bool ready) { (*self).ready = ready; }
;;GETTER
static inline bool HeadlessProtocol_failed(const HeadlessProtocol *self) { return (*self).failed; }
;;SETTER
static inline fn HeadlessProtocol_setFailed(HeadlessProtocol *self, bool failed) { (*self).failed = failed; }

static u16 read16(const u8 *p) {
    return (u16) ((u16) p[0] | ((u16) p[1] << 8u));
}

static u32 read32(const u8 *p) {
    return (u32) p[0] | ((u32) p[1] << 8u) |
        ((u32) p[2] << 16u) | ((u32) p[3] << 24u);
}

static fn write16(u16 value, u8 *p) {
    p[0] = (u8) value;
    p[1] = (u8) (value >> 8u);
}

static fn write32(u32 value, u8 *p) {
    for (u32 i = 0u; i < 4u; ++i)
        p[i] = (u8) (value >> (i * 8u));
}

static bool validHeader(u16 kind, u16 operation, u16 status, u32 id) {
    return id != 0u && operation != 0u &&
        ((kind == HEADLESS_WIRE_REQUEST && status == HEADLESS_WIRE_OK) ||
         (kind == HEADLESS_WIRE_RESPONSE && status <= HEADLESS_WIRE_INTERNAL_ERROR));
}

static bool reject(HeadlessProtocol *self, u32 code, bool terminal) {
    if (self != nullptr) {
        HeadlessProtocol_setCode(self, code);
        HeadlessProtocol_setFailed(self, HeadlessProtocol_failed(self) || terminal);
        if (code == HEADLESS_PROTOCOL_CAPACITY) {
            u64 count = HeadlessProtocol_exhaustionCount(self);
            if (count != UINT64_MAX) {
                ++count;
                HeadlessProtocol_setExhaustionCount(self, count);
            }
            if (count > 1u)
                return false;
            THROW("HeadlessProtocol: frame request %zu exceeds backing bound %zu",
                  HeadlessProtocol_expected(self), HeadlessProtocol_capacity(self));
            return false;
        }
    }
    THROW("HeadlessProtocol: rejected code %u", code);
    return false;
}

HeadlessProtocol HeadlessProtocol_0() {
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

bool HeadlessProtocol_feed(HeadlessProtocol *self, const u8 *data,
                           bytes length, bytes *outConsumed) {
    if (outConsumed != nullptr)
        *outConsumed = 0u;
    if (self == nullptr || outConsumed == nullptr || (data == nullptr && length != 0u))
        return reject(self, HEADLESS_PROTOCOL_INVALID, false);
    if (HeadlessProtocol_failed(self)) {
        if (HeadlessProtocol_code(self) == HEADLESS_PROTOCOL_CAPACITY)
            return reject(self, HEADLESS_PROTOCOL_CAPACITY, true);
        return false; // original terminal rejection already reported
    }
    u8 *buf = HeadlessProtocol_buffer(self);
    if (buf == nullptr)
        return reject(self, HEADLESS_PROTOCOL_CLOSED, false);
    if (HeadlessProtocol_ready(self))
        return reject(self, HEADLESS_PROTOCOL_BUSY, false);
    bytes cap = HeadlessProtocol_capacity(self);
    while (*outConsumed < length) {
        bytes expected = HeadlessProtocol_expected(self);
        bytes used = HeadlessProtocol_used(self);
        bytes target = expected == 0u ? HEADLESS_WIRE_PREFIX_BYTES : expected;
        bytes copy = target - used;
        bytes remaining = length - *outConsumed;
        if (copy > remaining)
            copy = remaining;
        memcpy(buf + used, data + *outConsumed, copy);
        used += copy;
        HeadlessProtocol_setUsed(self, used);
        *outConsumed += copy;
        if (expected == 0u && used == HEADLESS_WIRE_PREFIX_BYTES) {
            u32 body = read32(buf);
            if (body < HEADLESS_WIRE_HEADER_BYTES - HEADLESS_WIRE_PREFIX_BYTES ||
                body > HEADLESS_WIRE_MAX_BYTES - HEADLESS_WIRE_PREFIX_BYTES)
                return reject(self, HEADLESS_PROTOCOL_LENGTH, true);
            expected = (bytes) body + HEADLESS_WIRE_PREFIX_BYTES;
            HeadlessProtocol_setExpected(self, expected);
            if (expected > cap)
                return reject(self, HEADLESS_PROTOCOL_CAPACITY, true);
        }
        if (expected != 0u && used == expected) {
            const u8 *header = buf;
            if (read16(header + 4u) != HEADLESS_WIRE_VERSION)
                return reject(self, HEADLESS_PROTOCOL_VERSION, true);
            if (!validHeader(read16(header + 6u), read16(header + 8u),
                             read16(header + 10u), read32(header + 12u)))
                return reject(self, HEADLESS_PROTOCOL_HEADER, true);
            HeadlessProtocol_setReady(self, true);
            break;
        }
    }
    HeadlessProtocol_setCode(self, HEADLESS_PROTOCOL_OK);
    return true;
}

bool HeadlessProtocol_finish(HeadlessProtocol *self) {
    if (self == nullptr || HeadlessProtocol_buffer(self) == nullptr)
        return reject(self, HEADLESS_PROTOCOL_CLOSED, false);
    if (HeadlessProtocol_failed(self))
        return false;
    if (HeadlessProtocol_used(self) != 0u && !HeadlessProtocol_ready(self))
        return reject(self, HEADLESS_PROTOCOL_TRUNCATED, true);
    return true;
}

fn HeadlessProtocol_reset(HeadlessProtocol *self) {
    if (self == nullptr)
        return;
    bytes used = HeadlessProtocol_used(self);
    if (used != 0u)
        memset(HeadlessProtocol_buffer(self), 0, used);
    HeadlessProtocol_setUsed(self, 0u);
    HeadlessProtocol_setExpected(self, 0u);
    HeadlessProtocol_setExhaustionCount(self, 0u);
    HeadlessProtocol_setCode(self, HEADLESS_PROTOCOL_OK);
    HeadlessProtocol_setReady(self, false);
    HeadlessProtocol_setFailed(self, false);
}

bool HeadlessProtocol_release(HeadlessProtocol *self) {
    if (!HeadlessProtocol_isReady(self))
        return reject(self, HEADLESS_PROTOCOL_INVALID, false);
    u64 count = HeadlessProtocol_exhaustionCount(self);
    HeadlessProtocol_reset(self);
    HeadlessProtocol_setExhaustionCount(self, count);
    return true;
}

bool HeadlessProtocol_isReady(const HeadlessProtocol *self) {
    return self != nullptr && HeadlessProtocol_ready(self);
}

bool HeadlessProtocol_isFailed(const HeadlessProtocol *self) {
    return self != nullptr && HeadlessProtocol_failed(self);
}

u32 HeadlessProtocol_getCode(const HeadlessProtocol *self) {
    return self == nullptr ? HEADLESS_PROTOCOL_INVALID : HeadlessProtocol_code(self);
}

u64 HeadlessProtocol_getExhaustionCount(const HeadlessProtocol *self) {
    return self == nullptr ? 0u : HeadlessProtocol_exhaustionCount(self);
}

bytes HeadlessProtocol_getCapacity(const HeadlessProtocol *self) {
    return self == nullptr ? 0u : HeadlessProtocol_capacity(self);
}

bytes HeadlessProtocol_getUsed(const HeadlessProtocol *self) {
    return self == nullptr ? 0u : HeadlessProtocol_used(self);
}

bytes HeadlessProtocol_getExpected(const HeadlessProtocol *self) {
    return self == nullptr ? 0u : HeadlessProtocol_expected(self);
}

u16 HeadlessProtocol_getKind(const HeadlessProtocol *self) {
    return HeadlessProtocol_isReady(self) ? read16(HeadlessProtocol_buffer(self) + 6u) : 0u;
}

u16 HeadlessProtocol_getOperation(const HeadlessProtocol *self) {
    return HeadlessProtocol_isReady(self) ? read16(HeadlessProtocol_buffer(self) + 8u) : 0u;
}

u16 HeadlessProtocol_getStatus(const HeadlessProtocol *self) {
    return HeadlessProtocol_isReady(self) ? read16(HeadlessProtocol_buffer(self) + 10u) : 0u;
}

u32 HeadlessProtocol_getRequestId(const HeadlessProtocol *self) {
    return HeadlessProtocol_isReady(self) ? read32(HeadlessProtocol_buffer(self) + 12u) : 0u;
}

const u8 *HeadlessProtocol_getPayload(const HeadlessProtocol *self) {
    return HeadlessProtocol_isReady(self) ? HeadlessProtocol_buffer(self) + HEADLESS_WIRE_HEADER_BYTES : nullptr;
}

bytes HeadlessProtocol_getPayloadLength(const HeadlessProtocol *self) {
    return HeadlessProtocol_isReady(self) ? HeadlessProtocol_used(self) - HEADLESS_WIRE_HEADER_BYTES : 0u;
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
    i32 length;
    if (self == nullptr)
        length = snprintf(dest, capacity, "nullptr");
    else if (structure)
        length = snprintf(dest, capacity,
            "HeadlessProtocol{buffer:<redacted>,capacity:%zu,used:%zu,expected:%zu,exhaustionCount:%llu,code:%u,ready:%u,failed:%u}",
            HeadlessProtocol_capacity(self), HeadlessProtocol_used(self), HeadlessProtocol_expected(self),
            (u64) HeadlessProtocol_exhaustionCount(self), HeadlessProtocol_code(self),
            (u32) HeadlessProtocol_ready(self), (u32) HeadlessProtocol_failed(self));
    else
        length = snprintf(dest, capacity, "HeadlessProtocol{ready:%u,failed:%u}",
                           (u32) HeadlessProtocol_ready(self), (u32) HeadlessProtocol_failed(self));
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
