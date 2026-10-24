// yuv_present.c - libxenon PoC: sobe 3 planes YUV420p, converte em GPU, presenta.
// Padrao decalcado de xmplayer vo_xenon.c (Ced2911).
// Fonte: https://github.com/LibXenonProject/xmplayer/blob/master/mplayer/libvo/vo_xenon.c

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <xetypes.h>
#include <xenos/xe.h>
#include <xenos/xenos.h>

// Blobs .psu/.vsu gerados pelo rshadercompiler e embutidos via xxd -i.
// (build-time: veja Makefile.build-shader.sh)
extern const uint8_t g_yuv_ps[];
extern const uint32_t g_yuv_ps_len;
extern const uint8_t g_yuv_vs[];
extern const uint32_t g_yuv_vs_len;

typedef struct {
    struct XenosSurface *Y, *U, *V;
} YuvTex;

typedef struct {
    float x, y, z, w;
    float u, v;
} vtx_t;

static struct XenosDevice *g_xe = NULL;
static struct XenosShader *g_ps = NULL, *g_vs = NULL;
static struct XenosVertexBuffer *g_vb = NULL;
static YuvTex g_yuv = {0};

// -- 1) init: shaders + VB + textures ---------------------------------------
int yuv_init(struct XenosDevice *xe, int w, int h)
{
    g_xe = xe;

    // vertex buffer format tem que bater com o do .vs.hlsl
    struct XenosVBFFormat vbf = {
        2,
        {
            { XE_USAGE_POSITION, 0, XE_TYPE_FLOAT4 },
            { XE_USAGE_TEXCOORD, 0, XE_TYPE_FLOAT2 },
        }
    };

    // shaders (blobs embed no ELF via xxd -i)
    g_ps = Xe_LoadShaderFromMemory(xe, (void*)g_yuv_ps);
    Xe_InstantiateShader(xe, g_ps, 0);

    g_vs = Xe_LoadShaderFromMemory(xe, (void*)g_yuv_vs);
    Xe_InstantiateShader(xe, g_vs, 0);
    Xe_ShaderApplyVFetchPatches(xe, g_vs, 0, &vbf);

    // vertex buffer: 3 vertices para RECTLIST (top-left, bottom-left, top-right)
    g_vb = Xe_CreateVertexBuffer(xe, 3 * sizeof(vtx_t));
    vtx_t *v = (vtx_t*)Xe_VB_Lock(xe, g_vb, 0, 3 * sizeof(vtx_t), XE_LOCK_WRITE);
    // top-left
    v[0].x=-1; v[0].y= 1; v[0].z=0; v[0].w=1; v[0].u=0; v[0].v=0;
    // bottom-left
    v[1].x=-1; v[1].y=-1; v[1].z=0; v[1].w=1; v[1].u=0; v[1].v=1;
    // top-right
    v[2].x= 1; v[2].y= 1; v[2].z=0; v[2].w=1; v[2].u=1; v[2].v=0;
    Xe_VB_Unlock(xe, g_vb);

    // 3 texturas R8_UNORM (XE_FMT_8). U e V com metade da resolucao (YUV420).
    g_yuv.Y = Xe_CreateTexture(xe, w,      h,      1, XE_FMT_8, 0);
    g_yuv.U = Xe_CreateTexture(xe, w >> 1, h >> 1, 1, XE_FMT_8, 0);
    g_yuv.V = Xe_CreateTexture(xe, w >> 1, h >> 1, 1, XE_FMT_8, 0);

    g_yuv.Y->u_addressing = XE_TEXADDR_CLAMP;
    g_yuv.Y->v_addressing = XE_TEXADDR_CLAMP;
    g_yuv.U->u_addressing = XE_TEXADDR_CLAMP;
    g_yuv.U->v_addressing = XE_TEXADDR_CLAMP;
    g_yuv.V->u_addressing = XE_TEXADDR_CLAMP;
    g_yuv.V->v_addressing = XE_TEXADDR_CLAMP;

    return 0;
}

// -- 2) upload frame: copia 3 planos (Y, U, V) respeitando wpitch --------------
// src[0]=Y stride[0], src[1]=U stride[1], src[2]=V stride[2] (ffmpeg layout).
void yuv_upload(uint8_t *const src[3], const int stride[3], int w, int h)
{
    void *dstY = Xe_Surface_LockRect(g_xe, g_yuv.Y, 0,0,0,0, XE_LOCK_WRITE);
    void *dstU = Xe_Surface_LockRect(g_xe, g_yuv.U, 0,0,0,0, XE_LOCK_WRITE);
    void *dstV = Xe_Surface_LockRect(g_xe, g_yuv.V, 0,0,0,0, XE_LOCK_WRITE);

    // copia linha a linha porque wpitch (>=w, alinhado ao GPU) != stride
    for (int y = 0; y < h; y++)
        memcpy((uint8_t*)dstY + y * g_yuv.Y->wpitch, src[0] + y * stride[0], w);

    int cw = w >> 1, ch = h >> 1;
    for (int y = 0; y < ch; y++) {
        memcpy((uint8_t*)dstU + y * g_yuv.U->wpitch, src[1] + y * stride[1], cw);
        memcpy((uint8_t*)dstV + y * g_yuv.V->wpitch, src[2] + y * stride[2], cw);
    }

    Xe_Surface_Unlock(g_xe, g_yuv.Y);
    Xe_Surface_Unlock(g_xe, g_yuv.U);
    Xe_Surface_Unlock(g_xe, g_yuv.V);
}

// -- 3) render: bind, draw, resolve --------------------------------------------
void yuv_render(void)
{
    Xe_SetCullMode(g_xe, XE_CULL_NONE);
    Xe_SetStreamSource(g_xe, 0, g_vb, 0, sizeof(vtx_t));
    Xe_SetShader(g_xe, SHADER_TYPE_PIXEL,  g_ps, 0);
    Xe_SetShader(g_xe, SHADER_TYPE_VERTEX, g_vs, 0);
    Xe_SetTexture(g_xe, 0, g_yuv.Y);
    Xe_SetTexture(g_xe, 1, g_yuv.U);
    Xe_SetTexture(g_xe, 2, g_yuv.V);
    // RECTLIST = 1 rect a partir dos 3 primeiros vertices. Zero overdraw, zero index buffer.
    Xe_DrawPrimitive(g_xe, XE_PRIMTYPE_RECTLIST, 0, 1);
}
