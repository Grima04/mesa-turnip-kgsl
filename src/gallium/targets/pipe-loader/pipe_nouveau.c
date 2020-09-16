
#include "target-helpers/drm_helper.h"
#include "target-helpers/inline_debug_helper.h"
#include "frontend/drm_driver.h"
#include "nouveau/drm/nouveau_drm_public.h"

PUBLIC
DRM_DRIVER_DESCRIPTOR("nouveau", NULL, pipe_nouveau_create_screen)
