The external libraries included in this folder have their own licenses.  They are included here only so that the Win32 version of RISE can compile easier.  The source was not modified, it was merely compiled as a Win32 static library.  Included below is the license from both libpng and zlib

For more information about libpng:
http://www.libpng.org

For more information about zlib:
http://www.gzip.org/zlib/

For more information about libtiff:
http://www.libtiff.org

For more information about OpenEXR:
http://www.openexr.com/


====================================================================
oidn/  --  Intel Open Image Denoise (added 2026-04-29)
====================================================================

OIDN is included as a git SUBMODULE pointing at v2.4.1 of
https://github.com/RenderKit/oidn rather than as vendored source.
Reason: at full source + trained weights + built-binary scope, OIDN
is ~120 MB — disproportionate next to libpng / libtiff / openexr in
this directory (~5 MB total).  The submodule reference is just a
commit hash and lets `git submodule update --init` reproduce the
exact upstream tree on every clone.

After cloning RISE:

    git submodule update --init extlib/oidn/source

Then build it with the platform-appropriate script.  The build is
opt-in: RISE's build configs prefer extlib/oidn/install/ when
present and fall back to the system-installed OIDN otherwise (see
docs/OIDN.md OIDN-P0-3 for the install gotcha that motivated this).

macOS / Linux:
    extlib/oidn/build.sh

Windows (PowerShell):
    pwsh -File extlib\oidn\build.ps1

Both scripts produce extlib/oidn/install/ with platform-best
backends compiled in (CPU + Metal on macOS; CPU on Windows by
default with -EnableCuda / -EnableHip / -EnableSycl flags for GPU).

Prerequisites — Windows
~~~~~~~~~~~~~~~~~~~~~~~

OIDN's CMake build needs four host-side tools.  The build script
fails with a specific cmake error when any of them is missing; the
fixes below are what get you past each one.

  - Visual Studio 2022 or 2026 with the C++ desktop workload + CMake
    component.  build.ps1 omits cmake's `-G` so it auto-detects the
    most-recent installed VS — works for VS17 / VS18 / future without
    edits.

  - cmake (bundled with VS at e.g.
    `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\
    IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`; add
    that directory to PATH for this shell).

  - ispc — Intel SPMD Program Compiler.  OIDN uses it for SIMD-
    parallel code generation on the CPU device.  Try:

        winget install Intel.ImplicitSPMDProgramCompiler

    If winget can't find the package on your machine (the source
    sometimes drops it depending on region / Microsoft Store state),
    grab the binary release zip from
    https://github.com/ispc/ispc/releases — extract anywhere and add
    its `bin/` to PATH.  Verified working: ispc v1.30.0.

  - Python 3.x — used by OIDN's CMake at configure time for code
    generation; ZERO runtime dependency on Python.

        winget install Python.Python.3.13

    Then add `%LOCALAPPDATA%\Programs\Python\Python313` (or wherever
    winget put it) to PATH.

  - oneTBB — required by the OIDN CPU device's `find_package(TBB)`.
    Two ways to satisfy this:
      a) Reuse the TBB that already ships with your openpgl install.
         If you built openpgl from source into e.g.
         `C:\Dev\openpgl-install`, that dir is a valid `TBB_ROOT`
         (flat layout: `bin/tbb12.dll`, `lib/tbb12.lib`).  PREFER
         THIS — it guarantees OIDN and openpgl link the same TBB
         binary, avoiding the heap-corruption crashes that result
         from loading two different `tbb12.dll` versions into the
         same RISE process (observed 2026-04-29: openpgl built
         against TBB 2021.13 + OIDN built against oneTBB 2022.3.0
         crashed deep in Qt's allocator).
      b) Fresh oneTBB binary release zip from
         https://github.com/uxlfoundation/oneTBB/releases (e.g.
         `oneapi-tbb-2022.3.0-win.zip`) — extract anywhere and set
         `TBB_ROOT` to the unzipped root (redist layout:
         `redist/intel64/vc14/tbb12.dll`).  Use this only if you
         don't have an openpgl-install handy, AND verify openpgl
         uses the same major version.  Verified working alone:
         oneTBB v2022.3.0.
    `extlib\oidn\build.ps1` probes both layouts (redist + flat)
    and uses whichever it finds.

  - GPU backends (opt-in, only if you pass `-EnableCuda` / -EnableHip
    / -EnableSycl): CUDA Toolkit 11+ on PATH; oneAPI Base Toolkit
    for SYCL; ROCm SDK for HIP.

One-liner that runs the build with all four prereqs wired in:

    pwsh -ExecutionPolicy Bypass -Command "
      `$env:PATH='C:\Users\<you>\AppData\Local\Programs\Python\Python313;' +
        'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;' +
        'C:\Dev\ispc\ispc-v1.30.0-windows\bin;' + `$env:PATH;
      `$env:TBB_ROOT='C:\Dev\tbb\oneapi-tbb-2022.3.0';
      cd C:\Dev\GitHub\RISE;
      & extlib\oidn\build.ps1"

The script's output ends with `==> Done. OIDN install tree: ...` on
success.  Build time is ~10 min on a modern desktop.

Prerequisites — macOS / Linux
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  - Xcode Command Line Tools (macOS) or gcc/clang + cmake (Linux).
  - ispc on PATH (`brew install ispc` on macOS;
    https://github.com/ispc/ispc/releases on Linux).
  - Python 3 on PATH.
  - TBB headers + libs.  On macOS: `brew install tbb` and the script
    finds it through Homebrew's pkg-config.  On Linux: distro
    package or set `TBB_ROOT`.
  - For the macOS Metal device backend: a working Xcode Metal
    toolchain.  build.sh runs `xcodebuild -downloadComponent
    MetalToolchain` automatically — no manual step needed.

See the script headers for the per-platform invocations and
optional flags.

For more information about Open Image Denoise:
https://www.openimagedenoise.org/


OPEN IMAGE DENOISE LICENSE (Apache 2.0):

Licensed under the Apache License, Version 2.0 (the "License"); you
may not use this file except in compliance with the License.  You
may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
implied.  See the License for the specific language governing
permissions and limitations under the License.

Full third-party license details ship with each OIDN release at
extlib/oidn/install/share/doc/OpenImageDenoise/ once build.sh /
build.ps1 has been run.


LIBPNG LICENSE


This copy of the libpng notices is provided for your convenience.  In case of
any discrepancy between this copy and the notices in the file png.h that is
included in the libpng distribution, the latter shall prevail.

COPYRIGHT NOTICE, DISCLAIMER, and LICENSE:

If you modify libpng you may insert additional notices immediately following
this sentence.

libpng versions 1.0.7, July 1, 2000, through 1.2.1, December 12, 2001, are
Copyright (c) 2000 Glenn Randers-Pehrson
and are distributed according to the same disclaimer and license as libpng-1.0.6
with the following individuals added to the list of Contributing Authors

   Simon-Pierre Cadieux
   Eric S. Raymond
   Gilles Vollant

and with the following additions to the disclaimer:

   There is no warranty against interference with your enjoyment of the
   library or against infringement.  There is no warranty that our
   efforts or the library will fulfill any of your particular purposes
   or needs.  This library is provided with all faults, and the entire
   risk of satisfactory quality, performance, accuracy, and effort is with
   the user.

libpng versions 0.97, January 1998, through 1.0.6, March 20, 2000, are
Copyright (c) 1998, 1999 Glenn Randers-Pehrson, and are
distributed according to the same disclaimer and license as libpng-0.96,
with the following individuals added to the list of Contributing Authors:

   Tom Lane
   Glenn Randers-Pehrson
   Willem van Schaik

libpng versions 0.89, June 1996, through 0.96, May 1997, are
Copyright (c) 1996, 1997 Andreas Dilger
Distributed according to the same disclaimer and license as libpng-0.88,
with the following individuals added to the list of Contributing Authors:

   John Bowler
   Kevin Bracey
   Sam Bushell
   Magnus Holmgren
   Greg Roelofs
   Tom Tanner

libpng versions 0.5, May 1995, through 0.88, January 1996, are
Copyright (c) 1995, 1996 Guy Eric Schalnat, Group 42, Inc.

For the purposes of this copyright and license, "Contributing Authors"
is defined as the following set of individuals:

   Andreas Dilger
   Dave Martindale
   Guy Eric Schalnat
   Paul Schmidt
   Tim Wegner

The PNG Reference Library is supplied "AS IS".  The Contributing Authors
and Group 42, Inc. disclaim all warranties, expressed or implied,
including, without limitation, the warranties of merchantability and of
fitness for any purpose.  The Contributing Authors and Group 42, Inc.
assume no liability for direct, indirect, incidental, special, exemplary,
or consequential damages, which may result from the use of the PNG
Reference Library, even if advised of the possibility of such damage.

Permission is hereby granted to use, copy, modify, and distribute this
source code, or portions hereof, for any purpose, without fee, subject
to the following restrictions:

1. The origin of this source code must not be misrepresented.

2. Altered versions must be plainly marked as such and must not
   be misrepresented as being the original source.

3. This Copyright notice may not be removed or altered from any
   source or altered source distribution.

The Contributing Authors and Group 42, Inc. specifically permit, without
fee, and encourage the use of this source code as a component to
supporting the PNG file format in commercial products.  If you use this
source code in a product, acknowledgment is not required but would be
appreciated.


A "png_get_copyright" function is available, for convenient use in "about"
boxes and the like:

   printf("%s",png_get_copyright(NULL));

Also, the PNG logo (in PNG format, of course) is supplied in the
files "pngbar.png" and "pngbar.jpg (88x31) and "pngnow.png" (98x31).

Libpng is OSI Certified Open Source Software.  OSI Certified Open Source is a
certification mark of the Open Source Initiative.

Glenn Randers-Pehrson
randeg@alum.rpi.edu
December 12, 2001




ZLIB LICENSE

zlib 1.1.3 is a general purpose data compression library.  All the code
is thread safe.  The data format used by the zlib library
is described by RFCs (Request for Comments) 1950 to 1952 in the files 
ftp://ds.internic.net/rfc/rfc1950.txt (zlib format), rfc1951.txt (deflate
format) and rfc1952.txt (gzip format). These documents are also available in
other formats from ftp://ftp.uu.net/graphics/png/documents/zlib/zdoc-index.html

All functions of the compression library are documented in the file zlib.h
(volunteer to write man pages welcome, contact jloup@gzip.org). A usage
example of the library is given in the file example.c which also tests that
the library is working correctly. Another example is given in the file
minigzip.c. The compression library itself is composed of all source files
except example.c and minigzip.c.

To compile all files and run the test program, follow the instructions
given at the top of Makefile. In short "make test; make install"
should work for most machines. For Unix: "configure; make test; make install"
For MSDOS, use one of the special makefiles such as Makefile.msc.
For VMS, use Make_vms.com or descrip.mms.

Questions about zlib should be sent to <zlib@quest.jpl.nasa.gov>, or to
Gilles Vollant <info@winimage.com> for the Windows DLL version.
The zlib home page is http://www.cdrom.com/pub/infozip/zlib/
The official zlib ftp site is ftp://ftp.cdrom.com/pub/infozip/zlib/
Before reporting a problem, please check those sites to verify that
you have the latest version of zlib; otherwise get the latest version and
check whether the problem still exists or not.

Mark Nelson <markn@tiny.com> wrote an article about zlib for the Jan. 1997
issue of  Dr. Dobb's Journal; a copy of the article is available in
http://web2.airmail.net/markn/articles/zlibtool/zlibtool.htm

The changes made in version 1.1.3 are documented in the file ChangeLog.
The main changes since 1.1.2 are:

- fix "an inflate input buffer bug that shows up on rare but persistent
  occasions" (Mark)
- fix gzread and gztell for concatenated .gz files (Didier Le Botlan)
- fix gzseek(..., SEEK_SET) in write mode
- fix crc check after a gzeek (Frank Faubert)
- fix miniunzip when the last entry in a zip file is itself a zip file
  (J Lillge)
- add contrib/asm586 and contrib/asm686 (Brian Raiter)
  See http://www.muppetlabs.com/~breadbox/software/assembly.html
- add support for Delphi 3 in contrib/delphi (Bob Dellaca)
- add support for C++Builder 3 and Delphi 3 in contrib/delphi2 (Davide Moretti)
- do not exit prematurely in untgz if 0 at start of block (Magnus Holmgren)
- use macro EXTERN instead of extern to support DLL for BeOS (Sander Stoks)
- added a FAQ file

plus many changes for portability.

Unsupported third party contributions are provided in directory "contrib".

A Java implementation of zlib is available in the Java Development Kit 1.1
http://www.javasoft.com/products/JDK/1.1/docs/api/Package-java.util.zip.html
See the zlib home page http://www.cdrom.com/pub/infozip/zlib/ for details.

A Perl interface to zlib written by Paul Marquess <pmarquess@bfsec.bt.co.uk>
is in the CPAN (Comprehensive Perl Archive Network) sites, such as:
ftp://ftp.cis.ufl.edu/pub/perl/CPAN/modules/by-module/Compress/Compress-Zlib*

A Python interface to zlib written by A.M. Kuchling <amk@magnet.com>
is available in Python 1.5 and later versions, see
http://www.python.org/doc/lib/module-zlib.html

A zlib binding for TCL written by Andreas Kupries <a.kupries@westend.com>
is availlable at http://www.westend.com/~kupries/doc/trf/man/man.html

An experimental package to read and write files in .zip format,
written on top of zlib by Gilles Vollant <info@winimage.com>, is
available at http://www.winimage.com/zLibDll/unzip.html
and also in the contrib/minizip directory of zlib.


Notes for some targets:

- To build a Windows DLL version, include in a DLL project zlib.def, zlib.rc
  and all .c files except example.c and minigzip.c; compile with -DZLIB_DLL
  The zlib DLL support was initially done by Alessandro Iacopetti and is
  now maintained by Gilles Vollant <info@winimage.com>. Check the zlib DLL
  home page at http://www.winimage.com/zLibDll

  From Visual Basic, you can call the DLL functions which do not take
  a structure as argument: compress, uncompress and all gz* functions.
  See contrib/visual-basic.txt for more information, or get
  http://www.tcfb.com/dowseware/cmp-z-it.zip

- For 64-bit Irix, deflate.c must be compiled without any optimization.
  With -O, one libpng test fails. The test works in 32 bit mode (with
  the -n32 compiler flag). The compiler bug has been reported to SGI.

- zlib doesn't work with gcc 2.6.3 on a DEC 3000/300LX under OSF/1 2.1   
  it works when compiled with cc.

- on Digital Unix 4.0D (formely OSF/1) on AlphaServer, the cc option -std1
  is necessary to get gzprintf working correctly. This is done by configure.

- zlib doesn't work on HP-UX 9.05 with some versions of /bin/cc. It works
  with other compilers. Use "make test" to check your compiler.

- gzdopen is not supported on RISCOS, BEOS and by some Mac compilers.

- For Turbo C the small model is supported only with reduced performance to
  avoid any far allocation; it was tested with -DMAX_WBITS=11 -DMAX_MEM_LEVEL=3

- For PalmOs, see http://www.cs.uit.no/~perm/PASTA/pilot/software.html
  Per Harald Myrvang <perm@stud.cs.uit.no>


Acknowledgments:

  The deflate format used by zlib was defined by Phil Katz. The deflate
  and zlib specifications were written by L. Peter Deutsch. Thanks to all the
  people who reported problems and suggested various improvements in zlib;
  they are too numerous to cite here.

Copyright notice:

 (C) 1995-1998 Jean-loup Gailly and Mark Adler

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Jean-loup Gailly        Mark Adler
  jloup@gzip.org          madler@alumni.caltech.edu

If you use the zlib library in a product, we would appreciate *not*
receiving lengthy legal documents to sign. The sources are provided
for free but without warranty of any kind.  The library has been
entirely written by Jean-loup Gailly and Mark Adler; it does not
include third-party code.

If you redistribute modified sources, we would appreciate that you include
in the file ChangeLog history information documenting your changes.



LIBTIFF LICENSE:



Use and Copyright
-----------------
Silicon Graphics has seen fit to allow us to give this work away.  It
is free.  There is no support or guarantee of any sort as to its
operations, correctness, or whatever.  If you do anything useful with
all or parts of it you need to honor the copyright notices.   I would
also be interested in knowing about it and, hopefully, be acknowledged.

The legal way of saying that is:

Copyright (c) 1988-1997 Sam Leffler
Copyright (c) 1991-1997 Silicon Graphics, Inc.

Permission to use, copy, modify, distribute, and sell this software and 
its documentation for any purpose is hereby granted without fee, provided
that (i) the above copyright notices and this permission notice appear in
all copies of the software and related documentation, and (ii) the names of
Sam Leffler and Silicon Graphics may not be used in any advertising or
publicity relating to the software without the specific, prior written
permission of Sam Leffler and Silicon Graphics.

THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  

IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
OF THIS SOFTWARE.



OpenEXR License:

Copyright (c) 2004, Industrial Light & Magic, a division of Lucasfilm
Entertainment Company Ltd.  Portions contributed and copyright held by
others as indicated.  All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

    * Redistributions of source code must retain the above
      copyright notice, this list of conditions and the following
      disclaimer.

    * Redistributions in binary form must reproduce the above
      copyright notice, this list of conditions and the following
      disclaimer in the documentation and/or other materials provided with
      the distribution.

    * Neither the name of Industrial Light & Magic nor the names of
      any other contributors to this software may be used to endorse or
      promote products derived from this software without specific prior
      written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


