# ============================================================================
# Mac 主二进制 afar-agent 装配清单（由顶层 Makefile 置于全部模块 .mk 之后加载）
# ============================================================================

APP_SRC     := src/main.c
APP_CFLAGS  := -Isrc -Ishell $(LIBDM_INC) $(DMC_INC) $(DM_DEFS)
APP_LIBS    := $(AF_APP_EXT_LIB) $(AF_SHELL_LIB) $(AF_SESSION_LIB) $(AF_SETUP_LIB) $(AF_KBD_LIB) $(AF_DISPLAY_LIB) $(AF_SIDECAR_LIB) $(AF_POWER_LIB) $(AF_ENV_LIB) $(AF_AUTH_LIB) $(AF_BASE_LIB) $(AF_PLAT_LIB) $(DMC_CLIENT_LIBS) $(LIBDM_LIBS)
APP_LDFLAGS := $(AF_PLAT_LDFLAGS)
APP_BIN     := $(BIN)/afar-agent

$(eval $(call program,APP))
