#pragma once

#include "game/protocol_constants.hpp"

#include <cstdint>

namespace wowee {
namespace game {

// IsCurrentAction for a spell on the action bar: the spell the player is
// casting or channelling, or Auto Attack while the swing is on. castSpellId
// is 0 when nothing is being cast.
[[nodiscard]] constexpr bool isCurrentSpellAction(uint32_t slotSpellId, uint32_t castSpellId,
                                                  bool autoAttacking) {
    if (slotSpellId == 0) return false;
    if (slotSpellId == SPELL_ID_ATTACK) return autoAttacking;
    return slotSpellId == castSpellId;
}

}  // namespace game
}  // namespace wowee
