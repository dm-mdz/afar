# ============================================================================
# dmcmd —— 本机口诊断与测试 CLI（不链 libuv）
# ============================================================================

DMCMD_DIR    := public/dm/plug/dmcmd
DMCMD_SRC    := $(wildcard $(DMCMD_DIR)/src/*.c)
DMCMD_CFLAGS := -I$(DMCMD_DIR)/src $(DMC_INC) -Ipublic/third/yyjson/src $(DM_DEFS)
DMCMD_LIBS   := $(DMC_CLIENT_LIBS) $(LIBDM_JSON_LIB)
DMCMD_BIN    := $(BIN)/dmcmd

$(eval $(call program,DMCMD))
