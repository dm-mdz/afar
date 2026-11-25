# ============================================================================
# 业务层会话编排静态库 libaf_session.a（DD-05 §1 + DD v2.1 §3.2）
# ============================================================================

AF_SESSION_SRC    := src/session/af_session_front.c \
                     src/session/af_session_http.c \
                     src/session/af_session_mod.c \
                     src/session/af_session_direct.c
AF_SESSION_CFLAGS := -Isrc $(LIBDM_INC) $(AF_DIRECT_PROTO_INC) $(MBEDTLS_INC) $(DM_DEFS)
AF_SESSION_LIB    := $(LIB)/libaf_session.a
$(eval $(call static_lib,AF_SESSION))
