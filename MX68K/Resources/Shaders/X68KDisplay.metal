#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float2 position [[attribute(0)]];
    float2 texCoord [[attribute(1)]];
};

struct VertexOut {
    float4 position [[position]];
    float2 texCoord;
};

vertex VertexOut x68kVertex(VertexIn in [[stage_in]]) {
    VertexOut out;
    out.position = float4(in.position, 0.0, 1.0);
    out.texCoord = in.texCoord;
    return out;
}

fragment float4 x68kFragment(VertexOut in [[stage_in]],
                             texture2d<float> texture [[texture(0)]],
                             sampler s [[sampler(0)]],
                             constant uint &scanlineEnabled [[buffer(0)]]) {
    float4 color = texture.sample(s, in.texCoord);
    // P664: 走査線エフェクト(MVP)。スクリーン座標(in.position.y、drawable
    // ピクセル基準)の偶数行を減光し、CRTの走査線を模した見た目にする。
    // Retina等高DPI環境ではdrawableピクセル基準のため、ゲスト側の実ライン数
    // とは厳密には一致しない(MVPスコープの既知の簡略化)。
    if (scanlineEnabled != 0 && (int(in.position.y) % 2) == 0) {
        color.rgb *= 0.7;
    }
    return color;
}
