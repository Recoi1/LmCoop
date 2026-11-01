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

constexpr u16 kProtocolVersion = 1;
constexpr size_t kMaxPacket = 1200;
constexpr u32 kMagic = 0x4F434D4C;  // "LMCO"
constexpr float kMaxCoord = 1e5f;

enum class Kind : u8 { Hello = 1, Welcome = 2, Reject = 3, State = 4, Bye = 5 };
struct PadState { u16 buttons; s8 stickX, stickY, substickX, substickY; u8 triggerL, triggerR; };
struct PlayerState { u32 frame; u16 map; u8 room; float pos[3]; float rotY; float vel[3]; u16 anim; u16 hp; PadState pad; };
struct Hello { u16 version; u8 dolSha1[20]; u8 passwordHash[32]; };  // SHA-256 of the password
struct Header { u32 magic; Kind kind; u32 seq; };

constexpr size_t kHeaderWireSize = 4 + 1 + 4;
constexpr size_t kPadWireSize = 2 + 4 + 2;
constexpr size_t kStateWireSize = 4 + 2 + 1 + 12 + 4 + 12 + 2 + 2 + kPadWireSize;
constexpr size_t kHelloWireSize = 2 + 20 + 32;

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
    if (h.magic != kMagic || kind < u8(Kind::Hello) || kind > u8(Kind::Bye)) return false;
    h.kind = Kind(kind);
    body = in + kHeaderWireSize, bodySize = len - kHeaderWireSize;
    return true;
}

inline size_t encodeState(const PlayerState& s, u8* out, size_t cap) {
    wire::Writer w{out, cap};
    w.le(s.frame), w.le(s.map), w.le(s.room);
    for (float v : s.pos) w.f32(v);
    w.f32(s.rotY);
    for (float v : s.vel) w.f32(v);
    w.le(s.anim), w.le(s.hp);
    w.le(s.pad.buttons), w.le(u8(s.pad.stickX)), w.le(u8(s.pad.stickY)), w.le(u8(s.pad.substickX)),
        w.le(u8(s.pad.substickY)), w.le(s.pad.triggerL), w.le(s.pad.triggerR);
    return w.ok ? w.n : 0;
}

inline bool decodeState(const u8* body, size_t n, PlayerState& s) {
    if (n != kStateWireSize) return false;
    wire::Reader r{body, n};
    PlayerState t{};
    t.frame = r.le<u32>(), t.map = r.le<u16>(), t.room = r.le<u8>();
    for (float& v : t.pos) v = r.f32();
    t.rotY = r.f32();
    for (float& v : t.vel) v = r.f32();
    t.anim = r.le<u16>(), t.hp = r.le<u16>();
    t.pad.buttons = r.le<u16>();
    t.pad.stickX = s8(r.le<u8>()), t.pad.stickY = s8(r.le<u8>());
    t.pad.substickX = s8(r.le<u8>()), t.pad.substickY = s8(r.le<u8>());
    t.pad.triggerL = r.le<u8>(), t.pad.triggerR = r.le<u8>();
    const auto sane = [](float v, float lim) { return std::isfinite(v) && std::fabs(v) <= lim; };
    for (int i = 0; i < 3; i++)
        if (!sane(t.pos[i], kMaxCoord) || !sane(t.vel[i], kMaxCoord)) return false;
    if (!sane(t.rotY, kMaxCoord) || !r.ok) return false;
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
