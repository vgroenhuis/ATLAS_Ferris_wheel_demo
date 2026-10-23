#pragma once

// 128x64 SSD1306 on SPI: eight lines of 21 characters.
void display_start(void);                        // the panel and a first message
void display_run(void);                          // the live status page; call after sensors_start()
void display_boot_message(const char *line);     // before everything else is up
