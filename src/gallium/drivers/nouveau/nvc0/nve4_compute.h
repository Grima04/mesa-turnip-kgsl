
#ifndef NVE4_COMPUTE_H
#define NVE4_COMPUTE_H

#include "nvc0/nve4_compute.xml.h"

struct gp100_cp_launch_desc
{
   u32 unk0[8];
   u32 entry;
   u32 unk9[2];
   u32 unk11_0      : 30;
   u32 linked_tsc   : 1;
   u32 unk11_31     : 1;
   u32 griddim_x    : 31;
   u32 unk12        : 1;
   u16 griddim_y;
   u16 unk13;
   u16 griddim_z;
   u16 unk14;
   u32 unk15[2];
   u32 shared_size  : 18;
   u32 unk17        : 14;
   u16 unk18;
   u16 blockdim_x;
   u16 blockdim_y;
   u16 blockdim_z;
   u32 cb_mask      : 8;
   u32 unk20        : 24;
   u32 unk21[8];
   u32 local_size_p : 24;
   u32 unk29        : 3;
   u32 bar_alloc    : 5;
   u32 local_size_n : 24;
   u32 gpr_alloc    : 8;
   u32 cstack_size  : 24;
   u32 unk31        : 8;
   struct {
      u32 address_l;
      u32 address_h : 17;
      u32 reserved  : 2;
      u32 size_sh4  : 13;
   } cb[8];
   u32 unk48[16];
};

static inline void
gp100_cp_launch_desc_init_default(struct gp100_cp_launch_desc *desc)
{
   memset(desc, 0, sizeof(*desc));

   desc->unk0[4]  = 0x40;
   desc->unk11_0  = 0x04014000;
}

static inline void
gp100_cp_launch_desc_set_cb(struct gp100_cp_launch_desc *desc,
                            unsigned index,
                            struct nouveau_bo *bo,
                            uint32_t base, uint32_t size)
{
   uint64_t address = bo->offset + base;

   assert(index < 8);
   assert(!(base & 0xff));

   desc->cb[index].address_l = address;
   desc->cb[index].address_h = address >> 32;
   desc->cb[index].size_sh4 = DIV_ROUND_UP(size, 16);

   desc->cb_mask |= 1 << index;
}

struct nve4_mp_trap_info {
   u32 lock;
   u32 pc;
   u32 trapstat;
   u32 warperr;
   u32 tid[3];
   u32 ctaid[3];
   u32 pad028[2];
   u32 r[64];
   u32 flags;
   u32 pad134[3];
   u32 s[0x3000];
};

#endif /* NVE4_COMPUTE_H */
