#include "sfn_emitssboinstruction.h"

#include "sfn_instruction_fetch.h"
#include "sfn_instruction_gds.h"
#include "sfn_instruction_misc.h"
#include "../r600_pipe.h"

namespace r600 {

bool EmitSSBOInstruction::do_emit(nir_instr* instr)
{
   const nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   switch (intr->intrinsic) {
   case nir_intrinsic_atomic_counter_add:
   case nir_intrinsic_atomic_counter_and:
   case nir_intrinsic_atomic_counter_exchange:
   case nir_intrinsic_atomic_counter_max:
   case nir_intrinsic_atomic_counter_min:
   case nir_intrinsic_atomic_counter_or:
   case nir_intrinsic_atomic_counter_xor:
   case nir_intrinsic_atomic_counter_comp_swap:
      return emit_atomic(intr);
   case nir_intrinsic_atomic_counter_read:
   case nir_intrinsic_atomic_counter_post_dec:
      return emit_unary_atomic(intr);
   case nir_intrinsic_atomic_counter_inc:
      return emit_atomic_inc(intr);
   case nir_intrinsic_atomic_counter_pre_dec:
      return emit_atomic_pre_dec(intr);
   default:
      return false;
   }
}

bool EmitSSBOInstruction::emit_atomic(const nir_intrinsic_instr* instr)
{
   ESDOp op = get_opcode(instr->intrinsic);

   if (DS_OP_INVALID == op)
      return false;

   GPRVector dest = make_dest(instr);

   int base = nir_intrinsic_base(instr);

   PValue uav_id = from_nir(instr->src[0], 0);

   PValue value = from_nir_with_fetch_constant(instr->src[1], 0);

   GDSInstr *ir = nullptr;
   if (instr->intrinsic == nir_intrinsic_atomic_counter_comp_swap)  {
      PValue value2 = from_nir_with_fetch_constant(instr->src[1], 1);
      ir = new GDSInstr(op, dest, value, value2, uav_id, base);
   } else {
      ir = new GDSInstr(op, dest, value, uav_id, base);
   }

   emit_instruction(ir);
   return true;
}

bool EmitSSBOInstruction::emit_unary_atomic(const nir_intrinsic_instr* instr)
{
   ESDOp op = get_opcode(instr->intrinsic);

   if (DS_OP_INVALID == op)
      return false;

   GPRVector dest = make_dest(instr);

   PValue uav_id = from_nir(instr->src[0], 0);

   auto ir = new GDSInstr(op, dest, uav_id, nir_intrinsic_base(instr));

   emit_instruction(ir);
   return true;
}

ESDOp EmitSSBOInstruction::get_opcode(const nir_intrinsic_op opcode)
{
   switch (opcode) {
   case nir_intrinsic_atomic_counter_add:
      return DS_OP_ADD_RET;
   case nir_intrinsic_atomic_counter_and:
      return DS_OP_AND_RET;
   case nir_intrinsic_atomic_counter_exchange:
      return DS_OP_XCHG_RET;
   case nir_intrinsic_atomic_counter_inc:
      return DS_OP_INC_RET;
   case nir_intrinsic_atomic_counter_max:
      return DS_OP_MAX_UINT_RET;
   case nir_intrinsic_atomic_counter_min:
      return DS_OP_MIN_UINT_RET;
   case nir_intrinsic_atomic_counter_or:
      return DS_OP_OR_RET;
   case nir_intrinsic_atomic_counter_read:
      return DS_OP_READ_RET;
   case nir_intrinsic_atomic_counter_xor:
      return DS_OP_XOR_RET;
   case nir_intrinsic_atomic_counter_post_dec:
      return DS_OP_DEC_RET;
   case nir_intrinsic_atomic_counter_comp_swap:
      return DS_OP_CMP_XCHG_RET;
   case nir_intrinsic_atomic_counter_pre_dec:
   default:
      return DS_OP_INVALID;
   }
}


bool EmitSSBOInstruction::emit_atomic_add(const nir_intrinsic_instr* instr)
{
   GPRVector dest = make_dest(instr);

   PValue value = from_nir_with_fetch_constant(instr->src[1], 0);

   PValue uav_id = from_nir(instr->src[0], 0);

   auto ir = new GDSInstr(DS_OP_ADD_RET, dest, value, uav_id,
                          nir_intrinsic_base(instr));

   emit_instruction(ir);
   return true;
}

bool EmitSSBOInstruction::emit_atomic_inc(const nir_intrinsic_instr* instr)
{
   GPRVector dest = make_dest(instr);

   PValue uav_id = from_nir(instr->src[0], 0);


   if (!m_atomic_limit) {
      int one_tmp = allocate_temp_register();
      m_atomic_limit = PValue(new GPRValue(one_tmp, 0));
      emit_instruction(new AluInstruction(op1_mov, m_atomic_limit,
                       PValue(new LiteralValue(0xffffffff)),
                       {alu_write, alu_last_instr}));
   }

   auto ir = new GDSInstr(DS_OP_INC_RET, dest, m_atomic_limit, uav_id,
                          nir_intrinsic_base(instr));
   emit_instruction(ir);
   return true;
}

bool EmitSSBOInstruction::emit_atomic_pre_dec(const nir_intrinsic_instr *instr)
{
   GPRVector dest = make_dest(instr);

   PValue uav_id = from_nir(instr->src[0], 0);

   int one_tmp = allocate_temp_register();
   PValue value(new GPRValue(one_tmp, 0));
   emit_instruction(new AluInstruction(op1_mov, value,  Value::one_i,
                    {alu_write, alu_last_instr}));

   auto ir = new GDSInstr(DS_OP_SUB_RET, dest, value, uav_id,
                          nir_intrinsic_base(instr));
   emit_instruction(ir);

   ir = new GDSInstr(DS_OP_READ_RET, dest, uav_id, nir_intrinsic_base(instr));
   emit_instruction(ir);

   return true;
}

GPRVector EmitSSBOInstruction::make_dest(const nir_intrinsic_instr* ir)
{
   GPRVector::Values v;
   int i;
   for (i = 0; i < 4; ++i)
      v[i] = from_nir(ir->dest, i);
   return GPRVector(v);
}

}
