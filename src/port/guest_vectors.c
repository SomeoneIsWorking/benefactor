#include "port/guest_vectors.h"

#include "port/port_internal.h"

uint32_t guest_vector_handler(GuestVector vector) {
    if (g_chip == NULL)
        return 0;
    const uint8_t *const entry = g_chip + (uint32_t)vector;
    return ((uint32_t)entry[0] << 24) | ((uint32_t)entry[1] << 16) | ((uint32_t)entry[2] << 8) |
           (uint32_t)entry[3];
}
