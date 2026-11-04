// Run from the repository root: clang++ -std=c++20 -Isrc tools/recomp/online_protocol_test.cpp -o online_protocol_test
#include "recomp/online_protocol.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>

using namespace recomp::online;

static PlayerState sample() {
    PlayerState s{};
    s.frame = 4321;
    s.world = 17;
    s.map = 2;
    s.room = 14;
    s.pos[0] = 4115.5f, s.pos[1] = -31.8f, s.pos[2] = -3609.f;
    s.rotY = 1.25f;
    s.vel[0] = 2.f, s.vel[1] = 0.f, s.vel[2] = -3.5f;
    s.anim = 17;
    s.hp = 87;
    s.pad = {0x0120, -100, 64, 0, -5, 30, 200};
    std::strcpy(s.name, "Recoi1");
    return s;
}

int main() {
    u8 buf[kMaxPacket];
    Header h{kMagic, Kind::State, 77};
    u8 body[kMaxPacket];
    const PlayerState in = sample();
    const size_t bodyLen = encodeState(in, body, sizeof(body));
    assert(bodyLen == kStateWireSize);
    const size_t n = encode(h, body, bodyLen, buf, sizeof(buf));
    assert(n == kHeaderWireSize + bodyLen);

    // Round trip keeps every field and the sequence number.
    Header got{};
    const u8* b = nullptr;
    size_t bn = 0;
    assert(decode(buf, n, got, b, bn));
    assert(got.kind == Kind::State && got.seq == 77 && bn == bodyLen);
    PlayerState out{};
    assert(decodeState(b, bn, out));
    assert(out.frame == in.frame && out.world == in.world && out.map == in.map && out.room == in.room && out.anim == in.anim && out.hp == in.hp);
    for (int i = 0; i < 3; i++) assert(out.pos[i] == in.pos[i] && out.vel[i] == in.vel[i]);
    assert(out.rotY == in.rotY);
    assert(std::memcmp(&out.pad, &in.pad, sizeof(PadState)) == 0);
    assert(std::strcmp(out.name, "Recoi1") == 0);

    // Names: at most 15 characters arrive, always terminated; control bytes become '?'.
    PlayerState odd = in;
    std::memset(odd.name, 'A', sizeof(odd.name));  // no terminator
    odd.name[2] = '\n';
    encodeState(odd, body, sizeof(body));
    assert(decodeState(body, kStateWireSize, out));
    assert(std::strlen(out.name) == 15 && out.name[2] == '?' && out.name[0] == 'A');

    // Hello round trip.
    Hello hi{};
    hi.version = kProtocolVersion;
    for (int i = 0; i < 20; i++) hi.dolSha1[i] = u8(i);
    for (int i = 0; i < 32; i++) hi.passwordHash[i] = u8(0xA0 + i);
    const size_t hl = encodeHello(hi, body, sizeof(body));
    Hello ho{};
    assert(decodeHello(body, hl, ho));
    assert(ho.version == kProtocolVersion && std::memcmp(ho.dolSha1, hi.dolSha1, 20) == 0 &&
           std::memcmp(ho.passwordHash, hi.passwordHash, 32) == 0);
    assert(!decodeHello(body, hl - 1, ho));

    // Header rejections: wrong magic, unknown kind, truncated, oversized.
    u8 bad[kMaxPacket + 8];
    std::memcpy(bad, buf, n);
    bad[0] ^= 0xFF;
    assert(!decode(bad, n, got, b, bn));
    std::memcpy(bad, buf, n);
    bad[4] = 9;
    assert(!decode(bad, n, got, b, bn));
    assert(!decode(buf, kHeaderWireSize - 1, got, b, bn));
    assert(!decode(buf, kMaxPacket + 1, got, b, bn));

    // State rejections: short body, NaN, position out of range.
    assert(!decodeState(b, bn - 1, out));
    PlayerState nan = in;
    nan.pos[1] = std::numeric_limits<float>::quiet_NaN();
    encodeState(nan, body, sizeof(body));
    assert(!decodeState(body, kStateWireSize, out));

    GameState game{};
    game.world = 22, game.map = 2, game.room = 24, game.hidden = 1;
    game.pos[0] = 1234, game.pos[1] = -5, game.pos[2] = -789, game.rotY = 1.5f;
    for (size_t i = 0; i < kSaveSize; i++) game.save[i] = u8(i);
    game.save[0x234] = game.save[0x236] = 0;
    game.save[0x235] = 87, game.save[0x237] = 100;
    std::memset(game.save + 0x234 + 0xB4, 0, 4);
    game.flags[31] = 0x81;
    assert(encodeGame(game, body, sizeof(body)) == kGameWireSize);
    GameState restored{};
    assert(decodeGame(body, kGameWireSize, restored));
    assert(restored.world == 22 && restored.map == 2 && restored.room == 24 && restored.hidden == 1);
    assert(restored.pos[0] == 1234 && restored.rotY == 1.5f && restored.flags[31] == 0x81);
    assert(std::memcmp(game.save, restored.save, kSaveSize) == 0);
    assert(!decodeGame(body, kGameWireSize - 1, restored));
    assert(!decodeGame(body, kGameWireSize + 1, restored));
    assert(encodeGame(game, body, kGameWireSize - 1) == 0);
    for (int invalid = 0; invalid < 9; invalid++) {
        GameState badGame = game;
        if (invalid == 0) badGame.world = 0;
        if (invalid == 1) badGame.map = 14;
        if (invalid == 2) badGame.room = 255;
        if (invalid == 3) badGame.hidden = 2;
        if (invalid == 4) badGame.pos[0] = std::numeric_limits<float>::quiet_NaN();
        if (invalid == 5) badGame.rotY = std::numeric_limits<float>::infinity();
        if (invalid == 6) badGame.save[0x234] = 0xFF;
        if (invalid == 7) badGame.save[0x237] = 0;
        if (invalid == 8) badGame.save[0x234 + 0xB4 + 3] = 65;
        encodeGame(badGame, body, sizeof(body));
        assert(!decodeGame(body, kGameWireSize, restored));
        assert(restored.world == 22 && restored.pos[0] == 1234); // a rejected packet changes nothing
    }
    PlayerState far = in;
    far.pos[0] = 2e5f;
    encodeState(far, body, sizeof(body));
    assert(!decodeState(body, kStateWireSize, out));

    // Encoding into a too-small buffer writes nothing.
    assert(encode(h, body, bodyLen, buf, kHeaderWireSize) == 0);

    // State carries the cutscene progress and the scene (protocol 4).
    PlayerState ev = in;
    ev.event.serial = 9, ev.event.choice = 2, ev.event.choiceOffset = 0x1234, ev.event.selection = 3;
    ev.event.phase = EventPhase::Playing;
    ev.scene = 4, ev.sceneDone = true;
    ev.yaw = 0xBEEF, ev.aim = 201;
    encodeState(ev, body, sizeof(body));
    assert(decodeState(body, kStateWireSize, out));
    assert(out.event.serial == 9 && out.event.choice == 2 && out.event.choiceOffset == 0x1234 &&
           out.event.selection == 3 && out.event.phase == EventPhase::Playing && out.scene == 4 && out.sceneDone);
    assert(out.yaw == 0xBEEF && out.aim == 201);
    for (int invalid = 0; invalid < 4; invalid++) {
        PlayerState badEv = ev;
        if (invalid == 0) badEv.event.phase = EventPhase(9);
        if (invalid == 1) badEv.event.selection = 32;
        if (invalid == 2) badEv.event.choiceOffset = 0x100001;
        if (invalid == 3) badEv.scene = 8;
        encodeState(badEv, body, sizeof(body));
        assert(!decodeState(body, kStateWireSize, out));
    }

    // Game: the scene (2 main game, 4-7 the special scenes) and the rank.
    GameState scene = game;
    scene.scene = 5, scene.rank = 7;
    encodeGame(scene, body, sizeof(body));
    assert(decodeGame(body, kGameWireSize, restored) && restored.scene == 5 && restored.rank == 7);
    for (const u8 s : {u8(0), u8(1), u8(3), u8(8)}) {
        scene.scene = s, scene.rank = 0;
        encodeGame(scene, body, sizeof(body));
        assert(!decodeGame(body, kGameWireSize, restored));
    }
    scene.scene = 2, scene.rank = 8;
    encodeGame(scene, body, sizeof(body));
    assert(!decodeGame(body, kGameWireSize, restored));

    // Event: a cutscene start chosen by the host, with the host's game around it.
    SceneEvent se{};
    se.game = game, se.game.scene = 2;
    se.serial = 5, se.parameter = 0x1234, se.trigger = 17, se.number = 44;
    assert(encodeEvent(se, body, sizeof(body)) == kEventWireSize);
    assert(encodeEvent(se, body, kEventWireSize - 1) == 0);
    SceneEvent seOut{};
    assert(decodeEvent(body, kEventWireSize, seOut));
    assert(seOut.serial == 5 && seOut.parameter == 0x1234 && seOut.trigger == 17 && seOut.number == 44 &&
           seOut.game.world == 22 && std::memcmp(seOut.game.save, game.save, kSaveSize) == 0);
    assert(!decodeEvent(body, kEventWireSize - 1, seOut));
    assert(!decodeEvent(body, kEventWireSize + 1, seOut));
    se.parameter = 0xFFFFFFFF;  // "no parameter" is allowed
    encodeEvent(se, body, sizeof(body));
    assert(decodeEvent(body, kEventWireSize, seOut) && seOut.parameter == 0xFFFFFFFF);
    for (int invalid = 0; invalid < 5; invalid++) {
        SceneEvent badSe = se;
        if (invalid == 0) badSe.serial = 0;
        if (invalid == 1) badSe.game.scene = 4;
        if (invalid == 2) badSe.trigger = 2048;
        if (invalid == 3) badSe.number = 110;
        if (invalid == 4) badSe.parameter = 0x10000;
        encodeEvent(badSe, body, sizeof(body));
        seOut = {};
        assert(!decodeEvent(body, kEventWireSize, seOut));
        assert(seOut.serial == 0);  // a rejected packet changes nothing
    }
    // Every message still fits one datagram.
    assert(kHeaderWireSize + kEventWireSize <= kMaxPacket && kHeaderWireSize + kStateWireSize <= kMaxPacket);
    return 0;
}
