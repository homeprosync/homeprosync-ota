# Home Pro Sync — ESP32 Over-The-Air (OTA) Firmware System

Official cloud repository for Home Pro Sync IoT controllers (Water Tank & Universal Solar Management).

## 🚀 Repository Structure
- irmware/version.json: Live OTA manifest defining active Production and Targeted Beta channels.
- irmware/HomeProSync_ESP32/HomeProSync_ESP32.ino: ESP32 controller firmware source code (v4.0.0).
- irmware/HomeProSync_ESP32/firmware.bin: Production firmware binary.
- irmware/HomeProSync_ESP32/firmware_beta.bin: Targeted beta firmware binary.

## 📡 Live OTA Manifest URL
`
https://raw.githubusercontent.com/homeprosync/homeprosync-ota/main/firmware/version.json
`

## 🛡️ Channels & Features
- **Production Channel**: Standard stable releases delivered to all customer devices.
- **Targeted Test Channel**: Whitelisted beta releases delivered only to approved emails and device IDs.
- **Motor Safety Lock**: Water pump (DP 101) automatically shuts OFF before flashing begins.
