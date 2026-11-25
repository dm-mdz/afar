# ============================================================================
# 服务层 af_input 静态库 libaf_input.a（ModID 25，日常模式 UDP 实时输入与注入）
# ============================================================================

AF_INPUT_SRC    := src/input/af_input_mod.c \
                   src/input/af_input_inject.c
AF_INPUT_CFLAGS := -Isrc $(LIBDM_INC) $(AF_DIRECT_PROTO_INC) $(MBEDTLS_INC) $(DM_DEFS)
AF_INPUT_LIB    := $(LIB)/libaf_input.a
$(eval $(call static_lib,AF_INPUT))
