# Security policy

## Reporting a vulnerability

Please report privately through GitHub's **Security → Report a vulnerability**
form on this repository, which opens a private advisory visible only to the
maintainer. Do not open a public issue for a memory-safety or archive-parsing
bug.

Include the `gleipnir --version` output, the platform and compiler, and — most
usefully — the archive or input that triggers it. A file under a megabyte that
reproduces the fault is worth more than a description of it.

Expect an acknowledgement within a week. There is no bounty; this is a
single-maintainer project.

## What is in scope

The decompressor treats every archive as untrusted input, so anything that
breaks that assumption is in scope:

- Memory unsafety while parsing a header, index, trailer, segment or recovery
  block — out-of-bounds read or write, overflow in a length or offset, wild
  pointer, use-after-free.
- A crafted archive causing a crash, a hang, or unbounded allocation.
- **Path traversal on extract.** Member names that are absolute, carry a drive
  letter, contain a `..` component, or hold control characters are refused, and
  extraction must not write outside the destination directory.
- **Silent wrong output** — any input for which `gleipnir` exits 0 and produces
  bytes that are not what was compressed. Every field the reader parses is
  bounds-checked, and a damaged archive is required to produce a diagnostic
  and exit 2, never a crash, a hang, or plausible-looking wrong bytes.

`scripts/gfuzz.py` exercises exactly this threat model — bit flips, decayed sectors,
truncation, splices, zero holes, trailing junk and pure noise — and asserts
that every outcome is either byte-exact output or a non-zero exit. A case that
defeats it is a good report.

## What is out of scope

These are documented properties, not defects:

- **There is no encryption**, and none is planned. `gleipnir` provides integrity,
  not confidentiality. Encrypt the archive afterwards if the data needs it,
  and note that doing so removes the ability to scrub or repair it, since
  neither the checksums nor the parity blocks are readable through ciphertext.
- **Compression is not constant-time** and its timing and output size depend on
  the input. Do not compress attacker-controlled data together with secrets and
  expect the size not to leak — the CRIME/BREACH class of attack applies here as
  it does to every general-purpose compressor.
- **Memory use is large and preset-dependent**, and scales with `-t` because
  each worker owns a complete model. `-9 -t8` needs about 6.5 GB. An
  out-of-memory condition from settings chosen by the operator is a capacity
  question, covered in [USAGE.md](USAGE.md#memory-and-threads).
- **The whole input is buffered in RAM.** A large input needs roughly its own
  size plus tables plus an output buffer, which is a documented limit rather
  than a resource-exhaustion bug.
- Anything requiring the attacker to already have write access to the machine
  running `gleipnir`, or to the archive being read, without a crafted-input step.

## Supported versions

The latest release is supported. Fixes land on `main` and go out in the next
release; there is no backport branch.
