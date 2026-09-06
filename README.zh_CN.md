<p align="center">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# AI 随身话友 (Pocket AI Chat)

按住按键说一句话，AI Passport **开口回答**——对话像聊天一样以气泡显示在屏幕上。
它把设备变成一只口袋话友：放学路上、睡前躺床上，随口一问就有回应。

基于 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) 开发基线构建
（特性分支 `feature/ai-voice-chat`）。

## 它能做什么

- **按住说话**——按住【下】键讲话，松开即发送。
- **开口回答**——云端识别语音，OpenAI 兼容大模型生成回答，TTS 合成后从喇叭播出。
- **聊天气泡**——中文界面渲染对话；上下键翻看历史；它记得最近几轮，可以接着聊。
- **随手可控**——OK 短按打断回答，OK 长按返回菜单；应用就是主菜单里的一个「AI Chat」入口。

## 快速开始（直接烧录）

从 [Releases](https://github.com/xmwdsj666/ai-passport/releases) 页面（或本地构建的
`build/` 目录）取 `FoloToy-AI-Passport-full.bin`，把**合并镜像烧到 0x0**：

```bash
python -m esptool --chip esp32c3 -p <端口> -b 460800 write-flash 0x0 FoloToy-AI-Passport-full.bin
```

> 合并镜像已包含引导、分区表与应用。切勿把只含应用的 `FoloToy-AI-Passport.bin` 烧到 `0x0`。

首次使用前，先用 `idf.py menuconfig` 配置 WiFi 与 AI 服务密钥（见下节「配置」）——
这些设置保存在 `sdkconfig`，不会提交到仓库。

## 配置

全部配置在 `idf.py menuconfig` 的 **AI Voice Chat** 分组：

| 配置项 | 示例 | 说明 |
| --- | --- | --- |
| `AI_CHAT_WIFI_SSID` / `AI_CHAT_WIFI_PASSWORD` | `home-2g` | 2.4GHz 网络 |
| `AI_CHAT_BASE_URL` | `https://api.openai.com/v1` | OpenAI 兼容基础地址 |
| `AI_CHAT_API_KEY` | `sk-...` | Bearer 令牌；只存在设备里，不入仓库 |
| `AI_CHAT_LLM_MODEL` | `gpt-4o-mini` | `/chat/completions` 使用 |
| `AI_CHAT_ASR_MODEL` | `whisper-1` | `/audio/transcriptions` 使用 |
| `AI_CHAT_TTS_MODEL` / `AI_CHAT_TTS_VOICE` | `tts-1` / `alloy` | `/audio/speech` 使用，需 WAV 输出 |
| `AI_CHAT_REC_MAX_SEC` | `4` | 录音上限（内存保护） |

服务商速查（均为 OpenAI 兼容接口）：

| 服务商 | BASE_URL | 对话模型 | 识别模型 | 合成模型 |
| --- | --- | --- | --- | --- |
| OpenAI | `https://api.openai.com/v1` | `gpt-4o-mini` | `whisper-1` | `tts-1` |
| 智谱 GLM | `https://open.bigmodel.cn/api/paas/v4` | `glm-4-flash` | `glm-asr` | `cogtts` |
| SiliconFlow | `https://api.siliconflow.cn/v1` | `Qwen/Qwen2.5-7B-Instruct` | `FunAudioLLM/SenseVoiceSmall` | `fishaudio/fish-speech-1.5` |

TTS 必须支持 `response_format: "wav"`；本设备不做 MP3 解码。

## 从源码构建

- 必须使用 ESP-IDF **v5.5.3**（环境搭建见上游
  [环境引导文档](docs/development/engineering/environment-setup.zh_CN.md)）。

```bash
idf.py set-target esp32c3
idf.py build
./tools/validate.sh --static    # 仓库检查 + 主机逻辑测试
./tools/validate.sh --firmware  # 隔离构建 + 合并镜像校验
```

主机测试覆盖纯逻辑核心（`main/chat_core.c`：状态机、会话历史、请求组装、响应解析、
WAV 探测、multipart 构造），无需硬件即可运行。

## 串口截图协议

固件实现 `FAP_SCREENSHOT_V1`：从控制台串口发送该 ASCII 命令，设备把当前屏幕以
RGB565LE 像素回传。协议纯观测——不重启、不改配置、不暴露任何密钥。

## 工程结构（本特性分支）

| 路径 | 职责 |
| --- | --- |
| `main/chat_core.c/.h` | 纯逻辑：状态机/历史/JSON/WAV/multipart（主机可测） |
| `main/wifi_sta.c/.h` | 按需 WiFi STA 连接/断开 |
| `main/ai_client.c/.h` | ASR（multipart WAV 上传）/ 对话 / 流式 TTS |
| `main/voice_pipeline.c/.h` | 录音→识别→回答→播放 worker，带堆水位保护 |
| `main/ui_chat.c/.h` | 对话气泡、状态栏、电量 |
| `main/demo_ai_chat.c` | 菜单页面接线 |
| `main/font_ai_chat_16.c/.h` | 16px 中文字体子集（GB2312 一级字 + ASCII） |
| `main/serial_screenshot.c/.h` | `FAP_SCREENSHOT_V1` 监听 |
| `tests/test_chat_core.c` | 纯逻辑核心主机测试 |

## 已知限制

- 回答为非流式（一次 JSON 往返），长回答需要等待片刻。
- 对话历史在内存中（最近 8 条），重启后清空。
- 内置字体覆盖 GB2312 一级字（3755 常用字），生僻字会显示为方框；默认系统提示词
  已要求模型只用常用字。
- 受无 PSRAM 内存预算限制，录音默认上限 4 秒。

## 许可与致谢

- 基线与 BSP：[FoloToy/ai-passport](https://github.com/FoloToy/ai-passport)（MIT 许可）。
- 本应用同为 MIT。由 AI 编程助手协作完成；串口截图协议遵循 FoloToy 社区发布助手规范。
- 内置中文字体由本机安装的系统字体（黑体）派生，仅限个人使用；如需商业分发请用
  自有字体重新生成。
