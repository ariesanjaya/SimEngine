#include "Sim/Editor/WhiteboxCommands.h"

#include "Sim/Core/Log.h"
#include "Sim/SceneView/WhiteboxStore.h"

namespace sim::editor {

WhiteboxEditCommand::WhiteboxEditCommand(WhiteboxStore* store, Uuid guid,
                                         whitebox::WhiteboxData before,
                                         whitebox::WhiteboxData after, std::string label)
    : store_(store),
      guid_(guid),
      before_(std::move(before)),
      after_(std::move(after)),
      label_(std::move(label)) {}

void WhiteboxEditCommand::Apply(const whitebox::WhiteboxData& data) {
    whitebox::WhiteboxMesh* target = store_ == nullptr ? nullptr : store_->Find(guid_);
    if (target == nullptr) {
        return;
    }
    std::string error;
    if (!whitebox::WhiteboxMesh::Build(*target, data, error)) {
        // Cuplikan yang tidak bisa dibangun berarti sesuatu merusaknya sesudah
        // ia diambil. Terdengar keras: undo yang diam-diam tidak melakukan apa
        // pun jauh lebih membingungkan daripada undo yang mengeluh.
        SIM_ERROR("Whitebox", "undo/redo tidak bisa membangun ulang mesh: {}", error);
        return;
    }
    // Bentuknya sudah kembali; yang tergambar belum. Store yang memegang
    // penanda versinya, dan tanpa langkah ini viewport tetap menampilkan
    // geometri sebelum undo sampai ada suntingan lain yang kebetulan
    // menaikkannya.
    store_->MarkDirty(guid_);
}

void WhiteboxEditCommand::Do() { Apply(after_); }
void WhiteboxEditCommand::Undo() { Apply(before_); }

bool WhiteboxEditCommand::MergeWith(const ICommand& next) {
    const auto* other = dynamic_cast<const WhiteboxEditCommand*>(&next);
    if (other == nullptr || other->store_ != store_ || other->guid_ != guid_) {
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

SetPolygonMaterialCommand::SetPolygonMaterialCommand(WhiteboxStore* store, Uuid guid,
                                                     whitebox::PolygonHandle polygon,
                                                     int material)
    : store_(store), guid_(guid), polygon_(polygon), after_(material) {
    whitebox::WhiteboxMesh* target = store_ == nullptr ? nullptr : store_->Find(guid_);
    if (target != nullptr) {
        before_ = target->PolygonMaterial(polygon);
    }
}

void SetPolygonMaterialCommand::Apply(int material) {
    whitebox::WhiteboxMesh* target = store_ == nullptr ? nullptr : store_->Find(guid_);
    if (target == nullptr) {
        return;
    }
    target->SetPolygonMaterial(polygon_, material);
    // Material menentukan pembagian ruas mesh yang digambar, jadi ia sama
    // perlunya diberi tanda seperti perubahan bentuk.
    store_->MarkDirty(guid_);
}

void SetPolygonMaterialCommand::Do() { Apply(after_); }

void SetPolygonMaterialCommand::Undo() { Apply(before_); }

bool SetPolygonMaterialCommand::MergeWith(const ICommand& next) {
    const auto* other = dynamic_cast<const SetPolygonMaterialCommand*>(&next);
    // Hanya sisi yang sama pada whitebox yang sama: berpindah sisi adalah
    // keputusan baru pengguna, dan menggabungkannya berarti satu Ctrl+Z
    // membatalkan dua penetapan yang tidak berhubungan.
    if (other == nullptr || other->store_ != store_ || other->guid_ != guid_ ||
        other->polygon_ != polygon_) {
        return false;
    }
    after_ = other->after_;
    return true;
}

}  // namespace sim::editor
