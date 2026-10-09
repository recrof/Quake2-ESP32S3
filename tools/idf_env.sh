# Source this to get idf.py (uses the PlatformIO-installed ESP-IDF 5.5.5 toolchain)
export IDF_PATH="$HOME/.platformio/packages/framework-espidf@src-6c2e555ff1cf079c2c8f517e77b082e1"
export IDF_PYTHON_ENV_PATH="$HOME/.platformio/penv/.espidf-5.5.5"
export PATH="$HOME/.platformio/tools/toolchain-xtensa-esp-elf/bin:$HOME/.platformio/tools/tool-cmake/bin:$HOME/.platformio/tools/tool-ninja:$IDF_PYTHON_ENV_PATH/bin:$IDF_PATH/tools:$PATH"
export ESP_ROM_ELF_DIR="$HOME/.platformio/tools/tool-esp-rom-elfs/"
