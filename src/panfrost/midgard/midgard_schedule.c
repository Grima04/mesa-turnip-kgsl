/*
 * Copyright (C) 2018-2019 Alyssa Rosenzweig <alyssa@rosenzweig.io>
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
 */

#include "compiler.h"
#include "midgard_ops.h"
#include "util/u_memory.h"
#include "util/register_allocate.h"

/* Scheduling for Midgard is complicated, to say the least. ALU instructions
 * must be grouped into VLIW bundles according to following model:
 *
 * [VMUL] [SADD]
 * [VADD] [SMUL] [VLUT]
 *
 * A given instruction can execute on some subset of the units (or a few can
 * execute on all). Instructions can be either vector or scalar; only scalar
 * instructions can execute on SADD/SMUL units. Units on a given line execute
 * in parallel. Subsequent lines execute separately and can pass results
 * directly via pipeline registers r24/r25, bypassing the register file.
 *
 * A bundle can optionally have 128-bits of embedded constants, shared across
 * all of the instructions within a bundle.
 *
 * Instructions consuming conditionals (branches and conditional selects)
 * require their condition to be written into the conditional register (r31)
 * within the same bundle they are consumed.
 *
 * Fragment writeout requires its argument to be written in full within the
 * same bundle as the branch, with no hanging dependencies.
 *
 * Load/store instructions are also in bundles of simply two instructions, and
 * texture instructions have no bundling.
 *
 * -------------------------------------------------------------------------
 *
 */

/* We create the dependency graph with per-component granularity */

#define COMPONENT_COUNT 8

static void
add_dependency(struct util_dynarray *table, unsigned index, unsigned mask, midgard_instruction **instructions, unsigned child)
{
        for (unsigned i = 0; i < COMPONENT_COUNT; ++i) {
                if (!(mask & (1 << i)))
                        continue;

                struct util_dynarray *parents = &table[(COMPONENT_COUNT * index) + i];

                util_dynarray_foreach(parents, unsigned, parent) {
                        BITSET_WORD *dependents = instructions[*parent]->dependents;

                        /* Already have the dependency */
                        if (BITSET_TEST(dependents, child))
                                continue;

                        BITSET_SET(dependents, child);
                        instructions[child]->nr_dependencies++;
                }
        }
}

static void
mark_access(struct util_dynarray *table, unsigned index, unsigned mask, unsigned parent)
{
        for (unsigned i = 0; i < COMPONENT_COUNT; ++i) {
                if (!(mask & (1 << i)))
                        continue;

                util_dynarray_append(&table[(COMPONENT_COUNT * index) + i], unsigned, parent);
        }
}

static void
mir_create_dependency_graph(midgard_instruction **instructions, unsigned count, unsigned node_count)
{
        size_t sz = node_count * COMPONENT_COUNT;

        struct util_dynarray *last_read = calloc(sizeof(struct util_dynarray), sz);
        struct util_dynarray *last_write = calloc(sizeof(struct util_dynarray), sz);

        for (unsigned i = 0; i < sz; ++i) {
                util_dynarray_init(&last_read[i], NULL);
                util_dynarray_init(&last_write[i], NULL);
        }

        /* Initialize dependency graph */
        for (unsigned i = 0; i < count; ++i) {
                instructions[i]->dependents =
                        calloc(BITSET_WORDS(count), sizeof(BITSET_WORD));

                instructions[i]->nr_dependencies = 0;
        }

        /* Populate dependency graph */
        for (signed i = count - 1; i >= 0; --i) {
                if (instructions[i]->compact_branch)
                        continue;

                unsigned dest = instructions[i]->dest;
                unsigned mask = instructions[i]->mask;

                mir_foreach_src((*instructions), s) {
                        unsigned src = instructions[i]->src[s];

                        if (src < node_count) {
                                unsigned readmask = mir_mask_of_read_components(instructions[i], src);
                                add_dependency(last_write, src, readmask, instructions, i);
                        }
                }

                if (dest < node_count) {
                        add_dependency(last_read, dest, mask, instructions, i);
                        add_dependency(last_write, dest, mask, instructions, i);
                        mark_access(last_write, dest, mask, i);
                }

                mir_foreach_src((*instructions), s) {
                        unsigned src = instructions[i]->src[s];

                        if (src < node_count) {
                                unsigned readmask = mir_mask_of_read_components(instructions[i], src);
                                mark_access(last_read, src, readmask, i);
                        }
                }
        }

        /* If there is a branch, all instructions depend on it, as interblock
         * execution must be purely in-order */

        if (instructions[count - 1]->compact_branch) {
                BITSET_WORD *dependents = instructions[count - 1]->dependents;

                for (signed i = count - 2; i >= 0; --i) {
                        if (BITSET_TEST(dependents, i))
                                continue;

                        BITSET_SET(dependents, i);
                        instructions[i]->nr_dependencies++;
                }
        }

        /* Free the intermediate structures */
        for (unsigned i = 0; i < sz; ++i) {
                util_dynarray_fini(&last_read[i]);
                util_dynarray_fini(&last_write[i]);
        }
}

/* Create a mask of accessed components from a swizzle to figure out vector
 * dependencies */

static unsigned
swizzle_to_access_mask(unsigned swizzle)
{
        unsigned component_mask = 0;

        for (int i = 0; i < 4; ++i) {
                unsigned c = (swizzle >> (2 * i)) & 3;
                component_mask |= (1 << c);
        }

        return component_mask;
}

/* Does the mask cover more than a scalar? */

static bool
is_single_component_mask(unsigned mask)
{
        int components = 0;

        for (int c = 0; c < 8; ++c) {
                if (mask & (1 << c))
                        components++;
        }

        return components == 1;
}

/* Checks for an SSA data hazard between two adjacent instructions, keeping in
 * mind that we are a vector architecture and we can write to different
 * components simultaneously */

static bool
can_run_concurrent_ssa(midgard_instruction *first, midgard_instruction *second)
{
        /* Writeout has its own rules anyway */
        if (first->compact_branch || second->compact_branch)
                return true;

        /* Each instruction reads some registers and writes to a register. See
         * where the first writes */

        int source = first->dest;
        int source_mask = first->mask;

        /* As long as the second doesn't read from the first, we're okay */
        for (unsigned i = 0; i < ARRAY_SIZE(second->src); ++i) {
                if (second->src[i] != source)
                        continue;

                if (first->type != TAG_ALU_4)
                        return false;

                /* Figure out which components we just read from */

                int q = (i == 0) ? second->alu.src1 : second->alu.src2;
                midgard_vector_alu_src *m = (midgard_vector_alu_src *) &q;

                /* Check if there are components in common, and fail if so */
                if (swizzle_to_access_mask(m->swizzle) & source_mask)
                        return false;
        }

        /* Otherwise, it's safe in that regard. Another data hazard is both
         * writing to the same place, of course */

        if (second->dest == source) {
                /* ...but only if the components overlap */

                if (second->mask & source_mask)
                        return false;
        }

        /* ...That's it */
        return true;
}

static bool
midgard_has_hazard(
        midgard_instruction **segment, unsigned segment_size,
        midgard_instruction *ains)
{
        for (int s = 0; s < segment_size; ++s)
                if (!can_run_concurrent_ssa(segment[s], ains))
                        return true;

        return false;


}

/* Fragment writeout (of r0) is allowed when:
 *
 *  - All components of r0 are written in the bundle
 *  - No components of r0 are written in VLUT
 *  - Non-pipelined dependencies of r0 are not written in the bundle
 *
 * This function checks if these requirements are satisfied given the content
 * of a scheduled bundle.
 */

static bool
can_writeout_fragment(compiler_context *ctx, midgard_instruction **bundle, unsigned count, unsigned node_count, unsigned r0)
{
        /* First scan for which components of r0 are written out. Initially
         * none are written */

        uint8_t r0_written_mask = 0x0;

        /* Simultaneously we scan for the set of dependencies */

        size_t sz = sizeof(BITSET_WORD) * BITSET_WORDS(node_count);
        BITSET_WORD *dependencies = calloc(1, sz);
        memset(dependencies, 0, sz);

        bool success = false;

        for (unsigned i = 0; i < count; ++i) {
                midgard_instruction *ins = bundle[i];

                if (ins->dest != r0)
                        continue;

                /* Record written out mask */
                r0_written_mask |= ins->mask;

                /* Record dependencies, but only if they won't become pipeline
                 * registers. We know we can't be live after this, because
                 * we're writeout at the very end of the shader. So check if
                 * they were written before us. */

                unsigned src0 = ins->src[0];
                unsigned src1 = ins->src[1];

                if (!mir_is_written_before(ctx, bundle[0], src0))
                        src0 = ~0;

                if (!mir_is_written_before(ctx, bundle[0], src1))
                        src1 = ~0;

                if (src0 < node_count)
                        BITSET_SET(dependencies, src0);

                if (src1 < node_count)
                        BITSET_SET(dependencies, src1);

                /* Requirement 2 */
                if (ins->unit == UNIT_VLUT)
                        goto done;
        }

        /* Requirement 1 */
        if ((r0_written_mask & 0xF) != 0xF)
                goto done;

        /* Requirement 3 */

        for (unsigned i = 0; i < count; ++i) {
                unsigned dest = bundle[i]->dest;

                if (dest < node_count && BITSET_TEST(dependencies, dest))
                        goto done;
        }

        /* Otherwise, we're good to go */
        success = true;

done:
        free(dependencies);
        return success;
}

/* Helpers for scheudling */

static bool
mir_is_scalar(midgard_instruction *ains)
{
        /* Does the op support scalar units? */
        if (!(alu_opcode_props[ains->alu.op].props & UNITS_SCALAR))
                return false;

        /* Do we try to use it as a vector op? */
        if (!is_single_component_mask(ains->mask))
                return false;

        /* Otherwise, check mode hazards */
        bool could_scalar = true;

        /* Only 16/32-bit can run on a scalar unit */
        could_scalar &= ains->alu.reg_mode != midgard_reg_mode_8;
        could_scalar &= ains->alu.reg_mode != midgard_reg_mode_64;
        could_scalar &= ains->alu.dest_override == midgard_dest_override_none;

        if (ains->alu.reg_mode == midgard_reg_mode_16) {
                /* If we're running in 16-bit mode, we
                 * can't have any 8-bit sources on the
                 * scalar unit (since the scalar unit
                 * doesn't understand 8-bit) */

                midgard_vector_alu_src s1 =
                        vector_alu_from_unsigned(ains->alu.src1);

                could_scalar &= !s1.half;

                midgard_vector_alu_src s2 =
                        vector_alu_from_unsigned(ains->alu.src2);

                could_scalar &= !s2.half;
        }

        return could_scalar;
}

/* How many bytes does this ALU instruction add to the bundle? */

static unsigned
bytes_for_instruction(midgard_instruction *ains)
{
        if (ains->unit & UNITS_ANY_VECTOR)
                return sizeof(midgard_reg_info) + sizeof(midgard_vector_alu);
        else if (ains->unit == ALU_ENAB_BRANCH)
                return sizeof(midgard_branch_extended);
        else if (ains->compact_branch)
                return sizeof(ains->br_compact);
        else
                return sizeof(midgard_reg_info) + sizeof(midgard_scalar_alu);
}

/* Schedules, but does not emit, a single basic block. After scheduling, the
 * final tag and size of the block are known, which are necessary for branching
 * */

static midgard_bundle
schedule_bundle(compiler_context *ctx, midgard_block *block, midgard_instruction *ins, int *skip)
{
        int instructions_emitted = 0, packed_idx = 0;
        midgard_bundle bundle = { 0 };

        midgard_instruction *scheduled[5] = { NULL };

        uint8_t tag = ins->type;

        /* Default to the instruction's tag */
        bundle.tag = tag;

        switch (ins->type) {
        case TAG_ALU_4: {
                uint32_t control = 0;
                size_t bytes_emitted = sizeof(control);

                /* TODO: Constant combining */
                int index = 0, last_unit = 0;

                /* Previous instructions, for the purpose of parallelism */
                midgard_instruction *segment[4] = {0};
                int segment_size = 0;

                instructions_emitted = -1;
                midgard_instruction *pins = ins;

                unsigned constant_count = 0;

                for (;;) {
                        midgard_instruction *ains = pins;

                        /* Advance instruction pointer */
                        if (index) {
                                ains = mir_next_op(pins);
                                pins = ains;
                        }

                        /* Out-of-work condition */
                        if ((struct list_head *) ains == &block->instructions)
                                break;

                        /* Ensure that the chain can continue */
                        if (ains->type != TAG_ALU_4) break;

                        /* If there's already something in the bundle and we
                         * have weird scheduler constraints, break now */
                        if (ains->precede_break && index) break;

                        /* According to the presentation "The ARM
                         * Mali-T880 Mobile GPU" from HotChips 27,
                         * there are two pipeline stages. Branching
                         * position determined experimentally. Lines
                         * are executed in parallel:
                         *
                         * [ VMUL ] [ SADD ]
                         * [ VADD ] [ SMUL ] [ LUT ] [ BRANCH ]
                         *
                         * Verify that there are no ordering dependencies here.
                         *
                         * TODO: Allow for parallelism!!!
                         */

                        /* Pick a unit for it if it doesn't force a particular unit */

                        int unit = ains->unit;

                        if (!unit) {
                                int op = ains->alu.op;
                                int units = alu_opcode_props[op].props;
                                bool scalar = mir_is_scalar(ains);

                                if (!scalar) {
                                        if (last_unit >= UNIT_VADD) {
                                                if (units & UNIT_VLUT)
                                                        unit = UNIT_VLUT;
                                                else
                                                        break;
                                        } else {
                                                if ((units & UNIT_VMUL) && last_unit < UNIT_VMUL)
                                                        unit = UNIT_VMUL;
                                                else if ((units & UNIT_VADD) && !(control & UNIT_VADD))
                                                        unit = UNIT_VADD;
                                                else if (units & UNIT_VLUT)
                                                        unit = UNIT_VLUT;
                                                else
                                                        break;
                                        }
                                } else {
                                        if (last_unit >= UNIT_VADD) {
                                                if ((units & UNIT_SMUL) && !(control & UNIT_SMUL))
                                                        unit = UNIT_SMUL;
                                                else if (units & UNIT_VLUT)
                                                        unit = UNIT_VLUT;
                                                else
                                                        break;
                                        } else {
                                                if ((units & UNIT_VMUL) && (last_unit < UNIT_VMUL))
                                                        unit = UNIT_VMUL;
                                                else if ((units & UNIT_SADD) && !(control & UNIT_SADD) && !midgard_has_hazard(segment, segment_size, ains))
                                                        unit = UNIT_SADD;
                                                else if (units & UNIT_VADD)
                                                        unit = UNIT_VADD;
                                                else if (units & UNIT_SMUL)
                                                        unit = UNIT_SMUL;
                                                else if (units & UNIT_VLUT)
                                                        unit = UNIT_VLUT;
                                                else
                                                        break;
                                        }
                                }

                                assert(unit & units);
                        }

                        /* Late unit check, this time for encoding (not parallelism) */
                        if (unit <= last_unit) break;

                        /* Clear the segment */
                        if (last_unit < UNIT_VADD && unit >= UNIT_VADD)
                                segment_size = 0;

                        if (midgard_has_hazard(segment, segment_size, ains))
                                break;

                        /* We're good to go -- emit the instruction */
                        ains->unit = unit;

                        segment[segment_size++] = ains;

                        /* We try to reuse constants if possible, by adjusting
                         * the swizzle */

                        if (ains->has_blend_constant) {
                                /* Everything conflicts with the blend constant */
                                if (bundle.has_embedded_constants)
                                        break;

                                bundle.has_blend_constant = 1;
                                bundle.has_embedded_constants = 1;
                        } else if (ains->has_constants && ains->alu.reg_mode == midgard_reg_mode_16) {
                                /* TODO: DRY with the analysis pass */

                                if (bundle.has_blend_constant)
                                        break;

                                if (constant_count)
                                        break;

                                /* TODO: Fix packing XXX */
                                uint16_t *bundles = (uint16_t *) bundle.constants;
                                uint32_t *constants = (uint32_t *) ains->constants;

                                /* Copy them wholesale */
                                for (unsigned i = 0; i < 4; ++i)
                                        bundles[i] = constants[i];

                                bundle.has_embedded_constants = true;
                                constant_count = 4;
                        } else if (ains->has_constants) {
                                /* By definition, blend constants conflict with
                                 * everything, so if there are already
                                 * constants we break the bundle *now* */

                                if (bundle.has_blend_constant)
                                        break;

                                /* For anything but blend constants, we can do
                                 * proper analysis, however */

                                /* TODO: Mask by which are used */
                                uint32_t *constants = (uint32_t *) ains->constants;
                                uint32_t *bundles = (uint32_t *) bundle.constants;

                                uint32_t indices[4] = { 0 };
                                bool break_bundle = false;

                                for (unsigned i = 0; i < 4; ++i) {
                                        uint32_t cons = constants[i];
                                        bool constant_found = false;

                                        /* Search for the constant */
                                        for (unsigned j = 0; j < constant_count; ++j) {
                                                if (bundles[j] != cons)
                                                        continue;

                                                /* We found it, reuse */
                                                indices[i] = j;
                                                constant_found = true;
                                                break;
                                        }

                                        if (constant_found)
                                                continue;

                                        /* We didn't find it, so allocate it */
                                        unsigned idx = constant_count++;

                                        if (idx >= 4) {
                                                /* Uh-oh, out of space */
                                                break_bundle = true;
                                                break;
                                        }

                                        /* We have space, copy it in! */
                                        bundles[idx] = cons;
                                        indices[i] = idx;
                                }

                                if (break_bundle)
                                        break;

                                /* Cool, we have it in. So use indices as a
                                 * swizzle */

                                unsigned swizzle = SWIZZLE_FROM_ARRAY(indices);
                                unsigned r_constant = SSA_FIXED_REGISTER(REGISTER_CONSTANT);

                                if (ains->src[0] == r_constant)
                                        ains->alu.src1 = vector_alu_apply_swizzle(ains->alu.src1, swizzle);

                                if (ains->src[1] == r_constant)
                                        ains->alu.src2 = vector_alu_apply_swizzle(ains->alu.src2, swizzle);

                                bundle.has_embedded_constants = true;
                        }

                        if (ains->compact_branch) {
                                /* All of r0 has to be written out along with
                                 * the branch writeout */

                                if (ains->writeout && !can_writeout_fragment(ctx, scheduled, index, ctx->temp_count, ains->src[0])) {
                                        /* We only work on full moves
                                         * at the beginning. We could
                                         * probably do better */
                                        if (index != 0)
                                                break;

                                        /* Inject a move */
                                        midgard_instruction ins = v_mov(0, blank_alu_src, SSA_FIXED_REGISTER(0));
                                        ins.unit = UNIT_VMUL;
                                        control |= ins.unit;

                                        /* TODO don't leak */
                                        midgard_instruction *move =
                                                mem_dup(&ins, sizeof(midgard_instruction));
                                        bytes_emitted += bytes_for_instruction(move);
                                        bundle.instructions[packed_idx++] = move;
                                }
                        }

                        bytes_emitted += bytes_for_instruction(ains);

                        /* Defer marking until after writing to allow for break */
                        scheduled[index] = ains;
                        control |= ains->unit;
                        last_unit = ains->unit;
                        ++instructions_emitted;
                        ++index;
                }

                int padding = 0;

                /* Pad ALU op to nearest word */

                if (bytes_emitted & 15) {
                        padding = 16 - (bytes_emitted & 15);
                        bytes_emitted += padding;
                }

                /* Constants must always be quadwords */
                if (bundle.has_embedded_constants)
                        bytes_emitted += 16;

                /* Size ALU instruction for tag */
                bundle.tag = (TAG_ALU_4) + (bytes_emitted / 16) - 1;
                bundle.padding = padding;
                bundle.control = bundle.tag | control;

                break;
        }

        case TAG_LOAD_STORE_4: {
                /* Load store instructions have two words at once. If
                 * we only have one queued up, we need to NOP pad.
                 * Otherwise, we store both in succession to save space
                 * and cycles -- letting them go in parallel -- skip
                 * the next. The usefulness of this optimisation is
                 * greatly dependent on the quality of the instruction
                 * scheduler.
                 */

                midgard_instruction *next_op = mir_next_op(ins);

                if ((struct list_head *) next_op != &block->instructions && next_op->type == TAG_LOAD_STORE_4) {
                        /* TODO: Concurrency check */
                        instructions_emitted++;
                }

                break;
        }

        case TAG_TEXTURE_4: {
                /* Which tag we use depends on the shader stage */
                bool in_frag = ctx->stage == MESA_SHADER_FRAGMENT;
                bundle.tag = in_frag ? TAG_TEXTURE_4 : TAG_TEXTURE_4_VTX;
                break;
        }

        default:
                unreachable("Unknown tag");
                break;
        }

        /* Copy the instructions into the bundle */
        bundle.instruction_count = instructions_emitted + 1 + packed_idx;

        midgard_instruction *uins = ins;
        for (; packed_idx < bundle.instruction_count; ++packed_idx) {
                assert(&uins->link != &block->instructions);
                bundle.instructions[packed_idx] = uins;
                uins = mir_next_op(uins);
        }

        *skip = instructions_emitted;

        return bundle;
}

/* We would like to flatten the linked list of midgard_instructions in a bundle
 * to an array of pointers on the heap for easy indexing */

static midgard_instruction **
flatten_mir(midgard_block *block, unsigned *len)
{
        *len = list_length(&block->instructions);

        if (!(*len))
                return NULL;

        midgard_instruction **instructions =
                calloc(sizeof(midgard_instruction *), *len);

        unsigned i = 0;

        mir_foreach_instr_in_block(block, ins)
                instructions[i++] = ins;

        return instructions;
}

/* The worklist is the set of instructions that can be scheduled now; that is,
 * the set of instructions with no remaining dependencies */

static void
mir_initialize_worklist(BITSET_WORD *worklist, midgard_instruction **instructions, unsigned count)
{
        for (unsigned i = 0; i < count; ++i) {
                if (instructions[i]->nr_dependencies == 0)
                        BITSET_SET(worklist, i);
        }
}

/* Update the worklist after an instruction terminates. Remove its edges from
 * the graph and if that causes any node to have no dependencies, add it to the
 * worklist */

static void
mir_update_worklist(
                BITSET_WORD *worklist, unsigned count,
                midgard_instruction **instructions, midgard_instruction *done)
{
        /* Sanity check: if no instruction terminated, there is nothing to do.
         * If the instruction that terminated had dependencies, that makes no
         * sense and means we messed up the worklist. Finally, as the purpose
         * of this routine is to update dependents, we abort early if there are
         * no dependents defined. */

        if (!done)
                return;

        assert(done->nr_dependencies == 0);

        if (!done->dependents)
                return;

        /* We have an instruction with dependents. Iterate each dependent to
         * remove one dependency (`done`), adding dependents to the worklist
         * where possible. */

        unsigned i;
        BITSET_WORD tmp;
        BITSET_FOREACH_SET(i, tmp, done->dependents, count) {
                assert(instructions[i]->nr_dependencies);

                if (!(--instructions[i]->nr_dependencies))
                        BITSET_SET(worklist, i);
        }

        free(done->dependents);
}

/* While scheduling, we need to choose instructions satisfying certain
 * criteria. As we schedule backwards, we choose the *last* instruction in the
 * worklist to simulate in-order scheduling. Chosen instructions must satisfy a
 * given predicate. */

struct midgard_predicate {
        /* TAG or ~0 for dont-care */
        unsigned tag;

        /* True if we want to pop off the chosen instruction */
        bool destructive;

        /* For ALU, choose only this unit */
        unsigned unit;

        /* State for bundle constants. constants is the actual constants
         * for the bundle. constant_count is the number of bytes (up to
         * 16) currently in use for constants. When picking in destructive
         * mode, the constants array will be updated, and the instruction
         * will be adjusted to index into the constants array */

        uint8_t *constants;
        unsigned constant_count;
        bool blend_constant;

        /* Exclude this destination (if not ~0) */
        unsigned exclude;
};

/* For an instruction that can fit, adjust it to fit and update the constants
 * array, in destructive mode. Returns whether the fitting was successful. */

static bool
mir_adjust_constants(midgard_instruction *ins,
                struct midgard_predicate *pred,
                bool destructive)
{
        /* Blend constants dominate */
        if (ins->has_blend_constant) {
                if (pred->constant_count)
                        return false;
                else if (destructive) {
                        pred->blend_constant = true;
                        pred->constant_count = 16;
                        return true;
                }
        }

        /* No constant, nothing to adjust */
        if (!ins->has_constants)
                return true;

        /* TODO: Deduplicate; permit multiple constants within a bundle */

        if (destructive && !pred->constant_count) {
                if (ins->alu.reg_mode == midgard_reg_mode_16) {
                      /* TODO: Fix packing XXX */
                        uint16_t *bundles = (uint16_t *) pred->constants;
                        uint32_t *constants = (uint32_t *) ins->constants;

                        /* Copy them wholesale */
                        for (unsigned i = 0; i < 4; ++i)
                                bundles[i] = constants[i];
                } else {
                        memcpy(pred->constants, ins->constants, 16);
                }

                pred->constant_count = 16;
                return true;
        }

        return !pred->constant_count;
}

static midgard_instruction *
mir_choose_instruction(
                midgard_instruction **instructions,
                BITSET_WORD *worklist, unsigned count,
                struct midgard_predicate *predicate)
{
        /* Parse the predicate */
        unsigned tag = predicate->tag;
        bool alu = tag == TAG_ALU_4;
        unsigned unit = predicate->unit;
        bool branch = alu && (unit == ALU_ENAB_BR_COMPACT);

        /* Iterate to find the best instruction satisfying the predicate */
        unsigned i;
        BITSET_WORD tmp;

        signed best_index = -1;

        /* Enforce a simple metric limiting distance to keep down register
         * pressure. TOOD: replace with liveness tracking for much better
         * results */

        unsigned max_active = 0;
        unsigned max_distance = 6;

        BITSET_FOREACH_SET(i, tmp, worklist, count) {
                max_active = MAX2(max_active, i);
        }

        BITSET_FOREACH_SET(i, tmp, worklist, count) {
                if ((max_active - i) >= max_distance)
                        continue;

                if (tag != ~0 && instructions[i]->type != tag)
                        continue;

                if (predicate->exclude != ~0 && instructions[i]->dest == predicate->exclude)
                        continue;

                if (alu && !branch && !(alu_opcode_props[instructions[i]->alu.op].props & unit))
                        continue;

                if (branch && !instructions[i]->compact_branch)
                        continue;

                /* Simulate in-order scheduling */
                if ((signed) i < best_index)
                        continue;

                best_index = i;
        }


        /* Did we find anything?  */

        if (best_index < 0)
                return NULL;

        /* If we found something, remove it from the worklist */
        assert(best_index < count);

        if (predicate->destructive) {
                BITSET_CLEAR(worklist, best_index);
        }

        return instructions[best_index];
}

/* Still, we don't choose instructions in a vacuum. We need a way to choose the
 * best bundle type (ALU, load/store, texture). Nondestructive. */

static unsigned
mir_choose_bundle(
                midgard_instruction **instructions,
                BITSET_WORD *worklist, unsigned count)
{
        /* At the moment, our algorithm is very simple - use the bundle of the
         * best instruction, regardless of what else could be scheduled
         * alongside it. This is not optimal but it works okay for in-order */

        struct midgard_predicate predicate = {
                .tag = ~0,
                .destructive = false,
                .exclude = ~0
        };

        midgard_instruction *chosen = mir_choose_instruction(instructions, worklist, count, &predicate);

        if (chosen)
                return chosen->type;
        else
                return ~0;
}

/* We want to choose an ALU instruction filling a given unit */
static void
mir_choose_alu(midgard_instruction **slot,
                midgard_instruction **instructions,
                BITSET_WORD *worklist, unsigned len,
                struct midgard_predicate *predicate,
                unsigned unit)
{
        /* Did we already schedule to this slot? */
        if ((*slot) != NULL)
                return;

        /* Try to schedule something, if not */
        predicate->unit = unit;
        *slot = mir_choose_instruction(instructions, worklist, len, predicate);

        /* Store unit upon scheduling */
        if (*slot && !((*slot)->compact_branch))
                (*slot)->unit = unit;
}

/* When we are scheduling a branch/csel, we need the consumed condition in the
 * same block as a pipeline register. There are two options to enable this:
 *
 *  - Move the conditional into the bundle. Preferred, but only works if the
 *    conditional is used only once and is from this block.
 *  - Copy the conditional.
 *
 * We search for the conditional. If it's in this block, single-use, and
 * without embedded constants, we schedule it immediately. Otherwise, we
 * schedule a move for it.
 *
 * mir_comparison_mobile is a helper to find the moveable condition.
 */

static unsigned
mir_comparison_mobile(
                compiler_context *ctx,
                midgard_instruction **instructions,
                unsigned count,
                unsigned cond)
{
        if (!mir_single_use(ctx, cond))
                return ~0;

        unsigned ret = ~0;

        for (unsigned i = 0; i < count; ++i) {
                if (instructions[i]->dest != cond)
                        continue;

                /* Must fit in an ALU bundle */
                if (instructions[i]->type != TAG_ALU_4)
                        return ~0;

                /* We'll need to rewrite to .w but that doesn't work for vector
                 * ops that don't replicate (ball/bany), so bail there */

                if (GET_CHANNEL_COUNT(alu_opcode_props[instructions[i]->alu.op].props))
                        return ~0;

                /* TODO: moving conditionals with constants */

                if (instructions[i]->has_constants)
                        return ~0;

                /* Ensure it is written only once */

                if (ret != ~0)
                        return ~0;
                else
                        ret = i;
        }

        return ret;
}

/* Using the information about the moveable conditional itself, we either pop
 * that condition off the worklist for use now, or create a move to
 * artificially schedule instead as a fallback */

static midgard_instruction *
mir_schedule_comparison(
                compiler_context *ctx,
                midgard_instruction **instructions,
                BITSET_WORD *worklist, unsigned count,
                unsigned cond, bool vector, unsigned swizzle,
                midgard_instruction *user)
{
        /* TODO: swizzle when scheduling */
        unsigned comp_i =
                (!vector && (swizzle == 0)) ?
                mir_comparison_mobile(ctx, instructions, count, cond) : ~0;

        /* If we can, schedule the condition immediately */
        if ((comp_i != ~0) && BITSET_TEST(worklist, comp_i)) {
                assert(comp_i < count);
                BITSET_CLEAR(worklist, comp_i);
                return instructions[comp_i];
        }

        /* Otherwise, we insert a move */
        midgard_vector_alu_src csel = {
                .swizzle = swizzle
        };

        midgard_instruction mov = v_mov(cond, csel, cond);
        mov.mask = vector ? 0xF : 0x1;

        return mir_insert_instruction_before(ctx, user, mov);
}

/* Most generally, we need instructions writing to r31 in the appropriate
 * components */

static midgard_instruction *
mir_schedule_condition(compiler_context *ctx,
                struct midgard_predicate *predicate,
                BITSET_WORD *worklist, unsigned count,
                midgard_instruction **instructions,
                midgard_instruction *last)
{
        /* For a branch, the condition is the only argument; for csel, third */
        bool branch = last->compact_branch;
        unsigned condition_index = branch ? 0 : 2;

        /* csel_v is vector; otherwise, conditions are scalar */
        bool vector = !branch && OP_IS_CSEL_V(last->alu.op);

        /* Grab the conditional instruction */

        midgard_instruction *cond = mir_schedule_comparison(
                        ctx, instructions, worklist, count, last->src[condition_index],
                        vector, last->cond_swizzle, last);

        /* We have exclusive reign over this (possibly move) conditional
         * instruction. We can rewrite into a pipeline conditional register */

        predicate->exclude = cond->dest;
        cond->dest = SSA_FIXED_REGISTER(31);

        if (!vector) {
                cond->mask = (1 << COMPONENT_W);

                mir_foreach_src(cond, s) {
                        if (cond->src[s] == ~0)
                                continue;

                        mir_set_swizzle(cond, s, (mir_get_swizzle(cond, s) << (2*3)) & 0xFF);
                }
        }

        /* Schedule the unit: csel is always in the latter pipeline, so a csel
         * condition must be in the former pipeline stage (vmul/sadd),
         * depending on scalar/vector of the instruction itself. A branch must
         * be written from the latter pipeline stage and a branch condition is
         * always scalar, so it is always in smul (exception: ball/bany, which
         * will be vadd) */

        if (branch)
                cond->unit = UNIT_SMUL;
        else
                cond->unit = vector ? UNIT_VMUL : UNIT_SADD;

        return cond;
}

/* Schedules a single bundle of the given type */

static midgard_bundle
mir_schedule_texture(
                midgard_instruction **instructions,
                BITSET_WORD *worklist, unsigned len)
{
        struct midgard_predicate predicate = {
                .tag = TAG_TEXTURE_4,
                .destructive = true,
                .exclude = ~0
        };

        midgard_instruction *ins =
                mir_choose_instruction(instructions, worklist, len, &predicate);

        mir_update_worklist(worklist, len, instructions, ins);

        struct midgard_bundle out = {
                .tag = TAG_TEXTURE_4,
                .instruction_count = 1,
                .instructions = { ins }
        };

        return out;
}

static midgard_bundle
mir_schedule_ldst(
                midgard_instruction **instructions,
                BITSET_WORD *worklist, unsigned len)
{
        struct midgard_predicate predicate = {
                .tag = TAG_LOAD_STORE_4,
                .destructive = true,
                .exclude = ~0
        };

        /* Try to pick two load/store ops. Second not gauranteed to exist */

        midgard_instruction *ins =
                mir_choose_instruction(instructions, worklist, len, &predicate);

        midgard_instruction *pair =
                mir_choose_instruction(instructions, worklist, len, &predicate);

        struct midgard_bundle out = {
                .tag = TAG_LOAD_STORE_4,
                .instruction_count = pair ? 2 : 1,
                .instructions = { ins, pair }
        };

        /* We have to update the worklist atomically, since the two
         * instructions run concurrently (TODO: verify it's not pipelined) */

        mir_update_worklist(worklist, len, instructions, ins);
        mir_update_worklist(worklist, len, instructions, pair);

        return out;
}

static midgard_bundle
mir_schedule_alu(
                compiler_context *ctx,
                midgard_instruction **instructions,
                BITSET_WORD *worklist, unsigned len)
{
        struct midgard_bundle bundle = {};

        unsigned bytes_emitted = sizeof(bundle.control);

        struct midgard_predicate predicate = {
                .tag = TAG_ALU_4,
                .destructive = true,
                .exclude = ~0
        };

        midgard_instruction *ins =
                mir_choose_instruction(instructions, worklist, len, &predicate);

        midgard_instruction *vmul = NULL;
        midgard_instruction *vadd = NULL;
        midgard_instruction *vlut = NULL;
        midgard_instruction *smul = NULL;
        midgard_instruction *sadd = NULL;
        midgard_instruction *branch = NULL;

        mir_update_worklist(worklist, len, instructions, ins);

        if (ins->compact_branch) {
                branch = ins;
        } else if (!ins->unit) {
                unsigned units = alu_opcode_props[ins->alu.op].props;

                if (units & UNIT_VMUL) {
                        ins->unit = UNIT_VMUL;
                        vmul = ins;
                } else if (units & UNIT_VADD) {
                        ins->unit = UNIT_VADD;
                        vadd = ins;
                } else if (units & UNIT_VLUT) {
                        ins->unit = UNIT_VLUT;
                        vlut = ins;
                } else
                        assert(0);
        }

        bundle.has_embedded_constants = ins->has_constants;
        bundle.has_blend_constant = ins->has_blend_constant;

        if (ins->alu.reg_mode == midgard_reg_mode_16) {
              /* TODO: Fix packing XXX */
                uint16_t *bundles = (uint16_t *) bundle.constants;
                uint32_t *constants = (uint32_t *) ins->constants;

                /* Copy them wholesale */
                for (unsigned i = 0; i < 4; ++i)
                        bundles[i] = constants[i];
        } else {
                memcpy(bundle.constants, ins->constants, sizeof(bundle.constants));
        }

        if (ins->writeout) {
                unsigned src = (branch->src[0] == ~0) ? SSA_FIXED_REGISTER(0) : branch->src[0];
                unsigned temp = (branch->src[0] == ~0) ? SSA_FIXED_REGISTER(0) : make_compiler_temp(ctx);
                midgard_instruction mov = v_mov(src, blank_alu_src, temp);
                vmul = mem_dup(&mov, sizeof(midgard_instruction));
                vmul->unit = UNIT_VMUL;
                vmul->mask = 0xF;
                /* TODO: Don't leak */

                /* Rewrite to use our temp */
                midgard_instruction *stages[] = { sadd, vadd, smul };

                for (unsigned i = 0; i < ARRAY_SIZE(stages); ++i) {
                        if (stages[i])
                                mir_rewrite_index_dst_single(stages[i], src, temp);
                }

                mir_rewrite_index_src_single(branch, src, temp);
        }

        if ((vadd && OP_IS_CSEL(vadd->alu.op)) || (smul && OP_IS_CSEL(smul->alu.op)) || (ins->compact_branch && !ins->prepacked_branch && ins->branch.conditional)) {
                midgard_instruction *cond = mir_choose_instruction(instructions, worklist, len, &predicate);
                mir_update_worklist(worklist, len, instructions, cond);

                if (!cond->unit) {
                        unsigned units = alu_opcode_props[cond->alu.op].props;

                        if (units & UNIT_VMUL) {
                                cond->unit = UNIT_VMUL;
                        } else if (units & UNIT_VADD) {
                                cond->unit = UNIT_VADD;
                        } else
                                assert(0);
                }

                if (cond->unit & UNIT_VMUL)
                        vmul = cond;
                else if (cond->unit & UNIT_SADD)
                        sadd = cond;
                else if (cond->unit & UNIT_VADD)
                        vadd = cond;
                else if (cond->unit & UNIT_SMUL)
                        smul = cond;
                else
                        unreachable("Bad condition");
        }

        unsigned padding = 0;

        /* Now that we have finished scheduling, build up the bundle */
        midgard_instruction *stages[] = { vmul, sadd, vadd, smul, vlut, branch };

        for (unsigned i = 0; i < ARRAY_SIZE(stages); ++i) {
                if (stages[i]) {
                        bundle.control |= stages[i]->unit;
                        bytes_emitted += bytes_for_instruction(stages[i]);
                        bundle.instructions[bundle.instruction_count++] = stages[i];
                }
        }

        /* Pad ALU op to nearest word */

        if (bytes_emitted & 15) {
                padding = 16 - (bytes_emitted & 15);
                bytes_emitted += padding;
        }

        /* Constants must always be quadwords */
        if (bundle.has_embedded_constants)
                bytes_emitted += 16;

        /* Size ALU instruction for tag */
        bundle.tag = (TAG_ALU_4) + (bytes_emitted / 16) - 1;
        bundle.padding = padding;
        bundle.control |= bundle.tag;

        return bundle;
}

/* Schedule a single block by iterating its instruction to create bundles.
 * While we go, tally about the bundle sizes to compute the block size. */


static void
schedule_block(compiler_context *ctx, midgard_block *block)
{
        /* Copy list to dynamic array */
        unsigned len = 0;
        midgard_instruction **instructions = flatten_mir(block, &len);

        /* Calculate dependencies and initial worklist */
        unsigned node_count = ctx->temp_count + 1;
        mir_create_dependency_graph(instructions, len, node_count);

        /* Allocate the worklist */
        size_t sz = BITSET_WORDS(len) * sizeof(BITSET_WORD);
        BITSET_WORD *worklist = calloc(sz, 1);
        mir_initialize_worklist(worklist, instructions, len);

        util_dynarray_init(&block->bundles, NULL);

        block->quadword_count = 0;

        int skip = 0;
        mir_foreach_instr_in_block(block, ins) {
                if (skip) {
                        skip--;
                        continue;
                }

                midgard_bundle bundle = schedule_bundle(ctx, block, ins, &skip);
                util_dynarray_append(&block->bundles, midgard_bundle, bundle);

                if (bundle.has_blend_constant) {
                        unsigned offset = ctx->quadword_count + block->quadword_count + quadword_size(bundle.tag) - 1;
                        ctx->blend_constant_offset = offset * 0x10;
                }

                block->quadword_count += quadword_size(bundle.tag);
        }

        block->is_scheduled = true;
        ctx->quadword_count += block->quadword_count;
}

/* When we're 'squeezing down' the values in the IR, we maintain a hash
 * as such */

static unsigned
find_or_allocate_temp(compiler_context *ctx, unsigned hash)
{
        if (hash >= SSA_FIXED_MINIMUM)
                return hash;

        unsigned temp = (uintptr_t) _mesa_hash_table_u64_search(
                                ctx->hash_to_temp, hash + 1);

        if (temp)
                return temp - 1;

        /* If no temp is find, allocate one */
        temp = ctx->temp_count++;
        ctx->max_hash = MAX2(ctx->max_hash, hash);

        _mesa_hash_table_u64_insert(ctx->hash_to_temp,
                                    hash + 1, (void *) ((uintptr_t) temp + 1));

        return temp;
}

/* Reassigns numbering to get rid of gaps in the indices */

static void
mir_squeeze_index(compiler_context *ctx)
{
        /* Reset */
        ctx->temp_count = 0;
        /* TODO don't leak old hash_to_temp */
        ctx->hash_to_temp = _mesa_hash_table_u64_create(NULL);

        mir_foreach_instr_global(ctx, ins) {
                ins->dest = find_or_allocate_temp(ctx, ins->dest);

                for (unsigned i = 0; i < ARRAY_SIZE(ins->src); ++i)
                        ins->src[i] = find_or_allocate_temp(ctx, ins->src[i]);
        }
}

static midgard_instruction
v_load_store_scratch(
                unsigned srcdest,
                unsigned index,
                bool is_store,
                unsigned mask)
{
        /* We index by 32-bit vec4s */
        unsigned byte = (index * 4 * 4);

        midgard_instruction ins = {
                .type = TAG_LOAD_STORE_4,
                .mask = mask,
                .dest = ~0,
                .src = { ~0, ~0, ~0 },
                .load_store = {
                        .op = is_store ? midgard_op_st_int4 : midgard_op_ld_int4,
                        .swizzle = SWIZZLE_XYZW,

                        /* For register spilling - to thread local storage */
                        .arg_1 = 0xEA,
                        .arg_2 = 0x1E,

                        /* Splattered across, TODO combine logically */
                        .varying_parameters = (byte & 0x1FF) << 1,
                        .address = (byte >> 9)
                },

                /* If we spill an unspill, RA goes into an infinite loop */
                .no_spill = true
        };

       if (is_store) {
                /* r0 = r26, r1 = r27 */
                assert(srcdest == SSA_FIXED_REGISTER(26) || srcdest == SSA_FIXED_REGISTER(27));
                ins.src[0] = srcdest;
        } else {
                ins.dest = srcdest;
        }

        return ins;
}

/* If register allocation fails, find the best spill node and spill it to fix
 * whatever the issue was. This spill node could be a work register (spilling
 * to thread local storage), but it could also simply be a special register
 * that needs to spill to become a work register. */

static void mir_spill_register(
                compiler_context *ctx,
                struct ra_graph *g,
                unsigned *spill_count)
{
        unsigned spill_index = ctx->temp_count;

        /* Our first step is to calculate spill cost to figure out the best
         * spill node. All nodes are equal in spill cost, but we can't spill
         * nodes written to from an unspill */

        for (unsigned i = 0; i < ctx->temp_count; ++i) {
                ra_set_node_spill_cost(g, i, 1.0);
        }

        /* We can't spill any bundles that contain unspills. This could be
         * optimized to allow use of r27 to spill twice per bundle, but if
         * you're at the point of optimizing spilling, it's too late. */

        mir_foreach_block(ctx, block) {
                mir_foreach_bundle_in_block(block, bun) {
                        bool no_spill = false;

                        for (unsigned i = 0; i < bun->instruction_count; ++i)
                                no_spill |= bun->instructions[i]->no_spill;

                        if (!no_spill)
                                continue;

                        for (unsigned i = 0; i < bun->instruction_count; ++i) {
                                unsigned dest = bun->instructions[i]->dest;
                                if (dest < ctx->temp_count)
                                        ra_set_node_spill_cost(g, dest, -1.0);
                        }
                }
        }

        int spill_node = ra_get_best_spill_node(g);

        if (spill_node < 0) {
                mir_print_shader(ctx);
                assert(0);
        }

        /* We have a spill node, so check the class. Work registers
         * legitimately spill to TLS, but special registers just spill to work
         * registers */

        unsigned class = ra_get_node_class(g, spill_node);
        bool is_special = (class >> 2) != REG_CLASS_WORK;
        bool is_special_w = (class >> 2) == REG_CLASS_TEXW;

        /* Allocate TLS slot (maybe) */
        unsigned spill_slot = !is_special ? (*spill_count)++ : 0;

        /* For TLS, replace all stores to the spilled node. For
         * special reads, just keep as-is; the class will be demoted
         * implicitly. For special writes, spill to a work register */

        if (!is_special || is_special_w) {
                if (is_special_w)
                        spill_slot = spill_index++;

                mir_foreach_block(ctx, block) {
                mir_foreach_instr_in_block_safe(block, ins) {
                        if (ins->dest != spill_node) continue;

                        midgard_instruction st;

                        if (is_special_w) {
                                st = v_mov(spill_node, blank_alu_src, spill_slot);
                                st.no_spill = true;
                        } else {
                                ins->dest = SSA_FIXED_REGISTER(26);
                                ins->no_spill = true;
                                st = v_load_store_scratch(ins->dest, spill_slot, true, ins->mask);
                        }

                        /* Hint: don't rewrite this node */
                        st.hint = true;

                        mir_insert_instruction_after_scheduled(ctx, block, ins, st);

                        if (!is_special)
                                ctx->spills++;
                }
                }
        }

        /* For special reads, figure out how many components we need */
        unsigned read_mask = 0;

        mir_foreach_instr_global_safe(ctx, ins) {
                read_mask |= mir_mask_of_read_components(ins, spill_node);
        }

        /* Insert a load from TLS before the first consecutive
         * use of the node, rewriting to use spilled indices to
         * break up the live range. Or, for special, insert a
         * move. Ironically the latter *increases* register
         * pressure, but the two uses of the spilling mechanism
         * are somewhat orthogonal. (special spilling is to use
         * work registers to back special registers; TLS
         * spilling is to use memory to back work registers) */

        mir_foreach_block(ctx, block) {
                bool consecutive_skip = false;
                unsigned consecutive_index = 0;

                mir_foreach_instr_in_block(block, ins) {
                        /* We can't rewrite the moves used to spill in the
                         * first place. These moves are hinted. */
                        if (ins->hint) continue;

                        if (!mir_has_arg(ins, spill_node)) {
                                consecutive_skip = false;
                                continue;
                        }

                        if (consecutive_skip) {
                                /* Rewrite */
                                mir_rewrite_index_src_single(ins, spill_node, consecutive_index);
                                continue;
                        }

                        if (!is_special_w) {
                                consecutive_index = ++spill_index;

                                midgard_instruction *before = ins;

                                /* For a csel, go back one more not to break up the bundle */
                                if (ins->type == TAG_ALU_4 && OP_IS_CSEL(ins->alu.op))
                                        before = mir_prev_op(before);

                                midgard_instruction st;

                                if (is_special) {
                                        /* Move */
                                        st = v_mov(spill_node, blank_alu_src, consecutive_index);
                                        st.no_spill = true;
                                } else {
                                        /* TLS load */
                                        st = v_load_store_scratch(consecutive_index, spill_slot, false, 0xF);
                                }

                                /* Mask the load based on the component count
                                 * actually needed to prvent RA loops */

                                st.mask = read_mask;

                                mir_insert_instruction_before_scheduled(ctx, block, before, st);
                               // consecutive_skip = true;
                        } else {
                                /* Special writes already have their move spilled in */
                                consecutive_index = spill_slot;
                        }


                        /* Rewrite to use */
                        mir_rewrite_index_src_single(ins, spill_node, consecutive_index);

                        if (!is_special)
                                ctx->fills++;
                }
        }

        /* Reset hints */

        mir_foreach_instr_global(ctx, ins) {
                ins->hint = false;
        }
}

void
schedule_program(compiler_context *ctx)
{
        struct ra_graph *g = NULL;
        bool spilled = false;
        int iter_count = 1000; /* max iterations */

        /* Number of 128-bit slots in memory we've spilled into */
        unsigned spill_count = 0;

        midgard_promote_uniforms(ctx, 16);

        /* Must be lowered right before RA */
        mir_squeeze_index(ctx);
        mir_lower_special_reads(ctx);
        mir_squeeze_index(ctx);

        /* Lowering can introduce some dead moves */

        mir_foreach_block(ctx, block) {
                midgard_opt_dead_move_eliminate(ctx, block);
                schedule_block(ctx, block);
        }

        mir_create_pipeline_registers(ctx);

        do {
                if (spilled) 
                        mir_spill_register(ctx, g, &spill_count);

                mir_squeeze_index(ctx);

                g = NULL;
                g = allocate_registers(ctx, &spilled);
        } while(spilled && ((iter_count--) > 0));

        if (iter_count <= 0) {
                fprintf(stderr, "panfrost: Gave up allocating registers, rendering will be incomplete\n");
                assert(0);
        }

        /* Report spilling information. spill_count is in 128-bit slots (vec4 x
         * fp32), but tls_size is in bytes, so multiply by 16 */

        ctx->tls_size = spill_count * 16;

        install_registers(ctx, g);
}
