idf_component_register(
    SRCS "recovery_boot_hook.c"
    PRIV_REQUIRES bootloader_support
    # ${IDF_PATH}(cmake 缓存变量,由 idf.py 传入,正斜杠形式)而非 $ENV{IDF_PATH}:
    # Windows 下环境变量是反斜杠路径,`C:\Users` 的 \U 会在 foreach 解析时报
    # Invalid character escape。语义二者等价,此处仅修复 Windows 本机构建。
    PRIV_INCLUDE_DIRS "${IDF_PATH}/components/bootloader_support/private_include"
)
