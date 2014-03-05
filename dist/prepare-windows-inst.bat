:: This script prepares the Win32 installation.
:: 
:: The plugin must be already built in build subdirectory.
:: The resulting file is placed in windows/purple-vk-plugin-VERSION-win32.exe

set MSYSPATH=C:\MinGW\msys\1.0\bin

set PATH=%PATH%;C:\Program Files\NSIS;C:\Program Files (x86)\NSIS;%MSYSPATH%

mingw32-strip ../build/libpurple-vk-plugin.dll

sh -c ". version; cat windows/purple-vk-plugin.nsi.template | sed s/PACKAGENAME/$PACKAGENAME/g | sed s/PACKAGEVERSION/$PACKAGEVERSION/g > windows/purple-vk-plugin.nsi"

makensis windows/purple-vk-plugin.nsi
