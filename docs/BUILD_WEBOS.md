# Aurora – Build and installation for LG webOS

This guide explains how to enable developer mode on the TV, build Aurora, and install it manually on LG webOS TVs (LG C1 and compatible).

---

## Table of contents

1. [Developer mode on the TV](#1-developer-mode-on-the-tv)
2. [Manual installation](#2-manual-installation)
3. [Build](#3-build)
4. [Troubleshooting](#4-troubleshooting)
5. [HID Passthrough (Experimental)](#5-hid-passthrough-experimental)
6. [webOS Homebrew (catalog)](#6-webos-homebrew-catalog)

---

## 1. Developer mode on the TV

To install apps manually (.ipk), the TV must be in developer mode.

### Step-by-step (LG official)

1. **LG developer account**
   - Go to [developer.lge.com](https://developer.lge.com) and create an account

2. **Developer Mode app on the TV**
   - Press **Home** on the remote
   - Open **LG Content Store**
   - Search for **"Developer Mode"**
   - Install the app

3. **Enable developer mode**
   - Open the **Developer Mode** app
   - Sign in with your LG account
   - Turn on **"Dev Mode Status"** (the TV may restart)
   - Turn on **Key Server**

4. **Connection passphrase**
   - In the Developer Mode app, note the **passphrase** shown
   - You will use it to connect the TV to your PC

> Developer mode disables automatically after several reboots without a network connection. Re-enable it from the app when needed.

### Alternative: webosbrew

If you use [webosbrew](https://webosbrew.org/):

1. Install the Homebrew Channel (see webosbrew docs)
2. Install [dev-manager-desktop](https://github.com/webosbrew/dev-manager-desktop) to install .ipk via the GUI

---

## 2. Manual installation

### Prerequisites

- TV in developer mode (see section 1)
- TV and PC on the **same network**
- Built `.ipk` package (section 3) or from a release

### Method A: ares-cli (command line)

1. **Install webOS CLI**
   - Follow [webOS TV Developer – CLI](https://webostv.developer.lge.com/develop/tools/cli-dev-guide)
   - Windows: `npm install -g @webos-tools/ares-cli`
   - Linux: `sudo npm install -g @webos-tools/ares-cli`

2. **Configure the device**
   ```bash
   ares-setup-device
   ```
   - Enter name, TV IP, and when prompted, the **passphrase** from the Developer Mode app

3. **Install the .ipk**
   ```bash
   ares-install dist/com.aurora.gamestream_1.0.2_arm.ipk -d <TV_NAME>
   ```
   (Adjust the filename for your build version.)

4. **Launch Aurora**
   ```bash
   ares-launch com.aurora.gamestream -d <TV_NAME>
   ```

### Method B: dev-manager-desktop (GUI)

1. Install [webosbrew](https://webosbrew.org/) on the TV
2. Install [dev-manager-desktop](https://github.com/webosbrew/dev-manager-desktop)
3. Ensure TV and PC are on the same network
4. Open dev-manager-desktop and install the `.ipk` file

---

## 3. Build

Aurora is built via **cross-compilation** on **Linux** (or WSL2 on Windows).

### Compatibility

- **webOS 6.x** (LG C1) – NDL, SMP, H.265, HDR
- **ARM** – toolchain `arm-webos-linux-gnueabi`

### Prerequisites

**Linux (Ubuntu/Debian):**
```bash
sudo apt-get update
sudo apt-get install -y cmake gawk curl git build-essential
```

**Windows:** Use Docker or WSL2 with Ubuntu (see section 3.4).

### 3.1. Quick build (automated script)

```bash
cd moonlight-tv   # or your repo folder name
chmod +x scripts/webos/build_for_lg.sh
./scripts/webos/build_for_lg.sh
```

The `.ipk` will be created in `dist/`.

### 3.2. Manual build (step by step)

**1. webOS SDK**

```bash
cd /tmp
curl -L -O https://github.com/openlgtv/buildroot-nc4/releases/download/webos-b17b4cc/arm-webos-linux-gnueabi_sdk-buildroot.tar.gz
tar -xzf arm-webos-linux-gnueabi_sdk-buildroot.tar.gz
./arm-webos-linux-gnueabi_sdk-buildroot/relocate-sdk.sh
```

**2. Submodules**

```bash
cd moonlight-tv   # repo folder
git submodule update --init --recursive
```

**3. Configure and build**

```bash
export TOOLCHAIN_FILE=/tmp/arm-webos-linux-gnueabi_sdk-buildroot/share/buildroot/toolchainfile.cmake
./scripts/webos/easy_build.sh -DCMAKE_BUILD_TYPE=Release
```

**4. Release build**

```bash
CMAKE_BUILD_TYPE=Release ./scripts/webos/build_for_lg.sh
```

### 3.3. Output

The resulting package will be at:
```
dist/com.aurora.gamestream_1.0.2_arm.ipk
```

### 3.4. Windows (Docker or WSL2)

**Docker:**
```powershell
.\scripts\webos\build_with_docker.ps1
```

**WSL2 (Ubuntu):**
```bash
wsl --install -d Ubuntu
# In Ubuntu:
sudo apt-get install cmake gawk curl git build-essential
./scripts/webos/build_for_lg.sh
```

---

## 4. Troubleshooting

### "TOOLCHAIN_FILE not found"

Set the SDK path:
```bash
export WEBOS_SDK_DIR=/path/to/arm-webos-linux-gnueabi_sdk-buildroot
./scripts/webos/build_for_lg.sh
```

### Custom SDK location

```bash
sudo mv /tmp/arm-webos-linux-gnueabi_sdk-buildroot /opt/
export TOOLCHAIN_FILE=/opt/arm-webos-linux-gnueabi_sdk-buildroot/share/buildroot/toolchainfile.cmake
./scripts/webos/easy_build.sh -DCMAKE_BUILD_TYPE=Release
```

### CMake dependency errors

The buildroot-nc4 SDK includes pbnjson_c, PmLogLib, webosi18n, etc. If something is missing, verify your SDK installation.

### TV does not appear in ares-setup-device

- TV and PC on the same network
- Developer mode enabled on the TV
- Firewall not blocking the ares-cli port

---

## 5. HID Passthrough (Experimental)

HID Passthrough sends raw controller HID reports from the TV to **[CTM-USBIP](https://github.com/CTM-Bridge/CTM-USBIP)** on your PC, so Windows sees the manufacturer's native USB driver instead of a virtual Xbox pad from Sunshine.

### PC setup

1. Install **CTM-Bridge-Setup.exe** from the [CTM-USBIP releases](https://github.com/CTM-Bridge/CTM-USBIP/releases).
2. The installer registers the `ctm-usbip` Windows service (default control port **48054**) and the usbip-win2 driver.
3. PC and TV must be on the **same LAN** as the Moonlight stream.

### TV setup

1. Pair your controller to the TV via **webOS Settings** (Bluetooth), not inside Aurora.
2. In Aurora: **Settings → Input → Enable HID Passthrough (Experimental)**.
3. Start a stream. During streaming, open the status overlay (BACK / gamepad combo) and choose **HID Devices** (next to Virtual Mouse).
4. In the device list, tap **Plug in** on each gamepad you want bridged to CTM-USBIP on the PC. Use **Plug out** to stop bridging without unpairing the controller from the TV.

Video, audio, keyboard, and mouse still use Moonlight; **only plugged HID gamepads** are sent to CTM-USBIP.

When HID Passthrough is enabled, Aurora does **not** send Moonlight gamepad events (avoids duplicate controllers on the host).

### Supported controllers

CTM-USBIP maps most devices automatically:

| Controller | Host map | Notes |
|------------|----------|-------|
| DualSense / DS4 | Dedicated `.map` files | BT reports translated on the host |
| Xbox (GIP) | `xbox_gip_*.map` | |
| Flydigi Apex 4 (`04b4:2412`) | `hid_identity.map` | Composite device; Aurora picks the gamepad HID interface |
| Gamesir G7 Pro / others | `hid_identity.map` | Generic byte-for-byte passthrough |

No Aurora-side tweaks are needed per brand — if a device fails on the host, add a `.map` in CTM-USBIP, not in Aurora.

### Developer mode / hidraw access

HID capture reads `/dev/hidraw*`. On developer-mode installs this is typically available to the app. If controllers are not detected, verify nodes exist (e.g. with the standalone [ctm-bridge-webos](https://github.com/CTM-Bridge/ctm-bridge-webos) `--enumerate` tool).

Optional INI keys in `moonlight.ini` under `[input]`:

```ini
hid_passthrough=true
hid_passthrough_port=48054
```

---

## 6. webOS Homebrew catalog

To list **Aurora** in the [Homebrew Channel](https://webosbrew.org/) app store ([repo.webosbrew.org](https://repo.webosbrew.org/)), submit a PR to [webosbrew/apps-repo](https://github.com/webosbrew/apps-repo) using [`deploy/webosbrew/com.aurora.gamestream.yml`](../deploy/webosbrew/com.aurora.gamestream.yml) and the checklist in **[WEBOS_HOMEBREW.md](WEBOS_HOMEBREW.md)**.
