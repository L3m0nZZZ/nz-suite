# NZ 广播（nzg）

Android 电视盒子上的电台应用，`com.shoubo.radio`。从 `nzp` 整体 fork 过来，保活那一套全保留，只把 `litenews://` 解析器改成先查广播端点。

台少但稳：内置 28 个台，山东 18 个，剩下是河南/安徽/内蒙古/广东的县台。

## 跟 nzp 的区别

| 项 | nzp | nzg |
|---|---|---|
| 应用名 | NZ播放器 | NZ广播 |
| 包名 | `com.shoubo.tv` | `com.shoubo.radio` |
| 配置目录 | `/sdcard/守播/` | `/sdcard/NZ广播/` |
| 远程服务端口 | 60080 | 60081 |
| litenews 解析 | 只查 tv 端点 | 先查 radio，未命中回落 tv |

包名不同，所以两个 App 能装在同一台盒子上共存，各跑各的。

完整差异清单、改动点、运维注意事项：**[docs/NZG.md](docs/NZG.md)**

## 构建

```bash
python build_nzg.py v0.1.x_xx台_20261105
```

产物落在 `D:/ningz/nzg_versions/<标签>/`，含 APK + `deploy_config.json` + `channels.json` + 源码快照。
脚本里写死了 JDK / SDK / Gradle 的绝对路径（`D:/ningz/.buildtools/`），换机器要改这几行。
