/* Opus decode of the first N packets of a .pkts file. Run under qemu-ppc with
 * an instruction trace; (count(N2) - count(N1)) / (N2 - N1) = instructions
 * per 20 ms frame. usage: ppc_bench file.pkts N [plc_every] */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <opus.h>

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    int n = atoi(argv[2]), plc = argc > 3 ? atoi(argv[3]) : 0;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    static uint8_t buf[2000];
    static int16_t pcm[5760 * 2];
    int err, done = 0;
    OpusDecoder *d = opus_decoder_create(48000, 2, &err);
    uint8_t h[8];
    while (done < n && fread(h, 1, 8, f) == 8) {
        int len = h[0] << 8 | h[1];
        if (len > 2000 || fread(buf, 1, len, f) != (size_t)len) break;
        int r = (plc && done % plc == 0) ? opus_decode(d, NULL, 0, pcm, 960, 0)
                                         : opus_decode(d, buf, len, pcm, 5760, 0);
        if (r < 0) return 3;
        done++;
    }
    printf("%d\n", done);
    return 0;
}
