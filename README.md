# DeskFlip
### Flip Clock Widget for the Windows Desktop

A flip clock for your Windows desktop - beautiful, lightweight, and unobtrusive. Each digit is
rendered in real 3D perspective, so the cards genuinely flip. Frameless and transparent, it sits on
your wallpaper, stays put through Show Desktop, and remembers its place. Under 1 MB, around 15 MB of
RAM, no telemetry.

---

## 🎬 Preview

<!-- Drop a screen recording or screenshot here -->
<img width="1280" height="800" alt="Screenshot (127)" src="https://github.com/user-attachments/assets/36b2db8a-44e7-42ac-b1c4-d43e2cae575b" />

---

## ✨ Features

- **Real 3D Flip** - Each digit is drawn in true perspective; the top half releases, swings down through the hinge with natural foreshortening, and lands on the card below. The shadow moves with it.
- **Per-Card Motion** - A card flips only when its own digit changes, exactly like a mechanical flip clock
- **Frameless & Transparent** - No window, no background. It sits directly on your wallpaper with a soft drop shadow
- **Drag Anywhere** - Grab it and move it; position and size are remembered
- **Scale From Widget to Wall** - *Resize* from the right-click menu takes it from a small desktop widget to a wall-sized display
- **Show Seconds** - Six cards or four, your choice
- **Light & Dark** - Light mode is a separate theme, not an inversion
- **Survives Win+D** - Stays where you put it, even after Show Desktop
- **Always on Top** *(optional)* - Or let it live quietly behind your windows
- **Start with Windows** *(optional)*
- **System Tray Icon** *(optional)* - Same menu, from the notification area
- **High-DPI Sharp** - Rescales cleanly across monitors and DPI boundaries
- **18 Languages** - Matched automatically to your Windows display language
- **Native & Tiny** - Direct2D on the GPU. No frameworks, no background services, no runtime to install
- **No Telemetry** - Nothing collected, nothing sent, no account, no internet needed

---

## 📥 Download

<a href="https://apps.microsoft.com/detail/9MSXBKV3295F?referrer=appbadge&mode=full&cid=from_github" target="_blank" rel="noopener noreferrer">
  <img src="https://get.microsoft.com/images/en-us%20light.svg" width="200"/>
</a>

- Purchasing from the Microsoft Store helps support ongoing development ❤️
- You can also support via GitHub Sponsors:    [![Sponsor](https://img.shields.io/badge/Sponsor-%E2%9D%A4-fe8e86?logo=github)](https://github.com/sponsors/riyasy)

### 📊 Store vs GitHub Version

|  | Microsoft Store | GitHub Release |
|--|--|--|
| **Price** | 🪙 Paid | 🆓 Free |
| **Updates** | ✅ Automatic | ❌ Manual |
| **Security** | ✅ Signed & verified | ❌ Not signed |

---

## 🚀 Installation

- Option 1 : [**Install from Microsoft Store**](https://apps.microsoft.com/detail/9MSXBKV3295F?launch=true&cid=from_github&mode=full)
- Option 2 : Download the portable exe from the Github [**Releases Page**](https://github.com/riyasy/DeskFlip/releases)
- Option 3 : Build it yourself (see below)

---

## ⚙️ Settings

The right-click menu is the entire interface. There is no settings window to learn:

- **Show seconds** - Six cards instead of four
- **Light mode** - A separate light theme
- **Resize** - Turns on the grip at the bottom-right corner; drag it to scale
- **Always on top**
- **Start with Windows**
- **Show in system tray**
- **About DeskFlip...**
- **Exit**

Everything is saved to `%LOCALAPPDATA%\DeskFlip.ini`.

---

## 📌 Requirements

- Windows 10 version 1809 (build 17763) or later, or Windows 11
- No .NET, no Visual C++ redistributable, no runtime of any kind

---

## 🧰 Building

MSVC only. C++20, Windows SDK 10.0.26100.

```powershell
msbuild src\DeskFlip.slnx /t:DeskFlip /p:Configuration=Release /p:Platform=x64
# -> src\x64\Release\DeskFlip.exe
```

The solution is the XML `.slnx` format - VS 2022 17.14+ / MSBuild 17.13+ is needed to read it.
`assets\JetBrainsMono-ExtraBold.ttf` is not in the repo; see `src\DeskFlip\assets\README.md`.
Without it the clock falls back to Cascadia Mono.

---

## 🪶 How lightweight, exactly

Measured at 1180x460 on a 75 Hz display:

| | |
|--|--|
| **Binary** | under 1 MB |
| **Memory** | ~15 MB as a small widget, under 30 MB enlarged |
| **CPU** | ~5% of one core while a card is flipping, near zero at rest |
| **Idle** | Sleeps until the next second boundary - no animation loop |

Nothing is rasterized twice: glyphs, card faces and the case are baked into GPU bitmaps once per
size, and a frame is a handful of blits. A card at rest is not drawn at all.

---

## 🛠️ Roadmap

- 12-hour / AM-PM display
- Date and weekday cards
- Accent themes beyond light and dark
- Optional flip sound

---

## 🤝 Contributing

Contributions are welcome! Feel free to fork and submit pull requests.

---

## 💡 Tip

Park it in a corner with **Always on top** off - it stays visible on the desktop, out of the way of
your windows, and Win+D never loses it.

---

(c) 2026 RYF Tools. All rights reserved.
