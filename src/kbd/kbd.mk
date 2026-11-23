# ============================================================================
# 服务层蓝牙键盘交接静态库 libaf_kbd.a（DD-04）
# ============================================================================

AF_KBD_SRC    := src/kbd/af_kbd_flow.c src/kbd/af_kbd_filter.c src/kbd/af_kbd_mod.c
AF_KBD_CFLAGS := -Isrc $(LIBDM_INC) $(DM_DEFS)
AF_KBD_LIB    := $(LIB)/libaf_kbd.a
$(eval $(call static_lib,AF_KBD))
