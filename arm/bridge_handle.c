#include "bridge_handle.h"
#include <string.h>

void hmap_init(handle_map_t *m)
{
    memset(m, 0, sizeof(*m));
    for (int t = 0; t < HTYPE_COUNT; t++)
        m->next_id[t] = 1;
}

uint32_t hmap_add(handle_map_t *m, handle_type_t type, void *ptr)
{
    if (!ptr) return 0;
    for (int i = 0; i < MAX_HANDLES_PER_TYPE; i++) {
        if (!m->entries[type][i].in_use) {
            uint32_t id = m->next_id[type]++;
            m->entries[type][i].ptr = ptr;
            m->entries[type][i].id = id;
            m->entries[type][i].in_use = 1;
            return id;
        }
    }
    return 0; /* full */
}

void *hmap_get(handle_map_t *m, handle_type_t type, uint32_t id)
{
    if (id == 0) return NULL;
    for (int i = 0; i < MAX_HANDLES_PER_TYPE; i++) {
        if (m->entries[type][i].in_use && m->entries[type][i].id == id)
            return m->entries[type][i].ptr;
    }
    return NULL;
}

void hmap_remove(handle_map_t *m, handle_type_t type, uint32_t id)
{
    if (id == 0) return;
    for (int i = 0; i < MAX_HANDLES_PER_TYPE; i++) {
        if (m->entries[type][i].in_use && m->entries[type][i].id == id) {
            m->entries[type][i].in_use = 0;
            m->entries[type][i].ptr = NULL;
            return;
        }
    }
}

uint32_t hmap_find_by_ptr(handle_map_t *m, handle_type_t type, void *ptr)
{
    if (!ptr) return 0;
    for (int i = 0; i < MAX_HANDLES_PER_TYPE; i++) {
        if (m->entries[type][i].in_use && m->entries[type][i].ptr == ptr)
            return m->entries[type][i].id;
    }
    return 0;
}
