# Third-party code

| Path | Source | License |
|------|--------|---------|
| `decomp/` | [SomeJakeGuy/lm-decomp](https://github.com/SomeJakeGuy/lm-decomp) @ `c347326b12c38281c5528d36bda6334997fa8d3c` | CC0 1.0 (`decomp/LICENSE`) |
| `libs/JSystem/`, `include/global.h`, `include/helpers/`, `src/port/os/OS{Thread,Mutex,Context}.cpp` | [TwilitRealm/dusklight](https://github.com/TwilitRealm/dusklight) @ `2c4960dcdfaedf9b0c96e2410a1cca2bc1657131` (adapted) | CC0 1.0 |
| `extern/aurora/` | [encounter/aurora](https://github.com/encounter/aurora) (submodule) | MIT |
| `llvm/bin/clang.exe`, `llvm/bin/lld-link.exe` (release packages only, copied at install time; not in this repository) | [LLVM Project](https://llvm.org) | Apache-2.0 WITH LLVM-exception (`packaging/LLVM-LICENSE.TXT`, installed as `llvm/LLVM-LICENSE.TXT`) |
| libdatachannel 0.24.6 (fetched and linked statically at build time; internet play) | [paullouisageneau/libdatachannel](https://github.com/paullouisageneau/libdatachannel) with its libjuice, usrsctp and plog submodules | MPL-2.0 (libdatachannel, libjuice), BSD-3-Clause (usrsctp), MIT (plog) |
| MbedTLS 3.6.7 (fetched and linked statically; DTLS for internet play) | [Mbed-TLS/mbedtls](https://github.com/Mbed-TLS/mbedtls) | Apache-2.0 OR GPL-2.0-or-later (used under Apache-2.0) |
| nlohmann/json 3.11.3 (fetched at build time; matchmaking messages) | [nlohmann/json](https://github.com/nlohmann/json) | MIT |
| `server/` dependency `ws` (matchmaking server only, installed with npm) | [websockets/ws](https://github.com/websockets/ws) | MIT |

lm-decomp does not accept AI-assisted contributions. Code in this repository that was
written with AI assistance must not be submitted upstream to lm-decomp.

No extracted game data or generated code from the retail executable is distributed.
Users must provide their own disc dump. The loading artwork in `assets/join-loading.png`
was supplied by the project maintainer. Character artwork and trademarks belong to
their respective owners and are not covered by the code licenses.
