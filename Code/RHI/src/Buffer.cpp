#include "Sim/RHI/Buffer.h"

#include "Sim/Core/Assert.h"

#include <cstring>

namespace sim::rhi {

DynamicBuffer::~DynamicBuffer() {
    Destroy();
}

bool DynamicBuffer::Create(Device& device, VkBufferUsageFlags usage, VkDeviceSize initialBytes) {
    device_ = &device;
    usage_ = usage;
    return Reserve(initialBytes);
}

bool DynamicBuffer::Reserve(VkDeviceSize bytes) {
    if (device_ == nullptr || bytes == 0) {
        return false;
    }
    if (bytes <= capacity_) {
        return true;
    }

    // Tumbuh berlipat, bukan pas-pasan: menyeret seleksi menambah beberapa
    // vertex per frame, dan menaikkan kapasitas satu langkah tiap kali berarti
    // alokasi ulang tiap frame.
    VkDeviceSize newCapacity = capacity_ == 0 ? 64u * 1024u : capacity_;
    while (newCapacity < bytes) {
        newCapacity *= 2;
    }

    Destroy();

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = newCapacity;
    bufferInfo.usage = usage_;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                           VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo mappedInfo{};
    const VkResult result = vmaCreateBuffer(device_->Allocator(), &bufferInfo, &allocationInfo,
                                            &buffer_, &allocation_, &mappedInfo);
    if (result != VK_SUCCESS) {
        SIM_ERROR("RHI", "vmaCreateBuffer failed: {}", ResultToString(result));
        buffer_ = VK_NULL_HANDLE;
        return false;
    }

    mapped_ = mappedInfo.pMappedData;
    capacity_ = newCapacity;
    return true;
}

bool DynamicBuffer::Write(const void* data, VkDeviceSize bytes) {
    if (bytes == 0) {
        return true;
    }
    if (!Reserve(bytes) || mapped_ == nullptr) {
        return false;
    }
    std::memcpy(mapped_, data, bytes);
    // Memori HOST_ACCESS_SEQUENTIAL_WRITE dari VMA_MEMORY_USAGE_AUTO selalu
    // coherent pada perangkat yang kita dukung, jadi tidak perlu flush manual.
    return true;
}

bool DynamicBuffer::WriteAt(VkDeviceSize offset, const void* data, VkDeviceSize bytes) {
    if (bytes == 0) {
        return true;
    }
    // **Tidak memanggil `Reserve`.** Membuat ulang buffer di tengah pengemasan
    // akan membuang potongan yang sudah ditulis sebelumnya, dan pemanggilnya
    // sudah menyimpan offset ke buffer yang lama. Pemanggil yang mengemas harus
    // memesan kapasitasnya lebih dulu, sekali.
    if (mapped_ == nullptr || offset + bytes > capacity_) {
        return false;
    }
    std::memcpy(static_cast<std::byte*>(mapped_) + offset, data, bytes);
    return true;
}

void DynamicBuffer::Destroy() {
    if (device_ == nullptr || buffer_ == VK_NULL_HANDLE) {
        return;
    }
    vmaDestroyBuffer(device_->Allocator(), buffer_, allocation_);
    buffer_ = VK_NULL_HANDLE;
    allocation_ = VK_NULL_HANDLE;
    mapped_ = nullptr;
    capacity_ = 0;
}

}  // namespace sim::rhi
