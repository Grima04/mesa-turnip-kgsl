
#include "zink_context.h"
#include "zink_screen.h"

#include "util/u_memory.h"
struct zink_query {
   VkQueryPool queryPool;
   VkQueryType vkqtype;
   bool use_64bit;
   bool precise;
};

static VkQueryType
convert_query_type(unsigned query_type, bool *use_64bit, bool *precise)
{
   *use_64bit = false;
   *precise = false;
   switch (query_type) {
   case PIPE_QUERY_OCCLUSION_COUNTER:
      *precise = true;
      *use_64bit = true;
   case PIPE_QUERY_OCCLUSION_PREDICATE:
   case PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE:
      return VK_QUERY_TYPE_OCCLUSION;
   case PIPE_QUERY_TIMESTAMP:
      *use_64bit = true;
      return VK_QUERY_TYPE_TIMESTAMP;
   case PIPE_QUERY_PIPELINE_STATISTICS:
      return VK_QUERY_TYPE_PIPELINE_STATISTICS;
   default:
      fprintf(stderr, "zink: unknown query type\n");
      return -1;
   }
}

static struct pipe_query *
zink_create_query(struct pipe_context *pctx,
                  unsigned query_type, unsigned index)
{
   struct zink_screen *screen = zink_screen(pctx->screen);
   struct zink_query *query = CALLOC_STRUCT(zink_query);
   VkQueryPoolCreateInfo pool_create = {};

   if (!query)
      return NULL;

   query->vkqtype = convert_query_type(query_type, &query->use_64bit, &query->precise);
   if (query->vkqtype == -1)
      return NULL;

   pool_create.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
   pool_create.queryType = query->vkqtype;
   pool_create.queryCount = 1;

   VkResult status = vkCreateQueryPool(screen->dev, &pool_create, NULL, &query->queryPool);
   if (status != VK_SUCCESS) {
      FREE(query);
      return NULL;
   }
   return (struct pipe_query *)query;
}

static void
zink_destroy_query(struct pipe_context *pctx,
                   struct pipe_query *q)
{
   struct zink_screen *screen = zink_screen(pctx->screen);
   struct zink_query *query = CALLOC_STRUCT(zink_query);

   vkDestroyQueryPool(screen->dev, query->queryPool, NULL);
}

static bool
zink_begin_query(struct pipe_context *pctx,
                 struct pipe_query *q)
{
   struct zink_context *ctx = zink_context(pctx);
   struct zink_query *query = (struct zink_query *)q;

   if (query->vkqtype == VK_QUERY_TYPE_TIMESTAMP)
      return true;

   VkQueryControlFlags flags = 0;
   if (query->precise)
      flags |= VK_QUERY_CONTROL_PRECISE_BIT;

   struct zink_batch *batch = zink_curr_batch(ctx);
   vkCmdBeginQuery(batch->cmdbuf, query->queryPool, 0, flags);

   return true;
}

static bool
zink_end_query(struct pipe_context *pctx,
               struct pipe_query *q)
{
   struct zink_context *ctx = zink_context(pctx);
   struct zink_query *query = (struct zink_query *)q;

   struct zink_batch *batch = zink_curr_batch(ctx);
   if (query->vkqtype == VK_QUERY_TYPE_TIMESTAMP)
      vkCmdWriteTimestamp(batch->cmdbuf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                          query->queryPool, 0);
   else
      vkCmdEndQuery(batch->cmdbuf, query->queryPool, 0);
   return true;
}

static bool
zink_get_query_result(struct pipe_context *pctx,
                      struct pipe_query *q,
                      bool wait,
                      union pipe_query_result *result)
{
   struct zink_screen *screen = zink_screen(pctx->screen);
   struct zink_query *query = (struct zink_query *)q;
   VkQueryResultFlagBits flags = 0;

   pctx->flush(pctx, NULL, 0);

   if (wait)
      flags |= VK_QUERY_RESULT_WAIT_BIT;

   if (query->use_64bit)
      flags |= VK_QUERY_RESULT_64_BIT;

   VkResult status = vkGetQueryPoolResults(screen->dev, query->queryPool,
                                           0, 1, sizeof(*result), result,
                                           0, flags);
   return status == VK_SUCCESS;
}

void
zink_context_query_init(struct pipe_context *pctx)
{
   pctx->create_query = zink_create_query;
   pctx->destroy_query = zink_destroy_query;
   pctx->begin_query = zink_begin_query;
   pctx->end_query = zink_end_query;
   pctx->get_query_result = zink_get_query_result;
}
