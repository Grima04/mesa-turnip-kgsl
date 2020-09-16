#ifndef DRM_HELPER_H
#define DRM_HELPER_H

#include <stdio.h>
#include "target-helpers/inline_debug_helper.h"
#include "target-helpers/drm_helper_public.h"
#include "frontend/drm_driver.h"
#include "util/driconf.h"

/**
 * Instantiate a drm_driver_descriptor struct.
 */
#define DEFINE_DRM_DRIVER_DESCRIPTOR(descriptor_name, driver, driconf, func) \
const struct drm_driver_descriptor descriptor_name = {         \
   .driver_name = #driver,                                     \
   .driconf_xml = driconf,                                     \
   .create_screen = func,                                      \
};

/* The static pipe loader refers to the *_driver_descriptor structs for all
 * drivers, regardless of whether they are configured in this Mesa build, or
 * whether they're included in the specific gallium target.  The target (dri,
 * vdpau, etc.) will include this header with the #defines for the specific
 * drivers it's including, and the disabled drivers will have a descriptor
 * with a stub create function logging the failure.
 *
 * The dynamic pipe loader instead has target/pipeloader/pipe_*.c including
 * this header in a pipe_*.so for each driver which will have one driver's
 * GALLIUM_* defined.  We make a single driver_descriptor entrypoint that is
 * dlsym()ed by the dynamic pipe loader.
 */

#ifdef PIPE_LOADER_DYNAMIC

#define DRM_DRIVER_DESCRIPTOR(driver, driconf)                          \
   PUBLIC DEFINE_DRM_DRIVER_DESCRIPTOR(driver_descriptor, driver, driconf, pipe_##driver##_create_screen)

#define DRM_DRIVER_DESCRIPTOR_STUB(driver)

#define DRM_DRIVER_DESCRIPTOR_ALIAS(driver, alias, driconf)

#else

#define DRM_DRIVER_DESCRIPTOR(driver, driconf)                          \
   DEFINE_DRM_DRIVER_DESCRIPTOR(driver##_driver_descriptor, driver, driconf, pipe_##driver##_create_screen)

#define DRM_DRIVER_DESCRIPTOR_STUB(driver)                              \
   static struct pipe_screen *                                          \
   pipe_##driver##_create_screen(int fd, const struct pipe_screen_config *config) \
   {                                                                    \
      fprintf(stderr, #driver ": driver missing\n");                    \
      return NULL;                                                      \
   }                                                                    \
   DRM_DRIVER_DESCRIPTOR(driver, NULL)

#define DRM_DRIVER_DESCRIPTOR_ALIAS(driver, alias, driconf) \
   DEFINE_DRM_DRIVER_DESCRIPTOR(alias##_driver_descriptor, alias, driconf, pipe_##driver##_create_screen)

#endif

#ifdef GALLIUM_I915
#include "i915/drm/i915_drm_public.h"
#include "i915/i915_public.h"

static struct pipe_screen *
pipe_i915_create_screen(int fd, const struct pipe_screen_config *config)
{
   struct i915_winsys *iws;
   struct pipe_screen *screen;

   iws = i915_drm_winsys_create(fd);
   if (!iws)
      return NULL;

   screen = i915_screen_create(iws);
   return screen ? debug_screen_wrap(screen) : NULL;
}
DRM_DRIVER_DESCRIPTOR(i915, NULL)
#else
DRM_DRIVER_DESCRIPTOR_STUB(i915)
#endif

#ifdef GALLIUM_IRIS
#include "iris/drm/iris_drm_public.h"

static struct pipe_screen *
pipe_iris_create_screen(int fd, const struct pipe_screen_config *config)
{
   struct pipe_screen *screen;

   screen = iris_drm_screen_create(fd, config);
   return screen ? debug_screen_wrap(screen) : NULL;
}

const char *iris_driconf_xml =
      #include "iris/driinfo_iris.h"
      ;
DRM_DRIVER_DESCRIPTOR(iris, &iris_driconf_xml)

#else
DRM_DRIVER_DESCRIPTOR_STUB(iris)
#endif

#ifdef GALLIUM_NOUVEAU
#include "nouveau/drm/nouveau_drm_public.h"

static struct pipe_screen *
pipe_nouveau_create_screen(int fd, const struct pipe_screen_config *config)
{
   struct pipe_screen *screen;

   screen = nouveau_drm_screen_create(fd);
   return screen ? debug_screen_wrap(screen) : NULL;
}
DRM_DRIVER_DESCRIPTOR(nouveau, NULL)

#else
DRM_DRIVER_DESCRIPTOR_STUB(nouveau)
#endif

#if defined(GALLIUM_VC4) || defined(GALLIUM_V3D)
   const char *v3d_driconf_xml =
      #include "v3d/driinfo_v3d.h"
      ;
#endif

#ifdef GALLIUM_KMSRO
#include "kmsro/drm/kmsro_drm_public.h"

static struct pipe_screen *
pipe_kmsro_create_screen(int fd, const struct pipe_screen_config *config)
{
   struct pipe_screen *screen;

   screen = kmsro_drm_screen_create(fd, config);
   return screen ? debug_screen_wrap(screen) : NULL;
}
#if defined(GALLIUM_VC4) || defined(GALLIUM_V3D)
DRM_DRIVER_DESCRIPTOR(kmsro, &v3d_driconf_xml)
#else
DRM_DRIVER_DESCRIPTOR(kmsro, NULL)
#endif

#else
DRM_DRIVER_DESCRIPTOR_STUB(kmsro)
#endif

#ifdef GALLIUM_R300
#include "radeon/radeon_winsys.h"
#include "radeon/drm/radeon_drm_public.h"
#include "r300/r300_public.h"

static struct pipe_screen *
pipe_r300_create_screen(int fd, const struct pipe_screen_config *config)
{
   struct radeon_winsys *rw;

   rw = radeon_drm_winsys_create(fd, config, r300_screen_create);
   return rw ? debug_screen_wrap(rw->screen) : NULL;
}
DRM_DRIVER_DESCRIPTOR(r300, NULL)

#else
DRM_DRIVER_DESCRIPTOR_STUB(r300)
#endif

#ifdef GALLIUM_R600
#include "radeon/radeon_winsys.h"
#include "radeon/drm/radeon_drm_public.h"
#include "r600/r600_public.h"

static struct pipe_screen *
pipe_r600_create_screen(int fd, const struct pipe_screen_config *config)
{
   struct radeon_winsys *rw;

   rw = radeon_drm_winsys_create(fd, config, r600_screen_create);
   return rw ? debug_screen_wrap(rw->screen) : NULL;
}
DRM_DRIVER_DESCRIPTOR(r600, NULL)

#else
DRM_DRIVER_DESCRIPTOR_STUB(r600)
#endif

#ifdef GALLIUM_RADEONSI
#include "radeonsi/si_public.h"

static struct pipe_screen *
pipe_radeonsi_create_screen(int fd, const struct pipe_screen_config *config)
{
   struct pipe_screen *screen = radeonsi_screen_create(fd, config);

   return screen ? debug_screen_wrap(screen) : NULL;
}

const char *radeonsi_driconf_xml =
      #include "radeonsi/driinfo_radeonsi.h"
      ;
DRM_DRIVER_DESCRIPTOR(radeonsi, &radeonsi_driconf_xml)

#else
DRM_DRIVER_DESCRIPTOR_STUB(radeonsi)
#endif

#ifdef GALLIUM_VMWGFX
#include "svga/drm/svga_drm_public.h"
#include "svga/svga_public.h"

static struct pipe_screen *
pipe_vmwgfx_create_screen(int fd, const struct pipe_screen_config *config)
{
   struct svga_winsys_screen *sws;
   struct pipe_screen *screen;

   sws = svga_drm_winsys_screen_create(fd);
   if (!sws)
      return NULL;

   screen = svga_screen_create(sws);
   return screen ? debug_screen_wrap(screen) : NULL;
}
DRM_DRIVER_DESCRIPTOR(vmwgfx, NULL)

#else
DRM_DRIVER_DESCRIPTOR_STUB(vmwgfx)
#endif

#ifdef GALLIUM_FREEDRENO
#include "freedreno/drm/freedreno_drm_public.h"

static struct pipe_screen *
pipe_msm_create_screen(int fd, const struct pipe_screen_config *config)
{
   struct pipe_screen *screen;

   screen = fd_drm_screen_create(fd, NULL);
   return screen ? debug_screen_wrap(screen) : NULL;
}
DRM_DRIVER_DESCRIPTOR(msm, NULL)
#else
DRM_DRIVER_DESCRIPTOR_STUB(msm)
#endif
DRM_DRIVER_DESCRIPTOR_ALIAS(msm, kgsl, NULL)

#ifdef GALLIUM_VIRGL
#include "virgl/drm/virgl_drm_public.h"
#include "virgl/virgl_public.h"

static struct pipe_screen *
pipe_virtio_gpu_create_screen(int fd, const struct pipe_screen_config *config)
{
   struct pipe_screen *screen;

   screen = virgl_drm_screen_create(fd, config);
   return screen ? debug_screen_wrap(screen) : NULL;
}

const char *virgl_driconf_xml =
      #include "virgl/virgl_driinfo.h.in"
      ;
DRM_DRIVER_DESCRIPTOR(virtio_gpu, &virgl_driconf_xml)

#else
DRM_DRIVER_DESCRIPTOR_STUB(virtio_gpu)
#endif

#ifdef GALLIUM_VC4
#include "vc4/drm/vc4_drm_public.h"

static struct pipe_screen *
pipe_vc4_create_screen(int fd, const struct pipe_screen_config *config)
{
   struct pipe_screen *screen;

   screen = vc4_drm_screen_create(fd, config);
   return screen ? debug_screen_wrap(screen) : NULL;
}
DRM_DRIVER_DESCRIPTOR(vc4, &v3d_driconf_xml)
#else
DRM_DRIVER_DESCRIPTOR_STUB(vc4)
#endif

#ifdef GALLIUM_V3D
#include "v3d/drm/v3d_drm_public.h"

static struct pipe_screen *
pipe_v3d_create_screen(int fd, const struct pipe_screen_config *config)
{
   struct pipe_screen *screen;

   screen = v3d_drm_screen_create(fd, config);
   return screen ? debug_screen_wrap(screen) : NULL;
}

DRM_DRIVER_DESCRIPTOR(v3d, &v3d_driconf_xml)

#else
DRM_DRIVER_DESCRIPTOR_STUB(v3d)
#endif

#ifdef GALLIUM_PANFROST
#include "panfrost/drm/panfrost_drm_public.h"

static struct pipe_screen *
pipe_panfrost_create_screen(int fd, const struct pipe_screen_config *config)
{
   struct pipe_screen *screen;

   screen = panfrost_drm_screen_create(fd);
   return screen ? debug_screen_wrap(screen) : NULL;
}
DRM_DRIVER_DESCRIPTOR(panfrost, NULL)

#else
DRM_DRIVER_DESCRIPTOR_STUB(panfrost)
#endif

#ifdef GALLIUM_ETNAVIV
#include "etnaviv/drm/etnaviv_drm_public.h"

static struct pipe_screen *
pipe_etnaviv_create_screen(int fd, const struct pipe_screen_config *config)
{
   struct pipe_screen *screen;

   screen = etna_drm_screen_create(fd);
   return screen ? debug_screen_wrap(screen) : NULL;
}
DRM_DRIVER_DESCRIPTOR(etnaviv, NULL)

#else
DRM_DRIVER_DESCRIPTOR_STUB(etnaviv)
#endif

#ifdef GALLIUM_TEGRA
#include "tegra/drm/tegra_drm_public.h"

static struct pipe_screen *
pipe_tegra_create_screen(int fd, const struct pipe_screen_config *config)
{
   struct pipe_screen *screen;

   screen = tegra_drm_screen_create(fd);

   return screen ? debug_screen_wrap(screen) : NULL;
}
DRM_DRIVER_DESCRIPTOR(tegra, NULL)

#else
DRM_DRIVER_DESCRIPTOR_STUB(tegra)
#endif

#ifdef GALLIUM_LIMA
#include "lima/drm/lima_drm_public.h"

static struct pipe_screen *
pipe_lima_create_screen(int fd, const struct pipe_screen_config *config)
{
   struct pipe_screen *screen;

   screen = lima_drm_screen_create(fd);
   return screen ? debug_screen_wrap(screen) : NULL;
}
DRM_DRIVER_DESCRIPTOR(lima, NULL)

#else
DRM_DRIVER_DESCRIPTOR_STUB(lima)
#endif

#ifdef GALLIUM_ZINK
#include "zink/zink_public.h"

static struct pipe_screen *
pipe_zink_create_screen(int fd, const struct pipe_screen_config *config)
{
   struct pipe_screen *screen;
   screen = zink_drm_create_screen(fd);
   return screen ? debug_screen_wrap(screen) : NULL;
}
DRM_DRIVER_DESCRIPTOR(zink, NULL)

#else
DRM_DRIVER_DESCRIPTOR_STUB(zink)
#endif

#endif /* DRM_HELPER_H */
