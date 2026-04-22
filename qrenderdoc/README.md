# Windows build

To build with PySide2 support or SSL support in Qt download [this zip](https://renderdoc.org/qrenderdoc_3rdparty.zip) and unzip to this folder (the 3rdparty/ in the zip should go into the 3rdparty/ folder here) before building.

## HTTPS / Pipeline Agent (OpenSSL)

If `qrenderdoc.exe` fails HTTPS with `Error creating SSL context`, Qt did not find compatible OpenSSL DLLs.

1. Copy `libssl-*.dll` and `libcrypto-*.dll` from the **same** Qt `bin` directory you built with, next to `qrenderdoc.exe`, **or**
2. Set environment variable `RENDERDOC_OPENSSL_PATH` to a folder containing those two DLLs, **or**
3. Place them under `qrenderdoc/3rdparty/openssl/x64/` (or `Win32/`) and rebuild; the project copies `*.dll` from there to the output folder (see `3rdparty/openssl/README.txt`).

For QML-heavy reference UIs (glass / rounded kits), upstream Qt ecosystems include [QmlMaterial](https://github.com/hypengw/QmlMaterial), [Quey UI](https://github.com/quey-project/quey-ui), and widget sets such as [QFluentWidgets](https://github.com/zhiyiYo/QFluentWidgets); RenderDoc stays on QWidget + RDStyle for the main UI.
