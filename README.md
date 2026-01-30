# SynthPass

An open-source protocol & hardware design for to detect nose boops & transfer simple data, designed for protogen/synth fursuits.

Built to solve a very important problem. With SynthPass, you won't have to break character or pull off your paws and fumble with your phone to trade contact details. Just nose-boop, and SynthPass will automagically trade a small chunk of user-defined data, like a web link, username, or anything else that can fit in a couple hundred bytes of plain text. Then plug in your SynthPass at the end of the day and download all the data you've collected.

SynthPass is also designed to easily interface with nearly any protogen/synth fursuit. SynthPass hardware can be powered by a single 3.3V supply, and supports digital several digital interface options.

## Features

- ### Simple interface
	- GPIO: pin HIGH (3.3V) when booped, LOW (0V) when not booped, plus proximity pin (TODO)
	- UART: Message for boop start/end
	- I2C: Register for boop status
    - Don't want to modify your firmware or connect the digital pins to your controller? The GPIO output can also drive an LED to indicate when another SynthPass is nearby and when a nose boop is detected.
- ### Advanced interface
	- UART/I2C Only
	- Approximate range & unique ID of peer prior to boop
	- Extra boop parameters (e.g. boop expression, color, name/species of peer)
- ### USB interface
    - (TODO) Mass storage device for downloading collected data
    - Bootloader switch for firmware updates over USB

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