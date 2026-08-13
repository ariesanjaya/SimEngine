#pragma once

#include "Sim/Core/Uuid.h"
#include "Sim/Editor/Command.h"
#include "Sim/Whitebox/WhiteboxMesh.h"

#include <string>

/// Menyunting whitebox lewat riwayat undo yang sama dengan operasi scene
/// lainnya.
namespace sim::editor {

class WhiteboxStore;

/// Satu perubahan bentuk whitebox, disimpan sebagai keadaan sebelum dan
/// sesudah.
///
/// **Cuplikan utuh, bukan tambalan.** Operasi whitebox membangun ulang meshnya
/// — itu keputusan W2, diambil karena menyulam pointer half-edge adalah tempat
/// bug topologi hidup — jadi tidak ada "tambalan kecil" yang bisa disimpan.
/// Mesh blockout berukuran puluhan sampai ratusan sisi; cuplikannya beberapa
/// kilobyte, dan ia **persis** terbalikkan.
///
/// `MemoryCost` melaporkan ukuran sebenarnya supaya batas memori riwayat tetap
/// berarti.
///
/// Sasarannya disebut lewat store dan guid, bukan lewat pointer mesh.
/// **Bukan kerapian:** store yang menandai versi, dan versi itulah yang membuat
/// viewport mengunggah ulang geometrinya. Perintah yang memegang mesh langsung
/// akan membatalkan bentuknya dengan benar sambil meninggalkan layar
/// menggambarkan bentuk yang sudah tidak ada — bug yang tampak seperti "undo
/// tidak bekerja" padahal datanya sudah benar.
class WhiteboxEditCommand final : public ICommand {
public:
    WhiteboxEditCommand(WhiteboxStore* store, Uuid guid, whitebox::WhiteboxData before,
                        whitebox::WhiteboxData after, std::string label);

    void Do() override;
    void Undo() override;
    std::string Name() const override { return label_; }

    /// Menyerap perubahan berikutnya bila ia menyunting whitebox yang sama.
    ///
    /// Inilah yang membuat satu seretan gizmo menghasilkan satu entri undo, dan
    /// bukan ratusan — aturan yang sama dengan seretan transform.
    bool MergeWith(const ICommand& next) override;
    std::size_t MemoryCost() const override;

private:
    void Apply(const whitebox::WhiteboxData& data);

    WhiteboxStore* store_;
    Uuid guid_;
    whitebox::WhiteboxData before_;
    whitebox::WhiteboxData after_;
    std::string label_;
};

/// Menetapkan material sebuah sisi.
///
/// Terpisah dari `WhiteboxEditCommand` karena ia tidak mengubah bentuk sama
/// sekali: menyimpan cuplikan mesh utuh untuk mengganti satu bilangan bulat
/// berarti riwayat yang penuh oleh salinan geometri yang tidak berubah.
class SetPolygonMaterialCommand final : public ICommand {
public:
    SetPolygonMaterialCommand(WhiteboxStore* store, Uuid guid, whitebox::PolygonHandle polygon,
                              int material);

    void Do() override;
    void Undo() override;
    std::string Name() const override { return "Assign Material"; }
    bool MergeWith(const ICommand& next) override;

private:
    void Apply(int material);

    WhiteboxStore* store_;
    Uuid guid_;
    whitebox::PolygonHandle polygon_;
    int before_ = whitebox::kNoMaterial;
    int after_ = whitebox::kNoMaterial;
};

}  // namespace sim::editor
