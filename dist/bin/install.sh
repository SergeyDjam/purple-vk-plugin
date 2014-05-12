#!/bin/bash

if [ $EUID != 0 ]; then
  echo "Please run this script as root, e.g. sudo $0"
  exit 1
fi

if [ `uname -m` == x86_64 ]; then
  if [ -d /usr/lib64/purple-2 ]; then
    echo "Copying plugin to /usr/lib64/purple-2"
    cp bin/x86_64/*.so /usr/lib64/purple-2
  else
    if [ -d /usr/lib/purple-2 ]; then
      echo "Copying plugin to /usr/lib/purple-2"
      cp bin/x86_64/*.so /usr/lib/purple-2
    else
      echo "ERROR: Either pidgin is not installed or is installed in unknown location"
    fi
  fi
else
  if [ ! -d /usr/lib/purple-2 ]; then
    echo "ERROR: Either pidgin is not installed or is installed in unknown location"
    exit 1
  fi
  echo "Copying plugin to /usr/lib/purple-2"
  cp bin/i386/*.so /usr/lib/purple-2
fi

cp -r data/protocols /usr/share/pixmaps/pidgin
cp -r data/smileys/vk /usr/share/pixmaps/pidgin/emotes
