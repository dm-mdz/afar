# ============================================================================
# Afar_C 裁剪版 libdm —— 仅保留 core / json / net 三库无环拓扑（DD-01 §1）：
#   1. libdm_core.a  (infra/ + runtime/ + os/)     依赖：libuv.a
#   2. libdm_json.a  (storage/json.c + yyjson)     依赖：libdm_core.a
#   3. libdm_net.a   (transport/ + rpc/)           依赖：libdm_json.a, libdm_core.a, libllhttp.a, mbedtls, libuv.a
# ============================================================================

LIBDM_DIR := public/dm/libdm

# 公共头路径：供 src/、public/dm/plug/、黑盒测试使用（零 LIBUV_INC！）
LIBDM_INC := -I$(LIBDM_DIR)/include -Ipublic/third/yyjson/src

# 内部头路径：供 libdm 自身构建与白盒单测使用
LIBDM_PRIV_INC := $(LIBDM_INC) -I$(LIBDM_DIR)/src -Ipublic/third/llhttp/include \
                  $(MBEDTLS_INC) $(LIBUV_INC)

# ---- 第三方 llhttp 静态库 ----
LLHTTP_SRC    := public/third/llhttp/src/llhttp.c \
                 public/third/llhttp/src/api.c \
                 public/third/llhttp/src/http.c
LLHTTP_CFLAGS := -Ipublic/third/llhttp/include
LLHTTP_LIB    := $(LIB)/libllhttp.a
$(eval $(call static_lib,LLHTTP))
$(call obj_of,$(LLHTTP_SRC)): WARN :=

# ---- 1. libdm_core.a ----
LIBDM_CORE_SRC    := $(call rwildcard,$(LIBDM_DIR)/src/infra/,*.c) \
                     $(call rwildcard,$(LIBDM_DIR)/src/runtime/,*.c) \
                     $(call rwildcard,$(LIBDM_DIR)/src/os/,*.c)
LIBDM_CORE_CFLAGS := $(LIBDM_INC) -I$(LIBDM_DIR)/src $(LIBUV_INC) $(DM_DEFS)
LIBDM_CORE_LIB    := $(LIB)/libdm_core.a
$(eval $(call static_lib,LIBDM_CORE))

# ---- 2. libdm_json.a ----
YYJSON_SRC        := public/third/yyjson/src/yyjson.c
LIBDM_JSON_SRC    := $(LIBDM_DIR)/src/storage/json.c $(YYJSON_SRC)
LIBDM_JSON_CFLAGS := $(LIBDM_INC) -I$(LIBDM_DIR)/src $(DM_DEFS)
LIBDM_JSON_LIB    := $(LIB)/libdm_json.a
$(eval $(call static_lib,LIBDM_JSON))
$(call obj_of,$(YYJSON_SRC)): WARN :=

# ---- 3. libdm_net.a（含独立 https_srv.c 所需 MBEDTLS_INC） ----
LIBDM_NET_SRC    := $(call rwildcard,$(LIBDM_DIR)/src/transport/,*.c) \
                    $(call rwildcard,$(LIBDM_DIR)/src/rpc/,*.c)
LIBDM_NET_CFLAGS := $(LIBDM_INC) -I$(LIBDM_DIR)/src -Ipublic/third/llhttp/include \
                    $(MBEDTLS_INC) $(LIBUV_INC) $(DM_DEFS)
LIBDM_NET_LIB    := $(LIB)/libdm_net.a
$(eval $(call static_lib,LIBDM_NET))

# 全量单向静态链接序列（由上至下无环）
LIBDM_LIBS := $(LIBDM_NET_LIB) $(LIBDM_JSON_LIB) $(LIBDM_CORE_LIB) \
              $(LLHTTP_LIB) $(MBEDTLS_LIBS) $(LIBUV_LIB)
