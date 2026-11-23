# ============================================================================
# 基础层 macOS 平台适配库 libaf_plat.a（ObjC，私有 API 严格收敛在 private/）
# ============================================================================

AF_PLAT_SRC     := $(wildcard src/plat/*.m src/plat/private/*.m)
AF_PLAT_CFLAGS  := -Isrc $(LIBDM_INC) $(DM_DEFS)
AF_PLAT_LIB     := $(LIB)/libaf_plat.a
AF_PLAT_LDFLAGS := -framework Cocoa -framework CoreGraphics -framework CoreFoundation \
                   -framework IOKit -framework IOBluetooth -framework CoreBluetooth \
                   -framework OpenDirectory -framework Security -framework WebKit \
                   -framework UserNotifications
$(eval $(call static_lib,AF_PLAT))
