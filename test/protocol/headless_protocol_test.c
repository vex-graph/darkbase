// HeadlessProtocol owner: byte-level wire oracle, adversarial framing and recovery.
// No sockets or credentials; borrowed spans are live/disjoint, calls serialized.
#include "protocol/headless_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static unsigned diagnostics;
#define REFUSES(expression) do { ++diagnostics; assert(!(expression)); } while (false)

// Known byte oracle, all stream splits, retained tail, request/response and busy.
static void roundtrip(void) {
    const uint8_t payload[] = {0u, 255u, 's', 'e', 'c', 'r', 'e', 't'};
    const uint8_t golden[] = {20,0,0,0, 1,0, 1,0, 2,0, 0,0, 4,3,2,1,
                              0,255,'s','e','c','r','e','t'};
    uint8_t encoded[128], backing[128];
    size_t length = SIZE_MAX;
    bool truncated = true;
    assert(HeadlessProtocol_encode(HEADLESS_WIRE_REQUEST, 2u, HEADLESS_WIRE_OK,
        0x01020304u, payload, sizeof payload, encoded, sizeof encoded, &length, &truncated));
    assert(length == sizeof golden && !truncated && memcmp(encoded, golden, length) == 0);
    for (size_t split = 0u; split <= length; ++split) {
        HeadlessProtocol protocol = HeadlessProtocol(backing, sizeof backing);
        size_t used = SIZE_MAX;
        assert(HeadlessProtocol_feed(&protocol, encoded, split, &used) && used == split);
        if (split != length)
            assert(HeadlessProtocol_feed(&protocol, encoded + split, length - split, &used));
        assert(HeadlessProtocol_isReady(&protocol));
        assert(HeadlessProtocol_getCapacity(&protocol) == sizeof backing);
        assert(HeadlessProtocol_getUsed(&protocol) == length);
        assert(HeadlessProtocol_getExpected(&protocol) == length);
        assert(!HeadlessProtocol_isFailed(&protocol));
        assert(HeadlessProtocol_getKind(&protocol) == HEADLESS_WIRE_REQUEST);
        assert(HeadlessProtocol_getOperation(&protocol) == 2u);
        assert(HeadlessProtocol_getStatus(&protocol) == HEADLESS_WIRE_OK);
        assert(HeadlessProtocol_getRequestId(&protocol) == 0x01020304u);
        assert(HeadlessProtocol_getPayloadLength(&protocol) == sizeof payload);
        assert(memcmp(HeadlessProtocol_getPayload(&protocol), payload, sizeof payload) == 0);
        assert(HeadlessProtocol_finish(&protocol));
        REFUSES(HeadlessProtocol_feed(&protocol, encoded, length, &used));
        assert(used == 0u && HeadlessProtocol_getCode(&protocol) == HEADLESS_PROTOCOL_BUSY);
        assert(memcmp(HeadlessProtocol_getPayload(&protocol), payload, sizeof payload) == 0);
        assert(HeadlessProtocol_release(&protocol));
        for (size_t i = 0u; i < length; ++i)
            assert(backing[i] == 0u);
        assert(HeadlessProtocol_getPayload(&protocol) == nullptr);
    }
    memcpy(encoded + length, golden, sizeof golden);
    HeadlessProtocol protocol = HeadlessProtocol(backing, sizeof backing);
    size_t used = 0u;
    assert(HeadlessProtocol_feed(&protocol, encoded, length * 2u, &used));
    assert(used == length); // coalesced second frame stays with host until release
    assert(HeadlessProtocol_release(&protocol));
    assert(HeadlessProtocol_feed(&protocol, encoded + used, length, &used));
    assert(HeadlessProtocol_isReady(&protocol));
    HeadlessProtocol_reset(&protocol);
    for (uint16_t status = HEADLESS_WIRE_OK; status <= HEADLESS_WIRE_INTERNAL_ERROR; ++status) {
        assert(HeadlessProtocol_encode(HEADLESS_WIRE_RESPONSE, UINT16_MAX, status,
            UINT32_MAX, nullptr, 0u, encoded, sizeof encoded, &length, &truncated));
        for (size_t i = 0u; i < length; ++i) {
            assert(HeadlessProtocol_feed(&protocol, encoded + i, 1u, &used));
            assert(used == 1u);
        }
        assert(HeadlessProtocol_getStatus(&protocol) == status);
        assert(HeadlessProtocol_getOperation(&protocol) == UINT16_MAX);
        assert(HeadlessProtocol_getRequestId(&protocol) == UINT32_MAX);
        assert(HeadlessProtocol_getPayloadLength(&protocol) == 0u);
        assert(HeadlessProtocol_release(&protocol));
    }
}

// Every truncation, each header constraint, explicit terminal latch and reset.
static void hostile(void) {
    uint8_t encoded[64], backing[64];
    size_t length, used;
    bool truncated;
    assert(HeadlessProtocol_encode(1u, 1u, 0u, 1u, nullptr, 0u,
                                   encoded, sizeof encoded, &length, &truncated));
    for (size_t cut = 1u; cut < length; ++cut) {
        HeadlessProtocol protocol = HeadlessProtocol(backing, sizeof backing);
        assert(HeadlessProtocol_feed(&protocol, encoded, cut, &used));
        REFUSES(HeadlessProtocol_finish(&protocol));
        assert(HeadlessProtocol_getCode(&protocol) == HEADLESS_PROTOCOL_TRUNCATED);
        assert(HeadlessProtocol_isFailed(&protocol));
        assert(!HeadlessProtocol_feed(&protocol, encoded, length, &used));
        assert(used == 0u); // latched rejection is already diagnosed
        HeadlessProtocol_reset(&protocol);
        assert(HeadlessProtocol_finish(&protocol));
        assert(HeadlessProtocol_feed(&protocol, encoded, length, &used));
        assert(HeadlessProtocol_isReady(&protocol));
    }
    const unsigned offsets[] = {0u, 0u, 0u, 4u, 6u, 8u, 10u, 12u};
    const uint8_t values[] = {0u, 11u, 255u, 2u, 3u, 0u, 1u, 0u};
    for (size_t i = 0u; i < sizeof values; ++i) {
        uint8_t bad[64];
        memcpy(bad, encoded, length);
        bad[offsets[i]] = values[i];
        if (i == 2u)
            memset(bad, 255, 4u); // UINT32_MAX prefix never wraps
        HeadlessProtocol protocol = HeadlessProtocol(backing, sizeof backing);
        REFUSES(HeadlessProtocol_feed(&protocol, bad, length, &used));
        assert(HeadlessProtocol_isFailed(&protocol));
        assert(!HeadlessProtocol_isReady(&protocol));
        assert(HeadlessProtocol_getPayload(&protocol) == nullptr);
        if (i == 3u)
            assert(HeadlessProtocol_getCode(&protocol) == HEADLESS_PROTOCOL_VERSION);
    }
    // Legal length exceeding this connection's backing: report once, count always.
    HeadlessProtocol protocol = HeadlessProtocol(backing, HEADLESS_WIRE_HEADER_BYTES);
    encoded[0] = 13u;
    REFUSES(HeadlessProtocol_feed(&protocol, encoded, 4u, &used));
    assert(used == 4u && HeadlessProtocol_getExhaustionCount(&protocol) == 1u);
    assert(!HeadlessProtocol_feed(&protocol, encoded, 4u, &used));
    assert(used == 0u && HeadlessProtocol_getExhaustionCount(&protocol) == 2u);
    HeadlessProtocol_reset(&protocol);
    assert(HeadlessProtocol_getExhaustionCount(&protocol) == 0u);
    encoded[0] = 12u;
    assert(HeadlessProtocol_feed(&protocol, encoded, length, &used));
    assert(HeadlessProtocol_isReady(&protocol));
}

// Exact protocol maximum with full payload comparison and canary; output atomicity.
static void boundaries(void) {
    static uint8_t payload[HEADLESS_WIRE_MAX_BYTES];
    static uint8_t encoded[HEADLESS_WIRE_MAX_BYTES + 1u];
    static uint8_t backing[HEADLESS_WIRE_MAX_BYTES + 1u];
    for (size_t i = 0u; i < sizeof payload; ++i)
        payload[i] = (uint8_t) (i * 37u); // deterministic full retained payload
    size_t length;
    bool truncated;
    size_t maximumPayload = HEADLESS_WIRE_MAX_BYTES - HEADLESS_WIRE_HEADER_BYTES;
    memset(encoded, 0xa5, sizeof encoded);
    memset(backing, 0xa5, sizeof backing);
    assert(HeadlessProtocol_encode(1u, 1u, 0u, 1u, payload, maximumPayload,
                                   encoded, HEADLESS_WIRE_MAX_BYTES, &length, &truncated));
    HeadlessProtocol protocol = HeadlessProtocol(backing, HEADLESS_WIRE_MAX_BYTES);
    size_t used;
    assert(HeadlessProtocol_feed(&protocol, encoded, length, &used));
    assert(used == HEADLESS_WIRE_MAX_BYTES && HeadlessProtocol_isReady(&protocol));
    assert(memcmp(HeadlessProtocol_getPayload(&protocol), payload, maximumPayload) == 0);
    assert(backing[HEADLESS_WIRE_MAX_BYTES] == 0xa5 && encoded[HEADLESS_WIRE_MAX_BYTES] == 0xa5);
    HeadlessProtocol_reset(&protocol);
    memset(encoded, 0xa5, sizeof encoded);
    REFUSES(HeadlessProtocol_encode(1u, 1u, 0u, 1u, payload, maximumPayload,
        encoded, HEADLESS_WIRE_MAX_BYTES - 1u, &length, &truncated));
    assert(length == 0u && truncated);
    for (size_t i = 0u; i < sizeof encoded; ++i)
        assert(encoded[i] == 0xa5);
    REFUSES(HeadlessProtocol_encode(1u, 1u, 0u, 1u, payload, maximumPayload + 1u,
                                   encoded, sizeof encoded, &length, &truncated));
    assert(!truncated && length == 0u);
    REFUSES(HeadlessProtocol_encode(1u, 1u, 0u, 1u, payload, SIZE_MAX,
                                   encoded, sizeof encoded, &length, &truncated));
    REFUSES(HeadlessProtocol_encode(1u, 1u, 0u, 1u, nullptr, 0u,
                                   encoded, 0u, &length, &truncated));
    assert(truncated && length == 0u);
}

// Null/defaults, API misuse preservation, every public constructor/projection.
static void defaults(void) {
    uint8_t backing[64];
    size_t used = SIZE_MAX, length;
    bool truncated;
    HeadlessProtocol protocol = HeadlessProtocol();
    HeadlessProtocol zero = HeadlessProtocol_zero();
    assert(!HeadlessProtocol_isReady(&zero));
    REFUSES(HeadlessProtocol_feed(&protocol, nullptr, 0u, &used));
    REFUSES(HeadlessProtocol_finish(&protocol));
    REFUSES(HeadlessProtocol_release(&protocol));
    REFUSES(HeadlessProtocol_feed(nullptr, nullptr, 0u, &used));
    REFUSES(HeadlessProtocol_finish(nullptr));
    REFUSES(HeadlessProtocol_release(nullptr));
    HeadlessProtocol_reset(nullptr);
    assert(HeadlessProtocol_getCode(nullptr) == HEADLESS_PROTOCOL_INVALID);
    assert(!HeadlessProtocol_isReady(nullptr) && !HeadlessProtocol_isFailed(nullptr));
    assert(HeadlessProtocol_getExhaustionCount(nullptr) == 0u);
    assert(HeadlessProtocol_getCapacity(nullptr) == 0u);
    assert(HeadlessProtocol_getUsed(nullptr) == 0u);
    assert(HeadlessProtocol_getExpected(nullptr) == 0u);
    assert(HeadlessProtocol_getKind(nullptr) == 0u);
    assert(HeadlessProtocol_getOperation(nullptr) == 0u);
    assert(HeadlessProtocol_getStatus(nullptr) == 0u);
    assert(HeadlessProtocol_getRequestId(nullptr) == 0u);
    assert(HeadlessProtocol_getPayload(nullptr) == nullptr);
    assert(HeadlessProtocol_getPayloadLength(nullptr) == 0u);
    ++diagnostics;
    protocol = HeadlessProtocol(nullptr, sizeof backing);
    assert(HeadlessProtocol_isFailed(&protocol));
    ++diagnostics;
    protocol = HeadlessProtocol(backing, HEADLESS_WIRE_HEADER_BYTES - 1u);
    assert(HeadlessProtocol_isFailed(&protocol));
    ++diagnostics;
    protocol = HeadlessProtocol(backing, SIZE_MAX);
    assert(HeadlessProtocol_isFailed(&protocol));
    protocol = HeadlessProtocol(backing, sizeof backing);
    REFUSES(HeadlessProtocol_feed(&protocol, nullptr, 1u, &used));
    REFUSES(HeadlessProtocol_feed(&protocol, backing, 0u, nullptr));
    assert(!HeadlessProtocol_isFailed(&protocol));
    assert(HeadlessProtocol_feed(&protocol, nullptr, 0u, &used) && used == 0u);
    REFUSES(HeadlessProtocol_encode(0u, 1u, 0u, 1u, nullptr, 0u, backing, sizeof backing, &length, &truncated));
    REFUSES(HeadlessProtocol_encode(1u, 0u, 0u, 1u, nullptr, 0u, backing, sizeof backing, &length, &truncated));
    REFUSES(HeadlessProtocol_encode(1u, 1u, 1u, 1u, nullptr, 0u, backing, sizeof backing, &length, &truncated));
    REFUSES(HeadlessProtocol_encode(2u, 1u, 7u, 1u, nullptr, 0u, backing, sizeof backing, &length, &truncated));
    REFUSES(HeadlessProtocol_encode(1u, 1u, 0u, 0u, nullptr, 0u, backing, sizeof backing, &length, &truncated));
    REFUSES(HeadlessProtocol_encode(1u, 1u, 0u, 1u, nullptr, 1u, backing, sizeof backing, &length, &truncated));
    REFUSES(HeadlessProtocol_encode(1u, 1u, 0u, 1u, nullptr, 0u, nullptr, 0u, &length, &truncated));
    REFUSES(HeadlessProtocol_encode(1u, 1u, 0u, 1u, nullptr, 0u, backing, sizeof backing, nullptr, &truncated));
    REFUSES(HeadlessProtocol_encode(1u, 1u, 0u, 1u, nullptr, 0u, backing, sizeof backing, &length, nullptr));
    char text[256];
    assert(HeadlessProtocol_toString(nullptr, text, sizeof text, &truncated));
    assert(!truncated && strcmp(text, "nullptr") == 0);
    assert(HeadlessProtocol_toStringStruct(nullptr, text, sizeof text, &truncated));
    assert(HeadlessProtocol_toString(&protocol, text, sizeof text, &truncated));
    assert(HeadlessProtocol_toStringStruct(&protocol, text, sizeof text, &truncated));
    assert(strstr(text, "buffer:<redacted>") != nullptr);
    assert(!HeadlessProtocol_toStringStruct(&protocol, text, 1u, &truncated));
    assert(truncated && text[0] == '\0');
    assert(!HeadlessProtocol_toString(&protocol, nullptr, 0u, &truncated));
    assert(truncated);
    assert(HeadlessProtocol_toString(&protocol, text, sizeof text, nullptr));
}

int main(void) {
    FILE *capture = tmpfile();
    assert(capture != nullptr);
    int saved = dup(STDERR_FILENO);
    assert(saved >= 0 && dup2(fileno(capture), STDERR_FILENO) >= 0);
    roundtrip();
    hostile();
    boundaries();
    defaults();
    fflush(stderr);
    assert(dup2(saved, STDERR_FILENO) >= 0);
    close(saved);
    rewind(capture);
    unsigned count = 0u;
    char line[512];
    bool sawCapacity = false, sawVersion = false;
    while (fgets(line, sizeof line, capture) != nullptr) {
        assert(strstr(line, "[vex] ") == line);
        assert(strstr(line, "secret") == nullptr);
        sawCapacity |= strstr(line, "frame request 17 exceeds backing bound 16") != nullptr;
        sawVersion |= strstr(line, "rejected code 4") != nullptr;
        ++count;
    }
    fclose(capture);
    assert(count == diagnostics && sawCapacity && sawVersion);
    printf("HeadlessProtocol: all split/boundary/negative/recovery checks passed; %u expected diagnostics\n", count);
    return 0;
}
