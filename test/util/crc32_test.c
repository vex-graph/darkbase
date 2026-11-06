// Owner: darkbase-engine/src/util/crc32.c — reflected IEEE CRC32.
// Proves the published check vector, incremental equivalence, and empty-span
// identity; database and WAL owners cover its stored-format call sites.
#include "util/crc32.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void) {
    static const uint8_t vector[] = "123456789";
    assert(Crc32_of(vector, sizeof vector - 1u) == 0xCBF43926u);

    uint32_t crc = CRC32_SEED;
    crc = Crc32_update(crc, vector, 4u);
    crc = Crc32_update(crc, vector + 4u, sizeof vector - 1u - 4u);
    assert(Crc32_finalize(crc) == Crc32_of(vector, sizeof vector - 1u));
    assert(Crc32_of(nullptr, 0u) == 0u);
    assert(Crc32_finalize(Crc32_update(CRC32_SEED, nullptr, 0u)) == 0u);
    return 0;
}
