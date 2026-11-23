# ============================================================================
# 服务层 af_display 静态库 libaf_display.a（ModID 17）
# ============================================================================

AF_DISPLAY_SRC    := src/display/af_display_algo.c \
                     src/display/af_display_vd.c \
                     src/display/af_display_mod.c
AF_DISPLAY_CFLAGS := -Isrc $(LIBDM_INC) $(DM_DEFS)
AF_DISPLAY_LIB    := $(LIB)/libaf_display.a
$(eval $(call static_lib,AF_DISPLAY))
