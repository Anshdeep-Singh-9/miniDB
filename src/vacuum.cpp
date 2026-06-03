#include "vacuum.h"

#include <cstring>

bool compact_page_buffer(char* page_buffer) {
    // What: compact live tuples inside one slotted page without changing slot ids.
    // Why: DELETE/large UPDATE can leave holes; INSERT may need contiguous free space again.
    // Example: slots 0 and 2 stay same, deleted slot 1 remains tombstone, live bytes move together.
    if (page_buffer == NULL) {
        return false;
    }

    char old_buffer[STORAGE_PAGE_SIZE];
    char new_buffer[STORAGE_PAGE_SIZE] = {0};
    std::memcpy(old_buffer, page_buffer, STORAGE_PAGE_SIZE);

    PageHeader* old_header = reinterpret_cast<PageHeader*>(old_buffer);
    PageHeader* new_header = reinterpret_cast<PageHeader*>(new_buffer);

    if (old_header->free_start > STORAGE_PAGE_SIZE || old_header->free_end > STORAGE_PAGE_SIZE) {
        return false;
    }

    std::memcpy(new_buffer, old_buffer, old_header->free_start);
    new_header->free_end = STORAGE_PAGE_SIZE;

    SlotEntry* old_slots = reinterpret_cast<SlotEntry*>(old_buffer + sizeof(PageHeader));
    SlotEntry* new_slots = reinterpret_cast<SlotEntry*>(new_buffer + sizeof(PageHeader));

    bool changed = false;

    for (int i = 0; i < new_header->slot_count; i++) {
        if (new_slots[i].length > 0) {
            new_header->free_end -= new_slots[i].length;

            std::memcpy(
                new_buffer + new_header->free_end,
                old_buffer + old_slots[i].offset,
                new_slots[i].length
            );

            if (new_slots[i].offset != new_header->free_end) {
                changed = true;
            }

            new_slots[i].offset = new_header->free_end;
        } else if (old_slots[i].length == 0) {
            changed = true;
        }
    }

    if (changed) {
        std::memcpy(page_buffer, new_buffer, STORAGE_PAGE_SIZE);
    }

    return changed;
}

void compact_page(uint32_t page_id, DiskManager* disk) {
    // What: read a page from disk, compact it in RAM, and write it back if changed.
    // Why: this is the disk-level wrapper around compact_page_buffer().
    // Example: compact_page(0, disk) cleans fragmentation inside page 0.
    if (disk == NULL) {
        return;
    }

    char buffer[STORAGE_PAGE_SIZE];
    if (!disk->read_page(page_id, buffer)) {
        return;
    }

    if (compact_page_buffer(buffer)) {
        disk->write_page(page_id, buffer);
    }
}
