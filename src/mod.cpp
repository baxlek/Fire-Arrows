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
// original code). This is where we (re-)mark the arrow as a fire arrow if the combo is active,
// and where the fire-arrow's ignition effect (as opposed to its purely cosmetic flame, which can
// already be showing at this point - see on_alink_make_arrow_post below) actually turns on.
DEFINE_HOOK(&daArrow_c::arrowShooting, ArrowShooting);

// Hook target: the moment the Hero's Bow nocks a new arrow (on drawing the bow back, and again
// whenever the arrow type is switched mid-draw), well before it's actually released. This is
// where we start the fire arrow's cosmetic flame effect early, so the arrowhead already looks lit
// while the shot is being lined up, exactly like it looks once fired.
DEFINE_HOOK(&daAlink_c::makeArrow, AlinkMakeArrow);

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

// A second, independent attack collider (separate from the arrow's own main collider,
// field_0x688) used purely so a fire-combo arrow's impact ignites flammable objects (candles,
// firewood, torches, etc.) exactly like a lantern swing would. Ignitable objects all check for an
// *exact* match on AtType (== AT_TYPE_LANTERN_SWING, not a bitmask test) before igniting, so this
// can't be folded into field_0x688's own AtType: that collider has to keep its default
// AT_TYPE_ARROW so normal arrow damage and enemy-specific arrow-hit reactions (several enemies key
// special reactions off ChkAtType(AT_TYPE_ARROW)) keep working. This mirrors the game's own enemy
// fire-arrow actor (d_a_e_arrow.cpp), which likewise layers a second, separate collider
// (mCcFireEffSph) alongside its main hit collider (mCcAtSph) rather than overloading one
// collider's AtType for two purposes - so yes, both AT_TYPE_ARROW and an AT_TYPE_LANTERN_SWING
// effect can coexist on the same arrow, just not on the same collider.
static const dCcD_SrcSph l_igniteSphSrc = {
    {
        {0x0, {{AT_TYPE_LANTERN_SWING, 0x0, 0x0}, {0x0, 0x0}, 0x0}},  // mObj
        {dCcD_SE_NONE, 0x0, 0x0, dCcD_MTRL_FIRE, 0x0},                // mGObjAt
        {dCcD_SE_NONE, 0x0, 0x0, 0x0, 0x0},                           // mGObjTg
        {0x0},                                                       // mGObjCo
    },                                                                // mObjInf
    {
        {{0.0f, 0.0f, 0.0f}, 50.0f}  // mSph (same radius as daAlink_c::initKandelaarSwing)
    }                                // mSphAttr
};

// Tracks the fire trail particle and ignition collider for the player's own in-flight fire
// arrows, so both can be re-anchored to each arrow's current position every frame (see
// updateFireArrowEffect below). daArrow_c has no spare fields to stash these in, unlike the enemy
// fire arrow actor's own mFireEMKeys/mCcFireEffSph, so they're tracked externally here instead,
// keyed by the arrow's actor pointer.
struct TrackedFireArrow {
    daArrow_c* arrow = nullptr;
    u32 particleKey = 0;
    // Handed to the emitter via setUserWork() below; must outlive the emitter itself, so it's
    // stored here rather than as a stack temporary.
    cXyz velocity = {0.0f, 0.0f, 0.0f};
    dCcD_Sph igniteSph;
    // Its own dedicated Stts, rather than sharing the arrow's own field_0x64c. The collision
    // system tracks per-hit dedup/apid state (e.g. ChkAtNoConHit()'s "already hit this frame/
    // actor" bookkeeping) on the Stts itself, not on the individual At collider, so sharing
    // field_0x688's Stts let its AT_TYPE_ARROW hit against a target silently suppress
    // igniteSph's own AT_TYPE_LANTERN_SWING hit against that same target (or vice versa) -
    // which is what was actually blocking ignition, not the At-set bit. A dedicated Stts (still
    // Init'd with the arrow as its actor, same args field_0x64c itself uses) keeps the two
    // colliders' hit-registration fully independent.
    dCcD_Stts igniteStts;
    // Only turned on once the arrow is actually fired (see arrowShooting's hook below). The
    // cosmetic flame can start as soon as the arrow is nocked, but the collider that ignites
    // flammable objects stays off until then, so merely drawing the bow back near a torch doesn't
    // light it before the shot is actually released.
    bool igniteActive = false;
};

// The player only ever has a handful of arrows in flight at once; a small ring buffer is more
// than enough, and simply evicts the oldest tracked arrow (long since landed/despawned by then)
// if it somehow fills up.
static constexpr int MAX_TRACKED_FIRE_ARROWS = 8;
static TrackedFireArrow g_fireArrows[MAX_TRACKED_FIRE_ARROWS];
static int g_nextFireArrowSlot = 0;

// Called both when an arrow is nocked (drawn back) and again when it's actually fired, to start
// tracking its fire trail particle and (re-)initialize its ignition collider. Idempotent: calling
// it again for an arrow that's already tracked (e.g. the shooting-time call, for an arrow that was
// already tracked at nock time) is a no-op, so the same arrow never ends up straddling two ring
// buffer slots at once.
static void trackFireArrow(daArrow_c* arrow) {
    for (TrackedFireArrow& slot : g_fireArrows) {
        if (slot.arrow == arrow) {
            return;
        }
    }

    TrackedFireArrow& slot = g_fireArrows[g_nextFireArrowSlot];
    g_nextFireArrowSlot = (g_nextFireArrowSlot + 1) % MAX_TRACKED_FIRE_ARROWS;
    slot.arrow = arrow;
    slot.particleKey = 0;
    slot.igniteActive = false;
    // Same Init() args daArrow_c itself uses for field_0x64c (d_a_arrow.cpp), so the ignition
    // collider's Stts still correctly identifies the arrow as its owning actor - it just doesn't
    // share field_0x688's hit-dedup/apid bookkeeping (see igniteStts's declaration above).
    slot.igniteStts.Init(10, 0xff, arrow);
    slot.igniteSph.Set(l_igniteSphSrc);
    slot.igniteSph.SetStts(&slot.igniteStts);
}

// Called once the arrow is actually fired, to turn on the ignition collider for an already
// (or newly) tracked arrow. Split out from trackFireArrow() so nocking an arrow can start its
// cosmetic flame without also making it able to ignite things before the shot is released.
static void activateFireArrowIgnition(daArrow_c* arrow) {
    for (TrackedFireArrow& slot : g_fireArrows) {
        if (slot.arrow == arrow) {
            slot.igniteActive = true;
            return;
        }
    }
}

// True while the arrow isn't actually flying (nocked and waiting to be released, or stopped/stuck
// and waiting to be re-obtained), as opposed to genuinely moving through the air (procMove, or
// procReturn/procSlingHit after deflecting off certain surfaces). Used to keep the fire trail's
// particle trace callback from continuing to advance already-emitted trail particles along a
// stale, no-longer-updated velocity once the arrow itself has stopped moving.
static bool isArrowStationary(daArrow_c* arrow) {
    return arrow->mProcFunc == &daArrow_c::procWait ||
           arrow->mProcFunc == &daArrow_c::procBGStop ||
           arrow->mProcFunc == &daArrow_c::procActorStop ||
           arrow->mProcFunc == &daArrow_c::procActorControllStop;
}

// Called every frame for every live fire arrow (see on_arrow_execute_post). Re-issues the fire
// trail particle emitter (by reusing its key) at the arrow's up-to-date position, exactly like the
// Bulblin (Bokoblin) archers' own fire arrows keep their trail attached to the arrow in
// fire_eff_set() (d_a_e_arrow.cpp) instead of spawning a new, stationary burst once. Also mirrors
// that function's use of the particle "trace" callback: without it, each emitter only knows the
// position it was (re)issued at and its own particles don't get interpolated towards the arrow's
// position in between frames, so the trail can lag behind or, if re-issued too infrequently
// relative to the arrow's speed, appear to not be there at all.
//
// The particle resource used is ID_ZF_J_FIRE02_FIRE, the common (always-resident, never behind
// dPa_RM's per-room-pack bit) flame particle that lit torches (d_a_obj_ktOnFire.cpp), campfires
// (d_a_obj_fireWood[2].cpp) and other fire objects across the game all fall back to using outside
// of rooms with a fancier room-specific variant loaded - i.e. the general-purpose "open flame"
// effect that's guaranteed to actually spawn in any area, unlike the previous attempt
// (ID_IT_JN_ARWFIR_FIRE00), which - it turns out - is a dead resource ID never referenced anywhere
// else in the game and never actually renders regardless of which pack is loaded.
//
// While the arrow is nocked (waiting to be released), the flame is drawn just in front of the
// player's face and at the full "impact-sized" scale would cover most of the first-person aiming
// view. Everywhere else (flying, or stuck after landing) keeps the normal, unscaled size.
static constexpr f32 NOCKED_FLAME_SCALE = 1.0f / 3.0f;

// Also re-anchors the ignition collider (see l_igniteSphSrc above) to the arrow's current
// position every frame, the same way the arrow's own "stuck in wall" collider (field_0x7cc) is
// simply re-issued at current.pos each frame rather than swept.
static void updateFireArrowEffect(daArrow_c* arrow) {
    for (TrackedFireArrow& slot : g_fireArrows) {
        if (slot.arrow == arrow) {
            // arrow->current.pos is the arrow's *held* origin - while nocked, that's the bow's
            // grip/nock matrix (daAlink_c::getLeftItemMatrix(), see setKeepMatrix()), well behind
            // the arrowhead, which is why the flame used to show at the bowstring instead. Bomb
            // Arrows already solve exactly this for their fuse effect (setSmokePos(), which
            // transforms a fixed near-tip offset by the arrow model's current base matrix rather
            // than using current.pos), so reuse that same helper and its output (field_0x9cc) to
            // put our own flame in the same visual spot, correctly tracking the tip whether the
            // arrow is nocked, flying, or stuck. field_0x9cc/mSmokePos are otherwise only written
            // by mArrowType==1 (Bomb Arrow) code, which the fire-arrow combo never applies to, so
            // repurposing them here for a plain arrow doesn't collide with anything.
            arrow->setSmokePos();

            // While the arrow is nocked/waiting or has come to rest (stuck in a wall or an
            // actor), it isn't actually moving frame-to-frame even though arrow->speed may still
            // hold a stale, nonzero pre-impact (or never-set, pre-shot) value. Feeding that stale
            // vector to the trace callback would otherwise keep dragging already-emitted trail
            // particles forward indefinitely, e.g. straight through and out the other side of
            // whatever the arrow just stuck into. Zero it out in those cases instead, so the
            // trail settles into a small, stationary flame at the arrowhead.
            slot.velocity = isArrowStationary(arrow) ? cXyz(0.0f, 0.0f, 0.0f) : arrow->speed;
            slot.particleKey =
                dComIfGp_particle_set(slot.particleKey, ID_ZF_J_FIRE02_FIRE,
                                      &arrow->field_0x9cc, &arrow->shape_angle, NULL);

            JPABaseEmitter* emitter = dComIfGp_particle_getEmitter(slot.particleKey);
            if (emitter != NULL) {
                emitter->setParticleCallBackPtr(dPa_control_c::getParticleTracePCB());
                emitter->setUserWork((uintptr_t)&slot.velocity);

                f32 scale = arrow->mProcFunc == &daArrow_c::procWait ? NOCKED_FLAME_SCALE : 1.0f;
                emitter->setGlobalParticleScale(scale, scale);
            }

            if (slot.igniteActive) {
                // igniteSph has to cover the same forward-swept region as the arrow's own main
                // collider (field_0x688) does each frame, not just sit at the arrowhead's current
                // position. field_0x688 is Set() every frame as a capsule from current.pos out to
                // current.pos + speed * (getOutLengthRate() + 1.0f) (daArrow_c::setArrowAt,
                // d_a_arrow.cpp) - a sweep roughly two frames' worth of travel *ahead* of the
                // visible arrowhead, precisely so a fast arrow can't tunnel through something in a
                // single frame. Because of that head start, field_0x688 reaches a torch/candle's
                // own collider before igniteSph (previously anchored to plain current.pos) ever
                // would, so the arrow already registers its own AT_TYPE_ARROW hit - and stops/
                // deletes itself - before igniteSph gets within range to register its own
                // AT_TYPE_LANTERN_SWING hit on the same object at all. Mirroring the same forward
                // sweep here (center on the swept segment's midpoint, radius grown to cover its
                // full length) lets igniteSph reach anything field_0x688 can reach on the very
                // same frame, so its hit is the one still on record afterwards (see the
                // "collision system"/Stts-sharing note above for why that's the one that wins).
                cXyz igniteCenter = arrow->current.pos;
                f32 igniteRadius = 50.0f;
                if (!isArrowStationary(arrow)) {
                    cXyz sweepTarget =
                        arrow->current.pos + arrow->speed * (arrow->getOutLengthRate() + 1.0f);
                    igniteCenter = (arrow->current.pos + sweepTarget) * 0.5f;
                    igniteRadius = 50.0f + (sweepTarget - arrow->current.pos).abs() * 0.5f;
                }
                slot.igniteSph.SetR(igniteRadius);

                // Mirrors daAlink_c's own real lantern-swing collider (d_a_alink.cpp, the
                // checkKandelaarSwing(1) branch): re-centering the sphere alone isn't enough for
                // it to actually register hits, since the collision system skips any At-side
                // collider whose "set" bit isn't on (see ChkAtSet() gating in d_cc_mass_s.cpp/
                // d_cc_s.cpp). l_igniteSphSrc's mObjAt.mBase.mSPrm starts at 0 (bit not set), so
                // it has to be turned on explicitly the first time this collider becomes active,
                // the same way daAlink_c's mAtSph starts off and gets OnAtSetBit()'d only once an
                // actual swing begins. MoveCAt() (rather than plain SetC()) also keeps the
                // collider's sweep vector up to date every frame after that, the same way the
                // real lantern-swing collider does, so a fast-moving arrow can't tunnel past a
                // torch/candle between two frames.
                if (slot.igniteSph.ChkAtSet()) {
                    slot.igniteSph.MoveCAt(igniteCenter);
                } else {
                    slot.igniteSph.OnAtSetBit();
                    slot.igniteSph.StartCAt(igniteCenter);
                }
                dComIfG_Ccsp()->Set(&slot.igniteSph);
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
    // dCcD_MTRL_LIGHT a few lines below in the original function. This keeps field_0x688's AtType
    // at its default AT_TYPE_ARROW (so normal arrow damage and enemy-specific arrow-hit reactions
    // still work), while still giving the arrow the elemental "fire" material property, exactly
    // like Light Arrows do for dCcD_MTRL_LIGHT without touching their own AtType either.
    arrow->field_0x688.SetAtMtrl(dCcD_MTRL_FIRE);

    // The actual AT_TYPE_LANTERN_SWING ignition effect is provided by a second, independent
    // collider tracked alongside the arrow (see trackFireArrow/l_igniteSphSrc below) rather than
    // by this collider, since ignitable objects require an exact AtType match and AT_TYPE_ARROW
    // can't be combined with AT_TYPE_LANTERN_SWING on one collider without losing one or the
    // other. Usually already tracked from nock time (see on_alink_make_arrow_post below), so this
    // is normally just a no-op that turns the ignition collider itself on.
    trackFireArrow(arrow);
    activateFireArrowIgnition(arrow);

    Z2GetAudioMgr()->seStart(Z2SE_OBJ_ARROW_SHOT_FIRE, &arrow->current.pos, 0, 0, 1.0f, 1.0f, -1.0f,
                             -1.0f, 0);

    return HOOK_CONTINUE;
}

// Runs right after the Hero's Bow nocks a new arrow (drawing the bow back, or switching arrow
// types mid-draw). Marks the arrow as a fire arrow immediately, purely so its cosmetic flame
// effect (see updateFireArrowEffect) already shows on the arrowhead while the shot is being lined
// up, matching how it looks once actually fired. Doesn't touch oil (that's only spent once the
// arrow is actually released, in on_arrow_shooting_pre) or the ignition collider (see
// activateFireArrowIgnition), so merely drawing the bow back near a torch can't ignite it.
static void on_alink_make_arrow_post(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    if (!checkBowLanternCombo() || dComIfGs_getOil() == 0) {
        return;
    }

    fopAc_ac_c* actor = link->mItemAcKeep.getActor();
    if (actor == NULL || fopAcM_GetName(actor) != fpcNm_ARROW_e) {
        return;
    }

    daArrow_c* arrow = static_cast<daArrow_c*>(actor);
    arrow->field_0x688.SetAtMtrl(dCcD_MTRL_FIRE);
    trackFireArrow(arrow);
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

    result = mods::hook::add_post<AlinkMakeArrow>(on_alink_make_arrow_post);
    if (result != MOD_OK) {
        mods::log::error("failed to install post hook on_alink_make_arrow_post");
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
