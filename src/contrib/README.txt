This file lists the contents of contrib/ directory.

cpputils
========

A small library of useful C++ utilities, separately maintained in
https://bitbucket.org/olegoandreev/cpputils.

purple
======

These following files have been taken from pidgin tip (future 3.0 release branch) and placed
in this repository:
 * http.c
 * http.h

These files provide a simple to use HTTP library which is perfectly compatible with libpurple event
loop. This library will be present in future libpurple releases (starting with 3.0).

The only modifications to the files concern fixing the build, making it clean (silencing
the warnings) and removing handling "expires" in Set-cookie header as it gets wrongly (?)
parsed and. therefore, makes cookies rejected.

A number of functions purple_util_fetch_url_* currently present in libpurple 2.x do not provide
the required capabilities (modifying request headers, storing and setting cookies, changing the
number of redirects). Adding an external library (libcurl, libevent) dependancy seemed
a bit too much.

picojson.h
==========

The only file in the PicoJSON library (https://github.com/kazuho/picojson/).

PicoJSON was chosen, because it is the smallest C++ JSON library with nice interface. Unfortunately,
there is no "golden standard" among C++ JSON libraries like libxml2 for XML libraries.
