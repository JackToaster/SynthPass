# SynthPass

An open-source protocol & hardware design for to detect nose boops & transfer simple data, designed for protogen/synth fursuits.

Built to solve a very important problem. With SynthPass, you won't have to break character or pull off your paws and fumble with your phone to trade contact details. Just nose-boop, and SynthPass will automagically trade a small chunk of user-defined data, like a web link, username, or anything else that can fit in a couple hundred bytes of plain text. Then plug in your SynthPass at the end of the day and download all the data you've collected.

SynthPass is also designed to easily interface with nearly any protogen/synth fursuit. SynthPass hardware can be powered by a single 3.3V supply, and supports digital several digital interface options.

## Features

- ### Simple interface to fursuits
	- GPIO: pin HIGH (3.3V) when booped, LOW (0V) when not booped. Additional pin that's high
	- UART: Message for boop start/end
	- I2C: Register for boop status
    - Don't want to modify your firmware or connect the digital pins to your controller? The GPIO output can also drive an LED to indicate when another SynthPass is nearby and when a nose boop is detected.
- ### Advanced interface
	- UART/I2C Only
	- Approximate range & unique ID of peer prior to boop
	- Extra boop parameters (e.g. boop expression, color, name/species of peer)
- ### USB interface
    - Mass storage device for downloading collected data. Simply plug in the SynthPass into a computer and open `MESSAGES.HTM` in your web browser.
        - Edit the files `ME/BOOP.TXT` and `ME/PROX.TXT` to set your own messages to be transferred on proximity/boop
        - Edit `ME/SYNTHPAS.INI` to change settings for LED behavior, boop range, and enable proximity messages
    - Software bootloader switch for firmware updates over USB - send the character 'b' to the USB TTY that appears to switch to the USB bootloader.
## Hardware
SynthPass is based on the ultra-low-cost WCH CH57x series microcontrollers.
- ## 2.4GHz radio
	- Boop detection by RSSI (radio signal strength)
	- ISLER protocol for peer-to-peer and broadcast communication without need for pairing
	- CH572 also supports BLE (possibility for future companion app?)
- ## Protogen/Synth Suit Interface
	- 4-pin JST SH connector, compatible with QWIIC/Stemma QT (TODO?)
	- Single 3.3V input (possibly support 5V input with some hardware designs)
	- Option for I2C, UART, or GPIO communication protocol
	- Designed for easy integration with most synth/protogen heads

## Software
Using CH32Fun + iSLER library for communication

SynthPass communicates using BLE advertising frames with a fixed MAC address so that SynthPassdevices can easily be detected.

Communication (currently) uses a fixed channel (Channel 37) and PHY mode (1Mbps).

TODO document stuff better :3

## Development

**Building SynthPass**

SynthPass uses PlatformIO to manage the firmware toolchain, and the ch32fun framework to handle the ch572's hardware interfaces.

To compile the firmware, you'll need to:

- Clone the SynthPass repo `git clone https://github.com/JackToaster/SynthPass.git`
- Run `pio run -e [board hardware target]`. `[board hardware target]` should be replaced with one of the targets defined in `platformio.ini`:
  - `nanoch57x` for the nanoch57x dev board
  - `pretzelslab` for the SynthPass hardware in this repo & the SynthPasses handed out at Anthrocon 2026
  - `axionqc` for AxionQc's hardware design

**Updating firmware**

We have a custom upload script built into the PlatformIO configuration for SynthPass, all you need to do to update the firmware is:

`pio run -e [board target] -t upload`

for the "official" Pretzel's Lab hardware: `pio run -e pretzelslab -t upload`

If the firmware is borked and the flashing script doesn't work, hold down the boot button while plugging in the SynthPass to force it into ISP bootloader mode.
