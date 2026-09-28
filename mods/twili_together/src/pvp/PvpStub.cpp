#include "pvp/Pvp.hpp"

// PvP is off until P5 replaces this file.
namespace twili::pvp {

bool classifyLocalAttack(fopAc_ac_c*, dCcD_GObjInf*, dCcD_GObjInf*, fopAc_ac_c*, HitReport&) {
    return false;
}

bool hurtboxEnabled(const Client&) {
    return false;
}

void noteLocalExplosion(const cXyz&) {}

uint16_t localVisFlags(daAlink_c*) {
    return 0;
}

}  // namespace twili::pvp
