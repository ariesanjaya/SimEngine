#include "Sim/AIBridge/ToolRegistry.h"

#include <algorithm>

namespace sim::ai {

const char* ToString(ToolPermission permission) {
    switch (permission) {
        case ToolPermission::Read:
            return "read";
        case ToolPermission::Write:
            return "write";
        case ToolPermission::Dangerous:
            return "dangerous";
    }
    return "read";
}

void ToolRegistry::Register(ToolDefinition definition) {
    const auto existing = std::find_if(tools_.begin(), tools_.end(),
                                       [&](const ToolDefinition& tool) {
                                           return tool.name == definition.name;
                                       });
    if (existing != tools_.end()) {
        *existing = std::move(definition);
        return;
    }
    tools_.push_back(std::move(definition));
}

const ToolDefinition* ToolRegistry::Find(std::string_view name) const {
    const auto found = std::find_if(
        tools_.begin(), tools_.end(),
        [&](const ToolDefinition& tool) { return tool.name == name; });
    return found == tools_.end() ? nullptr : &*found;
}

void ToolRegistry::Clear() { tools_.clear(); }

}  // namespace sim::ai
