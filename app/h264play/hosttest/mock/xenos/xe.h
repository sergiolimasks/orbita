/* Host-test mock of libxenon <xenos/xe.h>: only what gpu_present.c uses. */
#ifndef MOCK_XE_H
#define MOCK_XE_H
#include <stdint.h>
#include "xetypes.h"

#define SHADER_TYPE_PIXEL 1
#define SHADER_TYPE_VERTEX 0
#define XE_PRIMTYPE_RECTLIST 8
#define XE_CULL_NONE 0
#define XE_TEXADDR_CLAMP 2
#define XE_TYPE_FLOAT2 37
#define XE_TYPE_FLOAT4 38
#define XE_USAGE_POSITION 0
#define XE_USAGE_TEXCOORD 5
#define XE_FMT_8 2
#define XE_FMT_8888 6
#define XE_FMT_BGRA 0x00
#define XE_LOCK_WRITE 2
#define XE_SOURCE_COLOR 0
#define XE_CLEAR_COLOR 1
#define XE_CLEAR_DS 2

struct XenosVBFElement { int usage, index, fmt; };
struct XenosVBFFormat { int num; struct XenosVBFElement e[10]; };
struct XenosSurface {
    int width, height, wpitch, hpitch, tiled, format;
    u32 ptr, ptr_mip; int bypp; int use_filtering; int u_addressing, v_addressing;
    void *base;
    int id;              /* mock */
};
struct XenosShader { int dummy; };
struct XenosVertexBuffer { int dummy; };
struct XenosDevice {
    volatile void *rb, *rb_primary, *rb_secondary;
    int rb_secondary_wptr;
    int frameidx;
    struct XenosSurface tex_fb, default_fb;
};

void Xe_Init(struct XenosDevice *xe);
struct XenosSurface *Xe_GetFramebufferSurface(struct XenosDevice *xe);
void Xe_SetFrameBufferSurface(struct XenosDevice *xe, struct XenosSurface *fb);
struct XenosSurface *Xe_CreateTexture(struct XenosDevice *xe, unsigned w, unsigned h, unsigned levels, int format, int tiled);
void *Xe_Surface_LockRect(struct XenosDevice *xe, struct XenosSurface *s, int x, int y, int w, int h, int flags);
void Xe_Surface_Unlock(struct XenosDevice *xe, struct XenosSurface *s);
void Xe_SetRenderTarget(struct XenosDevice *xe, struct XenosSurface *rt);
struct XenosShader *Xe_LoadShaderFromMemory(struct XenosDevice *xe, void *sh);
void Xe_InstantiateShader(struct XenosDevice *xe, struct XenosShader *sh, unsigned idx);
void Xe_ShaderApplyVFetchPatches(struct XenosDevice *xe, struct XenosShader *sh, unsigned idx, const struct XenosVBFFormat *f);
struct XenosVertexBuffer *Xe_CreateVertexBuffer(struct XenosDevice *xe, int len);
void *Xe_VB_Lock(struct XenosDevice *xe, struct XenosVertexBuffer *vb, int off, int size, int flags);
void Xe_VB_Unlock(struct XenosDevice *xe, struct XenosVertexBuffer *vb);
void Xe_SetClearColor(struct XenosDevice *xe, u32 c);
void Xe_InvalidateState(struct XenosDevice *xe);
void Xe_SetZEnable(struct XenosDevice *xe, int z);
void Xe_SetCullMode(struct XenosDevice *xe, unsigned m);
void Xe_SetStreamSource(struct XenosDevice *xe, int i, struct XenosVertexBuffer *vb, int off, int stride);
void Xe_SetShader(struct XenosDevice *xe, int type, struct XenosShader *sh, int inst);
void Xe_SetTexture(struct XenosDevice *xe, int i, struct XenosSurface *t);
void Xe_DrawPrimitive(struct XenosDevice *xe, int type, int start, int n);
void Xe_ResolveInto(struct XenosDevice *xe, struct XenosSurface *s, int src, int clear);
void Xe_Execute(struct XenosDevice *xe);
void Xe_VBReclaim(struct XenosDevice *xe);
int  Xe_IsVBlank(struct XenosDevice *xe);
#endif
