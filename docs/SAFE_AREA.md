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
[`EegoA4Hardware.h`](../lib/EegoA4Support/include/EegoA4Hardware.h)。应用不要复制
magic number；重新标定硬件时应同步修改常量、诊断页和测试。

## 使用规则

`PortraitCanvas` 默认裁剪到普通 UI 区，页面仍应显式声明：

```cpp
canvas.useUiContentRect();
canvas.clear();
canvas.setCursor(eego::UI_CONTENT_X + 4, eego::UI_CONTENT_Y + 4);
canvas.print("SAFE TITLE");
```

建议正文和标题再留 4 px 视觉内边距。裁剪只是兜底，布局不能故意让半个文字或按钮
落到区域外；文字右边界要按实际字体宽度计算。

只有全屏背景、显示波形/边缘测试、触控边缘测试或不承载信息的装饰可以越界：

```cpp
canvas.useFullPanel();
// 仅限本段硬件测试或背景绘制
canvas.useUiContentRect();
```

例外不会传给子元素：全屏背景上的正文和按钮仍须回到安全区。直接修改
`EInkDisplay::getFrameBuffer()` 会绕过裁剪，只能用于明确的底层显示测试。

运行 `python3 scripts/validate_project.py --quick` 检查共享几何和示例。旧诊断固件
可用 `screen safe` 查看边界，再用 `screenshot` 回读 framebuffer；物理圆角遮挡和
残影仍需肉眼确认。
