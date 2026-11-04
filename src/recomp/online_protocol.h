#pragma once
// Online co-op wire format (online.cpp): little-endian, packed field by field. Everything here
// is checked before use; nothing received is ever used as a guest address.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace recomp::online {
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using s8 = std::int8_t;

constexpr u16 kProtocolVersion = 4;  // 4: shared cutscene starts, completion and choices
constexpr size_t kMaxPacket = 1200;
constexpr u32 kMagic = 0x4F434D4C;  // "LMCO"
constexpr float kMaxCoord = 1e5f;

enum class Kind : u8 { Hello = 1, Welcome = 2, Reject = 3, State = 4, Bye = 5, GameRequest = 6, Game = 7, Event = 8 };
struct PadState { u16 buttons; s8 stickX, stickY, substickX, substickY; u8 triggerL, triggerR; };
constexpr size_t kNameSize = 16;  // 15 characters and the terminator
enum class EventPhase : u8 { None, Prepared, Playing, Ending, Finished };
struct EventProgress {
    u32 serial = 0, choice = 0, choiceOffset = 0;
    u8 selection = 0;
    EventPhase phase = EventPhase::None;
};
struct PlayerState { u32 frame; u32 world; u16 map; u8 room; float pos[3]; float rotY; float vel[3]; u16 anim; u16 hp; PadState pad; char name[kNameSize]; EventProgress event; u8 scene = 2; bool sceneDone = false; u16 yaw = 0; u8 aim = 128; };  // yaw: actor angle (0x10000 = 360 degrees); aim: flashlight/vacuum aim 0..255 (0.5 neutral)
// The game's pointer-free save record, encoded/decoded by its own save routines. No game
// resources or memory addresses travel over the connection, and this is never written to a card.
constexpr size_t kSaveSize = 0x3B0;
struct GameState {
    u32 world;
    u16 map;
    u8 room, hidden;
    u8 scene = 2;
    u8 rank = 0;
    float pos[3], rotY;
    u8 save[kSaveSize], flags[32];
};
struct SceneEvent { GameState game; u32 serial, parameter; u16 trigger, number; };
struct Hello { u16 version; u8 dolSha1[20]; u8 passwordHash[32]; };  // legacy salted SHA-1, zero-padded
struct Header { u32 magic; Kind kind; u32 seq; };

constexpr size_t kHeaderWireSize = 4 + 1 + 4;
constexpr size_t kPadWireSize = 2 + 4 + 2;
constexpr size_t kStateWireSize = 4 + 4 + 2 + 1 + 12 + 4 + 12 + 2 + 2 + kPadWireSize + kNameSize + 14 + 2 + 3;
constexpr size_t kHelloWireSize = 2 + 20 + 32;
constexpr size_t kGameWireSize = 4 + 2 + 1 + 1 + 1 + 1 + 12 + 4 + kSaveSize + 32;
constexpr size_t kEventWireSize = kGameWireSize + 12;
static_assert(kHeaderWireSize + kEventWireSize <= kMaxPacket);

namespace wire {
struct Writer {
    u8* p; size_t cap, n = 0;
    bool ok = true;
    void bytes(const void* v, size_t len) {
        if (n + len > cap) { ok = false; return; }
        std::memcpy(p + n, v, len), n += len;
    }
    template <typename T> void le(T v) {
        u8 b[sizeof(T)];
        for (size_t i = 0; i < sizeof(T); i++) b[i] = u8(u32(v) >> (8 * i));
        bytes(b, sizeof(T));
    }
    void f32(float v) { u32 u; std::memcpy(&u, &v, 4); le(u); }
};
struct Reader {
    const u8* p; size_t len, n = 0;
    bool ok = true;
    void bytes(void* v, size_t k) {
        if (n + k > len) { ok = false; std::memset(v, 0, k); return; }
        std::memcpy(v, p + n, k), n += k;
    }
    template <typename T> T le() {
        u8 b[sizeof(T)];
        bytes(b, sizeof(T));
        u32 v = 0;
        for (size_t i = 0; i < sizeof(T); i++) v |= u32(b[i]) << (8 * i);
        return T(v);
    }
    float f32() { const u32 u = le<u32>(); float v; std::memcpy(&v, &u, 4); return v; }
};
}  // namespace wire

// Header + body into out; 0 when it does not fit (nothing usable written).
inline size_t encode(const Header& h, const void* body, size_t bodySize, u8* out, size_t cap) {
    if (kHeaderWireSize + bodySize > cap || kHeaderWireSize + bodySize > kMaxPacket) return 0;
    wire::Writer w{out, cap};
    w.le(h.magic), w.le(u8(h.kind)), w.le(h.seq), w.bytes(body, bodySize);
    return w.ok ? w.n : 0;
}

inline bool decode(const u8* in, size_t len, Header& h, const u8*& body, size_t& bodySize) {
    if (len < kHeaderWireSize || len > kMaxPacket) return false;
    wire::Reader r{in, len};
    h.magic = r.le<u32>();
    const u8 kind = r.le<u8>();
    h.seq = r.le<u32>();
    if (h.magic != kMagic || kind < u8(Kind::Hello) || kind > u8(Kind::Event)) return false;
    h.kind = Kind(kind);
    body = in + kHeaderWireSize, bodySize = len - kHeaderWireSize;
    return true;
}

inline size_t encodeState(const PlayerState& s, u8* out, size_t cap) {
    wire::Writer w{out, cap};
    w.le(s.frame), w.le(s.world), w.le(s.map), w.le(s.room);
    for (float v : s.pos) w.f32(v);
    w.f32(s.rotY);
    for (float v : s.vel) w.f32(v);
    w.le(s.anim), w.le(s.hp);
    w.le(s.pad.buttons), w.le(u8(s.pad.stickX)), w.le(u8(s.pad.stickY)), w.le(u8(s.pad.substickX)),
        w.le(u8(s.pad.substickY)), w.le(s.pad.triggerL), w.le(s.pad.triggerR);
    w.bytes(s.name, kNameSize);
    w.le(s.event.serial), w.le(s.event.choice), w.le(s.event.choiceOffset), w.le(s.event.selection), w.le(u8(s.event.phase));
    w.le(s.scene), w.le(u8(s.sceneDone));
    w.le(s.yaw), w.le(s.aim);
    return w.ok ? w.n : 0;
}

inline bool decodeState(const u8* body, size_t n, PlayerState& s) {
    if (n != kStateWireSize) return false;
    wire::Reader r{body, n};
    PlayerState t{};
    t.frame = r.le<u32>(), t.world = r.le<u32>(), t.map = r.le<u16>(), t.room = r.le<u8>();
    for (float& v : t.pos) v = r.f32();
    t.rotY = r.f32();
    for (float& v : t.vel) v = r.f32();
    t.anim = r.le<u16>(), t.hp = r.le<u16>();
    t.pad.buttons = r.le<u16>();
    t.pad.stickX = s8(r.le<u8>()), t.pad.stickY = s8(r.le<u8>());
    t.pad.substickX = s8(r.le<u8>()), t.pad.substickY = s8(r.le<u8>());
    t.pad.triggerL = r.le<u8>(), t.pad.triggerR = r.le<u8>();
    r.bytes(t.name, kNameSize);
    t.event.serial = r.le<u32>(), t.event.choice = r.le<u32>(), t.event.choiceOffset = r.le<u32>();
    t.event.selection = r.le<u8>(), t.event.phase = EventPhase(r.le<u8>());
    t.scene = r.le<u8>();
    const u8 done = r.le<u8>();
    t.yaw = r.le<u16>(), t.aim = r.le<u8>();
    if (t.scene > 7 || done > 1) return false;
    t.sceneDone = done != 0;
    if (u8(t.event.phase) > u8(EventPhase::Finished) || t.event.selection > 31 || t.event.choiceOffset > 0x100000) return false;
    // Shown on screen: printable ASCII only, always terminated.
    t.name[kNameSize - 1] = 0;
    for (char& ch : t.name)
        if (ch && (ch < 0x20 || ch > 0x7E)) ch = '?';
    const auto sane = [](float v, float lim) { return std::isfinite(v) && std::fabs(v) <= lim; };
    for (int i = 0; i < 3; i++)
        if (!sane(t.pos[i], kMaxCoord) || !sane(t.vel[i], kMaxCoord)) return false;
    if (!sane(t.rotY, kMaxCoord) || (t.world && (t.map > 13 || t.room > 127)) || !r.ok) return false;
    s = t;
    return true;
}

inline size_t encodeGame(const GameState& s, u8* out, size_t cap) {
    wire::Writer w{out, cap};
    w.le(s.world), w.le(s.map), w.le(s.room), w.le(s.hidden), w.le(s.scene), w.le(s.rank);
    for (float v : s.pos) w.f32(v);
    w.f32(s.rotY);
    w.bytes(s.save, kSaveSize), w.bytes(s.flags, sizeof(s.flags));
    return w.ok ? w.n : 0;
}

inline bool decodeGame(const u8* body, size_t n, GameState& s) {
    if (n != kGameWireSize) return false;
    wire::Reader r{body, n};
    GameState t{};
    t.world = r.le<u32>(), t.map = r.le<u16>(), t.room = r.le<u8>(), t.hidden = r.le<u8>();
    t.scene = r.le<u8>();
    t.rank = r.le<u8>();
    for (float& v : t.pos) v = r.f32();
    t.rotY = r.f32();
    r.bytes(t.save, kSaveSize), r.bytes(t.flags, sizeof(t.flags));
    if (!r.ok || !t.world || t.map > 13 || t.room > 127 || t.hidden > 1 || t.rank > 7 || (t.scene != 2 && (t.scene < 4 || t.scene > 7))) return false;
    for (float v : t.pos) if (!std::isfinite(v) || std::fabs(v) > kMaxCoord) return false;
    if (!std::isfinite(t.rotY) || std::fabs(t.rotY) > 3.142f) return false;
    const auto be16 = [&](size_t p) { return (u32(t.save[p]) << 8) | t.save[p + 1]; };
    const auto be32 = [&](size_t p) { return (be16(p) << 16) | be16(p + 2); };
    // These saved counts become actor health and the bounds of its captured-ghost list.
    if (be16(0x234) > 999 || be16(0x236) < 1 || be16(0x236) > 999 || be32(0x234 + 0xB4) > 64) return false;
    s = t;
    return true;
}

inline size_t encodeEvent(const SceneEvent& s, u8* out, size_t cap) {
    if (cap < kEventWireSize || encodeGame(s.game, out, cap) != kGameWireSize) return 0;
    wire::Writer w{out + kGameWireSize, cap - kGameWireSize};
    w.le(s.serial), w.le(s.parameter), w.le(s.trigger), w.le(s.number);
    return w.ok ? kEventWireSize : 0;
}
inline bool decodeEvent(const u8* body, size_t n, SceneEvent& s) {
    if (n != kEventWireSize) return false;
    SceneEvent t{};
    if (!decodeGame(body, kGameWireSize, t.game)) return false;
    wire::Reader r{body + kGameWireSize, n - kGameWireSize};
    t.serial = r.le<u32>(), t.parameter = r.le<u32>(), t.trigger = r.le<u16>(), t.number = r.le<u16>();
    if (!r.ok || !t.serial || t.game.scene != 2 || t.trigger >= 2048 || t.number > 109 ||
        (t.parameter > 0xFFFF && t.parameter != 0xFFFFFFFF)) return false;
    s = t;
    return true;
}

inline size_t encodeHello(const Hello& h, u8* out, size_t cap) {
    wire::Writer w{out, cap};
    w.le(h.version), w.bytes(h.dolSha1, 20), w.bytes(h.passwordHash, 32);
    return w.ok ? w.n : 0;
}

inline bool decodeHello(const u8* body, size_t n, Hello& h) {
    if (n != kHelloWireSize) return false;
    wire::Reader r{body, n};
    h.version = r.le<u16>();
    r.bytes(h.dolSha1, 20), r.bytes(h.passwordHash, 32);
    return r.ok;
}
}  // namespace recomp::online
