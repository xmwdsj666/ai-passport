<p align="center">
  <strong>English</strong> · <a href="README.zh_CN.md">简体中文</a>
</p>

# Pocket AI Chat

Hold a button, say anything, and the AI Passport answers **out loud** — your conversation appears
as chat bubbles on the screen. It turns the device into a pocket companion you can chat with on the
way home, before bed, or any time a quick answer or a silly joke makes the day better.

Built on the [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) development baseline
(feature branch `feature/ai-voice-chat`).

## What it does

- **Hold to talk** — press and hold the DOWN key, speak, release to send.
- **Spoken answers** — speech recognition in the cloud, an OpenAI-compatible chat model replies,
  and the answer is synthesized to the speaker.
- **Chat bubbles** — the conversation renders on screen with a CJK font; scroll UP/DOWN through
  history; the device remembers the last few turns for natural follow-ups.
- **Full control** — tap OK to cut an answer short, long-press OK to return to the menu; the app
  lives as a regular "AI Chat" entry in the baseline demo menu.

## Quick start (flash a ready image)

Grab `FoloToy-AI-Passport-full.bin` from the
[Releases](https://github.com/xmwdsj666/ai-passport/releases) page (or `build/` after a local
build), then flash the **merged image at offset 0x0**:

```bash
python -m esptool --chip esp32c3 -p <PORT> -b 460800 write-flash 0x0 FoloToy-AI-Passport-full.bin
```

> The merged image already contains the bootloader, partition table, and application.
> Never write the application-only `FoloToy-AI-Passport.bin` to `0x0`.

Before first use, set your Wi-Fi and AI service credentials with `idf.py menuconfig`
(see [Configuration](#configuration)) — the settings live in `sdkconfig`, which is not committed
to the repository.

## Configuration

Everything lives under **AI Voice Chat** in `idf.py menuconfig`:

| Option | Example | Notes |
| --- | --- | --- |
| `AI_CHAT_WIFI_SSID` / `AI_CHAT_WIFI_PASSWORD` | `home-2g` | 2.4 GHz network |
| `AI_CHAT_BASE_URL` | `https://api.openai.com/v1` | OpenAI-compatible base URL |
| `AI_CHAT_API_KEY` | `sk-...` | Bearer token; stays on your device, never committed |
| `AI_CHAT_LLM_MODEL` | `gpt-4o-mini` | used for `/chat/completions` |
| `AI_CHAT_ASR_MODEL` | `whisper-1` | used for `/audio/transcriptions` |
| `AI_CHAT_TTS_MODEL` / `AI_CHAT_TTS_VOICE` | `tts-1` / `alloy` | used for `/audio/speech`, WAV output |
| `AI_CHAT_REC_MAX_SEC` | `4` | recording cap (no-PSRAM memory guard) |

Provider cheat-sheet (all OpenAI-compatible):

| Provider | BASE_URL | LLM | ASR | TTS |
| --- | --- | --- | --- | --- |
| OpenAI | `https://api.openai.com/v1` | `gpt-4o-mini` | `whisper-1` | `tts-1` |
| Zhipu GLM | `https://open.bigmodel.cn/api/paas/v4` | `glm-4-flash` | `glm-asr` | `cogtts` |
| SiliconFlow | `https://api.siliconflow.cn/v1` | `Qwen/Qwen2.5-7B-Instruct` | `FunAudioLLM/SenseVoiceSmall` | `fishaudio/fish-speech-1.5` |

TTS must support `response_format: "wav"`; MP3 decoding is out of scope on this device.

## Build from source

- ESP-IDF **v5.5.3** exactly (see the upstream
  [environment bootstrap guide](docs/development/engineering/environment-setup.md)).

```bash
idf.py set-target esp32c3
idf.py build
./tools/validate.sh --static    # repository checks + host logic tests
./tools/validate.sh --firmware  # isolated build + merged-image verification
```

Host tests cover the pure logic core (`main/chat_core.c`: state machine, history, request
assembly, response parsing, WAV probing, multipart building) and run without hardware.

## Serial screenshot protocol

The firmware implements `FAP_SCREENSHOT_V1`: send the ASCII command over the console serial port
and the device replies with the active screen as RGB565LE pixels. It is observation-only — no
reboot, no setting changes, no secrets.

## Project layout (this feature branch)

| Path | Role |
| --- | --- |
| `main/chat_core.c/.h` | Pure state machine / history / JSON / WAV / multipart (host-tested) |
| `main/wifi_sta.c/.h` | On-demand Wi-Fi STA connect/disconnect |
| `main/ai_client.c/.h` | ASR (multipart WAV upload) / chat / streaming TTS |
| `main/voice_pipeline.c/.h` | Record → ASR → LLM → TTS worker with heap-watermark guard |
| `main/ui_chat.c/.h` | Chat bubbles, status bar, battery |
| `main/demo_ai_chat.c` | Menu page wiring |
| `main/font_ai_chat_16.c/.h` | 16 px Chinese font subset (GB2312 level-1 + ASCII) |
| `main/serial_screenshot.c/.h` | `FAP_SCREENSHOT_V1` listener |
| `tests/test_chat_core.c` | Host tests for the pure logic core |

## Known limitations

- Answers are generated non-streaming (one JSON round trip); long replies take a moment.
- Conversation history lives in RAM (last 8 messages); it clears on reboot.
- The bundled font covers GB2312 level-1 (3755 common characters); rare characters render as
  boxes. The default system prompt asks the model to stay within common characters.
- Recording is capped (default 4 s) by the no-PSRAM memory budget.

## License & credits

- Baseline and BSP: [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) (MIT License).
- This application: MIT as well. Prepared with an AI coding assistant; the serial screenshot
  protocol follows the FoloToy community publisher specification.
- The embedded Chinese font is derived from a locally installed system font (SimHei) and is
  intended for personal use; regenerate it with your own font before commercial distribution.
