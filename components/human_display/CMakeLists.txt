# 帧源文件由 tools/prep_pet.py 自动生成到 human/<角色>/ 下，数量多且随角色变化。
# 递归收集 human/ 下所有 .c，新增/重生成帧后重新 build 即纳入
# （ESP-IDF 组件 CMake 不支持 CONFIGURE_DEPENDS；若新增帧未被识别，跑一次 `idf.py reconfigure`）。
file(GLOB_RECURSE HUMAN_FRAME_SRCS "human/*.c")

idf_component_register(
    SRCS "human_display.c" ${HUMAN_FRAME_SRCS}
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
