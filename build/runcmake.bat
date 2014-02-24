:: Path to unpacked sources of Pidgin. There is no need to compile Pidgin, only headers
:: are needed.
set PIDGINSRCPATH=c:\src\pidgin-2.10.9
:: Path to unpacked sources of Glib-2.28. Copy glib\glibconfig.h.win32 to glib\glibconfig.h
:: There is no need to compile Glib, only headers are needed.
set GLIBSRCPATH=c:\src\glib-2.28.8

:: Path to directory, containing libpurple.dll, libglib-2.0-0.dll and libgio-2.0-0.dll
:: from Pidgin installation. Path should not contain spaces, so we copy the files
:: to a separate directory.
set PIDGINBINPATH=c:\src\pidgin-bin\

:: The following mingw packages must be additionally installed:
::   mingw32-libiconv
::   mingw32-libz
::
:: Please check that msys-zlib is not installed, as it fucks up the linking process.
::
:: Please check that mingw/msys/1.0/bin is not in PATH, or cmake will fail
:: with error about /bin/sh.exe in PATH.

:: Path to mingw installation
set MINGWPATH=C:\mingw

:: Get libxml2 from http://ftp.gnome.org/pub/GNOME/binaries/win32/dependencies/libxml2-dev_2.9.0-1_win32.zip
:: and http://ftp.gnome.org/pub/GNOME/binaries/win32/dependencies/libxml2_2.9.0-1_win32.zip (or whatever
:: current Pidgin Windows install guide says) and unpack into the directory.
set LIBXML2PATH=C:\mingw\libxml2

set PATH=%PATH%;C:\Program Files (x86)\CMake 2.8\bin\;C:\Program Files\CMake 2.8\bin\
set PATH=%PATH%;%MINGWPATH%\bin

:: libxml2, libz, libgcc and libstdc++ are all linked in statically to ease deployment

cmake -G "MinGW Makefiles" ^
      -DPURPLE_INCLUDE_DIRS=%PIDGINSRCPATH%\libpurple;%GLIBSRCPATH%;%GLIBSRCPATH%\glib;%GLIBSRCPATH%\gmodule ^
      -DPURPLE_LIBRARY_DIRS=%PIDGINBINPATH% ^
      -DPURPLE_LIBRARIES=purple;glib-2.0-0 ^
      -DGIO_LIBRARIES=gio-2.0-0 ^
      -DLIBXML2_DEFINITIONS=-DLIBXML_STATIC ^
      -DLIBXML2_INCLUDE_DIR=%LIBXML2PATH%\include\libxml2 ^
      -DLIBXML2_LIBRARY_DIRS=%LIBXML2PATH%\lib ^
      -DLIBXML2_LIBRARIES=xml2 ^
      -DZLIB_LIBRARIES=z.a ^
      ..
