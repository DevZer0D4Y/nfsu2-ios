/*
 * nv2a_metal_msl.c -- NV2A shaders as Metal Shading Language.
 *
 * The translation itself is not repeated here. gl_vsh.c (vertex microcode)
 * and gl_psh.c (register combiners) emit their Vulkan dialect, and the parts
 * of it that do the work -- the program body and main()'s statements -- are
 * GLSL that MSL also accepts once a few names exist: vec2..vec4 are typedefs,
 * texture() and discard are macros, lessThan/greaterThanEqual are functions.
 * What differs is around them, written here:
 *
 *   vertex     The body is `void nv2a_program(void)` working on globals
 *              (registers, outputs, inputs, c[]). MSL has no mutable
 *              globals, so the body becomes a member function of a struct
 *              holding them, with the ILU helpers beside it. main() and the
 *              clip-space mapping are this file's: the same as gl_vsh.c's
 *              Vulkan ones except that Metal's NDC y points up.
 *   fragment   gl_psh.c's whole shader; its GLSL header (everything up to
 *              "void main() {") is replaced by an MSL function header with
 *              the same names, and `return fragColor;` closes the body.
 *
 * The bindings and uniform layouts are in nv2a_metal_msl.h.
 */
#include "nv2a_metal_msl.h"
#include "../nv2a_gl/gl_vsh.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char  *p;
    size_t left;
    int    overflow;
} Out;

static void emit(Out *o, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (o->overflow)
        return;
    va_start(ap, fmt);
    n = vsnprintf(o->p, o->left, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->left) {
        o->overflow = 1;
        return;
    }
    o->p += n;
    o->left -= (size_t)n;
}

/* GLSL names the generated bodies use. Scalar-vector mixes such as
 * max(vec3, 0.0) are fine in MSL as in GLSL. */
static const char s_common[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "#pragma clang diagnostic ignored \"-Wunused-variable\"\n"
    "typedef float4 vec4;\n"
    "typedef float3 vec3;\n"
    "typedef float2 vec2;\n"
    "static inline bool4 lessThan(float4 a, float4 b) { return a < b; }\n"
    "static inline bool4 greaterThanEqual(float4 a, float4 b) { return a >= b; }\n";

static const char s_vs_head[] =
    "constant bool nv2a_points [[function_constant(0)]];\n"
    "struct VsU {\n"
    "    float4 u_surf;\n"
    "    float4 u_m[4];\n"
    "    float4 u_vpoff;\n"
    "    float2 u_aa;\n"
    "    int u_xform;\n"
    "};\n"
    "struct VIn {\n"
    "    float4 v0 [[attribute(0)]];   float4 v1 [[attribute(1)]];\n"
    "    float4 v2 [[attribute(2)]];   float4 v3 [[attribute(3)]];\n"
    "    float4 v4 [[attribute(4)]];   float4 v5 [[attribute(5)]];\n"
    "    float4 v6 [[attribute(6)]];   float4 v7 [[attribute(7)]];\n"
    "    float4 v8 [[attribute(8)]];   float4 v9 [[attribute(9)]];\n"
    "    float4 v10 [[attribute(10)]]; float4 v11 [[attribute(11)]];\n"
    "    float4 v12 [[attribute(12)]]; float4 v13 [[attribute(13)]];\n"
    "    float4 v14 [[attribute(14)]]; float4 v15 [[attribute(15)]];\n"
    "};\n"
    "struct VOut {\n"
    "    float4 pos [[position]];\n"
    "    float4 vD0; float4 vD1;\n"
    "    float4 vT0; float4 vT1; float4 vT2; float4 vT3;\n"
    "    float vFog;\n"
    "    float psize [[point_size, function_constant(nv2a_points)]];\n"
    "};\n"
    /* gl_vsh.c's NV2A_SNAP_GLSL and Vulkan nv2a_clip, with y negated: real
     * pixel rows run top-first, and Metal's NDC y = +1 is the top row. */
    "static float2 nv2a_snap(constant VsU& U, float2 sw, float w) {\n"
    "    return w > 0.0 ? (trunc(sw / w * 16.0) / 16.0 - U.u_surf.w) * w : sw;\n"
    "}\n"
    "static float4 nv2a_clip(constant VsU& U, float3 sw, float w) {\n"
    "    float z = sw.z * U.u_surf.z;\n"
    "    sw.xy = nv2a_snap(U, sw.xy, w);\n"
    "    return float4(sw.x * U.u_surf.x - w, w - sw.y * U.u_surf.y,\n"
    "                  w > 0.0 ? clamp(z, 0.0, w) : z, w);\n"
    "}\n";

/* The register file and ILU helpers of gl_vsh.c's prelude, as members. */
static const char s_vs_struct[] =
    "struct Nv2aVs {\n"
    "    float4 v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15;\n"
    "    constant float4 *c;\n"
    "    int nv2a_ib;\n"
    "    float4 R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11;\n"
    "    float4 oPos, o1, o2, oD0, oD1, oFog, oPts, oB0, oB1, oT0, oT1, oT2, oT3;\n"
    "    float nv2a_rcp(float x) { return x != 0.0 ? 1.0 / x : 1.884467e+19; }\n"
    "    float nv2a_rcc(float x) {\n"
    "        float y = nv2a_rcp(x); float a = clamp(abs(y), 5.42101e-20, 1.884467e+19);\n"
    "        return y < 0.0 ? -a : a;\n"
    "    }\n"
    "    float4 nv2a_exp(float x) { float f = floor(x); return float4(exp2(f), x - f, exp2(x), 1.0); }\n"
    "    float4 nv2a_log(float x) {\n"
    "        float a = abs(x);\n"
    "        if (a == 0.0) return float4(-1.884467e+19, 1.0, -1.884467e+19, 1.0);\n"
    "        float e = floor(log2(a));\n"
    "        return float4(e, a / exp2(e), log2(a), 1.0);\n"
    "    }\n"
    "    float4 nv2a_lit(float4 s) {\n"
    "        float lx = max(s.x, 0.0), ly = max(s.y, 0.0), w = clamp(s.w, -127.996, 127.996);\n"
    "        return float4(1.0, lx, (lx > 0.0 && ly > 0.0) ? exp2(w * log2(ly)) : 0.0, 1.0);\n"
    "    }\n";

static const char s_vs_main_program[] =
    "};\n"
    "vertex VOut nv2a_vs(VIn in [[stage_in]],\n"
    "                    constant VsU& U [[buffer(16)]],\n"
    "                    constant float4 *c [[buffer(17)]],\n"
    "                    uint iid [[instance_id]]) {\n"
    "    Nv2aVs s;\n"
    "    s.v0 = in.v0; s.v1 = in.v1; s.v2 = in.v2; s.v3 = in.v3;\n"
    "    s.v4 = in.v4; s.v5 = in.v5; s.v6 = in.v6; s.v7 = in.v7;\n"
    "    s.v8 = in.v8; s.v9 = in.v9; s.v10 = in.v10; s.v11 = in.v11;\n"
    "    s.v12 = in.v12; s.v13 = in.v13; s.v14 = in.v14; s.v15 = in.v15;\n"
    "    s.c = c;\n"
    "    s.nv2a_ib = int(iid) * 192;\n"
    "    s.R0 = s.R1 = s.R2 = s.R3 = s.R4 = s.R5 = s.R6 = s.R7 = s.R8 = s.R9 = s.R10 = s.R11 = float4(0.0);\n"
    "    s.oPos = s.o1 = s.o2 = s.oFog = s.oPts = float4(0.0);\n"
    "    s.oD0 = s.oD1 = s.oB0 = s.oB1 = float4(0.0, 0.0, 0.0, 1.0);\n"
    "    s.oT0 = s.oT1 = s.oT2 = s.oT3 = float4(0.0, 0.0, 0.0, 1.0);\n"
    "    s.nv2a_program();\n"
    "    VOut o;\n"
    "    float w = abs(s.oPos.w) < 1e-6 ? 1e-6 : s.oPos.w;\n"
    "    float3 sc = float3(s.oPos.xy * U.u_aa, s.oPos.z);\n"
    "    o.pos = nv2a_clip(U, sc * w, w);\n"
    "    o.vD0 = clamp(s.oD0, 0.0, 1.0); o.vD1 = clamp(s.oD1, 0.0, 1.0);\n"
    "    o.vT0 = s.oT0; o.vT1 = s.oT1; o.vT2 = s.oT2; o.vT3 = s.oT3; o.vFog = s.oFog.x;\n"
    "    if (nv2a_points) o.psize = 1.0;\n"
    "    return o;\n"
    "}\n";

/* gl_vsh.c's nv2a_gl_vsh_fixed(), Vulkan dialect. */
static const char s_vs_main_fixed[] =
    "vertex VOut nv2a_vs(VIn in [[stage_in]],\n"
    "                    constant VsU& U [[buffer(16)]]) {\n"
    "    VOut o;\n"
    "    if (U.u_xform == 1) {\n"
    "        float4 clip = float4(dot(U.u_m[0], in.v0), dot(U.u_m[1], in.v0),\n"
    "                             dot(U.u_m[2], in.v0), dot(U.u_m[3], in.v0));\n"
    "        float w = abs(clip.w) < 1e-6 ? 1e-6 : clip.w;\n"
    "        float3 sw = clip.xyz + U.u_vpoff.xyz * w;\n"
    "        sw.xy *= U.u_aa;\n"
    "        o.pos = nv2a_clip(U, sw, w);\n"
    "    } else {\n"
    "        float w = (in.v0.w > 0.0) ? 1.0 / in.v0.w : 1.0;\n"
    "        o.pos = nv2a_clip(U, in.v0.xyz * w, w);\n"
    "    }\n"
    "    o.vD0 = clamp(in.v3, 0.0, 1.0); o.vD1 = clamp(in.v4, 0.0, 1.0);\n"
    "    o.vT0 = in.v9; o.vT1 = in.v10; o.vT2 = in.v11; o.vT3 = in.v12; o.vFog = 0.0;\n"
    "    if (nv2a_points) o.psize = 1.0;\n"
    "    return o;\n"
    "}\n";

int nv2a_msl_vertex(const uint32_t (*prog)[4], uint32_t slots, uint32_t start,
                    char *buf, size_t cap)
{
    Out o = { buf, cap, 0 };

    nv2a_shader_vk = 1;                 /* the bodies index c[] per instance */
    emit(&o, "%s%s", s_common, s_vs_head);
    if (!prog) {
        emit(&o, "%s", s_vs_main_fixed);
    } else {
        int n;
        emit(&o, "%s", s_vs_struct);
        if (o.overflow)
            return -1;
        n = nv2a_gl_vsh_program(prog, slots, start, o.p, o.left);
        if (n < 0)
            return -1;
        o.p += n;
        o.left -= (size_t)n;
        emit(&o, "%s", s_vs_main_program);
    }
    return o.overflow ? -1 : (int)(cap - o.left);
}

int nv2a_msl_stage_is_cube(const Nv2aPshKey *k, int i)
{
    return ((k->shader_program >> (5 * i)) & 0x1F) == 3;
}

int nv2a_msl_fragment(const Nv2aPshKey *k, char *buf, size_t cap)
{
    static const char marker[] = "void main() {\n";
    static char glsl[64 * 1024];
    Out o = { buf, cap, 0 };
    const char *body;
    size_t len;
    int n, i;

    nv2a_shader_vk = 1;                 /* samplerCube where a stage is a cube map */
    n = nv2a_gl_psh(k, glsl, sizeof glsl);
    if (n < 0)
        return -1;
    body = strstr(glsl, marker);
    len = (size_t)n;
    /* The body runs to main()'s closing brace, the last line. */
    if (!body || len < 2 || strcmp(glsl + len - 2, "}\n") != 0)
        return -1;
    body += sizeof marker - 1;
    len = (size_t)(glsl + len - 2 - body);

    emit(&o, "%s", s_common);
    emit(&o,
        "struct FsU {\n"
        "    float2 u_tscale[4];\n"
        "    float4 u_c0[8]; float4 u_c1[8];\n"
        "    float4 u_fc0; float4 u_fc1;\n"
        "    float4 u_fogcolor;\n"
        "    int u_alpha_func; float u_alpha_ref;\n"
        "};\n"
        "struct FIn {\n"
        "    float4 pos [[position]];\n"
        "    float4 vD0; float4 vD1;\n"
        "    float4 vT0; float4 vT1; float4 vT2; float4 vT3;\n"
        "    float vFog;\n"
        "};\n"
        "fragment float4 nv2a_fs(FIn in [[stage_in]],\n"
        "                        constant FsU& F [[buffer(0)]]");
    for (i = 0; i < 4; i++)
        emit(&o, ",\n                        %s<float> t%d_tex [[texture(%d)]],"
                 " sampler t%d_smp [[sampler(%d)]]",
             nv2a_msl_stage_is_cube(k, i) ? "texturecube" : "texture2d", i, i, i, i);
    emit(&o,
        ") {\n"
        "    constant float2 (&u_tscale)[4] = F.u_tscale;\n"
        "    constant float4 (&u_c0)[8] = F.u_c0;\n"
        "    constant float4 (&u_c1)[8] = F.u_c1;\n"
        "    float4 u_fc0 = F.u_fc0, u_fc1 = F.u_fc1, u_fogcolor = F.u_fogcolor;\n"
        "    int u_alpha_func = F.u_alpha_func;\n"
        "    float u_alpha_ref = F.u_alpha_ref;\n"
        "    float4 vD0 = in.vD0, vD1 = in.vD1;\n"
        "    float4 vT0 = in.vT0, vT1 = in.vT1, vT2 = in.vT2, vT3 = in.vT3;\n"
        "    float vFog = in.vFog;\n"
        "    float4 fragColor;\n"
        /* After the parameters: [[texture(n)]] would expand too. */
        "#define texture(t, uv) t##_tex.sample(t##_smp, uv)\n"
        "#define discard discard_fragment()\n");
    if (o.overflow || o.left <= len)
        return -1;
    memcpy(o.p, body, len);
    o.p += len;
    o.left -= len;
    emit(&o, "    return fragColor;\n}\n");
    return o.overflow ? -1 : (int)(cap - o.left);
}
