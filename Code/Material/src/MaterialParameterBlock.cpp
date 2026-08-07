#include "Sim/Material/MaterialParameterBlock.h"

#include <cstring>

namespace sim::material {
namespace {

uint32_t Components(ValueKind kind) {
    switch (kind) {
        case ValueKind::Float:
        case ValueKind::Bool:
            return 1;
        case ValueKind::Float2:
            return 2;
        case ValueKind::Float3:
            return 3;
        case ValueKind::Float4:
            return 4;
        case ValueKind::Texture:
        case ValueKind::Numeric:
            return 0;
    }
    return 0;
}

uint32_t Align(uint32_t offset, uint32_t alignment) {
    return alignment == 0 ? offset : ((offset + alignment - 1) / alignment) * alignment;
}

}  // namespace

uint32_t ValueSize(ValueKind kind) {
    return Components(kind) * 4u;
}

uint32_t ValueAlignment(ValueKind kind) {
    switch (Components(kind)) {
        case 1: return 4;
        case 2: return 8;
        // **float3 berjajar 16, bukan 12.** Inilah aturan std140 yang paling
        // sering dilupakan, dan satu-satunya yang salahnya tidak menghasilkan
        // galat apa pun — hanya nilai yang bergeser.
        case 3:
        case 4: return 16;
        default: return 0;
    }
}

void MaterialParameterBlock::Build(const std::vector<MaterialParameter>& parameters) {
    slots_.clear();
    uint32_t offset = 0;
    for (const MaterialParameter& parameter : parameters) {
        const uint32_t size = ValueSize(parameter.kind);
        if (size == 0) {
            // Tekstur tidak tinggal di blok uniform; ia punya tabel slotnya
            // sendiri. Melewatkannya di sini, bukan memberinya offset nol,
            // supaya offset parameter sesudahnya tidak ikut bergeser.
            continue;
        }
        const uint32_t alignment = ValueAlignment(parameter.kind);
        offset = Align(offset, alignment);
        slots_.push_back(ParameterSlot{parameter.name, parameter.kind, offset, size});
        offset += size;
    }
    // Blok dibulatkan ke 16: itu penjajaran minimum sebuah constant buffer, dan
    // ukuran yang tidak dibulatkan membuat larik blok berurutan meleset.
    bytes_ = Align(offset, 16);
}

void MaterialParameterBlock::Clear() {
    slots_.clear();
    bytes_ = 0;
}

const ParameterSlot& MaterialParameterBlock::Slot(int index) const {
    static const ParameterSlot kFallback;
    if (index < 0 || index >= SlotCount()) {
        return kFallback;
    }
    return slots_[static_cast<std::size_t>(index)];
}

int MaterialParameterBlock::Find(std::string_view name) const {
    for (int i = 0; i < SlotCount(); ++i) {
        if (slots_[static_cast<std::size_t>(i)].name == name) {
            return i;
        }
    }
    return -1;
}

void MaterialParameterBlock::Fill(const std::vector<MaterialParameter>& parameters,
                                  const std::vector<ParameterOverride>& overrides,
                                  std::vector<uint8_t>& out) const {
    // Nol lebih dulu, termasuk sisipannya. Sisipan yang tidak diinisialisasi
    // membuat dua blok yang isinya sama tidak lagi berbanding sama byte demi
    // byte — dan itu yang dipakai renderer untuk memutuskan apakah sebuah blok
    // perlu diunggah ulang.
    out.assign(bytes_, 0u);
    for (const MaterialParameter& parameter : parameters) {
        const int slot = Find(parameter.name);
        if (slot < 0) {
            continue;
        }
        Write(out, parameter.name, ParseValue(parameter.kind, parameter.defaultValue));
    }
    for (const ParameterOverride& override : overrides) {
        Write(out, override.name, override.value);
    }
}

bool MaterialParameterBlock::Write(std::vector<uint8_t>& block, std::string_view name,
                                   const MaterialValue& value) const {
    const int index = Find(name);
    if (index < 0) {
        return false;
    }
    const ParameterSlot& slot = slots_[static_cast<std::size_t>(index)];
    if (block.size() < static_cast<std::size_t>(slot.offset) + slot.size) {
        return false;
    }
    // Lebar slot yang menentukan berapa komponen ditulis, bukan lebar nilainya.
    // Sebuah instance yang menyimpan float4 untuk parameter yang sudah diubah
    // menjadi float3 tidak boleh menulis melewati slotnya.
    std::memcpy(block.data() + slot.offset, value.components.data(), slot.size);
    return true;
}

std::vector<TextureSlot> BuildTextureSlots(const std::vector<TextureBinding>& bindings) {
    std::vector<TextureSlot> slots;
    slots.reserve(bindings.size());
    for (const TextureBinding& binding : bindings) {
        slots.push_back(TextureSlot{binding.name, binding.texture});
    }
    return slots;
}

}  // namespace sim::material
