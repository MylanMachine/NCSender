# NCSender

A Windows desktop replacement for NCLite: sends CNC programs to machine controls over RS232 and receives (punches) programs back.

**What it adds over NCLite:** several machine panels side by side (1 to 6, two by default). Each panel has its own COM port, its own thread, and its own serial port object, so transfers run at the same time without affecting each other. Pausing, stalling, unplugging or cancelling one machine does not touch the others.

- C++17, Qt 6 (Widgets + SerialPort), CMake
- **Fully offline.** It makes no network calls, sends no telemetry, has no auto-update, and doesn't link Qt Network. Everything it writes stays in its own folder.
- Profiles, settings and the transfer log are plain files next to the exe.
- **Open source** under the [MIT License](LICENSE). The released exe also contains Qt, used under the LGPLv3; see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

**Download:** the latest standalone `NCSender.exe` is on the [Releases page](https://github.com/MylanMachine/NCSender/releases/latest).

---

## Contents

1. [Building](#building)
2. [Installing on a shop PC](#installing-on-a-shop-pc)
3. [Using it](#using-it)
4. [Drip feed (programs too big for the control)](#drip-feed-programs-too-big-for-the-control)
5. [Setting up machine profiles](#setting-up-machine-profiles)
6. [Testing without machines (com0com)](#testing-without-machines-com0com)
7. [Code layout](#code-layout)
8. [Troubleshooting](#troubleshooting)

---

## Building

### What you need

| Tool | Version | Notes |
|---|---|---|
| Visual Studio 2022 **or** Build Tools for VS 2022 | 17.x | Workload: **Desktop development with C++**. Includes CMake and Ninja. |
| Qt | 6.5 or newer (6.8 LTS recommended) | Kit **MSVC 2022 64-bit**, plus the **Qt Serial Port** module, which the Qt installer lists under *Additional Libraries*. Tick it, because it isn't selected by default. |
| CMake | 3.21+ | Comes with Visual Studio. |

You can use MinGW instead of MSVC: pick Qt's *MinGW 64-bit* kit together with the MinGW toolchain it offers.

> **Offline build machines:** the Qt and Visual Studio installers need internet access. Install them on a connected PC, or use their offline layouts (`vs_buildtools.exe --layout`, or copy a finished `C:\Qt\6.x.x\msvc2022_64` folder). Once the tools are installed, building and running NCSender needs no network.

### Build with the script

Open **Developer PowerShell for VS 2022**, `cd` to this folder and run:

```powershell
.\build.ps1 -QtDir C:\Qt\6.8.3\msvc2022_64
```

If you leave out `-QtDir`, the script picks the newest Qt 6 kit under `C:\Qt`.

### Build by hand

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
cmake --build build --config Release
```

With Ninja (faster):

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
cmake --build build
```

### Build with Qt Creator

*File > Open File or Project >* `CMakeLists.txt`, choose the MSVC 2022 64-bit kit, then Build.

### Output

The Visual Studio generator writes everything to `build\bin\Release\`; Ninja writes it to `build\bin\`.

| File | What it is |
|---|---|
| `NCSender.exe` | The desktop app |
| `nccli.exe` | Headless sender/receiver built on the same engine (used by the tests) |
| `nctestpeer.exe` | Simulated CNC control, for testing |
| `nctestrun.exe` | Automated test suite over com0com virtual ports |
| `Qt6*.dll`, `platforms\`, ... | Qt runtime, copied by `windeployqt` after each build |

CMake options:

- `-DNCSENDER_BUILD_TOOLS=OFF` builds the app only.
- `-DNCSENDER_DEPLOY_QT=OFF` skips copying the Qt DLLs.

This normal build is the one to use while developing and testing. For the shop, build the single exe described next.

### Standalone single-exe build (for the shop)

This produces **one `NCSender.exe`** with Qt and the C++ runtime built in. It needs no DLLs, no Visual C++ redistributable and no installer: copy the file anywhere and double-click it.

It needs a *static* build of Qt, which the Qt installer doesn't provide. You only have to make it once per build PC (it's already done on this one, in `C:\Qt\6.8.3-static`):

1. From `https://download.qt.io/archive/qt/6.8/6.8.3/submodules/`, download `qtbase-everywhere-src-6.8.3.tar.xz` and `qtserialport-everywhere-src-6.8.3.tar.xz`. Check them against `md5sums.txt` in the same folder, then extract both into `C:\Qt\_src\`.
2. In **Developer PowerShell for VS 2022** (about 30-60 minutes):

   ```powershell
   mkdir C:\Qt\_b\qtbase; cd C:\Qt\_b\qtbase
   C:\Qt\_src\qtbase-everywhere-src-6.8.3\configure.bat -static -static-runtime -release -prefix C:\Qt\6.8.3-static -nomake examples -nomake tests -no-opengl -no-dbus -no-feature-sql
   cmake --build . --parallel
   cmake --install .
   mkdir C:\Qt\_b\qtserialport; cd C:\Qt\_b\qtserialport
   C:\Qt\6.8.3-static\bin\qt-configure-module.bat C:\Qt\_src\qtserialport-everywhere-src-6.8.3
   cmake --build . --parallel
   cmake --install .
   ```

Then, any time you want a new exe, run this from this folder in an ordinary PowerShell window:

```powershell
.\build-standalone.ps1
```

The result is `dist\NCSender.exe`. CMake detects the static Qt by itself and switches to the static C++ runtime.

> **Licence note:** Qt is linked into the exe under the LGPLv3. The exe is published on GitHub together with this full source and the build scripts, which is what lets anyone relink it against a different Qt, as the LGPL requires. Keep [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) with any release.

---

## Installing on a shop PC

1. Download `NCSender.exe` from the latest release at https://github.com/MylanMachine/NCSender/releases/latest (or copy `dist\NCSender.exe` over). Put it in its own folder (for example `C:\NCSender\`), because it keeps its profiles and log next to itself. Nothing needs installing, and it doesn't touch the registry.
2. Double-click `NCSender.exe`. To get a Start-menu or desktop shortcut, right-click it and choose *Pin to Start* or *Send to > Desktop (create shortcut)*.

(If you use the normal developer build instead, copy the whole `build\bin` folder, because the Qt DLLs have to stay next to the exe. Run `vc_redist.x64.exe` once if the PC lacks the Visual C++ runtime.)

Windows **Smart App Control** or **SmartScreen** may warn about, or block, an unsigned exe it hasn't seen before. The permanent fix is to sign `NCSender.exe` with a code-signing certificate.

On first run NCSender creates these files next to the exe:

| File | Contents |
|---|---|
| `profiles.json` | Machine profiles. Created the first time you save in the profile editor. |
| `ncsender.ini` | Window layout, and the last profile and file used in each panel |
| `transfer_log.csv` | One line per transfer. Opens in Excel. |
| `profiles.json.bak` | The previous profiles file, kept on every save |

If the exe's folder is read-only (for example `C:\Program Files`), these files go to `%LOCALAPPDATA%\NCSender\NCSender\` instead. *File > Open Data Folder* shows where they are.

---

## Using it

Each machine is a card:

```
+---------------------------------------------------------------------+
| Machine A                                               ( Sending ) |  <- state badge, coloured
| Profile  [Robodrill                                              v] |
|          COM3 . 9600 7-E-2 . XON/XOFF . CR LF          * COM3 connected
| Program  [Browse...]  O1001.nc  C:\Programs                    [ R ] |
|          23 lines . 412 bytes on disk . 437 bytes will be sent       |
| [ Program | Received data ]                           [Follow line] |
|   1 | %                                                             |
|   2 | O1001 (FACE AND DRILL)                                        |
| > 3 | G00 X0 Y0                 <- line being sent, highlighted      |
| [Send] [Drip Feed] [Receive]              [Received Folder] [Cancel] |
| Line 3 of 23                                  210 / 437 bytes (48%) |
| [=================>                                               ] |
| Sending O1001.nc to COM3                                            |
+---------------------------------------------------------------------+
Recent transfers: Time | Machine | Direction | File | Bytes | Result | Detail
```

- **The program view** shows the file with line numbers and G-code colouring (N numbers grey, G codes blue, M codes purple, axis words teal, feeds/speeds/tools orange, comments green, % red). During a Send or Drip Feed, the line going out on the cable is highlighted and the view follows it. Turn **Follow line** off to scroll around freely. Files up to 8 MB are shown and tracked in full; for bigger files, the first 8 MB are.
- **Pick a file** with *Browse*, or by dragging a file onto the panel. The panel shows the file's name, with its folder beside it (hover for the full path). The program view shows the text, the line count, the size on disk, and the exact number of bytes that will go out once the profile's line ending, % and leader/trailer are applied.
- **Send.** Opens the port with the profile's settings, sends, then closes the port.
  - **Plain send** (*Wait for XON* off): put the control in program input mode first (EDIT > READ / INPUT), then click **Send**.
  - **Wait-for-XON send** (*Wait for XON* on): click **Send** first. The status shows *Waiting for XON*. Then start READ at the control. The transfer begins when the control sends XON (DC1). It fails with a clear message if XON doesn't arrive within the profile's XON timeout.
  - While sending, the status shows *Paused* if the control holds the transfer with XOFF or by dropping CTS, and goes back to *Sending* when it releases.
- **Receive.** Opens the port and listens. Start the punch at the control (EDIT > PUNCH / OUTPUT). Incoming text shows live in *Received data*. Receive stops at the closing `%`, or after the silence timeout. The program is saved as `<profile>_<O-number>_<yyyyMMdd_HHmmss>.nc` in the profile's receive folder. Click **Received Folder** to open that folder.
- **Cancel** stops the transfer immediately and releases the port. If a receive was cancelled, whatever had arrived is still saved, with `_PARTIAL` in the name.
- **A punch that stops early** (it opened with `%` but never closed, for example the cable was pulled) is reported as an **Error**, not Done. What arrived is saved with `_PARTIAL` in the name so it isn't mistaken for a complete program.
- **If the receive folder can't be reached** (offline network share, missing drive, disk full), the receive still runs. The status line warns you, and the program is saved in NCSender's own `received` folder (or the Windows temp folder as a last resort). The message says exactly where. A received program is never thrown away.
- **Refresh Ports** (F5) re-scans the COM ports. Each panel tells you if its profile's port isn't present.
- **Recent transfers** at the bottom lists the last 500 log entries. *Open Log File* opens `transfer_log.csv`.

Status values: `Idle`, `Opening`, `Waiting for XON`, `Sending`, `Paused`, `Receiving`, `Finishing`, `Done`, `Cancelled`, `Error`. Error messages say what went wrong, for example:

- *COM3 is already in use by another program...*
- *COM3 was not found. Check that the USB-serial adapter is plugged in...*
- *Lost connection to COM3 - the serial adapter was unplugged...*
- *Timed out after 60 s waiting for XON from the control...*
- *File not found: ...*

Any panel can use any profile. If you try to use a COM port that another panel is busy on, you get a clear error instead of a clash.

### Showing more or fewer machines

The **Machines** menu (and the toolbar) sets how many panels you see, from 1 to 6:

- **Add Machine** (Ctrl+Shift+N) adds the next panel: Machine C, then D, and so on.
- **Remove Machine** hides the last panel.
- **Show 1 machine ... Show 6 machines** jumps straight to a number.

Up to three machines sit side by side; four or more are arranged in two rows. The number is remembered the next time NCSender starts, and so is each panel's profile and file. A newly added panel starts with **no profile selected**, so it can't accidentally use another machine's COM port: pick its profile first. A panel that is in the middle of a transfer can't be removed until it finishes or you cancel it.

---

## Drip feed (programs too big for the control)

**Drip Feed** runs a program straight from the PC while the machine cuts it, so the program never has to fit in the control's memory. It is built for Fanuc controls such as the Robodrill, but works with any control that reads in Remote/DNC mode and paces the PC with XON/XOFF.

Once you click **Drip Feed**, the panel stays armed: **every time the operator presses Cycle Start on the machine, the program runs again from the top.** Nobody needs to touch the PC between parts. It stays armed until you click **Cancel**.

### One-time setup at the control

The exact screens and parameter numbers depend on the control. Check them in your control's manual before relying on them.

1. Set the control's **I/O channel** to the RS-232 port the cable is plugged into (on most Fanuc controls this is on the SETTING screen, or parameter 20).
2. Set that channel's baud rate, stop bits and code (ISO) to match the NCSender profile. On many Fanuc controls these are parameters 101-103 for channel 0, 111-113 for channel 1 and 121-123 for channel 2.
3. In the NCSender profile, use **XON/XOFF** flow control. The control uses XON/XOFF to start the program and to hold the PC back while its buffer is full, so drip feed doesn't work without it. (The profile's *Wait for XON* and *XON timeout* settings don't matter here: drip feed always waits for the control, however long that takes.)
4. Make sure the program ends with **M30** followed by a **%** line. NCSender adds the % if it's missing, as long as *Add % at start and end* is on.

### Running parts

1. Select the program in the panel and click **Drip Feed**. The panel shows **Ready**: put the machine in Remote mode and press Cycle Start.
2. At the machine, select **RMT** (Remote) mode and press **Cycle Start**. The control sends XON, and the program streams in as fast as the machine uses it. While the machine works through its buffer, the status shows *Paused*. That is normal.
3. When the control has the whole program, the run is logged, and the panel shows **Ready** again with a count of completed runs.
4. For the next part, press **Cycle Start** again. The file is read from disk again for each run, so an edit saved between parts is used on the next one.
5. Click **Cancel** when you're done for the day.

### If you press Reset part way through a program

**Click Restart on the PC before you press Cycle Start again.** After a Reset, the PC is still holding the rest of the program. Without Restart, the next Cycle Start would carry on from the middle of the program instead of from the top. Restart (the Drip Feed button, while drip feed is running) throws away the rest of the current run and arms again from the top. The panel reminds you of this whenever the control is holding data.

### If the panel stays on Paused after the machine has finished

The control stopped reading before it took the closing %. NCSender will not mark the run done or move on while part of the program is still unsent, because that could start the next part from the wrong place. Click **Restart** before the next Cycle Start. If this happens on every run, check that the program ends with `M30` and then `%`.

### Notes

- There is no size limit. The file is read from disk a little at a time and never held in memory (a normal **Send** is limited to 64 MB).
- Leader characters are sent at the start of each run; trailer characters are never sent in drip feed. The control stops reading at the closing %, so anything after it would sit in the port and go out at the start of the next run.
- Every run is a line in the transfer log (*Drip fed N bytes*). Cancel and Restart are logged as *Cancelled*.

---

## Setting up machine profiles

Open *Profiles > Edit Profiles...* (Ctrl+P). Use **Add**, **Duplicate** or **Delete** in the list, edit the fields on the right, then click **OK** to save to `profiles.json`. A transfer that is already running keeps the settings it started with.

### Fields

| Field | Meaning | Typical Fanuc |
|---|---|---|
| Profile name | Shown in the panel dropdown and used in received file names | `Haas VF2`, `Fanuc 0i Lathe` |
| COM port | Pick from the detected list or type one | `COM3` |
| Baud rate | Must match the control's I/O channel parameter | 9600 (4800 on older controls) |
| Data bits | | 7 (ISO) or 8 |
| Parity | | Even (with 7 data bits) or None (with 8) |
| Stop bits | | 2 |
| Flow control | **XON/XOFF**: the control sends DC3 to pause and DC1 to resume. **RTS/CTS**: the hardware handshake lines; the cable must have them wired. **None**. | XON/XOFF |
| Line ending | Every line ending in the file is converted to this before sending | CR LF or LF (Fanuc ISO uses LF and ignores CR) |
| Add % at start and end | Counts the lines that start with `%`. None: wraps the program in `%` lines. One, with program lines after it: it's the opening `%`, so only a closing `%` is added (comments above the opening `%` are left alone). One at the end: only an opening `%` is added. Two or more: the file is sent as is. | On |
| Leader / Trailer | N copies of a character sent before and after the program. 0x00 = NUL (tape feed). | 0, or 10-50 x 0x00 |
| Character delay | Pause after each character, for controls that overrun. 0 = full speed. | 0 |
| Wait for XON before sending | See [Using it](#using-it) | Depends on how your operators work |
| XON timeout | How long to wait for that XON. "Wait until cancelled" = no limit. | 60 s |
| Program folder | Where *Browse* opens | `D:\Programs\Mill1` |
| Save to folder (receive) | Where received programs go. Blank = `<program folder>\received` | |
| File extension | For received files | `nc` |
| Wait for first data | Receive gives up if nothing arrives within this time. "Wait until cancelled" = no limit. | Wait until cancelled |
| Stop after silence | Once the program has started, ends the receive after this many silent seconds even without a closing `%`. The data is always kept: if the program opened with `%` it's flagged as incomplete (Error, `_PARTIAL` file), and a program without `%` markers ends normally. Leader or noise bytes alone don't start this timer. "Off" = only the closing `%` or Cancel ends it. | 10 s |
| Remove NUL leader and DC codes | Strips 0x00 and DC1-DC4 from the saved file | On |

The first run includes four profiles:

- **Robodrill** (COM3) and **Hawk 150 Lathe (Fanuc 21i-T)** (COM4): the shop's machines, at 9600 baud, 7 data bits, even parity, 2 stop bits, XON/XOFF. The baud rate is confirmed for both. The data format is the standard Fanuc setting and the COM ports are placeholders, so check them: the ports in Windows Device Manager (*Ports (COM & LPT)*), the format on each control's SETTING screen / I/O channel parameters. The panel shows *COM3 not found* until the port is right.
- **Virtual Test A** (COM10) and **Virtual Test B** (COM12) for com0com testing.

Edit or delete them as needed. If a `profiles.json` already exists next to the exe, it is used instead; add the machines in the profile editor, or delete the file to get these defaults.

`profiles.json` is plain JSON, so you can also edit it by hand or copy it between PCs. Keys match the names above, for example `"baudRate": 9600, "parity": "even", "stopBits": "2", "flowControl": "xonxoff", "lineEnding": "CRLF"`.

---

## Testing without machines (com0com)

[com0com](https://com0com.sourceforge.net/) is a free virtual null-modem driver. It creates pairs of COM ports wired to each other: whatever is written to one comes out of the other, and the RTS/CTS and DTR/DSR lines are cross-connected like a real null-modem cable. NCSender talks to one end of each pair, and `nctestpeer.exe` plays the CNC control on the other end.

```
NCSender / nccli  COM10 <==== com0com pair ====> COM11  nctestpeer (Machine A's "control")
NCSender / nccli  COM12 <==== com0com pair ====> COM13  nctestpeer (Machine B's "control")
```

### 1. Install com0com

1. Download the **signed** x64 build. On SourceForge (com0com > Files) it's the `com0com-2.2.2.0-x64-fre-signed.zip` package. Unsigned or newer test builds need Windows test-signing mode, which you should not turn on on a shop PC. Copy the zip over by USB if the PC is offline.
2. Run `setup.exe` as Administrator. Untick the option that creates the default `CNCA0/CNCB0` pair (or delete that pair later).
3. If Windows refuses to load the driver (Secure Boot / driver signature errors), use a separate test PC or VM for virtual-port testing. The automated tests are only needed on whatever PC you develop on.

### 2. Create the two pairs, with baud-rate emulation on

Open an **Administrator** command prompt in the com0com folder (usually `C:\Program Files (x86)\com0com`):

```bat
setupc install PortName=COM10,EmuBR=yes PortName=COM11,EmuBR=yes
setupc install PortName=COM12,EmuBR=yes PortName=COM13,EmuBR=yes
setupc list
```

`EmuBR=yes` matters. Without it com0com moves data instantly, whatever the baud rate. That makes the flow-control and two-at-once tests meaningless, and `nctestrun` warns about it.

If COM10 to COM13 are already taken on that PC, use other numbers and pass them to `nctestrun` (`--app-port-a COM20 --peer-port-a COM21 ...`). Also update the two *Virtual Test* profiles to match.

To remove the pairs later: `setupc list`, then `setupc remove 0` (and `remove 1`).

### 3. Run the unit and GUI tests

Three test programs are built with the project. They use Qt's own test framework and need no extra tools:

| Program | What it covers | Needs com0com? |
|---|---|---|
| `nccore_tests.exe` | Program formatting (`%` rules, line endings, BOM/Ctrl-Z, blank files), profiles and their validation, `profiles.json` loading (corrupt, BOM, missing list), the transfer log (escaping, locked file), file loading, channel errors, cancel, port release | No (a few tests use COM10 if present) |
| `nctransfer_tests.exe` | Real transfers through the transfer engine, with the test itself playing the control: exact bytes, XON/XOFF pause and resume, wait-for-XON, cancel then resend, port in use, noise before `%`, leader-only, cut-off punch, unreachable receive folder, two machines saving the same program at once, send on one machine while the other receives | Yes |
| `ncui_tests.exe` | The real NCSender window driven by code: Send, Receive and Cancel buttons, status line, progress bar, preview, log table, both machines at once, port conflicts, deleted/renamed profiles, the profile editor (port autocomplete, baud input, duplicate names, delete), a locked log file, closing the window mid-transfer | Most tests |

Run them from the bin folder:

```powershell
.\nccore_tests.exe
```

```powershell
.\nctransfer_tests.exe
```

```powershell
.\ncui_tests.exe
```

Or run all three with `ctest --test-dir build`. Each prints `PASS`/`FAIL` per test and a `Totals:` line, and the exit code is the number of failures. The GUI tests draw offscreen, so no windows appear. All of them use a temporary data folder, so they never touch your real profiles or log.

### 4. Run the end-to-end suite

The test suite is `nctestrun.exe`, built into the bin folder next to the app. It's a normal program, not a script, so PowerShell execution policy and antivirus script-scanning don't get in the way. Build first, then from the bin folder (for example `build\bin\Release`):

```powershell
.\nctestrun.exe
```

It takes about 2-3 minutes at 9600 baud. For each test it starts `nctestpeer.exe` as the simulated control and `nccli.exe` as the sender, then checks what they report. Each test prints PASS or FAIL with details, and a summary follows at the end. Exit code: 0 = all passed, 1 = a test failed, 2 = setup problem (for example the COM ports don't exist). All logs, received files, the generated test programs and the exact command lines go to `testrun-out\` in the bin folder.

| Test | Checks |
|---|---|
| T01 / T02 | Plain send on A and on B, content byte-exact (ignoring CR/NUL) |
| T03 | `%` is added at start and end when the file lacks it |
| T04 | *Wait for XON*: nothing leaves the PC until the control sends XON |
| T05 | No XON arrives: clean error after the timeout |
| T06 | **XON/XOFF**: the control pauses the sender every 1500 bytes for 1.5 s. Bytes in the first 150 ms of a pause count as already in flight; after that, nothing may arrive until the control resumes. The content must be intact. |
| T07 | **RTS/CTS**: same, with the control dropping CTS |
| T08 | **Receive**: the peer punches leader + DC2 + program + DC4 + trailer; the saved file must match |
| T09 | Receive cut off before the closing `%`: ends on the silence timeout, is reported as incomplete, and the data is saved as `_PARTIAL` |
| T10 | **Two sends at once** on A and B: both correct, and they must overlap in time (truly parallel) |
| T11 | Send on A and receive on B at the same time |
| T12 | **Independence**: A is held by XOFF for long stretches while B runs; B must finish far sooner than A |
| T13 | Port already in use: clear "in use" error |
| T14 | File not found: clear error |
| T15 | Cancel mid-send releases the port; the next send on it works |

Options:

| Option | Effect |
|---|---|
| `--only T06,T10` | Run only the listed tests |
| `--list` | List the tests without running anything |
| `--baud 19200` | Run at a different baud rate (faster) |
| `--large-lines 1000` | Make the large test programs bigger |
| `--app-port-a`, `--peer-port-a`, `--app-port-b`, `--peer-port-b` | Use different COM numbers (defaults COM10-COM13) |
| `--bin-dir path` | Where `nccli.exe` and `nctestpeer.exe` are (default: next to `nctestrun.exe`) |
| `--out-dir path` | Where logs go (default: `testrun-out\` next to the exe) |

### 5. Test the GUI by hand

Open two terminals in the bin folder (for example `build\bin\Release`), start NCSender, and select **Virtual Test A** in panel A and **Virtual Test B** in panel B.

**Send and flow control.** Terminal 1 (Machine A's control, pausing every 1000 bytes):

```powershell
.\nctestpeer.exe receive --port COM11 --pause-every 1000 --pause-ms 2000
```

Terminal 2 (Machine B's control):

```powershell
.\nctestpeer.exe receive --port COM13
```

Pick a program in each panel (`tests\programs\O1001.nc`, or the larger generated files in `testrun-out\` after a test run) and click **Send** on both. Panel A should flip between *Sending* and *Paused (XOFF received)* while panel B keeps going. Each terminal prints `PEER RESULT: PASS ...` when its transfer ends.

**Wait for XON.** In the profile editor, turn on *Wait for XON* for Virtual Test A. Click **Send**; the status sits at *Waiting for XON*. Then run:

```powershell
.\nctestpeer.exe receive --port COM11 --xon-delay 0
```

The transfer starts as soon as the peer sends XON.

**Receive.** Click **Receive** on panel A, then:

```powershell
.\nctestpeer.exe punch --port COM11 --file ..\..\..\tests\programs\O1001.nc --leader 20 --dc2
```

The text appears live in *Received data* and is saved with a timestamped name.

**Errors.** Run `.\nctestpeer.exe echo --port COM10` (this holds the app's port), then click Send on A: you should get "COM10 is already in use". Remove a profile's COM port, or point it at `COM99`, to see "not found".

**Unplug mid-transfer** needs a real USB-serial adapter. Two adapters joined by a null-modem cable (or one adapter looped to another PC running `nctestpeer`) work the same as com0com, using real port names. Pull the adapter during a send: the panel shows *Lost connection to COMx*, the log records it, and the other panel is unaffected.

### nctestpeer reference

```
nctestpeer receive --port COM11 [--expect file] [--out file]
                   [--xon-start | --xon-delay ms]
                   [--pause-every N --pause-ms ms --pause-mode xoff|rts]
                   [--grace-ms 150 --tolerance 0]   (bytes allowed after the grace window of a pause)
                   [--idle-timeout ms] [--max-wait ms]
nctestpeer punch   --port COM11 --file prog.nc [--leader N] [--dc2] [--start-delay ms]
                   [--char-delay ms] [--truncate N] [--eol keep|lf|crlf|cr]
nctestpeer echo    --port COM11
nctestpeer list
common: --baud 9600 --databits 7 --parity even --stopbits 2
```

### nccli reference

```
nccli --send "Profile Name=C:\path\prog.nc" [--send ...] [--receive "Profile Name" ...]
      [--profiles file.json] [--set key=value ...] [--log file.csv]
      [--expect-error text] [--timeout seconds] [--quiet]
nccli --list-ports
```

All jobs start at the same moment, each on its own channel and thread. `--set` overrides any `profiles.json` key for every job, for example `--set waitForXon=true --set baudRate=4800`.

---

## Code layout

```
NCSender/
  CMakeLists.txt
  build.ps1
  src/core/                 nccore static library: Qt Core + SerialPort only, no UI
    SerialSettings.*        parity/stop bits/flow/line ending enums and conversions
    MachineProfile.*        profile struct, JSON load/save, validation
    ProfileStore.*          profiles.json (atomic save + .bak)
    ProgramFormatter.*      line endings, % wrapping, leader/trailer
    DataSource.*            where send bytes come from: in memory, or streamed from disk for drip feed
    TransferTypes.*         TransferState / TransferResult
    TransferWorker.*        one QSerialPort + send/receive state machine, event driven
    MachineChannel.*        owns one QThread + one TransferWorker; the UI's handle
    TransferLog.*           thread-safe CSV transfer log
    AppPaths.*              where data files live
  src/app/                  Qt Widgets GUI
    MainWindow.*            menus, two panels, port refresh, log wiring
    MachinePanel.*          one machine's controls; talks only to its MachineChannel
    CodeView.*              program view: line numbers, G-code colours, line being sent
    Theme.*                 colours, fonts, style sheet and the icons (drawn in code)
    images/                 drop-down arrow images used by the style sheet
    ProfileEditorDialog.*   profile editor
    LogView.*               recent transfers table
  tools/nccli/              headless front end to nccore (tests, scripting)
  tools/nctestpeer/         simulated control (independent of nccore)
  tools/nctestrun/          automated com0com test suite (runs nccli + nctestpeer)
  tests/
    programs/               sample programs for manual testing
```

**Threading model.** Each `MachineChannel` creates a `QThread` and moves a `TransferWorker` onto it. The worker owns that machine's `QSerialPort` and all of its timers. Commands go from the UI to the worker as queued calls, and the worker's signals (`stateChanged`, `progress`, `dataReceived`, `runFinished`, `finished`) come back as queued signals. The worker never blocks: it reacts to `readyRead`, `bytesWritten`, timers and errors. A cancel or an unplugged adapter is handled within milliseconds, and nothing it does can freeze the window. The two channels share no objects. The only shared resource is the log file, which is mutex-guarded and written from the UI thread.

**How a send works.** The worker pulls small chunks (128 bytes) from the `DataSource` and never keeps more than about 256 bytes queued inside `QSerialPort`. XON/XOFF and RTS/CTS are enforced by the Windows serial driver, which stops within about a character. Progress counts bytes the driver has confirmed. During a send the worker polls the driver's status (`ClearCommError`) to show *Paused (XOFF)* or *waiting for CTS*. For *Wait for XON*, the port opens with flow control off so the app can see the control's first DC1, because the driver would otherwise swallow it. XON/XOFF is switched on just before sending. At the end it waits for the driver's output queue to drain, plus a short guard time for the adapter's FIFO, before closing the port.

**How drip feed works.** `StreamingFileSource` reads the file twice: once when the run is armed, to decide where the % lines go and to work out the exact size, then again while sending, a line at a time. It produces exactly what `ProgramFormatter::format()` would, minus the trailer, and both use the same `PercentPlanner`. The worker waits for XON with flow control off, as for *Wait for XON*. A run is complete once everything has left the driver; in drip feed a hold after that is ignored, because the control sends XOFF when it reads the closing %. It then emits `runFinished` and re-arms on the **same open port**: it re-reads the file, turns flow control off again and waits for the next XON. The port is never closed or purged between runs, so the end of a program can't be lost from a USB adapter's buffer. If the control holds the transfer with part of the program still unsent, the run stays open (*Paused*) and the worker never skips ahead.

**Reuse.** `nccore` has no UI code. `nccli` is a working example of driving it without a GUI: about 30 lines create channels and start jobs.

---

## Troubleshooting

| Symptom | Check |
|---|---|
| "is already in use" | Another program (NCLite, a terminal, a second NCSender) has the port open, or the other panel is set to the same port. |
| "was not found" | The adapter is unplugged, or its COM number changed. Check Device Manager > Ports, then press F5. |
| Stuck at *Waiting for XON* | The control isn't in READ mode, or baud/parity don't match, or the cable has no RX line, or the profile shouldn't have *Wait for XON* on (with a plain send, put the control in READ mode first). |
| Stuck at *Paused (XOFF)* | The control's buffer is full and it never sent XON. Look for an alarm on the control. |
| Control alarms with an overrun/parity error | Baud, parity, data or stop bits don't match. Try a character delay of 1-5 ms on older controls. |
| Received file has garbage | Baud/parity mismatch. 7-E-2 vs 8-N-1 is the usual cause. |
| "An Application Control policy has blocked this file" | Windows Smart App Control blocks unsigned programs it doesn't recognize, and a fresh build can trip it. The fix is to sign the exes with a code-signing certificate, or build/test on a PC without Smart App Control. Turning it off often can't be undone without resetting Windows. |
| `nctestrun` hangs with empty `*-peer.out.txt` files | `nctestpeer.exe` isn't starting. Run it by hand (`.\nctestpeer.exe list`) to see why, for example the Smart App Control block above. |
| Receive ends early | Increase *Stop after silence*, or set it to Off. |
| Nothing received | Check the punch device setting on the control (I/O channel), the cable, and the DTR/DSR wiring. NCSender raises DTR and RTS on open. |
