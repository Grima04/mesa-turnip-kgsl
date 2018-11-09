/*
 * Copyright © 2019 Intel Corporation
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/** @file brw_fs_scoreboard.cpp
 *
 * Gen12+ hardware lacks the register scoreboard logic that used to guarantee
 * data coherency between register reads and writes in previous generations.
 * This lowering pass runs after register allocation in order to make up for
 * it.
 *
 * It works by performing global dataflow analysis in order to determine the
 * set of potential dependencies of every instruction in the shader, and then
 * inserts any required SWSB annotations and additional SYNC instructions in
 * order to guarantee data coherency.
 *
 * WARNING - Access of the following (rarely used) ARF registers is not
 *           tracked here, and require the RegDist SWSB annotation to be set
 *           to 1 by the generator in order to avoid data races:
 *
 *  - sp stack pointer
 *  - sr0 state register
 *  - cr0 control register
 *  - ip instruction pointer
 *  - tm0 timestamp register
 *  - dbg0 debug register
 *
 * The following ARF registers don't need to be tracked here because data
 * coherency is still provided transparently by the hardware:
 *
 *  - f0-1 flag registers
 *  - n0 notification register
 *  - tdr0 thread dependency register
 */

#include <tuple>
#include <vector>

#include "brw_fs.h"
#include "brw_cfg.h"

using namespace brw;

namespace {
   /**
    * In-order instruction accounting.
    * @{
    */

   /**
    * Number of in-order hardware instructions contained in this IR
    * instruction.  This determines the increment applied to the RegDist
    * counter calculated for any ordered dependency that crosses this
    * instruction.
    */
   unsigned
   ordered_unit(const fs_inst *inst)
   {
      switch (inst->opcode) {
      case BRW_OPCODE_SYNC:
      case BRW_OPCODE_DO:
      case SHADER_OPCODE_UNDEF:
      case FS_OPCODE_PLACEHOLDER_HALT:
         return 0;
      default:
         /* Note that the following is inaccurate for virtual instructions
          * that expand to more in-order instructions than assumed here, but
          * that can only lead to suboptimal execution ordering, data
          * coherency won't be impacted.  Providing exact RegDist counts for
          * each virtual instruction would allow better ALU performance, but
          * it would require keeping this switch statement in perfect sync
          * with the generator in order to avoid data corruption.  Lesson is
          * (again) don't use virtual instructions if you want optimal
          * scheduling.
          */
         return is_unordered(inst) ? 0 : 1;
      }
   }

   /**
    * Type for an instruction counter that increments for in-order
    * instructions only, arbitrarily denoted 'jp' throughout this lowering
    * pass in order to distinguish it from the regular instruction counter.
    */
   typedef int ordered_address;

   /**
    * Calculate the local ordered_address instruction counter at every
    * instruction of the shader for subsequent constant-time look-up.
    */
   std::vector<ordered_address>
   ordered_inst_addresses(const fs_visitor *shader)
   {
      std::vector<ordered_address> jps;
      ordered_address jp = 0;

      foreach_block_and_inst(block, fs_inst, inst, shader->cfg) {
         jps.push_back(jp);
         jp += ordered_unit(inst);
      }

      return jps;
   }

   /**
    * Synchronization mode required for data manipulated by in-order
    * instructions.
    *
    * Similar to tgl_sbid_mode, but without SET mode.  Defined as a separate
    * enum for additional type safety.  The hardware doesn't provide control
    * over the synchronization mode for RegDist annotations, this is only used
    * internally in this pass in order to optimize out redundant read
    * dependencies where possible.
    */
   enum tgl_regdist_mode {
      TGL_REGDIST_NULL = 0,
      TGL_REGDIST_SRC = 1,
      TGL_REGDIST_DST = 2
   };

   /**
    * Allow bitwise arithmetic of tgl_regdist_mode enums.
    */
   tgl_regdist_mode
   operator|(tgl_regdist_mode x, tgl_regdist_mode y)
   {
      return tgl_regdist_mode(unsigned(x) | unsigned(y));
   }

   tgl_regdist_mode
   operator&(tgl_regdist_mode x, tgl_regdist_mode y)
   {
      return tgl_regdist_mode(unsigned(x) & unsigned(y));
   }

   tgl_regdist_mode &
   operator|=(tgl_regdist_mode &x, tgl_regdist_mode y)
   {
      return x = x | y;
   }

   tgl_regdist_mode &
   operator&=(tgl_regdist_mode &x, tgl_regdist_mode y)
   {
      return x = x & y;
   }

   /** @} */

   /**
    * Representation of an equivalence relation among the set of unsigned
    * integers.
    *
    * Its initial state is the identity relation '~' such that i ~ j if and
    * only if i == j for every pair of unsigned integers i and j.
    */
   struct equivalence_relation {
      /**
       * Return equivalence class index of the specified element.  Effectively
       * this is the numeric value of an arbitrary representative from the
       * equivalence class.
       *
       * Allows the evaluation of the equivalence relation according to the
       * rule that i ~ j if and only if lookup(i) == lookup(j).
       */
      unsigned
      lookup(unsigned i) const
      {
         if (i < is.size() && is[i] != i)
            return lookup(is[i]);
         else
            return i;
      }

      /**
       * Create an array with the results of the lookup() method for
       * constant-time evaluation.
       */
      std::vector<unsigned>
      flatten() const {
         std::vector<unsigned> ids;

         for (const auto i : is)
            ids.push_back(lookup(i));

         return ids;
      }

      /**
       * Mutate the existing equivalence relation minimally by imposing the
       * additional requirement that i ~ j.
       *
       * The algorithm updates the internal representation recursively in
       * order to guarantee transitivity while preserving the previously
       * specified equivalence requirements.
       */
      unsigned
      link(unsigned i, unsigned j)
      {
         const unsigned k = lookup(i);
         assign(i, k);
         assign(j, k);
         return k;
      }

   private:
      /**
       * Assign the representative of \p from to be equivalent to \p to.
       *
       * At the same time the data structure is partially flattened as much as
       * it's possible without increasing the number of recursive calls.
       */
      void
      assign(unsigned from, unsigned to)
      {
         if (from != to) {
            if (from < is.size() && is[from] != from)
               assign(is[from], to);

            for (unsigned i = is.size(); i <= from; i++)
               is.push_back(i);

            is[from] = to;
         }
      }

      std::vector<unsigned> is;
   };

   /**
    * Representation of a data dependency between two instructions in the
    * program.
    * @{
    */
   struct dependency {
      /**
       * No dependency information.
       */
      dependency() : ordered(TGL_REGDIST_NULL), jp(INT_MIN),
                     unordered(TGL_SBID_NULL), id(0) {}

      /**
       * Construct a dependency on the in-order instruction with the provided
       * ordered_address instruction counter.
       */
      dependency(tgl_regdist_mode mode, ordered_address jp) :
         ordered(mode), jp(jp), unordered(TGL_SBID_NULL), id(0) {}

      /**
       * Construct a dependency on the out-of-order instruction with the
       * specified synchronization token.
       */
      dependency(tgl_sbid_mode mode, unsigned id) :
         ordered(TGL_REGDIST_NULL), jp(INT_MIN), unordered(mode), id(id) {}

      /**
       * Synchronization mode of in-order dependency, or zero if no in-order
       * dependency is present.
       */
      tgl_regdist_mode ordered;

      /**
       * Instruction counter of in-order dependency.
       *
       * For a dependency part of a different block in the program, this is
       * relative to the specific control flow path taken between the
       * dependency and the current block: It is the ordered_address such that
       * the difference between it and the ordered_address of the first
       * instruction of the current block is exactly the number of in-order
       * instructions across that control flow path.  It is not guaranteed to
       * be equal to the local ordered_address of the generating instruction
       * [as returned by ordered_inst_addresses()], except for block-local
       * dependencies.
       */
      ordered_address jp;

      /**
       * Synchronization mode of unordered dependency, or zero if no unordered
       * dependency is present.
       */
      tgl_sbid_mode unordered;

      /** Synchronization token of out-of-order dependency. */
      unsigned id;

      /**
       * Trivial in-order dependency that's always satisfied.
       *
       * Note that unlike a default-constructed dependency() which is also
       * trivially satisfied, this is considered to provide dependency
       * information and can be used to clear a previously pending dependency
       * via shadow().
       */
      static const dependency done;

      friend bool
      operator==(const dependency &dep0, const dependency &dep1)
      {
         return dep0.ordered == dep1.ordered &&
                dep0.jp == dep1.jp &&
                dep0.unordered == dep1.unordered &&
                dep0.id == dep1.id;
      }

      friend bool
      operator!=(const dependency &dep0, const dependency &dep1)
      {
         return !(dep0 == dep1);
      }
   };

   const dependency dependency::done = dependency(TGL_REGDIST_SRC, INT_MIN);

   /**
    * Return whether \p dep contains any dependency information.
    */
   bool
   is_valid(const dependency &dep)
   {
      return dep.ordered || dep.unordered;
   }

   /**
    * Combine \p dep0 and \p dep1 into a single dependency object that is only
    * satisfied when both original dependencies are satisfied.  This might
    * involve updating the equivalence relation \p eq in order to make sure
    * that both out-of-order dependencies are assigned the same hardware SBID
    * as synchronization token.
    */
   dependency
   merge(equivalence_relation &eq,
         const dependency &dep0, const dependency &dep1)
   {
      dependency dep;

      if (dep0.ordered || dep1.ordered) {
         dep.ordered = dep0.ordered | dep1.ordered;
         dep.jp = MAX2(dep0.jp, dep1.jp);
      }

      if (dep0.unordered || dep1.unordered) {
         dep.unordered = dep0.unordered | dep1.unordered;
         dep.id = eq.link(dep0.unordered ? dep0.id : dep1.id,
                          dep1.unordered ? dep1.id : dep0.id);
      }

      return dep;
   }

   /**
    * Override dependency information of \p dep0 with that of \p dep1.
    */
   dependency
   shadow(const dependency &dep0, const dependency &dep1)
   {
      return is_valid(dep1) ? dep1 : dep0;
   }

   /**
    * Translate dependency information across the program.
    *
    * This returns a dependency on the same instruction translated to the
    * ordered_address space of a different block.  The correct shift for
    * transporting a dependency across an edge of the CFG is the difference
    * between the local ordered_address of the first instruction of the target
    * block and the local ordered_address of the instruction immediately after
    * the end of the origin block.
    */
   dependency
   transport(dependency dep, int delta)
   {
      if (dep.ordered && dep.jp > INT_MIN)
         dep.jp += delta;

      return dep;
   }

   /**
    * Return simplified dependency removing any synchronization modes not
    * applicable to an instruction reading the same register location.
    */
   dependency
   dependency_for_read(dependency dep)
   {
      dep.ordered &= TGL_REGDIST_DST;
      return dep;
   }

   /**
    * Return simplified dependency removing any synchronization modes not
    * applicable to an instruction \p inst writing the same register location.
    */
   dependency
   dependency_for_write(const fs_inst *inst, dependency dep)
   {
      if (!is_unordered(inst))
         dep.ordered &= TGL_REGDIST_DST;
      return dep;
   }

   /** @} */

   /**
    * Scoreboard representation.  This keeps track of the data dependencies of
    * registers with GRF granularity.
    */
   class scoreboard {
   public:
      /**
       * Look up the most current data dependency for register \p r.
       */
      dependency
      get(const fs_reg &r) const
      {
         if (const dependency *p = const_cast<scoreboard *>(this)->dep(r))
            return *p;
         else
            return dependency();
      }

      /**
       * Specify the most current data dependency for register \p r.
       */
      void
      set(const fs_reg &r, const dependency &d)
      {
         if (dependency *p = dep(r))
            *p = d;
      }

      /**
       * Component-wise merge() of corresponding dependencies from two
       * scoreboard objects.  \sa merge().
       */
      friend scoreboard
      merge(equivalence_relation &eq,
            const scoreboard &sb0, const scoreboard &sb1)
      {
         scoreboard sb;

         for (unsigned i = 0; i < ARRAY_SIZE(sb.grf_deps); i++)
            sb.grf_deps[i] = merge(eq, sb0.grf_deps[i], sb1.grf_deps[i]);

         sb.addr_dep = merge(eq, sb0.addr_dep, sb1.addr_dep);

         for (unsigned i = 0; i < ARRAY_SIZE(sb.accum_deps); i++)
            sb.accum_deps[i] = merge(eq, sb0.accum_deps[i], sb1.accum_deps[i]);

         return sb;
      }

      /**
       * Component-wise shadow() of corresponding dependencies from two
       * scoreboard objects.  \sa shadow().
       */
      friend scoreboard
      shadow(const scoreboard &sb0, const scoreboard &sb1)
      {
         scoreboard sb;

         for (unsigned i = 0; i < ARRAY_SIZE(sb.grf_deps); i++)
            sb.grf_deps[i] = shadow(sb0.grf_deps[i], sb1.grf_deps[i]);

         sb.addr_dep = shadow(sb0.addr_dep, sb1.addr_dep);

         for (unsigned i = 0; i < ARRAY_SIZE(sb.accum_deps); i++)
            sb.accum_deps[i] = shadow(sb0.accum_deps[i], sb1.accum_deps[i]);

         return sb;
      }

      /**
       * Component-wise transport() of dependencies from a scoreboard
       * object.  \sa transport().
       */
      friend scoreboard
      transport(const scoreboard &sb0, int delta)
      {
         scoreboard sb;

         for (unsigned i = 0; i < ARRAY_SIZE(sb.grf_deps); i++)
            sb.grf_deps[i] = transport(sb0.grf_deps[i], delta);

         sb.addr_dep = transport(sb0.addr_dep, delta);

         for (unsigned i = 0; i < ARRAY_SIZE(sb.accum_deps); i++)
            sb.accum_deps[i] = transport(sb0.accum_deps[i], delta);

         return sb;
      }

      friend bool
      operator==(const scoreboard &sb0, const scoreboard &sb1)
      {
         for (unsigned i = 0; i < ARRAY_SIZE(sb0.grf_deps); i++) {
            if (sb0.grf_deps[i] != sb1.grf_deps[i])
               return false;
         }

         if (sb0.addr_dep != sb1.addr_dep)
            return false;

         for (unsigned i = 0; i < ARRAY_SIZE(sb0.accum_deps); i++) {
            if (sb0.accum_deps[i] != sb1.accum_deps[i])
               return false;
         }

         return true;
      }

      friend bool
      operator!=(const scoreboard &sb0, const scoreboard &sb1)
      {
         return !(sb0 == sb1);
      }

   private:
      dependency grf_deps[BRW_MAX_GRF];
      dependency addr_dep;
      dependency accum_deps[10];

      dependency *
      dep(const fs_reg &r)
      {
         const unsigned reg = (r.file == VGRF ? r.nr + r.offset / REG_SIZE :
                               reg_offset(r) / REG_SIZE);

         return (r.file == VGRF || r.file == FIXED_GRF ? &grf_deps[reg] :
                 r.file == MRF ? &grf_deps[GEN7_MRF_HACK_START + reg] :
                 r.file == ARF && reg >= BRW_ARF_ADDRESS &&
                                  reg < BRW_ARF_ACCUMULATOR ? &addr_dep :
                 r.file == ARF && reg >= BRW_ARF_ACCUMULATOR &&
                                  reg < BRW_ARF_FLAG ? &accum_deps[
                                     reg - BRW_ARF_ACCUMULATOR] :
                 NULL);
      }
   };

   /**
    * Dependency list handling.
    * @{
    */

   /**
    * Add dependency \p dep to the list of dependencies of an instruction
    * \p deps.
    */
   void
   add_dependency(const std::vector<unsigned> &ids,
                  std::vector<dependency> &deps, dependency dep)
   {
      if (is_valid(dep)) {
         /* Translate the unordered dependency token first in order to keep
          * the list minimally redundant.
          */
         if (dep.unordered && dep.id < ids.size())
            dep.id = ids[dep.id];

         /* Try to combine the specified dependency with any existing ones. */
         for (auto &dep1 : deps) {
            if (dep.ordered && dep1.ordered) {
               dep1.jp = MAX2(dep1.jp, dep.jp);
               dep1.ordered |= dep.ordered;
               dep.ordered = TGL_REGDIST_NULL;
            }

            if (dep.unordered && dep1.unordered && dep1.id == dep.id) {
               dep1.unordered |= dep.unordered;
               dep.unordered = TGL_SBID_NULL;
            }
         }

         /* Add it to the end of the list if necessary. */
         if (is_valid(dep))
            deps.push_back(dep);
      }
   }

   /**
    * Construct a tgl_swsb annotation encoding any ordered dependencies from
    * the dependency list \p deps of an instruction with ordered_address
    * \p jp.
    */
   tgl_swsb
   ordered_dependency_swsb(const std::vector<dependency> &deps,
                           const ordered_address &jp)
   {
      unsigned min_dist = ~0u;

      for (const auto &dep : deps) {
         if (dep.ordered) {
            const unsigned dist = jp - dep.jp;
            const unsigned max_dist = 10;
            assert(jp > dep.jp);
            if (dist <= max_dist)
               min_dist = MIN3(min_dist, dist, 7);
         }
      }

      return { min_dist == ~0u ? 0 : min_dist };
   }

   /**
    * Return whether the dependency list \p deps of an instruction with
    * ordered_address \p jp has any non-trivial ordered dependencies.
    */
   bool
   find_ordered_dependency(const std::vector<dependency> &deps,
                           const ordered_address &jp)
   {
      return ordered_dependency_swsb(deps, jp).regdist;
   }

   /**
    * Return the full tgl_sbid_mode bitset for the first unordered dependency
    * on the list \p deps that matches the specified tgl_sbid_mode, or zero if
    * no such dependency is present.
    */
   tgl_sbid_mode
   find_unordered_dependency(const std::vector<dependency> &deps,
                             tgl_sbid_mode unordered)
   {
      if (unordered) {
         for (const auto &dep : deps) {
            if (unordered & dep.unordered)
               return dep.unordered;
         }
      }

      return TGL_SBID_NULL;
   }

   /**
    * Return the tgl_sbid_mode bitset of an unordered dependency from the list
    * \p deps that can be represented directly in the SWSB annotation of the
    * instruction without additional SYNC instructions, or zero if no such
    * dependency is present.
    */
   tgl_sbid_mode
   baked_unordered_dependency_mode(const fs_inst *inst,
                                   const std::vector<dependency> &deps,
                                   const ordered_address &jp)
   {
      const bool has_ordered = find_ordered_dependency(deps, jp);

      if (find_unordered_dependency(deps, TGL_SBID_SET))
         return find_unordered_dependency(deps, TGL_SBID_SET);
      else if (has_ordered && is_unordered(inst))
         return TGL_SBID_NULL;
      else if (find_unordered_dependency(deps, TGL_SBID_DST) &&
               (!has_ordered || !is_unordered(inst)))
         return find_unordered_dependency(deps, TGL_SBID_DST);
      else if (!has_ordered)
         return find_unordered_dependency(deps, TGL_SBID_SRC);
      else
         return TGL_SBID_NULL;
   }

   /** @} */

   /**
    * Shader instruction dependency calculation.
    * @{
    */

   /**
    * Update scoreboard object \p sb to account for the execution of
    * instruction \p inst.
    */
   void
   update_inst_scoreboard(const fs_visitor *shader,
                          const std::vector<ordered_address> &jps,
                          const fs_inst *inst, unsigned ip, scoreboard &sb)
   {
      /* Track any source registers that may be fetched asynchronously by this
       * instruction, otherwise clear the dependency in order to avoid
       * subsequent redundant synchronization.
       */
      for (unsigned i = 0; i < inst->sources; i++) {
         const dependency rd_dep =
            inst->is_payload(i) || inst->is_math() ? dependency(TGL_SBID_SRC, ip) :
            ordered_unit(inst) ? dependency(TGL_REGDIST_SRC, jps[ip]) :
            dependency::done;

         for (unsigned j = 0; j < regs_read(inst, i); j++)
            sb.set(byte_offset(inst->src[i], REG_SIZE * j), rd_dep);
      }

      if (is_send(inst) && inst->base_mrf != -1) {
         const dependency rd_dep = dependency(TGL_SBID_SRC, ip);

         for (unsigned j = 0; j < inst->mlen; j++)
            sb.set(brw_uvec_mrf(8, inst->base_mrf + j, 0), rd_dep);
      }

      /* Track any destination registers of this instruction. */
      const dependency wr_dep =
         is_unordered(inst) ? dependency(TGL_SBID_DST, ip) :
         ordered_unit(inst) ? dependency(TGL_REGDIST_DST, jps[ip]) :
         dependency();

      if (is_valid(wr_dep) && inst->dst.file != BAD_FILE &&
          !inst->dst.is_null()) {
         for (unsigned j = 0; j < regs_written(inst); j++)
            sb.set(byte_offset(inst->dst, REG_SIZE * j), wr_dep);
      }
   }

   /**
    * Calculate scoreboard objects locally that represent any pending (and
    * unconditionally resolved) dependencies at the end of each block of the
    * program.
    */
   std::vector<scoreboard>
   gather_block_scoreboards(const fs_visitor *shader,
                            const std::vector<ordered_address> &jps)
   {
      std::vector<scoreboard> sbs(shader->cfg->num_blocks);
      unsigned ip = 0;

      foreach_block_and_inst(block, fs_inst, inst, shader->cfg)
         update_inst_scoreboard(shader, jps, inst, ip++, sbs[block->num]);

      return sbs;
   }

   /**
    * Propagate data dependencies globally through the control flow graph
    * until a fixed point is reached.
    *
    * Calculates the set of dependencies potentially pending at the beginning
    * of each block, and returns it as an array of scoreboard objects.
    */
   std::pair<std::vector<scoreboard>, std::vector<unsigned>>
   propagate_block_scoreboards(const fs_visitor *shader,
                               const std::vector<ordered_address> &jps)
   {
      const std::vector<scoreboard> delta_sbs =
         gather_block_scoreboards(shader, jps);
      std::vector<scoreboard> in_sbs(shader->cfg->num_blocks);
      std::vector<scoreboard> out_sbs(shader->cfg->num_blocks);
      equivalence_relation eq;

      for (bool progress = true; progress;) {
         progress = false;

         foreach_block(block, shader->cfg) {
            const scoreboard sb = shadow(in_sbs[block->num],
                                         delta_sbs[block->num]);

            if (sb != out_sbs[block->num]) {
               foreach_list_typed(bblock_link, child_link, link,
                                  &block->children) {
                  scoreboard &in_sb = in_sbs[child_link->block->num];
                  const int delta =
                     jps[child_link->block->start_ip] - jps[block->end_ip]
                     - ordered_unit(static_cast<const fs_inst *>(block->end()));

                  in_sb = merge(eq, in_sb, transport(sb, delta));
               }

               out_sbs[block->num] = sb;
               progress = true;
            }
         }
      }

      return { std::move(in_sbs), eq.flatten() };
   }

   /**
    * Return the list of potential dependencies of each instruction in the
    * shader based on the result of global dependency analysis.
    */
   std::vector<std::vector<dependency>>
   gather_inst_dependencies(const fs_visitor *shader,
                            const std::vector<ordered_address> &jps)
   {
      std::vector<scoreboard> sbs;
      std::vector<unsigned> ids;
      std::vector<std::vector<dependency>> deps;
      unsigned ip = 0;

      std::tie(sbs, ids) = propagate_block_scoreboards(shader, jps);

      foreach_block_and_inst(block, fs_inst, inst, shader->cfg) {
         scoreboard &sb = sbs[block->num];
         std::vector<dependency> inst_deps;

         for (unsigned i = 0; i < inst->sources; i++) {
            for (unsigned j = 0; j < regs_read(inst, i); j++)
               add_dependency(ids, inst_deps, dependency_for_read(
                  sb.get(byte_offset(inst->src[i], REG_SIZE * j))));
         }

         if (is_send(inst) && inst->base_mrf != -1) {
            for (unsigned j = 0; j < inst->mlen; j++)
               add_dependency(ids, inst_deps, dependency_for_read(
                  sb.get(brw_uvec_mrf(8, inst->base_mrf + j, 0))));
         }

         if (is_unordered(inst))
            add_dependency(ids, inst_deps, dependency(TGL_SBID_SET, ip));

         if (!inst->no_dd_check) {
            if (inst->dst.file != BAD_FILE && !inst->dst.is_null()) {
               for (unsigned j = 0; j < regs_written(inst); j++) {
                  add_dependency(ids, inst_deps, dependency_for_write(inst,
                     sb.get(byte_offset(inst->dst, REG_SIZE * j))));
               }
            }

            if (is_send(inst) && inst->base_mrf != -1) {
               for (int j = 0; j < shader->implied_mrf_writes(inst); j++)
                  add_dependency(ids, inst_deps, dependency_for_write(inst,
                     sb.get(brw_uvec_mrf(8, inst->base_mrf + j, 0))));
            }
         }

         deps.push_back(inst_deps);
         update_inst_scoreboard(shader, jps, inst, ip, sb);
         ip++;
      }

      return deps;
   }

   /** @} */

   /**
    * Allocate SBID tokens to track the execution of every out-of-order
    * instruction of the shader.
    */
   std::vector<std::vector<dependency>>
   allocate_inst_dependencies(const fs_visitor *shader,
                              const std::vector<std::vector<dependency>> &deps0)
   {
      /* XXX - Use bin-packing algorithm to assign hardware SBIDs optimally in
       *       shaders with a large number of SEND messages.
       */
      std::vector<std::vector<dependency>> deps1;
      std::vector<unsigned> ids(deps0.size(), ~0u);
      unsigned next_id = 0;

      for (const auto &inst_deps0 : deps0) {
         std::vector<dependency> inst_deps1;

         for (const auto &dep : inst_deps0) {
            if (dep.unordered && ids[dep.id] == ~0u)
               ids[dep.id] = (next_id++) & 0xf;

            add_dependency(ids, inst_deps1, dep);
         }

         deps1.push_back(inst_deps1);
      }

      return deps1;
   }

   /**
    * Emit dependency information provided by \p deps into the shader,
    * inserting additional SYNC instructions for dependencies that can't be
    * represented directly by annotating existing instructions.
    */
   void
   emit_inst_dependencies(fs_visitor *shader,
                          const std::vector<ordered_address> &jps,
                          const std::vector<std::vector<dependency>> &deps)
   {
      unsigned ip = 0;

      foreach_block_and_inst_safe(block, fs_inst, inst, shader->cfg) {
         tgl_swsb swsb = ordered_dependency_swsb(deps[ip], jps[ip]);
         const tgl_sbid_mode unordered_mode =
            baked_unordered_dependency_mode(inst, deps[ip], jps[ip]);

         for (const auto &dep : deps[ip]) {
            if (dep.unordered) {
               if (unordered_mode == dep.unordered && !swsb.mode) {
                  /* Bake unordered dependency into the instruction's SWSB if
                   * possible.
                   */
                  swsb.sbid = dep.id;
                  swsb.mode = dep.unordered;
               } else {
                  /* Emit dependency into the SWSB of an extra SYNC
                   * instruction.
                   */
                  const fs_builder ibld = fs_builder(shader, block, inst)
                     .exec_all().group(1, 0);
                  fs_inst *sync = ibld.emit(BRW_OPCODE_SYNC, ibld.null_reg_ud(),
                                            brw_imm_ud(TGL_SYNC_NOP));
                  sync->sched.sbid = dep.id;
                  sync->sched.mode = dep.unordered;
                  assert(!(sync->sched.mode & TGL_SBID_SET));
               }
            }
         }

         /* Update the IR. */
         inst->sched = swsb;
         inst->no_dd_check = inst->no_dd_clear = false;
         ip++;
      }
   }
}

bool
fs_visitor::lower_scoreboard()
{
   if (devinfo->gen >= 12) {
      const std::vector<ordered_address> jps = ordered_inst_addresses(this);
      emit_inst_dependencies(this, jps,
         allocate_inst_dependencies(this,
            gather_inst_dependencies(this, jps)));
   }

   return true;
}
