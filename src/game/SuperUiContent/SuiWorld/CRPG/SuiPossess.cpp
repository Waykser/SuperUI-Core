/*
 * SuperUI CRPG/RTS possession core. See SuiPossess.h for the state model.
 *
 * Ordering law (borrowed from Unit::ModPossess): Camera::SetView must precede
 * SetClientControl or the client ignores the control packets; SetMover must
 * precede the client's CMSG_SET_ACTIVE_MOVER confirmation or
 * HandleSetActiveMoverOpcode snaps the client mover back.
 */

#include "SuiPossess.h"
#include <unordered_map>
#include <cmath>

#include "AiBotAIMain.h"
#include "Bag.h"
#include "Chat.h"
#include "Group.h"
#include "MasterPlayer.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Objects/Player.h"
#include "PlayerBotMgr.h"
#include "Database/DBCStores.h"   // sTalentStore / sTalentTabStore — .sui companion talent
#include "Server/WorldSession.h"
#include "Spell.h"        // SpellCastResult + the SPELL_FAILED_* codes ordered casts report
#include "SpellMgr.h"     // sSpellMgr.GetSpellEntry — reject a spell id that does not exist
#include "SuiFactionControl.h"
#include "SuperUiContent/SuiWorld/Bridge/SuiPortal.h"
#include "SuiWorldState.h"
#include "World.h"

namespace SuiPossess
{

static void SendSnapshot(WorldSession* to, Player* bot);   // defined with the M4 block below
static Creature* FreecamEyeOf(Player* player);
static void EnsureFreecamEye(Player* player);
static void RemoveFreecamEye(Player* player);

static void SendAck(WorldSession* session, ObjectGuid guid, AckResult result, Player* positionOf)
{
    // Custom SMSGs only ever go to clients that have spoken SUI (MSUIClient);
    // a stock 1.12 client driving a GM-command possession must not receive
    // opcodes beyond its table.
    if (!session->IsSuiCapable())
        return;
    // 25-byte ACK prefix + 8-byte SUI1 trailer + 4-byte catalog header +
    // six 32-byte prewarm rows. WorldPacket grows dynamically, but reserving
    // the negotiated maximum avoids reallocating every capability probe.
    WorldPacket data(SMSG_SUI_CONTROL_ACK, 25 + 8 + 4 + 6 * 32);
    data << uint64(guid.GetRawValue());
    data << uint8(result);
    if (positionOf)
        data << positionOf->GetPositionX() << positionOf->GetPositionY()
             << positionOf->GetPositionZ() << positionOf->GetOrientation();
    else
        data << 0.0f << 0.0f << 0.0f << 0.0f;
    // Existing clients consume only the fixed 25-byte ACK. The portal helper
    // appends the self-identifying capability suffix and its optional cast-warm
    // catalog; older clients safely ignore both.
    SuiPortal::WriteCapabilityTrailer(data);
    session->SendPacket(&data);
}

Player* GetControlledBot(WorldSession const* session)
{
    ObjectGuid guid = session->GetSuiControlledGuid();
    return guid.IsEmpty() ? nullptr : sObjectMgr.GetPlayer(guid);
}

Player* GetPossessor(Unit const* bot)
{
    ObjectGuid possessorGuid = bot->GetPossessorGuid();
    if (possessorGuid.IsEmpty() || !possessorGuid.IsPlayer())
        return nullptr;
    Player* possessor = sObjectMgr.GetPlayer(possessorGuid);
    if (!possessor || !possessor->GetSession())
        return nullptr;
    // Distinguish SUI possession from spell (charm-based) possession.
    return possessor->GetSession()->GetSuiControlledGuid() == bot->GetObjectGuid()
        ? possessor : nullptr;
}

bool IsSuiPossessed(Unit const* unit)
{
    return GetPossessor(unit) != nullptr;
}

static AiBotAI* BotAiOf(Player* bot)
{
    return bot ? dynamic_cast<AiBotAI*>(bot->AI()) : nullptr;
}

// ── Own-character autonomy (M5, design corrected 2026-08-10) ─────────────────
// While the human drives a bot or the free camera, their real character runs
// the SAME fleet AI as every SuperUI bot (AiBotAI): it enrolls with the C#
// brain through the normal HELLO/STATE bridge flow, follows the party boss
// (possessed-first pre-pass in FindPartyBoss, so no anchor plumbing), and
// assists in combat. In-party behaviour is the gate — group break force-
// releases and detaches — and the bridge SELL_ITEMS wall refuses live-client
// sessions, so it can never vendor. Never stomps a foreign AI (mind control);
// deletion is explicit — this AI is owned here, not by PlayerBotMgr.

static void AttachUnattendedAI(Player* owner, Player* /*anchor*/)
{
    if (!owner || !owner->IsInWorld())
        return;
    if (dynamic_cast<AiBotAI*>(owner->AI()))
        return;   // already enrolled (bot switch / freecam transition)
    if (owner->AI())
        return;   // charmed/controlled by something else — leave it alone
    AiBotAI::AttachToRealCharacter(owner);
}

static void DetachUnattendedAI(Player* owner)
{
    if (!owner)
        return;
    AiBotAI* ai = dynamic_cast<AiBotAI*>(owner->AI());
    if (!ai)
        return;
    owner->SetAI(nullptr);
    delete ai;
    owner->StopMoving();
    owner->GetMotionMaster()->Clear(false, true);
    owner->GetMotionMaster()->MoveIdle();
    owner->AttackStop();
    // The AI era walked this body by server splines the real client never
    // confirms (no CMSG_MOVE_SPLINE_DONE in MSUIClient); a pending flag left
    // set here discards every movement packet of the returning driver.
    owner->SetSplineDonePending(false);
    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI] %s back under manual control", owner->GetName());
}

static void ParkUnattendedBody(Player* owner)
{
    if (!owner)
        return;
    // A faction-wide target need not share the owner's group. The generic
    // unattended AiBotAI has no explicit anchor in that case and may select
    // solo doctrine/quests while the human is driving a distant army unit.
    // Remove only our attached real-character AI and park the owner exactly at
    // the relocation destination until release restores manual control.
    DetachUnattendedAI(owner);
    owner->StopMoving();
    owner->GetMotionMaster()->Clear(false, true);
    owner->GetMotionMaster()->MoveIdle();
    owner->SetSplineDonePending(false);
    owner->AttackStop();
    owner->CombatStop(true);
}

/// Everything except the ACK — shared by the wire handler and the GM command.
static AckResult TryBegin(WorldSession* session, ObjectGuid targetGuid, Player** grantedBot)
{
    Player* possessor = session->GetPlayer();
    if (!possessor || !possessor->IsInWorld() || session->GetBot())
        return DENY_REQUESTER_STATE;
    if (!session->GetSuiControlledGuid().IsEmpty())
        return DENY_REQUESTER_STATE;    // release first
    if (possessor->IsDead() || possessor->IsTaxiFlying() || possessor->GetTransport() ||
        possessor->IsBeingTeleported() || !possessor->IsSelfMover())
        return DENY_REQUESTER_STATE;

    Player* bot = sObjectMgr.GetPlayer(targetGuid);
    if (!bot || !bot->IsInWorld())
        return DENY_NOT_FOUND;
    if (!bot->GetSession() || !bot->GetSession()->GetBot())
        return DENY_NOT_BOT;
    AiBotAI* ai = BotAiOf(bot);
    if (!ai)
        return DENY_NOT_BOT;
    Group* group = possessor->GetGroup();
    bool factionAuthorized = SuiFactionControl::CanControl(possessor, bot);
    bool const partyAuthorized = group && group == bot->GetGroup();
    if (!partyAuthorized && !factionAuthorized)
        return DENY_NOT_IN_GROUP;
    if (!bot->GetPossessorGuid().IsEmpty())
        return DENY_BUSY;
    if (bot->IsDead() || bot->IsTaxiFlying() || bot->GetTransport() || bot->IsBeingTeleported())
        return DENY_TARGET_STATE;
    // Camera::SetView requires the target on the same map. Faction control can
    // explicitly relocate the owner's body outdoors; the client retries this
    // request only after normal world streaming has made the target visible.
    bool const sameMapInstance = bot->GetMapId() == possessor->GetMapId() &&
        bot->GetInstanceId() == possessor->GetInstanceId();
    bool const visible = sameMapInstance && possessor->IsInVisibleList(bot);
    if (!visible)
    {
        if (!factionAuthorized)
            return DENY_NOT_FOUND;
        SuiFactionControl::RelocateResult relocate =
            SuiFactionControl::TryRelocate(possessor, bot);
        if (relocate == SuiFactionControl::RELOCATE_INSTANCE_DENIED)
            return DENY_CROSS_INSTANCE;
        if (relocate == SuiFactionControl::RELOCATE_ACCEPTED)
        {
            if (grantedBot)
                *grantedBot = bot;
            return ACK_RELOCATING;
        }
        return DENY_NOT_FOUND;
    }

    // ── Grant ──
    // This request may originate from Commander free view (including the GM
    // diagnostic path). Retire its streaming eye only after every denial gate
    // has passed so a rejected request leaves the existing view untouched.
    RemoveFreecamEye(possessor);
    ai->SetPossessed(true);
    // Stop whatever the AI had it doing FIRST. A bot taken mid-stride keeps its movespline,
    // and HandleMovementOpcodes drops every client movement packet while one is unfinalized
    // (HandleMovementOpcodes: if movespline is not Finalized, return) — so the human would
    // drive a body that
    // ignores them and keeps walking to wherever the AI was already sending it. Since bots are
    // almost always following the party when you take them, that is the normal case, not an
    // edge one: it reads as "I move but nothing happens, and I snap back to the group".
    bot->StopMoving();
    bot->GetMotionMaster()->Clear(false, true);
    bot->GetMotionMaster()->MoveIdle();
    // StopMoving on a moving Player launches a stop spline, and Launch marks every
    // player spline "done pending" until a client confirms it — which this client
    // never will (MSUIClient does not speak CMSG_MOVE_SPLINE_DONE). Together with
    // any stale flag from the bot's AI era, that blocked HandleMovementOpcodes
    // wholesale: the human drove a body whose every packet was discarded. Clear
    // it LAST, after the stop above.
    bot->SetSplineDonePending(false);
    // The bot's brain-era journey does not survive a human taking the body: a
    // live TASK_MOVE_TO gates DoPartyFollow and resumes walking on release.
    ai->SuiAbandonJourney();
    // A half-open loot window would strand the loot session (loot is
    // player-scoped); mirror ModPossess's force-release.
    if (ObjectGuid lootGuid = bot->GetLootGuid())
        bot->GetSession()->DoLootRelease(lootGuid);

    possessor->GetCamera().SetView(bot);
    bot->SetPossessorGuid(possessor->GetObjectGuid());
    session->SetSuiControlledGuid(bot->GetObjectGuid());
    possessor->SetMover(bot);
    possessor->SetClientControl(bot, 1);

    // The abandoned real character follows and assists whoever the human drives.
    if (partyAuthorized)
        AttachUnattendedAI(possessor, bot);
    else
        ParkUnattendedBody(possessor);

    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI] %s now possesses bot %s",
        possessor->GetName(), bot->GetName());
    if (grantedBot)
        *grantedBot = bot;
    return ACK_OK;
}

/// Shared release path. Safe against a despawned bot or a tearing-down session.
// ── Freecam eye ──────────────────────────────────────────────────────────────
// The free view must LOAD what it overflies. Visibility/grid streaming follows
// the player's Camera, so while in the free view the camera rides an invisible
// World Trigger summon (active object: keeps its grid ticking) that the client
// repositions with CMSG_SUI_CAM. Torn down on every path that leaves the view.
static std::unordered_map<uint64, uint64> s_freecamEyes;

static Creature* FreecamEyeOf(Player* player)
{
    auto it = s_freecamEyes.find(player->GetObjectGuid().GetRawValue());
    if (it == s_freecamEyes.end())
        return nullptr;
    return player->GetMap()->GetCreature(ObjectGuid(it->second));
}

static void EnsureFreecamEye(Player* player)
{
    if (!player || !player->IsInWorld())
        return;
    if (Creature* existing = FreecamEyeOf(player))
    {
        player->GetCamera().SetView(existing);
        return;
    }
    Creature* eye = player->SummonCreature(15384 /* World Trigger, invisible model */,
        player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), 0.0f,
        TEMPSUMMON_MANUAL_DESPAWN, 0, true /* active object */);
    if (!eye)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[SUI] %s: freecam eye summon failed",
            player->GetName());
        return;
    }
    eye->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE);
    s_freecamEyes[player->GetObjectGuid().GetRawValue()] = eye->GetObjectGuid().GetRawValue();
    player->GetCamera().SetView(eye);
    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI] %s: freecam eye up", player->GetName());
}

static void RemoveFreecamEye(Player* player)
{
    if (!player)
        return;
    auto it = s_freecamEyes.find(player->GetObjectGuid().GetRawValue());
    if (it == s_freecamEyes.end())
        return;
    if (player->IsInWorld())
        if (Creature* eye = player->GetMap()->GetCreature(ObjectGuid(it->second)))
        {
            // Give the view back to whatever the player is actually looking through. Landing
            // out of the free view while still possessing must return it to the BOT — a blind
            // ResetView would point the camera at the abandoned body and stop streaming the
            // world around the character being driven.
            Player* stillDriving = nullptr;
            if (WorldSession* session = player->GetSession())
            {
                ObjectGuid driven = session->GetSuiControlledGuid();
                if (!driven.IsEmpty())
                    stillDriving = sObjectMgr.GetPlayer(driven);
            }
            if (stillDriving && stillDriving->IsInWorld())
                player->GetCamera().SetView(stillDriving);
            else
                player->GetCamera().ResetView();
            eye->AddObjectToRemoveList();
        }
    s_freecamEyes.erase(it);
}

static bool DoRelease(WorldSession* session, AckResult reason, bool serverInitiated)
{
    ObjectGuid botGuid = session->GetSuiControlledGuid();
    if (botGuid.IsEmpty())
        return false;
    session->SetSuiControlledGuid(ObjectGuid());

    Player* possessor = session->GetPlayer();
    if (Player* bot = sObjectMgr.GetPlayer(botGuid))
    {
        bot->SetPossessorGuid(ObjectGuid());
        if (AiBotAI* ai = BotAiOf(bot))
            ai->SetPossessed(false);
    }
    if (possessor)
    {
        possessor->GetCamera().ResetView();
        possessor->SetMover(nullptr);           // resolves to self
        possessor->SetClientControl(possessor, 1);
        // Manual control resumes — except into the free camera, where the own
        // character stays autonomous. Its anchor remains the just-released bot
        // (still a valid group member); it only re-points on the next possess.
        if (reason != RELEASED_FREECAM)
        {
            DetachUnattendedAI(possessor);
            RemoveFreecamEye(possessor);
        }
        else
            EnsureFreecamEye(possessor);
    }
    if (serverInitiated)
        // In-flight MSG_MOVE_* still carry bot coordinates; without the drain
        // window they would be attributed to the own character standing far
        // away (GetConfirmedMover fallback) and trip anticheat/teleport.
        session->RejectMovementPacketsFor(1000);

    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI] %s released bot %s (reason %u)",
        possessor ? possessor->GetName() : "<logging out>",
        botGuid.GetString().c_str(), uint32(reason));

    SendAck(session, possessor ? possessor->GetObjectGuid() : ObjectGuid(), reason, possessor);
    if (possessor && possessor->GetGroup())
        BroadcastRoster(possessor->GetGroup());
    return true;
}

void HandleRequest(WorldSession* session, ObjectGuid targetGuid)
{
    session->SetSuiCapable(true);
    Player* bot = nullptr;
    AckResult result = TryBegin(session, targetGuid, &bot);
    SendAck(session, targetGuid, result, bot);
    if (result == ACK_OK && bot)
    {
        // Owner data follows the grant, deliberately routed through the BOT's
        // own (socket-less) session: the SendPacket mirror wraps each packet in
        // SMSG_SUI_PROXY with the right source guid. Mid-session re-sends of
        // both packets are proven safe in this fork (SpecCommands .testbars).
        bot->SendInitialSpells();
        if (MasterPlayer* master = bot->GetSession()->GetMasterPlayer())
            master->SendInitialActionButtons();
        SendSnapshot(session, bot);
        if (Group* group = session->GetPlayer()->GetGroup())
            BroadcastRoster(group);
    }
}

bool IsCommandedFromFreeView(Unit const* bot)
{
    Player* possessor = GetPossessor(bot);
    return possessor != nullptr && FreecamEyeOf(possessor) != nullptr;
}

bool IsFreeViewUp(Player* player)
{
    return player != nullptr && FreecamEyeOf(player) != nullptr;
}

bool PrepareForRelocation(Player* player)
{
    bool const restoreFreeView = FreecamEyeOf(player) != nullptr;
    RemoveFreecamEye(player);
    return restoreFreeView;
}

void RestoreAfterFailedRelocation(Player* player, bool restoreFreeView)
{
    if (restoreFreeView)
        EnsureFreecamEye(player);
}

void HandleCam(WorldSession* session, float x, float y, float z, bool active)
{
    Player* player = session->GetPlayer();
    if (!player || !player->IsInWorld())
        return;
    // The free view came down. Drop the eye — which also ends IsCommandedFromFreeView, handing
    // a possessed bot back to the client that is once again really driving it.
    if (!active)
    {
        // Landing. Tear the eye down FIRST so the commanded-remotely waiver is
        // already off when the stop below finalizes the bot's spline — otherwise
        // MovementInform still reads "commanded" and chains the next task leg
        // under the feet of the client that is about to really drive it.
        RemoveFreecamEye(player);
        // The command era walked the bot by server splines this client never
        // confirms; stop it where it stands and drop the pending flag, or every
        // movement packet from its returning driver is silently discarded.
        if (Player* bot = GetControlledBot(session))
        {
            bot->StopMoving();
            bot->GetMotionMaster()->Clear(false, true);
            bot->GetMotionMaster()->MoveIdle();
            bot->SetSplineDonePending(false);
        }
        return;
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        return;
    // ENSURE, not just move. HandleRequest tears the eye down ("possess overrides the
    // free-camera view"), which was true while possessing meant leaving the free view — it
    // does not any more: the client now commands a toon with the camera still up, and kept
    // flying with nothing streaming the world in around it. The client only ever sends
    // CMSG_SUI_CAM while its free view is up, so rebuilding here makes the eye's existence
    // track the client's real camera mode instead of its possession state.
    EnsureFreecamEye(player);
    if (Creature* eye = FreecamEyeOf(player))
        eye->NearTeleportTo(x, y, z, 0.0f);
}

void HandleRelease(WorldSession* session, uint8 mode)
{
    session->SetSuiCapable(true);
    AckResult reason = mode == RELEASE_TO_FREECAM ? RELEASED_FREECAM : RELEASED;
    if (!DoRelease(session, reason, false))
    {
        // Nothing possessed: the freecam enter/leave path from the own character.
        if (Player* player = session->GetPlayer())
        {
            if (mode == RELEASE_TO_FREECAM)
            {
                Player* anchor = nullptr;
                if (Group* group = player->GetGroup())
                    anchor = sObjectMgr.GetPlayer(group->GetLeaderGuid());
                AttachUnattendedAI(player, anchor && anchor != player ? anchor : nullptr);
                EnsureFreecamEye(player);
            }
            else
            {
                DetachUnattendedAI(player);
                RemoveFreecamEye(player);
            }
        }
        // Ack so the client state machine resolves either way.
        SendAck(session, session->GetPlayer() ? session->GetPlayer()->GetObjectGuid() : ObjectGuid(),
            reason, session->GetPlayer());
    }
}

void ForceRelease(WorldSession* session, AckResult reason)
{
    DoRelease(session, reason, true);
}

// One member, one order. Split out of HandleOrder's lambda (2026-08-20) so the
// stock-client `.sui order` chat command can reach the same actuation WITHOUT going
// through HandleOrder — that entry point calls SetSuiCapable, which is correct for a
// CMSG_SUI_ORDER packet (the client just proved it speaks the custom protocol) and
// wrong for a chat line (a stock client would then be sent SMSG_SUI_* it cannot parse).
// Authority checks live here, so both callers get them.
void OrderOne(Player* player, Player* pMember, uint8 orderType,
    ObjectGuid targetGuid, float x, float y, float z)
{
    if (!player || !pMember)
        return;

    // Solo is legal: the unattended own character (freecam with no party) must
    // obey RTS orders too — a group only widens the orderable set. This gate
    // silently ate every order a partyless owner clicked from the free view.
    Group* group = player->GetGroup();

    {   // Body kept at its original indentation: this was HandleOrder's orderBot lambda
        // verbatim, and de-indenting it would bury a pure move in a whitespace diff.
        // In a party: subjects must be members. Solo (group null): ONLY the own
        // character — matching on GetGroup() alone would let a partyless session
        // order any ungrouped AI-attached body on the server.
        if (group ? pMember->GetGroup() != group : pMember != player)
            return;
        // AI-attached is the real gate: fabricated bots always are; the human's
        // own character only while unattended (possession/freecam), which is
        // exactly when it must obey RTS orders alongside the bots. A manually
        // driven character has no AiBotAI, and the possessed bot stays excluded.
        AiBotAI* ai = dynamic_cast<AiBotAI*>(pMember->AI());
        if (!ai)
            return;
        if (ai->IsPossessed())
        {
            // Normally excluded: possession makes the CLIENT the bot's mover, and a
            // server-side MOVE_TO would fight the movement stream coming the other way.
            // From the FREE VIEW that conflict cannot arise — the client's controller is the
            // detached camera, its movement stream is parked, and the possessed bot receives
            // no client input at all. So the toon you are commanding stays orderable, which
            // is the whole point of clicking it: halo, bars, and a right-click that moves it.
            // The freecam eye is the server's evidence of that camera mode (the client sends
            // CMSG_SUI_CAM only while the free view is up, and HandleCam keeps the eye alive).
            Player* possessor = GetPossessor(pMember);
            if (!possessor || !FreecamEyeOf(possessor))
                return;
        }

        // Reuse the bridge command paths verbatim — ordered behaviour is then
        // bit-identical to a brain-issued MOVE_TO / ATTACK_TARGET, including the
        // chunked pathfinding and the in-combat MOVE_TO deferral. The PlayerParty
        // escort loop yields to an active TASK_MOVE_TO, so orders are not
        // formation-snapped on the next tick.
        char json[192];
        switch (orderType)
        {
            case ORDER_MOVE:
                ai->SuiClearWaypoints();   // a plain move order replaces any chain
                snprintf(json, sizeof(json),
                    "{\"type\":\"MOVE_TO\",\"payload\":{\"mapId\":%u,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}}",
                    pMember->GetMapId(), x, y, z);
                ai->BridgeProcessLine(json);
                break;
            case ORDER_ATTACK:
                if (targetGuid.IsCreature())
                {
                    // Carry the ENTRY too. A vmangos creature ObjectGuid is
                    // (HIGHGUID_UNIT, entry, counter) and Map::GetCreature matches on all
                    // three, so a counter-only payload can never be resolved on the far side
                    // — every RTS attack order died as "creature guid N not found on map".
                    snprintf(json, sizeof(json),
                        "{\"type\":\"ATTACK_TARGET\",\"payload\":{\"guid\":%u,\"entry\":%u}}",
                        targetGuid.GetCounter(), targetGuid.GetEntry());
                    ai->BridgeProcessLine(json);
                }
                break;
            case ORDER_MOVE_QUEUE:
                ai->SuiQueueWaypoint(x, y, z);
                break;
            case ORDER_FOLLOW:
            {
                // Portrait drag chain: this bot escorts the named group member.
                // Rides the [FOLLOW-CMD] escort override, whose resolution is
                // case-insensitive and falls back to the auto split when the
                // name never resolves — so an empty/unknown target just clears.
                Player* followTarget = targetGuid.IsEmpty() ? nullptr
                    : sObjectMgr.GetPlayer(targetGuid);
                snprintf(json, sizeof(json),
                    "{\"type\":\"SET_ESCORT\",\"payload\":{\"player_name\":\"%s\"}}",
                    followTarget ? followTarget->GetName() : "");
                ai->BridgeProcessLine(json);
                break;
            }
            case ORDER_LINK:
                // Divinity-style chain toggle. Linked members keep formation on the
                // driven character; an unlinked member stands its ground from here.
                ai->m_suiUnlinked = x < 0.5f;
                if (ai->m_suiUnlinked)
                {
                    ai->StopMoving();
                    pMember->GetMotionMaster()->Clear(false, true);
                    pMember->GetMotionMaster()->MoveIdle();
                }
                break;
            case ORDER_PATROL:
                // Convert the queued chain into a loop. The coordinate in the
                // packet joins the route as its final point (a bare patrol
                // click with no chain gives a two-point there-and-back).
                ai->SuiQueueWaypoint(x, y, z);
                ai->m_suiPatrolLoop = true;
                break;
            case ORDER_STOP:
                ai->SuiClearWaypoints();
                ai->StopMoving();
                pMember->AttackStop();
                ai->m_currentTask.type = TASK_IDLE;
                pMember->GetMotionMaster()->MoveIdle();
                ai->ClearPendingCast();   // an explicit stop cancels a cast still closing in
                break;
            case ORDER_CAST:
            {
                // Spell id rides in `x` (see OrderType in SuiPossess.h). An empty
                // targetGuid means "the commander's own target", which is what a hotbar
                // press without an explicit subject should do; OrderCast falls back to
                // the member itself when there is no target at all, so self-buffs and
                // heals work from the same button.
                Unit* castTarget = nullptr;
                if (!targetGuid.IsEmpty())
                    castTarget = pMember->GetMap()->GetUnit(targetGuid);
                else if (ObjectGuid sel = player->GetSelectionGuid())
                    castTarget = pMember->GetMap()->GetUnit(sel);
                OrderCast(player, pMember, uint32(x), castTarget);
                break;
            }
            default:
                break;
        }
    }
}

void HandleOrder(WorldSession* session, uint8 orderType,
    std::vector<ObjectGuid> const& subjects, ObjectGuid targetGuid,
    float x, float y, float z)
{
    // The session just sent CMSG_SUI_ORDER, so it demonstrably speaks the custom
    // protocol and may be sent SMSG_SUI_*. Only this packet-driven entry point may
    // conclude that — see the note on OrderOne.
    session->SetSuiCapable(true);
    Player* player = session->GetPlayer();
    if (!player || session->GetBot())
        return;

    Group* group = player->GetGroup();

    if (!subjects.empty())
        for (ObjectGuid guid : subjects)
            OrderOne(player, sObjectMgr.GetPlayer(guid), orderType, targetGuid, x, y, z);
    else if (group)
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            OrderOne(player, itr->getSource(), orderType, targetGuid, x, y, z);
    else
        OrderOne(player, player, orderType, targetGuid, x, y, z);   // empty subject list solo = the own character
}

// ── Hooks ────────────────────────────────────────────────────────────────────

void OnPlayerRemovedFromGroup(Player* player)
{
    if (!player)
        return;
    if (Player* possessor = GetPossessor(player))
        ForceRelease(possessor->GetSession(), RELEASED_GROUP);
    else if (player->GetSession() && !player->GetSession()->GetSuiControlledGuid().IsEmpty())
        ForceRelease(player->GetSession(), RELEASED_GROUP);
}

void OnPlayerTeleport(Player* player)
{
    if (!player)
        return;
    if (Player* possessor = GetPossessor(player))
        ForceRelease(possessor->GetSession(), RELEASED_TELEPORT);
    else if (player->GetSession() && !player->GetSession()->GetSuiControlledGuid().IsEmpty())
        ForceRelease(player->GetSession(), RELEASED_TELEPORT);
}

void OnPlayerDeath(Player* player)
{
    // Only the possessed bot's death breaks possession; the possessor's own
    // character dying under AI is surfaced client-side, not force-released.
    if (Player* possessor = GetPossessor(player))
        ForceRelease(possessor->GetSession(), RELEASED_DEATH);
}

void OnLogout(WorldSession* session)
{
    // Session was possessing someone → clean release.
    DoRelease(session, RELEASED_LOGOUT, true);
    if (Player* player = session->GetPlayer())
    {
        // Freecam logout: the unattended AI is owned here, not by a bot entry —
        // reclaim it before Player teardown.
        DetachUnattendedAI(player);
        RemoveFreecamEye(player);
        // Session's player IS a possessed bot (bot despawn path) → release its human.
        if (Player* possessor = GetPossessor(player))
            ForceRelease(possessor->GetSession(), RELEASED_LOGOUT);
    }
}

// ── Roster ───────────────────────────────────────────────────────────────────

void SendRoster(Player* realPlayer)
{
    WorldSession* session = realPlayer->GetSession();
    if (!session || session->GetBot() || !session->IsSuiCapable())
        return;

    WorldPacket data(SMSG_SUI_CONTROL_ROSTER, 1 + 9 * MAX_RAID_SIZE);
    Group* group = realPlayer->GetGroup();
    if (!group)
    {
        data << uint8(0);
        session->SendPacket(&data);
        return;
    }

    size_t countPos = data.wpos();
    data << uint8(0);
    uint8 count = 0;
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->getSource();
        if (!member || !member->IsInWorld())
            continue;
        uint8 flags = 0;
        if (member->GetSession() && member->GetSession()->GetBot() && BotAiOf(member))
            flags |= ROSTER_CONTROLLABLE;
        if (IsSuiPossessed(member))
            flags |= ROSTER_POSSESSED;
        data << uint64(member->GetObjectGuid().GetRawValue());
        data << flags;
        ++count;
    }
    data.put<uint8>(countPos, count);
    session->SendPacket(&data);
}

void BroadcastRoster(Group* group)
{
    if (!group)
        return;
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->getSource();
        if (member && member->GetSession() && !member->GetSession()->GetBot())
            SendRoster(member);
    }
}

// ── Ordered casting ──────────────────────────────────────────────────────────

// How long a member keeps trying to close on a target that was merely out of range or
// behind a corner before the order is declared failed. Long enough to cross a room,
// short enough that a bad order does not have the member jogging after a fleeing mob.
#define SUI_CAST_RETRY_BUDGET_MS 5000

char const* CastResultText(SpellCastResult result)
{
    switch (result)
    {
        case SPELL_FAILED_OUT_OF_RANGE:       return "out of range";
        case SPELL_FAILED_LINE_OF_SIGHT:      return "no line of sight";
        case SPELL_FAILED_NO_POWER:           return "not enough power";
        case SPELL_FAILED_NOT_READY:          return "on cooldown";
        case SPELL_FAILED_UNIT_NOT_INFRONT:   return "target not in front";
        case SPELL_FAILED_BAD_TARGETS:        return "invalid target";
        case SPELL_FAILED_BAD_IMPLICIT_TARGETS: return "invalid target";
        case SPELL_FAILED_SPELL_IN_PROGRESS:  return "already casting";
        case SPELL_FAILED_MOVING:             return "moving";
        case SPELL_FAILED_CASTER_AURASTATE:   return "cannot cast right now";
        case SPELL_FAILED_TARGET_AURASTATE:   return "target state prevents it";
        case SPELL_FAILED_IMMUNE:             return "target is immune";
        case SPELL_FAILED_INTERRUPTED:        return "interrupted";
        case SPELL_FAILED_TARGET_AFFECTING_COMBAT: return "target is in combat";
        default:                              return nullptr;   // caller prints the code
    }
}

static void ReportCastResult(Player* commander, Player* member, uint32 spellId,
    SpellCastResult result)
{
    if (!commander || !commander->GetSession())
        return;

    ChatHandler handler(commander);
    if (char const* text = CastResultText(result))
        handler.PSendSysMessage("[SUI] %s: %s (spell %u)", member->GetName(), text, spellId);
    else
        handler.PSendSysMessage("[SUI] %s: cast failed, code %u (spell %u)",
            member->GetName(), uint32(result), spellId);
}

void OrderCast(Player* commander, Player* member, uint32 spellId, Unit* target)
{
    if (!commander || !member || !member->IsInWorld())
        return;

    ChatHandler handler(commander);

    if (!spellId || !sSpellMgr.GetSpellEntry(spellId))
    {
        handler.PSendSysMessage("[SUI] spell %u does not exist.", spellId);
        return;
    }

    // The hotbar only ever offers spells the member actually knows, so an unknown spell
    // here means a stale addon catalog (levelled since the last download) or a typo.
    // Refusing is honest; casting it triggered would be inventing an ability.
    if (!member->HasSpell(spellId))
    {
        handler.PSendSysMessage("[SUI] %s does not know spell %u.", member->GetName(), spellId);
        return;
    }

    if (!member->IsAlive())
    {
        handler.PSendSysMessage("[SUI] %s is dead.", member->GetName());
        return;
    }

    if (!target)
        target = member;

    if (!target->IsInWorld() || target->GetMapId() != member->GetMapId())
    {
        handler.PSendSysMessage("[SUI] %s: target is not here.", member->GetName());
        return;
    }

    AiBotAI* ai = dynamic_cast<AiBotAI*>(member->AI());

    // A new order supersedes whatever the member was still trying to land.
    if (ai)
        ai->ClearPendingCast();

    // Face the target and stop: a moving caster fails CheckCast on anything with a cast
    // time, and a mis-faced one fails UNIT_NOT_INFRONT. Doing this before the attempt is
    // the difference between "your companion tried" and "your companion refused".
    if (target != member)
        member->SetFacingToObject(target);
    if (member->IsMoving())
        member->StopMoving();

    SpellCastResult result = member->CastSpell(target, spellId, false);
    if (result == SPELL_CAST_OK)
        return;

    // Range and LOS are positional, and position is the one thing the member can fix by
    // itself. Everything else is a real refusal and is reported now.
    if ((result == SPELL_FAILED_OUT_OF_RANGE || result == SPELL_FAILED_LINE_OF_SIGHT) && ai)
    {
        ai->ArmPendingCast(spellId, target->GetObjectGuid(), commander->GetObjectGuid(),
            SUI_CAST_RETRY_BUDGET_MS);

        char json[192];
        snprintf(json, sizeof(json),
            "{\"type\":\"MOVE_TO\",\"payload\":{\"mapId\":%u,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}}",
            target->GetMapId(), target->GetPositionX(), target->GetPositionY(),
            target->GetPositionZ());
        ai->BridgeProcessLine(json);   // owner order, not brain: un-gated by design

        handler.PSendSysMessage("[SUI] %s: %s — closing in.",
            member->GetName(), CastResultText(result));
        return;
    }

    ReportCastResult(commander, member, spellId, result);
}

} // namespace SuiPossess

// ── Owner-data mirror (M3) + inventory/talent snapshot (M4) ──────────────────

namespace SuiPossess
{

static void AppendSnapshotItem(WorldPacket& data, uint8 bag, uint8 slot, Item* item)
{
    // The client queries these templates the moment the snapshot lands, and the
    // anti-datamining gate (HandleItemQuerySingleOpcode: ItemPrototype::Discovered)
    // answers "no such item" for anything neither the startup scan nor a runtime
    // Item::Create has marked — fabricated bots' gear can be exactly that after a
    // restart. The client caches the refusal permanently: nameless icon-less bags
    // with working stack counts. An item in a possessed party member's bags is
    // discovered by any honest reading of the rule.
    if (ItemPrototype const* proto = item->GetProto())
        proto->Discovered = true;
    data << uint8(bag);
    data << uint8(slot);
    data << uint64(item->GetObjectGuid().GetRawValue());
    data << uint32(item->GetEntry());
    data << uint32(item->GetCount());
    uint8 bagSlots = 0;
    if (ItemPrototype const* proto = item->GetProto())
        if (proto->Class == ITEM_CLASS_CONTAINER)
            bagSlots = (uint8)((Bag*)item)->GetBagSize();
    data << bagSlots;
}

/// Read-only bags + talent points for the possessed bot, pushed once per grant.
/// bag 255 = character-held (equipment 0-18, bag slots 19-22, backpack 23-38,
/// keyring 81+ — one contiguous slot numbering); bag 19-22 = inside that
/// equipped bag. Bag rows precede their contents by construction.
static void SendSnapshot(WorldSession* to, Player* bot)
{
    if (!to->IsSuiCapable())
        return;
    WorldPacket data(SMSG_SUI_SNAPSHOT, 4096);
    data << uint64(bot->GetObjectGuid().GetRawValue());
    data << uint32(bot->GetUInt32Value(PLAYER_CHARACTER_POINTS1));
    data << uint32(bot->GetMoney());
    size_t countPos = data.wpos();
    data << uint16(0);
    uint16 count = 0;

    for (uint8 i = EQUIPMENT_SLOT_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            AppendSnapshotItem(data, 255, i, item);
            ++count;
        }
    for (uint8 i = KEYRING_SLOT_START; i < KEYRING_SLOT_END; ++i)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            AppendSnapshotItem(data, 255, i, item);
            ++count;
        }
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Item* bagItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot))
            if (ItemPrototype const* proto = bagItem->GetProto())
                if (proto->Class == ITEM_CLASS_CONTAINER)
                {
                    Bag* pBag = (Bag*)bagItem;
                    for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
                        if (Item* item = pBag->GetItemByPos((uint8)j))
                        {
                            AppendSnapshotItem(data, bagSlot, (uint8)j, item);
                            ++count;
                        }
                }

    data.put<uint16>(countPos, count);

    // ── Snapshot v2: the paper-doll stat block. These UNIT_FIELD values are
    // owner-only on the vanilla wire (never streamed for another player), so a
    // possessed bot's character sheet rendered all zeros without them. Raw field
    // values, mirrored verbatim into the same fields client-side; the client
    // reads the block only when present, so the growth is compatible both ways.
    for (int i = 0; i < 5; ++i)
        data << bot->GetUInt32Value(UNIT_FIELD_STAT0 + i);
    for (int i = 0; i < 7; ++i)
        data << bot->GetUInt32Value(UNIT_FIELD_RESISTANCES + i);
    data << bot->GetUInt32Value(UNIT_FIELD_ATTACK_POWER);
    data << bot->GetUInt32Value(UNIT_FIELD_ATTACK_POWER_MODS);
    data << bot->GetUInt32Value(UNIT_FIELD_RANGED_ATTACK_POWER);
    data << bot->GetUInt32Value(UNIT_FIELD_RANGED_ATTACK_POWER_MODS);
    data << bot->GetUInt32Value(UNIT_FIELD_BASEATTACKTIME);
    data << bot->GetUInt32Value(UNIT_FIELD_BASEATTACKTIME + 1);   // offhand
    data << bot->GetUInt32Value(UNIT_FIELD_RANGEDATTACKTIME);
    data << bot->GetFloatValue(UNIT_FIELD_MINDAMAGE);
    data << bot->GetFloatValue(UNIT_FIELD_MAXDAMAGE);
    data << bot->GetFloatValue(UNIT_FIELD_MINOFFHANDDAMAGE);
    data << bot->GetFloatValue(UNIT_FIELD_MAXOFFHANDDAMAGE);
    data << bot->GetFloatValue(UNIT_FIELD_MINRANGEDDAMAGE);
    data << bot->GetFloatValue(UNIT_FIELD_MAXRANGEDDAMAGE);

    to->SendPacket(&data);
}

void MirrorOwnerPacket(WorldSession* botSession, WorldPacket const* packet)
{
    // Whitelist first: this sits on every socket-less SendPacket, keep it cheap.
    // Mirroring everything would double-deliver broadcasts the possessor already
    // receives; these are the strictly owner-only spell/bar/cooldown packets.
    switch (packet->GetOpcode())
    {
        case SMSG_ACTION_BUTTONS:
        case SMSG_INITIAL_SPELLS:
        case SMSG_LEARNED_SPELL:
        case SMSG_SUPERCEDED_SPELL:
        case SMSG_REMOVED_SPELL:
        case SMSG_SPELL_COOLDOWN:
        case SMSG_COOLDOWN_EVENT:
        case SMSG_CLEAR_COOLDOWN:
        case SMSG_CAST_RESULT:
            break;
        default:
            return;
    }

    Player* bot = botSession->GetPlayer();
    if (!bot)
        return;
    Player* possessor = GetPossessor(bot);
    if (!possessor || !possessor->GetSession() || !possessor->GetSession()->IsSuiCapable())
        return;

    WorldPacket data(SMSG_SUI_PROXY, 8 + 2 + packet->size());
    data << uint64(bot->GetObjectGuid().GetRawValue());
    data << uint16(packet->GetOpcode());
    if (packet->size() > 0)
        data.append(packet->contents(), packet->size());
    possessor->GetSession()->SendPacket(&data);
}

} // namespace SuiPossess

// ── Zone intel (commander map) ────────────────────────────────────────────────

void SuiPossess::HandleZoneIntel(WorldSession* session, uint8 /*flags*/)
{
    // Any CMSG_SUI_* proves the sender is MSUIClient (same opportunistic mark
    // as the other handlers); the reply below goes only to the asker.
    session->SetSuiCapable(true);
    Player* requester = session->GetPlayer();
    if (!requester || !requester->IsInWorld() || session->GetBot())
        return;

    // Census: every in-world player bucketed by CACHED zone id. GetCachedZoneId
    // is a plain field read at most one zone-tick stale; WorldObject::GetZoneId
    // is a terrain query and must never run in this loop.
    std::unordered_map<uint32, std::pair<uint16, uint16>> census;   // zone -> {bots, players}
    {
        HashMapHolder<Player>::ReadGuard g(HashMapHolder<Player>::GetLock());
        for (auto const& itr : sObjectAccessor.GetPlayers())
        {
            Player* p = itr.second;
            if (!p || !p->IsInWorld())
                continue;
            uint32 zone = p->GetCachedZoneId();
            if (!zone)
                continue;                    // mid-login, zone not resolved yet
            auto& c = census[zone];
            uint16& bucket = p->IsBot() ? c.first : c.second;
            if (bucket < 0xFFFF)
                ++bucket;                    // unattended real characters count as players
        }
    }

    // The asker's own forces: self + every in-world group member, with live
    // positions. The client has no position data for unstreamed members — this
    // block is what places the unit markers on the zone map.
    std::vector<Player*> units;
    units.push_back(requester);
    if (Group* group = requester->GetGroup())
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->getSource();
            if (member && member->IsInWorld() && member != requester)
                units.push_back(member);
        }
    if (units.size() > 250)
        units.resize(250);                   // u8 count; far above any raid size

    // Both blocks carry an explicit row stride so a future server can append
    // per-row facts while an older client skips them (see SUI_WIRE_PROTOCOL.md).
    WorldPacket data(SMSG_SUI_ZONE_INTEL, 3 + census.size() * 9 + 2 + units.size() * 29);
    data << uint16(census.size());
    data << uint8(9);                        // zone row stride (R1: +controller byte)
    for (auto const& kv : census)
    {
        data << uint32(kv.first);
        data << uint16(kv.second.first);
        data << uint16(kv.second.second);
        data << uint8(0);                    // controller (0 until territory R3; 0x80 = contested)
    }
    data << uint8(units.size());
    data << uint8(29);                       // unit row stride
    for (Player* u : units)
    {
        uint8 unitFlags = 0;
        if (u->IsAlive()) unitFlags |= 1;
        if (u->IsBot())   unitFlags |= 2;
        data << uint64(u->GetObjectGuid().GetRawValue());
        data << uint32(u->GetMapId());
        data << uint32(u->GetCachedZoneId());
        data << float(u->GetPositionX());
        data << float(u->GetPositionY());
        data << float(u->GetPositionZ());
        data << unitFlags;
    }
    session->SendPacket(&data);
}

// ── Wire handlers ────────────────────────────────────────────────────────────

Player* WorldSession::GetSuiActor()
{
    // The unit the session's gameplay input acts as: the possessed bot while
    // driving one, else the session's own player. Threaded through the
    // cast/target/melee handler family — bit-identical when not possessing.
    if (!m_suiControlledGuid.IsEmpty())
        if (Player* bot = SuiPossess::GetControlledBot(this))
            return bot;
    return _player;
}

void WorldSession::HandleSuiControlRequestOpcode(WorldPackets::SuiControl::ControlRequest const& packet)
{
    SuiPossess::HandleRequest(this, packet.targetGuid);
}

void WorldSession::HandleSuiControlReleaseOpcode(WorldPackets::SuiControl::ControlRelease const& packet)
{
    SuiPossess::HandleRelease(this, packet.mode);
}

void WorldSession::HandleSuiOrderOpcode(WorldPackets::SuiControl::Order const& packet)
{
    SuiPossess::HandleOrder(this, packet.orderType, packet.subjects,
        packet.targetGuid, packet.x, packet.y, packet.z);
}

void WorldSession::HandleSuiCamOpcode(WorldPackets::SuiControl::Cam const& packet)
{
    SuiPossess::HandleCam(this, packet.x, packet.y, packet.z, packet.active != 0);
}

void WorldSession::HandleSuiZoneIntelOpcode(WorldPackets::SuiControl::ZoneIntel const& packet)
{
    SuiPossess::HandleZoneIntel(this, packet.flags);
}

// ── GM commands (stock-client testable: .sui possess <name> / .sui release) ──

bool ChatHandler::HandleSuiWorldStateCommand(char* args)
{
    if (char* arg = ExtractLiteralArg(&args))
    {
        PSendSysMessage("SUI worldstate is boot-latched; '%s' was not applied.", arg);
        return true;
    }
    PSendSysMessage("SUI worldstate: %s",
        SuiWorldState::RtsWorldState() ? "RTS MATCH" : "vanilla");
    return true;
}

bool ChatHandler::HandleSuiPossessCommand(char* args)
{
    Player* requester = m_session ? m_session->GetPlayer() : nullptr;
    if (!requester)
        return false;

    ObjectGuid targetGuid;
    if (char* name = ExtractLiteralArg(&args))
    {
        std::string playerName = name;
        if (Player* target = sObjectMgr.GetPlayer(playerName.c_str()))
            targetGuid = target->GetObjectGuid();
    }
    else if (Player* selected = GetSelectedPlayer())
        targetGuid = selected->GetObjectGuid();

    if (targetGuid.IsEmpty())
    {
        SendSysMessage("[SUI] usage: .sui possess <botname> (or select the bot)");
        SetSentErrorMessage(true);
        return false;
    }

    Player* bot = nullptr;
    SuiPossess::AckResult result = SuiPossess::TryBegin(m_session, targetGuid, &bot);
    if (result == SuiPossess::ACK_OK)
        PSendSysMessage("[SUI] possessing %s — WASD drives the bot, .sui release to stop",
            bot ? bot->GetName() : "?");
    else
        PSendSysMessage("[SUI] possess denied (code %u)", uint32(result));
    return true;
}

bool ChatHandler::HandleSuiReleaseCommand(char* /*args*/)
{
    if (!m_session)
        return false;
    if (m_session->GetSuiControlledGuid().IsEmpty())
    {
        SendSysMessage("[SUI] not possessing anything");
        return true;
    }
    // Voluntary GM release still opens the drain window: a stock client has no
    // pending-state machine parking its movement stream first.
    SuiPossess::ForceRelease(m_session, SuiPossess::RELEASED);
    SendSysMessage("[SUI] released");
    return true;
}

// ── Companions (.sui companion add|remove|list) ──────────────────────────────
//
// A companion is one of the OWNER'S OWN characters run headless beside them: real
// gear, real talents, real quest log, real progression. It is NOT a fabricated bot
// and is deliberately never written to the `playerbot` registry — that table is the
// fabricated-bot roster, and a real character in it would be respawned on a synthetic
// account after a restart (the Tesfff failure, see the wall in AiBotAI::OnSessionLoaded).
// Enrolment is therefore runtime-only and re-issued after a server restart.
//
// Behaviour comes free: once the companion is in a group with a real player,
// ResolveDoctrine selects PlayerParty — assist the human's target, defend the party,
// run the class rotation, and never initiate a pull. Authorship is what the companion
// flag changes: STATE carries companion:1 and the C# brain never plans for it.

bool ChatHandler::HandleSuiCompanionAddCommand(char* args)
{
    Player* owner = m_session ? m_session->GetPlayer() : nullptr;
    if (!owner)
        return false;

    char* name = ExtractLiteralArg(&args);
    if (!name || !*name)
    {
        SendSysMessage("[SUI] usage: .sui companion add <charactername>");
        SetSentErrorMessage(true);
        return false;
    }

    std::string charName = name;
    ObjectGuid guid = sObjectMgr.GetPlayerGuidByName(charName);
    if (guid.IsEmpty())
    {
        PSendSysMessage("[SUI] no character named '%s'.", charName.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    uint32 const lowGuid = guid.GetCounter();

    if (ObjectAccessor::FindPlayer(guid))
    {
        PSendSysMessage("[SUI] %s is already in the world.", charName.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    if (PlayerBotEntry* existing = sPlayerBotMgr.GetBotEntry(lowGuid))
    {
        if (existing->state != PB_STATE_OFFLINE)
        {
            PSendSysMessage("[SUI] %s is already loaded as a bot.", charName.c_str());
            SetSentErrorMessage(true);
            return false;
        }
    }

    PlayerCacheData const* cache = sObjectMgr.GetPlayerDataByGUID(lowGuid);
    if (!cache)
    {
        PSendSysMessage("[SUI] no cached data for '%s'.", charName.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    // One account carries one session, so a companion cannot live on the account you
    // are playing. This is the single most common way the command fails; say so plainly
    // rather than letting AddBot fail with a log line the player never sees.
    if (cache->uiAccount == m_session->GetAccountId())
    {
        PSendSysMessage("[SUI] %s is on the account you are logged into. "
            "A companion needs its own account — one account carries one session.",
            charName.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    // Spawn params are inert on this path: the character exists, so OnSessionLoaded
    // takes the restart branch and LoginPlayer restores its real position and stats.
    // They are passed truthfully anyway so any future first-spawn branch cannot lie.
    AiBotAI* ai = new AiBotAI(uint8(cache->uiRace), uint8(cache->uiClass), cache->uiLevel,
        cache->uiMapId, 0, cache->fPosX, cache->fPosY, cache->fPosZ, cache->fOrientation);
    ai->SetCompanion(true);
    ai->SetCompanionOwner(owner->GetObjectGuid());

    if (!sPlayerBotMgr.AddBot(lowGuid, false, ai, true))
    {
        delete ai;
        PSendSysMessage("[SUI] could not load %s — is its account online?", charName.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    PSendSysMessage("[SUI] %s enrolled as a companion; it will join your party on login.",
        charName.c_str());
    return true;
}

bool ChatHandler::HandleSuiCompanionRemoveCommand(char* args)
{
    if (!m_session || !m_session->GetPlayer())
        return false;

    char* name = ExtractLiteralArg(&args);
    if (!name || !*name)
    {
        SendSysMessage("[SUI] usage: .sui companion remove <charactername>");
        SetSentErrorMessage(true);
        return false;
    }

    std::string charName = name;
    ObjectGuid guid = sObjectMgr.GetPlayerGuidByName(charName);
    if (guid.IsEmpty())
    {
        PSendSysMessage("[SUI] no character named '%s'.", charName.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    PlayerBotEntry* entry = sPlayerBotMgr.GetBotEntry(guid.GetCounter());
    if (!entry || !entry->isCompanion)
    {
        PSendSysMessage("[SUI] %s is not an active companion.", charName.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    // The manager's Update loop owns teardown: it pulls the body from the group and
    // logs the session out through IsSavingAllowed(entry), which is true for a
    // companion regardless of PlayerBot.AllowSaving. That is the path that persists
    // the session's XP, loot and quest progress — do not shortcut it.
    entry->requestRemoval = true;
    PSendSysMessage("[SUI] %s is logging out (progress will be saved).", charName.c_str());
    return true;
}

bool ChatHandler::HandleSuiCompanionListCommand(char* /*args*/)
{
    Player* owner = m_session ? m_session->GetPlayer() : nullptr;

    std::vector<PlayerBotEntry*> companions;
    sPlayerBotMgr.GetCompanions(companions);

    if (companions.empty())
    {
        SendSysMessage("[SUI] no companions online.");
        return true;
    }

    for (PlayerBotEntry* entry : companions)
    {
        Player* body = entry->ai ? entry->ai->me : nullptr;
        if (!body)
        {
            PSendSysMessage("  guid %u — loading", uint32(entry->playerGUID));
            continue;
        }

        char const* focus = "idle";
        if (Unit* victim = body->GetVictim())
            focus = victim->GetName();

        if (owner && owner->IsInWorld() && body->IsInWorld() && body->GetMapId() == owner->GetMapId())
            PSendSysMessage("  %s — level %u, %.0fyd away, %s",
                body->GetName(), body->GetLevel(), owner->GetDistance(body), focus);
        else
            PSendSysMessage("  %s — level %u, %s", body->GetName(), body->GetLevel(), focus);
    }
    return true;
}

// ── Companion talents ────────────────────────────────────────────────────────
//
// A companion's session has no socket, so it can never receive CMSG_LEARN_TALENT, and the
// possessor's copy of that opcode is hardwired to _player (SkillHandler.cpp) — it spends YOUR
// point, not the companion's. `.spec` is the same shape: every handler resolves
// m_session->GetPlayer(). So without these commands there is no in-game way to spend a
// companion's talent points at all; the only routes are raw SQL or logging the character in
// with a real client.
//
// Deliberately NOT solved by rerouting the opcode: your client's talent frame renders YOUR
// tree — point totals, learned ranks, row gating all belong to your character — so a reroute
// would spend the companion's points against the wrong UI. A command is the honest surface.

namespace
{
    // Resolve an ENROLLED, in-world companion by name. Nothing here may operate on an
    // arbitrary player: these commands mutate a character permanently.
    Player* ResolveCompanionBody(ChatHandler* handler, std::string const& name)
    {
        ObjectGuid guid = sObjectMgr.GetPlayerGuidByName(name);
        if (guid.IsEmpty())
        {
            handler->PSendSysMessage("[SUI] no character named '%s'.", name.c_str());
            return nullptr;
        }

        PlayerBotEntry* entry = sPlayerBotMgr.GetBotEntry(guid.GetCounter());
        if (!entry || !entry->isCompanion)
        {
            handler->PSendSysMessage("[SUI] %s is not an active companion.", name.c_str());
            return nullptr;
        }

        Player* body = entry->ai ? entry->ai->me : nullptr;
        if (!body || !body->IsInWorld())
        {
            handler->PSendSysMessage("[SUI] %s is still loading.", name.c_str());
            return nullptr;
        }
        return body;
    }

    // How many ranks of this talent the body already has. Same walk Player::LearnTalent uses
    // to compute curtalent_maxrank, so "the next rank" here means what LearnTalent means.
    uint32 LearnedRankCount(Player* body, TalentEntry const* talentInfo)
    {
        for (int32 k = MAX_TALENT_RANK - 1; k > -1; --k)
            if (talentInfo->RankID[k] && body->HasSpell(talentInfo->RankID[k]))
                return uint32(k + 1);
        return 0;
    }

    // The rotation reads a cached spell list, so a freshly learned talent is invisible to it
    // until that cache is rebuilt. Same pair the AI runs at login and on level-up.
    void RefreshCompanionSpells(Player* body)
    {
        if (AiBotAI* ai = dynamic_cast<AiBotAI*>(body->AI()))
        {
            ai->ResetSpellData();
            ai->PopulateSpellData();
        }
    }
}

// .sui companion talent <name> [talentlink|talentId] [rank]
//
// With no talent argument: report unspent points. Otherwise spend one.
//
// The rank argument is 0-based and rarely wanted. Omitted, the command spends the NEXT rank,
// derived from the body's own spellbook — which is why a talent link shift-clicked out of
// YOUR tree works as the argument even though its embedded rank describes your character.
bool ChatHandler::HandleSuiCompanionTalentCommand(char* args)
{
    if (!m_session || !m_session->GetPlayer())
        return false;

    char* name = ExtractLiteralArg(&args);
    if (!name || !*name)
    {
        SendSysMessage("[SUI] usage: .sui companion talent <name> [talentlink|talentId] [rank]");
        SetSentErrorMessage(true);
        return false;
    }

    std::string charName = name;
    Player* body = ResolveCompanionBody(this, charName);
    if (!body)
    {
        SetSentErrorMessage(true);
        return false;
    }

    uint32 const freePoints = body->GetFreeTalentPoints();

    uint32 talentId = 0;
    if (!ExtractTalentFromLink(&args, talentId))
    {
        // No talent given — treat as a query rather than an error. Checking before spending
        // is the common case, and there is no other way to see a companion's point total
        // (the M4 snapshot that carries it is MSUIClient-only).
        PSendSysMessage("[SUI] %s (level %u) has %u unspent talent point(s).",
            body->GetName(), body->GetLevel(), freePoints);
        PSendSysMessage("[SUI] shift-click a talent into: .sui companion talent %s <talent>",
            body->GetName());
        return true;
    }

    TalentEntry const* talentInfo = sTalentStore.LookupEntry(talentId);
    if (!talentInfo)
    {
        PSendSysMessage("[SUI] talent %u does not exist.", talentId);
        SetSentErrorMessage(true);
        return false;
    }

    // Pre-checks exist purely to produce a REASON. Player::LearnTalent returns a bare bool,
    // so without these every refusal would read the same and be undiagnosable. It re-checks
    // all of this itself — these never substitute for its authority, only explain it.
    TalentTabEntry const* tabInfo = sTalentTabStore.LookupEntry(talentInfo->TalentTab);
    if (!tabInfo)
    {
        PSendSysMessage("[SUI] talent %u has no talent tab.", talentId);
        SetSentErrorMessage(true);
        return false;
    }

    if ((body->GetClassMask() & tabInfo->ClassMask) == 0)
    {
        PSendSysMessage("[SUI] that talent is not for %s's class.", body->GetName());
        SetSentErrorMessage(true);
        return false;
    }

    uint32 const learned = LearnedRankCount(body, talentInfo);

    // 0-based: the count of learned ranks IS the index of the next one. Initialised up front
    // so a partial parse inside ExtractUInt32 can never leave it indeterminate.
    uint32 rank = learned;
    if (!ExtractUInt32(&args, rank))
        rank = learned;

    if (rank >= MAX_TALENT_RANK)
    {
        PSendSysMessage("[SUI] rank must be 0-%u (0-based).", MAX_TALENT_RANK - 1);
        SetSentErrorMessage(true);
        return false;
    }

    if (!talentInfo->RankID[rank])
    {
        PSendSysMessage("[SUI] %s is already at max rank (%u).", body->GetName(), learned);
        SetSentErrorMessage(true);
        return false;
    }

    if (learned >= rank + 1)
    {
        PSendSysMessage("[SUI] %s already has rank %u of that talent.", body->GetName(), learned);
        SetSentErrorMessage(true);
        return false;
    }

    if (freePoints < (rank - learned + 1))
    {
        PSendSysMessage("[SUI] %s has %u unspent point(s); that needs %u.",
            body->GetName(), freePoints, rank - learned + 1);
        SetSentErrorMessage(true);
        return false;
    }

    if (!body->LearnTalent(talentId, rank))
    {
        // Everything cheap has been checked, so what is left is the tree's own structure:
        // an unmet prerequisite talent, a required spell, or not enough points spent in the
        // tab to open that row. Name those three rather than printing a bare failure.
        PSendSysMessage("[SUI] %s could not learn that talent — check its prerequisite, "
            "its required spell, and whether enough points are spent in that tree to open the row.",
            body->GetName());
        SetSentErrorMessage(true);
        return false;
    }

    RefreshCompanionSpells(body);

    PSendSysMessage("[SUI] %s learned rank %u; %u point(s) left.",
        body->GetName(), rank + 1, body->GetFreeTalentPoints());
    return true;
}

// .sui companion untalent <name> — full respec, no cost.
//
// Free on purpose: a companion cannot walk itself to a trainer, and without this a misclick
// would be unfixable without logging the character in on a real client. The trainer's gold
// cost exists to make respeccing a decision; that pressure belongs to characters you play
// directly, not to a mis-typed command.
bool ChatHandler::HandleSuiCompanionUntalentCommand(char* args)
{
    if (!m_session || !m_session->GetPlayer())
        return false;

    char* name = ExtractLiteralArg(&args);
    if (!name || !*name)
    {
        SendSysMessage("[SUI] usage: .sui companion untalent <name>");
        SetSentErrorMessage(true);
        return false;
    }

    std::string charName = name;
    Player* body = ResolveCompanionBody(this, charName);
    if (!body)
    {
        SetSentErrorMessage(true);
        return false;
    }

    if (!body->ResetTalents(true))
    {
        PSendSysMessage("[SUI] %s has no talents to reset.", body->GetName());
        SetSentErrorMessage(true);
        return false;
    }

    RefreshCompanionSpells(body);

    PSendSysMessage("[SUI] %s respecced — %u point(s) available.",
        body->GetName(), body->GetFreeTalentPoints());
    return true;
}

// .sui cast <member> <spellId> [target|me|self|<playername>]
//
// The stock-client surface for ordered casting, and the one the MSUI_Companion hotbar
// drives — a 1.12 addon can only reach the server through SendChatMessage, so a chat
// command is the channel. Same implementation as the custom-client ORDER_CAST opcode.
//
//   target   (default) the unit YOU have selected — "cast this at what I'm looking at"
//   me                 you, the commander — heals and buffs aimed at yourself
//   self               the member itself — its own buffs, bandages, defensive cooldowns
//   <name>             any player, for spot-healing a specific party member
bool ChatHandler::HandleSuiCastCommand(char* args)
{
    Player* commander = m_session ? m_session->GetPlayer() : nullptr;
    if (!commander)
        return false;

    char* memberName = ExtractLiteralArg(&args);
    uint32 spellId = 0;
    if (!memberName || !*memberName || !ExtractUInt32(&args, spellId) || !spellId)
    {
        SendSysMessage("[SUI] usage: .sui cast <member> <spellId> [target|me|self|<playername>]");
        SetSentErrorMessage(true);
        return false;
    }

    std::string name = memberName;
    Player* member = sObjectMgr.GetPlayer(name.c_str());
    if (!member || !member->IsInWorld())
    {
        PSendSysMessage("[SUI] %s is not in the world.", name.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    // Only your own party, and only bodies that actually have an AI to command. This is
    // the same authority test HandleOrder applies: a manually driven character has no
    // AiBotAI and must never be puppeted by someone else's chat line.
    Group* group = commander->GetGroup();
    if (member != commander && (!group || member->GetGroup() != group))
    {
        PSendSysMessage("[SUI] %s is not in your party.", member->GetName());
        SetSentErrorMessage(true);
        return false;
    }
    if (!dynamic_cast<AiBotAI*>(member->AI()))
    {
        PSendSysMessage("[SUI] %s is not under AI control.", member->GetName());
        SetSentErrorMessage(true);
        return false;
    }

    Unit* target = nullptr;
    char* targetArg = ExtractLiteralArg(&args);
    std::string targetSpec = targetArg ? targetArg : "target";

    if (targetSpec == "self")
        target = member;
    else if (targetSpec == "me")
        target = commander;
    else if (targetSpec == "target")
    {
        if (ObjectGuid sel = commander->GetSelectionGuid())
            target = commander->GetMap()->GetUnit(sel);
        // No selection: OrderCast defaults to the member, so a self-buff bound to a
        // plain button still works when you happen to have nothing targeted.
    }
    else if (Player* named = sObjectMgr.GetPlayer(targetSpec.c_str()))
        target = named;
    else
    {
        PSendSysMessage("[SUI] no player named '%s'.", targetSpec.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    SuiPossess::OrderCast(commander, member, spellId, target);
    return true;
}

// .sui order <member> <stop|come|hold|follow>
//
// The standing-order half of the hotbar. These are the existing RTS orders, reached
// from a stock client: HandleOrder does the authority checks and the actuation, this
// only translates a word into an order type and its one meaningful parameter.
//
//   stop     break off, clear the waypoint chain, cancel a cast still closing in
//   come     walk to where you are standing right now
//   hold     unlink from the chain — stand this ground, keep assisting from it
//   follow   relink — resume formation on you
bool ChatHandler::HandleSuiOrderCommand(char* args)
{
    Player* commander = m_session ? m_session->GetPlayer() : nullptr;
    if (!commander)
        return false;

    char* memberName = ExtractLiteralArg(&args);
    char* orderWord = ExtractLiteralArg(&args);
    if (!memberName || !*memberName || !orderWord || !*orderWord)
    {
        SendSysMessage("[SUI] usage: .sui order <member> <stop|come|hold|follow>");
        SetSentErrorMessage(true);
        return false;
    }

    std::string name = memberName;
    Player* member = sObjectMgr.GetPlayer(name.c_str());
    if (!member || !member->IsInWorld())
    {
        PSendSysMessage("[SUI] %s is not in the world.", name.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    std::string word = orderWord;
    uint8 orderType;
    float x = 0.0f, y = 0.0f, z = 0.0f;

    if (word == "stop")
        orderType = SuiPossess::ORDER_STOP;
    else if (word == "come")
    {
        orderType = SuiPossess::ORDER_MOVE;
        x = commander->GetPositionX();
        y = commander->GetPositionY();
        z = commander->GetPositionZ();
    }
    else if (word == "hold")
    {
        orderType = SuiPossess::ORDER_LINK;
        x = 0.0f;   // < 0.5 unlinks: stands its ground
    }
    else if (word == "follow")
    {
        orderType = SuiPossess::ORDER_LINK;
        x = 1.0f;   // >= 0.5 links back into the chain
    }
    else
    {
        PSendSysMessage("[SUI] unknown order '%s' — try stop, come, hold or follow.", word.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    // Straight to OrderOne, NOT through HandleOrder: that entry point marks the session
    // SUI-capable, which is right for a custom-client packet and wrong here — a stock
    // client issuing a chat command would then be sent SMSG_SUI_* it cannot parse.
    // OrderOne still enforces the party and AI-attached checks.
    SuiPossess::OrderOne(commander, member, orderType, ObjectGuid(), x, y, z);

    PSendSysMessage("[SUI] %s: %s.", member->GetName(), word.c_str());
    return true;
}
