Description
===========

cpputils are a bunch of disjointed small libraries for C++. Currently included are:

* contutils, a number of STL container-related functions (copying, removing items from
  containers etc.)
* strutils, a number of string-related functions (splitting, replacing, formatting etc.)
* trie, a simple trie implementation

cpputils does not provide build system for the library itself, you should manually include
it in your project. No configuration is required, just place add include/ and src/ directories.

Most of the libraries require a C++11 compiler and were tested on GCC 4.8 and Clang 3.5

Testing
=======

CMake is used for building and running tests. Create a subdirectory, named build:
::
  $ mkdir build

Run cmake and make from this subdirectory:
::
  $ cd build
  $ cmake ..
  $ make

A bunch of executables named test_* will be created in build. Each of them may be run
individually or you can use CTest runner:
::
  $ ctest -V

Tests are implemented with lightweight and nice CppUT framework (https://github.com/murrekatt/cpput)

License
=======

cpputils license is 2-Clause BSD license. Details can be found in LICENSE.

Unit testing framework is licensed separately (see tests/contrib/cpput/LICENSE).
