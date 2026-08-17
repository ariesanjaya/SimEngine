#include "Sim/Assets/Cook.h"

#include "Sim/Core/Log.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <fstream>
#include <unordered_set>

namespace sim::assets {
namespace {

bool IsHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/// Apakah `text` pada posisi `at` berbentuk 8-4-4-4-12 heksadesimal.
bool LooksLikeGuid(std::string_view text, std::size_t at) {
    constexpr int kGroups[] = {8, 4, 4, 4, 12};
    std::size_t cursor = at;
    for (int group = 0; group < 5; ++group) {
        if (group > 0) {
            if (cursor >= text.size() || text[cursor] != '-') {
                return false;
            }
            ++cursor;
        }
        for (int digit = 0; digit < kGroups[group]; ++digit) {
            if (cursor >= text.size() || !IsHex(text[cursor])) {
                return false;
            }
            ++cursor;
        }
    }
    // Karakter sesudahnya tidak boleh heksadesimal: tanpa pemeriksaan ini,
    // sebuah hash 40 karakter terbaca sebagai GUID yang diikuti sampah.
    return cursor >= text.size() || !IsHex(text[cursor]);
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

}  // namespace

std::vector<Uuid> GuidsIn(std::string_view text) {
    std::vector<Uuid> found;
    std::unordered_set<std::string> seen;
    constexpr std::size_t kGuidLength = 36;
    if (text.size() < kGuidLength) {
        return found;
    }
    for (std::size_t at = 0; at + kGuidLength <= text.size(); ++at) {
        // Sebuah GUID tidak pernah didahului heksadesimal. Tanpa ini, setiap
        // pergeseran satu karakter di dalam sebuah GUID yang lebih panjang ikut
        // diperiksa dan sebagian lolos.
        if (at > 0 && IsHex(text[at - 1])) {
            continue;
        }
        if (!LooksLikeGuid(text, at)) {
            continue;
        }
        std::string candidate(text.substr(at, kGuidLength));
        std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const Uuid parsed = Uuid::Parse(candidate);
        if (parsed.IsValid() && seen.insert(candidate).second) {
            found.push_back(parsed);
        }
        at += kGuidLength - 1;
    }
    return found;
}

CookPlan PlanCook(const AssetDatabase& database,
                  const std::vector<std::filesystem::path>& levels) {
    CookPlan plan;
    plan.levels = levels;
    std::sort(plan.levels.begin(), plan.levels.end());

    std::unordered_set<std::string> reached;
    std::deque<Uuid> pending;

    // GUID yang tidak ada di indeks dilewati diam-diam. Ia bisa berarti apa
    // saja: GUID entity, GUID node material, atau referensi yang benar-benar
    // menggantung — dan tidak ada di sini yang bisa membedakannya. Lihat
    // catatan di `CookPlan`.
    const auto push = [&](const Uuid& guid) {
        if (!guid.IsValid() || database.Find(guid) == nullptr) {
            return;
        }
        if (reached.insert(guid.ToString()).second) {
            pending.push_back(guid);
        }
    };

    for (const std::filesystem::path& level : plan.levels) {
        const std::string text = ReadFile(level);
        if (text.empty()) {
            SIM_WARN("Assets", "cook: cannot read {}", level.string());
            continue;
        }
        for (const Uuid& guid : GuidsIn(text)) {
            push(guid);
        }
    }

    // Lebar dulu, dan lewat `dependencies` — indeks yang sama yang dipakai
    // `UsersOf`, hanya dibaca ke arah sebaliknya.
    while (!pending.empty()) {
        const Uuid guid = pending.front();
        pending.pop_front();
        const AssetRecord* record = database.Find(guid);
        if (record == nullptr) {
            continue;
        }
        for (const Uuid& dependency : record->dependencies) {
            push(dependency);
        }
    }

    for (const AssetRecord& record : database.All()) {
        const bool kept = reached.count(record.guid.ToString()) != 0;
        if (kept) {
            plan.reachable.push_back(record.guid);
            plan.reachableBytes += record.fileSize;
        } else {
            plan.unreachable.push_back(record.guid);
            plan.unreachableBytes += record.fileSize;
        }
    }

    // Terurut menurut jalur, bukan menurut GUID: dua kali menjalankan cook atas
    // project yang sama harus menghasilkan manifest yang sama persis, dan GUID
    // tidak punya urutan yang berarti bagi siapa pun yang membacanya.
    const auto byPath = [&database](const Uuid& a, const Uuid& b) {
        const AssetRecord* left = database.Find(a);
        const AssetRecord* right = database.Find(b);
        if (left == nullptr || right == nullptr) {
            return left != nullptr;
        }
        return left->relativePath < right->relativePath;
    };
    std::sort(plan.reachable.begin(), plan.reachable.end(), byPath);
    std::sort(plan.unreachable.begin(), plan.unreachable.end(), byPath);

    return plan;
}

}  // namespace sim::assets
