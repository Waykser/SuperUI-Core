/*
 * SuperUI CRPG/RTS possession core.
 *
 * A real client session takes direct control of a party AiBot: the session's
 * mover is re-pointed at the bot (stock mover machinery — MSG_MOVE_* then
 * drive the bot), the bot's autonomous AI is suspended, and owner-only data is
 * proxied back to the possessor (M3). Deliberately NOT built on the charm
 * aura machinery: ModPossess is mind-control semantics (faction swap, threat
 * wipe) which is wrong for driving your own party bot mid-fight. Only the
 * ordering law is borrowed: Camera::SetView BEFORE SetMover BEFORE
 * SetClientControl (see Unit::ModPossess).
 *
 * State model (no global registry):
 *   - possessor session:  WorldSession::m_suiControlledGuid
 *   - bot:                Unit::m_possessorGuid (SetPossessorGuid) — also
 *                         denies a second possessor and routes
 *                         GetConfirmedMover/UpdateControl correctly
 *   - bot AI:             AiBotAI::m_possessed (suspends behaviour + bridge
 *                         commands; bridge STATE keeps flowing with
 *                         possessed:1 so the C# brain stands down)
 */

#ifndef MANGOS_SUI_POSSESS_H
#define MANGOS_SUI_POSSESS_H

#include "Common.h"
#include "ObjectGuid.h"
#include "SpellDefines.h"   // SpellCastResult, in the OrderCast/CastResultText signatures

#include <vector>

class Player;
class Unit;
class WorldSession;
class WorldPacket;
class Group;

namespace SuiPossess
{
    // SMSG_SUI_CONTROL_ACK result codes. 0 = granted; 1..15 denials answer a
    // CMSG_SUI_CONTROL_REQUEST; 16+ are releases — solicited or forced — and
    // must be accepted by the client in ANY control state.
    enum AckResult : uint8
    {
        ACK_OK                  = 0,
        DENY_NOT_FOUND          = 1,   // target missing / not a player / not visible / other map
        DENY_NOT_BOT            = 2,   // target is a real player or has no AiBotAI
        DENY_NOT_IN_GROUP       = 3,   // requester and target not in the same group
        DENY_BUSY               = 4,   // already possessed by someone
        DENY_TARGET_STATE       = 5,   // dead / taxi / transport / teleporting
        DENY_REQUESTER_STATE    = 6,   // requester is a bot, not self-mover, dead, on taxi…
        ACK_RELOCATING          = 7,   // outdoor transfer accepted; retry after ordinary streaming
        DENY_CROSS_INSTANCE     = 8,   // faction control never enters a foreign instance

        RELEASED                = 16,  // voluntary, back to own character
        RELEASED_FREECAM        = 17,  // voluntary, own character stays autonomous
        RELEASED_DEATH          = 18,  // possessed bot died
        RELEASED_TELEPORT       = 19,  // either side got teleported / changed map
        RELEASED_GROUP          = 20,  // group membership between the pair broke
        RELEASED_LOGOUT         = 21,  // possessor logging out, or the bot despawned
    };

    enum ReleaseMode : uint8
    {
        RELEASE_TO_SELF     = 0,
        RELEASE_TO_FREECAM  = 1,
    };

    enum OrderType : uint8              // CMSG_SUI_ORDER (M6)
    {
        ORDER_MOVE   = 0,
        ORDER_ATTACK = 1,
        ORDER_STOP   = 2,
        ORDER_MOVE_QUEUE = 3,   // append a waypoint; arrival chains the next leg
        ORDER_PATROL = 4,       // loop the queued waypoints until MOVE/STOP clears them
        ORDER_FOLLOW = 5,       // targetGuid = group member to escort; empty = auto split
        ORDER_LINK = 6,         // x >= 0.5 links the member into the chain; else unlinks
        ORDER_CAST = 7,         // x = spell id (see below); targetGuid = the unit to cast on
    };
    // ORDER_CAST carries its spell id in `x` rather than growing the Order packet, which
    // would break the existing client contract. ORDER_LINK already overloads `x` as a
    // boolean, so the precedent is the packet's own. A float holds every 1.12 spell id
    // exactly (ids are far below 2^24), so the round-trip is lossless.

    // SMSG_SUI_CONTROL_ROSTER member flags
    enum RosterFlags : uint8
    {
        ROSTER_CONTROLLABLE = 0x01,    // AiBot in your group you may possess
        ROSTER_POSSESSED    = 0x02,    // currently driven by a real player
    };

    /// Attempt possession. Sends the ACK (grant or deny) itself.
    void HandleRequest(WorldSession* session, ObjectGuid targetGuid);

    /// Voluntary release (client asked). Safe to call when nothing is possessed.
    void HandleRelease(WorldSession* session, uint8 mode);

    // CMSG_SUI_CAM: reposition the freecam eye so grid/visibility streaming
    // follows the free camera instead of the abandoned body. No-op outside
    // the free view.
    void HandleCam(WorldSession* session, float x, float y, float z, bool active);

    // CMSG_SUI_ZONE_INTEL: the commander map census. Answers ONLY the asker with
    // per-zone bots/players counts plus the live positions of its own forces
    // (self + group). Client-driven polling; no server-side broadcast timer.
    void HandleZoneIntel(WorldSession* session, uint8 flags);

    /// RTS order from the free camera: move/attack/stop for the group's AiBots
    /// (explicit subject list, or empty = every controllable bot). Reuses the
    /// bridge command paths so ordered movement/attack behaves exactly like a
    /// brain-issued command.
    void HandleOrder(WorldSession* session, uint8 orderType,
        std::vector<ObjectGuid> const& subjects, ObjectGuid targetGuid,
        float x, float y, float z);

    /// Apply ONE order to ONE member, authority checks included. HandleOrder is the
    /// packet entry point and fans out to this; the stock-client `.sui order` command
    /// calls it directly, deliberately bypassing HandleOrder's SetSuiCapable — a chat
    /// line is no evidence the client can parse SMSG_SUI_*, and marking it capable
    /// would start pushing custom opcodes at a client that cannot read them.
    void OrderOne(Player* commander, Player* member, uint8 orderType,
        ObjectGuid targetGuid, float x, float y, float z);

    /// Order one party member to cast one spell. The shared implementation behind both
    /// ORDER_CAST and the stock-client `.sui cast` command.
    ///
    /// The cast runs through Player::CastSpell UNTRIGGERED on purpose: Spell::CheckCast
    /// then enforces range, line of sight, power cost, cooldown, GCD, aura state and
    /// facing exactly as it would for a human. "Respect limitations" is not reimplemented
    /// here — it is inherited by refusing to bypass the real spell path.
    ///
    /// A member whose session has no socket cannot be told why its own cast failed, so
    /// the result is reported to `commander` instead. Range and LOS failures are not
    /// terminal: they arm a pending cast that walks the member in and retries.
    void OrderCast(Player* commander, Player* member, uint32 spellId, Unit* target);

    /// Plain-English name for a cast failure, or nullptr when the code has no friendly
    /// text and the caller should print the number. Shared with the retry path in
    /// AiBotAI::UpdatePendingCast so one order never reports two vocabularies.
    char const* CastResultText(SpellCastResult result);

    /// Forced release with a reason code; no-op when the session possesses nothing.
    /// Server-initiated paths open the movement drain window (m_moveRejectTime) so
    /// in-flight bot-coordinate MSG_MOVE_* are not attributed to the distant own char.
    void ForceRelease(WorldSession* session, AckResult reason);

    // ── Hooks (called from core seams) ────────────────────────────────────────
    void OnPlayerRemovedFromGroup(Player* player);   // Group::RemoveMember / Disband
    void OnPlayerTeleport(Player* player);           // Player::TeleportTo (either side of the pair)
    void OnPlayerDeath(Player* player);              // Player::SetDeathState(JUST_DIED)
    void OnLogout(WorldSession* session);            // WorldSession::LogoutPlayer

    // ── Queries ───────────────────────────────────────────────────────────────
    /// The real player driving `bot` via SUI possession, or nullptr.
    Player* GetPossessor(Unit const* bot);
    // True when this bot is possessed AND its possessor is commanding from the free view
    // (evidenced by a live freecam eye). Such a bot takes RTS orders and executes them under
    // its own AI: the client that "owns" it is parked on a camera and will never move it.
    bool IsCommandedFromFreeView(Unit const* bot);
    /// True while this player's session has the free view up (live freecam eye). The
    /// driving client is parked on the camera then, so server-side facing help applies
    /// to whatever unit its casts act through (commanded bot or unattended own char).
    bool IsFreeViewUp(Player* player);
    /// The bot this session is driving, or nullptr.
    Player* GetControlledBot(WorldSession const* session);
    bool IsSuiPossessed(Unit const* unit);

    // Cross-map faction control must retire the source-map free-camera eye
    // before Player::TeleportTo starts ordinary NEW_WORLD streaming.  The
    // return value records whether a failed transfer should restore that view.
    bool PrepareForRelocation(Player* player);
    void RestoreAfterFailedRelocation(Player* player, bool restoreFreeView);

    // ── Roster ────────────────────────────────────────────────────────────────
    /// Push SMSG_SUI_CONTROL_ROSTER to one real player (their current group view).
    void SendRoster(Player* realPlayer);
    /// Push the roster to every real-session member of a group.
    void BroadcastRoster(Group* group);

    // ── Owner-data mirror (M3) ────────────────────────────────────────────────
    /// Wrap whitelisted owner-only packets of a possessed bot's socket-less
    /// session into SMSG_SUI_PROXY toward the possessor. Called from
    /// WorldSession::SendPacket; no-op unless possessed and whitelisted.
    void MirrorOwnerPacket(WorldSession* botSession, WorldPacket const* packet);
}

#endif
