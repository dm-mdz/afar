# ============================================================================
# 基础层纯 C 核心与独立线程 HTTPS 库 libaf_base.a（af_store + af_https_srv）
# ============================================================================

AF_BASE_SRC    := src/af_store.c \
                  src/base/af_https_srv.c \
                  src/base/af_https_conn.c
AF_BASE_CFLAGS := -Isrc $(LIBDM_INC) -Ipublic/third/llhttp/include $(MBEDTLS_INC) $(DM_DEFS)
AF_BASE_LIB    := $(LIB)/libaf_base.a
$(eval $(call static_lib,AF_BASE))
