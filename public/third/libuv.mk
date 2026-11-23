# ============================================================================
# libuv —— 上游只读三方源码（public/third/libuv），由 build.py 调 CMake 构建
# ============================================================================

LIBUV_DIR := public/third/libuv
LIBUV_INC := -I$(LIBUV_DIR)/include
LIBUV_LIB := $(OUT)/libuv/libuv.a

$(LIBUV_LIB):
	@echo "$(LIBUV_LIB) not found: run \"python3 build.py\" first" >&2; exit 1
