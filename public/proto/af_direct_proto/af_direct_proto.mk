# ============================================================================
# WP-1A: MIT 纯 C11 直连共享协议库（零 OS / 零 libdm / 零加密库依赖）
# ============================================================================

AF_DIRECT_PROTO_INC    := -Ipublic/proto/af_direct_proto
AF_DIRECT_PROTO_SRC    := public/proto/af_direct_proto/af_direct_proto.c \
                          public/proto/af_direct_proto/af_vid_udp.c
AF_DIRECT_PROTO_CFLAGS := $(AF_DIRECT_PROTO_INC)
AF_DIRECT_PROTO_LIB    := $(LIB)/libaf_direct_proto.a

$(eval $(call static_lib,AF_DIRECT_PROTO))
