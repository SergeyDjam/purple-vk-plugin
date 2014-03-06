Описание
========

Purple-vk-plugin --- это плагин для Pidgin, который позволяет принимать и посылать личные сообщения
с сайта Vk.com (Вконтакте)

В настоящее время активно поддерживается только версия под Linux, однако в разработке находится
и версия под Windows.

Установка
=========

Windows
-------

Нестабильные девелоперские релизы purple-vk-plugin выкладываются на
https://bitbucket.org/olegoandreev/purple-vk-plugin/downloads.
Файл инсталлятора называется purple-vk-plugin-НОМЕРВЕРСИИ-win32.exe 

Ubuntu Linux
------------

Нестабильные девелоперские релизы purple-vk-plugin выкладываются в PPA:
https://launchpad.net/~purple-vk-plugin/+archive/dev

Для установки выполните следующие команды:
::
  $ apt-add-repository ppa:purple-vk-plugin/dev
  $ apt-get update
  $ apt-get install purple-vk-plugin

Arch Linux
----------

Нестабильные девелоперские релизы purple-vk-plugin выкладываются в AUR:
https://aur.archlinux.org/packages/purple-vk-plugin/

Для установки выполните следующие команды:
::
  $ curl -O https://aur.archlinux.org/packages/pu/purple-vk-plugin/purple-vk-plugin.tar.gz
  $ tar xvfz purple-vk-plugin.tar.gz
  $ cd purple-vk-plugin
  $ makepkg -s
  $ pacman -U purple-vk-plugin-*.pkg.tar.xz

Другие дистрибутивы Linux
-------------------------

Бинарные сборки плагина выкладываются на https://bitbucket.org/olegoandreev/purple-vk-plugin/downloads.
Файл с бинарной сборкой называется purple-vk-plugin-НОМЕРВЕРСИИ-linux-bin.tar.gz. Архив содержит как i386,
так и x86-64 версию. Входящий в состав архива установочный скрипт копирует сам плагин и файлы данных
в требуемые директории.

Бинарники собираются на CentOS 6.5, так что должны быть совместимы с достаточно старыми дистрибутивами
(2009-2010 года).

Сборка
======

Требования:

* cmake >= 2.6
* компилятор, поддерживающий C++11 (проверялось gcc 4.6, 4.7, 4.8, clang 3.2, 3.4)
* libpurple >= 2.7
* libxml2 >= 2.7

Инструкции даются для свежих версий Ubuntu, но должны легко транслироватьс€ и на другие дистрибутивы Linux.

1. Установите компил€тор и CMake. Для Ubuntu используйте::

     $ apt-get install g++ cmake

2. Установите development пакеты дл€ библиотек. Для Ubuntu используйте::

     $ apt-get install libpurple-dev libxml2-dev

3. Перейдите в директорию build::

     $ cd build

4. Запустите cmake из директории build::

     $ cmake ..

   Запуск cmake должен закончиться без ошибок следующими сообщениями::

     -- Configuring done
     -- Generating done
     -- Build files have been written to: /home/oleg/projects/purple-vk-plugin/build

5. Запустите::

     $ make

   Запуск make должен закончиться без ошибок и предупреждений::

     [100%] Built target purple-vk-plugin

6. Для установки плагина для всех пользователей запустите из под пользователя root::

     $ make install

7. Для ручной установки скопируйте файл libpurple-vk-plugin.so либо в ~/.purple/plugins (создайте
   эту директорию, если она не существует) или в /usr/lib/purple-2. Скопируйте содержимое data/protocols
   в /usr/share/pixmaps/pidgin/protocols.

Description
===========

Purple-vk-plugin is a plugin for Pidgin, which supports Vk.com (Vkontakte) messaging.

Currently only Pidgin on Linux is actively supported, however Windows version
is being developed.

Installing
==========

Windows
-------

Unstable windows binaries can be downloaded from https://bitbucket.org/olegoandreev/purple-vk-plugin/downloads
Installer file is named purple-vk-plugin-VERSION-win32.exe

Ubuntu Linux
------------

purple-vk-plugin development releases are maintained in PPA:
https://launchpad.net/~purple-vk-plugin/+archive/dev

In order to install the package execute the following commands:
::
  $ apt-add-repository ppa:purple-vk-plugin/dev
  $ apt-get update
  $ apt-get install purple-vk-plugin


Arch Linux
----------

purple-vk-plugin development releases are maintained in AUR:
https://aur.archlinux.org/packages/purple-vk-plugin/

In order to build the package execute the following commands:
::
  $ curl -O https://aur.archlinux.org/packages/pu/purple-vk-plugin/purple-vk-plugin.tar.gz
  $ tar xvfz purple-vk-plugin.tar.gz
  $ cd purple-vk-plugin
  $ makepkg -s
  $ pacman -U purple-vk-plugin-*.pkg.tar.xz

Other Linux distro
------------------

Plugin binaries can be downloaded from https://bitbucket.org/olegoandreev/purple-vk-plugin/downloads
Binary builds are named purple-vk-plugin-VERSION-linux-bin.tar.gz. The archive contains both i386
and x86-64 builds. Included install script copies plugin .so file and data files into required directories.

Binaries were built on CentOS 6.5, so should be compatible with rather old distros (since 2009-2010).

Building
========

Requirements:

* cmake >= 2.6
* C++11-conformant compiler (tested on gcc 4.6, 4.7, 4.8, clang 3.2, 3.4)
* libpurple >= 2.7
* libxml2 >= 2.7

The instructions will be given for recent Ubuntu, however should be easily translatable to other
Linux distributions.

1. Install a compiler and CMake. For Ubuntu use::

     $ apt-get install g++ cmake

2. Install development packages for the libraries. For Ubuntu use::

     $ apt-get install libpurple-dev libxml2-dev

3. Create an empty build subdirectory of top directory and go into it::

     $ mkdir build
     $ cd build

4. Run cmake from the build subdirectory::

     $ cmake ..

   It should finish without errors::

     -- Configuring done
     -- Generating done
     -- Build files have been written to: /home/oleg/projects/purple-vk-plugin/build

5. Run::

     $ make

   It should finish without errors and warnings::

     [100%] Built target purple-vk-plugin

6. For system-wide installation run::

     $ make install

7. For manual installation copy libpurple-vk-plugin.so either to ~/.purple/plugins (create this directory
   if it does not exist) or to /usr/lib/purple-2. Copy all contents from data/protocols subdirectory to
   /usr/share/pixmaps/pidgin/protocols.
