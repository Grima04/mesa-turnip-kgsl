#include "iris_context.h"
#include "iris_seqno.h"
#include "util/u_upload_mgr.h"

static void
iris_seqno_reset(struct iris_batch *batch)
{
   u_upload_alloc(batch->seqno.uploader, 0, sizeof(uint64_t), sizeof(uint64_t),
                  &batch->seqno.ref.offset, &batch->seqno.ref.res,
                  (void **)&batch->seqno.map);
   WRITE_ONCE(*batch->seqno.map, 0);
   batch->seqno.next++;
}

void
iris_seqno_init(struct iris_batch *batch)
{
   batch->seqno.ref.res = NULL;
   batch->seqno.next = 0;
   iris_seqno_reset(batch);
}

static uint32_t
iris_seqno_next(struct iris_batch *batch)
{
   uint32_t seqno = batch->seqno.next++;

   if (batch->seqno.next == 0)
      iris_seqno_reset(batch);

   return seqno;
}

void
iris_seqno_destroy(struct iris_screen *screen, struct iris_seqno *sq)
{
   iris_syncobj_reference(screen, &sq->syncobj, NULL);
   pipe_resource_reference(&sq->ref.res, NULL);
   free(sq);
}

struct iris_seqno *
iris_seqno_new(struct iris_batch *batch, unsigned flags)
{
   struct iris_seqno *sq = calloc(1, sizeof(*sq));
   if (!sq)
      return NULL;

   pipe_reference_init(&sq->reference, 1);

   sq->seqno = iris_seqno_next(batch);

   iris_syncobj_reference(batch->screen, &sq->syncobj,
                          iris_batch_get_signal_syncobj(batch));

   pipe_resource_reference(&sq->ref.res, batch->seqno.ref.res);
   sq->ref.offset = batch->seqno.ref.offset;
   sq->map = batch->seqno.map;
   sq->flags = flags;

   unsigned pc;
   if (flags & IRIS_SEQNO_TOP_OF_PIPE) {
      pc = PIPE_CONTROL_WRITE_IMMEDIATE | PIPE_CONTROL_CS_STALL;
   } else {
      pc = PIPE_CONTROL_WRITE_IMMEDIATE |
           PIPE_CONTROL_RENDER_TARGET_FLUSH |
           PIPE_CONTROL_DEPTH_CACHE_FLUSH |
           PIPE_CONTROL_DATA_CACHE_FLUSH;
   }
   iris_emit_pipe_control_write(batch, "fence: seqno", pc,
                                iris_resource_bo(sq->ref.res),
                                sq->ref.offset,
                                sq->seqno);

   return sq;
}
