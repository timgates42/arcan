Introduction
============
Aloadimage is a simple command-line imageviewer for arcan, built on the image
parsers provided by stb (https://github.com/nothings/stb). Image loading is
performed in the background as separate, sandboxed processes and should be safe
against maliciously crafted input sources.

The primary purpose is to provide an arcan- specific replacement for
xloadimage, and to serve as a testing ground for advanced image output such as
full HDR- paths. The secondary purpose is to create the image- loading worker
pool to be secure and efficient enough to act as a building block for other
components in the arcan umbrella.

For more detailed instructions, see the manpage.

Building/use
============
the build needs access to the arcan-shmif library matching the arcan instance
that it should connect to. It can either be detected through the normal
pkgconfig, or by explicitly pointing cmake to the
-DARCAN\_SOURCE\_DIR=/absolute/path/to/arcan/src

The appl- that arcan is running will also need to expose an appropriate
connection point (where you point the ARCAN\_CONNPATH environment variable)

         mkdir build
         cd build
         cmake ../
         make
         ARCAN_CONNPATH=something ./aloadimage file1.png file2.jpg file3.png

Status
======
 - [x] Basic controls
 - [x] Multiprocess/sandboxed parsing
   - [x] Expiration timer
	 - [x] Upper memory consumption cap (no gzip bombing)
	 - [x] Seccmp- style syscall filtering
 - [x] Playlist
   - [x] Read/load-ahead
 - [ ] Drag/zoom/pan controls
 - [ ] sRGB <-> Linear controls
 - [ ] Full-chain FP16 format support
 - [ ] GPU acceleration toggle
 - [ ] Per image transformations (rotate, flip, skew, ...)
 - [ ] Internationalization