#pragma once

#include "Sim/Core/Uuid.h"
#include "Sim/Editor/Command.h"
#include "Sim/Scene/ComponentRegistry.h"
#include "Sim/Scene/Components.h"
#include "Sim/Scene/World.h"

#include <string>
#include <vector>

namespace sim::editor {

class Selection;

/// Perintah yang dipakai bersama oleh Outliner, Inspector, dan Viewport.
///
/// Semuanya menyimpan GUID, bukan handle entity. Alasannya: undo bisa
/// menghidupkan kembali entity yang sudah dihapus, dan entt boleh memakai ulang
/// nomor entity yang sama untuk objek yang sama sekali berbeda. GUID adalah
/// satu-satunya identitas yang bertahan melewati siklus hapus–undo.

/// Mengubah transform sejumlah entity sekaligus.
///
/// Dipakai gizmo. Merge-nya membuat satu seretan gizmo — berapa pun frame yang
/// dilaluinya — menjadi tepat satu entri undo.
class SetTransformsCommand final : public ICommand {
public:
    struct Item {
        Uuid guid;
        scene::TransformComponent before;
        scene::TransformComponent after;
    };

    SetTransformsCommand(scene::World* world, std::vector<Item> items, std::string label);

    void Do() override;
    void Undo() override;
    std::string Name() const override { return label_; }
    bool MergeWith(const ICommand& next) override;
    std::size_t MemoryCost() const override;

private:
    void Apply(bool useAfter);

    scene::World* world_;
    std::vector<Item> items_;
    std::string label_;
};

/// Menyunting satu jenis komponen pada sejumlah entity sekaligus.
///
/// Dipakai Inspector untuk seleksi tunggal maupun jamak — satu jalur kode, jadi
/// perilaku undo-nya tidak mungkin berbeda antara keduanya. Nilai disimpan
/// sebagai cuplikan JSON, bukan salinan byte, karena komponen memuat std::string
/// dan std::vector.
class SetComponentsCommand final : public ICommand {
public:
    struct Item {
        Uuid guid;
        std::string before;
        std::string after;
    };

    SetComponentsCommand(scene::World* world, const scene::ComponentOps* ops,
                         std::vector<Item> items);

    void Do() override;
    void Undo() override;
    std::string Name() const override;
    bool MergeWith(const ICommand& next) override;
    std::size_t MemoryCost() const override;

private:
    void Apply(bool useAfter);

    scene::World* world_;
    const scene::ComponentOps* ops_;
    std::vector<Item> items_;
};

class RenameEntityCommand final : public ICommand {
public:
    RenameEntityCommand(scene::World* world, Uuid guid, std::string before, std::string after);

    void Do() override;
    void Undo() override;
    std::string Name() const override { return "Rename to \"" + after_ + "\""; }
    bool MergeWith(const ICommand& next) override;
    std::size_t MemoryCost() const override;

private:
    scene::World* world_;
    Uuid guid_;
    std::string before_;
    std::string after_;
};

/// Memindahkan entity ke induk baru.
///
/// Induk tidak valid berarti dijadikan akar. Perintah ini menolak dirinya
/// sendiri bila akan membentuk siklus — pemeriksaannya ada di World::SetParent,
/// dan hasilnya diperiksa di sini supaya undo tidak menyimpan langkah yang
/// sebenarnya tidak terjadi.
class ReparentCommand final : public ICommand {
public:
    ReparentCommand(scene::World* world, Uuid child, Uuid newParent, Uuid oldParent);

    void Do() override;
    void Undo() override;
    std::string Name() const override;

private:
    scene::World* world_;
    Uuid child_;
    Uuid newParent_;
    Uuid oldParent_;
};

/// Menyalakan/mematikan visibility atau lock pada sejumlah entity.
class SetVisibilityCommand final : public ICommand {
public:
    enum class Flag { Visible, Locked };

    SetVisibilityCommand(scene::World* world, std::vector<Uuid> guids, Flag flag, bool value);

    void Do() override;
    void Undo() override;
    std::string Name() const override;
    std::size_t MemoryCost() const override;

private:
    void Apply(bool value);

    scene::World* world_;
    std::vector<Uuid> guids_;
    std::vector<bool> before_;
    Flag flag_;
    bool value_;
};

/// Menghapus sejumlah entity beserta keturunannya.
///
/// Sub-pohonnya disimpan sebagai teks level sehingga undo membangunnya kembali
/// utuh: GUID, hierarki, dan seluruh komponen.
class DeleteEntitiesCommand final : public ICommand {
public:
    DeleteEntitiesCommand(scene::World* world, Selection* selection, std::vector<Uuid> guids);

    void Do() override;
    void Undo() override;
    std::string Name() const override;
    std::size_t MemoryCost() const override;

private:
    struct Removed {
        Uuid guid;
        Uuid parentGuid;
        std::string snapshot;
    };

    scene::World* world_;
    Selection* selection_;
    std::vector<Uuid> guids_;
    std::vector<Removed> removed_;
};

/// Menempelkan sub-pohon dari teks level ke dalam dunia.
///
/// Dipakai duplikasi maupun paste — keduanya operasi yang sama, hanya beda asal
/// teksnya. GUID di dalam teks ditukar sekali saja pada Do() pertama lalu
/// disimpan, supaya redo menghasilkan GUID yang sama persis dan perintah lain
/// yang menunjuk hasil paste tetap sah setelah undo–redo.
class PasteEntitiesCommand final : public ICommand {
public:
    PasteEntitiesCommand(scene::World* world, Selection* selection, std::vector<std::string> subtrees,
                         Uuid parentGuid, std::string label);

    void Do() override;
    void Undo() override;
    std::string Name() const override { return label_; }
    std::size_t MemoryCost() const override;

    /// Akar-akar yang dihasilkan Do() terakhir. Kosong sebelum dijalankan.
    const std::vector<Uuid>& CreatedRoots() const { return rootGuids_; }

private:
    scene::World* world_;
    Selection* selection_;
    std::vector<std::string> subtrees_;
    std::vector<Uuid> rootGuids_;
    Uuid parentGuid_;
    std::string label_;
    bool prepared_ = false;
};

/// Menyalin sub-pohon entity terpilih menjadi teks, siap dipakai
/// PasteEntitiesCommand. Entity yang leluhurnya ikut terpilih dilewati supaya
/// tidak tersalin dua kali.
std::vector<std::string> CopySubtrees(const scene::World& world, const std::vector<Uuid>& guids);

}  // namespace sim::editor
