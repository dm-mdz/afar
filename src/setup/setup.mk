# ============================================================================
# 业务层配置向导库 libaf_setup.a（af_setup_scripts.c + af_setup_mod.c）
# ============================================================================

AF_SETUP_SRC    := src/setup/af_setup_scripts.c src/setup/af_setup_mod.c
AF_SETUP_CFLAGS := -Isrc $(LIBDM_INC) $(DM_DEFS)
AF_SETUP_LIB    := $(LIB)/libaf_setup.a
$(eval $(call static_lib,AF_SETUP))
