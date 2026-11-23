# ============================================================================
# Mac 壳库 libaf_shell.a（严守 shell/ 头文件白名单）
# ============================================================================

AF_SHELL_SRC    := $(wildcard shell/*.m)
AF_SHELL_CFLAGS := -Isrc -Ishell $(LIBDM_INC) $(DMC_INC) $(DM_DEFS)
AF_SHELL_LIB    := $(LIB)/libaf_shell.a
$(eval $(call static_lib,AF_SHELL))
