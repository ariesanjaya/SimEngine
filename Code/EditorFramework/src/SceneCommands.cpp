#include "Sim/Editor/SceneCommands.h"

#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/Selection.h"
#include "Sim/Scene/Serialization.h"

#include <algorithm>

namespace sim::editor {
namespace {

/// Mencari entity dari GUID, atau kNullEntity bila sudah tidak ada.
scene::Entity Resolve(scene::World& world, Uuid guid) {
    return guid.IsValid() ? world.FindByGuid(guid) : scene::kNullEntity;
}

}  // namespace

// --- SetTransformsCommand ---------------------------------------------------

SetTransformsCommand::SetTransformsCommand(scene::World* world, std::vector<Item> items,
                                           std::string label)
    : world_(world), items_(std::move(items)), label_(std::move(label)) {}

void SetTransformsCommand::Do() {
    Apply(true);
}

void SetTransformsCommand::Undo() {
    Apply(false);
}

void SetTransformsCommand::Apply(bool useAfter) {
    for (const Item& item : items_) {
        const scene::Entity entity = Resolve(*world_, item.guid);
        if (!scene::IsValid(entity)) {
            continue;
        }
        auto* transform = world_->TryGet<scene::TransformComponent>(entity);
        if (transform == nullptr) {
            continue;
        }
        *transform = useAfter ? item.after : item.before;
        world_->MarkTransformDirty(entity);
    }
}

bool SetTransformsCommand::MergeWith(const ICommand& next) {
    const auto* other = dynamic_cast<const SetTransformsCommand*>(&next);
    if (other == nullptr || other->world_ != world_ || other->items_.size() != items_.size()) {
        return false;
    }
    // Hanya digabung bila menyentuh entity yang sama persis dalam urutan yang
    // sama. Kalau tidak, satu seretan yang seleksinya berubah di tengah jalan
    // akan menyimpan "sebelum" milik objek yang salah.
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].guid != other->items_[i].guid) {
            return false;
        }
    }
    for (std::size_t i = 0; i < items_.size(); ++i) {
        items_[i].after = other->items_[i].after;
    }
    return true;
}

std::size_t SetTransformsCommand::MemoryCost() const {
    return sizeof(SetTransformsCommand) + items_.capacity() * sizeof(Item) + label_.capacity();
}

// --- RenameEntityCommand ----------------------------------------------------

RenameEntityCommand::RenameEntityCommand(scene::World* world, Uuid guid, std::string before,
                                         std::string after)
    : world_(world), guid_(guid), before_(std::move(before)), after_(std::move(after)) {}

void RenameEntityCommand::Do() {
    const scene::Entity entity = Resolve(*world_, guid_);
    if (scene::IsValid(entity)) {
        world_->SetName(entity, after_);
    }
}

void RenameEntityCommand::Undo() {
    const scene::Entity entity = Resolve(*world_, guid_);
    if (scene::IsValid(entity)) {
        world_->SetName(entity, before_);
    }
}

bool RenameEntityCommand::MergeWith(const ICommand& next) {
    const auto* other = dynamic_cast<const RenameEntityCommand*>(&next);
    if (other == nullptr || other->world_ != world_ || other->guid_ != guid_) {
        return false;
    }
    after_ = other->after_;
    return true;
}

std::size_t RenameEntityCommand::MemoryCost() const {
    return sizeof(RenameEntityCommand) + before_.capacity() + after_.capacity();
}

// --- ReparentCommand --------------------------------------------------------

ReparentCommand::ReparentCommand(scene::World* world, Uuid child, Uuid newParent, Uuid oldParent)
    : world_(world), child_(child), newParent_(newParent), oldParent_(oldParent) {}

void ReparentCommand::Do() {
    const scene::Entity child = Resolve(*world_, child_);
    if (scene::IsValid(child)) {
        world_->SetParent(child, Resolve(*world_, newParent_));
    }
}

void ReparentCommand::Undo() {
    const scene::Entity child = Resolve(*world_, child_);
    if (scene::IsValid(child)) {
        world_->SetParent(child, Resolve(*world_, oldParent_));
    }
}

std::string ReparentCommand::Name() const {
    const scene::Entity child = Resolve(*world_, child_);
    const std::string name = scene::IsValid(child) ? world_->NameOf(child) : std::string("entity");
    const scene::Entity parent = Resolve(*world_, newParent_);
    if (!scene::IsValid(parent)) {
        return "Move " + name + " to root";
    }
    return "Move " + name + " under " + world_->NameOf(parent);
}

// --- SetVisibilityCommand ---------------------------------------------------

SetVisibilityCommand::SetVisibilityCommand(scene::World* world, std::vector<Uuid> guids, Flag flag,
                                           bool value)
    : world_(world), guids_(std::move(guids)), flag_(flag), value_(value) {
    before_.reserve(guids_.size());
    for (const Uuid& guid : guids_) {
        const scene::Entity entity = Resolve(*world_, guid);
        const auto* visibility =
            scene::IsValid(entity) ? world_->TryGet<scene::VisibilityComponent>(entity) : nullptr;
        if (visibility == nullptr) {
            before_.push_back(flag_ == Flag::Visible);  // bawaan: terlihat, tidak terkunci
        } else {
            before_.push_back(flag_ == Flag::Visible ? visibility->visible : visibility->locked);
        }
    }
}

void SetVisibilityCommand::Do() {
    Apply(value_);
}

void SetVisibilityCommand::Undo() {
    for (std::size_t i = 0; i < guids_.size(); ++i) {
        const scene::Entity entity = Resolve(*world_, guids_[i]);
        if (!scene::IsValid(entity)) {
            continue;
        }
        auto& visibility = world_->Registry().get_or_emplace<scene::VisibilityComponent>(
            scene::World::ToEntt(entity));
        (flag_ == Flag::Visible ? visibility.visible : visibility.locked) = before_[i];
    }
}

void SetVisibilityCommand::Apply(bool value) {
    for (const Uuid& guid : guids_) {
        const scene::Entity entity = Resolve(*world_, guid);
        if (!scene::IsValid(entity)) {
            continue;
        }
        auto& visibility = world_->Registry().get_or_emplace<scene::VisibilityComponent>(
            scene::World::ToEntt(entity));
        (flag_ == Flag::Visible ? visibility.visible : visibility.locked) = value;
    }
}

std::string SetVisibilityCommand::Name() const {
    if (flag_ == Flag::Visible) {
        return value_ ? "Show entities" : "Hide entities";
    }
    return value_ ? "Lock entities" : "Unlock entities";
}

std::size_t SetVisibilityCommand::MemoryCost() const {
    return sizeof(SetVisibilityCommand) + guids_.capacity() * sizeof(Uuid);
}

// --- DeleteEntitiesCommand --------------------------------------------------

DeleteEntitiesCommand::DeleteEntitiesCommand(scene::World* world, Selection* selection,
                                             std::vector<Uuid> guids)
    : world_(world), selection_(selection), guids_(std::move(guids)) {}

void DeleteEntitiesCommand::Do() {
    // Cuplikan diambil di sini, bukan di konstruktor: pada redo, isi entity
    // bisa berbeda dari saat perintah pertama dibuat karena perintah lain di
    // antaranya sudah di-undo.
    removed_.clear();
    removed_.reserve(guids_.size());

    for (const Uuid& guid : guids_) {
        const scene::Entity entity = Resolve(*world_, guid);
        if (!scene::IsValid(entity)) {
            continue;
        }
        Removed record;
        record.guid = guid;
        record.parentGuid = world_->GuidOf(world_->ParentOf(entity));
        record.snapshot = scene::SaveSubtreeToString(*world_, entity);
        removed_.push_back(std::move(record));
    }
    for (const Removed& record : removed_) {
        const scene::Entity entity = Resolve(*world_, record.guid);
        if (scene::IsValid(entity)) {
            world_->Destroy(entity);
        }
    }
    if (selection_ != nullptr) {
        selection_->Clear();
    }
}

void DeleteEntitiesCommand::Undo() {
    // Dibangun ulang dalam urutan terbalik supaya induk yang ikut terhapus
    // sudah ada kembali sebelum anaknya dipasang.
    for (auto it = removed_.rbegin(); it != removed_.rend(); ++it) {
        scene::RestoreSubtree(*world_, it->snapshot, it->parentGuid);
    }
    if (selection_ != nullptr) {
        selection_->Clear();
        for (const Removed& record : removed_) {
            const scene::Entity entity = Resolve(*world_, record.guid);
            if (scene::IsValid(entity)) {
                selection_->Add(ToSelectionId(entity));
            }
        }
    }
}

std::string DeleteEntitiesCommand::Name() const {
    if (guids_.size() == 1) {
        return "Delete Entity";
    }
    return "Delete " + std::to_string(guids_.size()) + " Entities";
}

std::size_t DeleteEntitiesCommand::MemoryCost() const {
    std::size_t bytes = sizeof(DeleteEntitiesCommand) + guids_.capacity() * sizeof(Uuid);
    for (const Removed& record : removed_) {
        bytes += record.snapshot.capacity();
    }
    return bytes;
}

// --- PasteEntitiesCommand ---------------------------------------------------

PasteEntitiesCommand::PasteEntitiesCommand(scene::World* world, Selection* selection,
                                           std::vector<std::string> subtrees, Uuid parentGuid,
                                           std::string label)
    : world_(world),
      selection_(selection),
      subtrees_(std::move(subtrees)),
      parentGuid_(parentGuid),
      label_(std::move(label)) {}

void PasteEntitiesCommand::Do() {
    if (!prepared_) {
        // GUID ditukar sekali saja, lalu teks hasilnya disimpan. Kalau ditukar
        // ulang tiap redo, entity hasil paste akan punya identitas berbeda
        // setiap kali, dan perintah sesudahnya yang menunjuk entity itu jadi
        // tidak sah setelah satu putaran undo–redo.
        std::vector<std::string> remapped;
        remapped.reserve(subtrees_.size());
        rootGuids_.clear();

        for (const std::string& subtree : subtrees_) {
            std::string rootGuid;
            std::string text = scene::RemapGuids(subtree, &rootGuid);
            if (text.empty()) {
                continue;
            }
            remapped.push_back(std::move(text));
            rootGuids_.push_back(Uuid::Parse(rootGuid));
        }
        subtrees_ = std::move(remapped);
        prepared_ = true;
    }

    for (const std::string& subtree : subtrees_) {
        scene::RestoreSubtree(*world_, subtree, parentGuid_);
    }
    if (selection_ != nullptr) {
        selection_->Clear();
        for (const Uuid& guid : rootGuids_) {
            const scene::Entity entity = Resolve(*world_, guid);
            if (scene::IsValid(entity)) {
                selection_->Add(ToSelectionId(entity));
            }
        }
    }
}

void PasteEntitiesCommand::Undo() {
    for (const Uuid& guid : rootGuids_) {
        const scene::Entity entity = Resolve(*world_, guid);
        if (scene::IsValid(entity)) {
            world_->Destroy(entity);
        }
    }
    if (selection_ != nullptr) {
        selection_->Clear();
    }
}

std::size_t PasteEntitiesCommand::MemoryCost() const {
    std::size_t bytes = sizeof(PasteEntitiesCommand) + label_.capacity();
    for (const std::string& subtree : subtrees_) {
        bytes += subtree.capacity();
    }
    return bytes;
}

// --- CopySubtrees -----------------------------------------------------------

std::vector<std::string> CopySubtrees(const scene::World& world, const std::vector<Uuid>& guids) {
    std::vector<scene::Entity> entities;
    entities.reserve(guids.size());
    for (const Uuid& guid : guids) {
        const scene::Entity entity = world.FindByGuid(guid);
        if (scene::IsValid(entity)) {
            entities.push_back(entity);
        }
    }

    std::vector<std::string> result;
    for (const scene::Entity entity : entities) {
        // Entity yang leluhurnya ikut terpilih dilewati: sub-pohon leluhurnya
        // sudah memuatnya, dan menyalinnya lagi akan menghasilkan duplikat.
        const bool coveredByAncestor =
            std::any_of(entities.begin(), entities.end(), [&](scene::Entity other) {
                return other != entity && world.IsDescendantOf(entity, other);
            });
        if (coveredByAncestor) {
            continue;
        }
        std::string text = scene::SaveSubtreeToString(world, entity);
        if (!text.empty()) {
            result.push_back(std::move(text));
        }
    }
    return result;
}

}  // namespace sim::editor
