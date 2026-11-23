# ============================================================================
# 服务层 af_power 静态库 libaf_power.a（ModID 19）
# ============================================================================

AF_POWER_SRC    := src/power/af_power_mod.c
AF_POWER_CFLAGS := -Isrc $(LIBDM_INC) $(DM_DEFS)
AF_POWER_LIB    := $(LIB)/libaf_power.a
$(eval $(call static_lib,AF_POWER))
