#include "IblPrefilter.h"

#include "Sim/Core/Log.h"
#include "Sim/Render/Ibl.h"

#include <algorithm>
#include <chrono>

namespace sim::render {
namespace {

/// Format yang dituntut `[format("rgba32f")]` di shader-nya.
///
/// **Diperiksa, bukan diandaikan.** Storage image dengan format yang tidak
/// cocok dengan deklarasi shader adalah perilaku tak terdefinisi — dan yang
/// muncul bukan galat melainkan texel yang isinya diterjemahkan salah, yaitu
/// pantulan berwarna aneh yang paling mudah dikira masalah tone mapping.
constexpr VkFormat kRequiredFormat = VK_FORMAT_R32G32B32A32_SFLOAT;

/// Harus sama persis dengan blok push_constant di
/// `Shaders/ibl_prefilter.comp.slang`.
struct PrefilterPush {
    uint32_t extent = 0;
    uint32_t sampleCount = 0;
    float roughness = 0.0f;
    float pad = 0.0f;
};

}  // namespace

bool IblPrefilter::Create(rhi::Device& device, const std::filesystem::path& shaderDirectory) {
    Destroy();
    device_ = &device;
    VkDevice handle = device.Handle();

    // Binding 0 mip tujuan, binding 1 mip 0 sebagai kubus. Keduanya di set yang
    // sama: yang berganti antar dispatch hanya binding 0, dan memisahkannya ke
    // dua set berarti dua set yang harus diikat untuk satu perbedaan.
    const std::array<VkDescriptorSetLayoutBinding, 2> bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(handle, &layoutInfo, nullptr, &setLayout_) != VK_SUCCESS) {
        Destroy();
        return false;
    }

    const std::array<VkDescriptorSetLayout, 1> sets{setLayout_};
    rhi::ComputePipelineDesc compute;
    compute.shader = shaderDirectory / "ibl_prefilter.comp.spv";
    compute.setLayouts = sets;
    compute.pushConstantBytes = sizeof(PrefilterPush);
    if (!pipeline_.Create(device, compute)) {
        Destroy();
        return false;
    }

    // Satu set per mip, dialokasikan sekali. Yang ditunjuknya berpindah tiap
    // panggangan lewat `vkUpdateDescriptorSets`, dan itu sah karena `Run`
    // menunggu queue diam sebelum kembali — tidak ada command buffer yang masih
    // memegang set lamanya saat set itu ditulis ulang.
    const std::array<VkDescriptorPoolSize, 2> sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxMips},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxMips}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kMaxMips;
    poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();
    if (vkCreateDescriptorPool(handle, &poolInfo, nullptr, &pool_) != VK_SUCCESS) {
        Destroy();
        return false;
    }

    std::array<VkDescriptorSetLayout, kMaxMips> layouts{};
    layouts.fill(setLayout_);
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = pool_;
    allocate.descriptorSetCount = kMaxMips;
    allocate.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(handle, &allocate, sets_.data()) != VK_SUCCESS) {
        Destroy();
        return false;
    }
    return true;
}

void IblPrefilter::ReleaseViews() {
    if (device_ == nullptr) {
        return;
    }
    VkDevice handle = device_->Handle();
    for (VkImageView& view : mipViews_) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(handle, view, nullptr);
            view = VK_NULL_HANDLE;
        }
    }
    if (sourceView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(handle, sourceView_, nullptr);
        sourceView_ = VK_NULL_HANDLE;
    }
}

void IblPrefilter::Destroy() {
    if (device_ == nullptr) {
        return;
    }
    VkDevice handle = device_->Handle();
    ReleaseViews();
    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(handle, pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
        sets_.fill(VK_NULL_HANDLE);
    }
    pipeline_.Destroy();
    if (setLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(handle, setLayout_, nullptr);
        setLayout_ = VK_NULL_HANDLE;
    }
    device_ = nullptr;
}

bool IblPrefilter::Run(const rhi::TextureCube& cube, uint32_t firstMip, uint32_t sampleCount) {
    if (device_ == nullptr || !IsValid()) {
        SIM_WARN("Render", "GPU prefilter asked to run before it was created");
        return false;
    }
    if (!cube.IsValid() || !cube.IsStorage()) {
        SIM_WARN("Render", "GPU prefilter needs a cubemap created with storage usage");
        return false;
    }
    if (cube.Format() != kRequiredFormat) {
        SIM_WARN("Render", "GPU prefilter needs RGBA32F, got format {}",
                 static_cast<int>(cube.Format()));
        return false;
    }
    const uint32_t mipCount = std::min(cube.MipCount(), kMaxMips);
    if (cube.MipCount() > kMaxMips) {
        SIM_WARN("Render", "cubemap has {} mips, more than the {} this pass can filter",
                 cube.MipCount(), kMaxMips);
        return false;
    }
    // Mip 0 tidak pernah lewat sini: ia mencuplik lingkungan, bukan menyaring
    // mip 0. Yang memintanya salah membaca kontraknya, jadi ia ditolak
    // alih-alih diam-diam digeser ke satu.
    if (firstMip == 0 || firstMip >= mipCount) {
        SIM_WARN("Render", "GPU prefilter asked for mip {} of {} — nothing to filter", firstMip,
                 mipCount);
        return false;
    }

    const auto started = std::chrono::steady_clock::now();
    VkDevice handle = device_->Handle();
    ReleaseViews();

    // Sumbernya mip 0 saja. View yang mencakup seluruh rantai akan menyatakan
    // bahwa dispatch ini membaca mip yang sedang ditulisnya sendiri — sah
    // menurut driver, tetapi bukan yang dimaksud, dan tidak ada yang akan
    // menemukannya lagi setelah baris ini ditulis.
    VkImageViewCreateInfo sourceInfo{};
    sourceInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    sourceInfo.image = cube.Image();
    sourceInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    sourceInfo.format = cube.Format();
    sourceInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    sourceInfo.subresourceRange.baseMipLevel = 0;
    sourceInfo.subresourceRange.levelCount = 1;
    sourceInfo.subresourceRange.layerCount = 6;
    if (vkCreateImageView(handle, &sourceInfo, nullptr, &sourceView_) != VK_SUCCESS) {
        SIM_WARN("Render", "cannot view mip 0 of the environment cubemap");
        ReleaseViews();
        return false;
    }

    // **`2D_ARRAY`, bukan `CUBE`.** Vulkan tidak punya storage image bertipe
    // kubus; enam mukanya ditulis sebagai enam lapis, dan urutan lapisnya
    // urutan muka yang sama yang dipetakan `CubeFaceDirection`.
    for (uint32_t mip = firstMip; mip < mipCount; ++mip) {
        VkImageViewCreateInfo mipInfo{};
        mipInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        mipInfo.image = cube.Image();
        mipInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        mipInfo.format = cube.Format();
        mipInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        mipInfo.subresourceRange.baseMipLevel = mip;
        mipInfo.subresourceRange.levelCount = 1;
        mipInfo.subresourceRange.layerCount = 6;
        if (vkCreateImageView(handle, &mipInfo, nullptr, &mipViews_[mip]) != VK_SUCCESS) {
            SIM_WARN("Render", "cannot view mip {} of the environment cubemap", mip);
            ReleaseViews();
            return false;
        }

        // GENERAL pada keduanya, karena itulah layout seluruh image selama
        // dispatch berjalan — dan layout di sebuah descriptor bukan keterangan
        // melainkan janji.
        VkDescriptorImageInfo storage{};
        storage.imageView = mipViews_[mip];
        storage.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo source{};
        source.sampler = cube.Sampler();
        source.imageView = sourceView_;
        source.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = sets_[mip];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &storage;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = sets_[mip];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &source;
        vkUpdateDescriptorSets(handle, static_cast<uint32_t>(writes.size()), writes.data(), 0,
                               nullptr);
    }

    VkImageSubresourceRange whole{};
    whole.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    whole.levelCount = cube.MipCount();
    whole.layerCount = 6;

    const auto barrier = [&](VkCommandBuffer cmd, VkImageLayout from, VkImageLayout to,
                             VkPipelineStageFlags2 sourceStage, VkAccessFlags2 sourceAccess,
                             VkPipelineStageFlags2 destinationStage,
                             VkAccessFlags2 destinationAccess) {
        VkImageMemoryBarrier2 image{};
        image.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        image.srcStageMask = sourceStage;
        image.srcAccessMask = sourceAccess;
        image.dstStageMask = destinationStage;
        image.dstAccessMask = destinationAccess;
        image.oldLayout = from;
        image.newLayout = to;
        image.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image.image = cube.Image();
        image.subresourceRange = whole;

        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &image;
        vkCmdPipelineBarrier2(cmd, &dependency);
    };

    VkCommandBuffer cmd = device_->BeginOneShot();
    // `TextureCube::Create` meninggalkan seluruh image di SHADER_READ_ONLY, dan
    // unggahannya sudah menunggu queue diam — jadi tidak ada tulisan yang masih
    // mengudara untuk ditunggu barrier ini.
    barrier(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // **Tanpa satu barrier pun di antara dispatch.** Setiap mip membaca mip 0
    // dan menulis mip yang berbeda; tidak ada satu pun yang menjadi masukan
    // yang berikutnya. Menyisipkan barrier di sini akan menyerialkan pekerjaan
    // yang memang saling bebas — dan itu justru seluruh alasan pass ini ada.
    const uint32_t samples = std::max(sampleCount, 1u);
    for (uint32_t mip = firstMip; mip < mipCount; ++mip) {
        const uint32_t extent = std::max(cube.Size() >> mip, 1u);
        PrefilterPush push{};
        push.extent = extent;
        push.sampleCount = samples;
        // Pemetaan mip→kekasaran milik sisi CPU, dipanggil dari sini. Menyalinnya
        // ke dalam shader berarti dua pemetaan yang harus berpindah bersama.
        push.roughness = RoughnessForMip(mip, cube.MipCount());

        const std::array<VkDescriptorSet, 1> bound{sets_[mip]};
        pipeline_.Bind(cmd, bound);
        pipeline_.Push(cmd, &push, sizeof(push));
        vkCmdDispatch(cmd, rhi::GroupCount(extent, kGroupSize), rhi::GroupCount(extent, kGroupSize),
                      6);
    }

    barrier(cmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    device_->EndOneShot(cmd);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    SIM_INFO("Render", "IBL prefilter on GPU: mips {}..{} of {}x{}, {} samples, {} ms", firstMip,
             mipCount - 1, cube.Size(), cube.Size(), samples, elapsed.count());
    return true;
}

}  // namespace sim::render
