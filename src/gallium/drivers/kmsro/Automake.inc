if HAVE_GALLIUM_KMSRO

TARGET_DRIVERS += pl111
TARGET_CPPFLAGS += -DGALLIUM_KMSRO
TARGET_LIB_DEPS += \
    $(top_builddir)/src/gallium/winsys/kmsro/drm/libkmsrodrm.la \
    $(LIBDRM_LIBS)

endif
