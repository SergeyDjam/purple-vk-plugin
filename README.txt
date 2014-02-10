Description
===========

Purple-vk-plugin is a plugin for Pidgin, which supports Vk.com messaging.

Currently only Pidgin on Linux is actively supported, however Windows version
is planned (any development help on this front is greatly appreciated!).

Installing
==========

Arch Linux
==========

purple-vk-plugin is maintained (or at least seems to be :-) in AUR: https://aur.archlinux.org/packages/purple-vk-plugin/

In order to build the package execute the following commands:
  curl -O https://aur.archlinux.org/packages/pu/purple-vk-plugin/purple-vk-plugin.tar.gz
  tar xvfz purple-vk-plugin.tar.gz
  cd purple-vk-plugin
  makepkg -s
  pacman -U purple-vk-plugin-*.pkg.tar.xz

Building
========

Requirements:
 * cmake >= 2.8 (tested on 2.8.10 and 2.8.11)
 * C++11-conformant compiler (tested on gcc 4.7.2, 4.8.1, clang 3.2, 3.4)
 * libpurple >= 2.10 (tested on 2.10.7)
 * libxml2 >= 2.9 (tested on 2.9.1)


The instructions will be given for recent Ubuntu, however should be easily translatable to other
Linux distributions.

1. Install a compiler and CMake
   For Ubuntu use:
     apt-get install g++ cmake
2. Install development packages for the libraries.
   For Ubuntu use:
     apt-get install libpurple-dev libxml2-dev
3. Create an empty build subdirectory of top directory and go into it:
     mkdir build
     cd build
4. Run cmake from the build subdirectory:
     cmake ..
   It should finish without errors, like:
     -- Configuring done
     -- Generating done
     -- Build files have been written to: /home/oleg/projects/purple-vk-plugin/build
5. Run
     make
   It should finish without errors and warnings, like:
     [100%] Built target purple-vk-plugin
6. For system-wide installation run
     make install
7. For manual installation copy libpurple-vk-plugin.so either to ~/.purple/plugins (create this directory
   if it does not exist) or to /usr/lib/purple-2. Copy all contents from data/protocols subdirectory to
   /usr/share/pixmaps/pidgin/protocols.

Future development
==================

Currently, there are quite a few areas where the plugin is lacking:
 * Groupchat support. This is currently planned.
 * Pidgin on Windows support. This is currently planned.
 * Adium, Empathy (and other Telepathy-based clients) support. Usually these client do not fully
   implement libpurple (e.g. they do not support Request API => troubles with showing captcha
   requests), so they are a bit bothersome. Any help is appreciated.
 * Mostly minor problems in TODO.txt
