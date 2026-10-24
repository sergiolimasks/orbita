# ffmpeg-xenon AltiVec patch

## Arquivos
- `xen_altivec_asm.h` — header wrapper com 20 wrappers inline-asm + macro-dispatch por tipo
- `patched-ffmpeg.diff` — diff unified aplicavel com `patch -p1` no root do ffmpeg

## Aplicar

```bash
cd /home/srglimasks/ffmpeg-xenon
patch -p1 < /mnt/storage/projetos/XCloud360/research/altivec/patched-ffmpeg.diff
```

O patch:
1. Adiciona `libavcodec/ppc/xen_altivec_asm.h` (novo)
2. Modifica `libavcodec/ppc/types_altivec.h` pra ativar `XEN_ALTIVEC_MACRO_DISPATCH` + include

Como TODOS os arquivos que usam builtins problematicos incluem `types_altivec.h` (direta ou
indiretamente via `#include "h264_template_altivec.c"` no `h264_altivec.c`), o macro
dispatch substitui as chamadas em compile-time via `__builtin_choose_expr` +
`__builtin_types_compatible_p` — SEM tocar nas call sites.

## Reconfigurar ffmpeg (remover --disable-altivec)

Comando original (do config.log) tinha `--disable-altivec`. Novo:

```bash
cd /home/srglimasks/ffmpeg-xenon
./configure \
  --target-os=none --arch=powerpc --cpu=cell \
  --enable-cross-compile --cc=xenon-gcc --as=xenon-as --ar=xenon-ar \
  --nm=xenon-nm --strip=xenon-strip --cross-prefix=xenon- \
  --enable-static --disable-shared \
  --disable-everything --enable-decoder=h264 --enable-parser=h264 \
  --enable-demuxer=h264 --enable-protocol=file \
  --disable-doc --disable-network --disable-encoders --disable-muxers \
  --disable-devices --disable-filters --disable-ffmpeg --disable-ffplay \
  --disable-ffprobe --disable-ffserver --disable-swscale --disable-postproc \
  --disable-avdevice --disable-pthreads --disable-w32threads --disable-os2threads \
  --disable-symver \
  --extra-cflags='-O3 -m32 -fno-pic -mpowerpc64 -mcpu=cell -mtune=cell -mhard-float -maltivec -mabi=altivec -DXENON -D__XENON__' \
  --extra-ldflags='-m32 -fno-pic -mpowerpc64 -L/home/srglimasks/devkitxenon/xenon/lib/32 -L/home/srglimasks/devkitxenon/usr/lib -T/home/srglimasks/devkitxenon/app.lds -u read -u _start -u exc_base' \
  --extra-libs='-lxenon -lm -lc'
```

**Mudancas do comando original**:
- Removido: `--disable-altivec`
- Adicionado em `--extra-cflags`: `-maltivec -mabi=altivec` (redundante — configure adiciona,
  mas garante fallback caso o check_cflags interno falhe)

Depois:
```bash
make clean && make -j$(nproc)
```

## Cobertura verificada

Todos os 143 call-sites de builtins quebrados mapeados. Dispatch por tipo:

| Builtin | Ramos | Ocorrencias no ffmpeg |
|---------|-------|-----------------------|
| `vec_msum` | u8/s8/s16/u16 → xen_vmsumubm/mbm/shm/uhm | 12 (dsputil, int, vp8dsp) |
| `vec_mladd` | s16/u16 → xen_vmladduhm(_u) | 40 (dsputil, h264, gmc, int, mpegvideo) |
| `vec_sum4s` | u8/s8/s16 → xen_vsum4ubs/sbs/shs | 30 (dsputil) |
| `vec_sums` | s32 → xen_vsumsws | 14 (dsputil, int) |
| `vec_mule` | u8/s8/u16/s16 → xen_vmul[eo](ub/sb/uh/sh) | 6 |
| `vec_mulo` | idem | 8 |
| `vec_sum2s` | s32 → xen_vsum2sws (exposto, nao usado por ffmpeg) | 0 |

## Que NAO precisa wrapper

Funciona vanilla no gcc 9.2 xenon:
- `vec_pack` (47x), `vec_packs` (4x), `vec_packsu` (23x)
- `vec_avg` (16x)
- `vec_add`, `vec_sub`, `vec_madd` (float), `vec_perm`, `vec_ld`, `vec_st`
- `vec_mergeh`, `vec_mergel`, `vec_unpackh`, `vec_unpackl`
- `vec_and`, `vec_or`, `vec_xor`, `vec_min`, `vec_max`, `vec_sra`, `vec_sld`, `vec_sel`
- `vec_adds`, `vec_subs`, `vec_splat*`, `vec_cmp*`

## Bloqueadores

Nenhum. Todos os 4 arquivos `.S` (asm.S, fft_altivec_s.S) sao assembly puro
sem builtins C — nao precisam patch.

## Assembly files

- `libavcodec/ppc/asm.S` — helpers PPC genericos (nao usa builtins)
- `libavcodec/ppc/fft_altivec_s.S` — kernel FFT (assembly puro AltiVec, ok como esta)

Ambos sao assembly Motorola AltiVec, ja aceitos pelo xenon-as vanilla.
