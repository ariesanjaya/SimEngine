#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace sim::ai {

/// Sesuatu yang dibaca agen sebagai konteks, tanpa memanggil tool.
///
/// **Bedanya dengan tool bukan teknis melainkan soal maksud.** Tool adalah
/// tindakan yang diminta; resource adalah latar yang boleh dibaca kapan saja
/// tanpa dianggap melakukan apa pun. Klien memperlakukan keduanya berbeda —
/// sebagian melampirkan resource ke konteks tanpa bertanya, dan tidak ada satu
/// pun yang melakukan itu pada tool.
struct ResourceDefinition {
    /// Pengenalnya, mis. `simengine://logs/recent`. Skema `simengine://`
    /// dipakai supaya resource dari server ini tidak pernah bisa dikira berkas.
    std::string uri;
    std::string name;
    std::string description;
    std::string mimeType = "application/json";

    /// Alasannya sama dengan `ToolDefinition::needsMainThread`, dan bawaannya
    /// juga sama: yang harus menyatakan diri adalah yang benar-benar tidak
    /// menyentuh apa-apa.
    bool needsMainThread = true;

    std::function<std::string()> read;
};

class ResourceRegistry {
public:
    void Register(ResourceDefinition definition);
    const ResourceDefinition* Find(std::string_view uri) const;
    const std::vector<ResourceDefinition>& All() const { return resources_; }
    std::size_t Count() const { return resources_.size(); }
    void Clear();

private:
    std::vector<ResourceDefinition> resources_;
};

}  // namespace sim::ai
