# SidecarTridge Multi-device Oric Emulator

This micro-firmware app for the **SidecarTridge Multi-device platform** emulates an
**Oric** computer in the Atari ST class of machines.

This emulator is based on the awesome [Reload Emulator](https://github.com/vsladkov/reload-emulator) project by Vladimir Sladkov. I can only thank him for his great work! 

## ⚠️ Attention

This emualator is designed **only for low-resolution monitors**. It will **not** work in high-resolution modes.

## 🚀 Installation

To install the Emulator app on your SidecarTridge Multi-device:

1. **Launch** the **Booster App** on your SidecarTridge.
2. Open the **Booster web interface** in your browser.
3. Go to the **Apps** tab and select **Oric Emulator** from the list.
4. Click **Download** to install the app to your SidecarTridge’s microSD card.
5. Once installed, select the app and click **Launch**.

After installation, the Oric Emulator will start automatically every time your Atari is powered on.

## 🕹️ Usage

### Configuration

The emulator requires an image file with the Oric ROM to function. There are plenty of Oric ROM images available online, try searching for "orica.zip" or similar. Once you have the ROM image, rename to `rom.img` and copy it to the directory `/oric` on the SidecarTridge microSD card. 

This should be enough to get started, but if you want to load applications and games, copy `.TAP` files to the same `/oric` directory with the following structure:

- `0.tap` - first tape file, loaded when pressing F1 key.
- `1.tap` - second tape file, loaded when pressing F2 key.
- `2.tap` - third tape file, loaded when pressing F3 key.
- and so on...

Once the files are in place, launch the Oric Emulator app from the Booster interface or by rebooting your Atari with the Multi-device set to auto-launch the app.

### Emulator Controls

The emulator at this moment only supports file loading from tape images. Use the following keys to load the corresponding tape files:

* **F1** → Load `0.tap` in the virtual cassette drive.
* **F2** → Load `1.tap` in the virtual cassette drive.
* **F3** → Load `2.tap` in the virtual cassette drive.
* and so on...

After loading the tape file, use the command `CLOAD""` in the Oric BASIC prompt to load the program from the virtual tape.

You can find plenty of Oric software online in places like [Oric.org](http://www.oric.org/).

The `HELP` button will perform a soft reset of the Oric machine. The `UNDO` button will pause the emulation.

### ⏏️ Exiting to Booster

The emulator cannot exit back to GEM or the Booster interface directly. To exit the emulator and return to the Booster app, you need to **power cycle** your Atari ST and at the same time hold down the **SELECT** button on the SidecarTridge. This will interrupt the normal boot process and launch the Booster app instead of the Oric Emulator.


### 🔄 Power Cycling

After a power cycle, the emulator auto-launches.


## 🛠️  Under the hood

The Oric Emulator is built using the Reload Emulator core, adapted to run on the SidecarTridge Multi-device platform. The emulator uses the following components.

## What’s next

Maintining and improving this emulator is a hobby project for me. If you have any suggestions or find any issues, please feel free to open an issue on the GitHub repository.

## License

This project is released under the GNU General Public License v3.0. See the [LICENSE](LICENSE) file for details.
