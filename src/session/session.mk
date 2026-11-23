# ============================================================================
# 业务层会话编排静态库 libaf_session.a（DD-05 §1）
# ============================================================================

AF_SESSION_SRC    := src/session/af_session_front.c \
                     src/session/af_session_http.c \
                     src/session/af_session_mod.c
AF_SESSION_CFLAGS := -Isrc $(LIBDM_INC) $(DM_DEFS)
AF_SESSION_LIB    := $(LIB)/libaf_session.a
$(eval $(call static_lib,AF_SESSION))
