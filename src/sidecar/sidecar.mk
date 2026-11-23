# ============================================================================
# 服务层 af_sidecar 静态库 libaf_sidecar.a（ModID 18）
# ============================================================================

AF_SIDECAR_SRC    := src/sidecar/af_sidecar_mod.c
AF_SIDECAR_CFLAGS := -Isrc $(LIBDM_INC) $(DM_DEFS)
AF_SIDECAR_LIB    := $(LIB)/libaf_sidecar.a
$(eval $(call static_lib,AF_SIDECAR))
