:: Path to unpacked sources of Pidgin
set PIDGINSRCPATH=g:\src\pidgin-2.10.9
:: Path to unpacked sources of Glib-2.28. Copy glib/glibconfig.h.win32 to glib/glibconfig.h
set GLIBSRCPATH=g:\src\glib-2.28.8

:: Path to directory, containing purple.dll, libglib-2.0-0.dll and libgio-2.0-0.dll
:: from Pidgin installation. Path should not contain spaces, so we copy the files to separate directory.
set PIDGINBINPATH=g:\src\pidgin-bin\

:: Path to mingw installation
set MINGWPATH=C:\mingw
:: msys-libxml2 must be installed
set MSYSPATH=%MINGWPATH%\msys\1.0

set PATH=%PATH%;C:\Program Files (x86)\CMake 2.8\bin\;C:\Program Files\CMake 2.8\bin\
set PATH=%PATH%;%MINGWPATH%\bin

cmake -G "MinGW Makefiles" ^
      -DZLIB_LIBRARIES=z ^
      -DGIO_INCLUDE_DIRS=%GLIBSRCPATH%;%GLIBSRCPATH%\glib;%GLIBSRCPATH%\gmodule ^
      -DGIO_LIBRARY_DIRS=%PIDGINPATH% ^
      -DGIO_LIBRARIES=gio-2.0-0 ^
      -DLIBXML2_INCLUDE_DIR=%MSYSPATH%\include\libxml2 ^
      -DLIBXML2_LIBRARY_DIRS=%MSYSPATH%\lib ^
      -DLIBXML2_LIBRARIES=xml2.a ^
      -DPURPLE_INCLUDE_DIRS=%PIDGINSRCPATH%\libpurple ^
      -DPURPLE_LIBRARY_DIRS=%PIDGINPATH% ^
      -DPURPLE_LIBRARIES=purple;glib-2.0-0 ^
      ..