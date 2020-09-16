
#include "target-helpers/inline_debug_helper.h"
#include "target-helpers/drm_helper.h"
#include "frontend/drm_driver.h"
#include "svga/drm/svga_drm_public.h"
#include "svga/svga_public.h"

PUBLIC
DRM_DRIVER_DESCRIPTOR("vmwgfx", NULL, pipe_vmwgfx_create_screen)
