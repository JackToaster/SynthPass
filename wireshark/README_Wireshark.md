# Wireshark packet dissector for SynthPass

Installation: Add synthpass.lua to your Wireshark plugins folder

Linux/macOS: ~/.local/lib/wireshark/plugins/

Windows: %APPDATA%\Wireshark\plugins\

## Implementation details

This is a Post-Dissector which runs after the packet is already received/dissected by the default BLE dissectors. This is because we are doing nasty stuff to the manufacturer specific data field (manufacturer ID is actually part of the device UID).

A consequence is that this dissector is run on all packets (not optimal for performance, but you'll typically handle relatively few packets so it should be OK)

The dissector matches SynthPass packets by MAC address, and by matching the structure of the BLE packet (must have manufacturer specific advertising data field).

It might be possible to parse it in a less-bad way, but I'm not aware how to do it without reimplementing bluetooth parsing.