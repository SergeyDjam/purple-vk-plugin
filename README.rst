��������
========

Purple-vk-plugin --- ��� ������ ��� Pidgin, ������� ��������� ��������� � �������� ������ ���������
� ����� Vk.com (���������)

� ��������� ����� ������� �������������� ������ ������ ��� Linux, ������ � ���������� ���������
� ������ ��� Windows.

���������
=========

Ubuntu Linux
------------

������������ ������������� ������ purple-vk-plugin ������������� � PPA:
https://launchpad.net/~purple-vk-plugin/+archive/dev

��� ��������� ��������� ��������� �������:
::
  $ apt-add-repository ppa:purple-vk-plugin/dev
  $ apt-get update
  $ apt-get install purple-vk-plugin

Arch Linux
----------

������������ ������������� ������ purple-vk-plugin ������������� � AUR:
https://aur.archlinux.org/packages/purple-vk-plugin/

��� ��������� ��������� ��������� �������:
::
  $ curl -O https://aur.archlinux.org/packages/pu/purple-vk-plugin/purple-vk-plugin.tar.gz
  $ tar xvfz purple-vk-plugin.tar.gz
  $ cd purple-vk-plugin
  $ makepkg -s
  $ pacman -U purple-vk-plugin-*.pkg.tar.xz

������ ������������ Linux
-------------------------

�������� ������ ������� ������������� �� https://bitbucket.org/olegoandreev/purple-vk-plugin/downloads.
���� ���������� purple-vk-plugin-�����������-bin.tar.gz. ����� �������� ��� i386, ��� � x86-64 ������.
�������� � ������ ������ ������������ ������ �������� ��� ������ � ����� ������ � ��������� ����������.

��������� ���������� �� CentOS 6.5, ��� ��� ������ ���� ���������� � ���������� ������� ��������������
(2009-2010 ����).

������
======

����������:

* cmake >= 2.6
* ����������, �������������� C++11 (����������� gcc 4.6, 4.7, 4.8, clang 3.2, 3.4)
* libpurple >= 2.7
* libxml2 >= 2.7

���������� ������ ��� ������ ������ Ubuntu, �� ������ ����� ��������������� � �� ������ ������������ Linux.

1. ���������� ���������� � CMake. ��� Ubuntu �����������::

     $ apt-get install g++ cmake

2. ���������� development ������ ��� ���������. ��� Ubuntu �����������::

     $ apt-get install libpurple-dev libxml2-dev

3. ��������� � ���������� build::

     $ cd build

4. �������� cmake �� ���������� build::

     $ cmake ..

   ������ cmake ������ ����������� ��� ������ ���������� �����������::

     -- Configuring done
     -- Generating done
     -- Build files have been written to: /home/oleg/projects/purple-vk-plugin/build

5. ���������::

     $ make

   ������ make ������ ����������� ��� ������ � ��������������::

     [100%] Built target purple-vk-plugin

6. ��� ��������� ������� ��� ���� ������������� ��������� �� ��� ������������ root::

     $ make install

7. ��� ������ ��������� ���������� ���� libpurple-vk-plugin.so ���� � ~/.purple/plugins (��������
   ��� ����������, ���� ��� �� ����������) ��� � /usr/lib/purple-2. ���������� ���������� data/protocols
   � /usr/share/pixmaps/pidgin/protocols.

Description
===========

Purple-vk-plugin is a plugin for Pidgin, which supports Vk.com (Vkontakte) messaging.

Currently only Pidgin on Linux is actively supported, however Windows version
is being developed.

Installing
==========

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
binary builds are named purple-vk-plugin-VERSION-bin.tar.gz). The archive contains both i386 and x86-64
builds. Included install script copies plugin .so file and data files into required directories.

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