
#include "target-helpers/inline_debug_helper.h"
#include "frontend/drm_driver.h"
#include "kmsro/drm/kmsro_drm_public.h"

PUBLIC
DRM_DRIVER_DESCRIPTOR("kmsro", NULL, pipe_kmsro_create_screen)
