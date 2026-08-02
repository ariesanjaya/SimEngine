#include "Sim/Editor/Selection.h"

#include <algorithm>

namespace sim::editor {

void Selection::Clear() {
    if (items_.empty()) {
        return;
    }
    items_.clear();
    Bump();
}

void Selection::SelectOnly(SelectionId id) {
    if (items_.size() == 1 && items_.front() == id) {
        return;
    }
    items_.clear();
    if (id != kInvalidSelection) {
        items_.push_back(id);
    }
    Bump();
}

void Selection::Add(SelectionId id) {
    if (id == kInvalidSelection || Contains(id)) {
        return;
    }
    items_.push_back(id);
    Bump();
}

void Selection::Remove(SelectionId id) {
    const auto it = std::find(items_.begin(), items_.end(), id);
    if (it == items_.end()) {
        return;
    }
    items_.erase(it);
    Bump();
}

void Selection::Toggle(SelectionId id) {
    if (Contains(id)) {
        Remove(id);
    } else {
        Add(id);
    }
}

void Selection::SetItems(std::vector<SelectionId> items) {
    // Duplikat dibuang tapi urutannya dipertahankan, karena Primary()
    // bergantung pada urutan itu.
    std::vector<SelectionId> unique;
    unique.reserve(items.size());
    for (SelectionId id : items) {
        if (id != kInvalidSelection &&
            std::find(unique.begin(), unique.end(), id) == unique.end()) {
            unique.push_back(id);
        }
    }
    if (unique == items_) {
        return;
    }
    items_ = std::move(unique);
    Bump();
}

bool Selection::Contains(SelectionId id) const {
    return std::find(items_.begin(), items_.end(), id) != items_.end();
}

}  // namespace sim::editor
