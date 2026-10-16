# Quake II for ESP32-S3

A port of id Software's Quake II to the ESP32-S3, running the full retail game on the
[Seeed Wio Tracker L2](https://wiki.seeedstudio.com/meshtastic_wio_tracker_l2_intro/)
(ESP32-S3, 8 MB PSRAM, 16 MB flash, 320x240 screen, microSD, speaker, touch screen) and the
[LilyGO T-Deck](https://lilygo.cc/products/t-deck) (same chip, 320x240 screen, microSD,
speaker, touch screen, keyboard and trackball).

The engine comes from the ESP32-P4 port [ESP32_QUAKE2](https://github.com/alexkid77) by
Alejandro Villegas Alonso, itself based on [quake2generic](https://github.com/ozkl/quake2generic)
(id Software's Quake II source with the software renderer). The board support (display, touch,
audio, SD card, Bluetooth LE input) comes from the ESP32-S3 port of Quake 1 for the same board.

Quake II was made for PCs with 16 MB of RAM or more: the P4 port uses about 23 MB. This port
fits in the 8 MB of PSRAM of the ESP32-S3 (5.9 MB left for the game data):

- One memory arena for everything, with exactly sized model hunks instead of fixed 1-4 MB
  reservations; transient file buffers from its top.
- Maps are read one lump at a time from the SD card instead of being loaded whole (twice,
  for the collision and the render models); the collision map is allocated for the map
  instead of using fixed arrays (3 MB); the renderer shares its planes and visibility data.
- The models (MD2) and their skins are stored in their in-memory format in a flash partition
  (copied there from the PAK files on the first start) and used in place: they take no PSRAM.
- The wall textures are used from mip level 1 (half resolution, `sw_mipcap 1`), and level 0
  is not loaded (except for water, sky and translucent surfaces).
- Sound cache with a budget (`s_cachesize`, 1.5 MB, less when memory is low) and least recently used eviction; sounds
  are resampled to 11 kHz 8-bit.
- Smaller surface cache (288 kB), particle and image limits, no OpenGL data in the models,
  monster animation tables in flash, and many large static buffers allocated on demand.

## Status

The whole single player game (all units and secret levels) loads and plays, with sound
and level changes within and between units (save games are not tested much yet). Frame
rate: 15-30 fps in game, 25 fps in the `demo1` timedemo (T-Deck: 24 fps). The largest
maps (`lab`, `hangar2`, `city3`) leave a few hundred kB of PSRAM free. Multiplayer and
mods are not supported.

## Performance

- The second core draws the world spans (rows split by parity) and the model triangles
  (split by triangle index), while the first core walks the BSP and builds the edges.
- Single precision math everywhere (`-fsingle-precision-constant`, `sinf`/`sqrtf`...): the
  ESP32-S3 FPU has no double precision, and float division is a ROM call, so the renderer
  uses the `recip0.s` reciprocal instruction with Newton steps (`R_Recip`).
- Moving brushes (`SV_Push`) query the area tree instead of every entity.
- Flash and octal PSRAM at 120 MHz (about 20% faster than 80 MHz). ESP-IDF marks octal
  PSRAM at 120 MHz as experimental: the timing is tuned at boot and a large temperature
  change (about 20 °C) can make it fail. If the game crashes at random once warm, go back to
  `CONFIG_ESPTOOLPY_FLASHFREQ_80M` and `CONFIG_SPIRAM_SPEED_80M` in `sdkconfig.defaults`
  (delete `sdkconfig` and rebuild).

## Game data

Copy `pak0.pak`, `pak1.pak` and `pak2.pak` of the retail Quake II (patched to 3.20) to the
`baseq2` folder of a FAT32 microSD card:

```
baseq2/pak0.pak
baseq2/pak1.pak
baseq2/pak2.pak
```

The other files of a Quake II installation (`.exe`, `.dll`, mods, `players`, `.tga` skies...)
are not needed. The data can also be copied without taking the card out, with the firmware
running: `tools/sdput.py baseq2/pak0.pak ...` (slow: USB full speed).

On the first start, the board copies the models and skins from the PAK files to its flash
(about 100 seconds, with the progress on the screen), and again when the PAK files change.
To skip that, the image can be built on a computer and flashed (needs Python 3):

```sh
python3 tools/mkassets.py gamedata/assets.bin pak2.pak pak1.pak pak0.pak
```

## Building and flashing

ESP-IDF 5.5. From an ESP-IDF shell (or after `. tools/idf_env.sh` with the PlatformIO
toolchain):

```sh
cmake -G Ninja -B build -DPYTHON_DEPS_CHECKED=1
ninja -C build
tools/flash.sh            # firmware
tools/flash.sh assets     # and gamedata/assets.bin (optional)
```

For the T-Deck, build in its own directory with its own `sdkconfig`:

```sh
cmake -G Ninja -B build-tdeck -DPYTHON_DEPS_CHECKED=1 -DBOARD=BOARD_LILYGO_TDECK -DSDKCONFIG=build-tdeck/sdkconfig
BUILD=build-tdeck tools/flash.sh assets
```

`tools/flash.sh` builds before flashing. With several boards connected it does not guess:
give the port, `PORT=/dev/cu.usbmodem3101 tools/flash.sh`. After changing
`sdkconfig.defaults`, delete `sdkconfig` and run `cmake` again.

If flashing fails, hold BOOT and press RESET to enter the download mode.

`tools/merge.sh` (`BUILD=build-tdeck` for the T-Deck) makes a single image in `dist/` to
flash at 0x0 with any ESP32-S3 flasher, e.g.
`esptool.py --chip esp32s3 write_flash 0x0 quake2-build.bin`: with the PAK files on the SD
card, nothing else is needed.

## Controls

**Touch screen** (drawn over the game once the screen has been touched):

- left half: virtual stick where the finger lands (move, strafe)
- right half: drag to turn and look
- FIRE, JUMP and NEXT WEAPON buttons on the right (dragging from FIRE also looks)
- MENU in the top left corner
- in menus and the console: swipe for the arrow keys, tap for enter

**BOOT button**: menu (escape) on the Wio Tracker L2.

**T-Deck keyboard and trackball**:

| Key | Game | Menus |
|---|---|---|
| W / S | forward / back | |
| A / D | strafe left / right | |
| E, trackball click | fire | enter |
| Q | next weapon | |
| space / C | jump / crouch | |
| Shift | run | |
| Sym + 1-0 | select a weapon | |
| Alt | menu | escape |
| Mic | console | console |
| Speaker / Enter | inventory / use item | |
| trackball | turn, look | arrows |

The keyboard needs the LilyGO keyboard firmware from June 2025 or later to hold keys; older
firmware sends one character per key press, and each key is released shortly after.

**Bluetooth LE gamepad, keyboard or mouse**: the board scans for BLE HID devices and connects
to the first one found (bonded devices first) and reconnects to it later. To pair a new
device, put it in pairing mode. Only Bluetooth LE devices work (the ESP32-S3 has no
Bluetooth Classic), e.g. Xbox Wireless controllers or 8BitDo in Android mode. Gamepad:

| Control | Game | Menus |
|---|---|---|
| left stick | move, strafe | arrows |
| right stick | turn, look | |
| RT / LT | fire / run | enter / shift |
| A / B | jump / crouch | enter / escape |
| X / Y | next weapon / scores | |
| LB / RB | strafe left / right | page up / down |
| L3 / R3 | look up / down | |
| START / SELECT | menu / console | |

Keyboards work as on a PC (default Quake II bindings), mice turn and look and their buttons
are MOUSE1-3.

## Serial console

The USB serial port is a Quake console: any line is a console command (`map base1`, `god`,
`give all`, `mem`...). Local commands:

| Command | |
|---|---|
| `shot` | screenshot, saved as a PNG by `tools/serial_daemon.py` |
| `sysmem`, `tasks` | ESP-IDF heap and task stacks |
| `cpu` | CPU use per task, time spent waiting for the second core and the display |
| `prof [ms]` | sampling profiler of the game core (see `tools/profile.py`) |
| `perf start [frames]`, `perf dump` | time of each part of every drawn frame: rendering, waiting for the second core or the display, SD card reads, sound and image loading (see `tools/perfgraph.py`) |
| `ls <dir>`, `put` | SD card listing and upload (see `tools/sdput.py`) |
| `reboot`, `start` | restart, start the game after a crash |

On macOS, opening the port resets the ESP32-S3: `tools/logger.sh <dir>` keeps it open
(console in `<dir>/console.log`, commands appended to `<dir>/commands`).

Benchmark: `timedemo 1` then `demomap demo1.dm2` prints the frame rate at the end. Set
`timedemo 0` afterwards: otherwise the game runs faster than real time.

After a crash, the board restarts and waits on the serial console (send `start`) instead of
crashing again: press RESET to play.

## Tools

| Tool | |
|---|---|
| `tools/flash.sh [assets]` | build and flash (`PORT=...`) |
| `tools/merge.sh [out.bin]` | single firmware image to flash at 0x0 (`BUILD=...`) |
| `tools/mkassets.py` | flash asset image (models and skins) from the PAK files |
| `tools/logger.sh`, `tools/serial_daemon.py` | serial console logger, screenshots |
| `tools/sdput.py` | copy files to the SD card over USB |
| `tools/mapsweep.sh` | load maps one after the other and report errors, fps and memory |
| `tools/profile.py` | per function histogram of a `prof` capture: `tools/profile.py console.log build/Quake2ESP32S3.elf` |
| `tools/perfgraph.py` | interactive per frame graph (HTML) of a `perf dump` capture: `tools/perfgraph.py console.log` |

## Host build

`host/` builds the same engine for macOS or Linux with SDL2, to test changes and measure the
memory use (`Q2MEM=<bytes>` limits the arena, `Q2ASSETS=gamedata/assets.bin` uses the asset
image as the ESP32 does):

```sh
make -C host -j8
cd gamedata && Q2ASSETS=assets.bin ../host/q2host +set basedir . +map base1
```

`-msec <n>` runs with a fixed frame time and a simulated clock, for reproducible tests.

## License

GNU General Public License v2 (see LICENSE), as the Quake II source.
