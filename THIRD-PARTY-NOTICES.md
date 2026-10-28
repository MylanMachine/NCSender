# Third-party notices

## Qt

The released `NCSender.exe` is linked statically with the Qt 6.8.3 libraries
**Qt Core, Qt Gui, Qt Widgets and Qt Serial Port**, which are used under the
**GNU Lesser General Public License version 3** (LGPLv3).

- LGPLv3: https://www.gnu.org/licenses/lgpl-3.0.html
- GPLv3 (which the LGPLv3 builds on): https://www.gnu.org/licenses/gpl-3.0.html
- Qt licensing: https://doc.qt.io/qt-6/licensing.html
- Qt 6.8.3 source code: https://download.qt.io/archive/qt/6.8/6.8.3/submodules/
  (`qtbase-everywhere-src-6.8.3.tar.xz`, `qtserialport-everywhere-src-6.8.3.tar.xz`)

Qt is Copyright (C) The Qt Company Ltd. and other contributors.

### Relinking against a different Qt

The LGPL lets you use NCSender with a modified version of Qt. The complete
source code of NCSender and its build scripts are in this repository:

- `build.ps1` builds against any Qt 6 kit you install (Qt as DLLs).
- `build-standalone.ps1` builds the single-file exe against a static Qt you
  build yourself; the README's *Standalone single-exe build* section lists
  the exact Qt configure options used for the released exe.

## Microsoft C++ runtime

The released exe is built with Microsoft Visual C++ and links the C++ runtime
statically, as allowed by the Visual Studio licence terms for redistributable
code.
