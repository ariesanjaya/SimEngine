#include "Sim/Reflect/TypeRegistry.h"

#include <algorithm>

namespace sim::reflect {

const FieldDesc* TypeDesc::FindField(std::string_view fieldName) const {
    const auto it = std::find_if(fields.begin(), fields.end(), [fieldName](const FieldDesc& field) {
        return field.name == fieldName;
    });
    return it == fields.end() ? nullptr : &*it;
}

TypeRegistry& TypeRegistry::Get() {
    static TypeRegistry registry;
    return registry;
}

const TypeDesc* TypeRegistry::Find(TypeKey key) const {
    const auto it = types_.find(key);
    return it == types_.end() ? nullptr : &it->second;
}

const TypeDesc* TypeRegistry::Find(std::string_view name) const {
    const auto it = byName_.find(std::string(name));
    return it == byName_.end() ? nullptr : Find(it->second);
}

bool TypeRegistry::Validate(std::vector<std::string>& problems) const {
    const std::size_t before = problems.size();

    for (const TypeKey key : order_) {
        const TypeDesc* desc = Find(key);
        if (desc == nullptr) {
            continue;
        }
        for (const FieldDesc& field : desc->fields) {
            const std::string where = desc->name + "." + field.name;

            if (field.kind == FieldKind::Struct && Find(field.type) == nullptr) {
                problems.push_back(where + ": struct type is not registered");
            }
            if (field.kind == FieldKind::Enum && field.attributes.enumNames.empty()) {
                // Enum tanpa nama akan diserialisasi sebagai angka mentah, dan
                // angka mentah pecah begitu ada nilai disisipkan di tengah.
                problems.push_back(where + ": enum has no value names");
            }
            if (field.kind == FieldKind::Vector) {
                if (field.vector == nullptr) {
                    problems.push_back(where + ": vector has no element operations");
                } else if (field.vector->elementKind == FieldKind::Struct &&
                           Find(field.vector->elementType) == nullptr) {
                    problems.push_back(where + ": vector element type is not registered");
                }
            }
            if (field.access == nullptr) {
                problems.push_back(where + ": field has no accessor");
            }
        }
    }
    return problems.size() == before;
}

}  // namespace sim::reflect
