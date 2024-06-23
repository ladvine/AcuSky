export PATH="$HOME/arduino_ide:$PATH"
arduino --board esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=min_spiffs,CPUFreq=240,FlashMode=qio,FlashFreq=80,DebugLevel=none --pref compiler.warning_level=all --save-prefs
#arduino --verbose --verify esp32-cam-webserver.ino - commented to let platformio to download libs
platformio run
