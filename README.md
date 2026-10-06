# Cardputer ADV WebDAV MP3 Player (lite)

A stripped-down, glitch-free WebDAV / HTTP MP3 player for the **M5Stack Cardputer ADV** (ESP32-S3, no PSRAM).
It streams MP3 files straight from a NAS or any WebDAV server over Wi-Fi: files of any size, up to 320 kbps.

Based on [CardputerADV-NAS-mp3player](https://github.com/HardCore-Gamer/CardputerADV-NAS-mp3player) 1.4.0 by HardCore-Gamer.

## What is different from the original

- **No more stutter**: a network task streams the file into a large ring buffer (32-96 KB, depending on free RAM)
  and a separate decode task feeds the speaker. Drawing the screen or reading the keyboard can no longer
  interrupt the sound. The original decoded in the UI loop with an 8 KB buffer and stuttered on 320 kbps files.
- **Real track duration**: read from the MP3 header (Xing / Info / VBRI frame count, or bitrate for CBR files).
  The original assumed every file was 128 kbps.
- **Clean folder names**: rewritten streaming PROPFIND parser. The original showed raw XML
  (`<D:propstat><D:prop><D:displayname>...`) for most entries and stopped at 120 entries / 96 KB of XML;
  this version lists up to 1000 entries per folder (as long as RAM allows).
- **One folder = one playlist**: every MP3 of the folder is played in natural order ("2" before "10"),
  then playback stops (the original looped forever).
- **Large folders**: names are stored decoded, packed in 4 KB blocks (they were kept percent-encoded, one
  String each: 6 bytes per Cyrillic letter), so folders with 500+ entries are no longer cut off.
- **Russian, accented and other special names**: font with Cyrillic and all Latin-1 accents (`efontJA_12`);
  letters still missing from the font get a look-alike (`Ї` -> `Ï`, `Ґ` -> `Г`...) or `?` instead of an empty box,
  decomposed accents (`e` + `´`) are merged, `+` in names is kept, numeric XML entities (`&#1055;`) are decoded,
  and URLs are rebuilt with standard percent-encoding.
- **Keyboard never freezes**: the keys are read on every loop. The M5Cardputer library waits for the
  TCA8418 keyboard interrupt, which can get lost when a key arrives at the wrong moment: the keyboard then
  stopped responding while the app kept running.
- **Browse while listening**: go back to the folder list without stopping the music, and return to the player.
- **Stripped down**: search, sleep timer, eco/screen-off mode, resume on boot, seeking, NTP clock and help
  screen were removed.
- Diagnostics: the player shows the buffer fill level and the number of underruns ("cuts");
  `[boot]`, `[play]` and `[end]` lines (free heap, buffer size, bitrate...) are printed on the USB serial port.

## Controls

**Folder list**

| Key | Action |
|---|---|
| `;` / `.` (or `W` / `S`) | Move up / down (hold to scroll fast) |
| `Enter` | Open folder / play the folder starting from this track |
| `` ` `` or `Del` | Parent folder |
| `Tab` | Back to the player screen (while something is playing) |
| `N` | Change the server address |
| `Q` | Wi-Fi networks |
| `R` | Reload the folder |

**Player**

| Key | Action |
|---|---|
| `Space` | Pause / resume |
| `N` / `P` | Next / previous track (`P` restarts the track after 3 s) |
| `+` / `-` | Volume |
| `` ` ``, `Del` or `Tab` | Back to the folder list (music keeps playing) |

**Text fields** (Wi-Fi password, server address): `Enter` to confirm, `Del` to erase, `Tab` to go back.

## Server address

Plain HTTP only (no HTTPS), for example:

```
http://192.168.1.20:5005/music/
http://user:password@192.168.1.20:5005/music/
```

The Wi-Fi network, the address and the last opened folder are saved and reopened at boot.

## Build

[PlatformIO](https://platformio.org/):

```
pio run
```

`espressif32@6.7.0` (Arduino core 2.0.x), M5Cardputer, M5Unified, ESP8266Audio 1.9.9 (libmad MP3 decoder).
The resulting `.pio/build/cardputer_adv_nas_mp3/firmware.bin` can be installed with
[M5Launcher](https://github.com/bmorcelli/Launcher) from the SD card, or flashed at `0x10000`.

## Credits

- Original player: [HardCore-Gamer/CardputerADV-NAS-mp3player](https://github.com/HardCore-Gamer/CardputerADV-NAS-mp3player)
- [ESP8266Audio](https://github.com/earlephilhower/ESP8266Audio), [M5Unified / M5Cardputer](https://github.com/m5stack)
