#include <codex/lighting.h>

bool codex_lighting_zone_on(const struct codex_lighting_zone *zone)
{
    return zone != NULL && zone->effect != CODEX_EFFECT_OFF;
}

bool codex_lighting_all_off(const struct codex_lighting_model *model)
{
    if (model == NULL || codex_lighting_zone_on(&model->keys) ||
        codex_lighting_zone_on(&model->ambient)) {
        return false;
    }
    for (size_t i = 0U; i < CODEX_AGENT_COUNT; i++) {
        if (codex_lighting_zone_on(&model->agents[i].zone)) {
            return false;
        }
    }
    return true;
}
