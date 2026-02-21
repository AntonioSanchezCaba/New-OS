/*
 * kernel/multiboot2.c - Multiboot2 info structure parser
 */
#include <multiboot2.h>
#include <types.h>

/*
 * multiboot2_find_tag - search for a specific tag in the Multiboot2 info.
 *
 * @info: pointer to the Multiboot2 boot information structure
 * @type: the tag type to search for
 *
 * Returns a pointer to the first matching tag, or NULL if not found.
 */
struct multiboot2_tag* multiboot2_find_tag(struct multiboot2_info* info,
                                            uint32_t type)
{
    /* Tags begin immediately after the 8-byte info header */
    struct multiboot2_tag* tag =
        (struct multiboot2_tag*)((uint8_t*)info + sizeof(struct multiboot2_info));

    while (tag->type != MULTIBOOT2_TAG_END) {
        if (tag->type == type) {
            return tag;
        }
        /* Each tag is padded to an 8-byte boundary */
        tag = MULTIBOOT2_TAG_NEXT(tag);
    }

    return NULL;
}
