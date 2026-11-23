# ============================================================================
# 服务层系统环境只读探测库 libaf_env.a（af_env_mod.c）
# ============================================================================

AF_ENV_SRC    := src/env/af_env_mod.c
AF_ENV_CFLAGS := -Isrc $(LIBDM_INC) $(DM_DEFS)
AF_ENV_LIB    := $(LIB)/libaf_env.a
$(eval $(call static_lib,AF_ENV))
