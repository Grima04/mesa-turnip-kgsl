
#include "target-helpers/inline_debug_helper.h"
#include "frontend/drm_driver.h"
#include "i915/drm/i915_drm_public.h"
#include "i915/i915_public.h"

PUBLIC
DRM_DRIVER_DESCRIPTOR("i915", NULL, pipe_i915_create_screen)
