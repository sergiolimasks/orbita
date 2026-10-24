// yuv_vs.hlsl - Xbox 360 vertex shader passthrough para RECTLIST fullscreen quad.
// Target: vs_3_0
//
// Baseado no `static const uint32_t vs[]` do vo_xenon.c (sem constant buffer,
// so passa POSITION + TEXCOORD0). VBFFormat esperado:
//   { {XE_USAGE_POSITION, 0, XE_TYPE_FLOAT4},
//     {XE_USAGE_TEXCOORD, 0, XE_TYPE_FLOAT2} }
//
// Vertex buffer struct { float x,y,z,w; float u,v; } == stride 24 bytes.
// Para XE_PRIMTYPE_RECTLIST voce passa apenas 3 vertices (top-left, bottom-left,
// top-right) -- o 4o e derivado. Xe_DrawPrimitive(dev, XE_PRIMTYPE_RECTLIST, 0, 1).

struct VS_INPUT {
    float4 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

struct VS_OUTPUT {
    float4 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

VS_OUTPUT main(VS_INPUT input)
{
    VS_OUTPUT output;
    output.pos = input.pos;  // ja em clip-space [-1,1]
    output.uv  = input.uv;
    return output;
}
