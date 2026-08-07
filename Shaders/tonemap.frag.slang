// Penyelesaian HDR ke target tampilan: eksposur, ACES, encode sRGB.

#include "tonemap_common.slang"

struct Push {
    /// x: 1 saat post-process menyala. Saat mati, isinya disalin apa adanya —
    /// **tetap lewat pass ini, bukan dengan melewatinya.** Melewatinya berarti
    /// target tampilan tidak tersentuh sama sekali dan yang terlihat adalah isi
    /// frame sebelumnya, yang tampak seperti gambar yang membeku.
    float4 params;
};

[[vk::push_constant]] ConstantBuffer<Push> pc;
[[vk::binding(0, 0)]] Sampler2D uScene;
[[vk::binding(1, 0)]] Sampler2D uExposure;

[shader("fragment")]
float4 main(float4 position : SV_Position) : SV_Target {
    // Koordinat texel bulat, bukan uv ternormalisasi: gambar HDR dialokasikan
    // lebih besar daripada viewport, jadi uv 1,0 bukan tepi gambar yang digambar.
    int3 texel = int3(int2(position.xy), 0);
    float3 color = uScene.Load(texel).rgb;

    if (pc.params.x < 0.5) {
        return float4(saturate(color), 1.0);
    }

    color *= uExposure.Load(int3(0, 0, 0)).r;
    return float4(linearToSrgb(acesToneMap(color)), 1.0);
}
