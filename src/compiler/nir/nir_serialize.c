/*
 * Copyright © 2017 Connor Abbott
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

#include "nir_serialize.h"
#include "nir_control_flow.h"
#include "util/u_dynarray.h"
#include "util/u_math.h"

#define NIR_SERIALIZE_FUNC_HAS_IMPL ((void *)(intptr_t)1)
#define MAX_OBJECT_IDS (1 << 20)

typedef struct {
   size_t blob_offset;
   nir_ssa_def *src;
   nir_block *block;
} write_phi_fixup;

typedef struct {
   const nir_shader *nir;

   struct blob *blob;

   /* maps pointer to index */
   struct hash_table *remap_table;

   /* the next index to assign to a NIR in-memory object */
   uint32_t next_idx;

   /* Array of write_phi_fixup structs representing phi sources that need to
    * be resolved in the second pass.
    */
   struct util_dynarray phi_fixups;

   /* Don't write optional data such as variable names. */
   bool strip;
} write_ctx;

typedef struct {
   nir_shader *nir;

   struct blob_reader *blob;

   /* the next index to assign to a NIR in-memory object */
   uint32_t next_idx;

   /* The length of the index -> object table */
   uint32_t idx_table_len;

   /* map from index to deserialized pointer */
   void **idx_table;

   /* List of phi sources. */
   struct list_head phi_srcs;

} read_ctx;

static void
write_add_object(write_ctx *ctx, const void *obj)
{
   uint32_t index = ctx->next_idx++;
   assert(index != MAX_OBJECT_IDS);
   _mesa_hash_table_insert(ctx->remap_table, obj, (void *)(uintptr_t) index);
}

static uint32_t
write_lookup_object(write_ctx *ctx, const void *obj)
{
   struct hash_entry *entry = _mesa_hash_table_search(ctx->remap_table, obj);
   assert(entry);
   return (uint32_t)(uintptr_t) entry->data;
}

static void
write_object(write_ctx *ctx, const void *obj)
{
   blob_write_uint32(ctx->blob, write_lookup_object(ctx, obj));
}

static void
read_add_object(read_ctx *ctx, void *obj)
{
   assert(ctx->next_idx < ctx->idx_table_len);
   ctx->idx_table[ctx->next_idx++] = obj;
}

static void *
read_lookup_object(read_ctx *ctx, uint32_t idx)
{
   assert(idx < ctx->idx_table_len);
   return ctx->idx_table[idx];
}

static void *
read_object(read_ctx *ctx)
{
   return read_lookup_object(ctx, blob_read_uint32(ctx->blob));
}

static uint32_t
encode_bit_size_3bits(uint8_t bit_size)
{
   /* Encode values of 0, 1, 2, 4, 8, 16, 32, 64 in 3 bits. */
   assert(bit_size <= 64 && util_is_power_of_two_or_zero(bit_size));
   if (bit_size)
      return util_logbase2(bit_size) + 1;
   return 0;
}

static uint8_t
decode_bit_size_3bits(uint8_t bit_size)
{
   if (bit_size)
      return 1 << (bit_size - 1);
   return 0;
}

static uint8_t
encode_num_components_in_3bits(uint8_t num_components)
{
   if (num_components <= 4)
      return num_components;
   if (num_components == 8)
      return 5;
   if (num_components == 16)
      return 6;

   unreachable("invalid number in num_components");
   return 0;
}

static uint8_t
decode_num_components_in_3bits(uint8_t value)
{
   if (value <= 4)
      return value;
   if (value == 5)
      return 8;
   if (value == 6)
      return 16;

   unreachable("invalid num_components encoding");
   return 0;
}

static void
write_constant(write_ctx *ctx, const nir_constant *c)
{
   blob_write_bytes(ctx->blob, c->values, sizeof(c->values));
   blob_write_uint32(ctx->blob, c->num_elements);
   for (unsigned i = 0; i < c->num_elements; i++)
      write_constant(ctx, c->elements[i]);
}

static nir_constant *
read_constant(read_ctx *ctx, nir_variable *nvar)
{
   nir_constant *c = ralloc(nvar, nir_constant);

   blob_copy_bytes(ctx->blob, (uint8_t *)c->values, sizeof(c->values));
   c->num_elements = blob_read_uint32(ctx->blob);
   c->elements = ralloc_array(nvar, nir_constant *, c->num_elements);
   for (unsigned i = 0; i < c->num_elements; i++)
      c->elements[i] = read_constant(ctx, nvar);

   return c;
}

union packed_var {
   uint32_t u32;
   struct {
      unsigned has_name:1;
      unsigned has_constant_initializer:1;
      unsigned has_interface_type:1;
      unsigned num_state_slots:13;
      unsigned num_members:16;
   } u;
};

static void
write_variable(write_ctx *ctx, const nir_variable *var)
{
   write_add_object(ctx, var);
   encode_type_to_blob(ctx->blob, var->type);

   assert(var->num_state_slots < (1 << 13));
   assert(var->num_members < (1 << 16));

   STATIC_ASSERT(sizeof(union packed_var) == 4);
   union packed_var flags;
   flags.u32 = 0;

   flags.u.has_name = !ctx->strip && var->name;
   flags.u.has_constant_initializer = !!(var->constant_initializer);
   flags.u.has_interface_type = !!(var->interface_type);
   flags.u.num_state_slots = var->num_state_slots;
   flags.u.num_members = var->num_members;

   blob_write_uint32(ctx->blob, flags.u32);

   if (flags.u.has_name)
      blob_write_string(ctx->blob, var->name);

   struct nir_variable_data data = var->data;

   /* When stripping, we expect that the location is no longer needed,
    * which is typically after shaders are linked.
    */
   if (ctx->strip &&
       data.mode != nir_var_shader_in &&
       data.mode != nir_var_shader_out)
      data.location = 0;

   blob_write_bytes(ctx->blob, &data, sizeof(data));

   for (unsigned i = 0; i < var->num_state_slots; i++) {
      blob_write_bytes(ctx->blob, &var->state_slots[i],
                       sizeof(var->state_slots[i]));
   }
   if (var->constant_initializer)
      write_constant(ctx, var->constant_initializer);
   if (var->interface_type)
      encode_type_to_blob(ctx->blob, var->interface_type);
   if (var->num_members > 0) {
      blob_write_bytes(ctx->blob, (uint8_t *) var->members,
                       var->num_members * sizeof(*var->members));
   }
}

static nir_variable *
read_variable(read_ctx *ctx)
{
   nir_variable *var = rzalloc(ctx->nir, nir_variable);
   read_add_object(ctx, var);

   var->type = decode_type_from_blob(ctx->blob);

   union packed_var flags;
   flags.u32 = blob_read_uint32(ctx->blob);

   if (flags.u.has_name) {
      const char *name = blob_read_string(ctx->blob);
      var->name = ralloc_strdup(var, name);
   } else {
      var->name = NULL;
   }
   blob_copy_bytes(ctx->blob, (uint8_t *) &var->data, sizeof(var->data));
   var->num_state_slots = flags.u.num_state_slots;
   if (var->num_state_slots != 0) {
      var->state_slots = ralloc_array(var, nir_state_slot,
                                      var->num_state_slots);
      for (unsigned i = 0; i < var->num_state_slots; i++) {
         blob_copy_bytes(ctx->blob, &var->state_slots[i],
                         sizeof(var->state_slots[i]));
      }
   }
   if (flags.u.has_constant_initializer)
      var->constant_initializer = read_constant(ctx, var);
   else
      var->constant_initializer = NULL;
   if (flags.u.has_interface_type)
      var->interface_type = decode_type_from_blob(ctx->blob);
   else
      var->interface_type = NULL;
   var->num_members = flags.u.num_members;
   if (var->num_members > 0) {
      var->members = ralloc_array(var, struct nir_variable_data,
                                  var->num_members);
      blob_copy_bytes(ctx->blob, (uint8_t *) var->members,
                      var->num_members * sizeof(*var->members));
   }

   return var;
}

static void
write_var_list(write_ctx *ctx, const struct exec_list *src)
{
   blob_write_uint32(ctx->blob, exec_list_length(src));
   foreach_list_typed(nir_variable, var, node, src) {
      write_variable(ctx, var);
   }
}

static void
read_var_list(read_ctx *ctx, struct exec_list *dst)
{
   exec_list_make_empty(dst);
   unsigned num_vars = blob_read_uint32(ctx->blob);
   for (unsigned i = 0; i < num_vars; i++) {
      nir_variable *var = read_variable(ctx);
      exec_list_push_tail(dst, &var->node);
   }
}

static void
write_register(write_ctx *ctx, const nir_register *reg)
{
   write_add_object(ctx, reg);
   blob_write_uint32(ctx->blob, reg->num_components);
   blob_write_uint32(ctx->blob, reg->bit_size);
   blob_write_uint32(ctx->blob, reg->num_array_elems);
   blob_write_uint32(ctx->blob, reg->index);
   blob_write_uint32(ctx->blob, !ctx->strip && reg->name);
   if (!ctx->strip && reg->name)
      blob_write_string(ctx->blob, reg->name);
}

static nir_register *
read_register(read_ctx *ctx)
{
   nir_register *reg = ralloc(ctx->nir, nir_register);
   read_add_object(ctx, reg);
   reg->num_components = blob_read_uint32(ctx->blob);
   reg->bit_size = blob_read_uint32(ctx->blob);
   reg->num_array_elems = blob_read_uint32(ctx->blob);
   reg->index = blob_read_uint32(ctx->blob);
   bool has_name = blob_read_uint32(ctx->blob);
   if (has_name) {
      const char *name = blob_read_string(ctx->blob);
      reg->name = ralloc_strdup(reg, name);
   } else {
      reg->name = NULL;
   }

   list_inithead(&reg->uses);
   list_inithead(&reg->defs);
   list_inithead(&reg->if_uses);

   return reg;
}

static void
write_reg_list(write_ctx *ctx, const struct exec_list *src)
{
   blob_write_uint32(ctx->blob, exec_list_length(src));
   foreach_list_typed(nir_register, reg, node, src)
      write_register(ctx, reg);
}

static void
read_reg_list(read_ctx *ctx, struct exec_list *dst)
{
   exec_list_make_empty(dst);
   unsigned num_regs = blob_read_uint32(ctx->blob);
   for (unsigned i = 0; i < num_regs; i++) {
      nir_register *reg = read_register(ctx);
      exec_list_push_tail(dst, &reg->node);
   }
}

union packed_src {
   uint32_t u32;
   struct {
      unsigned is_ssa:1;   /* <-- Header */
      unsigned is_indirect:1;
      unsigned object_idx:20;
      unsigned _footer:10; /* <-- Footer */
   } any;
   struct {
      unsigned _header:22; /* <-- Header */
      unsigned negate:1;   /* <-- Footer */
      unsigned abs:1;
      unsigned swizzle_x:2;
      unsigned swizzle_y:2;
      unsigned swizzle_z:2;
      unsigned swizzle_w:2;
   } alu;
   struct {
      unsigned _header:22; /* <-- Header */
      unsigned src_type:5; /* <-- Footer */
      unsigned _pad:5;
   } tex;
};

static void
write_src_full(write_ctx *ctx, const nir_src *src, union packed_src header)
{
   /* Since sources are very frequent, we try to save some space when storing
    * them. In particular, we store whether the source is a register and
    * whether the register has an indirect index in the low two bits. We can
    * assume that the high two bits of the index are zero, since otherwise our
    * address space would've been exhausted allocating the remap table!
    */
   header.any.is_ssa = src->is_ssa;
   if (src->is_ssa) {
      header.any.object_idx = write_lookup_object(ctx, src->ssa);
      blob_write_uint32(ctx->blob, header.u32);
   } else {
      header.any.object_idx = write_lookup_object(ctx, src->reg.reg);
      header.any.is_indirect = !!src->reg.indirect;
      blob_write_uint32(ctx->blob, header.u32);
      blob_write_uint32(ctx->blob, src->reg.base_offset);
      if (src->reg.indirect) {
         union packed_src header = {0};
         write_src_full(ctx, src->reg.indirect, header);
      }
   }
}

static void
write_src(write_ctx *ctx, const nir_src *src)
{
   union packed_src header = {0};
   write_src_full(ctx, src, header);
}

static union packed_src
read_src(read_ctx *ctx, nir_src *src, void *mem_ctx)
{
   STATIC_ASSERT(sizeof(union packed_src) == 4);
   union packed_src header;
   header.u32 = blob_read_uint32(ctx->blob);

   src->is_ssa = header.any.is_ssa;
   if (src->is_ssa) {
      src->ssa = read_lookup_object(ctx, header.any.object_idx);
   } else {
      src->reg.reg = read_lookup_object(ctx, header.any.object_idx);
      src->reg.base_offset = blob_read_uint32(ctx->blob);
      if (header.any.is_indirect) {
         src->reg.indirect = ralloc(mem_ctx, nir_src);
         read_src(ctx, src->reg.indirect, mem_ctx);
      } else {
         src->reg.indirect = NULL;
      }
   }
   return header;
}

union packed_dest {
   uint8_t u8;
   struct {
      uint8_t is_ssa:1;
      uint8_t has_name:1;
      uint8_t num_components:3;
      uint8_t bit_size:3;
   } ssa;
   struct {
      uint8_t is_ssa:1;
      uint8_t is_indirect:1;
      uint8_t _pad:6;
   } reg;
};

union packed_instr {
   uint32_t u32;
   struct {
      unsigned instr_type:4; /* always present */
      unsigned _pad:20;
      unsigned dest:8;       /* always last */
   } any;
   struct {
      unsigned instr_type:4;
      unsigned exact:1;
      unsigned no_signed_wrap:1;
      unsigned no_unsigned_wrap:1;
      unsigned saturate:1;
      unsigned writemask:4;
      unsigned op:9;
      unsigned _pad:3;
      unsigned dest:8;
   } alu;
   struct {
      unsigned instr_type:4;
      unsigned deref_type:3;
      unsigned mode:10;
      unsigned _pad:7;
      unsigned dest:8;
   } deref;
   struct {
      unsigned instr_type:4;
      unsigned intrinsic:9;
      unsigned num_components:3;
      unsigned _pad:8;
      unsigned dest:8;
   } intrinsic;
   struct {
      unsigned instr_type:4;
      unsigned last_component:4;
      unsigned bit_size:3;
      unsigned _pad:21;
   } load_const;
   struct {
      unsigned instr_type:4;
      unsigned last_component:4;
      unsigned bit_size:3;
      unsigned _pad:21;
   } undef;
   struct {
      unsigned instr_type:4;
      unsigned num_srcs:4;
      unsigned op:4;
      unsigned texture_array_size:12;
      unsigned dest:8;
   } tex;
   struct {
      unsigned instr_type:4;
      unsigned num_srcs:20;
      unsigned dest:8;
   } phi;
   struct {
      unsigned instr_type:4;
      unsigned type:2;
      unsigned _pad:26;
   } jump;
};

/* Write "lo24" as low 24 bits in the first uint32. */
static void
write_dest(write_ctx *ctx, const nir_dest *dst, union packed_instr header)
{
   STATIC_ASSERT(sizeof(union packed_dest) == 1);
   union packed_dest dest;
   dest.u8 = 0;

   dest.ssa.is_ssa = dst->is_ssa;
   if (dst->is_ssa) {
      dest.ssa.has_name = !ctx->strip && dst->ssa.name;
      dest.ssa.num_components =
         encode_num_components_in_3bits(dst->ssa.num_components);
      dest.ssa.bit_size = encode_bit_size_3bits(dst->ssa.bit_size);
   } else {
      dest.reg.is_indirect = !!(dst->reg.indirect);
   }

   header.any.dest = dest.u8;
   blob_write_uint32(ctx->blob, header.u32);

   if (dst->is_ssa) {
      write_add_object(ctx, &dst->ssa);
      if (dest.ssa.has_name)
         blob_write_string(ctx->blob, dst->ssa.name);
   } else {
      blob_write_uint32(ctx->blob, write_lookup_object(ctx, dst->reg.reg));
      blob_write_uint32(ctx->blob, dst->reg.base_offset);
      if (dst->reg.indirect)
         write_src(ctx, dst->reg.indirect);
   }
}

static void
read_dest(read_ctx *ctx, nir_dest *dst, nir_instr *instr,
          union packed_instr header)
{
   union packed_dest dest;
   dest.u8 = header.any.dest;

   if (dest.ssa.is_ssa) {
      unsigned bit_size = decode_bit_size_3bits(dest.ssa.bit_size);
      unsigned num_components =
         decode_num_components_in_3bits(dest.ssa.num_components);
      char *name = dest.ssa.has_name ? blob_read_string(ctx->blob) : NULL;
      nir_ssa_dest_init(instr, dst, num_components, bit_size, name);
      read_add_object(ctx, &dst->ssa);
   } else {
      dst->reg.reg = read_object(ctx);
      dst->reg.base_offset = blob_read_uint32(ctx->blob);
      if (dest.reg.is_indirect) {
         dst->reg.indirect = ralloc(instr, nir_src);
         read_src(ctx, dst->reg.indirect, instr);
      }
   }
}

static void
write_alu(write_ctx *ctx, const nir_alu_instr *alu)
{
   /* 9 bits for nir_op */
   STATIC_ASSERT(nir_num_opcodes <= 512);
   union packed_instr header;
   header.u32 = 0;

   header.alu.instr_type = alu->instr.type;
   header.alu.exact = alu->exact;
   header.alu.no_signed_wrap = alu->no_signed_wrap;
   header.alu.no_unsigned_wrap = alu->no_unsigned_wrap;
   header.alu.saturate = alu->dest.saturate;
   header.alu.writemask = alu->dest.write_mask;
   header.alu.op = alu->op;

   write_dest(ctx, &alu->dest.dest, header);

   for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
      union packed_src src;
      src.u32 = 0;

      src.alu.negate = alu->src[i].negate;
      src.alu.abs = alu->src[i].abs;
      src.alu.swizzle_x = alu->src[i].swizzle[0];
      src.alu.swizzle_y = alu->src[i].swizzle[1];
      src.alu.swizzle_z = alu->src[i].swizzle[2];
      src.alu.swizzle_w = alu->src[i].swizzle[3];

      write_src_full(ctx, &alu->src[i].src, src);
   }
}

static nir_alu_instr *
read_alu(read_ctx *ctx, union packed_instr header)
{
   nir_alu_instr *alu = nir_alu_instr_create(ctx->nir, header.alu.op);

   alu->exact = header.alu.exact;
   alu->no_signed_wrap = header.alu.no_signed_wrap;
   alu->no_unsigned_wrap = header.alu.no_unsigned_wrap;
   alu->dest.saturate = header.alu.saturate;
   alu->dest.write_mask = header.alu.writemask;

   read_dest(ctx, &alu->dest.dest, &alu->instr, header);

   for (unsigned i = 0; i < nir_op_infos[header.alu.op].num_inputs; i++) {
      union packed_src src = read_src(ctx, &alu->src[i].src, &alu->instr);

      alu->src[i].negate = src.alu.negate;
      alu->src[i].abs = src.alu.abs;
      alu->src[i].swizzle[0] = src.alu.swizzle_x;
      alu->src[i].swizzle[1] = src.alu.swizzle_y;
      alu->src[i].swizzle[2] = src.alu.swizzle_z;
      alu->src[i].swizzle[3] = src.alu.swizzle_w;
   }

   return alu;
}

static void
write_deref(write_ctx *ctx, const nir_deref_instr *deref)
{
   assert(deref->deref_type < 8);
   assert(deref->mode < (1 << 10));

   union packed_instr header;
   header.u32 = 0;

   header.deref.instr_type = deref->instr.type;
   header.deref.deref_type = deref->deref_type;
   header.deref.mode = deref->mode;

   write_dest(ctx, &deref->dest, header);
   encode_type_to_blob(ctx->blob, deref->type);

   if (deref->deref_type == nir_deref_type_var) {
      write_object(ctx, deref->var);
      return;
   }

   write_src(ctx, &deref->parent);

   switch (deref->deref_type) {
   case nir_deref_type_struct:
      blob_write_uint32(ctx->blob, deref->strct.index);
      break;

   case nir_deref_type_array:
   case nir_deref_type_ptr_as_array:
      write_src(ctx, &deref->arr.index);
      break;

   case nir_deref_type_cast:
      blob_write_uint32(ctx->blob, deref->cast.ptr_stride);
      break;

   case nir_deref_type_array_wildcard:
      /* Nothing to do */
      break;

   default:
      unreachable("Invalid deref type");
   }
}

static nir_deref_instr *
read_deref(read_ctx *ctx, union packed_instr header)
{
   nir_deref_type deref_type = header.deref.deref_type;
   nir_deref_instr *deref = nir_deref_instr_create(ctx->nir, deref_type);

   read_dest(ctx, &deref->dest, &deref->instr, header);

   deref->mode = header.deref.mode;
   deref->type = decode_type_from_blob(ctx->blob);

   if (deref_type == nir_deref_type_var) {
      deref->var = read_object(ctx);
      return deref;
   }

   read_src(ctx, &deref->parent, &deref->instr);

   switch (deref->deref_type) {
   case nir_deref_type_struct:
      deref->strct.index = blob_read_uint32(ctx->blob);
      break;

   case nir_deref_type_array:
   case nir_deref_type_ptr_as_array:
      read_src(ctx, &deref->arr.index, &deref->instr);
      break;

   case nir_deref_type_cast:
      deref->cast.ptr_stride = blob_read_uint32(ctx->blob);
      break;

   case nir_deref_type_array_wildcard:
      /* Nothing to do */
      break;

   default:
      unreachable("Invalid deref type");
   }

   return deref;
}

static void
write_intrinsic(write_ctx *ctx, const nir_intrinsic_instr *intrin)
{
   /* 9 bits for nir_intrinsic_op */
   STATIC_ASSERT(nir_num_intrinsics <= 512);
   unsigned num_srcs = nir_intrinsic_infos[intrin->intrinsic].num_srcs;
   unsigned num_indices = nir_intrinsic_infos[intrin->intrinsic].num_indices;
   assert(intrin->intrinsic < 512);

   union packed_instr header;
   header.u32 = 0;

   header.intrinsic.instr_type = intrin->instr.type;
   header.intrinsic.intrinsic = intrin->intrinsic;
   header.intrinsic.num_components =
      encode_num_components_in_3bits(intrin->num_components);

   if (nir_intrinsic_infos[intrin->intrinsic].has_dest)
      write_dest(ctx, &intrin->dest, header);
   else
      blob_write_uint32(ctx->blob, header.u32);

   for (unsigned i = 0; i < num_srcs; i++)
      write_src(ctx, &intrin->src[i]);

   for (unsigned i = 0; i < num_indices; i++)
      blob_write_uint32(ctx->blob, intrin->const_index[i]);
}

static nir_intrinsic_instr *
read_intrinsic(read_ctx *ctx, union packed_instr header)
{
   nir_intrinsic_op op = header.intrinsic.intrinsic;
   nir_intrinsic_instr *intrin = nir_intrinsic_instr_create(ctx->nir, op);

   unsigned num_srcs = nir_intrinsic_infos[op].num_srcs;
   unsigned num_indices = nir_intrinsic_infos[op].num_indices;

   intrin->num_components =
      decode_num_components_in_3bits(header.intrinsic.num_components);

   if (nir_intrinsic_infos[op].has_dest)
      read_dest(ctx, &intrin->dest, &intrin->instr, header);

   for (unsigned i = 0; i < num_srcs; i++)
      read_src(ctx, &intrin->src[i], &intrin->instr);

   for (unsigned i = 0; i < num_indices; i++)
      intrin->const_index[i] = blob_read_uint32(ctx->blob);

   return intrin;
}

static void
write_load_const(write_ctx *ctx, const nir_load_const_instr *lc)
{
   assert(lc->def.num_components >= 1 && lc->def.num_components <= 16);
   union packed_instr header;
   header.u32 = 0;

   header.load_const.instr_type = lc->instr.type;
   header.load_const.last_component = lc->def.num_components - 1;
   header.load_const.bit_size = encode_bit_size_3bits(lc->def.bit_size);

   blob_write_uint32(ctx->blob, header.u32);
   blob_write_bytes(ctx->blob, lc->value, sizeof(*lc->value) * lc->def.num_components);
   write_add_object(ctx, &lc->def);
}

static nir_load_const_instr *
read_load_const(read_ctx *ctx, union packed_instr header)
{
   nir_load_const_instr *lc =
      nir_load_const_instr_create(ctx->nir, header.load_const.last_component + 1,
                                  decode_bit_size_3bits(header.load_const.bit_size));

   blob_copy_bytes(ctx->blob, lc->value, sizeof(*lc->value) * lc->def.num_components);
   read_add_object(ctx, &lc->def);
   return lc;
}

static void
write_ssa_undef(write_ctx *ctx, const nir_ssa_undef_instr *undef)
{
   assert(undef->def.num_components >= 1 && undef->def.num_components <= 16);

   union packed_instr header;
   header.u32 = 0;

   header.undef.instr_type = undef->instr.type;
   header.undef.last_component = undef->def.num_components - 1;
   header.undef.bit_size = encode_bit_size_3bits(undef->def.bit_size);

   blob_write_uint32(ctx->blob, header.u32);
   write_add_object(ctx, &undef->def);
}

static nir_ssa_undef_instr *
read_ssa_undef(read_ctx *ctx, union packed_instr header)
{
   nir_ssa_undef_instr *undef =
      nir_ssa_undef_instr_create(ctx->nir, header.undef.last_component + 1,
                                 decode_bit_size_3bits(header.undef.bit_size));

   read_add_object(ctx, &undef->def);
   return undef;
}

union packed_tex_data {
   uint32_t u32;
   struct {
      enum glsl_sampler_dim sampler_dim:4;
      nir_alu_type dest_type:8;
      unsigned coord_components:3;
      unsigned is_array:1;
      unsigned is_shadow:1;
      unsigned is_new_style_shadow:1;
      unsigned component:2;
      unsigned unused:10; /* Mark unused for valgrind. */
   } u;
};

static void
write_tex(write_ctx *ctx, const nir_tex_instr *tex)
{
   assert(tex->num_srcs < 16);
   assert(tex->op < 16);
   assert(tex->texture_array_size < 1024);

   union packed_instr header;
   header.u32 = 0;

   header.tex.instr_type = tex->instr.type;
   header.tex.num_srcs = tex->num_srcs;
   header.tex.op = tex->op;
   header.tex.texture_array_size = tex->texture_array_size;

   write_dest(ctx, &tex->dest, header);

   blob_write_uint32(ctx->blob, tex->texture_index);
   blob_write_uint32(ctx->blob, tex->sampler_index);
   if (tex->op == nir_texop_tg4)
      blob_write_bytes(ctx->blob, tex->tg4_offsets, sizeof(tex->tg4_offsets));

   STATIC_ASSERT(sizeof(union packed_tex_data) == sizeof(uint32_t));
   union packed_tex_data packed = {
      .u.sampler_dim = tex->sampler_dim,
      .u.dest_type = tex->dest_type,
      .u.coord_components = tex->coord_components,
      .u.is_array = tex->is_array,
      .u.is_shadow = tex->is_shadow,
      .u.is_new_style_shadow = tex->is_new_style_shadow,
      .u.component = tex->component,
   };
   blob_write_uint32(ctx->blob, packed.u32);

   for (unsigned i = 0; i < tex->num_srcs; i++) {
      union packed_src src;
      src.u32 = 0;
      src.tex.src_type = tex->src[i].src_type;
      write_src_full(ctx, &tex->src[i].src, src);
   }
}

static nir_tex_instr *
read_tex(read_ctx *ctx, union packed_instr header)
{
   nir_tex_instr *tex = nir_tex_instr_create(ctx->nir, header.tex.num_srcs);

   read_dest(ctx, &tex->dest, &tex->instr, header);

   tex->op = header.tex.op;
   tex->texture_index = blob_read_uint32(ctx->blob);
   tex->texture_array_size = header.tex.texture_array_size;
   tex->sampler_index = blob_read_uint32(ctx->blob);
   if (tex->op == nir_texop_tg4)
      blob_copy_bytes(ctx->blob, tex->tg4_offsets, sizeof(tex->tg4_offsets));

   union packed_tex_data packed;
   packed.u32 = blob_read_uint32(ctx->blob);
   tex->sampler_dim = packed.u.sampler_dim;
   tex->dest_type = packed.u.dest_type;
   tex->coord_components = packed.u.coord_components;
   tex->is_array = packed.u.is_array;
   tex->is_shadow = packed.u.is_shadow;
   tex->is_new_style_shadow = packed.u.is_new_style_shadow;
   tex->component = packed.u.component;

   for (unsigned i = 0; i < tex->num_srcs; i++) {
      union packed_src src = read_src(ctx, &tex->src[i].src, &tex->instr);
      tex->src[i].src_type = src.tex.src_type;
   }

   return tex;
}

static void
write_phi(write_ctx *ctx, const nir_phi_instr *phi)
{
   union packed_instr header;
   header.u32 = 0;

   header.phi.instr_type = phi->instr.type;
   header.phi.num_srcs = exec_list_length(&phi->srcs);

   /* Phi nodes are special, since they may reference SSA definitions and
    * basic blocks that don't exist yet. We leave two empty uint32_t's here,
    * and then store enough information so that a later fixup pass can fill
    * them in correctly.
    */
   write_dest(ctx, &phi->dest, header);

   nir_foreach_phi_src(src, phi) {
      assert(src->src.is_ssa);
      size_t blob_offset = blob_reserve_uint32(ctx->blob);
      ASSERTED size_t blob_offset2 = blob_reserve_uint32(ctx->blob);
      assert(blob_offset + sizeof(uint32_t) == blob_offset2);
      write_phi_fixup fixup = {
         .blob_offset = blob_offset,
         .src = src->src.ssa,
         .block = src->pred,
      };
      util_dynarray_append(&ctx->phi_fixups, write_phi_fixup, fixup);
   }
}

static void
write_fixup_phis(write_ctx *ctx)
{
   util_dynarray_foreach(&ctx->phi_fixups, write_phi_fixup, fixup) {
      uint32_t *blob_ptr = (uint32_t *)(ctx->blob->data + fixup->blob_offset);
      blob_ptr[0] = write_lookup_object(ctx, fixup->src);
      blob_ptr[1] = write_lookup_object(ctx, fixup->block);
   }

   util_dynarray_clear(&ctx->phi_fixups);
}

static nir_phi_instr *
read_phi(read_ctx *ctx, nir_block *blk, union packed_instr header)
{
   nir_phi_instr *phi = nir_phi_instr_create(ctx->nir);

   read_dest(ctx, &phi->dest, &phi->instr, header);

   /* For similar reasons as before, we just store the index directly into the
    * pointer, and let a later pass resolve the phi sources.
    *
    * In order to ensure that the copied sources (which are just the indices
    * from the blob for now) don't get inserted into the old shader's use-def
    * lists, we have to add the phi instruction *before* we set up its
    * sources.
    */
   nir_instr_insert_after_block(blk, &phi->instr);

   for (unsigned i = 0; i < header.phi.num_srcs; i++) {
      nir_phi_src *src = ralloc(phi, nir_phi_src);

      src->src.is_ssa = true;
      src->src.ssa = (nir_ssa_def *)(uintptr_t) blob_read_uint32(ctx->blob);
      src->pred = (nir_block *)(uintptr_t) blob_read_uint32(ctx->blob);

      /* Since we're not letting nir_insert_instr handle use/def stuff for us,
       * we have to set the parent_instr manually.  It doesn't really matter
       * when we do it, so we might as well do it here.
       */
      src->src.parent_instr = &phi->instr;

      /* Stash it in the list of phi sources.  We'll walk this list and fix up
       * sources at the very end of read_function_impl.
       */
      list_add(&src->src.use_link, &ctx->phi_srcs);

      exec_list_push_tail(&phi->srcs, &src->node);
   }

   return phi;
}

static void
read_fixup_phis(read_ctx *ctx)
{
   list_for_each_entry_safe(nir_phi_src, src, &ctx->phi_srcs, src.use_link) {
      src->pred = read_lookup_object(ctx, (uintptr_t)src->pred);
      src->src.ssa = read_lookup_object(ctx, (uintptr_t)src->src.ssa);

      /* Remove from this list */
      list_del(&src->src.use_link);

      list_addtail(&src->src.use_link, &src->src.ssa->uses);
   }
   assert(list_is_empty(&ctx->phi_srcs));
}

static void
write_jump(write_ctx *ctx, const nir_jump_instr *jmp)
{
   assert(jmp->type < 4);

   union packed_instr header;
   header.u32 = 0;

   header.jump.instr_type = jmp->instr.type;
   header.jump.type = jmp->type;

   blob_write_uint32(ctx->blob, header.u32);
}

static nir_jump_instr *
read_jump(read_ctx *ctx, union packed_instr header)
{
   nir_jump_instr *jmp = nir_jump_instr_create(ctx->nir, header.jump.type);
   return jmp;
}

static void
write_call(write_ctx *ctx, const nir_call_instr *call)
{
   blob_write_uint32(ctx->blob, write_lookup_object(ctx, call->callee));

   for (unsigned i = 0; i < call->num_params; i++)
      write_src(ctx, &call->params[i]);
}

static nir_call_instr *
read_call(read_ctx *ctx)
{
   nir_function *callee = read_object(ctx);
   nir_call_instr *call = nir_call_instr_create(ctx->nir, callee);

   for (unsigned i = 0; i < call->num_params; i++)
      read_src(ctx, &call->params[i], call);

   return call;
}

static void
write_instr(write_ctx *ctx, const nir_instr *instr)
{
   /* We have only 4 bits for the instruction type. */
   assert(instr->type < 16);

   switch (instr->type) {
   case nir_instr_type_alu:
      write_alu(ctx, nir_instr_as_alu(instr));
      break;
   case nir_instr_type_deref:
      write_deref(ctx, nir_instr_as_deref(instr));
      break;
   case nir_instr_type_intrinsic:
      write_intrinsic(ctx, nir_instr_as_intrinsic(instr));
      break;
   case nir_instr_type_load_const:
      write_load_const(ctx, nir_instr_as_load_const(instr));
      break;
   case nir_instr_type_ssa_undef:
      write_ssa_undef(ctx, nir_instr_as_ssa_undef(instr));
      break;
   case nir_instr_type_tex:
      write_tex(ctx, nir_instr_as_tex(instr));
      break;
   case nir_instr_type_phi:
      write_phi(ctx, nir_instr_as_phi(instr));
      break;
   case nir_instr_type_jump:
      write_jump(ctx, nir_instr_as_jump(instr));
      break;
   case nir_instr_type_call:
      blob_write_uint32(ctx->blob, instr->type);
      write_call(ctx, nir_instr_as_call(instr));
      break;
   case nir_instr_type_parallel_copy:
      unreachable("Cannot write parallel copies");
   default:
      unreachable("bad instr type");
   }
}

static void
read_instr(read_ctx *ctx, nir_block *block)
{
   STATIC_ASSERT(sizeof(union packed_instr) == 4);
   union packed_instr header;
   header.u32 = blob_read_uint32(ctx->blob);
   nir_instr *instr;

   switch (header.any.instr_type) {
   case nir_instr_type_alu:
      instr = &read_alu(ctx, header)->instr;
      break;
   case nir_instr_type_deref:
      instr = &read_deref(ctx, header)->instr;
      break;
   case nir_instr_type_intrinsic:
      instr = &read_intrinsic(ctx, header)->instr;
      break;
   case nir_instr_type_load_const:
      instr = &read_load_const(ctx, header)->instr;
      break;
   case nir_instr_type_ssa_undef:
      instr = &read_ssa_undef(ctx, header)->instr;
      break;
   case nir_instr_type_tex:
      instr = &read_tex(ctx, header)->instr;
      break;
   case nir_instr_type_phi:
      /* Phi instructions are a bit of a special case when reading because we
       * don't want inserting the instruction to automatically handle use/defs
       * for us.  Instead, we need to wait until all the blocks/instructions
       * are read so that we can set their sources up.
       */
      read_phi(ctx, block, header);
      return;
   case nir_instr_type_jump:
      instr = &read_jump(ctx, header)->instr;
      break;
   case nir_instr_type_call:
      instr = &read_call(ctx)->instr;
      break;
   case nir_instr_type_parallel_copy:
      unreachable("Cannot read parallel copies");
   default:
      unreachable("bad instr type");
   }

   nir_instr_insert_after_block(block, instr);
}

static void
write_block(write_ctx *ctx, const nir_block *block)
{
   write_add_object(ctx, block);
   blob_write_uint32(ctx->blob, exec_list_length(&block->instr_list));
   nir_foreach_instr(instr, block)
      write_instr(ctx, instr);
}

static void
read_block(read_ctx *ctx, struct exec_list *cf_list)
{
   /* Don't actually create a new block.  Just use the one from the tail of
    * the list.  NIR guarantees that the tail of the list is a block and that
    * no two blocks are side-by-side in the IR;  It should be empty.
    */
   nir_block *block =
      exec_node_data(nir_block, exec_list_get_tail(cf_list), cf_node.node);

   read_add_object(ctx, block);
   unsigned num_instrs = blob_read_uint32(ctx->blob);
   for (unsigned i = 0; i < num_instrs; i++) {
      read_instr(ctx, block);
   }
}

static void
write_cf_list(write_ctx *ctx, const struct exec_list *cf_list);

static void
read_cf_list(read_ctx *ctx, struct exec_list *cf_list);

static void
write_if(write_ctx *ctx, nir_if *nif)
{
   write_src(ctx, &nif->condition);

   write_cf_list(ctx, &nif->then_list);
   write_cf_list(ctx, &nif->else_list);
}

static void
read_if(read_ctx *ctx, struct exec_list *cf_list)
{
   nir_if *nif = nir_if_create(ctx->nir);

   read_src(ctx, &nif->condition, nif);

   nir_cf_node_insert_end(cf_list, &nif->cf_node);

   read_cf_list(ctx, &nif->then_list);
   read_cf_list(ctx, &nif->else_list);
}

static void
write_loop(write_ctx *ctx, nir_loop *loop)
{
   write_cf_list(ctx, &loop->body);
}

static void
read_loop(read_ctx *ctx, struct exec_list *cf_list)
{
   nir_loop *loop = nir_loop_create(ctx->nir);

   nir_cf_node_insert_end(cf_list, &loop->cf_node);

   read_cf_list(ctx, &loop->body);
}

static void
write_cf_node(write_ctx *ctx, nir_cf_node *cf)
{
   blob_write_uint32(ctx->blob, cf->type);

   switch (cf->type) {
   case nir_cf_node_block:
      write_block(ctx, nir_cf_node_as_block(cf));
      break;
   case nir_cf_node_if:
      write_if(ctx, nir_cf_node_as_if(cf));
      break;
   case nir_cf_node_loop:
      write_loop(ctx, nir_cf_node_as_loop(cf));
      break;
   default:
      unreachable("bad cf type");
   }
}

static void
read_cf_node(read_ctx *ctx, struct exec_list *list)
{
   nir_cf_node_type type = blob_read_uint32(ctx->blob);

   switch (type) {
   case nir_cf_node_block:
      read_block(ctx, list);
      break;
   case nir_cf_node_if:
      read_if(ctx, list);
      break;
   case nir_cf_node_loop:
      read_loop(ctx, list);
      break;
   default:
      unreachable("bad cf type");
   }
}

static void
write_cf_list(write_ctx *ctx, const struct exec_list *cf_list)
{
   blob_write_uint32(ctx->blob, exec_list_length(cf_list));
   foreach_list_typed(nir_cf_node, cf, node, cf_list) {
      write_cf_node(ctx, cf);
   }
}

static void
read_cf_list(read_ctx *ctx, struct exec_list *cf_list)
{
   uint32_t num_cf_nodes = blob_read_uint32(ctx->blob);
   for (unsigned i = 0; i < num_cf_nodes; i++)
      read_cf_node(ctx, cf_list);
}

static void
write_function_impl(write_ctx *ctx, const nir_function_impl *fi)
{
   write_var_list(ctx, &fi->locals);
   write_reg_list(ctx, &fi->registers);
   blob_write_uint32(ctx->blob, fi->reg_alloc);

   write_cf_list(ctx, &fi->body);
   write_fixup_phis(ctx);
}

static nir_function_impl *
read_function_impl(read_ctx *ctx, nir_function *fxn)
{
   nir_function_impl *fi = nir_function_impl_create_bare(ctx->nir);
   fi->function = fxn;

   read_var_list(ctx, &fi->locals);
   read_reg_list(ctx, &fi->registers);
   fi->reg_alloc = blob_read_uint32(ctx->blob);

   read_cf_list(ctx, &fi->body);
   read_fixup_phis(ctx);

   fi->valid_metadata = 0;

   return fi;
}

static void
write_function(write_ctx *ctx, const nir_function *fxn)
{
   uint32_t flags = fxn->is_entrypoint;
   if (fxn->name)
      flags |= 0x2;
   if (fxn->impl)
      flags |= 0x4;
   blob_write_uint32(ctx->blob, flags);
   if (fxn->name)
      blob_write_string(ctx->blob, fxn->name);

   write_add_object(ctx, fxn);

   blob_write_uint32(ctx->blob, fxn->num_params);
   for (unsigned i = 0; i < fxn->num_params; i++) {
      uint32_t val =
         ((uint32_t)fxn->params[i].num_components) |
         ((uint32_t)fxn->params[i].bit_size) << 8;
      blob_write_uint32(ctx->blob, val);
   }

   /* At first glance, it looks like we should write the function_impl here.
    * However, call instructions need to be able to reference at least the
    * function and those will get processed as we write the function_impls.
    * We stop here and write function_impls as a second pass.
    */
}

static void
read_function(read_ctx *ctx)
{
   uint32_t flags = blob_read_uint32(ctx->blob);
   bool has_name = flags & 0x2;
   char *name = has_name ? blob_read_string(ctx->blob) : NULL;

   nir_function *fxn = nir_function_create(ctx->nir, name);

   read_add_object(ctx, fxn);

   fxn->num_params = blob_read_uint32(ctx->blob);
   fxn->params = ralloc_array(fxn, nir_parameter, fxn->num_params);
   for (unsigned i = 0; i < fxn->num_params; i++) {
      uint32_t val = blob_read_uint32(ctx->blob);
      fxn->params[i].num_components = val & 0xff;
      fxn->params[i].bit_size = (val >> 8) & 0xff;
   }

   fxn->is_entrypoint = flags & 0x1;
   if (flags & 0x4)
      fxn->impl = NIR_SERIALIZE_FUNC_HAS_IMPL;
}

/**
 * Serialize NIR into a binary blob.
 *
 * \param strip  Don't serialize information only useful for debugging,
 *               such as variable names, making cache hits from similar
 *               shaders more likely.
 */
void
nir_serialize(struct blob *blob, const nir_shader *nir, bool strip)
{
   write_ctx ctx = {0};
   ctx.remap_table = _mesa_pointer_hash_table_create(NULL);
   ctx.blob = blob;
   ctx.nir = nir;
   ctx.strip = strip;
   util_dynarray_init(&ctx.phi_fixups, NULL);

   size_t idx_size_offset = blob_reserve_uint32(blob);

   struct shader_info info = nir->info;
   uint32_t strings = 0;
   if (!strip && info.name)
      strings |= 0x1;
   if (!strip && info.label)
      strings |= 0x2;
   blob_write_uint32(blob, strings);
   if (!strip && info.name)
      blob_write_string(blob, info.name);
   if (!strip && info.label)
      blob_write_string(blob, info.label);
   info.name = info.label = NULL;
   blob_write_bytes(blob, (uint8_t *) &info, sizeof(info));

   write_var_list(&ctx, &nir->uniforms);
   write_var_list(&ctx, &nir->inputs);
   write_var_list(&ctx, &nir->outputs);
   write_var_list(&ctx, &nir->shared);
   write_var_list(&ctx, &nir->globals);
   write_var_list(&ctx, &nir->system_values);

   blob_write_uint32(blob, nir->num_inputs);
   blob_write_uint32(blob, nir->num_uniforms);
   blob_write_uint32(blob, nir->num_outputs);
   blob_write_uint32(blob, nir->num_shared);
   blob_write_uint32(blob, nir->scratch_size);

   blob_write_uint32(blob, exec_list_length(&nir->functions));
   nir_foreach_function(fxn, nir) {
      write_function(&ctx, fxn);
   }

   nir_foreach_function(fxn, nir) {
      if (fxn->impl)
         write_function_impl(&ctx, fxn->impl);
   }

   blob_write_uint32(blob, nir->constant_data_size);
   if (nir->constant_data_size > 0)
      blob_write_bytes(blob, nir->constant_data, nir->constant_data_size);

   *(uint32_t *)(blob->data + idx_size_offset) = ctx.next_idx;

   _mesa_hash_table_destroy(ctx.remap_table, NULL);
   util_dynarray_fini(&ctx.phi_fixups);
}

nir_shader *
nir_deserialize(void *mem_ctx,
                const struct nir_shader_compiler_options *options,
                struct blob_reader *blob)
{
   read_ctx ctx = {0};
   ctx.blob = blob;
   list_inithead(&ctx.phi_srcs);
   ctx.idx_table_len = blob_read_uint32(blob);
   ctx.idx_table = calloc(ctx.idx_table_len, sizeof(uintptr_t));

   uint32_t strings = blob_read_uint32(blob);
   char *name = (strings & 0x1) ? blob_read_string(blob) : NULL;
   char *label = (strings & 0x2) ? blob_read_string(blob) : NULL;

   struct shader_info info;
   blob_copy_bytes(blob, (uint8_t *) &info, sizeof(info));

   ctx.nir = nir_shader_create(mem_ctx, info.stage, options, NULL);

   info.name = name ? ralloc_strdup(ctx.nir, name) : NULL;
   info.label = label ? ralloc_strdup(ctx.nir, label) : NULL;

   ctx.nir->info = info;

   read_var_list(&ctx, &ctx.nir->uniforms);
   read_var_list(&ctx, &ctx.nir->inputs);
   read_var_list(&ctx, &ctx.nir->outputs);
   read_var_list(&ctx, &ctx.nir->shared);
   read_var_list(&ctx, &ctx.nir->globals);
   read_var_list(&ctx, &ctx.nir->system_values);

   ctx.nir->num_inputs = blob_read_uint32(blob);
   ctx.nir->num_uniforms = blob_read_uint32(blob);
   ctx.nir->num_outputs = blob_read_uint32(blob);
   ctx.nir->num_shared = blob_read_uint32(blob);
   ctx.nir->scratch_size = blob_read_uint32(blob);

   unsigned num_functions = blob_read_uint32(blob);
   for (unsigned i = 0; i < num_functions; i++)
      read_function(&ctx);

   nir_foreach_function(fxn, ctx.nir) {
      if (fxn->impl == NIR_SERIALIZE_FUNC_HAS_IMPL)
         fxn->impl = read_function_impl(&ctx, fxn);
   }

   ctx.nir->constant_data_size = blob_read_uint32(blob);
   if (ctx.nir->constant_data_size > 0) {
      ctx.nir->constant_data =
         ralloc_size(ctx.nir, ctx.nir->constant_data_size);
      blob_copy_bytes(blob, ctx.nir->constant_data,
                      ctx.nir->constant_data_size);
   }

   free(ctx.idx_table);

   return ctx.nir;
}

void
nir_shader_serialize_deserialize(nir_shader *shader)
{
   const struct nir_shader_compiler_options *options = shader->options;

   struct blob writer;
   blob_init(&writer);
   nir_serialize(&writer, shader, false);

   /* Delete all of dest's ralloc children but leave dest alone */
   void *dead_ctx = ralloc_context(NULL);
   ralloc_adopt(dead_ctx, shader);
   ralloc_free(dead_ctx);

   dead_ctx = ralloc_context(NULL);

   struct blob_reader reader;
   blob_reader_init(&reader, writer.data, writer.size);
   nir_shader *copy = nir_deserialize(dead_ctx, options, &reader);

   blob_finish(&writer);

   nir_shader_replace(shader, copy);
   ralloc_free(dead_ctx);
}
