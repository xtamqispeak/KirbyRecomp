/* Original coordinate LUT: no game textures or executable data. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "nvn/tegra.h"

int main(void) {
    static const FormatInfo rgba8 = {4, 1, 1, FMT_COLOR, "test RGBA8"};
    TexLayout t = {.width=16, .height=16, .depth=16, .layers=1, .levels=1, .fmt=&rgba8};
    tegra_layout_init(&t);
    assert(t.block_height_log2 == 0 && t.block_depth_log2 == 4);
    assert(tegra_total_size(&t) == 16384);
    unsigned char tiled[16384], linear[16384];
    memset(tiled, 0, sizeof tiled);
    /* Explicit hardware address-bit wiring for a 16^3 RGBA8 volume.
       Unlike a swizzle/deswizzle roundtrip, this fixture does not use the
       implementation under test to generate its tiled input. */
    for (unsigned z=0; z<16; z++) for (unsigned y=0; y<16; y++)
        for (unsigned x=0; x<16; x++) for (unsigned c=0; c<4; c++) {
            unsigned byte=x*4+c;
            unsigned offset=((y&8)<<10) | (z<<9) | ((byte&32)<<3)
                | ((y&6)<<5) | ((byte&16)<<1) | ((y&1)<<4) | (byte&15);
            tiled[offset] = c==0 ? x*17 : c==1 ? y*17 : c==2 ? z*17 : 255;
        }
    LevelLayout l=tegra_level(&t, 0);
    tegra_deswizzle(tiled, linear, l.wb, l.hb, l.d, 4,
                   l.block_height_log2, l.block_depth_log2);
    for (unsigned z=0; z<16; z++) for (unsigned y=0; y<16; y++)
        for (unsigned x=0; x<16; x++) {
            unsigned i=((z*16+y)*16+x)*4;
            assert(linear[i]==x*17 && linear[i+1]==y*17);
            assert(linear[i+2]==z*17 && linear[i+3]==255);
        }
    t.depth=3; tegra_layout_init(&t);
    assert(t.block_height_log2==0 && t.block_depth_log2==2);
    t.depth=32; tegra_layout_init(&t);
    assert(t.block_depth_log2==4); /* Hardware tile-depth cap. */
    t.depth=1; tegra_layout_init(&t);
    assert(t.block_height_log2==1 && t.block_depth_log2==0);
    assert(tegra_total_size(&t)==1024);
    puts("Tegra 3D coordinate LUT and 2D layout regression checks passed");
    return 0;
}
