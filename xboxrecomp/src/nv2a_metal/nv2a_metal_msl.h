/* nv2a_metal_msl.h -- NV2A vertex programs and register combiners as Metal
 * Shading Language (see nv2a_metal_msl.c). */
#ifndef NV2A_METAL_MSL_H
#define NV2A_METAL_MSL_H

#include <stddef.h>
#include <stdint.h>

#include "../nv2a_gl/gl_psh.h"

/* Bindings shared by the generated shaders and nv2a_metal.m.
 *   vertex:   attribute a in buffer a (0..15), VsBlock in 16, the transform
 *             constants (192 float4 per instance) in 17
 *   fragment: FsBlock in buffer 0, stage i's texture and sampler at i
 * Function constant 0 (bool): the pipeline draws points (writes point_size). */
#define NV2A_MSL_VS_BLOCK   16
#define NV2A_MSL_VS_CONSTS  17
#define NV2A_MSL_FS_BLOCK   0
#define NV2A_MSL_FC_POINTS  0
#define NV2A_MSL_VS_NAME    "nv2a_vs"
#define NV2A_MSL_FS_NAME    "nv2a_fs"

/* The uniform blocks, laid out as the MSL structs VsU and FsU. */
typedef struct {
    float   surf[4];            /* 2/W, 2/H, 1/zmax, snap offset (gl_vsh.c) */
    float   m[16];              /* fixed-function composite rows */
    float   vpoff[4];           /* fixed-function viewport offset */
    float   aa[2];              /* logical -> real pixels */
    int32_t xform;              /* 1 fixed-function, 0 pre-transformed */
    int32_t pad;
} Nv2aMslVs;

typedef struct {
    float   tscale[4][2];       /* texcoord scale per stage */
    float   c0[8][4], c1[8][4]; /* combiner factors per stage */
    float   fc0[4], fc1[4];     /* final combiner factors */
    float   fog[4];
    int32_t afunc;              /* alpha test, GL compare enum (0x207 always) */
    float   aref;               /* 0..255 */
    float   pad[2];
} Nv2aMslFs;

/* Vertex shader for a vertex program (prog != NULL: slots from `start` to
 * FINAL) or for fixed-function / pre-transformed batches (prog == NULL).
 * Returns bytes written, -1 on overflow or a generator error. */
int nv2a_msl_vertex(const uint32_t (*prog)[4], uint32_t slots, uint32_t start,
                    char *buf, size_t cap);

/* Fragment shader for the combiner state k. Returns bytes written, -1 on
 * overflow or a generator error. */
int nv2a_msl_fragment(const Nv2aPshKey *k, char *buf, size_t cap);

/* Nonzero: texture stage i of k samples a cube map (texturecube). */
int nv2a_msl_stage_is_cube(const Nv2aPshKey *k, int i);

#endif
