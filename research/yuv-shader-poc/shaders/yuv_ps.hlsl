// yuv_ps.hlsl - Xbox 360 pixel shader YUV420p -> RGB BT.601 (limited range)
// Target: ps_3_0 (compilar com rshadercompiler + XNA 3.0 no Windows/Wine)
// Reconstruido a partir de:
//   https://github.com/LibXenonProject/xmplayer/blob/master/mplayer/libvo/vo_xenon.c
//   (blob binario em `static const uint32_t ps[]` linha 82-103, reflection header
//    linhas 68-81: registra 3 samplers s0=YTexture s1=UTexture s2=VTexture)
//
// A matematica no bytecode dele (constantes 0xbf5ee978 etc.) confere com BT.601
// limited-range: Y' = 1.164*(Y-16) ; U' = U-128 ; V' = V-128
//   R = Y' + 1.596*V'
//   G = Y' - 0.391*U' - 0.813*V'
//   B = Y' + 2.018*U'
//
// Note: input Y/U/V vem como float [0,1] porque sample de XE_FMT_8 (R8_UNORM).
// Isso equivale ao byte /255 -- ajustar constantes se quiser byte cru.

sampler YTexture: register(s0);
sampler UTexture: register(s1);
sampler VTexture: register(s2);

struct PS_INPUT {
    float2 uv : TEXCOORD0;
};

float4 main(PS_INPUT input) : COLOR
{
    // Sample dos 3 planes. UV subamostrado 2x2 -- bilinear do sampler faz o resto.
    float y = tex2D(YTexture, input.uv).r;
    float u = tex2D(UTexture, input.uv).r;
    float v = tex2D(VTexture, input.uv).r;

    // BT.601 limited-range (TV range). Se seu H.264 sair full-range, use BT.601 full.
    float3 yuv = float3(y - (16.0/255.0), u - 0.5, v - 0.5);

    float3 rgb;
    rgb.r = 1.164 * yuv.x                 + 1.596 * yuv.z;
    rgb.g = 1.164 * yuv.x - 0.391 * yuv.y - 0.813 * yuv.z;
    rgb.b = 1.164 * yuv.x + 2.018 * yuv.y;

    return float4(saturate(rgb), 1.0);
}
