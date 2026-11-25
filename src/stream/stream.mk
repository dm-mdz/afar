# ============================================================================
# 服务层 af_stream 静态库 libaf_stream.a（ModID 24，日常模式视频推流与编码编排）
# ============================================================================

AF_STREAM_SRC    := src/stream/af_stream_mod.c \
                    src/stream/af_stream_tx.c
AF_STREAM_CFLAGS := -Isrc $(LIBDM_INC) $(AF_DIRECT_PROTO_INC) $(MBEDTLS_INC) $(DM_DEFS)
AF_STREAM_LIB    := $(LIB)/libaf_stream.a
$(eval $(call static_lib,AF_STREAM))
