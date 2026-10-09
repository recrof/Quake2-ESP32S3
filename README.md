# Quake II for ESP32-S3

A port of id Software's Quake II to the ESP32-S3, running the full retail game on the
[Seeed Wio Tracker L2](https://wiki.seeedstudio.com/meshtastic_wio_tracker_l2_intro/)
(ESP32-S3, 8 MB PSRAM, 16 MB flash, 320x240 screen, microSD, speaker, touch screen).

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
  (`tools/mkassets.py`) and used in place: they take no PSRAM.
- The wall textures are used from mip level 1 (half resolution, `sw_mipcap 1`), and level 0
  is not loaded (except for water, sky and translucent surfaces).
- Sound cache with a budget (`s_cachesize`, 384 kB) and least recently used eviction; sounds
  are resampled to 11 kHz 8-bit.
- Smaller surface cache (288 kB), particle and image limits, no OpenGL data in the models,
  monster animation tables in flash, and many large static buffers allocated on demand.

## Status

Work in progress: the game runs (about 15-20 fps), the largest maps may still run out of
memory.

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

Then build the flash asset image from the same PAK files (needs Python 3):

```sh
python3 tools/mkassets.py gamedata/assets.bin pak2.pak pak1.pak pak0.pak
```

## Building and flashing

ESP-IDF 5.5. From an ESP-IDF shell (or after `. tools/idf_env.sh` with the PlatformIO
toolchain):

```sh
cmake -G Ninja -B build -DPYTHON_DEPS_CHECKED=1
ninja -C build
tools/flash.sh assets     # firmware and gamedata/assets.bin (14 MB, once)
tools/flash.sh            # firmware only
```

If flashing fails, hold BOOT and press RESET to enter the download mode.

## Serial console

The USB serial port is a Quake console: any line is a console command (`map base1`, `god`,
`give all`, `mem`...). Local commands: `shot` (screenshot, saved as a PNG by
`tools/serial_daemon.py`), `sysmem`, `tasks`, `ls <dir>`, `put` (see `tools/sdput.py`),
`reboot`. On macOS, opening the port resets the ESP32-S3: `tools/logger.sh <dir>` keeps it open
(console in `<dir>/console.log`, commands appended to `<dir>/commands`).

After a crash, the board restarts and waits on the serial console (send `start`) instead of
crashing again: press RESET to play.

## Host build

`host/` builds the same engine for macOS or Linux with SDL2, to test changes and measure the
memory use (`Q2MEM=<bytes>` limits the arena, `Q2ASSETS=gamedata/assets.bin` uses the asset
image as the ESP32 does):

```sh
make -C host -j8
cd gamedata && Q2ASSETS=assets.bin ../host/q2host +set basedir . +map base1
```

## License

GNU General Public License v2 (see LICENSE), as the Quake II source.
