#ifndef CXL_MEMORY_H
#define CXL_MEMORY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// CXL device types
typedef enum {
    CXL_DEV_TYPE_1 = 1,  // CXL.io + CXL.cache
    CXL_DEV_TYPE_2 = 2,  // CXL.io + CXL.cache + CXL.mem
    CXL_DEV_TYPE_3 = 3   // CXL.io + CXL.mem
} cxl_device_type_t;

// CXL memory region types
typedef enum {
    CXL_MEM_HDM_H = 0,   // Hardware coherent (Host-managed)
    CXL_MEM_HDM_D = 1    // Device coherent (Device-managed)
} cxl_mem_type_t;

// CXL device information
typedef struct {
    char device_path[256];
    cxl_device_type_t type;
    uint64_t base_address;
    size_t total_size;
    size_t available_size;
    uint16_t numa_node;
    bool supports_volatile;
    bool supports_persistent;
} cxl_device_info_t;

// CXL memory region
typedef struct {
    uint64_t phys_addr;
    void* virt_addr;
    size_t size;
    cxl_mem_type_t mem_type;
    int dax_fd;
    bool is_mapped;
} cxl_region_t;

// CXL memory handle
typedef struct {
    int num_devices;
    cxl_device_info_t* devices;
    cxl_region_t* regions;
    int num_regions;
    void* allocator;  // Internal allocator state
} cxl_handle_t;

// Initialization
int cxl_init(cxl_handle_t** handle, const char* config);
void cxl_finalize(cxl_handle_t* handle);

// Device discovery
int cxl_enumerate_devices(cxl_handle_t* handle);
int cxl_get_device_count(cxl_handle_t* handle);
const cxl_device_info_t* cxl_get_device_info(cxl_handle_t* handle, int idx);

// Memory region management
int cxl_create_region(cxl_handle_t* handle, size_t size, cxl_mem_type_t type, cxl_region_t** region);
int cxl_map_region(cxl_handle_t* handle, cxl_region_t* region);
int cxl_unmap_region(cxl_handle_t* handle, cxl_region_t* region);
void cxl_destroy_region(cxl_handle_t* handle, cxl_region_t* region);

// Memory allocation within regions
void* cxl_alloc(cxl_handle_t* handle, size_t size, size_t alignment);
void cxl_free(cxl_handle_t* handle, void* ptr);

// Cache coherency operations
void cxl_flush(void* addr, size_t size);
void cxl_invalidate(void* addr, size_t size);
void cxl_writeback(void* addr, size_t size);

// Memory barriers
void cxl_fence(void);
void cxl_sfence(void);
void cxl_lfence(void);

// NUMA-aware operations
int cxl_get_numa_node(cxl_handle_t* handle, void* addr);
int cxl_move_pages(cxl_handle_t* handle, void* addr, size_t size, int target_node);

// DAX (Direct Access) support
int cxl_dax_open(const char* dax_path, cxl_region_t* region);
int cxl_dax_mmap(cxl_region_t* region, size_t size);

// Statistics
typedef struct {
    uint64_t allocations;
    uint64_t deallocations;
    uint64_t bytes_allocated;
    uint64_t bytes_freed;
    uint64_t cache_flushes;
    uint64_t cache_invalidates;
} cxl_stats_t;

void cxl_get_stats(cxl_handle_t* handle, cxl_stats_t* stats);

#ifdef __cplusplus
}
#endif

#endif // CXL_MEMORY_H
