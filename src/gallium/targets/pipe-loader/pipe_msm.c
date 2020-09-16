
#include "target-helpers/drm_helper.h"
#include "target-helpers/inline_debug_helper.h"
#include "frontend/drm_driver.h"
#include "freedreno/drm/freedreno_drm_public.h"

PUBLIC
DRM_DRIVER_DESCRIPTOR("msm", NULL, pipe_freedreno_create_screen)
