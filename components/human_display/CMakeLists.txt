# 帧源文件由 tools/prep_pet.py 自动生成到 human/<角色>/ 下，数量多且随角色变化。
# 用 CONFIGURE_DEPENDS 递归收集，新增/重生成帧后无需再手改本文件——重新 build 即自动纳入。
file(GLOB_RECURSE HUMAN_FRAME_SRCS CONFIGURE_DEPENDS "human/*.c")

idf_component_register(
    SRCS "human_display.c" ${HUMAN_FRAME_SRCS}
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
