<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# 口袋话友 — 时钟 · 天气 · 日历 · AI 对话

FoloToy AI Passport 的一体化日常伴侣固件：开机直进主页——大时钟、农历、节日、实时天气；
还能对话——按住按键说话，回答直接从喇叭播出。

基于 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) 基线构建
（分支 `feature/standalone-app`）。

## 页面与按键

| 按键 | 动作 |
| --- | --- |
| 上/下（短按） | 切换页面：主页 → 天气 → 日历 → AI 对话 |
| OK（短按） | 页面动作（天气页=立即刷新；AI 对话页=停止播放） |
| OK（长按） | 回主页 |
| 下键（按住，对话页内） | 说话——松开即发送 |

- **主页**：大时钟（网络对时）、日期星期、农历、节日、天气摘要、电池。
- **天气**：实况（温度/体感/湿度/风力）+ 三日预报，数据来自
  [和风天气](https://dev.qweather.com)，30 分钟自动刷新，NVS 缓存断电/断网不丢。
- **日历**：当月网格 + 农历 + 节日 + 法定节假日/调休角标（内置 2026 数据表）；上下键翻月。
- **AI 对话**：按住下键说话 → 云端识别 → 大模型回答 → 喇叭播出，气泡滚动显示。

## 烧录（合并镜像 @ 0x0）

```bash
python -m esptool --chip esp32c3 -p <端口> -b 460800 write-flash 0x0 FoloToy-AI-Passport-full.bin
```

## 配置（`idf.py menuconfig`）

**AI Voice Chat** — WiFi、OpenAI 兼容 BASE_URL / API Key / 模型名，详见 `main/Kconfig.projbuild`。

**Weather & Time**

| 配置项 | 示例 | 说明 |
| --- | --- | --- |
| `QWEATHER_API_KEY` | `xxxxxxxx` | [和风控制台](https://console.qweather.com)获取 |
| `QWEATHER_HOST` | `abc123.xyz.qweatherapi.com` | **专属** API Host（控制台-设置查看，新账号必填） |
| `QWEATHER_LOCATION_ID` | `101010100` | 北京；其他城市见和风文档 |
| `QWEATHER_LOCATION_NAME` | `北京` | 屏显城市名 |
| `QWEATHER_REFRESH_MIN` | `30` | 自动刷新间隔（分钟） |
| `TIME_SYNC_NTP_SERVER` | `ntp.aliyun.com` | NTP 服务器 |
| `TIME_SYNC_TZ_OFFSET` | `8` | UTC 偏移（小时） |

Key 与 WiFi 凭证只保存在本机 `sdkconfig`，不提交仓库。
TTS 需支持 `response_format: "wav"`；天气仅使用和风免费接口（实时 + 3 天预报）。

## 从源码构建

- 必须使用 ESP-IDF **v5.5.3**。

```bash
idf.py set-target esp32c3
idf.py build
./tools/validate.sh --static    # 仓库检查 + 主机测试（农历/天气解析/对话核心）
./tools/validate.sh --firmware  # 隔离构建 + 合并镜像校验
```

## 工程结构（本分支）

| 路径 | 职责 |
| --- | --- |
| `main/main.c` | 应用外壳：初始化、页面路由、网络启动任务 |
| `main/page_home.c` | 时钟/农历/节日/天气摘要 |
| `main/page_weather.c` | 实况 + 三日预报 |
| `main/page_calendar.c` | 月历网格（农历/休班角标） |
| `main/page_chat.c` + `ui_chat.c` | AI 语音对话页 |
| `main/lunar.c` | 公历↔农历、节日、2026 节假日（主机可测） |
| `main/qweather_parse.c` / `qweather.c` | 天气解析（主机可测）+ 拉取/缓存 |
| `main/time_sync.c` | SNTP 对时 |
| `main/chat_core.c` / `ai_client.c` / `voice_pipeline.c` / `wifi_sta.c` | 语音对话引擎（沿用） |

## 已知限制

- 天气与对时依赖 WiFi；未联网时主页时钟显示 `--:--` 直到同步成功。
- 节假日调休数据仅内置 **2026** 年，其他年份按普通星期语义显示。
- 字体覆盖 GB2312 一级字（3755 常用字），生僻字显示为方框。
- 回答非流式；对话历史在内存（最近 8 条），重启清空。

## 许可

- 基线与 BSP：[FoloToy/ai-passport](https://github.com/FoloToy/ai-passport)（MIT）。
- 本应用同为 MIT。内置中文字体由本机系统字体（黑体）派生，仅限个人使用。
