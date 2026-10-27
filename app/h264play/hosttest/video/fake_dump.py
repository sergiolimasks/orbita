#!/usr/bin/env python3
"""hosttest: gera um dump SINTÉTICO no formato do orbita-dump (XEX local),
pra testar tools/video/dump2mode.py sem console.

  fake_dump.py 720p  <libxenon xenos_videomodesdata.h> > d720.txt
      ANA = ana_hdmi_720p_corona do libxenon (o check720 tem que dar 100%)
  fake_dump.py 1080p <libxenon xenos_videomodesdata.h> > d1080.txt
      ANA = a mesma tabela com o PLL/timing HDMI trocados por valores
      inventados (NÃO É UMA TABELA REAL), CRTC 2200x1125, scaler ligado.
GPU sai "cru" com bswap (como poderia vir pela janela 0x7FC80000), pra
testar a detecção de ordem de bytes.
"""
import re
import sys


def table(header, name):
    txt = open(header, encoding="utf-8", errors="replace").read()
    body = re.search(r"uint32_t\s+" + name + r"\s*\[\]\s*=\s*\{(.*?)\};", txt, re.S).group(1)
    body = re.sub(r"//[^\n]*", "", body)
    return [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", body)]


def bswap(v):
    return int.from_bytes(v.to_bytes(4, "big"), "little")


def main():
    kind, header = sys.argv[1], sys.argv[2]
    ana = table(header, "ana_hdmi_720p_corona")
    if kind == "1080p":
        ana = list(ana)
        for i, v in ((0xCD, 0x11111111), (0xCE, 0x22222222), (0xD0, 0x33333333), (0xD1, 0x44444444),
                     (0xD2, 0x00005555), (0x13, 0x82500898), (0x15, 0x32028465)):
            ana[i] = v
        w, h, tw, th, hso, vso = 1920, 1080, 2200, 1125, 191, 41
    else:
        w, h, tw, th, hso, vso = 1280, 720, 1650, 750, 259, 25
    print("# orbita-dump v1 (SINTETICO, hosttest)")
    print(f"FILE game:\\orbita-dump-{h}p.txt")
    print(f"VIDEO width={w} height={h} interlaced=0 widescreen=1 hidef=1 refresh=60.000 standard=1")
    print("AVPACK smc=0x1f   # 0f1f0000000000000000000000000000")
    print("HWINFO flags=0x60000000 type=6 pcibridge=0x70 raw=60000000027000000000000000000000")
    print("KERNEL 2.0.17559 qfe=0")
    print("DVE ana_fe=0x00020003 version=0x23")
    for ph in "AB":
        for i, v in enumerate(ana):
            if v == 0xFFFFFFFF:
                print(f"ANA {ph} 0x{i:02x} ERR 5   # 9105000000000000")
            else:
                print(f"ANA {ph} 0x{i:02x} 0x{v:08x}   # 91000000{v:08x}")
    gpu = {0x6000: tw - 1, 0x6010: th - 1, 0x6004: (hso << 16) | (w + hso), 0x6014: (vso << 16) | (h + vso),
           0x6110: 0x1e000000, 0x6120: 1280, 0x6134: 1280, 0x6138: 720, 0x652c: 0x300,
           0x6580: 0, 0x6584: (1280 << 16) | 720, 0x6590: 1 if w != 1280 else 0, 0x6594: 0x905,
           0x65a0: 0, 0x65a4: 0, 0x65b0: 0, 0x65b4: 0x00AAAAA0 if w != 1280 else 0x01000000,
           0x65b8: 0x3d600, 0x65c0: 0x01000000, 0x65c4: 0x00AAAAA0 if w != 1280 else 0x01000000,
           0x65c8: 0x35600, 0x65cc: 0, 0x65d0: 0, 0x65dc: 0}
    for ph in "AB":
        for o in sorted(gpu):
            print(f"GPU {ph} 0x{o:04x} 0x{bswap(gpu[o]):08x}   # bswap 0x{gpu[o]:08x}")
    print("D3D createdevice=0x00000000 backbuffer=1280x720")
    print("END gpu_fault=0")


if __name__ == "__main__":
    main()
