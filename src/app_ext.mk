# ============================================================================
# CLI 与老版本无损迁移库 libaf_app_ext.a（src/af_cli.c + src/af_migrate.c）
# ============================================================================

AF_APP_EXT_SRC    := src/af_cli.c src/af_migrate.c
AF_APP_EXT_CFLAGS := -Isrc -Ishell $(LIBDM_INC) $(DMC_INC) $(DM_DEFS)
AF_APP_EXT_LIB    := $(LIB)/libaf_app_ext.a
$(eval $(call static_lib,AF_APP_EXT))
