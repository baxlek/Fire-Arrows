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

// Hook target: the menu code that lets the player drag one item onto another to combine them
// (used natively for Bow+Bomb and Bow+Hawkeye). Its whitelist doesn't know about the Lantern, so
// while the player is combining items we briefly present the Lantern as a Hawkeye (a combo the
// whitelist already accepts), then restore the real item right after the call. This reuses all
// of the vanilla combining/bookkeeping logic instead of reimplementing it.
DEFINE_HOOK(&dMenu_Ring_c::setMixItem, SetMixItem);

// Hook target: the moment an arrow is actually released, where the game already special-cases
// the arrow's attack material per arrow type (see the ARROW_TYPE_LIGHT branch below it in the
// original code). This is where we mark the arrow as a fire arrow if the combo is active.
DEFINE_HOOK(&daArrow_c::arrowShooting, ArrowShooting);

// Slot temporarily disguised by on_set_mix_item_pre, restored by on_set_mix_item_post.
// NO_MIX_ITEM means "nothing to restore".
static u8 g_disguisedItemSlot = NO_MIX_ITEM;

static HookAction on_set_mix_item_pre(ModContext*, void* args, void*, void*) {
    dMenu_Ring_c* menu = mods::arg<dMenu_Ring_c*>(args, 0);
    u8 slot = menu->mItemSlots[menu->mCurrentSlot];

    if (dComIfGs_getItem(slot, false) == dItemNo_KANTERA_e) {
        dComIfGs_setItem(slot, dItemNo_HAWK_EYE_e);
        g_disguisedItemSlot = slot;
    }

    return HOOK_CONTINUE;
}

static void on_set_mix_item_post(ModContext*, void*, void*, void*) {
    if (g_disguisedItemSlot != NO_MIX_ITEM) {
        dComIfGs_setItem(g_disguisedItemSlot, dItemNo_KANTERA_e);
        g_disguisedItemSlot = NO_MIX_ITEM;
    }
}

// True if the Bow is assigned to one of the two C-button slots and the Lantern is mixed into it,
// the same way dItemNo_HAWK_ARROW_e/dItemNo_BOMB_ARROW_e detect their combos.
static bool checkBowLanternCombo() {
    for (int selectItemIdx = 0; selectItemIdx < 2; selectItemIdx++) {
        if (dComIfGp_getSelectItem(selectItemIdx) != dItemNo_BOW_e) {
            continue;
        }

        u8 mixSlot = dComIfGs_getMixItemIndex(selectItemIdx);
        if (mixSlot != NO_MIX_ITEM && dComIfGs_getItem(mixSlot, false) == dItemNo_KANTERA_e) {
            return true;
        }
    }

    return false;
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

    static const cXyz scale = {1.0f, 1.0f, 1.0f};
    dComIfGp_particle_set(dPa_RM(ID_ZI_S_RD_ARROWFIRE_A), &arrow->current.pos, &arrow->shape_angle,
                          &scale);
    Z2GetAudioMgr()->seStart(Z2SE_OBJ_ARROW_SHOT_FIRE, &arrow->current.pos, 0, 0, 1.0f, 1.0f, -1.0f,
                             -1.0f, 0);

    return HOOK_CONTINUE;
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

    result = mods::hook::add_pre<ArrowShooting>(on_arrow_shooting_pre);
    if (result != MOD_OK) {
        mods::log::error("failed to install pre hook on_arrow_shooting_pre");
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
