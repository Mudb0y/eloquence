#ifndef BRIDGE_HANDLE_H
#define BRIDGE_HANDLE_H

#include <stdint.h>

/* Maps uint32_t IDs <-> real ARM pointers for a client.
 * Separate maps for ECIHand, ECIDictHand, ECIFilterHand. */

#define MAX_HANDLES_PER_TYPE 64

typedef enum {
    HTYPE_ECI = 0,
    HTYPE_DICT,
    HTYPE_FILTER,
    HTYPE_COUNT
} handle_type_t;

typedef struct {
    void    *ptr;
    uint32_t id;
    int      in_use;
} handle_entry_t;

typedef struct {
    handle_entry_t entries[HTYPE_COUNT][MAX_HANDLES_PER_TYPE];
    uint32_t next_id[HTYPE_COUNT];
} handle_map_t;

void     hmap_init(handle_map_t *m);
uint32_t hmap_add(handle_map_t *m, handle_type_t type, void *ptr);
void    *hmap_get(handle_map_t *m, handle_type_t type, uint32_t id);
void     hmap_remove(handle_map_t *m, handle_type_t type, uint32_t id);
uint32_t hmap_find_by_ptr(handle_map_t *m, handle_type_t type, void *ptr);

#endif /* BRIDGE_HANDLE_H */
