# 显示安全区

EEGO A4 使用 552×768 竖屏逻辑坐标，左上角为 `(0,0)`。安全区用于避开物理圆角
和装配遮挡，所有必读文字、图标、焦点框和按钮都必须完整可见。

## 几何合同

```text
物理外轮廓：552×768，R60
圆角安全区：x=12, y=12, w=528, h=744, r=48
普通 UI 区：x=28, y=28, w=496, h=712, r=0
```

圆角安全区来自：

```text
inner radius = outer radius - inset = 60 - 12 = 48 px
```

普通 UI 使用其中的统一零圆角矩形，而不是逐行计算圆角宽度：

```text
ceil(48 × (1 - 1/√2)) = 15 px
最小总边距 = 12 + 15 = 27 px，布局取 28 px
```

权威常量位于
[`eego_pins.h`](../firmware/components/eego_hal/include/eego_pins.h)。应用不要复制
magic number；重新标定硬件时应同步修改常量、页面和检查记录。

## 使用规则

当前 ESP-IDF 的 `PortraitCanvas` 通过 `useUiContentRect()` 设置普通 UI 裁剪：

```cpp
canvas.useUiContentRect();
canvas.clear();
canvas.drawRect(eego::UI_CONTENT_X, eego::UI_CONTENT_Y,
                eego::UI_CONTENT_WIDTH, eego::UI_CONTENT_HEIGHT, 0);
```

建议正文和标题再留 4 px 视觉内边距。裁剪只是兜底，布局不能故意让半个文字或按钮
落到区域外；文字右边界要按实际字体宽度计算。

只有全屏背景、显示波形/边缘测试、触控边缘测试或不承载信息的装饰可以越界。
需要完整面板时使用 `canvas.resetClip()`，完成后立即恢复
`canvas.useUiContentRect()`。例外不会传给子元素：全屏背景上的正文和按钮仍须
回到安全区。直接修改 `Uc8279cDisplay::framebuffer()` 会绕过画布裁剪。

修改几何后需重新构建固件，并在真机检查四角、文字和控件；物理圆角遮挡、装配
误差和残影不能由静态检查代替。
