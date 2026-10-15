// Server packets through GameHandler's own dispatch. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "addons/lua_api_registrations.hpp"
#include "game/entity.hpp"
#include "game/game_utils.hpp"
#include "game/packed_time.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

using replay::Session;
using replay::UpdateObject;
using wowee::game::UF;

namespace {

std::shared_ptr<wowee::game::Unit> playerUnit(Session& s) {
    return std::dynamic_pointer_cast<wowee::game::Unit>(
        s.handler().getEntityManager().getEntity(Session::kPlayerGuid));
}

constexpr uint32_t kAutoShot = 75;
constexpr uint64_t kMob = 0xF130000000000123ull;
constexpr uint64_t kOtherHunter = 0x0000000000000077ull;

// A hunter in the world with a mob to shoot.
void huntMob(Session& s) {
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_LEVEL, 9}});
    s.receive(UpdateObject().create(kMob, wowee::game::ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_LEVEL, 8}}));
}

// The castCount CMSG_CAST_SPELL carried on the latest press, its first byte.
uint8_t lastCastCount(const Session& s) {
    auto casts = s.sent().of(wowee::game::Opcode::CMSG_CAST_SPELL);
    REQUIRE_FALSE(casts.empty());
    return casts.back().getData().at(0);
}

// Spell::SendCastResult: castCount, spellId, result.
wowee::network::Packet castFailed(uint8_t castCount, uint32_t spellId, uint8_t result) {
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_CAST_FAILED));
    p.writeUInt8(castCount);
    p.writeUInt32(spellId);
    p.writeUInt8(result);
    return p;
}

// Player::SendAutoRepeatCancel: the shooter's packed guid, to everyone nearby.
wowee::network::Packet autoRepeatCancelled(uint64_t shooter) {
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_CANCEL_AUTO_REPEAT));
    p.writePackedGuid(shooter);
    return p;
}

constexpr uint8_t kNotReady = 67;
constexpr uint8_t kOutOfRange = 97;

}  // namespace

TEST_CASE("the session speaks WotLK", "[replay]") {
    Session s;
    CHECK(wowee::game::isActiveExpansion("wotlk"));
    CHECK_FALSE(wowee::game::isPreWotlk());
}

TEST_CASE("the player's create block makes the player's unit", "[replay]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181},
                   {UF::UNIT_FIELD_MAXHEALTH, 181},
                   {UF::UNIT_FIELD_LEVEL, 9}});

    auto unit = playerUnit(s);
    REQUIRE(unit);
    CHECK(unit->getHealth() == 181);
    CHECK(unit->getMaxHealth() == 181);
    CHECK(unit->getLevel() == 9);
}

TEST_CASE("a values block changes only the fields it carries", "[replay]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181},
                   {UF::UNIT_FIELD_MAXHEALTH, 181},
                   {UF::UNIT_FIELD_LEVEL, 9}});
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_HEALTH, 65}}));

    auto unit = playerUnit(s);
    REQUIRE(unit);
    CHECK(unit->getHealth() == 65);
    CHECK(unit->getMaxHealth() == 181);
}

// AzerothCore's Player::SetRestState and RemoveRestState set and clear
// PLAYER_FLAGS_RESTING (0x20).
TEST_CASE("resting follows the resting player flag", "[replay]") {
    Session s;
    s.spawnPlayer({{UF::PLAYER_FLAGS, 0x20}});
    CHECK(s.handler().isPlayerResting());

    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_FLAGS, 0}}));
    CHECK_FALSE(s.handler().isPlayerResting());
}

// Player::SendInitialPacketsBeforeAddToMap: the packed game time, the speed,
// and a zero added in 3.1.2.
TEST_CASE("the login time sets the game clock", "[replay]") {
    Session s;
    wowee::game::WowDate now;
    now.hour = 13;
    now.minute = 30;
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_LOGIN_SETTIMESPEED));
    p.writeUInt32(wowee::game::packWowPackedTime(now));
    p.writeFloat(0.01666667f);
    p.writeUInt32(0);
    s.receive(p);

    CHECK(s.handler().getGameTime() == Catch::Approx(13.5f).margin(0.01f));
}

TEST_CASE("a refused Auto Shot press stops the repeat it started", "[replay]") {
    Session s;
    huntMob(s);
    s.handler().castSpell(kAutoShot, kMob);
    REQUIRE(s.handler().getAutoRepeatSpellId() == kAutoShot);

    s.receive(castFailed(lastCastCount(s), kAutoShot, kOutOfRange));
    CHECK(s.handler().getAutoRepeatSpellId() == 0);
}

TEST_CASE("a refused second Auto Shot press leaves the first one repeating", "[replay]") {
    Session s;
    huntMob(s);
    s.handler().castSpell(kAutoShot, kMob);
    s.handler().castSpell(kAutoShot, kMob);

    s.receive(castFailed(lastCastCount(s), kAutoShot, kNotReady));
    CHECK(s.handler().getAutoRepeatSpellId() == kAutoShot);
}

// Unit::InterruptSpell sends it for whoever stopped shooting.
TEST_CASE("only the player's own auto repeat cancel stops Auto Shot", "[replay]") {
    Session s;
    huntMob(s);
    s.handler().castSpell(kAutoShot, kMob);

    s.receive(autoRepeatCancelled(kOtherHunter));
    CHECK(s.handler().getAutoRepeatSpellId() == kAutoShot);

    s.receive(autoRepeatCancelled(Session::kPlayerGuid));
    CHECK(s.handler().getAutoRepeatSpellId() == 0);
}

constexpr uint32_t kArcaneShot = 3044;

TEST_CASE("a refused cast takes back the GCD it started", "[replay]") {
    Session s;
    huntMob(s);
    s.handler().castSpell(kArcaneShot, kMob);
    REQUIRE(s.handler().isGCDActive());

    s.receive(castFailed(lastCastCount(s), kArcaneShot, kOutOfRange));
    CHECK_FALSE(s.handler().isGCDActive());
}

TEST_CASE("a press refused during the GCD leaves the GCD running", "[replay]") {
    Session s;
    huntMob(s);
    s.handler().castSpell(kArcaneShot, kMob);
    s.handler().castSpell(kArcaneShot, kMob);

    s.receive(castFailed(lastCastCount(s), kArcaneShot, kNotReady));
    CHECK(s.handler().isGCDActive());
}

namespace {

constexpr uint64_t kPet = 0xF140000000000051ull;

// Player::PetSpellInitialize up to the spell count: guid, family, duration,
// react and command states, flags, the ten action bar slots.
wowee::network::Packet petSpells(uint64_t pet) {
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_PET_SPELLS));
    p.writeUInt64(pet);
    p.writeUInt16(0);
    p.writeUInt32(0);
    p.writeUInt8(1);
    p.writeUInt8(1);
    p.writeUInt16(0);
    for (int i = 0; i < 10; ++i) p.writeUInt32(0);
    p.writeUInt8(0);
    return p;
}

bool petCanBeAbandoned(Session& s) {
    lua_State* L = luaL_newstate();
    lua_pushlightuserdata(L, &s.handler());
    lua_setfield(L, LUA_REGISTRYINDEX, "wowee_game_handler");
    wowee::addons::registerUnitLuaAPI(L);
    REQUIRE(luaL_dostring(L, "return PetCanBeAbandoned()") == 0);
    const bool result = lua_toboolean(L, -1) != 0;
    lua_close(L);
    return result;
}

}  // namespace

// Pet::CreateBaseAtTamed and Pet::LoadPetFromDB set UNIT_CAN_BE_ABANDONED
// (0x02) in UNIT_FIELD_BYTES_2 byte 2 on a hunter's beast, and nothing else
// sets it.
TEST_CASE("PetCanBeAbandoned is true for a tamed pet only", "[replay]") {
    for (const bool tamed : {true, false}) {
        Session s;
        s.spawnPlayer({{UF::UNIT_FIELD_LEVEL, 10}});
        s.receive(UpdateObject().create(
            kPet, wowee::game::ObjectType::UNIT,
            {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_BYTES_2, tamed ? 0x03u << 16 : 0u}}));
        s.receive(petSpells(kPet));
        REQUIRE(s.handler().getPetGuid() == kPet);

        CHECK(petCanBeAbandoned(s) == tamed);
    }
}

// WorldPackets::Misc::Weather: the WeatherState, its intensity, and whether
// it changes at once. Weather::GetWeatherState picks the state.
TEST_CASE("weather states read as what the server means", "[replay]") {
    using wowee::game::WeatherKind;
    struct Case { uint32_t state; WeatherKind kind; bool raining; bool snowing; };
    const Case cases[] = {
        {0, WeatherKind::NONE, false, false},
        {1, WeatherKind::FOG, false, false},
        {3, WeatherKind::RAIN, true, false},
        {4, WeatherKind::RAIN, true, false},
        {5, WeatherKind::RAIN, true, false},
        {6, WeatherKind::SNOW, false, true},
        {7, WeatherKind::SNOW, false, true},
        {8, WeatherKind::SNOW, false, true},
        {22, WeatherKind::SANDSTORM, false, false},
        {41, WeatherKind::SANDSTORM, false, false},
        {42, WeatherKind::SANDSTORM, false, false},
        {86, WeatherKind::THUNDERSTORM, true, false},
        {90, WeatherKind::RAIN, true, false},
        {106, WeatherKind::SNOW, false, true},
    };
    for (const Case& c : cases) {
        CAPTURE(c.state);
        Session s;
        wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_WEATHER));
        p.writeUInt32(c.state);
        p.writeFloat(0.5f);
        p.writeUInt8(0);
        s.receive(p);

        CHECK(s.handler().getWeatherKind() == c.kind);
        CHECK(s.handler().isRaining() == c.raining);
        CHECK(s.handler().isSnowing() == c.snowing);
    }
}

namespace {

wowee::network::Packet weather(uint32_t state, float intensity) {
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_WEATHER));
    p.writeUInt32(state);
    p.writeFloat(intensity);
    p.writeUInt8(0);
    return p;
}

std::vector<std::string> chatLines(Session& s) {
    std::vector<std::string> lines;
    for (const auto& m : s.handler().getChatHistory()) lines.push_back(m.message);
    return lines;
}

}  // namespace

TEST_CASE("weather is announced once per kind, and clears only after weather", "[replay]") {
    Session s;
    s.receive(weather(3, 0.3f));
    s.receive(weather(5, 0.9f));
    s.receive(weather(0, 0.0f));
    s.receive(weather(1, 0.5f));
    s.receive(weather(0, 0.0f));
    CHECK(chatLines(s) == std::vector<std::string>{"It begins to rain.", "The weather clears."});
}

TEST_CASE("faint rain is not rain", "[replay]") {
    Session s;
    s.receive(weather(4, 0.01f));
    CHECK_FALSE(s.handler().isRaining());
}

// AuraEffect::HandleFeignDeath sets UNIT_FLAG2_FEIGN_DEATH and
// UNIT_DYNFLAG_DEAD together, and removes them together.
TEST_CASE("Feign Death is not a death", "[replay]") {
    Session s;
    std::vector<std::string> events;
    s.handler().setAddonEventCallback(
        [&events](const std::string& name, const std::vector<std::string>&) { events.push_back(name); });
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});

    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_FLAGS_2, 0x1}, {UF::UNIT_DYNAMIC_FLAGS, 0x20}}));
    CHECK_FALSE(s.handler().isPlayerDead());

    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_FLAGS_2, 0}, {UF::UNIT_DYNAMIC_FLAGS, 0}}));
    CHECK_FALSE(s.handler().isPlayerDead());
    CHECK(std::count(events.begin(), events.end(), "PLAYER_DEAD") == 0);
    CHECK(std::count(events.begin(), events.end(), "PLAYER_ALIVE") == 0);
}

TEST_CASE("the dead flag at zero health is still a death", "[replay]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});
    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_HEALTH, 0}, {UF::UNIT_DYNAMIC_FLAGS, 0x20}}));
    CHECK(s.handler().isPlayerDead());
}

// vmangos's Unit::SetFeignDeath sets the dead flag alone.
TEST_CASE("the dead flag at full health is Feign Death without its flags2 bit", "[replay]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_DYNAMIC_FLAGS, 0x20}}));
    CHECK_FALSE(s.handler().isPlayerDead());
}

TEST_CASE("killed while feigning stays dead when the feign ends", "[replay]") {
    Session s;
    std::vector<std::string> events;
    s.handler().setAddonEventCallback(
        [&events](const std::string& name, const std::vector<std::string>&) { events.push_back(name); });
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});
    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_FLAGS_2, 0x1}, {UF::UNIT_DYNAMIC_FLAGS, 0x20}}));
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_HEALTH, 0}}));
    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_FLAGS_2, 0}, {UF::UNIT_DYNAMIC_FLAGS, 0}}));
    CHECK(s.handler().isPlayerDead());
    CHECK(std::count(events.begin(), events.end(), "PLAYER_ALIVE") == 0);
}

// A player created while feigning, as after a relog or a teleport mid-feign.
TEST_CASE("a player created feigning is alive, and stays so when it ends", "[replay]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300},
                   {UF::UNIT_FIELD_MAXHEALTH, 300},
                   {UF::UNIT_FIELD_FLAGS_2, 0x1},
                   {UF::UNIT_DYNAMIC_FLAGS, 0x20}});
    CHECK_FALSE(s.handler().isPlayerDead());
    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_FLAGS_2, 0}, {UF::UNIT_DYNAMIC_FLAGS, 0}}));
    CHECK_FALSE(s.handler().isPlayerDead());
}

namespace {

bool petCanBeRenamed(Session& s) {
    lua_State* L = luaL_newstate();
    lua_pushlightuserdata(L, &s.handler());
    lua_setfield(L, LUA_REGISTRYINDEX, "wowee_game_handler");
    wowee::addons::registerSocialLuaAPI(L);
    REQUIRE(luaL_dostring(L, "return PetCanBeRenamed()") == 0);
    const bool result = lua_toboolean(L, -1) != 0;
    lua_close(L);
    return result;
}

}  // namespace

// Pet::LoadPetFromDB sets UNIT_CAN_BE_RENAMED (0x01) beside
// UNIT_CAN_BE_ABANDONED only on a pet never named, and
// WorldSession::HandlePetRename clears it.
TEST_CASE("PetCanBeRenamed follows the pet's rename flag", "[replay]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_LEVEL, 10}});
    s.receive(UpdateObject().create(kPet, wowee::game::ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_BYTES_2, 0x03u << 16}}));
    s.receive(petSpells(kPet));
    CHECK(petCanBeRenamed(s));

    s.receive(UpdateObject().values(kPet, {{UF::UNIT_FIELD_BYTES_2, 0x02u << 16}}));
    CHECK_FALSE(petCanBeRenamed(s));
}
