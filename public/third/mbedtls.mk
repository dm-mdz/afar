# ============================================================================
# mbedTLS 3.6.8 —— 上游只读三方源码（public/third/mbedtls），由 build.py 调 CMake 构建
# ============================================================================

MBEDTLS_DIR  := public/third/mbedtls
MBEDTLS_INC  := -I$(MBEDTLS_DIR)/include -Ipublic/third -DMBEDTLS_USER_CONFIG_FILE=\"mbedtls_user_config.h\"
MBEDTLS_LIBS := $(OUT)/mbedtls/library/libmbedtls.a \
                $(OUT)/mbedtls/library/libmbedx509.a \
                $(OUT)/mbedtls/library/libmbedcrypto.a

$(MBEDTLS_LIBS):
	@echo "$@ not found: run \"python3 build.py\" first" >&2; exit 1
