# EEGO A4 管理前端（web/）

React 18 + TypeScript（strict）+ Vite 编写的设备管理界面：仪表盘可视化编辑、模板库、
数据源向导、凭据、设备设置、告警、系统与屏幕预览。中文优先，可切换英文；浅色/深色主题。

## 运行架构

```
浏览器 ──GET /──────────────▶ 设备（ESP32-S3）: 加载页 + /loader.js（固件内置）
        ──GET version.json ─▶ GitHub Pages  https://huangzheng2016.github.io/eego-a4/
        ──GET assets/app.{js,css}?v=<version> ─▶ GitHub Pages
        ──/api/v1/*（同源）──▶ 设备 REST API（Bearer token，sessionStorage）
```

* 应用运行在**设备的 origin** 上，所有 API 都是相对路径 `/api/v1/...`，不需要 CORS。
* 构建产物使用固定文件名 `assets/app.js`、`assets/app.css`（单个 JS chunk，无 hash），
  外加 `version.json`。加载页按 `version || build || hash` 拼接 `?v=` 做缓存失效；
  `version` 字段为 `<package version>+<内容 hash>`，每次内容变化都会变。
* 应用启动时立即设置 `window.__EEGO_APP_LOADED = true`；加载页 8 秒内未看到该标志
  则显示指向 `/legacy`（设备内置简易页面）的链接。
* `loader/index.html`、`loader/loader.js` 是固件内置加载页的**参考副本**
  （权威版本在 `firmware/components/admin/src/admin_page.cpp`），保持两者一致。

### 设备加载页所需 CSP

加载页不含任何内联脚本/样式，React 仅通过 CSSOM 设置 `style` 属性（不受 `style-src`
限制），因此不需要 `'unsafe-inline'` 或 hash：

```
default-src 'self';
script-src  'self' https://huangzheng2016.github.io;
style-src   'self' https://huangzheng2016.github.io;
connect-src 'self' https://huangzheng2016.github.io;
img-src     'self' data: blob: https://huangzheng2016.github.io;
font-src    'self' data: https://huangzheng2016.github.io;
object-src 'none'; base-uri 'self'; frame-ancestors 'none'
```

* `connect-src` 需要 Pages origin 以读取 `version.json`（GitHub Pages 返回
  `Access-Control-Allow-Origin: *`）。
* `img-src blob:` 用于屏幕预览（`/api/v1/display/snapshot.bmp` 以 Bearer 拉取后转
  object URL）。
* 不要给应用注入 `<style>` 标签的 CSS-in-JS 库，否则会被 `style-src` 拦截。

## 开发

```sh
cd web
npm install

# 1) 纯前端 mock（无需硬件）：浏览器内拦截 /api/v1，密码 admin
npm run dev:mock            # 等价于 VITE_MOCK=1 vite

# 2) 连接真实设备：Vite 把 /api 代理到设备（并移除 Origin 头以通过设备的同源校验）
VITE_DEVICE=http://192.168.1.50 npm run dev
```

Mock 模式的模板库直接读取 `../firmware/presets/`，数据源抓取/预览返回内置示例 JSON。
Mock 代码通过 `import.meta.env.VITE_MOCK` 动态导入，生产构建中会被完全移除。

常用脚本：

| 命令 | 作用 |
|---|---|
| `npm run build` | 类型检查 + 构建到 `dist/` + 写 `version.json` |
| `npm test` | vitest 单元测试（网格吸附/重叠、JSONPath、bundle 往返、撤销历史） |
| `npm run lint` | ESLint |
| `npm run size` | 打印产物 gzip 体积（预算 200 KiB） |

## 代码结构

```
src/
  api/index.ts        所有 REST 端点（唯一的 API 入口，类型化）
  api/client.ts       fetch 封装：Bearer、If-Match、错误信封 {error:{code,message,fields}}
  api/mock.ts         开发用 mock 后端
  model/              纯逻辑：types（对应 dashboard_core/model.h）、grid（屏幕/网格规则）、
                      bundle（eego.dashboard-bundle/1 解析、预设展开、模板合并）、
                      jsonpath、errors（校验字段路径 → 编辑器对象）、metrics
  state/              session、draft（草稿/已应用、撤销重做、校验/应用/冲突）、toast、路由
  pages/editor/       画布（拖动/缩放/键盘微调）、面板属性、页面设置
  pages/*.tsx         模板库、数据源向导、凭据、设备设置、告警、系统、屏幕预览
  i18n/               zh（默认）/ en 字典；术语见 i18n/index.tsx 顶部注释
  ui/                 小型组件库（按钮、表单、弹窗、JSON 树、图标）
```

### 布局规则（与固件一致）

每屏 552×768，内容区 496×712（`dashboard_ui/geometry.h kDashboardContent`），8px 间距。
`grid_columns` 按比例映射到内容宽度；每屏固定 8 个网格行，第 n 屏为 `y ∈ [8n, 8n+8)`。
可见面板不得重叠、不得跨屏（`h ≤ 8` 且 `y % 8 + h ≤ 8`）。`row_height` 对设备无效。
编辑器拒绝非法放置（红色虚影 + 提示），并在本地实时标出布局问题；最终以设备
`/dashboard/validate` 为准。常量集中在 `src/model/grid.ts`。

### 草稿与应用

编辑都发生在本地草稿上（自动保存到 localStorage，可恢复）。“校验”调用
`POST /dashboard/validate`；“应用到设备”以 `If-Match: <revision>` `PUT /dashboard`
（未配置时 `POST`）。409 时弹出冲突对话框：加载设备版本或以最新 revision 覆盖。
校验错误（如 `dashboard.pages[0].panels[2].grid`）映射到对应页面/面板/数据源字段。

### 限速

设备对 `sources/test` 与 `sources/probe` 各限制 5 次/分钟/令牌；前端在本地计数并显示
等待秒数，收到 429 时按 `Retry-After` 冷却。
