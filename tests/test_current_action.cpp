#include <catch_amalgamated.hpp>

#include "game/current_action.hpp"

using wowee::game::isCurrentSpellAction;
using wowee::game::SPELL_ID_ATTACK;

// IsCurrentAction answered false for every slot, so a button never showed its
// checked state while its spell was being cast.

TEST_CASE("the slot holding the spell being cast is current", "[current_action]") {
    REQUIRE(isCurrentSpellAction(133, 133, false));
}

TEST_CASE("other slots are not current while a spell is cast", "[current_action]") {
    REQUIRE_FALSE(isCurrentSpellAction(116, 133, false));
}

TEST_CASE("nothing is current when nothing is cast", "[current_action]") {
    REQUIRE_FALSE(isCurrentSpellAction(133, 0, false));
}

TEST_CASE("an empty spell id never matches an idle cast", "[current_action]") {
    REQUIRE_FALSE(isCurrentSpellAction(0, 0, false));
}

TEST_CASE("auto attack is current while the swing is on", "[current_action]") {
    REQUIRE(isCurrentSpellAction(SPELL_ID_ATTACK, 0, true));
    REQUIRE(isCurrentSpellAction(SPELL_ID_ATTACK, 133, true));
}

TEST_CASE("auto attack is not current once the swing stops", "[current_action]") {
    REQUIRE_FALSE(isCurrentSpellAction(SPELL_ID_ATTACK, 0, false));
}

TEST_CASE("the swing does not make a cast spell current", "[current_action]") {
    REQUIRE_FALSE(isCurrentSpellAction(133, 0, true));
}
