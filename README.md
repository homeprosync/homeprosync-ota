# Home Pro Sync — Advanced Dual OTA System

Official cloud repository for Home Pro Sync IoT System (Mobile Android App + ESP32 Hardware Firmware).

## 🚀 Repository Structure
- pp/version.json: Live Mobile App release manifest (Production & Targeted Beta).
- pp/HomeProSync.apk: Mobile App production build.
- pp/HomeProSync_beta.apk: Mobile App targeted beta build.
- irmware/version.json: Live ESP32 Hardware Firmware release manifest.
- irmware/HomeProSync_ESP32/HomeProSync_ESP32.ino: ESP32 source code (v4.0.0).
- irmware/HomeProSync_ESP32/firmware.bin: Production firmware binary.
- irmware/HomeProSync_ESP32/firmware_beta.bin: Beta firmware binary.

## 📡 Live OTA Manifest Endpoints
- **Mobile App OTA Manifest:**
  https://raw.githubusercontent.com/homeprosync/homeprosync-ota/main/app/version.json
- **ESP32 Firmware OTA Manifest:**
  https://raw.githubusercontent.com/homeprosync/homeprosync-ota/main/firmware/version.json

## 🛡️ Targeted Beta Testing (Whitelisted Emails)
- Whitelisted Developer / Beta tester: mhdf786@gmail.com
- Motor Safety Lock: Automatically turns OFF dp101 motor prior to firmware update.
