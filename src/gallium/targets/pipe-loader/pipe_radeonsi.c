#include "frontend/drm_driver.h"
#include "target-helpers/drm_helper.h"
#include "target-helpers/inline_debug_helper.h"
#include "radeonsi/si_public.h"
#include "util/driconf.h"

static const char *driconf_xml =
   #include "radeonsi/si_driinfo.h"
   ;

PUBLIC
DRM_DRIVER_DESCRIPTOR("radeonsi", &driconf_xml, pipe_radeonsi_create_screen)
