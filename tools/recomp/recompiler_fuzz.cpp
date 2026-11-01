// Synthetic or privately supplied DOL inputs; no retail assets are embedded.
#include "recompiler/recompiler.h"
#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > 65536) return 0;
    lmrc::Input input;
    input.dol.assign(data, data + size);
    input.version = "GLME01";
    input.symbols = "__start = .text:0x80003100; // type:function size:0x4\n";
    lmrc::Output output;
    std::string error;
    lmrc::recompile(input, output, error);
    return 0;
}
