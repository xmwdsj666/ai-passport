<p align="right">
  <strong>English</strong> · <a href="README.zh_CN.md">简体中文</a>
</p>

# Pocket Buddy — Clock · Weather · Calendar · AI Chat

An all-in everyday companion firmware for the FoloToy AI Passport. It boots straight into a
home screen with a big clock, the lunar date, festivals, and live weather — and it still talks:
hold a button, say anything, and the answer is spoken back.

Built on the [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) baseline
(branch `feature/standalone-app`).

## Pages & keys

| Key | Action |
| --- | --- |
| UP / DOWN (short) | Switch page: Home → Weather → Calendar → AI Chat |
| OK (short) | Page action (Weather: refresh now; AI Chat: stop playback) |
| OK (long) | Back to Home |
| DOWN (hold, in AI Chat) | Talk — release to send |

- **Home** — big clock (SNTP-synced), date/weekday, lunar date, festivals, weather summary, battery.
- **Weather** — current conditions (temp / feels-like / humidity / wind) + a 3-day forecast,
  powered by [QWeather](https://dev.qweather.com) with a 30-minute auto refresh and an NVS cache
  that survives reboots and offline starts.
- **Calendar** — month grid with lunar days, festivals, and statutory holiday / makeup-workday
  badges (2026 table built in); UP/DOWN flips months.
- **AI Chat** — hold DOWN to talk, cloud speech-to-text, an OpenAI-compatible model replies,
  and text-to-speech plays the answer out loud.

## Flash a ready image

Flash the merged image `FoloToy-AI-Passport-full.bin` **at offset 0x0**:

```bash
python -m esptool --chip esp32c3 -p <PORT> -b 460800 write-flash 0x0 FoloToy-AI-Passport-full.bin
```

## Configuration (`idf.py menuconfig`)

**AI Voice Chat** — Wi-Fi credentials, OpenAI-compatible BASE_URL / API key / models
(see `main/Kconfig.projbuild` for the full list; provider cheat-sheet in the Chinese README).

**Weather & Time**

| Option | Example | Notes |
| --- | --- | --- |
| `QWEATHER_API_KEY` | `xxxxxxxx` | from [console.qweather.com](https://console.qweather.com) |
| `QWEATHER_HOST` | `abc123.xyz.qweatherapi.com` | your **dedicated** API Host from console settings (new accounts) |
| `QWEATHER_LOCATION_ID` | `101010100` | Beijing; see QWeather docs for other cities |
| `QWEATHER_LOCATION_NAME` | `Beijing` | label shown on screen |
| `QWEATHER_REFRESH_MIN` | `30` | auto refresh interval |
| `TIME_SYNC_NTP_SERVER` | `ntp.aliyun.com` | SNTP server |
| `TIME_SYNC_TZ_OFFSET` | `8` | UTC offset in hours |

API keys and Wi-Fi credentials live only in your local `sdkconfig` (not committed).

TTS must support `response_format: "wav"`. Free-tier QWeather APIs are used only
(real-time + 3-day forecast).

## Build from source

- ESP-IDF **v5.5.3** exactly.

```bash
idf.py set-target esp32c3
idf.py build
./tools/validate.sh --static    # repo checks + host tests (lunar / weather parser / chat core)
./tools/validate.sh --firmware  # isolated build + merged-image verification
```

## Layout (this branch)

| Path | Role |
| --- | --- |
| `main/main.c` | App shell: init, page routing, network boot task |
| `main/page_home.c` | Clock / lunar / festival / weather summary |
| `main/page_weather.c` | Current + 3-day forecast |
| `main/page_calendar.c` | Month grid with lunar & holiday badges |
| `main/page_chat.c` + `ui_chat.c` | Voice chat page |
| `main/lunar.c` | Solar↔lunar, festivals, 2026 holidays (host-tested) |
| `main/qweather_parse.c` / `qweather.c` | Weather parser (host-tested) + fetch/cache |
| `main/time_sync.c` | SNTP |
| `main/chat_core.c` / `ai_client.c` / `voice_pipeline.c` / `wifi_sta.c` | Voice chat engine (unchanged) |

## Known limitations

- Weather/calendar need Wi-Fi; without it the home clock shows `--:--` until synced.
- Holiday/makeup table covers **2026** only; other years show plain weekday semantics.
- Font covers GB2312 level-1 (3755 chars); rare characters render as boxes.
- Answers are non-streaming; history lives in RAM (8 messages).

## License

- Baseline & BSP: [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) (MIT).
- This application: MIT. The bundled Chinese font derives from a locally installed system font
  (SimHei) for personal use; regenerate with your own font before commercial distribution.
