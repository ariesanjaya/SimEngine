#include "Sim/Core/Uuid.h"

#include <array>
#include <cstdio>
#include <random>

namespace sim {
namespace {

/// Generator per-thread.
///
/// Satu generator bersama akan butuh kunci; satu generator per pemanggilan akan
/// menghasilkan nilai yang sama untuk dua GUID yang dibuat dalam satu tik jam.
/// Per-thread menghindari keduanya.
std::mt19937_64& Engine() {
    static thread_local std::mt19937_64 engine{std::random_device{}()};
    return engine;
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

}  // namespace

Uuid Uuid::Generate() {
    std::uniform_int_distribution<uint64_t> distribution;
    Uuid id;
    id.high = distribution(Engine());
    id.low = distribution(Engine());

    // Menandai versi 4 dan varian RFC 4122. Bukan sekadar formalitas: alat luar
    // yang membaca berkas level bisa mengenali ini sebagai UUID sungguhan.
    id.high = (id.high & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    id.low = (id.low & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    // Nilai nol dipakai sebagai "tidak valid", jadi tidak boleh dihasilkan.
    if (!id.IsValid()) {
        id.low = 1;
    }
    return id;
}

std::string Uuid::ToString() const {
    std::array<char, 40> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%08x-%04x-%04x-%04x-%012llx",
                  static_cast<unsigned>(high >> 32), static_cast<unsigned>((high >> 16) & 0xFFFF),
                  static_cast<unsigned>(high & 0xFFFF), static_cast<unsigned>(low >> 48),
                  static_cast<unsigned long long>(low & 0x0000FFFFFFFFFFFFULL));
    return buffer.data();
}

Uuid Uuid::Parse(std::string_view text) {
    // Tanda hubung hanya boleh di posisi baku; menerima format lain akan membuat
    // dua teks berbeda memetakan ke GUID yang sama.
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' ||
        text[23] != '-') {
        return {};
    }

    uint64_t value = 0;
    int digits = 0;
    Uuid id;
    for (const char c : text) {
        if (c == '-') {
            continue;
        }
        const int nibble = HexValue(c);
        if (nibble < 0) {
            return {};
        }
        value = (value << 4) | static_cast<uint64_t>(nibble);
        ++digits;
        if (digits == 16) {
            id.high = value;
            value = 0;
        }
    }
    if (digits != 32) {
        return {};
    }
    id.low = value;
    return id;
}

}  // namespace sim
