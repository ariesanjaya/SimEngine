// Satu-satunya TU yang meng-compile isi VulkanMemoryAllocator.
// VMA header-only: tanpa berkas ini, semua simbolnya tidak akan ada saat link.

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0

// VMA memakai pola penulisan yang memicu sebagian peringatan clang kita.
// Dibungkus di sini supaya sisa modul RHI tetap bisa dibangun dengan -Werror.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#include <vk_mem_alloc.h>
#pragma clang diagnostic pop
