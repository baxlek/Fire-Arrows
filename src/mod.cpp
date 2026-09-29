#include "mods/service.hpp"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.hpp"

// Game includes
#include "Z2AudioLib/Z2AudioMgr.h"
#include "Z2AudioLib/Z2SeMgr.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_arrow.h"
#include "d/d_cc_d.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_menu_ring.h"
#include "d/d_particle_name.h"
#include "d/d_save.h"
#include "f_op/f_op_actor_mng.h"

DEFINE_MOD();

IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);

// --------------------------------------------------------------------------------------------
// Fire Arrows
//
// Lets the player mix the Lantern into the Bow's C-button slot, exactly like the vanilla
// Bow & Hawkeye combo. While the combo is equipped, every arrow fired is lit on the way out,
// igniting whatever it hits the same way the fire arrows shot by Bulblin (Bokoblin) archers do.
// Each shot costs the same amount of lantern oil as a single lantern swing.
// --------------------------------------------------------------------------------------------

// dComIfGs_getMixItemIndex() returns this when a C-button slot has no item mixed into it.
static constexpr u8 NO_MIX_ITEM = 0xFF;

// Hook targets: the menu code that lets the player drag one item onto another to combine them
// (used natively for Bow+Bomb and Bow+Hawkeye), plus the two functions that decide whether to
// show the "Bow & Arrow Combo"/"Combo Off" prompt while an item is highlighted. All three have
// their own whitelist switch that doesn't know about the Lantern, so while any of them run we
// briefly present the highlighted Lantern as a Hawkeye (a combo the whitelists already accept),
// then restore the real item right after the call. This reuses all of the vanilla
// combining/bookkeeping and prompt-display logic instead of reimplementing it.
DEFINE_HOOK(&dMenu_Ring_c::setMixItem, SetMixItem);
DEFINE_HOOK(&dMenu_Ring_c::isMixItemOn, IsMixItemOn);
DEFINE_HOOK(&dMenu_Ring_c::isMixItemOff, IsMixItemOff);

// Hook target: the moment an arrow is actually released, where the game already special-cases
// the arrow's attack material per arrow type (see the ARROW_TYPE_LIGHT branch below it in the
// original code). This is where we mark the arrow as a fire arrow if the combo is active.
DEFINE_HOOK(&daArrow_c::arrowShooting, ArrowShooting);

// Hook target: the arrow's per-frame update. daArrow_c has no arrow-type value for "fire" (unlike
// the enemy fire arrows in d_a_e_arrow.cpp), so we detect it via the attack material we set
// ourselves in on_arrow_shooting_pre and use this to keep the fire trail following the arrow.
DEFINE_HOOK(&daArrow_c::execute, ArrowExecute);

// Slot temporarily disguised by disguiseLanternPre(), restored by restoreLanternPost().
// NO_MIX_ITEM means "nothing to restore". These hooks never nest (each menu function above runs
// to completion before the next is called), so a single slot is enough to track the disguise.
static u8 g_disguisedItemSlot = NO_MIX_ITEM;

static void disguiseLanternPre(dMenu_Ring_c* menu) {
    u8 slot = menu->mItemSlots[menu->mCurrentSlot];

    if (dComIfGs_getItem(slot, false) == dItemNo_KANTERA_e) {
        dComIfGs_setItem(slot, dItemNo_HAWK_EYE_e);
        g_disguisedItemSlot = slot;
    }
}

static void restoreLanternPost() {
    if (g_disguisedItemSlot != NO_MIX_ITEM) {
        dComIfGs_setItem(g_disguisedItemSlot, dItemNo_KANTERA_e);
        g_disguisedItemSlot = NO_MIX_ITEM;
    }
}

static HookAction on_set_mix_item_pre(ModContext*, void* args, void*, void*) {
    disguiseLanternPre(mods::arg<dMenu_Ring_c*>(args, 0));
    return HOOK_CONTINUE;
}

static void on_set_mix_item_post(ModContext*, void* args, void*, void*) {
    restoreLanternPost();

    // setMixItem() reloads the sliding combo-icon textures (via setJumpItem -> setSelectItem)
    // from save data *before* our restore above runs, so if the Lantern was involved it captured
    // the disguised Hawkeye icon instead. Now that the real item is back in the save slot, call
    // setJumpItem() again to reload the correct icon. Harmless to call unconditionally: it just
    // re-reads the (now-correct) current combo state, exactly like the original call did.
    mods::arg<dMenu_Ring_c*>(args, 0)->setJumpItem(false);
}

static HookAction on_is_mix_item_on_pre(ModContext*, void* args, void*, void*) {
    disguiseLanternPre(mods::arg<dMenu_Ring_c*>(args, 0));
    return HOOK_CONTINUE;
}

static void on_is_mix_item_on_post(ModContext*, void*, void*, void*) {
    restoreLanternPost();
}

static HookAction on_is_mix_item_off_pre(ModContext*, void* args, void*, void*) {
    disguiseLanternPre(mods::arg<dMenu_Ring_c*>(args, 0));
    return HOOK_CONTINUE;
}

static void on_is_mix_item_off_post(ModContext*, void*, void*, void*) {
    restoreLanternPost();
}

// True if the Bow is assigned to one of the two C-button slots and the Lantern is mixed into it,
// the same way dItemNo_HAWK_ARROW_e/dItemNo_BOMB_ARROW_e detect their combos.
//
// When a combo is active, dComIfGs_getSelectItemIndex(selectItemIdx) holds the *partner* item's
// inventory slot (e.g. the Lantern's slot) and dComIfGs_getMixItemIndex(selectItemIdx) holds
// SLOT_4 (the Bow's fixed slot) -- not the other way around. dComIfGp_getSelectItem() already
// confirms the mixed-in item is the Bow (it only returns dItemNo_BOW_e after swapping in that
// case), so all that's left to check here is that the partner slot holds the Lantern.
static bool checkBowLanternCombo() {
    for (int selectItemIdx = 0; selectItemIdx < 2; selectItemIdx++) {
        if (dComIfGp_getSelectItem(selectItemIdx) != dItemNo_BOW_e) {
            continue;
        }

        u8 partnerSlot = dComIfGs_getSelectItemIndex(selectItemIdx);
        if (dComIfGs_getItem(partnerSlot, false) == dItemNo_KANTERA_e) {
            return true;
        }
    }

    return false;
}

// Tracks the fire trail particle for the player's own in-flight fire arrows, so it can be
// re-anchored to each arrow's current position every frame (see updateFireArrowEffect below).
// daArrow_c has no spare field to stash this in, unlike the enemy fire arrow actor's own
// mFireEMKeys, so it's tracked externally here instead, keyed by the arrow's actor pointer.
struct TrackedFireArrow {
    daArrow_c* arrow = nullptr;
    u32 particleKey = 0;
    // Handed to the emitter via setUserWork() below; must outlive the emitter itself, so it's
    // stored here rather than as a stack temporary.
    cXyz velocity = {0.0f, 0.0f, 0.0f};
};

// The player only ever has a handful of arrows in flight at once; a small ring buffer is more
// than enough, and simply evicts the oldest tracked arrow (long since landed/despawned by then)
// if it somehow fills up.
static constexpr int MAX_TRACKED_FIRE_ARROWS = 8;
static TrackedFireArrow g_fireArrows[MAX_TRACKED_FIRE_ARROWS];
static int g_nextFireArrowSlot = 0;

// Called once, right when an arrow is fired as part of the combo, to start tracking its fire
// trail particle.
static void trackFireArrow(daArrow_c* arrow) {
    TrackedFireArrow& slot = g_fireArrows[g_nextFireArrowSlot];
    g_nextFireArrowSlot = (g_nextFireArrowSlot + 1) % MAX_TRACKED_FIRE_ARROWS;
    slot.arrow = arrow;
    slot.particleKey = 0;
}

// Called every frame for every live fire arrow (see on_arrow_execute_post). Re-issues the same
// particle emitter (by reusing its key) at the arrow's up-to-date position, exactly like the
// Bulblin (Bokoblin) archers' own fire arrows keep their trail attached to the arrow in
// fire_eff_set() (d_a_e_arrow.cpp) instead of spawning a new, stationary burst once. Also mirrors
// that function's use of the particle "trace" callback: without it, each emitter only knows the
// position it was (re)issued at and its own particles don't get interpolated towards the arrow's
// position in between frames, so the trail can lag behind or, if re-issued too infrequently
// relative to the arrow's speed, appear to not be there at all.
static void updateFireArrowEffect(daArrow_c* arrow) {
    for (TrackedFireArrow& slot : g_fireArrows) {
        if (slot.arrow == arrow) {
            slot.velocity = arrow->speed;
            slot.particleKey =
                dComIfGp_particle_set(slot.particleKey, dPa_RM(ID_ZI_S_RD_ARROWFIRE_A),
                                      &arrow->current.pos, &arrow->shape_angle, NULL);

            JPABaseEmitter* emitter = dComIfGp_particle_getEmitter(slot.particleKey);
            if (emitter != NULL) {
                emitter->setParticleCallBackPtr(dPa_control_c::getParticleTracePCB());
                emitter->setUserWork((uintptr_t)&slot.velocity);
            }
            return;
        }
    }
}

static HookAction on_arrow_shooting_pre(ModContext*, void* args, void*, void*) {
    daArrow_c* arrow = mods::arg<daArrow_c*>(args, 0);

    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == nullptr || !checkBowLanternCombo() || dComIfGs_getOil() == 0) {
        return HOOK_CONTINUE;
    }

    // Same oil cost as a single lantern swing (daAlink_c::initKandelaarSwing).
    dComIfGp_setItemOilCount(-link->mpHIO->mItem.mLantern.m.mShakeOilLoss);

    // Mark the arrow's attack collider as fire, same as how Light Arrows mark theirs as
    // dCcD_MTRL_LIGHT a few lines below in the original function. This makes the arrow ignite
    // flammable targets exactly like a bokoblin archer's fire arrow or a lit lantern swing does.
    arrow->field_0x688.SetAtMtrl(dCcD_MTRL_FIRE);
    trackFireArrow(arrow);

    Z2GetAudioMgr()->seStart(Z2SE_OBJ_ARROW_SHOT_FIRE, &arrow->current.pos, 0, 0, 1.0f, 1.0f, -1.0f,
                             -1.0f, 0);

    return HOOK_CONTINUE;
}

static void on_arrow_execute_post(ModContext*, void* args, void*, void*) {
    daArrow_c* arrow = mods::arg<daArrow_c*>(args, 0);
    if (arrow->field_0x688.GetAtMtrl() == dCcD_MTRL_FIRE) {
        updateFireArrowEffect(arrow);
    }
}

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError*) {
    ModResult result = mods::hook::add_pre<SetMixItem>(on_set_mix_item_pre);
    if (result != MOD_OK) {
        mods::log::error("failed to install pre hook on_set_mix_item_pre");
        return result;
    }

    result = mods::hook::add_post<SetMixItem>(on_set_mix_item_post);
    if (result != MOD_OK) {
        mods::log::error("failed to install post hook on_set_mix_item_post");
        return result;
    }

    result = mods::hook::add_pre<IsMixItemOn>(on_is_mix_item_on_pre);
    if (result != MOD_OK) {
        mods::log::error("failed to install pre hook on_is_mix_item_on_pre");
        return result;
    }

    result = mods::hook::add_post<IsMixItemOn>(on_is_mix_item_on_post);
    if (result != MOD_OK) {
        mods::log::error("failed to install post hook on_is_mix_item_on_post");
        return result;
    }

    result = mods::hook::add_pre<IsMixItemOff>(on_is_mix_item_off_pre);
    if (result != MOD_OK) {
        mods::log::error("failed to install pre hook on_is_mix_item_off_pre");
        return result;
    }

    result = mods::hook::add_post<IsMixItemOff>(on_is_mix_item_off_post);
    if (result != MOD_OK) {
        mods::log::error("failed to install post hook on_is_mix_item_off_post");
        return result;
    }

    result = mods::hook::add_pre<ArrowShooting>(on_arrow_shooting_pre);
    if (result != MOD_OK) {
        mods::log::error("failed to install pre hook on_arrow_shooting_pre");
        return result;
    }

    result = mods::hook::add_post<ArrowExecute>(on_arrow_execute_post);
    if (result != MOD_OK) {
        mods::log::error("failed to install post hook on_arrow_execute_post");
        return result;
    }

    mods::log::info("fire_arrows initialized");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    return MOD_OK;
}
}
