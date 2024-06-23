"export DISPLAY=:99.0"
sleep 3 # give xvfb some time to start
wget https://downloads.arduino.cc/arduino-1.8.19-linux64.tar.xz
tar xf arduino-1.8.19-linux64.tar.xz
mv arduino-1.8.19 $HOME/arduino_ide
cd $HOME/arduino_ide/hardware
mkdir esp32
cd esp32
wget https://github.com/espressif/arduino-esp32/archive/refs/tags/3.0.1.tar.gz
tar -xzf 3.0.1.tar.gz
mv arduino-esp32-3.0.1/ esp32
cd esp32/tools
python --version
python get.py
pip install --user platformio
platformio update
