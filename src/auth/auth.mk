# ============================================================================
# 服务层鉴权与配对库 libaf_auth.a（af_auth_cert/store/pair/mod.c）
# ============================================================================

AF_AUTH_SRC    := src/auth/af_auth_cert.c \
                  src/auth/af_auth_store.c \
                  src/auth/af_auth_pair.c \
                  src/auth/af_auth_mod.c
AF_AUTH_CFLAGS := -Isrc $(LIBDM_INC) $(MBEDTLS_INC) $(DM_DEFS)
AF_AUTH_LIB    := $(LIB)/libaf_auth.a
$(eval $(call static_lib,AF_AUTH))
