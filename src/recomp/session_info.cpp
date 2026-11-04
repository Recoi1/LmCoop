// What a crash report should know about this run beyond the log: hosting or joining, over what, and the mods that
// are on. Written to last_run_session.txt at start and again once the game runs (port/crash_report.cpp).
#include "recomp/characters.h"
#include "recomp/online.h"
#include "recomp/texture_packs.h"

#include <sstream>

namespace recomp {

std::string sessionInfo() {
    std::ostringstream o;
    const online::Config c = online::sessionConfig();
    o << "mode=" << (c.mode == online::Mode::Host ? "host" : c.mode == online::Mode::Join ? "join" : "off") << '\n';
    o << "net=" << (c.mode == online::Mode::Off ? "unknown" : !c.internet ? "direct" : c.steam ? "steam" : "internet") << '\n';
    o << "name=" << c.name << '\n';  // kept on this PC; the report scrubs it from the log
    for (const texpacks::Info& t : texpacks::installed())
        if (t.enabled) o << "mod=" << (t.files ? "files" : "textures") << '|' << t.name << '|' << t.version << '\n';
    const std::string ch = characters::selected();
    if (ch != "luigi")
        for (const characters::Info& i : characters::available())
            if (i.id == ch) o << "mod=character|" << i.name << "|\n";
    return o.str();
}

}  // namespace recomp
