#include "Sim/AIBridge/ResourceRegistry.h"

#include <algorithm>

namespace sim::ai {

void ResourceRegistry::Register(ResourceDefinition definition) {
    const auto existing =
        std::find_if(resources_.begin(), resources_.end(),
                     [&](const ResourceDefinition& resource) {
                         return resource.uri == definition.uri;
                     });
    if (existing != resources_.end()) {
        *existing = std::move(definition);
        return;
    }
    resources_.push_back(std::move(definition));
}

const ResourceDefinition* ResourceRegistry::Find(std::string_view uri) const {
    const auto found =
        std::find_if(resources_.begin(), resources_.end(),
                     [&](const ResourceDefinition& resource) { return resource.uri == uri; });
    return found == resources_.end() ? nullptr : &*found;
}

void ResourceRegistry::Clear() { resources_.clear(); }

}  // namespace sim::ai
