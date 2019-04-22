#include "state_tracker/drm_driver.h"
#include "target-helpers/inline_debug_helper.h"
#include "radeon/drm/radeon_drm_public.h"
#include "radeon/radeon_winsys.h"
#include "r600/r600_public.h"

static struct pipe_screen *
create_screen(int fd, const struct pipe_screen_config *config)
{
   struct radeon_winsys *rw;

   rw = radeon_drm_winsys_create(fd, config, r600_screen_create);
   return rw ? debug_screen_wrap(rw->screen) : NULL;
}

static const struct drm_conf_ret *drm_configuration(enum drm_conf conf)
{
   return NULL;
}

PUBLIC
DRM_DRIVER_DESCRIPTOR("r600", create_screen, drm_configuration)
