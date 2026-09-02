# Navigation Hierarchy (固件页面分级与返回规范)

> 生效日期：2026-08-29。此规范为硬性约束，不得随意更改。如需改动，必须同步更新本文档并通知所有开发者。

## 一、分级定义

每个页面有且仅有一个等级（Level），由 `s_view` 和 `s_prev_view` 共同维护返回链。

| 等级 | 页面 | `s_view` 值 | 父页面 |
|------|------|-------------|--------|
| 一级 | 主页 | `VIEW_LAUNCHER` | — |
| 二级 | 应用 | `VIEW_APPS` | 一级 |
| 二级 | 设置 | `VIEW_SETTINGS` | 一级 |
| 三级 | 语音输入 | `VIEW_VOICE` | 二级(应用) |
| 三级 | 网络与连接 | `VIEW_DEVICE_INFO` + `s_on_network_page=true` | 二级(设置) |
| 三级 | 设备信息 | `VIEW_DEVICE_INFO` | 二级(设置) |
| 三级 | 主题 | `VIEW_THEMES` | 二级(设置) |
| 四级 | WiFi 配网 | `VIEW_WIFI` | 三级(网络) |
| 四级 | 传输 | `VIEW_TRANSFER` | 三级(网络) |

## 二、按键行为

### 2.1 长按上键

| 当前等级 | 行为 |
|---------|------|
| 一级（主页） | 息屏（`bsp_display_backlight(0)`） |
| 二级 | 返回一级（`show_home()`） |
| 三级 | 返回二级父页面 |
| 四级 | 返回三级父页面 |

代码实现位置：`handle_key_event()` 中的 `BSP_BTN_UP + BSP_BTN_LONG` 分支。

### 2.2 长按 OK

| 当前等级 | 行为 |
|---------|------|
| 一级（主页） | 无操作 |
| 二级/三级/四级 | 返回一级（`show_home()`） |

### 2.3 短按 OK / 上下键

由各页面自己的 `handle_xxx_key()` 函数处理，不影响返回链。

## 三、页面函数规范

每个 `show_xxx()` 函数必须遵循以下模板：

```c
static void show_xxx(void)
{
    destroy_native_view();
    s_prev_view = s_view;          // ← 必须：保存当前页为上一级
    // ... 创建页面对象 ...
    s_view = VIEW_XXX;             // ← 必须：设置当前页等级
}
```

例外：`show_home()` 不需要设置 `s_prev_view`（一级页面无父页面）。

## 四、新增页面规则

### 4.1 新增二级页面（应用类）

在 `BUILTIN_APPS[]` 表中追加条目：

```c
static const builtin_app_t BUILTIN_APPS[] = {
    { "语音输入", show_voice },
    // 在此追加：{ "名称", 打开回调 },
};
```

打开回调的 `show_xxx()` 函数自动遵循三级→二级→一级的返回链。

### 4.2 新增三级/四级页面（设置类）

在对应父页面的按键处理中调用 `show_xxx()`，函数内按模板设置 `s_prev_view` 即可。

## 五、注意事项

1. `s_prev_view` 在 `handle_key_event` 的长按上键分支中**不得**被重置为 `VIEW_LAUNCHER`，否则多级返回会断裂。
2. `s_was_on_network` 仅在网络→WiFi/传输的返回路径中使用，返回网络页后立即设置 `s_prev_view = VIEW_SETTINGS` 以保证再次返回正确。
3. 网络与连接页使用 `s_view = VIEW_DEVICE_INFO` + `s_on_network_page = true` 来区分设备信息页，两者等级相同（三级），父页面均为设置页（二级）。
4. 息屏后仅 `BSP_BTN_PRESS` 事件可以唤醒，`BSP_BTN_RELEASE` 不触发唤醒。