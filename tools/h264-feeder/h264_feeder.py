#!/usr/bin/env python3
"""
XCloud360 — H.264 Annex-B TCP feeder pro Xbox 360 (dev loop).

Wire: [uint32_be size][annex-b AU bytes]  (1 AU por mensagem)

- Lê arquivo H.264 Annex-B do disco (default: GOLDEN).
- Segmenta em Access Units (AU) via spec H.264 7.4.1.2:
    prefix NALs (7 SPS, 8 PPS, 6 SEI, 9 AUD) pertencem à AU seguinte;
    o primeiro slice (NAL 1 ou 5) com first_mb_in_slice==0 abre AU.
- Aceita TCP no :9000 (SO_REUSEADDR + TCP_NODELAY + SO_SNDBUF baixo pra
  propagar backpressure).
- Serve N clientes concorrentes via thread por client (mesma sequência).
- Loop infinito: EOF → volta pro início (o primeiro AU do arquivo é sempre
  IDR + SPS+PPS, então o cliente recupera na virada sem pedir keyframe).
- Rate: 60fps steady (default). --burst manda full speed pra stress test.

Usage:
    python3 h264_feeder.py \
        --file ../xcloud-capture/output/GOLDEN-synth-720p60-baseline/stream.h264 \
        --port 9000 --fps 60

    # com stream shorter pra iterar rápido:
    python3 h264_feeder.py --file ../.../GOLDEN-synth-720p60-baseline-5s/stream.h264

Test do lado devbox (mesma máquina, valida wire):
    python3 h264_feeder.py --file <F> --port 9000 &
    python3 h264_feeder.py --self-test <F>   # roda validator local

Xbox client conecta em tcp://<devbox-ip>:9000 e drena com length-prefix parser.
"""
import argparse, socket, struct, sys, threading, time, pathlib

STARTCODE_4 = b"\x00\x00\x00\x01"
STARTCODE_3 = b"\x00\x00\x01"


def scan_nals(buf: bytes):
    """Yield (nal_offset_in_buf, nal_type). NAL offset = byte após start code."""
    i, n = 0, len(buf)
    while i < n - 3:
        if buf[i] == 0 and buf[i + 1] == 0:
            if buf[i + 2] == 1:
                sc = 3
            elif i + 3 < n and buf[i + 2] == 0 and buf[i + 3] == 1:
                sc = 4
            else:
                i += 1; continue
            nal_start = i + sc
            if nal_start >= n:
                break
            yield i, sc, buf[nal_start] & 0x1F
            i = nal_start + 1
        else:
            i += 1


def is_first_slice_of_picture(buf: bytes, nal_start: int) -> bool:
    """Baseline heuristic: first_mb_in_slice (ue(v)) == 0 → primeiro bit do
    RBSP após NAL header é '1' (Exp-Golomb de 0 = '1'). Robusto pra CBP."""
    if nal_start + 1 >= len(buf):
        return False
    return (buf[nal_start + 1] & 0x80) != 0  # MSB set → ue(v)=0


def segment_aus(data: bytes):
    """Segmenta arquivo Annex-B em AUs. Retorna lista de bytes(AU) com start
    codes preservados (cada AU é auto-contida, pronta pra av_parser_parse2)."""
    nals = list(scan_nals(data))
    if not nals:
        return []

    # boundaries[i] = True se a AU nova COMEÇA no NAL i.
    boundaries = [False] * len(nals)
    boundaries[0] = True
    seen_slice_in_current_au = False

    SLICE_TYPES = {1, 5}
    PREFIX_TYPES = {6, 7, 8, 9, 13, 14, 15, 16, 17, 18}  # SEI/SPS/PPS/AUD/…

    for idx, (off, sc, ntype) in enumerate(nals):
        if idx == 0:
            if ntype in SLICE_TYPES:
                seen_slice_in_current_au = True
            continue
        new_au = False
        if ntype == 9:                                 # AUD sempre abre
            new_au = True
        elif ntype in PREFIX_TYPES and seen_slice_in_current_au:
            new_au = True                              # prefix após slice
        elif ntype in SLICE_TYPES:
            nal_start = off + sc
            if is_first_slice_of_picture(data, nal_start):
                if seen_slice_in_current_au:
                    new_au = True                      # próxima picture
            # se NÃO é first_mb=0 → é slice adicional da mesma picture
        if new_au:
            boundaries[idx] = True
            seen_slice_in_current_au = False
        if ntype in SLICE_TYPES:
            seen_slice_in_current_au = True

    # Corta AUs pelas boundaries.
    aus = []
    start_offsets = [nals[i][0] for i, b in enumerate(boundaries) if b] + [len(data)]
    for i in range(len(start_offsets) - 1):
        aus.append(data[start_offsets[i]:start_offsets[i + 1]])
    return aus


def load_and_prepare(path: pathlib.Path):
    data = path.read_bytes()
    aus = segment_aus(data)
    # Sanity: primeiro AU precisa conter SPS(7)+PPS(8)+IDR(5) senão dev-loop
    # quebra ao reiniciar.
    first_types = {t for _, _, t in scan_nals(aus[0])}
    print(f"[feeder] file={path.name} size={len(data)} bytes  aus={len(aus)}  "
          f"first_au_nals={sorted(first_types)}")
    if not ({7, 8, 5} <= first_types):
        print("[feeder] WARN: primeiro AU sem SPS+PPS+IDR — loop pode falhar. "
              "Rode o arquivo por ffmpeg -bsf:v h264_mp4toannexb / regen com "
              "-force_key_frames.")
    return aus


def serve_client(conn: socket.socket, addr, aus, fps: int, burst: bool):
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    conn.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 64 * 1024)  # backpressure
    period = 1.0 / fps if fps > 0 else 0.0
    print(f"[feeder] client {addr} connected (fps={fps} burst={burst})")
    frames_sent = 0
    t0 = time.monotonic()
    try:
        while True:
            for au in aus:
                header = struct.pack(">I", len(au))
                conn.sendall(header + au)
                frames_sent += 1
                if not burst:
                    # rate limit steady (deadline-based, sem drift).
                    deadline = t0 + frames_sent * period
                    slack = deadline - time.monotonic()
                    if slack > 0:
                        time.sleep(slack)
                if frames_sent % 300 == 0:
                    elapsed = time.monotonic() - t0
                    print(f"[feeder] {addr} sent={frames_sent} "
                          f"eff_fps={frames_sent/elapsed:.1f}")
            print(f"[feeder] {addr} EOF → loop restart (frame {frames_sent})")
    except (BrokenPipeError, ConnectionResetError, OSError) as e:
        elapsed = time.monotonic() - t0
        print(f"[feeder] client {addr} gone after {frames_sent} frames / "
              f"{elapsed:.1f}s ({e.__class__.__name__})")
    finally:
        try: conn.close()
        except Exception: pass


def run_server(path, port, fps, burst):
    aus = load_and_prepare(pathlib.Path(path))
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", port))
    s.listen(4)
    print(f"[feeder] listening on 0.0.0.0:{port} (waiting Xbox)…")
    try:
        while True:
            conn, addr = s.accept()
            threading.Thread(
                target=serve_client, args=(conn, addr, aus, fps, burst),
                daemon=True, name=f"cli-{addr[0]}:{addr[1]}"
            ).start()
    except KeyboardInterrupt:
        print("\n[feeder] shutdown")
    finally:
        s.close()


def self_test(path):
    """Valida wire local: segmenta arquivo, verifica invariantes."""
    aus = load_and_prepare(pathlib.Path(path))
    total_bytes = sum(len(a) for a in aus)
    idrs = sum(1 for a in aus if any(t == 5 for _, _, t in scan_nals(a)))
    print(f"[test] AUs={len(aus)}  total={total_bytes}  IDR_AUs={idrs}")
    # emit binário length-prefixed pra inspecionar com xxd
    out = pathlib.Path(path).with_suffix(".framed")
    with out.open("wb") as f:
        for au in aus:
            f.write(struct.pack(">I", len(au))); f.write(au)
    print(f"[test] wrote {out} ({out.stat().st_size} bytes)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", "-f", required=False,
                    default="../xcloud-capture/output/GOLDEN-synth-720p60-baseline/stream.h264")
    ap.add_argument("--port", "-p", type=int, default=9000)
    ap.add_argument("--fps", type=int, default=60, help="0 = full speed / --burst equivale")
    ap.add_argument("--burst", action="store_true", help="ignora fps, manda full speed")
    ap.add_argument("--self-test", nargs="?", const=True, default=False,
                    help="valida segmentação e escreve arquivo .framed pra debug")
    args = ap.parse_args()
    target = args.__dict__["self_test"]
    if target:
        self_test(args.file if target is True else target)
        return
    run_server(args.file, args.port, args.fps, args.burst)


if __name__ == "__main__":
    main()
