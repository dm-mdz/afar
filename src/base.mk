# ============================================================================
# 基础层纯 C 存储库 libaf_base.a（af_store.c）
# ============================================================================

AF_BASE_SRC    := src/af_store.c
AF_BASE_CFLAGS := -Isrc $(LIBDM_INC) $(DM_DEFS)
AF_BASE_LIB    := $(LIB)/libaf_base.a
$(eval $(call static_lib,AF_BASE))
