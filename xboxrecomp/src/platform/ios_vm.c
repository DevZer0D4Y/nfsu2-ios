/*
 * ios_vm.c -- shared memory for CreateFileMapping/MapViewOfFileEx on iOS.
 *
 * The Xbox memory model maps one RAM object several times (the 64 MB RAM
 * mirrors, the tiled aperture over the contiguous window). Elsewhere on
 * Apple that object is a shm_open file descriptor, which the iOS app sandbox
 * refuses (CreateFileMapping failed with error 8 on an iPhone). A named Mach
 * memory entry is the same thing without a file: every vm_map of it is a view
 * of the same pages. The port number stands in for the descriptor in the
 * mapping object (w32_object.fd), so win32_compat.c only swaps three calls.
 */
#include <TargetConditionals.h>
#if defined(__APPLE__) && TARGET_OS_IPHONE

#include <mach/mach.h>
#include <mach/vm_map.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

/* A new zero-filled memory object of `size` bytes: its port, or -1. */
int ios_mapping_create(size_t size)
{
    memory_object_size_t sz = size;
    mach_port_t port = MACH_PORT_NULL;
    kern_return_t kr = mach_make_memory_entry_64(mach_task_self(), &sz, 0,
                                                 MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE,
                                                 &port, MACH_PORT_NULL);
    if (kr != KERN_SUCCESS || sz < size) {
        fprintf(stderr, "[ios] memory entry of %zu KB failed: %d\n", size / 1024, kr);
        return -1;
    }
    return (int)port;
}

/* Map `len` bytes at `off` of the object: at `base` (replacing whatever we
 * hold there -- the caller has already claimed the range) or anywhere.
 * Returns MAP_FAILED like mmap. */
void *ios_mapping_view(int port, void *base, size_t len, uint64_t off, int writable)
{
    vm_address_t addr = (vm_address_t)(uintptr_t)base;
    vm_prot_t prot = VM_PROT_READ | (writable ? VM_PROT_WRITE : 0);
    int flags = base ? (VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE) : VM_FLAGS_ANYWHERE;
    kern_return_t kr = vm_map(mach_task_self(), &addr, len, 0, flags, (mem_entry_name_port_t)port,
                              (vm_offset_t)off, FALSE, prot, prot, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS)
        return MAP_FAILED;
    return (void *)(uintptr_t)addr;
}

void ios_mapping_close(int port)
{
    mach_port_deallocate(mach_task_self(), (mach_port_t)port);
}

#endif /* TARGET_OS_IPHONE */
