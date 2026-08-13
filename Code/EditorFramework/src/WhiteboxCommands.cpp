#include "Sim/Editor/WhiteboxCommands.h"

#include "Sim/Core/Log.h"

namespace sim::editor {

WhiteboxEditCommand::WhiteboxEditCommand(whitebox::WhiteboxMesh* target,
                                         whitebox::WhiteboxData before,
                                         whitebox::WhiteboxData after, std::string label)
    : target_(target),
      before_(std::move(before)),
      after_(std::move(after)),
      label_(std::move(label)) {}

void WhiteboxEditCommand::Apply(const whitebox::WhiteboxData& data) {
    if (target_ == nullptr) {
        return;
    }
    std::string error;
    if (!whitebox::WhiteboxMesh::Build(*target_, data, error)) {
        // Cuplikan yang tidak bisa dibangun berarti sesuatu merusaknya sesudah
        // ia diambil. Terdengar keras: undo yang diam-diam tidak melakukan apa
        // pun jauh lebih membingungkan daripada undo yang mengeluh.
        SIM_ERROR("Whitebox", "undo/redo tidak bisa membangun ulang mesh: {}", error);
    }
}

void WhiteboxEditCommand::Do() { Apply(after_); }
void WhiteboxEditCommand::Undo() { Apply(before_); }

bool WhiteboxEditCommand::MergeWith(const ICommand& next) {
    const auto* other = dynamic_cast<const WhiteboxEditCommand*>(&next);
    if (other == nullptr || other->target_ != target_) {
        return false;
    }
    // Keadaan "sebelum" milik yang pertama dipertahankan, dan "sesudah" diambil
    // dari yang terakhir: satu seretan menjadi satu langkah dari awal ke akhir.
    after_ = other->after_;
    return true;
}

std::size_t WhiteboxEditCommand::MemoryCost() const {
    const auto sizeOf = [](const whitebox::WhiteboxData& data) {
        std::size_t bytes = data.positions.size() * sizeof(Vec3);
        for (const std::vector<uint32_t>& face : data.faces) {
            bytes += face.size() * sizeof(uint32_t);
        }
        bytes += data.hiddenEdges.size() * sizeof(uint32_t) * 2;
        bytes += data.faceMaterials.size() * sizeof(int);
        return bytes;
    };
    return sizeof(WhiteboxEditCommand) + sizeOf(before_) + sizeOf(after_);
}

SetPolygonMaterialCommand::SetPolygonMaterialCommand(whitebox::WhiteboxMesh* target,
                                                     whitebox::PolygonHandle polygon,
                                                     int material)
    : target_(target), polygon_(polygon), after_(material) {
    if (target_ != nullptr) {
        before_ = target_->PolygonMaterial(polygon);
    }
}

void SetPolygonMaterialCommand::Do() {
    if (target_ != nullptr) {
        target_->SetPolygonMaterial(polygon_, after_);
    }
}

void SetPolygonMaterialCommand::Undo() {
    if (target_ != nullptr) {
        target_->SetPolygonMaterial(polygon_, before_);
    }
}

bool SetPolygonMaterialCommand::MergeWith(const ICommand& next) {
    const auto* other = dynamic_cast<const SetPolygonMaterialCommand*>(&next);
    // Hanya sisi yang sama pada whitebox yang sama: berpindah sisi adalah
    // keputusan baru pengguna, dan menggabungkannya berarti satu Ctrl+Z
    // membatalkan dua penetapan yang tidak berhubungan.
    if (other == nullptr || other->target_ != target_ || other->polygon_ != polygon_) {
        return false;
    }
    after_ = other->after_;
    return true;
}

}  // namespace sim::editor
