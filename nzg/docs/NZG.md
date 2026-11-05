# NZ广播 (nzg) — 与 NZ播放器 (nzp) 的差异与运维说明

> 由 nzp 源码整体 fork（保留全部守播/保活/远程同步/热加载能力），仅做广播化改造。
> 生成日期 2026-09-11 · 版本 v0.1.1 · 13 台

## 1. 为什么要单独做一个 App

山东融媒（litenews）把**广播**和**电视**放在两个端点：

| 内容 | 端点 |
|---|---|
| 电视 | `GET https://app.litenews.cn/v1/app/play/tv/live?orgid=<n>` |
| 广播 | `GET https://app.litenews.cn/v1/app/play/radio/live?orgid=<n>` |

nzp 的 `litenews://` 解析器**只查 tv 端点**，所以**绝大多数县台的广播在 nzp 里根本取不到**
（少数账号如莱阳 241 会把“综合广播”混在 tv 端点里，那是例外）。
→ 广播独立成 nzg，解析器改查 radio 端点，职责单一、互不干扰。

## 2. 与 nzp 的差异（全量清单）

| 项 | nzp | nzg |
|---|---|---|
| 应用名 | NZ播放器 | **NZ广播** |
| 包名 / namespace | `com.shoubo.tv` | **`com.shoubo.radio`** |
| 源码包路径 | `java/com/shoubo/tv/**` | `java/com/shoubo/radio/**` |
| 版本 | 0.5.x | **0.1.0** |
| 配置目录 | `/sdcard/守播/config.json` | **`/sdcard/NZ广播/config.json`** |
| 日志目录 | `/sdcard/守播/logs/` | **`/sdcard/NZ广播/logs/`** |
| 远程频道服务端口 | 60080 | **60081**（避免与 nzp 撞端口） |
| 图标 | 深蓝电视 | **暖橙收音机** |
| 通知栏标题 | NZ播放器运行中 | **NZ广播运行中** |
| **litenews 解析器** | 只查 `tv` 端点 | **先查 `radio` 端点，未命中回落 `tv` 端点** |

> 包名不同 → 两个 App 可在同一台盒子上**共存**，各自独立配置、独立远程服务。
> adb 更新时注意 `adb push` 的目标路径是 `/sdcard/NZ广播/`，别推到 `/sdcard/守播/`。

## 3. 源码改动点（唯一一处逻辑改动）

`app/src/main/java/com/shoubo/radio/player/PlayerManager.kt`

- `resolveLiteAsync()`：按 `kind` 参数决定端点顺序，默认 `radio → tv` 依次尝试；
- 新增 `fetchLiteStream(orgid, kind, want)`：拉指定端点并按频道名子串匹配，返回 stream。

`litenews://` 动态源格式（nzg 版）：

```
litenews://<orgid>?name=<urlenc 频道名>                 # 默认：先 radio 后 tv
litenews://<orgid>?name=<urlenc 频道名>&kind=radio      # 强制只查广播端点
litenews://<orgid>?name=<urlenc 频道名>&kind=tv         # 强制只查电视端点
```

⚠️ 频道名仍是**子串匹配**（`nm.contains(want)`），同一 orgid 下 `name` 必须能唯一命中，
否则会串台 —— 这是本项目历史上最高频的错配来源，改配置时逐条核对。

## 3.5 稳定优先的源策略

每条频道 = `primary_url` 主源 + `backup_url` 备源，App 侧自动切换：

- **主源**：`litenews://`（官方融媒云）—— 播放前实时取签名，**不会因签名过期失效**，只要台里在推流就稳；
- **备源**：静态直链（蜻蜓FM `lhttp.qingting.fm/live/<id>/64k.mp3`、菏泽广电 `live.hznet.tv`）——
  **永不过期、不依赖平台**，官方断流时兜底。

菏泽这批当前带备源的：定陶 FM90.4、曹县 FM93.3、成武 FM103.1（蜻蜓）；
菏泽市级 3 台本身就是静态直链，直接做主源（更稳）。

## 4. 频道清单构成（v0.1.1 = 13 台）

**范围：菏泽全境 + 沈丘（沿用 nzp 里的沈丘听广播）**

| 分组 | 台数 | 内容 |
|---|---|---|
| 山东 / 菏泽（市级） | 3 | 新闻 FM87.5、音乐 FM91.8、交通 FM94.8 —— 菏泽广电 `live.hznet.tv` 静态直链，**最稳**，直接做主源 |
| 山东 / 菏泽（区县） | 9 | 定陶 FM90.4、巨野 FM94.1、牡丹 FM103.5、曹县 FM93.3、东明 FM90.2、成武 FM103.1、单县 FM88.8、鄄城 FM94.3、郓城 |
| 河南 / 沈丘 | 1 | 沈丘听广播 `hntv://shenqiu?cls=2`（xy-gw 签名接口，App 播放前实时取，token 约 4~6h） |

- 分类：菏泽= `category="山东"`；沈丘= `category="河南"`；`region` = 地市/区县（同级平铺）
- **不收录**：占位名频道（`频道名称`/纯频率号/`测试频道`）、沈丘看电视（属电视，nzp 已有）

### 扩展清单（备查，未启用）

山东全省县台广播共 **93 台**（含省级 6 + 其余 40 余县），已验证可解析的整份映射留在
`nzg_versions/v0.1.1_13台_20260911/src/扩展清单_山东全省93台.json`。
需要扩面时把它作为 `channels` 直接替换 `deploy_config.json` 即可（改配置热加载，无需重编译）。

### 实测结论（2026-09-11）

13 台中 **12 台主源三阶通过**（master→media playlist→ts 分片，实测分片 32~177KB）；唯一未通的是 **牡丹区资讯广播 FM103.5** ——
litenews 条目在（orgid 219，名 `牡丹之声—菏泽综合广播`）但**源头当前未推流（404）**，待复测。
沈丘听广播实测解析出 `xygbstream.hntv.tv/gb/.../playlist.m3u8`，三阶通过（ts 52KB×2）。
三条备源（定陶 / 成武 / 曹县 蜻蜓FM）实测 200 + `Content-Type: audio/mpeg` + 有真实音频数据。

### UI 决策（勿再改动）

**广播播放界面保持纯黑**（PlayerView 全屏黑底），**不加「正在播放」台名面板**。
—— 2026-09-11 用户明确："就要全黑面板就可以"。
曾试做过居中的「台名 + 声波图标」常驻面板，已按要求撤除（代码/布局/图标全部还原），
以后不要自作主张再加；需要看台名走「屏幕点击呼出侧边栏」或左下角状态栏（5 秒自动隐藏）。

> 验证脚本坑：mp3 是**无限流**，`urlopen(u).read()` 不传长度会**永不返回而挂死**（表现为脚本被 SIGTERM），必须限长读，如 `read(4096)`。


## 5. 构建

```bash
export JAVA_HOME="D:/ningz/.buildtools/x/jdk-17.0.20.1+1"
export ANDROID_HOME="D:/ningz/.buildtools/sdk"
cd D:/ningz/nzg
"D:/ningz/.buildtools/x/gradle-8.7/bin/gradle.bat" assembleDebug --no-daemon
# 产物：app/build/outputs/apk/debug/app-debug.apk
```

`local.properties` 已指向 `D:/ningz/.buildtools/sdk`。

## 6. 部署（与 nzp 同流程：改配置热加载，不重编译）

```bash
# 1) 推配置（改完自动热加载）
adb push deploy_config.json /sdcard/NZ广播/config.json
# 2) 首次安装 / 换包
adb install -r 守播_nzg_013_20260911.apk
# 3) Android 11+ 需授权"所有文件访问"才能直接改 /sdcard/NZ广播/
adb shell appops set com.shoubo.radio MANAGE_EXTERNAL_STORAGE allow
```

## 7. 归档

- 源码：`D:/ningz/nzg/`
- 版本目录：`D:/ningz/nzg_versions/v0.1.1_13台_20260911/`
  （APK + deploy_config.json + channels.json + src/生成器）
- 频道生成器：`gen_nzg.py`（拉 radio 端点 → 归属判定 → 生成三件套）
- 已知待复测：单县综合（tv）、牡丹之声 FM103.5（radio）当前源头未推流
