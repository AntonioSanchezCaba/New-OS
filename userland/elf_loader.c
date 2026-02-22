/*
 * userland/elf_loader.c - ELF64 executable loader
 *
 * Loads a statically-linked ELF64 executable into a process's address space.
 * Handles PT_LOAD segments: copies file content and zeroes BSS.
 */
#include <elf.h>
#include <process.h>
#include <memory.h>
#include <kernel.h>
#include <types.h>
#include <string.h>
#include <errno.h>

/*
 * elf_validate - sanity-check an ELF binary.
 * Returns true if the file looks like a valid ELF64 executable.
 */
bool elf_validate(const void* data, size_t size)
{
    if (size < sizeof(elf64_hdr_t)) return false;

    const elf64_hdr_t* hdr = (const elf64_hdr_t*)data;

    /* Check magic */
    if (*(uint32_t*)hdr->e_ident != ELF_MAGIC) return false;

    /* 64-bit */
    if (hdr->e_ident[4] != ELFCLASS64) return false;

    /* Little-endian */
    if (hdr->e_ident[5] != ELFDATA2LSB) return false;

    /* Executable file */
    if (hdr->e_type != ET_EXEC) return false;

    /* x86_64 */
    if (hdr->e_machine != EM_X86_64) return false;

    return true;
}

/*
 * elf_load - load ELF binary into the process's address space.
 *
 * @proc:       target process (its address_space must be set up)
 * @data:       raw ELF binary data
 * @size:       size of the binary in bytes
 * @entry:      output: virtual address of the entry point
 *
 * Returns 0 on success, negative error code on failure.
 */
int elf_load(process_t* proc, const void* data, size_t size, uint64_t* entry)
{
    if (!elf_validate(data, size)) return -EINVAL;

    const elf64_hdr_t* hdr = (const elf64_hdr_t*)data;

    *entry = hdr->e_entry;

    /* Iterate over program headers */
    for (uint16_t i = 0; i < hdr->e_phnum; i++) {
        const elf64_phdr_t* phdr = (const elf64_phdr_t*)(
            (const uint8_t*)data + hdr->e_phoff + i * hdr->e_phentsize);

        if (phdr->p_type != PT_LOAD) continue;
        if (phdr->p_memsz == 0) continue;

        /* Determine page-aligned load region */
        uint64_t vaddr_aligned = ALIGN_DOWN(phdr->p_vaddr, PAGE_SIZE);
        uint64_t vend_aligned  = ALIGN_UP(phdr->p_vaddr + phdr->p_memsz, PAGE_SIZE);
        uint64_t pages = (vend_aligned - vaddr_aligned) / PAGE_SIZE;

        /* Build PTE flags */
        uint64_t pte_flags = PTE_PRESENT | PTE_USER;
        if (phdr->p_flags & PF_W) pte_flags |= PTE_WRITABLE;
        if (!(phdr->p_flags & PF_X)) pte_flags |= PTE_NX;

        /* Allocate and map physical pages */
        for (uint64_t p = 0; p < pages; p++) {
            uint64_t vaddr = vaddr_aligned + p * PAGE_SIZE;

            /* Allocate a physical frame */
            void* phys = pmm_alloc_frame();
            if (!phys) {
                kerror("elf_load: out of physical memory");
                return -ENOMEM;
            }

            /* Zero the frame */
            memset(PHYS_TO_VIRT(phys), 0, PAGE_SIZE);

            /* Map it into the process address space */
            if (vmm_map_page(proc->address_space, vaddr, (uint64_t)phys,
                             pte_flags) != 0) {
                pmm_free_frame(phys);
                return -ENOMEM;
            }
        }

        /* Copy the file data into the mapped region */
        if (phdr->p_filesz > 0) {
            const uint8_t* src = (const uint8_t*)data + phdr->p_offset;
            uint64_t dst_vaddr = phdr->p_vaddr;
            size_t   to_copy   = (size_t)phdr->p_filesz;

            /* We need to write through the page tables.
             * Since the pages are mapped in the kernel via identity mapping,
             * we can find the physical address and write via PHYS_TO_VIRT. */
            while (to_copy > 0) {
                uint64_t phys_addr = vmm_get_physical(proc->address_space, dst_vaddr);
                if (!phys_addr) return -EFAULT;

                uint64_t page_offset = dst_vaddr & 0xFFF;
                uint64_t chunk = MIN(to_copy, PAGE_SIZE - page_offset);

                memcpy((uint8_t*)PHYS_TO_VIRT((void*)phys_addr) + page_offset,
                       src, chunk);

                src       += chunk;
                dst_vaddr += chunk;
                to_copy   -= chunk;
            }
        }

        /* BSS (p_memsz > p_filesz) is already zeroed since we zeroed all frames */

        /* Set up process heap start after the last LOAD segment */
        uint64_t seg_end = phdr->p_vaddr + phdr->p_memsz;
        seg_end = ALIGN_UP(seg_end, PAGE_SIZE);
        if (seg_end > proc->heap_start) {
            proc->heap_start = seg_end;
            proc->heap_end   = seg_end;
        }

        kdebug("ELF: loaded segment v=0x%llx-0x%llx (filesz=%llu, memsz=%llu)",
               phdr->p_vaddr, phdr->p_vaddr + phdr->p_memsz,
               phdr->p_filesz, phdr->p_memsz);
    }

    kinfo("ELF: loaded binary, entry=0x%llx, heap_start=0x%llx",
          *entry, proc->heap_start);
    return 0;
}
