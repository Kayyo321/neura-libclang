# Notice

The files in `include/clang-c/` are the libclang C API headers from the
LLVM Project, copied without modification from:

- Repository: <https://github.com/llvm/llvm-project>
- Tag: `llvmorg-19.1.7` (LLVM 19.1.7)
- Commit: `cd708029e0b2869e80abe31ddb175f7c35361f90`
- Directory: `clang/include/clang-c/`

`LICENSE.TXT` is `llvm/LICENSE.TXT` from the same commit. The LLVM Project
is under the Apache License v2.0 with LLVM Exceptions; the full text and the
notices for other licenses LLVM contains are in `LICENSE.TXT`.

The loader in `src/`, the build files, the scripts, and the tests in this
repository are original to neura-libclang and are under the same license.

`scripts/update-headers.sh` writes this file.
