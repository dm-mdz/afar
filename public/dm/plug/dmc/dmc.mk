# ============================================================================
# dmc —— 本机 CLI/唤醒客户端 SDK（libdmc_proto.a + libdmc_client.a，DM v2.1.0）
# ============================================================================

DMC_DIR := public/dm/plug/dmc
DMC_INC := -I$(DMC_DIR)/include -I$(LIBDM_DIR)/include

LIBDMC_PROTO_SRC    := $(DMC_DIR)/src/dmc_proto.c $(DMC_DIR)/src/dmc_frame_sync.c
LIBDMC_PROTO_CFLAGS := $(DMC_INC) $(DM_DEFS)
LIBDMC_PROTO_LIB    := $(LIB)/libdmc_proto.a
$(eval $(call static_lib,LIBDMC_PROTO))

LIBDMC_CLIENT_SRC    := $(DMC_DIR)/src/dmc_transport.c \
                        $(DMC_DIR)/src/dmc_transport_serial.c \
                        $(if $(filter linux,$(OS_TARGET)),$(DMC_DIR)/src/dmc_serial_linux_baud.c) \
                        $(DMC_DIR)/src/dmc_client.c \
                        $(DMC_DIR)/src/dmc_client_rpc.c
LIBDMC_CLIENT_CFLAGS := $(DMC_INC) -Ipublic/third/yyjson/src $(DM_DEFS)
LIBDMC_CLIENT_LIB    := $(LIB)/libdmc_client.a
$(eval $(call static_lib,LIBDMC_CLIENT))

DMC_CLIENT_LIBS := $(LIBDMC_CLIENT_LIB) $(LIBDMC_PROTO_LIB)
