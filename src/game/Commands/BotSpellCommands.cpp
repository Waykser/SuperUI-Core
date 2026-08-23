/*
 * [BOTBAR] Player-facing bot spell commands.
 *
 * Commands:
 *   .botspell list <botname>              dump the bot's castable spellbook
 *   .botspell cast <botname> <id> [anchor] order a one-shot cast
 *   .botspell stop <botname>              drop the bot's parked cast order
 *
 * These back the MSUI_BotBar addon, which speaks the same way every other MSUI
 * addon does: SendChatMessage(".botspell ...", "SAY") outbound, and a prefixed
 * PSendSysMessage scraped off CHAT_MSG_SYSTEM inbound. No new opcode, no client
 * patch — a stock 1.12 client is the target.
 *
 * PLAYER ACCESS
 *   Every other bot command in the tree is SEC_ADMINISTRATOR. These are SEC_PLAYER,
 *   so AuthorizeBotCommand below is the ONLY thing standing between a player and
 *   remote control of a bot. The addon cannot be trusted to enforce anything — the
 *   commands can simply be typed — so every guard that matters lives here:
 *
 *     - the caller must be a real (non-bot) session
 *     - the target must be an AiBotAI, not an arbitrary player
 *     - the target must be in the CALLER'S OWN GROUP
 *     - the target must not be possessed by someone else
 *     - the caller is rate limited
 *
 *   The group check is the load-bearing one. Without it this is a command that
 *   lets any player anywhere puppet any bot on the server.
 *
 * ANCHORS (the ground-targeting answer)
 *   Vanilla 1.12 Lua cannot turn a click in the 3D world into world coordinates —
 *   there is no camera ray and no API. So the addon never sends a position. It
 *   sends an ANCHOR KEYWORD and this file resolves it server-side, where the full
 *   world state is available:
 *
 *     target     the caller's own selection            (default)
 *     self       the caller's feet
 *     bottarget  whatever the bot is already fighting
 *     cluster    the densest pack of hostiles engaged with the group
 *
 *   For a non-ground spell the anchor just picks the unit. For a ground spell
 *   (Blizzard, Rain of Fire, Volley, Hurricane) `target` and `bottarget` still
 *   resolve to a unit and let SpellCaster::CastSpell convert it to a destination
 *   at that unit's feet, while `self` and `cluster` resolve to real coordinates.
 */

#include "Chat.h"
#include "Language.h"
#include "Player.h"
#include "Group.h"
#include "WorldSession.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "SpellEntry.h"
#include "DBCStores.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Timer.h"
#include "Log.h"
#include "SuperUiContent/SuiBots/AiBotAIMain.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace
{
    // Chat lines are capped well below 255 bytes in practice once the prefix and
    // the bot name are counted; 160 bytes of payload keeps a comfortable margin.
    constexpr size_t BOTBAR_CHUNK_BYTES = 160;

    // One command per caller per this many ms. The addon issues at most one
    // command per click, so this only ever bites a stuck key or a buggy build —
    // but a chat command runs on the world tick and an unbounded one is a way to
    // stall the server from the client.
    constexpr uint32 BOTBAR_COMMAND_COOLDOWN_MS = 400;

    // Keyed by the CALLER's guid low. CMSG_MESSAGECHAT is PACKET_PROCESS_WORLD
    // (Opcodes.cpp), so every chat command runs on the world thread and this needs
    // no locking.
    std::map<uint32, uint32> s_lastCommandMs;

    // Players come and go over a long uptime; without this the map would grow for
    // the life of the process. Anything older than a minute can never throttle
    // anything, so a sweep at a generous high-water mark is enough.
    constexpr size_t BOTBAR_THROTTLE_MAP_MAX = 512;
    constexpr uint32 BOTBAR_THROTTLE_STALE_MS = 60000;

    void SweepThrottleMap(uint32 now)
    {
        if (s_lastCommandMs.size() < BOTBAR_THROTTLE_MAP_MAX)
            return;

        for (auto itr = s_lastCommandMs.begin(); itr != s_lastCommandMs.end();)
        {
            if (now - itr->second > BOTBAR_THROTTLE_STALE_MS)
                itr = s_lastCommandMs.erase(itr);
            else
                ++itr;
        }
    }

    // Resolve and authorize in one place. Returns the bot's AI on success, and on
    // failure returns null with outErr set to a short machine-readable token —
    // the addon shows its own message, so these are not player-facing prose.
    AiBotAI* AuthorizeBotCommand(Player* commander, char const* botName, char const*& outErr)
    {
        outErr = "internal";

        if (!commander || !commander->GetSession())
            return nullptr;

        // A bot session commanding another bot would let the C# brain reach these
        // verbs by writing chat, bypassing the bridge's own possession guards.
        if (commander->GetSession()->GetBot())
        {
            outErr = "not_a_player";
            return nullptr;
        }

        // Throttle FIRST, before any lookup. Rate limiting only the success path
        // would leave ".botspell list <garbage>" — a name lookup per call — as an
        // unbounded loop a client could run at packet rate.
        uint32 const now = WorldTimer::getMSTime();
        SweepThrottleMap(now);
        uint32& last = s_lastCommandMs[commander->GetGUIDLow()];
        if (last && now - last < BOTBAR_COMMAND_COOLDOWN_MS)
        {
            outErr = "throttled";
            return nullptr;
        }
        last = now;

        if (!botName || !*botName)
        {
            outErr = "no_bot_named";
            return nullptr;
        }

        Player* pBot = sObjectMgr.GetPlayer(botName);
        if (!pBot || !pBot->IsInWorld())
        {
            outErr = "bot_offline";
            return nullptr;
        }

        AiBotAI* ai = dynamic_cast<AiBotAI*>(pBot->AI());
        if (!ai)
        {
            outErr = "not_a_bot";
            return nullptr;
        }

        // THE gate. Same group, and the caller must actually be in one — a bot with
        // no group is nobody's to command.
        Group* pGroup = commander->GetGroup();
        if (!pGroup || pBot->GetGroup() != pGroup)
        {
            outErr = "not_grouped";
            return nullptr;
        }

        // A possessed bot is being driven by a human's own movement stream; the
        // bridge drops commands for it anyway, but failing here gives the addon a
        // reason instead of silence.
        if (ai->IsPossessed())
        {
            outErr = "possessed";
            return nullptr;
        }

        outErr = nullptr;
        return ai;
    }

    bool IsGroundTargeted(SpellEntry const* pSpell)
    {
        return pSpell && (pSpell->Targets & TARGET_FLAG_DEST_LOCATION) != 0;
    }

    // Widest radius across the spell's three effects — used to score AoE placement.
    float LargestEffectRadius(SpellEntry const* pSpell)
    {
        float best = 0.f;
        for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            if (!pSpell->EffectRadiusIndex[i])
                continue;
            float const r = Spells::GetSpellRadius(
                sSpellRadiusStore.LookupEntry(pSpell->EffectRadiusIndex[i]));
            best = std::max(best, r);
        }
        return best > 0.f ? best : 8.0f;   // sane floor for spells with no radius row
    }

    // "cluster": pick the hostile standing in the middle of the most other hostiles.
    //
    // Using an existing unit's position rather than a true centroid is deliberate —
    // a real creature is always standing somewhere legal, so this needs no terrain
    // height lookup and can never drop an AoE inside a wall or under the floor.
    // Only mobs already engaged with the group count, so this cannot be used to
    // fish for or pull unrelated packs.
    Unit* FindDensestHostile(Player* pBot, SpellEntry const* pSpell)
    {
        Group* pGroup = pBot->GetGroup();
        if (!pGroup)
            return nullptr;

        SpellRangeEntry const* srange = sSpellRangeStore.LookupEntry(pSpell->rangeIndex);
        float const castRange = srange ? srange->maxRange : 30.0f;
        float const radius = LargestEffectRadius(pSpell);

        std::list<Unit*> candidates;
        {
            MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck check(pBot, pBot, castRange);
            MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck>
                searcher(candidates, check);
            Cell::VisitAllObjects(pBot, searcher, castRange);
        }

        // Keep only mobs actually fighting somebody in our group.
        std::vector<Unit*> engaged;
        for (Unit* pUnit : candidates)
        {
            if (!pUnit || !pUnit->IsAlive() || pUnit->GetTypeId() != TYPEID_UNIT)
                continue;
            Unit* pVictim = pUnit->GetVictim();
            if (!pVictim)
                continue;
            Player* pOwner = pVictim->GetCharmerOrOwnerPlayerOrPlayerItself();
            if (!pOwner || pOwner->GetGroup() != pGroup)
                continue;
            if (!pBot->IsWithinLOSInMap(pUnit))
                continue;
            engaged.push_back(pUnit);
        }

        Unit* pBest = nullptr;
        int bestCount = 0;
        for (Unit* pCentre : engaged)
        {
            int count = 0;
            for (Unit* pOther : engaged)
                if (pCentre->IsWithinDist(pOther, radius))
                    ++count;
            if (count > bestCount)
            {
                bestCount = count;
                pBest = pCentre;
            }
        }
        return pBest;
    }
}

// ============================================================================
// .botspell list <botname>
//
// Emits the bot's castable spell IDs as prefixed system messages the addon
// reassembles. IDs only: the addon resolves names and icons from the generated
// MSUI_BotSpellsData.lua, because vanilla Lua cannot look up an arbitrary spell
// id and shipping the names down this channel would multiply the traffic tenfold.
// ============================================================================
bool ChatHandler::HandleBotSpellListCommand(char* args)
{
    Player* commander = m_session ? m_session->GetPlayer() : nullptr;
    if (!commander)
        return false;

    char* nameArg = ExtractLiteralArg(&args);
    char const* err = nullptr;
    AiBotAI* ai = AuthorizeBotCommand(commander, nameArg, err);
    if (!ai)
    {
        PSendSysMessage("MSUIBS|ERR|%s|%s|0", nameArg ? nameArg : "?", err);
        return true;
    }

    Player* pBot = sObjectMgr.GetPlayer(nameArg);

    // Same filter PopulateSpellData uses, for the same reason: passives and
    // hidden spells are not things a player can meaningfully order.
    std::map<std::string, SpellEntry const*> best;
    for (auto const& spell : pBot->GetSpellMap())
    {
        if (spell.second.disabled || spell.second.state == PLAYERSPELL_REMOVED)
            continue;

        SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(spell.first);
        if (!pSpellEntry)
            continue;
        if (pSpellEntry->HasAttribute(SPELL_ATTR_PASSIVE))
            continue;
        if (pSpellEntry->HasAttribute(SPELL_ATTR_DO_NOT_DISPLAY))
            continue;

        // Collapse ranks: the bar wants "Frostbolt", not nine Frostbolts. Highest
        // rank wins, matching how the bot's own spell data is built.
        std::string const& key = pSpellEntry->SpellName[0];
        auto itr = best.find(key);
        if (itr == best.end())
        {
            best[key] = pSpellEntry;
            continue;
        }

        SpellEntry const* pOld = itr->second;
        uint32 const newRank = pSpellEntry->GetRank();
        bool const higher = newRank ? (newRank > pOld->GetRank())
                                    : (pSpellEntry->Id > pOld->Id);
        if (higher)
            itr->second = pSpellEntry;
    }

    std::vector<uint32> ids;
    ids.reserve(best.size());
    for (auto const& kv : best)
        ids.push_back(kv.second->Id);
    std::sort(ids.begin(), ids.end());

    // Build the chunks first so the header can carry an accurate total — the addon
    // uses it to know when the dump is complete rather than guessing on a timeout.
    std::vector<std::string> chunks;
    std::string current;
    for (uint32 id : ids)
    {
        char idBuf[16];
        snprintf(idBuf, sizeof(idBuf), "%u", id);
        if (!current.empty() && current.size() + strlen(idBuf) + 1 > BOTBAR_CHUNK_BYTES)
        {
            chunks.push_back(current);
            current.clear();
        }
        if (!current.empty())
            current += ",";
        current += idBuf;
    }
    if (!current.empty())
        chunks.push_back(current);

    PSendSysMessage("MSUIBS|BEGIN|%s|%u|%u|%u",
        pBot->GetName(), (uint32)pBot->GetClass(), (uint32)pBot->GetLevel(),
        (uint32)chunks.size());

    for (size_t i = 0; i < chunks.size(); ++i)
        PSendSysMessage("MSUIBS|SPELLS|%s|%u|%s",
            pBot->GetName(), (uint32)(i + 1), chunks[i].c_str());

    PSendSysMessage("MSUIBS|END|%s|%u", pBot->GetName(), (uint32)ids.size());
    return true;
}

// ============================================================================
// .botspell cast <botname> <spellId> [anchor]
//
// Resolves the anchor into either a unit or a destination, then hands the order
// to the bot through BridgeProcessLine — the same in-process injection seam the
// RTS order handler uses, so an ordered cast behaves identically to a
// brain-issued one including every guard on the far side.
// ============================================================================
bool ChatHandler::HandleBotSpellCastCommand(char* args)
{
    Player* commander = m_session ? m_session->GetPlayer() : nullptr;
    if (!commander)
        return false;

    char* nameArg = ExtractLiteralArg(&args);
    uint32 spellId = 0;
    if (!ExtractUInt32(&args, spellId) || !spellId)
    {
        PSendSysMessage("MSUIBS|ERR|%s|bad_args|0", nameArg ? nameArg : "?");
        return true;
    }

    char* anchorArg = ExtractLiteralArg(&args);
    std::string anchor = anchorArg ? anchorArg : "target";
    std::transform(anchor.begin(), anchor.end(), anchor.begin(), ::tolower);

    char const* err = nullptr;
    AiBotAI* ai = AuthorizeBotCommand(commander, nameArg, err);
    if (!ai)
    {
        PSendSysMessage("MSUIBS|ERR|%s|%s|%u", nameArg ? nameArg : "?", err, spellId);
        return true;
    }

    Player* pBot = sObjectMgr.GetPlayer(nameArg);

    SpellEntry const* pSpell = sSpellMgr.GetSpellEntry(spellId);
    if (!pSpell)
    {
        PSendSysMessage("MSUIBS|ERR|%s|unknown_spell|%u", pBot->GetName(), spellId);
        return true;
    }

    // Checked here as well as in the bridge handler so the player gets a precise
    // reason rather than a generic failure from the far side.
    if (!pBot->HasSpell(spellId))
    {
        PSendSysMessage("MSUIBS|ERR|%s|not_learned|%u", pBot->GetName(), spellId);
        return true;
    }

    // ---- Resolve the anchor. ----
    Unit* pUnitTarget = nullptr;
    bool positional = false;
    float px = 0.f, py = 0.f, pz = 0.f;

    if (anchor == "self")
    {
        // The player positions themselves and the AoE lands on them. For a
        // non-ground spell this reads as "cast it on me", which is what a player
        // asking for a heal or a buff means.
        if (IsGroundTargeted(pSpell))
        {
            positional = true;
            px = commander->GetPositionX();
            py = commander->GetPositionY();
            pz = commander->GetPositionZ();
        }
        else
            pUnitTarget = commander;
    }
    else if (anchor == "bottarget")
    {
        pUnitTarget = pBot->GetVictim();
    }
    else if (anchor == "cluster")
    {
        Unit* pCentre = FindDensestHostile(pBot, pSpell);
        if (pCentre && IsGroundTargeted(pSpell))
        {
            positional = true;
            px = pCentre->GetPositionX();
            py = pCentre->GetPositionY();
            pz = pCentre->GetPositionZ();
        }
        else
            pUnitTarget = pCentre;
    }
    else   // "target" — the default, and the one the unmodified click sends
    {
        pUnitTarget = commander->GetMap()->GetUnit(commander->GetSelectionGuid());

        // A friendly spell with no selection is almost always meant for the caller;
        // guessing here is what makes a one-click heal button work.
        if (!pUnitTarget && pSpell->IsPositiveSpell())
            pUnitTarget = commander;
    }

    if (!positional && !pUnitTarget)
    {
        PSendSysMessage("MSUIBS|ERR|%s|no_target|%u", pBot->GetName(), spellId);
        return true;
    }

    // Ordering a bot to attack something it would refuse to attack on its own is
    // not "best effort", it is a way to start fights through a proxy.
    if (pUnitTarget && !pSpell->IsPositiveSpell() && pUnitTarget != commander)
    {
        if (!ai->IsValidAssistTarget(pUnitTarget))
        {
            PSendSysMessage("MSUIBS|ERR|%s|bad_target|%u", pBot->GetName(), spellId);
            return true;
        }
    }

    // ---- Hand it over. Same injection seam SuiPossess::HandleOrder uses. ----
    char json[256];
    if (positional)
    {
        snprintf(json, sizeof(json),
            "{\"type\":\"CAST_SPELL\",\"payload\":{\"spell_id\":%u,"
            "\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"commander_guid\":%u}}",
            spellId, px, py, pz, commander->GetGUIDLow());
    }
    else
    {
        // Carry the ENTRY for creatures: a vmangos creature ObjectGuid is
        // (HIGHGUID_UNIT, entry, counter) and Map::GetCreature matches all three,
        // so a counter-only payload never resolves on the far side. Players have
        // no entry, and 0 is how the handler tells the two apart.
        uint32 const entry = pUnitTarget->GetTypeId() == TYPEID_UNIT
            ? pUnitTarget->GetObjectGuid().GetEntry() : 0;
        snprintf(json, sizeof(json),
            "{\"type\":\"CAST_SPELL\",\"payload\":{\"spell_id\":%u,"
            "\"target_guid\":%u,\"target_entry\":%u,\"commander_guid\":%u}}",
            spellId, pUnitTarget->GetGUIDLow(), entry, commander->GetGUIDLow());
    }

    ai->BridgeProcessLine(json);

    sLog.Out(LOG_BASIC, LOG_LVL_DEBUG,
        "[BOTBAR] %s ordered %s to cast %u (anchor=%s)",
        commander->GetName(), pBot->GetName(), spellId, anchor.c_str());

    PSendSysMessage("MSUIBS|SENT|%s|%u|%s", pBot->GetName(), spellId, anchor.c_str());
    return true;
}

// ============================================================================
// .botspell stop <botname>
//
// Drops a parked order. Exists because the order retries for several seconds:
// without this, a misclicked Polymorph on the wrong mob keeps trying to fire
// after the player has already noticed the mistake.
// ============================================================================
bool ChatHandler::HandleBotSpellStopCommand(char* args)
{
    Player* commander = m_session ? m_session->GetPlayer() : nullptr;
    if (!commander)
        return false;

    char* nameArg = ExtractLiteralArg(&args);
    char const* err = nullptr;
    AiBotAI* ai = AuthorizeBotCommand(commander, nameArg, err);
    if (!ai)
    {
        PSendSysMessage("MSUIBS|ERR|%s|%s|0", nameArg ? nameArg : "?", err);
        return true;
    }

    ai->ClearPlayerCastOrder(nullptr);
    PSendSysMessage("MSUIBS|STOP|%s", nameArg);
    return true;
}
