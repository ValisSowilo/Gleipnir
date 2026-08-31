Gleipnir 1.0.0 -- a context-mixing archiver
======================================

WHAT THIS IS

Gleipnir compresses smaller than mainstream archivers and is much slower than all
of them. On the Silesia corpus it produces a 35.6 MB archive where zpaq -m5
produces 39.1 MB, xz -9e produces 48.5 MB and gzip -9 produces 67.6 MB.

It is built for data you intend to keep and rarely read: archives you will
store and scrub periodically and restore once or never.


QUICK START

  gleipnir c archive.gl mydir            compress a directory (default preset -5)
  gleipnir t archive.gl                  check the archive is intact
  gleipnir l archive.gl                  list what is inside
  gleipnir x archive.gl outdir           extract  (d and e do the same)


RIGHT-CLICK MENU

If you ticked the Explorer option during install, File Explorer gains:

  right-click a folder or file   ->  Compress with Gleipnir
  right-click a .gl archive     ->  Extract here
                                     Verify (fast check)
                                     List contents

On Windows 11 these live under "Show more options" (or press Shift+F10) and
not on the short menu that opens first.

"Extract here" unpacks C:\path\data.gl into C:\path\data\, and the console
window stays open when it finishes so you can read the result. These runs take
minutes to hours, and a window that vanished would hide any error.

For cold storage, the recommended line is:

  gleipnir c -5 -t0 -p32 archive.gl /data
  gleipnir t archive.gl                  check the archive, then delete the
                                     originals yourself once it passes

  -t0   use all CPU cores
  -p32  add recovery records (~3% larger; any one damaged segment per
        group of 32 can be rebuilt with `gleipnir r`)


WHAT GLEIPNIR TOUCHES

Gleipnir never deletes or modifies anything you point it at. Input files are opened
read-only and there is no delete call anywhere in the program. After
compressing, you have two copies of your data, the originals and the archive,
and freeing the space is a manual step you take after `gleipnir t` passes.

The one thing Gleipnir will overwrite without asking is its own output. If the
archive name you give to `c` already exists, it is replaced, and if a file in
the extraction directory has the same name as an archive member, `d` overwrites
it. So the name to be careful about is the destination and not the source.


THE ONE THING TO UNDERSTAND BEFORE YOU USE THIS

Decompression is as slow as compression. This is inherent to how the engine
works and not a missing optimisation. The decoder runs the same model the
encoder ran, one bit at a time.

  preset   compress    decompress   memory     Silesia output
  -f1      1.84 MB/s   1.84 MB/s     204 MB    44.3 MB
  -1       1.52 MB/s   1.53 MB/s     196 MB    43.2 MB
  -3       0.85 MB/s   0.86 MB/s     305 MB    39.5 MB
  -5       0.63 MB/s   0.62 MB/s     483 MB    38.3 MB
  -7       0.47 MB/s   0.45 MB/s     922 MB    36.5 MB
  -9       0.36 MB/s   0.32 MB/s    1055 MB    35.6 MB

At -5 that is roughly 18 CPU-days per terabyte in each direction, single
threaded, or about a day with 16 cores. If you need fast restores, use zstd
or xz instead. They decompress hundreds of times faster and this tool will
disappoint you.

Memory figures above are for compression. Decompression peaks 37-40 MB higher,
so size a restore machine off the larger number.


VERIFYING WITHOUT DECOMPRESSING

  gleipnir t archive.gl        checks stored checksums, runs at disk speed
  gleipnir t -D archive.gl     decodes everything and checks SHA-256 per member

The first is about 1300x faster than the second and is what makes routine
scrubbing affordable. Checking a 2.2 GB archive takes seconds rather than most
of an hour. Use `gleipnir t` on a schedule to catch storage decay, and `gleipnir t -D`
before you rely on a restore.


"WINDOWS PROTECTED YOUR PC" / "BLOCKED BY APPLICATION CONTROL"

You will probably see one of these the first time you run the installer:

  * SmartScreen:  "Windows protected your PC. Unknown publisher."
                  Click "More info", then "Run anyway".

  * Smart App Control:  "An Application Control policy has blocked this
                        file." This one has no override button.

Both happen because Gleipnir is not signed by a certificate authority Windows has
heard of. Windows is reporting a missing certificate and nothing at all about
the program. The binary is built from the source in this repository and you can
rebuild it yourself and compare (see BUILDING below).

Smart App Control is the stricter of the two and is ON by default on new
Windows 11 machines. If it blocks Gleipnir and you want to run it anyway, your
options are:

  1. Build it yourself from source. The result is your own binary.
  2. Run it inside WSL, where none of this applies.
  3. Turn Smart App Control off, but understand that this is a ONE-WAY DOOR.
     Once disabled it can't be re-enabled without reinstalling Windows.
     Do not do this casually, and certainly not just to run an archiver.


REQUIREMENTS

  Windows 10 or later, 64-bit.
  Built for x86-64-v2 (SSE4.2), so any CPU from roughly 2009 onward.
  No DLLs to install: the only imports are KERNEL32 and the Windows UCRT.

  Linux: supported, built from source. See BUILDING.


BUILDING FROM SOURCE

Linux:

  apt install build-essential zlib1g-dev     # or dnf/pacman equivalent
  make
  make test                                  # round-trips a couple of files
  make install                               # into ~/.local/bin

`make install` puts the binary in ~/.local/bin, which is not on the PATH of
every system. Ubuntu adds it for normal user accounts at the next login and
never adds it for root, so on a root shell (which is what a fresh WSL Ubuntu
gives you) the binary lands where the shell won't look for it. Either add
~/.local/bin to your PATH, or install somewhere already on it:

  make install PREFIX=/usr/local

Windows (MSYS2 / MinGW-w64):

  sh build.sh

Both use -march=x86-64-v2 rather than -march=native, so the result runs on any
x86-64 machine from about 2009 onward. Add ARCH=-march=native if you only care
about the machine you built on.

Archives are portable between platforms. Verified in both directions: an
archive written on Ubuntu 26.04 extracts on Windows with every member's
SHA-256 intact, and one written on Windows extracts on Linux the same way.
Per-member compressed sizes are identical on both, so the model behaves the
same everywhere.

Two archives of the same directory are NOT byte-identical across platforms,
though. Directory enumeration returns members in a different order on each,
and each member is stored as its own segment, so the same content is arranged
differently in the file. Stored timestamps and permission bits differ too.
None of that affects what comes back out. It only means you can't compare
archives by hash across platforms. Compare the extracted files instead.

Memory is the real requirement. Each worker thread holds a complete model, so
`-t8 -9` wants about 8 GB. Gleipnir clamps the thread count to what the machine can
actually hold, but choosing a lower preset is usually better than being
clamped.


LIMITS WORTH KNOWING

  * No encryption. Encrypting the archive afterwards destroys the ability to
    scrub or repair it, since neither the checksums nor the parity blocks are
    readable through ciphertext.
  * No append. Adding to an archive means rewriting it.
  * Archives are format v2 and are not readable by earlier builds.
  * Verified up to 2.2 GB per file. Nothing larger has been tried.
  * Recovery covers one lost segment per group and not two.
  * Linux is verified on Ubuntu 26.04 (gcc 15.2), built from source via the
    Makefile. Other distributions should work but haven't been tried.


DOCUMENTATION

  USAGE.txt         full option list, preset table, cold-storage recipes,
                    scrubbing, recovery records, known limits

Run `gleipnir --help` for a short option list, or `gleipnir --version` to check which
build you have.

Two further documents ship with the source rather than the installer, since
they are about how Gleipnir was built rather than how to use it:

  ARCHITECTURE.md   how the compressor and archive format work, and the
                    measurements behind each decision
  EXPERIENCES.md    what building it taught, mistakes included


CREDITS AND LICENCE

  Gleipnir is written and maintained by ValisSowilo.
  https://github.com/ValisSowilo

  Copyright (C) 2026 ValisSowilo
  Licensed under the GNU General Public License, version 3 or later.

  Gleipnir is free software. You may run it for any purpose, including
  commercially and in production, at any scale, without paying anyone.
  You may study it, modify it, and redistribute it, modified or not, and
  charge for doing so. There are no user or revenue thresholds and no
  expiry date.

  The one obligation applies only if you distribute Gleipnir or something
  built from it: whoever receives it gets the same freedoms, which means
  offering them the corresponding source under the GPL and keeping the
  notices intact. Running Gleipnir inside your own organisation is not
  distribution.

  GPL-3.0 is OSI-approved, so Gleipnir is open source in the ordinary sense of
  the term.

  Gleipnir 1.0.0 was first published under the Business Source License 1.1,
  which would have converted to the GPL on 2030-08-04. That conversion
  was brought forward; the BSL terms no longer apply to any version.

  Full terms in LICENSE.md. LICENSE.txt has a plain-language summary and
  the zlib notice.

  The engine follows the context-mixing lineage of Matt Mahoney's PAQ and
  ZPAQ and the lpaq family. Bit-history state machines, logistic mixing in
  the stretched domain, SSE/APM refinement and indirect secondary symbol
  estimation all come from that work. No PAQ code is used here, but the
  ideas are not original to Gleipnir and it would be dishonest to imply so.

  DEFLATE stream recovery uses zlib, by Jean-loup Gailly and Mark Adler.
  Benchmarks use the Silesia corpus, assembled by Sebastian Deorowicz.
