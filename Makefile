# ============================================================================
# Afar_C 顶层 Makefile —— 非递归统一构建，子目录 .mk 自动发现防并行冲突。
# 日常请使用 python3 build.py（支持 build/check/test/asan/tsan/aqua/stress/clean）。
# ============================================================================

.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

OS_TARGET ?= darwin
MODE      ?= debug
MEM       ?= 0
ASAN      ?= 0
TSAN      ?= 0
STRESS    ?= 0
AQUA      ?= 0

ifeq ($(filter debug release,$(MODE)),)
  $(error MODE must be debug or release, got '$(MODE)')
endif

CC ?= clang
AR ?= ar

OUT ?= build/$(OS_TARGET)-$(MODE)$(if $(filter 1,$(MEM)),-mem)$(if $(filter 1,$(ASAN)),-asan)$(if $(filter 1,$(TSAN)),-tsan)$(if $(filter 1,$(STRESS)),-stress)$(if $(filter 1,$(AQUA)),-aqua)
BIN := $(OUT)/bin
LIB := $(OUT)/lib
OBJ := $(OUT)/obj

STD      := -std=c11
OBJC_STD := -fobjc-arc
ifeq ($(MODE),release)
  OPT := -O3 -DNDEBUG
else
  OPT := -g
endif
DEPFLAGS := -MMD -MP
WARN     := -Wall -Wextra -Werror -ffile-prefix-map=$(CURDIR)=/afar
SYS_LIBS := -lpthread

SAN_CFLAGS :=
ifeq ($(ASAN),1)
  SAN_CFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
else ifeq ($(TSAN),1)
  SAN_CFLAGS := -fsanitize=thread -fno-omit-frame-pointer
endif
SAN_LDFLAGS := $(SAN_CFLAGS)

DM_DEFS := $(if $(filter 1,$(MEM)),-DDM_MEM_DEBUG) \
           $(if $(filter 1,$(STRESS)),-DAF_STRESS_TEST) \
           $(if $(filter 1,$(AQUA)),-DAF_AQUA_TEST)

# 递归通配与目标路径映射（支持 .c 与 .m）
rwildcard = $(foreach d,$(wildcard $(1)*),$(call rwildcard,$(d)/,$(2)) $(filter $(subst *,%,$(2)),$(d)))
obj_of    = $(patsubst %.m,$(OBJ)/%.m.o,$(patsubst %.c,$(OBJ)/%.o,$(1)))

# 静态库模板
define static_lib
$(1)_OBJ := $$(call obj_of,$$($(1)_SRC))
$$($(1)_OBJ): MOD_CFLAGS := $$($(1)_CFLAGS)
$$($(1)_LIB): $$($(1)_OBJ)
	@mkdir -p $$(dir $$@)
	@rm -f $$@
	$$(AR) rcs $$@ $$^
ALL_OBJ += $$($(1)_OBJ)
endef

# 可执行文件模板（支持可选 $(1)_LDFLAGS 传 Apple framework）
define program
$(1)_OBJ := $$(call obj_of,$$($(1)_SRC))
$$($(1)_OBJ): MOD_CFLAGS := $$($(1)_CFLAGS)
$$($(1)_BIN): $$($(1)_OBJ) $$($(1)_LIBS)
	@mkdir -p $$(dir $$@)
	$$(CC) $$(SAN_LDFLAGS) $$(LDFLAGS) -o $$@ $$($(1)_OBJ) $$($(1)_LIBS) $$($(1)_LDFLAGS) $$(SYS_LIBS)
ALL_OBJ += $$($(1)_OBJ)
endef

TEST_BINS      :=
AQUA_TEST_BINS :=
STRESS_BINS    :=
APP_BIN        :=

# ----------------------------------------------------------------------------
# 基础三方与框架清单（按单向无环顺序）
# ----------------------------------------------------------------------------
include public/third/libuv.mk
include public/third/mbedtls.mk
include public/dm/libdm/libdm.mk
include public/dm/plug/dmc/dmc.mk
include public/dm/plug/dmcmd/dmcmd.mk

# ----------------------------------------------------------------------------
# 业务与测试子目录 .mk 严格分层自动发现（并行工作包仅维护本目录 .mk，零冲突）：
#   子模块静态库 → 壳 → iPad 核 → src 其余 .mk → src/main.mk（最终装配）→ 测试 .mk
# ----------------------------------------------------------------------------
-include $(sort $(wildcard src/*/*.mk))
-include $(sort $(wildcard shell/*.mk))
-include $(sort $(filter-out src/main.mk,$(wildcard src/*.mk)))
-include $(wildcard src/main.mk)

# 编译规则放在所有 .mk 之后，确保 $(MAKEFILE_LIST) 捕获全部清单变更
$(OBJ)/%.o: %.c $(MAKEFILE_LIST)
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(OPT) $(DEPFLAGS) $(WARN) $(SAN_CFLAGS) $(MOD_CFLAGS) $(CFLAGS) -c $< -o $@

$(OBJ)/%.m.o: %.m $(MAKEFILE_LIST)
	@mkdir -p $(dir $@)
	$(CC) $(OBJC_STD) $(OPT) $(DEPFLAGS) $(WARN) $(SAN_CFLAGS) $(MOD_CFLAGS) $(CFLAGS) -c $< -o $@

.PHONY: all libs app tests aqua-tests stress-tests list-tests list-aqua-tests list-stress-tests

all: libs $(if $(APP_BIN),app) $(DMCMD_BIN)
libs: $(LIBUV_LIB) $(MBEDTLS_LIBS) $(LIBDM_NET_LIB) $(LIBDM_JSON_LIB) $(LIBDM_CORE_LIB) $(LLHTTP_LIB) $(LIBDMC_PROTO_LIB) $(LIBDMC_CLIENT_LIB)
app: $(APP_BIN)
tests: $(TEST_BINS)
aqua-tests: $(AQUA_TEST_BINS)
stress-tests: $(STRESS_BINS)

list-tests:
	@:$(foreach t,$(TEST_BINS),; echo "$(or $(word 2,$(subst /, ,$(patsubst $(OUT)/%,%,$(t)))),default) $(t)")
list-aqua-tests:
	@:$(foreach t,$(AQUA_TEST_BINS),; echo "$(or $(word 2,$(subst /, ,$(patsubst $(OUT)/%,%,$(t)))),default) $(t)")
list-stress-tests:
	@:$(foreach t,$(STRESS_BINS),; echo "$(or $(word 2,$(subst /, ,$(patsubst $(OUT)/%,%,$(t)))),default) $(t)")

-include $(ALL_OBJ:.o=.d)
