# ATLAS 3-DoF sensor (ESP32-C5-DevKitC-1, ESP-IDF 6.1)

Port of Antonie Veldwachter's Arduino/PlatformIO firmware (`3-dof-atlas`, branch `demo` on git.ram.eemcs.utwente.nl) to ESP-IDF, so it
runs on the shared CloSer demo framework (`components/closer_demo`, a copy of the one in the Turtle demo: Wi-Fi, status reports, over-the-air updates
with rollback, roaming). Same hardware, same maths, same pages. Developed remotely and tested on the device with the user's help (see the notes at the end).

Hardware (pins as in the original, changeable in menuconfig > Atlas): I2C SDA=GPIO2, SCL=GPIO3 to a TCA9548A (0x70), four AS5600 on
its channels 2..5; SSD1306 128x64 on SPI: MOSI=7, CLK=6, DC=4, CS=5, RST=24; spool radius 51 mm.

## What is new compared with the Arduino version
* Wi-Fi network, password and the CloSer device key are in flash (NVS), not in the source: `python tools/provision.py`.
* Reports to the lab demos page: address, signal, access point, firmware, force/torque, sample rate, I2C errors, problems.
* OTA updates (the web pages are embedded gzipped in the firmware image, so one image is everything). Rollback if a new image can't report.
* No permanent halt: missing multiplexer / encoder is shown (OLED, page, report) and retried; bus reset; AS5600 magnet status
  (too weak / too strong / missing) is read once a second.
* The libraries three.js and chart.js are served from the device (no internet needed on the phone). `ATLAS.png` became a 512x768 JPEG.
* `POST /api/zero` (and a Zero button on the page) makes the current position zero; needs all four encoders (demo mode zeroes at start-up only: a sensor that is loaded at power-up reads garbage).
* Works with 3 of 4 encoders: the missing rope is estimated from the other three (opposite pairs add up to the same, r0+r1 = r2+r3); flagged as degraded on the OLED, in /data and in the report. An encoder that returns gets a zero that matches the others.
* The page is fed by a WebSocket (`/ws`, 50 Hz; the page sends "stream" when its socket opens, because this IDF version doesn't call the handler for the handshake) and animates smoothly; HTTP polling of `/data` is the fallback. The game page still polls `/data`.
* `GET /data` has the same fields as before plus `valid`, `demo`, `hz`.
* `/api/state`, `/api/mode?demo=0|1`, `/api/calibration` (GET/POST). Calibration lives in NVS; the interactive serial calibration of the
  original (`calibration.cpp`) is not ported yet.

## Build, flash, update
    . ~/esp/idf-env.sh
    cd ~/closer-deploy/labdemos/firmware/atlas
    idf.py build
    python tools/provision.py        # local/atlas-factory.bin (firmware + Wi-Fi + key): flash once at 0x0 over USB
    python -m esptool --chip esp32c5 -p <port> write_flash 0x0 atlas-factory.bin
    # later: bump VERSION, idf.py build, then on the server:
    cd ../.. && .venv/bin/python -m labdemos.cli publish atlas firmware/atlas/build/atlas.bin
`local/` holds the Wi-Fi password and key (not in git). The original firmware stays available in Antonie Veldwachter's repository.

## Notes from bring-up (2026-10-14)
* OLED: the IDF `esp_lcd` SSD1306 route left the panel white; the display is driven by bit-banged SPI exactly like Adafruit_SSD1306 did (`display.c`).
* I2C runs at 100 kHz (the original 400 kHz) and the display refresh and the encoder reads never overlap (the OLED pins are noisy next to the I2C wires).
* The shared Wi-Fi component limits the band in the STA_START event (set_band_mode fails before Wi-Fi is started).
* Wi-Fi credentials: iotroam seems to accept a key per MAC/device; the Turtle board's MAC works with the provisioned password, the original Atlas board's did not.

## Credits
ATLAS (Angular Torsion Load Acquisition Sensor) was designed and built by **Antonie Veldwachter** as a student project at the University of Twente,
including the original firmware, the sensor maths and calibration, the web pages, the game and the ATLAS illustration. He gave permission to
publish the source code under the MIT licence (`LICENSE`). The ESP-IDF port and the CloSer demo framework are by Vincent Groenhuis, with help from Claude.
