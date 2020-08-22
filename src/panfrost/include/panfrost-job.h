/*
 * © Copyright 2017-2018 Alyssa Rosenzweig
 * © Copyright 2017-2018 Connor Abbott
 * © Copyright 2017-2018 Lyude Paul
 * © Copyright2019 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#ifndef __PANFROST_JOB_H__
#define __PANFROST_JOB_H__

#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t mali_ptr;

/* Applies to tiler_gl_enables */

#define MALI_OCCLUSION_QUERY    (1 << 3)
#define MALI_OCCLUSION_PRECISE  (1 << 4)

/* Set for a glFrontFace(GL_CCW) in a Y=0=TOP coordinate system (like Gallium).
 * In OpenGL, this would corresponds to glFrontFace(GL_CW). Mesa and the blob
 * disagree about how to do viewport flipping, so the blob actually sets this
 * for GL_CW but then has a negative viewport stride */

#define MALI_FRONT_CCW_TOP      (1 << 5)

#define MALI_CULL_FACE_FRONT    (1 << 6)
#define MALI_CULL_FACE_BACK     (1 << 7)

enum mali_nondominant_mode {
        MALI_BLEND_NON_MIRROR = 0,
        MALI_BLEND_NON_ZERO = 1
};

enum mali_dominant_blend {
        MALI_BLEND_DOM_SOURCE = 0,
        MALI_BLEND_DOM_DESTINATION  = 1
};

enum mali_dominant_factor {
        MALI_DOMINANT_UNK0 = 0,
        MALI_DOMINANT_ZERO = 1,
        MALI_DOMINANT_SRC_COLOR = 2,
        MALI_DOMINANT_DST_COLOR = 3,
        MALI_DOMINANT_UNK4 = 4,
        MALI_DOMINANT_SRC_ALPHA = 5,
        MALI_DOMINANT_DST_ALPHA = 6,
        MALI_DOMINANT_CONSTANT = 7,
};

enum mali_blend_modifier {
        MALI_BLEND_MOD_UNK0 = 0,
        MALI_BLEND_MOD_NORMAL = 1,
        MALI_BLEND_MOD_SOURCE_ONE = 2,
        MALI_BLEND_MOD_DEST_ONE = 3,
};

struct mali_blend_mode {
        enum mali_blend_modifier clip_modifier : 2;
        unsigned unused_0 : 1;
        unsigned negate_source : 1;

        enum mali_dominant_blend dominant : 1;

        enum mali_nondominant_mode nondominant_mode : 1;

        unsigned unused_1 : 1;

        unsigned negate_dest : 1;

        enum mali_dominant_factor dominant_factor : 3;
        unsigned complement_dominant : 1;
} __attribute__((packed));

/* Compressed per-pixel formats. Each of these formats expands to one to four
 * floating-point or integer numbers, as defined by the OpenGL specification.
 * There are various places in OpenGL where the user can specify a compressed
 * format in memory, which all use the same 8-bit enum in the various
 * descriptors, although different hardware units support different formats.
 */

/* The top 3 bits specify how the bits of each component are interpreted. */

/* e.g. ETC2_RGB8 */
#define MALI_FORMAT_COMPRESSED (0 << 5)

/* e.g. R11F_G11F_B10F */
#define MALI_FORMAT_SPECIAL (2 << 5)

/* signed normalized, e.g. RGBA8_SNORM */
#define MALI_FORMAT_SNORM (3 << 5)

/* e.g. RGBA8UI */
#define MALI_FORMAT_UINT (4 << 5)

/* e.g. RGBA8 and RGBA32F */
#define MALI_FORMAT_UNORM (5 << 5)

/* e.g. RGBA8I and RGBA16F */
#define MALI_FORMAT_SINT (6 << 5)

/* These formats seem to largely duplicate the others. They're used at least
 * for Bifrost framebuffer output.
 */
#define MALI_FORMAT_SPECIAL2 (7 << 5)
#define MALI_EXTRACT_TYPE(fmt) ((fmt) & 0xe0)

/* If the high 3 bits are 3 to 6 these two bits say how many components
 * there are.
 */
#define MALI_NR_CHANNELS(n) ((n - 1) << 3)
#define MALI_EXTRACT_CHANNELS(fmt) ((((fmt) >> 3) & 3) + 1)

/* If the high 3 bits are 3 to 6, then the low 3 bits say how big each
 * component is, except the special MALI_CHANNEL_FLOAT which overrides what the
 * bits mean.
 */

#define MALI_CHANNEL_4 2

#define MALI_CHANNEL_8 3

#define MALI_CHANNEL_16 4

#define MALI_CHANNEL_32 5

/* For MALI_FORMAT_SINT it means a half-float (e.g. RG16F). For
 * MALI_FORMAT_UNORM, it means a 32-bit float.
 */
#define MALI_CHANNEL_FLOAT 7
#define MALI_EXTRACT_BITS(fmt) (fmt & 0x7)

/* The raw Midgard blend payload can either be an equation or a shader
 * address, depending on the context */

union midgard_blend {
        mali_ptr shader;

        struct {
                struct mali_blend_equation_packed equation;
                float constant;
        };
};

struct midgard_blend_rt {
        struct mali_blend_flags_packed flags;
        u32 zero;
        union midgard_blend blend;
} __attribute__((packed));

/* On Bifrost systems (all MRT), each render target gets one of these
 * descriptors */

enum bifrost_shader_type {
        BIFROST_BLEND_F16 = 0,
        BIFROST_BLEND_F32 = 1,
        BIFROST_BLEND_I32 = 2,
        BIFROST_BLEND_U32 = 3,
        BIFROST_BLEND_I16 = 4,
        BIFROST_BLEND_U16 = 5,
};

#define BIFROST_MAX_RENDER_TARGET_COUNT 8

struct bifrost_blend_rt {
        /* This is likely an analogue of the flags on
         * midgard_blend_rt */

        u16 flags; // = 0x200

        /* Single-channel blend constants are encoded in a sort of
         * fixed-point. Basically, the float is mapped to a byte, becoming
         * a high byte, and then the lower-byte is added for precision.
         * For the original float f:
         *
         * f = (constant_hi / 255) + (constant_lo / 65535)
         *
         * constant_hi = int(f / 255)
         * constant_lo = 65535*f - (65535/255) * constant_hi
         */
        u16 constant;

        struct mali_blend_equation_packed equation;

        /*
         * - 0x19 normally
         * - 0x3 when this slot is unused (everything else is 0 except the index)
         * - 0x11 when this is the fourth slot (and it's used)
         * - 0 when there is a blend shader
         */
        u16 unk2;

        /* increments from 0 to 3 */
        u16 index;

        union {
                struct {
                        /* So far, I've only seen:
                         * - R001 for 1-component formats
                         * - RG01 for 2-component formats
                         * - RGB1 for 3-component formats
                         * - RGBA for 4-component formats
                         */
                        u32 swizzle : 12;
                        enum mali_format format : 8;

                        /* Type of the shader output variable. Note, this can
                          * be different from the format.
                          * enum bifrost_shader_type
                         */
                        u32 zero1 : 4;
                        u32 shader_type : 3;
                        u32 zero2 : 5;
                };

                /* Only the low 32 bits of the blend shader are stored, the
                 * high 32 bits are implicitly the same as the original shader.
                 * According to the kernel driver, the program counter for
                 * shaders is actually only 24 bits, so shaders cannot cross
                 * the 2^24-byte boundary, and neither can the blend shader.
                 * The blob handles this by allocating a 2^24 byte pool for
                 * shaders, and making sure that any blend shaders are stored
                 * in the same pool as the original shader. The kernel will
                 * make sure this allocation is aligned to 2^24 bytes.
                 */
                u32 shader;
        };
} __attribute__((packed));

/* Possible values for job_descriptor_size */

#define MALI_JOB_32 0
#define MALI_JOB_64 1

struct mali_job_descriptor_header {
        u32 exception_status;
        u32 first_incomplete_task;
        u64 fault_pointer;
        u8 job_descriptor_size : 1;
        enum mali_job_type job_type : 7;
        u8 job_barrier : 1;
        u8 unknown_flags : 7;
        u16 job_index;
        u16 job_dependency_index_1;
        u16 job_dependency_index_2;
        u64 next_job;
} __attribute__((packed));

/* Details about write_value from panfrost igt tests which use it as a generic
 * dword write primitive */

#define MALI_WRITE_VALUE_ZERO 3

struct mali_payload_write_value {
        u64 address;
        u32 value_descriptor;
        u32 reserved;
        u64 immediate;
} __attribute__((packed));

/*
 * Mali Attributes
 *
 * This structure lets the attribute unit compute the address of an attribute
 * given the vertex and instance ID. Unfortunately, the way this works is
 * rather complicated when instancing is enabled.
 *
 * To explain this, first we need to explain how compute and vertex threads are
 * dispatched. This is a guess (although a pretty firm guess!) since the
 * details are mostly hidden from the driver, except for attribute instancing.
 * When a quad is dispatched, it receives a single, linear index. However, we
 * need to translate that index into a (vertex id, instance id) pair, or a
 * (local id x, local id y, local id z) triple for compute shaders (although
 * vertex shaders and compute shaders are handled almost identically).
 * Focusing on vertex shaders, one option would be to do:
 *
 * vertex_id = linear_id % num_vertices
 * instance_id = linear_id / num_vertices
 *
 * but this involves a costly division and modulus by an arbitrary number.
 * Instead, we could pad num_vertices. We dispatch padded_num_vertices *
 * num_instances threads instead of num_vertices * num_instances, which results
 * in some "extra" threads with vertex_id >= num_vertices, which we have to
 * discard.  The more we pad num_vertices, the more "wasted" threads we
 * dispatch, but the division is potentially easier.
 *
 * One straightforward choice is to pad num_vertices to the next power of two,
 * which means that the division and modulus are just simple bit shifts and
 * masking. But the actual algorithm is a bit more complicated. The thread
 * dispatcher has special support for dividing by 3, 5, 7, and 9, in addition
 * to dividing by a power of two. This is possibly using the technique
 * described in patent US20170010862A1. As a result, padded_num_vertices can be
 * 1, 3, 5, 7, or 9 times a power of two. This results in less wasted threads,
 * since we need less padding.
 *
 * padded_num_vertices is picked by the hardware. The driver just specifies the
 * actual number of vertices. At least for Mali G71, the first few cases are
 * given by:
 *
 * num_vertices	| padded_num_vertices
 * 3		| 4
 * 4-7		| 8
 * 8-11		| 12 (3 * 4)
 * 12-15	| 16
 * 16-19	| 20 (5 * 4)
 *
 * Note that padded_num_vertices is a multiple of four (presumably because
 * threads are dispatched in groups of 4). Also, padded_num_vertices is always
 * at least one more than num_vertices, which seems like a quirk of the
 * hardware. For larger num_vertices, the hardware uses the following
 * algorithm: using the binary representation of num_vertices, we look at the
 * most significant set bit as well as the following 3 bits. Let n be the
 * number of bits after those 4 bits. Then we set padded_num_vertices according
 * to the following table:
 *
 * high bits	| padded_num_vertices
 * 1000		| 9 * 2^n
 * 1001		| 5 * 2^(n+1)
 * 101x		| 3 * 2^(n+2)
 * 110x		| 7 * 2^(n+1)
 * 111x		| 2^(n+4)
 *
 * For example, if num_vertices = 70 is passed to glDraw(), its binary
 * representation is 1000110, so n = 3 and the high bits are 1000, and
 * therefore padded_num_vertices = 9 * 2^3 = 72.
 *
 * The attribute unit works in terms of the original linear_id. if
 * num_instances = 1, then they are the same, and everything is simple.
 * However, with instancing things get more complicated. There are four
 * possible modes, two of them we can group together:
 *
 * 1. Use the linear_id directly. Only used when there is no instancing.
 *
 * 2. Use the linear_id modulo a constant. This is used for per-vertex
 * attributes with instancing enabled by making the constant equal
 * padded_num_vertices. Because the modulus is always padded_num_vertices, this
 * mode only supports a modulus that is a power of 2 times 1, 3, 5, 7, or 9.
 * The shift field specifies the power of two, while the extra_flags field
 * specifies the odd number. If shift = n and extra_flags = m, then the modulus
 * is (2m + 1) * 2^n. As an example, if num_vertices = 70, then as computed
 * above, padded_num_vertices = 9 * 2^3, so we should set extra_flags = 4 and
 * shift = 3. Note that we must exactly follow the hardware algorithm used to
 * get padded_num_vertices in order to correctly implement per-vertex
 * attributes.
 *
 * 3. Divide the linear_id by a constant. In order to correctly implement
 * instance divisors, we have to divide linear_id by padded_num_vertices times
 * to user-specified divisor. So first we compute padded_num_vertices, again
 * following the exact same algorithm that the hardware uses, then multiply it
 * by the GL-level divisor to get the hardware-level divisor. This case is
 * further divided into two more cases. If the hardware-level divisor is a
 * power of two, then we just need to shift. The shift amount is specified by
 * the shift field, so that the hardware-level divisor is just 2^shift.
 *
 * If it isn't a power of two, then we have to divide by an arbitrary integer.
 * For that, we use the well-known technique of multiplying by an approximation
 * of the inverse. The driver must compute the magic multiplier and shift
 * amount, and then the hardware does the multiplication and shift. The
 * hardware and driver also use the "round-down" optimization as described in
 * http://ridiculousfish.com/files/faster_unsigned_division_by_constants.pdf.
 * The hardware further assumes the multiplier is between 2^31 and 2^32, so the
 * high bit is implicitly set to 1 even though it is set to 0 by the driver --
 * presumably this simplifies the hardware multiplier a little. The hardware
 * first multiplies linear_id by the multiplier and takes the high 32 bits,
 * then applies the round-down correction if extra_flags = 1, then finally
 * shifts right by the shift field.
 *
 * There are some differences between ridiculousfish's algorithm and the Mali
 * hardware algorithm, which means that the reference code from ridiculousfish
 * doesn't always produce the right constants. Mali does not use the pre-shift
 * optimization, since that would make a hardware implementation slower (it
 * would have to always do the pre-shift, multiply, and post-shift operations).
 * It also forces the multplier to be at least 2^31, which means that the
 * exponent is entirely fixed, so there is no trial-and-error. Altogether,
 * given the divisor d, the algorithm the driver must follow is:
 *
 * 1. Set shift = floor(log2(d)).
 * 2. Compute m = ceil(2^(shift + 32) / d) and e = 2^(shift + 32) % d.
 * 3. If e <= 2^shift, then we need to use the round-down algorithm. Set
 * magic_divisor = m - 1 and extra_flags = 1.
 * 4. Otherwise, set magic_divisor = m and extra_flags = 0.
 */

#define FBD_MASK (~0x3f)

/* MFBD, rather than SFBD */
#define MALI_MFBD (0x1)

/* ORed into an MFBD address to specify the fbx section is included */
#define MALI_MFBD_TAG_EXTRA (0x2)

/* On Bifrost, these fields are the same between the vertex and tiler payloads.
 * They also seem to be the same between Bifrost and Midgard. They're shared in
 * fused payloads.
 */

/* Applies to unknown_draw */

#define MALI_DRAW_INDEXED_UINT8  (0x10)
#define MALI_DRAW_INDEXED_UINT16 (0x20)
#define MALI_DRAW_INDEXED_UINT32 (0x30)
#define MALI_DRAW_INDEXED_SIZE   (0x30)
#define MALI_DRAW_INDEXED_SHIFT  (4)

#define MALI_DRAW_VARYING_SIZE   (0x100)

/* Set to use first vertex as the provoking vertex for flatshading. Clear to
 * use the last vertex. This is the default in DX and VK, but not in GL. */

#define MALI_DRAW_FLATSHADE_FIRST (0x800)

#define MALI_DRAW_PRIMITIVE_RESTART_FIXED_INDEX (0x10000)

struct mali_vertex_tiler_prefix {
        /* This is a dynamic bitfield containing the following things in this order:
         *
         * - gl_WorkGroupSize.x
         * - gl_WorkGroupSize.y
         * - gl_WorkGroupSize.z
         * - gl_NumWorkGroups.x
         * - gl_NumWorkGroups.y
         * - gl_NumWorkGroups.z
         *
         * The number of bits allocated for each number is based on the *_shift
         * fields below. For example, workgroups_y_shift gives the bit that
         * gl_NumWorkGroups.y starts at, and workgroups_z_shift gives the bit
         * that gl_NumWorkGroups.z starts at (and therefore one after the bit
         * that gl_NumWorkGroups.y ends at). The actual value for each gl_*
         * value is one more than the stored value, since if any of the values
         * are zero, then there would be no invocations (and hence no job). If
         * there were 0 bits allocated to a given field, then it must be zero,
         * and hence the real value is one.
         *
         * Vertex jobs reuse the same job dispatch mechanism as compute jobs,
         * effectively doing glDispatchCompute(1, vertex_count, instance_count)
         * where vertex count is the number of vertices.
         */
        u32 invocation_count;

        /* Bitfield for shifts:
         *
         * size_y_shift : 5
         * size_z_shift : 5
         * workgroups_x_shift : 6
         * workgroups_y_shift : 6
         * workgroups_z_shift : 6
         * workgroups_x_shift_2 : 4
         */
        u32 invocation_shifts;

        u32 draw_mode : 4;
        u32 unknown_draw : 22;

        /* This is the the same as workgroups_x_shift_2 in compute shaders, but
         * always 5 for vertex jobs and 6 for tiler jobs. I suspect this has
         * something to do with how many quads get put in the same execution
         * engine, which is a balance (you don't want to starve the engine, but
         * you also want to distribute work evenly).
         */
        u32 workgroups_x_shift_3 : 6;


        /* Negative of min_index. This is used to compute
         * the unbiased index in tiler/fragment shader runs.
         * 
         * The hardware adds offset_bias_correction in each run,
         * so that absent an index bias, the first vertex processed is
         * genuinely the first vertex (0). But with an index bias,
         * the first vertex process is numbered the same as the bias.
         *
         * To represent this more conviniently:
         * unbiased_index = lower_bound_index +
         *                  index_bias +
         *                  offset_bias_correction
         *
         * This is done since the hardware doesn't accept a index_bias
         * and this allows it to recover the unbiased index.
         */
        int32_t offset_bias_correction;
        u32 zero1;

        /* Like many other strictly nonzero quantities, index_count is
         * subtracted by one. For an indexed cube, this is equal to 35 = 6
         * faces * 2 triangles/per face * 3 vertices/per triangle - 1. That is,
         * for an indexed draw, index_count is the number of actual vertices
         * rendered whereas invocation_count is the number of unique vertices
         * rendered (the number of times the vertex shader must be invoked).
         * For non-indexed draws, this is just equal to invocation_count. */

        u32 index_count;

        /* No hidden structure; literally just a pointer to an array of uint
         * indices (width depends on flags). Thanks, guys, for not making my
         * life insane for once! NULL for non-indexed draws. */

        u64 indices;
} __attribute__((packed));

/* Point size / line width can either be specified as a 32-bit float (for
 * constant size) or as a [machine word size]-bit GPU pointer (for varying size). If a pointer
 * is selected, by setting the appropriate MALI_DRAW_VARYING_SIZE bit in the tiler
 * payload, the contents of varying_pointer will be intepreted as an array of
 * fp16 sizes, one for each vertex. gl_PointSize is therefore implemented by
 * creating a special MALI_R16F varying writing to varying_pointer. */

union midgard_primitive_size {
        float constant;
        u64 pointer;
};

struct bifrost_tiler_heap_meta {
        u32 zero;
        u32 heap_size;
        /* note: these are just guesses! */
        mali_ptr tiler_heap_start;
        mali_ptr tiler_heap_free;
        mali_ptr tiler_heap_end;

        /* hierarchy weights? but they're still 0 after the job has run... */
        u32 zeros[10];
        u32 unk1;
        u32 unk7e007e;
} __attribute__((packed));

struct bifrost_tiler_meta {
        u32 tiler_heap_next_start;  /* To be written by the GPU */
        u32 used_hierarchy_mask;  /* To be written by the GPU */
        u16 hierarchy_mask; /* Five values observed: 0xa, 0x14, 0x28, 0x50, 0xa0 */
        u16 flags;
        u16 width;
        u16 height;
        u64 zero0;
        mali_ptr tiler_heap_meta;
        /* TODO what is this used for? */
        u64 zeros[20];
} __attribute__((packed));

struct bifrost_tiler_only {
        /* 0x20 */
        union midgard_primitive_size primitive_size;

        mali_ptr tiler_meta;

        u64 zero1, zero2, zero3, zero4, zero5, zero6;
} __attribute__((packed));

struct mali_vertex_tiler_postfix {
        u16 gl_enables; // 0x6 on Midgard, 0x2 on Bifrost

        /* Both zero for non-instanced draws. For instanced draws, a
         * decomposition of padded_num_vertices. See the comments about the
         * corresponding fields in mali_attr for context. */

        unsigned instance_shift : 5;
        unsigned instance_odd : 3;

        u8 zero4;

        /* Offset for first vertex in buffer */
        u32 offset_start;

	u64 zero5;

        /* Zero for vertex jobs. Pointer to the position (gl_Position) varying
         * output from the vertex shader for tiler jobs.
         */

        u64 position_varying;

        /* An array of mali_uniform_buffer_meta's. The size is given by the
         * shader_meta.
         */
        u64 uniform_buffers;

        /* On Bifrost, this is a pointer to an array of bifrost_texture_descriptor.
         * On Midgard, this is a pointer to an array of pointers to the texture
         * descriptors, number of pointers bounded by number of textures. The
         * indirection is needed to accomodate varying numbers and sizes of
         * texture descriptors */
        u64 textures;

        /* For OpenGL, from what I've seen, this is intimately connected to
         * texture_meta. cwabbott says this is not the case under Vulkan, hence
         * why this field is seperate (Midgard is Vulkan capable). Pointer to
         * array of sampler descriptors (which are uniform in size) */
        u64 sampler_descriptor;

        u64 uniforms;
        u64 shader;
        u64 attributes; /* struct attribute_buffer[] */
        u64 attribute_meta; /* attribute_meta[] */
        u64 varyings; /* struct attr */
        u64 varying_meta; /* pointer */
        u64 viewport;
        u64 occlusion_counter; /* A single bit as far as I can tell */

        /* On Bifrost, this points directly to a mali_shared_memory structure.
         * On Midgard, this points to a framebuffer (either SFBD or MFBD as
         * tagged), which embeds a mali_shared_memory structure */
        mali_ptr shared_memory;
} __attribute__((packed));

struct midgard_payload_vertex_tiler {
        struct mali_vertex_tiler_prefix prefix;
        struct mali_vertex_tiler_postfix postfix;

        union midgard_primitive_size primitive_size;
} __attribute__((packed));

struct bifrost_payload_vertex {
        struct mali_vertex_tiler_prefix prefix;
        struct mali_vertex_tiler_postfix postfix;
} __attribute__((packed));

struct bifrost_payload_tiler {
        struct mali_vertex_tiler_prefix prefix;
        struct bifrost_tiler_only tiler;
        struct mali_vertex_tiler_postfix postfix;
} __attribute__((packed));

struct bifrost_payload_fused {
        struct mali_vertex_tiler_prefix prefix;
        struct bifrost_tiler_only tiler;
        struct mali_vertex_tiler_postfix tiler_postfix;
        u64 padding; /* zero */
        struct mali_vertex_tiler_postfix vertex_postfix;
} __attribute__((packed));

/* Purposeful off-by-one in width, height fields. For example, a (64, 64)
 * texture is stored as (63, 63) in these fields. This adjusts for that.
 * There's an identical pattern in the framebuffer descriptor. Even vertex
 * count fields work this way, hence the generic name -- integral fields that
 * are strictly positive generally need this adjustment. */

#define MALI_POSITIVE(dim) (dim - 1)

/* 8192x8192 */
#define MAX_MIP_LEVELS (13)

/* Cubemap bloats everything up */
#define MAX_CUBE_FACES (6)

/* For each pointer, there is an address and optionally also a stride */
#define MAX_ELEMENTS (2)

/* Used for lod encoding. Thanks @urjaman for pointing out these routines can
 * be cleaned up a lot. */

#define DECODE_FIXED_16(x) ((float) (x / 256.0))

static inline int16_t
FIXED_16(float x, bool allow_negative)
{
        /* Clamp inputs, accounting for float error */
        float max_lod = (32.0 - (1.0 / 512.0));
        float min_lod = allow_negative ? -max_lod : 0.0;

        x = ((x > max_lod) ? max_lod : ((x < min_lod) ? min_lod : x));

        return (int) (x * 256.0);
}

/* From presentations, 16x16 tiles externally. Use shift for fast computation
 * of tile numbers. */

#define MALI_TILE_SHIFT 4
#define MALI_TILE_LENGTH (1 << MALI_TILE_SHIFT)

/* Tile coordinates are stored as a compact u32, as only 12 bits are needed to
 * each component. Notice that this provides a theoretical upper bound of (1 <<
 * 12) = 4096 tiles in each direction, addressing a maximum framebuffer of size
 * 65536x65536. Multiplying that together, times another four given that Mali
 * framebuffers are 32-bit ARGB8888, means that this upper bound would take 16
 * gigabytes of RAM just to store the uncompressed framebuffer itself, let
 * alone rendering in real-time to such a buffer.
 *
 * Nice job, guys.*/

/* From mali_kbase_10969_workaround.c */
#define MALI_X_COORD_MASK 0x00000FFF
#define MALI_Y_COORD_MASK 0x0FFF0000

/* Extract parts of a tile coordinate */

#define MALI_TILE_COORD_X(coord) ((coord) & MALI_X_COORD_MASK)
#define MALI_TILE_COORD_Y(coord) (((coord) & MALI_Y_COORD_MASK) >> 16)

/* Helpers to generate tile coordinates based on the boundary coordinates in
 * screen space. So, with the bounds (0, 0) to (128, 128) for the screen, these
 * functions would convert it to the bounding tiles (0, 0) to (7, 7).
 * Intentional "off-by-one"; finding the tile number is a form of fencepost
 * problem. */

#define MALI_MAKE_TILE_COORDS(X, Y) ((X) | ((Y) << 16))
#define MALI_BOUND_TO_TILE(B, bias) ((B - bias) >> MALI_TILE_SHIFT)
#define MALI_COORDINATE_TO_TILE(W, H, bias) MALI_MAKE_TILE_COORDS(MALI_BOUND_TO_TILE(W, bias), MALI_BOUND_TO_TILE(H, bias))
#define MALI_COORDINATE_TO_TILE_MIN(W, H) MALI_COORDINATE_TO_TILE(W, H, 0)
#define MALI_COORDINATE_TO_TILE_MAX(W, H) MALI_COORDINATE_TO_TILE(W, H, 1)

struct mali_payload_fragment {
        u32 min_tile_coord;
        u32 max_tile_coord;
        mali_ptr framebuffer;
} __attribute__((packed));

/* Single Framebuffer Descriptor */

/* Flags apply to format. With just MSAA_A and MSAA_B, the framebuffer is
 * configured for 4x. With MSAA_8, it is configured for 8x. */

#define MALI_SFBD_FORMAT_MSAA_8 (1 << 3)
#define MALI_SFBD_FORMAT_MSAA_A (1 << 4)
#define MALI_SFBD_FORMAT_MSAA_B (1 << 4)
#define MALI_SFBD_FORMAT_SRGB 	(1 << 5)

/* Fast/slow based on whether all three buffers are cleared at once */

#define MALI_CLEAR_FAST         (1 << 18)
#define MALI_CLEAR_SLOW         (1 << 28)
#define MALI_CLEAR_SLOW_STENCIL (1 << 31)

/* Configures hierarchical tiling on Midgard for both SFBD/MFBD (embedded
 * within the larget framebuffer descriptor). Analogous to
 * bifrost_tiler_heap_meta and bifrost_tiler_meta*/

/* See pan_tiler.c for derivation */
#define MALI_HIERARCHY_MASK ((1 << 9) - 1)

/* Flag disabling the tiler for clear-only jobs, with
   hierarchical tiling */
#define MALI_TILER_DISABLED (1 << 12)

/* Flag selecting userspace-generated polygon list, for clear-only jobs without
 * hierarhical tiling. */
#define MALI_TILER_USER 0xFFF

/* Absent any geometry, the minimum size of the polygon list header */
#define MALI_TILER_MINIMUM_HEADER_SIZE 0x200

struct midgard_tiler_descriptor {
        /* Size of the entire polygon list; see pan_tiler.c for the
         * computation. It's based on hierarchical tiling */

        u32 polygon_list_size;

        /* Name known from the replay workaround in the kernel. What exactly is
         * flagged here is less known. We do that (tiler_hierarchy_mask & 0x1ff)
         * specifies a mask of hierarchy weights, which explains some of the
         * performance mysteries around setting it. We also see the bottom bit
         * of tiler_flags set in the kernel, but no comment why.
         *
         * hierarchy_mask can have the TILER_DISABLED flag */

        u16 hierarchy_mask;
        u16 flags;

        /* See mali_tiler.c for an explanation */
        mali_ptr polygon_list;
        mali_ptr polygon_list_body;

        /* Names based on we see symmetry with replay jobs which name these
         * explicitly */

        mali_ptr heap_start; /* tiler heap_free_address */
        mali_ptr heap_end;

        /* Hierarchy weights. We know these are weights based on the kernel,
         * but I've never seen them be anything other than zero */
        u32 weights[8];
};

struct mali_sfbd_format {
        /* 0x1 */
        unsigned unk1 : 6;

        /* mali_channel_swizzle */
        unsigned swizzle : 12;

        /* MALI_POSITIVE */
        unsigned nr_channels : 2;

        /* 0x4 */
        unsigned unk2 : 6;

        enum mali_block_format block : 2;

        /* 0xb */
        unsigned unk3 : 4;
};

/* Shared structure at the start of framebuffer descriptors, or used bare for
 * compute jobs, configuring stack and shared memory */

struct mali_shared_memory {
        u32 stack_shift : 4;
        u32 unk0 : 28;

        /* Configuration for shared memory for compute shaders.
         * shared_workgroup_count is logarithmic and may be computed for a
         * compute shader using shared memory as:
         *
         *  shared_workgroup_count = MAX2(ceil(log2(count_x)) + ... + ceil(log2(count_z), 10)
         *
         * For compute shaders that don't use shared memory, or non-compute
         * shaders, this is set to ~0
         */

        u32 shared_workgroup_count : 5;
        u32 shared_unk1 : 3;
        u32 shared_shift : 4;
        u32 shared_zero : 20;

        mali_ptr scratchpad;

        /* For compute shaders, the RAM backing of workgroup-shared memory. For
         * fragment shaders on Bifrost, apparently multisampling locations */

        mali_ptr shared_memory;
        mali_ptr unknown1;
} __attribute__((packed));

/* Configures multisampling on Bifrost fragment jobs */

struct bifrost_multisampling {
        u64 zero1;
        u64 zero2;
        mali_ptr sample_locations;
        u64 zero4;
} __attribute__((packed));

struct mali_single_framebuffer {
        struct mali_shared_memory shared_memory;
        struct mali_sfbd_format format;

        u32 clear_flags;
        u32 zero2;

        /* Purposeful off-by-one in these fields should be accounted for by the
         * MALI_DIMENSION macro */

        u16 width;
        u16 height;

        u32 zero3[4];
        mali_ptr checksum;
        u32 checksum_stride;
        u32 zero5;

        /* By default, the framebuffer is upside down from OpenGL's
         * perspective. Set framebuffer to the end and negate the stride to
         * flip in the Y direction */

        mali_ptr framebuffer;
        int32_t stride;

        u32 zero4;

        /* Depth and stencil buffers are interleaved, it appears, as they are
         * set to the same address in captures. Both fields set to zero if the
         * buffer is not being cleared. Depending on GL_ENABLE magic, you might
         * get a zero enable despite the buffer being present; that still is
         * disabled. */

        mali_ptr depth_buffer; // not SAME_VA
        u32 depth_stride_zero : 4;
        u32 depth_stride : 28;
        u32 zero7;

        mali_ptr stencil_buffer; // not SAME_VA
        u32 stencil_stride_zero : 4;
        u32 stencil_stride : 28;
        u32 zero8;

        u32 clear_color_1; // RGBA8888 from glClear, actually used by hardware
        u32 clear_color_2; // always equal, but unclear function?
        u32 clear_color_3; // always equal, but unclear function?
        u32 clear_color_4; // always equal, but unclear function?

        /* Set to zero if not cleared */

        float clear_depth_1; // float32, ditto
        float clear_depth_2; // float32, ditto
        float clear_depth_3; // float32, ditto
        float clear_depth_4; // float32, ditto

        u32 clear_stencil; // Exactly as it appears in OpenGL

        u32 zero6[7];

        struct midgard_tiler_descriptor tiler;

        /* More below this, maybe */
} __attribute__((packed));


#define MALI_MFBD_FORMAT_SRGB 	  (1 << 0)

struct mali_rt_format {
        unsigned unk1 : 32;
        unsigned unk2 : 3;

        unsigned nr_channels : 2; /* MALI_POSITIVE */

        unsigned unk3 : 4;
        unsigned unk4 : 1;
        enum mali_block_format block : 2;
        enum mali_msaa msaa : 2;
        unsigned flags : 2;

        unsigned swizzle : 12;

        unsigned zero : 3;

        /* Disables MFBD preload. When this bit is set, the render target will
         * be cleared every frame. When this bit is clear, the hardware will
         * automatically wallpaper the render target back from main memory.
         * Unfortunately, MFBD preload is very broken on Midgard, so in
         * practice, this is a chicken bit that should always be set.
         * Discovered by accident, as all good chicken bits are. */

        unsigned no_preload : 1;
} __attribute__((packed));

/* Flags for afbc.flags and ds_afbc.flags */

#define MALI_AFBC_FLAGS 0x10009

/* Lossless RGB and RGBA colorspace transform */
#define MALI_AFBC_YTR (1 << 17)

struct mali_render_target {
        struct mali_rt_format format;

        u64 zero1;

        struct {
                /* Stuff related to ARM Framebuffer Compression. When AFBC is enabled,
                 * there is an extra metadata buffer that contains 16 bytes per tile.
                 * The framebuffer needs to be the same size as before, since we don't
                 * know ahead of time how much space it will take up. The
                 * framebuffer_stride is set to 0, since the data isn't stored linearly
                 * anymore.
                 *
                 * When AFBC is disabled, these fields are zero.
                 */

                mali_ptr metadata;
                u32 stride; // stride in units of tiles
                u32 flags; // = 0x20000
        } afbc;

        mali_ptr framebuffer;

        u32 zero2 : 4;
        u32 framebuffer_stride : 28; // in units of bytes, row to next
        u32 layer_stride; /* For multisample rendering */

        u32 clear_color_1; // RGBA8888 from glClear, actually used by hardware
        u32 clear_color_2; // always equal, but unclear function?
        u32 clear_color_3; // always equal, but unclear function?
        u32 clear_color_4; // always equal, but unclear function?
} __attribute__((packed));

/* An optional part of mali_framebuffer. It comes between the main structure
 * and the array of render targets. It must be included if any of these are
 * enabled:
 *
 * - Transaction Elimination
 * - Depth/stencil
 * - TODO: Anything else?
 */

/* flags_hi */
#define MALI_EXTRA_PRESENT      (0x1)

/* flags_lo */
#define MALI_EXTRA_ZS           (0x4)

struct mali_framebuffer_extra  {
        mali_ptr checksum;
        /* Each tile has an 8 byte checksum, so the stride is "width in tiles * 8" */
        u32 checksum_stride;

        unsigned flags_lo : 4;
        enum mali_block_format zs_block : 2;

        /* Number of samples in Z/S attachment, MALI_POSITIVE. So zero for
         * 1-sample (non-MSAA), 0x3 for MSAA 4x, etc */
        unsigned zs_samples : 4;
        unsigned flags_hi : 22;

        union {
                /* Note: AFBC is only allowed for 24/8 combined depth/stencil. */
                struct {
                        mali_ptr depth_stencil_afbc_metadata;
                        u32 depth_stencil_afbc_stride; // in units of tiles
                        u32 flags;

                        mali_ptr depth_stencil;

                        u64 padding;
                } ds_afbc;

                struct {
                        /* Depth becomes depth/stencil in case of combined D/S */
                        mali_ptr depth;
                        u32 depth_stride_zero : 4;
                        u32 depth_stride : 28;
                        u32 depth_layer_stride;

                        mali_ptr stencil;
                        u32 stencil_stride_zero : 4;
                        u32 stencil_stride : 28;
                        u32 stencil_layer_stride;
                } ds_linear;
        };


        u32 clear_color_1;
        u32 clear_color_2;
        u64 zero3;
} __attribute__((packed));

/* Flags for mfbd_flags */

/* Enables writing depth results back to main memory (rather than keeping them
 * on-chip in the tile buffer and then discarding) */

#define MALI_MFBD_DEPTH_WRITE (1 << 10)

/* The MFBD contains the extra mali_framebuffer_extra  section */

#define MALI_MFBD_EXTRA (1 << 13)

struct mali_framebuffer {
        union {
                struct mali_shared_memory shared_memory;
                struct bifrost_multisampling msaa;
        };

        /* 0x20 */
        u16 width1, height1;
        u32 zero3;
        u16 width2, height2;
        u32 unk1 : 19; // = 0x01000
        u32 rt_count_1 : 3; // off-by-one (use MALI_POSITIVE)
        u32 unk2 : 2; // = 0
        u32 rt_count_2 : 3; // no off-by-one
        u32 zero4 : 5;
        /* 0x30 */
        u32 clear_stencil : 8;
        u32 mfbd_flags : 24; // = 0x100
        float clear_depth;

        union {
                struct midgard_tiler_descriptor tiler;
                struct {
                        mali_ptr tiler_meta;
                        u32 zeros[16];
                };
        };

        /* optional: struct mali_framebuffer_extra  extra */
        /* struct mali_render_target rts[] */
} __attribute__((packed));

#endif /* __PANFROST_JOB_H__ */
