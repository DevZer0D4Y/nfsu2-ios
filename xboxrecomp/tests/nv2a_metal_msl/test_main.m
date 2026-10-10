/*
 * nv2a_metal_msl test: every shader nv2a_metal_msl.c generates must compile
 * with the Metal compiler and link into a render pipeline, and the clip-space
 * mapping must put surface pixel (0,0) at the top left with the NV2A's
 * on-screen winding.
 *
 *   nv2a_metal_msl_test [progcache.bin]
 *
 * Without an argument it checks randomly generated vertex programs and
 * combiner states (every opcode and register reachable); with one it also
 * checks every program recorded by a run (the renderers' progcache.bin --
 * game-derived, so it is never part of the repository).
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "nv2a_metal_msl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static id<MTLDevice> s_dev;
static int s_fail, s_done;
static char s_vsrc[512 * 1024], s_fsrc[128 * 1024];

static uint32_t s_rng = 0x2545F491u;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static id<MTLFunction> function(const char *src, const char *name, BOOL points, const char *what)
{
    MTLCompileOptions *opt = [MTLCompileOptions new];
    NSError *err = nil;
    id<MTLLibrary> lib;
    id<MTLFunction> fn;
    MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];

    opt.mathMode = MTLMathModeSafe;
    lib = [s_dev newLibraryWithSource:[NSString stringWithUTF8String:src] options:opt error:&err];
    if (!lib) {
        fprintf(stderr, "FAIL %s: %s\n", what, err.localizedDescription.UTF8String);
        if (s_fail++ == 0)
            fprintf(stderr, "---- source ----\n%s\n----------------\n", src);
        return nil;
    }
    [cv setConstantValue:&points type:MTLDataTypeBool atIndex:NV2A_MSL_FC_POINTS];
    fn = [lib newFunctionWithName:[NSString stringWithUTF8String:name] constantValues:cv error:&err];
    if (!fn) {
        fprintf(stderr, "FAIL %s (function): %s\n", what, err.localizedDescription.UTF8String);
        s_fail++;
    }
    return fn;
}

static MTLVertexDescriptor *float4_inputs(void)
{
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    int a;
    for (a = 0; a < 16; a++) {
        vd.attributes[a].format = MTLVertexFormatFloat4;
        vd.attributes[a].offset = 0;
        vd.attributes[a].bufferIndex = (NSUInteger)a;
        vd.layouts[a].stride = 16;
        vd.layouts[a].stepFunction = MTLVertexStepFunctionPerVertex;
    }
    return vd;
}

static id<MTLRenderPipelineState> pipeline(id<MTLFunction> vs, id<MTLFunction> fs, const char *what)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    NSError *err = nil;
    id<MTLRenderPipelineState> p;
    d.vertexFunction = vs;
    d.fragmentFunction = fs;
    d.vertexDescriptor = float4_inputs();
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    d.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    p = [s_dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!p) {
        fprintf(stderr, "FAIL %s (pipeline): %s\n", what, err.localizedDescription.UTF8String);
        s_fail++;
    }
    return p;
}

/* One program pair: vertex shader both ways (points or not), fragment shader,
 * linked. */
static void check(const uint32_t (*prog)[4], uint32_t slots, uint32_t start,
                  const Nv2aPshKey *pk, const char *what)
{
    @autoreleasepool {
        id<MTLFunction> vs, vsp, fs;
        if (nv2a_msl_vertex(prog, slots, start, s_vsrc, sizeof s_vsrc) < 0) {
            fprintf(stderr, "FAIL %s: vertex shader generation\n", what);
            s_fail++;
            return;
        }
        if (nv2a_msl_fragment(pk, s_fsrc, sizeof s_fsrc) < 0) {
            fprintf(stderr, "FAIL %s: fragment shader generation\n", what);
            s_fail++;
            return;
        }
        vs = function(s_vsrc, NV2A_MSL_VS_NAME, NO, what);
        vsp = function(s_vsrc, NV2A_MSL_VS_NAME, YES, what);
        fs = function(s_fsrc, NV2A_MSL_FS_NAME, NO, what);
        if (vs && fs)
            pipeline(vs, fs, what);
        if (vsp && fs)
            pipeline(vsp, fs, what);
        s_done++;
    }
}

/* Texture modes gl_psh.c handles distinctly, and a few it maps to 2D. */
static const uint32_t s_modes[] = { 0, 1, 2, 3, 4, 5, 6, 15, 16, 17 };

static void random_key(Nv2aPshKey *k)
{
    int i;
    memset(k, 0, sizeof *k);
    for (i = 0; i < 8; i++) {
        k->color_icw[i] = rnd();
        k->color_ocw[i] = rnd() & 0x000FFFFFu;
        k->alpha_icw[i] = rnd();
        k->alpha_ocw[i] = rnd() & 0x0003FFFFu;
    }
    k->control = (rnd() % 9) | (rnd() & 0x00011100u);
    k->final0 = rnd();
    k->final1 = rnd() & 0xFFFFFFE0u;
    for (i = 0; i < 4; i++)
        k->shader_program |= s_modes[rnd() % (sizeof s_modes / sizeof s_modes[0])] << (5 * i);
    k->other_input = rnd() & 0x00FF0000u;
}

static void fuzz(int n)
{
    static uint32_t prog[136][4];
    int t;
    for (t = 0; t < n; t++) {
        uint32_t len = 1 + rnd() % 48, start = rnd() % 4, i;
        Nv2aPshKey k;
        char what[64];
        for (i = 0; i < start + len; i++) {
            prog[i][0] = rnd(); prog[i][1] = rnd(); prog[i][2] = rnd();
            prog[i][3] = rnd() & ~1u;                   /* FINAL only at the end */
        }
        prog[start + len - 1][3] |= 1;
        random_key(&k);
        snprintf(what, sizeof what, "random program %d", t);
        check((const uint32_t (*)[4])prog, start + len, start, &k, what);
        snprintf(what, sizeof what, "random fixed-function %d", t);
        check(NULL, 0, 0, &k, what);
    }
}

/* progcache.bin: nv2a_gl.c / nv2a_vk.c ProgRec records. */
#define PROG_REC_MAGIC 0x4350564Eu
typedef struct {
    uint32_t   magic, xform, vp_start, slots;
    Nv2aPshKey pk;
    uint32_t   prog[136][4];
} ProgRec;

static int records(const char *path)
{
    FILE *f = fopen(path, "rb");
    ProgRec rec;
    int n = 0;
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return -1;
    }
    while (fread(&rec, sizeof rec, 1, f) == 1) {
        char what[64];
        if (rec.magic != PROG_REC_MAGIC || rec.slots > 136)
            break;
        snprintf(what, sizeof what, "record %d (xform %u)", n, rec.xform);
        if (rec.xform == 2)
            check((const uint32_t (*)[4])rec.prog, rec.slots ? rec.slots : 136, rec.vp_start,
                  &rec.pk, what);
        else
            check(NULL, 0, 0, &rec.pk, what);
        n++;
    }
    fclose(f);
    return n;
}

/* Draw a pre-transformed triangle at surface pixels (0,0) (32,0) (0,32) --
 * clockwise on screen -- into a 64x64 target with back faces culled and
 * the given front face. With clockwise (what nv2a_metal.m sets for NV097
 * front face CW) it must cover the top-left corner and nothing at the
 * bottom right; with counter-clockwise it must be culled. */
static void orientation(MTLWinding front, int expect_drawn)
{
    Nv2aPshKey k;
    id<MTLFunction> vs, fs;
    id<MTLRenderPipelineState> p;
    id<MTLTexture> tex;
    id<MTLCommandQueue> q = [s_dev newCommandQueue];
    id<MTLCommandBuffer> cb;
    id<MTLRenderCommandEncoder> enc;
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    MTLTextureDescriptor *td;
    Nv2aMslVs u;
    Nv2aMslFs fu;
    float verts[16][3][4];
    uint8_t px[64 * 64 * 4];
    int a, v;

    memset(&k, 0, sizeof k);
    k.final0 = 0x04;                            /* D = V0: colour = diffuse */
    k.final1 = 0x14u << 8;                      /* G = V0.a */
    if (nv2a_msl_vertex(NULL, 0, 0, s_vsrc, sizeof s_vsrc) < 0
     || nv2a_msl_fragment(&k, s_fsrc, sizeof s_fsrc) < 0) {
        fprintf(stderr, "FAIL orientation: generation\n");
        s_fail++;
        return;
    }
    vs = function(s_vsrc, NV2A_MSL_VS_NAME, NO, "orientation vs");
    fs = function(s_fsrc, NV2A_MSL_FS_NAME, NO, "orientation fs");
    if (!vs || !fs || !(p = pipeline(vs, fs, "orientation")))
        return;
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                            width:64 height:64 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModeShared;
    tex = [s_dev newTextureWithDescriptor:td];
    memset(&u, 0, sizeof u);
    u.surf[0] = 2.0f / 64; u.surf[1] = 2.0f / 64; u.surf[2] = 1.0f; u.surf[3] = 0;
    u.aa[0] = u.aa[1] = 1;
    u.xform = 0;
    memset(&fu, 0, sizeof fu);
    fu.afunc = 0x207;
    memset(verts, 0, sizeof verts);
    for (v = 0; v < 3; v++) {
        verts[0][v][0] = v == 1 ? 32.0f : 0.0f;
        verts[0][v][1] = v == 2 ? 32.0f : 0.0f;
        verts[0][v][2] = 0.5f;
        verts[0][v][3] = 1.0f;                  /* rhw */
        verts[3][v][0] = 1.0f; verts[3][v][3] = 1.0f;   /* diffuse: red */
    }
    rp.colorAttachments[0].texture = tex;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    cb = [q commandBuffer];
    enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:p];
    [enc setCullMode:MTLCullModeBack];
    [enc setFrontFacingWinding:front];
    for (a = 0; a < 16; a++)
        [enc setVertexBytes:verts[a] length:sizeof verts[a] atIndex:(NSUInteger)a];
    [enc setVertexBytes:&u length:sizeof u atIndex:NV2A_MSL_VS_BLOCK];
    [enc setFragmentBytes:&fu length:sizeof fu atIndex:NV2A_MSL_FS_BLOCK];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    [tex getBytes:px bytesPerRow:64 * 4 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
    /* BGRA: red is byte 2. */
    if (!expect_drawn) {
        if (px[(4 * 64 + 4) * 4 + 2] != 0) {
            fprintf(stderr, "FAIL orientation: counter-clockwise front face did not cull\n");
            s_fail++;
        } else {
            printf("orientation: counter-clockwise front face culls it\n");
        }
    } else if (px[(4 * 64 + 4) * 4 + 2] < 200 || px[(60 * 64 + 60) * 4 + 2] != 0
     || px[(4 * 64 + 60) * 4 + 2] != 0) {
        fprintf(stderr, "FAIL orientation: top-left %u, bottom-right %u, top-right %u (red)\n",
                px[(4 * 64 + 4) * 4 + 2], px[(60 * 64 + 60) * 4 + 2], px[(4 * 64 + 60) * 4 + 2]);
        s_fail++;
    } else {
        printf("orientation: top-left drawn, clockwise front face kept\n");
    }
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        int n = 0;
        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev) {
            fprintf(stderr, "no Metal device\n");
            return 1;
        }
        {
            /* The renderer's own copy and clear shaders. */
            NSError *err = nil;
            id<MTLLibrary> lib = [s_dev newLibraryWithSource:[NSString stringWithUTF8String:nv2a_msl_fixed()]
                                                     options:nil error:&err];
            if (!lib || ![lib newFunctionWithName:@"copy_vs"] || ![lib newFunctionWithName:@"copy_fs"]
             || ![lib newFunctionWithName:@"clear_vs"] || ![lib newFunctionWithName:@"clear_fs"]) {
                fprintf(stderr, "FAIL fixed shaders: %s\n", err ? err.localizedDescription.UTF8String
                                                               : "a function is missing");
                s_fail++;
            } else {
                printf("fixed shaders: copy and clear compile\n");
            }
        }
        orientation(MTLWindingClockwise, 1);
        orientation(MTLWindingCounterClockwise, 0);
        fuzz(200);
        if (argc > 1 && (n = records(argv[1])) < 0)
            return 1;
        printf("%d program pairs checked (%d from %s), %d failures\n", s_done, n,
               argc > 1 ? argv[1] : "no record file", s_fail);
        return s_fail ? 1 : 0;
    }
}
