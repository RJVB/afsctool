# This is my version of brkirch's afsctool

> AFSC (Apple File System Compression) tool is a utility that can be used
to apply HFS+ compression to file(s), decompress HFS+ compressed file(s), or
get information about existing HFS+ compressed file(s).
Mac OS 10.6 or later is required. See: https://brkirch.wordpress.com/afsctool/

I have made several modifications, mostly concerning the compression feature:
- improved error reporting
- an attempt to reduce memory pressure compressing large files
- support for multiple files/folders specified on the commandline
- a backup option while compressing (that comes in addition to the existing undo if
  something went wrong)
- support for files that are read-only (and/or write-only) by changing their permissions
  temporarily. No error checking is done for this feature; failure will lead to
  errors that are already caught.


The main new feature that justifies the version bump, however, is the parallel
processing feature, allowing the user to specify an arbitray (though positive :))
number of threads that will compress the specified files in parallel.
This feature has two modes that each perform best in specific conditions:
- **perform only the actual compression in parallel.** Disk IO (reading the file into
  memory, writing it back to disk) is done with exclusive access to prevent disk
  trashing. This mode is selected with the -j option.
- **perform all file compression steps in parallel.** This mode is suitable for solid-
  state disks, file collections that reside on different disks or collections with
  files of different sizes (including notably very large files). This mode is
  selected with the -J option.
The performance difference is never enormous in my testing, but YMMV.

Interestingly, the optimum performance (on large collections) is not necessarily
obtained with as many worker threads as there are CPU cores. Collections with
a significant number of very large files and many more smaller files can apparently
be processed faster when using a rather larger number of workers, with a gain
approaching the number of cores.

Version 1.6.9 introduces two new options for the parallel compression mode:
- **size-sorting the item list (-S option).** The parallel workers each take the next
  uncompressed file off the item list, which normally is in the order in which
  files were found during the scan for compressable files in the user arguments.
  Sorting this list means that the smallest files will be compressed first, which
  may be of interest when the target disk is very (almost) full and the list also
  contains large files (which require more space for the temporary backup).
- **reverse workers (-RM option) when using a sorted list.** This option configures <M>
  out of the <N> worker threads to take items off the back of the list. The main
  purpose of the option is to combine `-Ji -S -Ri`, i.e. compress the largest files
  first. This may be beneficial to help limit (free space) fragmentation (if that's
  the goal, using a single worker thread is probably best).

### Installation

afsctool depends on zlib (v1.2.8 or newer) and Google's sparsehash library and on CMake
for building. The OS zlib copy may be recent enough (it is on 10.12 and later) but to be
certain to obtain the latest versions of both, use a package manager like MacPorts, Fink
or HomeBrew. Be sure to follow the general installation and usage instructions for those
projects, in particular how to update your PATH.

# using MacPorts:
```shell
port install sparsehash zlib cmake
```

# using HomeBrew:
```shell
brew install google-sparsehash zlib cmake
PKG_CONFIG_PATH=/usr/local/opt/zlib/lib/pkgconfig
```
(Setting PKG_CONFIG_PATH is only required with HomeBrew.)

## Compile
With the dependencies installed you can now build afsctool. In a directory of your choice:
```shell
git clone git://github.com/RJVB/afsctool
mkdir afsctool/build
cd afsctool/build
cmake -Wno-dev ..
make
```

This will leave the afsctool executable in `afsctool/build`; you can move it anywhere
you like from there. You can also do an "official" install, to /usr/local/bin by
default:
```shell
cd afsctool/build
sudo make install/fast V=1 VERBOSE=1
```
