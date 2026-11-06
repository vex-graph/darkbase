#ifndef HEADLESS_PROTOCOL_H
#define HEADLESS_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include "c23/constructor.h"

// V1 wire constants, not storage layout or configurable application defaults.
#define HEADLESS_WIRE_VERSION 1u
#define HEADLESS_WIRE_HEADER_BYTES 16u
#define HEADLESS_WIRE_PREFIX_BYTES 4u
// Safety bound per frame (including header); not a limit on connection count.
#define HEADLESS_WIRE_MAX_BYTES 65536u
#define HEADLESS_WIRE_REQUEST 1u
#define HEADLESS_WIRE_RESPONSE 2u
#define HEADLESS_WIRE_OK 0u
#define HEADLESS_WIRE_VERSION_MISMATCH 1u
#define HEADLESS_WIRE_BAD_REQUEST 2u
#define HEADLESS_WIRE_UNAUTHENTICATED 3u
#define HEADLESS_WIRE_REQUESTS_DISABLED 4u
#define HEADLESS_WIRE_UNSUPPORTED_OPERATION 5u
#define HEADLESS_WIRE_INTERNAL_ERROR 6u

#define HEADLESS_PROTOCOL_OK 0u
#define HEADLESS_PROTOCOL_INVALID 1u
#define HEADLESS_PROTOCOL_BUSY 2u
#define HEADLESS_PROTOCOL_LENGTH 3u
#define HEADLESS_PROTOCOL_VERSION 4u
#define HEADLESS_PROTOCOL_HEADER 5u
#define HEADLESS_PROTOCOL_CAPACITY 6u
#define HEADLESS_PROTOCOL_TRUNCATED 7u
#define HEADLESS_PROTOCOL_CLOSED 8u

typedef struct HeadlessProtocol {
    uint8_t *buffer;
    size_t capacity;
    size_t used;
    size_t expected;
    uint64_t exhaustionCount;
    uint32_t code;
    bool ready;
    bool failed;
} HeadlessProtocol;

// _0 is closed. _2 borrows disjoint writable storage, 16..65536 bytes.
// External serialization; buffer and all payload borrows must outlive use.
HeadlessProtocol HeadlessProtocol_0(void);
HeadlessProtocol HeadlessProtocol_2(uint8_t *buffer, size_t capacity);
#define HeadlessProtocol(...) CONSTRUCTOR_DISPATCH(HeadlessProtocol, __VA_ARGS__)
#define HeadlessProtocol_zero() HeadlessProtocol_0()

// At most one frame admitted. Stops exactly at its end; caller retains any tail.
// false: inspect code. Malformed/oversize/EOF latch failed until reset. Busy and
// invalid API arguments preserve admitted state. Input must not alias backing.
bool HeadlessProtocol_feed(HeadlessProtocol *self, const uint8_t *bytes,
                           size_t length, size_t *outConsumed);
bool HeadlessProtocol_finish(HeadlessProtocol *self);
// release requires ready and ends payload borrows; reset abandons any frame,
// clears refusal epoch and scrubs used bytes. Neither closes the host transport.
bool HeadlessProtocol_release(HeadlessProtocol *self);
void HeadlessProtocol_reset(HeadlessProtocol *self);
bool HeadlessProtocol_isReady(const HeadlessProtocol *self);
bool HeadlessProtocol_isFailed(const HeadlessProtocol *self);
uint32_t HeadlessProtocol_getCode(const HeadlessProtocol *self);
uint64_t HeadlessProtocol_getExhaustionCount(const HeadlessProtocol *self);
size_t HeadlessProtocol_getCapacity(const HeadlessProtocol *self);
size_t HeadlessProtocol_getUsed(const HeadlessProtocol *self);
size_t HeadlessProtocol_getExpected(const HeadlessProtocol *self);
uint16_t HeadlessProtocol_getKind(const HeadlessProtocol *self);
uint16_t HeadlessProtocol_getOperation(const HeadlessProtocol *self);
uint16_t HeadlessProtocol_getStatus(const HeadlessProtocol *self);
uint32_t HeadlessProtocol_getRequestId(const HeadlessProtocol *self);
const uint8_t *HeadlessProtocol_getPayload(const HeadlessProtocol *self);
size_t HeadlessProtocol_getPayloadLength(const HeadlessProtocol *self);

// Encodes version 1. Nonzero requestId/operation; request status must be OK.
// Payload/destination and all outputs must be disjoint. On rejection destination
// stays unchanged, outLength=0; insufficient capacity sets outTruncated=true.
bool HeadlessProtocol_encode(uint16_t kind, uint16_t operation, uint16_t status,
    uint32_t requestId, const uint8_t *payload, size_t length,
    uint8_t *dest, size_t capacity, size_t *outLength, bool *outTruncated);
bool HeadlessProtocol_toString(const HeadlessProtocol *self, char *dest,
                               size_t capacity, bool *outTruncated);
bool HeadlessProtocol_toStringStruct(const HeadlessProtocol *self, char *dest,
                                     size_t capacity, bool *outTruncated);

#endif
