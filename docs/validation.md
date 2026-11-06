# Validation and candidate identity

The physical v10 run confirmed Mali/Panfrost execution, native wallpaper appearance (user observation), persistent data activity and continued TD4150 reporting. It recorded 4,605 touch reports, 65 padding rejections, zero reported SPI bus failures, no touch reinitialization, and no kernel panic found in the recovered run.

The v11 touch candidate was built, installed in the guarded payload reservation and read back. The boot image stayed unchanged. Its restart gate was armed after the payload and boot hashes were rechecked.

| Component | SHA256 |
|---|---|
| v11 payload | `0f0cbed4034323b82daf247a8c330fbd7dd12ec336af8f2896480d9fc63b1d6b` |
| v11 kernel | `e8e259bcbd294813ad5f6bdbb0b163327a875103d2b0602bca4d9951fabc0e94` |
| v10/v11 ramdisk | `640bdf3eeeb4ccd9e342c5eaf51e5382f551751b1c0c5ebe5ba05ef4d3cbe47c` |
| Unchanged test boot image | `874ddd97d2428156c44489035f57587ff536caf7232ad03215aa94acd920a633` |

These identify local artifacts; compiled artifacts are not distributed in this repository.

Seventeen touch/display/CoreAnimation suites passed on the working source. The reader tests exercise the actual parser; queue tests exercise the real producer; wallpaper tests execute production callbacks rather than a duplicate of their logic. Eight physical malformed packets were also reconstructed and decoded to expected native coordinates with address/undefined-behavior sanitizers in a private fixture test.

The XNU kernel compilation and link succeeded. The project's build wrapper accepts only the documented unsupported kext symbol-set checks; it does not ignore arbitrary compiler/link failures. Public-source export adds a separate no-vendor-firmware guard to the unrelated FocalTech path; this is not represented as part of the already-installed v11 binary.

Host tests and a compiled candidate do not establish that physical touch is fully refined. The physical v11 comparison is pending. Metal, WLAN and Bluetooth are not enabled by this candidate.

The English publication records its own verification separately in `publication-validation.json` after running `tools/verify.py`. Historical files in `device/HANDOFF.md`, `device/docs/` and upstream kernel docs may describe earlier candidates; the current summaries in this `docs/` directory take precedence.
