#include "cxl_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

// Internal allocator block header
typedef struct alloc_block {
    size_t size;
    bool is_free;
    struct alloc_block* next;
    struct alloc_block* prev;
} alloc_block_t;

// Internal allocator state
typedef struct {
    alloc_block_t* free_list;
    alloc_block_t* used_list;
    void* heap_start;
    size_t heap_size;
    size_t allocated;
    pthread_mutex_t lock;
} allocator_t;

// Internal statistics
static cxl_stats_t global_stats = {0};

int cxl_init(cxl_handle_t** handle, const char* config) {
    (void)config;  // May use later for configuration

    *handle = calloc(1, sizeof(cxl_handle_t));
    if (!*handle) return -1;

    // Discover CXL devices
    if (cxl_enumerate_devices(*handle) < 0) {
        fprintf(stderr, "Warning: No CXL devices found, using emulated memory\n");

        // Create emulated CXL memory region
        (*handle)->num_devices = 1;
        (*handle)->devices = calloc(1, sizeof(cxl_device_info_t));
        if (!(*handle)->devices) {
            free(*handle);
            return -1;
        }

        strcpy((*handle)->devices[0].device_path, "/dev/shm/cxl_emulated");
        (*handle)->devices[0].type = CXL_DEV_TYPE_3;
        (*handle)->devices[0].total_size = 1ULL << 30;  // 1GB emulated
        (*handle)->devices[0].available_size = (*handle)->devices[0].total_size;
        (*handle)->devices[0].supports_volatile = true;
        (*handle)->devices[0].numa_node = 0;
    }

    // Create default region
    cxl_region_t* region;
    if (cxl_create_region(*handle, (*handle)->devices[0].total_size,
                          CXL_MEM_HDM_H, &region) != 0) {
        fprintf(stderr, "Failed to create CXL region\n");
        free((*handle)->devices);
        free(*handle);
        return -1;
    }

    // Map the region
    if (cxl_map_region(*handle, region) != 0) {
        fprintf(stderr, "Failed to map CXL region\n");
        cxl_destroy_region(*handle, region);
        free((*handle)->devices);
        free(*handle);
        return -1;
    }

    // Initialize allocator
    allocator_t* alloc = calloc(1, sizeof(allocator_t));
    if (!alloc) {
        cxl_destroy_region(*handle, region);
        free((*handle)->devices);
        free(*handle);
        return -1;
    }

    alloc->heap_start = region->virt_addr;
    alloc->heap_size = region->size;
    alloc->allocated = 0;
    pthread_mutex_init(&alloc->lock, NULL);

    // Initialize free list with entire heap
    alloc->free_list = (alloc_block_t*)alloc->heap_start;
    alloc->free_list->size = alloc->heap_size - sizeof(alloc_block_t);
    alloc->free_list->is_free = true;
    alloc->free_list->next = NULL;
    alloc->free_list->prev = NULL;

    (*handle)->allocator = alloc;

    // Store base address in device info
    (*handle)->devices[0].base_address = (uint64_t)region->virt_addr;

    printf("CXL memory initialized: %zu MB at %p\n",
           region->size / (1024 * 1024), region->virt_addr);

    return 0;
}

void cxl_finalize(cxl_handle_t* handle) {
    if (!handle) return;

    // Cleanup allocator
    if (handle->allocator) {
        allocator_t* alloc = (allocator_t*)handle->allocator;
        pthread_mutex_destroy(&alloc->lock);
        free(alloc);
    }

    // Unmap and destroy regions
    for (int i = 0; i < handle->num_regions; i++) {
        cxl_unmap_region(handle, &handle->regions[i]);
        cxl_destroy_region(handle, &handle->regions[i]);
    }
    free(handle->regions);

    free(handle->devices);
    free(handle);
}

int cxl_enumerate_devices(cxl_handle_t* handle) {
    // Try to find CXL devices via sysfs
    const char* cxl_bus_path = "/sys/bus/cxl/devices";
    DIR* dir = opendir(cxl_bus_path);

    if (!dir) {
        // Try alternative path for DAX devices
        const char* dax_path = "/dev/dax";
        dir = opendir("/dev");
        if (!dir) return -1;

        struct dirent* entry;
        int count = 0;

        // Count DAX devices
        while ((entry = readdir(dir)) != NULL) {
            if (strncmp(entry->d_name, "dax", 3) == 0) {
                count++;
            }
        }

        if (count == 0) {
            closedir(dir);
            return -1;
        }

        handle->devices = calloc(count, sizeof(cxl_device_info_t));
        if (!handle->devices) {
            closedir(dir);
            return -1;
        }

        // Reset directory
        rewinddir(dir);
        int idx = 0;

        while ((entry = readdir(dir)) != NULL && idx < count) {
            if (strncmp(entry->d_name, "dax", 3) == 0) {
                snprintf(handle->devices[idx].device_path,
                         sizeof(handle->devices[idx].device_path),
                         "/dev/%s", entry->d_name);

                // Get size from sysfs if available
                char size_path[512];
                snprintf(size_path, sizeof(size_path),
                         "/sys/class/dax/%s/size", entry->d_name);

                FILE* fp = fopen(size_path, "r");
                if (fp) {
                    fscanf(fp, "%lu", &handle->devices[idx].total_size);
                    fclose(fp);
                } else {
                    handle->devices[idx].total_size = 1ULL << 30;  // Default 1GB
                }

                handle->devices[idx].available_size = handle->devices[idx].total_size;
                handle->devices[idx].type = CXL_DEV_TYPE_3;
                handle->devices[idx].supports_volatile = true;
                idx++;
            }
        }

        handle->num_devices = idx;
        closedir(dir);
        return idx;
    }

    // Process CXL bus devices
    struct dirent* entry;
    int count = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "mem", 3) == 0) {
            count++;
        }
    }

    if (count == 0) {
        closedir(dir);
        return -1;
    }

    handle->devices = calloc(count, sizeof(cxl_device_info_t));
    rewinddir(dir);
    int idx = 0;

    while ((entry = readdir(dir)) != NULL && idx < count) {
        if (strncmp(entry->d_name, "mem", 3) == 0) {
            char info_path[512];
            snprintf(handle->devices[idx].device_path,
                     sizeof(handle->devices[idx].device_path),
                     "%s/%s", cxl_bus_path, entry->d_name);

            // Read device properties
            snprintf(info_path, sizeof(info_path),
                     "%s/%s/size", cxl_bus_path, entry->d_name);

            FILE* fp = fopen(info_path, "r");
            if (fp) {
                fscanf(fp, "%lu", &handle->devices[idx].total_size);
                fclose(fp);
            }

            handle->devices[idx].available_size = handle->devices[idx].total_size;
            handle->devices[idx].type = CXL_DEV_TYPE_3;
            idx++;
        }
    }

    handle->num_devices = idx;
    closedir(dir);
    return idx;
}

int cxl_get_device_count(cxl_handle_t* handle) {
    return handle ? handle->num_devices : 0;
}

const cxl_device_info_t* cxl_get_device_info(cxl_handle_t* handle, int idx) {
    if (!handle || idx < 0 || idx >= handle->num_devices) return NULL;
    return &handle->devices[idx];
}

int cxl_create_region(cxl_handle_t* handle, size_t size, cxl_mem_type_t type, cxl_region_t** region) {
    if (!handle) return -1;

    // Allocate region structure
    int new_count = handle->num_regions + 1;
    cxl_region_t* new_regions = realloc(handle->regions, new_count * sizeof(cxl_region_t));
    if (!new_regions) return -1;

    handle->regions = new_regions;
    *region = &handle->regions[handle->num_regions];
    memset(*region, 0, sizeof(cxl_region_t));

    (*region)->size = size;
    (*region)->mem_type = type;

    handle->num_regions = new_count;

    return 0;
}

int cxl_map_region(cxl_handle_t* handle, cxl_region_t* region) {
    if (!handle || !region) return -1;
    if (region->is_mapped) return 0;

    // Try to map DAX device first
    if (handle->num_devices > 0 && handle->devices[0].device_path[0] != '\0') {
        int fd = open(handle->devices[0].device_path, O_RDWR);
        if (fd >= 0) {
            region->virt_addr = mmap(NULL, region->size,
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED, fd, 0);
            if (region->virt_addr != MAP_FAILED) {
                region->dax_fd = fd;
                region->is_mapped = true;
                return 0;
            }
            close(fd);
        }
    }

    // Fallback: use anonymous memory (emulation)
    region->virt_addr = mmap(NULL, region->size,
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                             -1, 0);

    if (region->virt_addr == MAP_FAILED) {
        // Try without huge pages
        region->virt_addr = mmap(NULL, region->size,
                                 PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS,
                                 -1, 0);
    }

    if (region->virt_addr == MAP_FAILED) {
        return -1;
    }

    region->is_mapped = true;
    return 0;
}

int cxl_unmap_region(cxl_handle_t* handle, cxl_region_t* region) {
    (void)handle;
    if (!region || !region->is_mapped) return 0;

    if (region->virt_addr) {
        munmap(region->virt_addr, region->size);
        region->virt_addr = NULL;
    }

    if (region->dax_fd >= 0) {
        close(region->dax_fd);
        region->dax_fd = -1;
    }

    region->is_mapped = false;
    return 0;
}

void cxl_destroy_region(cxl_handle_t* handle, cxl_region_t* region) {
    cxl_unmap_region(handle, region);
}

void* cxl_alloc(cxl_handle_t* handle, size_t size, size_t alignment) {
    if (!handle || !handle->allocator) return NULL;

    allocator_t* alloc = (allocator_t*)handle->allocator;
    pthread_mutex_lock(&alloc->lock);

    // Align size
    size = (size + alignment - 1) & ~(alignment - 1);
    size += sizeof(alloc_block_t);

    // First-fit allocation
    alloc_block_t* block = alloc->free_list;
    while (block) {
        if (block->is_free && block->size >= size) {
            // Split block if large enough
            if (block->size >= size + sizeof(alloc_block_t) + 64) {
                alloc_block_t* new_block = (alloc_block_t*)((char*)block + size);
                new_block->size = block->size - size;
                new_block->is_free = true;
                new_block->next = block->next;
                new_block->prev = block;

                if (block->next) block->next->prev = new_block;
                block->next = new_block;
                block->size = size - sizeof(alloc_block_t);
            }

            block->is_free = false;
            alloc->allocated += block->size;
            global_stats.allocations++;
            global_stats.bytes_allocated += block->size;

            pthread_mutex_unlock(&alloc->lock);
            return (void*)(block + 1);
        }
        block = block->next;
    }

    pthread_mutex_unlock(&alloc->lock);
    return NULL;  // Out of memory
}

void cxl_free(cxl_handle_t* handle, void* ptr) {
    if (!handle || !handle->allocator || !ptr) return;

    allocator_t* alloc = (allocator_t*)handle->allocator;
    pthread_mutex_lock(&alloc->lock);

    alloc_block_t* block = ((alloc_block_t*)ptr) - 1;
    block->is_free = true;

    alloc->allocated -= block->size;
    global_stats.deallocations++;
    global_stats.bytes_freed += block->size;

    // Coalesce with next block
    if (block->next && block->next->is_free) {
        block->size += sizeof(alloc_block_t) + block->next->size;
        block->next = block->next->next;
        if (block->next) block->next->prev = block;
    }

    // Coalesce with previous block
    if (block->prev && block->prev->is_free) {
        block->prev->size += sizeof(alloc_block_t) + block->size;
        block->prev->next = block->next;
        if (block->next) block->next->prev = block->prev;
    }

    pthread_mutex_unlock(&alloc->lock);
}

void cxl_flush(void* addr, size_t size) {
    char* p = (char*)addr;
    for (size_t i = 0; i < size; i += 64) {
        __asm__ volatile("clflushopt (%0)" :: "r"(p + i) : "memory");
    }
    __asm__ volatile("sfence" ::: "memory");
    global_stats.cache_flushes++;
}

void cxl_invalidate(void* addr, size_t size) {
    char* p = (char*)addr;
    for (size_t i = 0; i < size; i += 64) {
        __asm__ volatile("clflush (%0)" :: "r"(p + i) : "memory");
    }
    __asm__ volatile("mfence" ::: "memory");
    global_stats.cache_invalidates++;
}

void cxl_writeback(void* addr, size_t size) {
    cxl_flush(addr, size);
}

void cxl_fence(void) {
    __asm__ volatile("mfence" ::: "memory");
}

void cxl_sfence(void) {
    __asm__ volatile("sfence" ::: "memory");
}

void cxl_lfence(void) {
    __asm__ volatile("lfence" ::: "memory");
}

void cxl_get_stats(cxl_handle_t* handle, cxl_stats_t* stats) {
    (void)handle;
    *stats = global_stats;
}

int cxl_dax_open(const char* dax_path, cxl_region_t* region) {
    int fd = open(dax_path, O_RDWR);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return -1;
    }

    region->size = st.st_size;
    region->dax_fd = fd;
    return 0;
}

int cxl_dax_mmap(cxl_region_t* region, size_t size) {
    if (region->dax_fd < 0) return -1;

    region->virt_addr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, region->dax_fd, 0);

    if (region->virt_addr == MAP_FAILED) {
        return -1;
    }

    region->is_mapped = true;
    return 0;
}
