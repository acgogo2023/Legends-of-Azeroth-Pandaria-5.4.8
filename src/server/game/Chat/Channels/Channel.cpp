/*
* This file is part of the Pandaria 5.4.8 Project. See THANKS file for Copyright information
*
* This program is free software; you can redistribute it and/or modify it
* under the terms of the GNU General Public License as published by the
* Free Software Foundation; either version 2 of the License, or (at your
* option) any later version.
*
* This program is distributed in the hope that it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
* more details.
*
* You should have received a copy of the GNU General Public License along
* with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "Channel.h"
#include "Chat.h"
#include "ObjectMgr.h"
#include "SocialMgr.h"
#include "World.h"
#include "DatabaseEnv.h"
#include "AccountMgr.h"
#include "Player.h"

Channel::Channel(std::string const& name, uint32 channelId, uint32 team):
    _announce(true),
    _ownership(true),
    _IsSaved(false),
    _flags(0),
    _channelId(channelId),
    _Team(team),
    _ownerGUID(),
    _name(name),
    _password("")
{
    // set special flags if built-in channel
    if (ChatChannelsEntry const* ch = sChatChannelsStore.LookupEntry(channelId)) // check whether it's a built-in channel
    {
        _announce = false;                                 // no join/leave announces
        _ownership = false;                                // no ownership handout

        _flags |= CHANNEL_FLAG_GENERAL;                    // for all built-in channels

        if (ch->flags & CHANNEL_DBC_FLAG_TRADE)             // for trade channel
            _flags |= CHANNEL_FLAG_TRADE;

        if (ch->flags & CHANNEL_DBC_FLAG_CITY_ONLY2)        // for city only channels
            _flags |= CHANNEL_FLAG_CITY;

        if (ch->flags & CHANNEL_DBC_FLAG_LFG)               // for LFG channel
            _flags |= CHANNEL_FLAG_LFG;
        else                                                // for all other channels
            _flags |= CHANNEL_FLAG_NOT_LFG;
    }
    else                                                    // it's custom channel
    {
        _flags |= CHANNEL_FLAG_CUSTOM;

        // If storing custom channels in the db is enabled either load or save the channel
        if (sWorld->getBoolConfig(CONFIG_PRESERVE_CUSTOM_CHANNELS))
        {
            CharacterDatabasePreparedStatement *stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHANNEL);
            stmt->setString(0, name);
            stmt->setUInt32(1, _Team);
            PreparedQueryResult result = CharacterDatabase.Query(stmt);

            if (result) //load
            {
                Field* fields = result->Fetch();
                _announce = fields[0].GetBool();
                _ownership = fields[1].GetBool();
                _password  = fields[2].GetString();
                const char* db_BannedList = fields[3].GetCString();

                if (db_BannedList)
                {
                    Tokenizer tokens(db_BannedList, ' ');
                    for (Tokenizer::const_iterator i = tokens.begin(); i != tokens.end(); ++i)
                    {
                        ObjectGuid banned_guid(HighGuid::Player, uint32(atol(*i)));
                        if (banned_guid)
                        {
                            TC_LOG_DEBUG("chat.system", "Channel(%s) loaded bannedStore guid:" UI64FMTD "", name.c_str(), banned_guid.GetRawValue());
                            bannedStore.insert(banned_guid);
                        }
                    }
                }
            }
            else // save
            {
                stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_CHANNEL);
                stmt->setString(0, name);
                stmt->setUInt32(1, _Team);
                CharacterDatabase.Execute(stmt);
                TC_LOG_DEBUG("chat.system", "Channel(%s) saved in database", name.c_str());
            }

            _IsSaved = true;
        }
    }
}

void Channel::UpdateChannelInDB() const
{
    if (_IsSaved)
    {
        std::ostringstream banlist;
        BannedContainer::const_iterator iter;
        for (iter = bannedStore.begin(); iter != bannedStore.end(); ++iter)
            banlist << (*iter) << ' ';

        std::string banListStr = banlist.str();

        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_CHANNEL);
        stmt->setBool(0, _announce);
        stmt->setBool(1, _ownership);
        stmt->setString(2, _password);
        stmt->setString(3, banListStr);
        stmt->setString(4, _name);
        stmt->setUInt32(5, _Team);
        CharacterDatabase.Execute(stmt);

        TC_LOG_DEBUG("chat.system", "Channel(%s) updated in database", _name.c_str());
    }
}

void Channel::UpdateChannelUseageInDB() const
{
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_CHANNEL_USAGE);
    stmt->setString(0, _name);
    stmt->setUInt32(1, _Team);
    CharacterDatabase.Execute(stmt);
}

void Channel::CleanOldChannelsInDB()
{
    if (sWorld->getIntConfig(CONFIG_PRESERVE_CUSTOM_CHANNEL_DURATION) > 0)
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_OLD_CHANNELS);
        stmt->setUInt32(0, sWorld->getIntConfig(CONFIG_PRESERVE_CUSTOM_CHANNEL_DURATION) * DAY);
        CharacterDatabase.Execute(stmt);

        TC_LOG_DEBUG("chat.system", "Cleaned out unused custom chat channels.");
    }
}

void Channel::JoinChannel(Player* player, std::string const& pass)
{
    ObjectGuid guid = player->GetGUID();
    if (IsOn(guid))
    {
        // Do not send error message for built-in channels
        if (!IsConstant())
        {
            WorldPacket data;
            MakePlayerAlreadyMember(&data, guid);
            SendToOne(&data, guid);
        }
        return;
    }

    if (IsBanned(guid))
    {
        WorldPacket data;
        MakeBanned(&data);
        SendToOne(&data, guid);
        return;
    }

    if (!_password.empty() && pass != _password)
    {
        WorldPacket data;
        MakeWrongPassword(&data);
        SendToOne(&data, guid);
        return;
    }

    if (HasFlag(CHANNEL_FLAG_LFG) &&
        sWorld->getBoolConfig(CONFIG_RESTRICTED_LFG_CHANNEL) &&
        AccountMgr::IsPlayerAccount(player->GetSession()->GetSecurity()) && //FIXME: Move to RBAC
        player->GetGroup())
    {
        WorldPacket data;
        MakeNotInLfg(&data);
        SendToOne(&data, guid);
        return;
    }

    player->JoinedChannel(this);

    if (_announce && (player->GetSession()->GetSecurity() < SEC_GAMEMASTER || !sWorld->getBoolConfig(CONFIG_SILENTLY_GM_JOIN_TO_CHANNEL)))
    {
        WorldPacket data;
        MakeJoined(&data, guid);
        SendToAll(&data);
    }

    PlayerInfo pinfo;
    pinfo.player = guid;
    pinfo.flags = MEMBER_FLAG_NONE;
    playersStore[guid] = pinfo;

    WorldPacket data;
    MakeYouJoined(&data);
    SendToOne(&data, guid);

    JoinNotify(guid);

    // Custom channel handling
    if (!IsConstant())
    {
        // Update last_used timestamp in db
        if (!playersStore.empty())
            UpdateChannelUseageInDB();

        // If the channel has no owner yet and ownership is allowed, set the new owner.
        if (!_ownerGUID && _ownership)
        {
            SetOwner(guid, playersStore.size() > 1);
            playersStore[guid].SetModerator(true);
        }
    }
}

void Channel::LeaveChannel(Player* player, bool send)
{
    ObjectGuid guid = player->GetGUID();
    if (!IsOn(guid))
    {
        if (send)
        {
            WorldPacket data;
            MakeNotMember(&data);
            SendToOne(&data, guid);
        }
        return;
    }

    if (send)
    {
        WorldPacket data;
        MakeYouLeft(&data);
        SendToOne(&data, guid);
        player->LeftChannel(this);
        data.clear();
    }

    bool changeowner = playersStore[guid].IsOwner();

    playersStore.erase(guid);

    if (_announce && (player->GetSession()->GetSecurity() < SEC_GAMEMASTER || !sWorld->getBoolConfig(CONFIG_SILENTLY_GM_JOIN_TO_CHANNEL)))
    {
        WorldPacket data;
        MakeLeft(&data, guid);
        SendToAll(&data);
    }

    LeaveNotify(guid);

    if (!IsConstant())
    {
        // Update last_used timestamp in db
        UpdateChannelUseageInDB();

        // If the channel owner left and there are still playersStore inside, pick a new owner
        if (changeowner && _ownership && !playersStore.empty())
        {
            ObjectGuid newowner = playersStore.begin()->second.player;
            playersStore[newowner].SetModerator(true);
            SetOwner(newowner);
        }
    }
}

void Channel::KickOrBan(Player const* player, std::string const& badname, bool ban)
{
    AccountTypes sec = player->GetSession()->GetSecurity();
    ObjectGuid good = player->GetGUID();

    if (!IsOn(good))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, good);
        return;
    }

    if (!playersStore[good].IsModerator() && sec < SEC_GAMEMASTER)
    {
        WorldPacket data;
        MakeNotModerator(&data);
        SendToOne(&data, good);
        return;
    }

    Player* bad = ObjectAccessor::FindPlayerByName(badname);
    ObjectGuid victim = bad ? bad->GetGUID() : ObjectGuid::Empty;
    if (!victim || !IsOn(victim))
    {
        WorldPacket data;
        MakePlayerNotFound(&data, badname);
        SendToOne(&data, good);
        return;
    }

    bool changeowner = _ownerGUID == victim;

    if (sec < SEC_GAMEMASTER && changeowner && good != _ownerGUID)
    {
        WorldPacket data;
        MakeNotOwner(&data);
        SendToOne(&data, good);
        return;
    }

    if (ban && !IsBanned(victim))
    {
        bannedStore.insert(victim);
        UpdateChannelInDB();

        WorldPacket data;
        MakePlayerBanned(&data, victim, good);
        SendToAll(&data);
    }
    else
    {
        WorldPacket data;
        MakePlayerKicked(&data, victim, good);
        SendToAll(&data);
    }

    playersStore.erase(victim);
    bad->LeftChannel(this);

    if (changeowner && _ownership && !playersStore.empty())
    {
        ObjectGuid newowner = good;
        playersStore[newowner].SetModerator(true);
        SetOwner(newowner);
    }
}

void Channel::UnBan(Player const* player, std::string const& badname)
{
    ObjectGuid good = player->GetGUID();

    if (!IsOn(good))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, good);
        return;
    }

    if (!playersStore[good].IsModerator() && player->GetSession()->GetSecurity() < SEC_GAMEMASTER)
    {
        WorldPacket data;
        MakeNotModerator(&data);
        SendToOne(&data, good);
        return;
    }

    Player* bad = ObjectAccessor::FindPlayerByName(badname);
    ObjectGuid victim = bad ? bad->GetGUID(): ObjectGuid::Empty;

    if (!victim || !IsBanned(victim))
    {
        WorldPacket data;
        MakePlayerNotFound(&data, badname);
        SendToOne(&data, good);
        return;
    }

    bannedStore.erase(victim);

    WorldPacket data;
    MakePlayerUnbanned(&data, victim, good);
    SendToAll(&data);

    UpdateChannelInDB();
}

void Channel::Password(Player const* player, std::string const& pass)
{
    ObjectGuid guid = player->GetGUID();

    ChatHandler chat(player->GetSession());
    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, guid);
        return;
    }

    if (!playersStore[guid].IsModerator() && player->GetSession()->GetSecurity() < SEC_GAMEMASTER)
    {
        WorldPacket data;
        MakeNotModerator(&data);
        SendToOne(&data, guid);
        return;
    }

    _password = pass;

    WorldPacket data;
    MakePasswordChanged(&data, guid);
    SendToAll(&data);

    UpdateChannelInDB();
}

void Channel::SetMode(Player const* player, std::string const& p2n, bool mod, bool set)
{
    uint32 sec = player->GetSession()->GetSecurity();
    ObjectGuid guid = player->GetGUID();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, guid);
        return;
    }

    if (!playersStore[guid].IsModerator() && sec < SEC_GAMEMASTER)
    {
        WorldPacket data;
        MakeNotModerator(&data);
        SendToOne(&data, guid);
        return;
    }

    if (guid == _ownerGUID && std::string(p2n) == player->GetName() && mod)
        return;

    Player* newp = ObjectAccessor::FindPlayerByName(p2n);
    ObjectGuid victim = newp ? newp->GetGUID() : ObjectGuid::Empty;

    if (!victim || !IsOn(victim) ||
        (player->GetTeam() != newp->GetTeam() &&
        (sec < SEC_GAMEMASTER ||
        newp->GetSession()->GetSecurity() < SEC_GAMEMASTER)))
    {
        WorldPacket data;
        MakePlayerNotFound(&data, p2n);
        SendToOne(&data, guid);
        return;
    }

    if (_ownerGUID == victim && _ownerGUID != guid)
    {
        WorldPacket data;
        MakeNotOwner(&data);
        SendToOne(&data, guid);
        return;
    }

    if (mod)
        SetModerator(newp->GetGUID(), set);
    else
        SetMute(newp->GetGUID(), set);
}

void Channel::SetOwner(Player const* player, std::string const& newname)
{
    uint32 sec = player->GetSession()->GetSecurity();
    ObjectGuid guid = player->GetGUID();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, guid);
        return;
    }

    if (sec < SEC_GAMEMASTER)
    {
        WorldPacket data;
        MakeNotOwner(&data);
        SendToOne(&data, guid);
        return;
    }

    Player* newp = ObjectAccessor::FindPlayerByName(newname);
    ObjectGuid victim = newp ? newp->GetGUID() : ObjectGuid::Empty;

    if (!victim || !IsOn(victim) ||
        (player->GetTeam() != newp->GetTeam() &&
        (sec < SEC_GAMEMASTER ||
        newp->GetSession()->GetSecurity() < SEC_GAMEMASTER)))
    {
        WorldPacket data;
        MakePlayerNotFound(&data, newname);
        SendToOne(&data, guid);
        return;
    }

    playersStore[victim].SetModerator(true);
    SetOwner(victim);
}

void Channel::SendWhoOwner(ObjectGuid guid)
{
    WorldPacket data;
    if (IsOn(guid))
        MakeChannelOwner(&data);
    else
        MakeNotMember(&data);
    SendToOne(&data, guid);
}

void Channel::List(Player const* player)
{
    ObjectGuid guid = player->GetGUID();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, guid);
        return;
    }

    TC_LOG_DEBUG("chat.system", "SMSG_CHANNEL_LIST %s Channel: %s",
        player->GetSession()->GetPlayerInfo().c_str(), GetName().c_str());

    WorldPacket data(SMSG_CHANNEL_LIST, 1+(GetName().size()+1)+1+4+playersStore.size()*(8+1));
    data << uint8(1);                                   // channel type?
    data << GetName();                                  // channel name
    data << uint8(GetFlags());                          // channel flags?

    size_t pos = data.wpos();
    data << uint32(0);                                  // size of list, placeholder

    uint32 gmLevelInWhoList = sWorld->getIntConfig(CONFIG_GM_LEVEL_IN_WHO_LIST);

    uint32 count  = 0;
    for (PlayerContainer::const_iterator i = playersStore.begin(); i != playersStore.end(); ++i)
    {
        Player* member = ObjectAccessor::FindPlayer(i->first);

        // PLAYER can't see MODERATOR, GAME MASTER, ADMINISTRATOR characters
        // MODERATOR, GAME MASTER, ADMINISTRATOR can see all
        if (member &&
            (player->GetSession()->GetSecurity() > SEC_PLAYER ||
            member->GetSession()->GetSecurity() <= AccountTypes(gmLevelInWhoList)) &&
            member->IsVisibleGloballyFor(player))
        {
            data << uint64(i->first);
            data << uint32(0); // Realm ID
            data << uint8(i->second.flags);             // flags seems to be changed...
            ++count;
        }
    }

    data.put<uint32>(pos, count);

    SendToOne(&data, guid);
}

void Channel::Announce(Player const* player)
{
    ObjectGuid guid = player->GetGUID();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, guid);
        return;
    }

    if (!playersStore[guid].IsModerator() && player->GetSession()->GetSecurity() < SEC_GAMEMASTER)
    {
        WorldPacket data;
        MakeNotModerator(&data);
        SendToOne(&data, guid);
        return;
    }

    _announce = !_announce;

    WorldPacket data;
    if (_announce)
        MakeAnnouncementsOn(&data, guid);
    else
        MakeAnnouncementsOff(&data, guid);
    SendToAll(&data);

    UpdateChannelInDB();
}

void Channel::Say(ObjectGuid guid, std::string const& what, uint32 lang)
{
    if (what.empty())
        return;

    uint8 chatTag = 0;
    bool isGM = false;

    if (Player* player = ObjectAccessor::FindPlayer(guid))
    {
        chatTag = player->GetChatTag();
        isGM = player->isGMChat();
    }

    // TODO: Add proper RBAC check
    if (sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_CHANNEL))
        lang = LANG_UNIVERSAL;

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, guid);
        return;
    }

    if (playersStore[guid].IsMuted())
    {
        WorldPacket data;
        MakeMuted(&data);
        SendToOne(&data, guid);
        return;
    }

    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_CHANNEL, Language(lang), guid, guid, what, chatTag, "", "", 0, isGM, _name);

    SendToAll(&data, !playersStore[guid].IsModerator() ? guid : ObjectGuid::Empty);
}

void Channel::Invite(Player const* player, std::string const& newname)
{
    ObjectGuid guid = player->GetGUID();
    uint32 sec = player->GetSession()->GetSecurity();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, guid);
        return;
    }

    Player* newp = ObjectAccessor::FindPlayerByName(newname);
    if (!newp || !newp->isGMVisible())
    {
        WorldPacket data;
        MakePlayerNotFound(&data, newname);
        SendToOne(&data, guid);
        return;
    }

    if (IsBanned(newp->GetGUID()))
    {
        WorldPacket data;
        MakePlayerInviteBanned(&data, newname);
        SendToOne(&data, guid);
        return;
    }

    if (newp->GetTeam() != player->GetTeam() &&
        (player->GetSession()->GetSecurity() < SEC_GAMEMASTER ||
        newp->GetSession()->GetSecurity() < SEC_GAMEMASTER))
    {
        WorldPacket data;
        MakeInviteWrongFaction(&data);
        SendToOne(&data, guid);
        return;
    }

    if (IsOn(newp->GetGUID()))
    {
        WorldPacket data;
        MakePlayerAlreadyMember(&data, newp->GetGUID());
        SendToOne(&data, guid);
        return;
    }

    if (!newp->GetSocial()->HasIgnore(guid))
    {
        WorldPacket data;
        MakeInvite(&data, guid);
        SendToOne(&data, newp->GetGUID());
        data.clear();
    }

    WorldPacket data;
    MakePlayerInvited(&data, newp->GetName());
    SendToOne(&data, guid);
}

void Channel::SetOwner(ObjectGuid guid, bool exclaim)
{
    if (_ownerGUID)
    {
        // [] will re-add player after it possible removed
        PlayerContainer::iterator p_itr = playersStore.find(_ownerGUID);
        if (p_itr != playersStore.end())
            p_itr->second.SetOwner(false);
    }

    _ownerGUID = guid;
    if (_ownerGUID)
    {
        uint8 oldFlag = GetPlayerFlags(_ownerGUID);
        playersStore[_ownerGUID].SetModerator(true);
        playersStore[_ownerGUID].SetOwner(true);
        if (exclaim)
        {
            WorldPacket data;
            MakeModeChange(&data, _ownerGUID, oldFlag);
            SendToAll(&data);

            MakeOwnerChanged(&data, _ownerGUID);
            SendToAll(&data);
        }

        UpdateChannelInDB();
    }
}

void Channel::SendToAll(WorldPacket* data, ObjectGuid guid)
{
    for (PlayerContainer::const_iterator i = playersStore.begin(); i != playersStore.end(); ++i)
        if (Player* player = ObjectAccessor::FindPlayer(i->first))
            if (!guid || !player->GetSocial()->HasIgnore(guid))
                player->GetSession()->SendPacket(data);
}

void Channel::SendToAllButOne(WorldPacket* data, ObjectGuid who)
{
    for (PlayerContainer::const_iterator i = playersStore.begin(); i != playersStore.end(); ++i)
        if (i->first != who)
            if (Player* player = ObjectAccessor::FindPlayer(i->first))
                player->GetSession()->SendPacket(data);
}

void Channel::SendToOne(WorldPacket* data, ObjectGuid who)
{
    if (Player* player = ObjectAccessor::FindPlayer(who))
        player->GetSession()->SendPacket(data);
}

void Channel::Voice(uint64 /*guid1*/, uint64 /*guid2*/)
{

}

void Channel::DeVoice(uint64 /*guid1*/, uint64 /*guid2*/)
{

}

void Channel::MakeNotifyPacket(WorldPacket* data, uint8 notify_type)
{
    data->Initialize(SMSG_CHANNEL_NOTIFY, 1 + _name.size());
    *data << uint8(notify_type);
    *data << _name;
}

void Channel::MakeJoined(WorldPacket* data, uint64 guid)
{
    MakeNotifyPacket(data, CHAT_JOINED_NOTICE);
    *data << uint64(guid);
}

void Channel::MakeLeft(WorldPacket* data, uint64 guid)
{
    MakeNotifyPacket(data, CHAT_LEFT_NOTICE);
    *data << uint64(guid);
}

void Channel::MakeYouJoined(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_YOU_JOINED_NOTICE);
    *data << uint8(GetFlags());
    *data << uint32(GetChannelId());
    *data << uint32(0);
}

void Channel::MakeYouLeft(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_YOU_LEFT_NOTICE);
    *data << uint32(GetChannelId());
    *data << uint8(IsConstant());
}

void Channel::MakeWrongPassword(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_WRONG_PASSWORD_NOTICE);
}

void Channel::MakeNotMember(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_NOT_MEMBER_NOTICE);
}

void Channel::MakeNotModerator(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_NOT_MODERATOR_NOTICE);
}

void Channel::MakePasswordChanged(WorldPacket* data, uint64 guid)
{
    MakeNotifyPacket(data, CHAT_PASSWORD_CHANGED_NOTICE);
    *data << uint64(guid);
}

void Channel::MakeOwnerChanged(WorldPacket* data, uint64 guid)
{
    MakeNotifyPacket(data, CHAT_OWNER_CHANGED_NOTICE);
    *data << uint64(guid);
}

void Channel::MakePlayerNotFound(WorldPacket* data, std::string const& name)
{
    MakeNotifyPacket(data, CHAT_PLAYER_NOT_FOUND_NOTICE);
    *data << name;
}

void Channel::MakeNotOwner(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_NOT_OWNER_NOTICE);
}

void Channel::MakeChannelOwner(WorldPacket* data)
{
    std::string name = "";

    if (!sObjectMgr->GetPlayerNameByGUID(_ownerGUID, name) || name.empty())
        name = "PLAYER_NOT_FOUND";

    MakeNotifyPacket(data, CHAT_CHANNEL_OWNER_NOTICE);
    *data << ((IsConstant() || !_ownerGUID) ? "Nobody" : name);
}

void Channel::MakeModeChange(WorldPacket* data, ObjectGuid guid, uint8 oldflags)
{
    MakeNotifyPacket(data, CHAT_MODE_CHANGE_NOTICE);
    *data << uint64(guid);
    *data << uint8(oldflags);
    *data << uint8(GetPlayerFlags(guid));
}

void Channel::MakeAnnouncementsOn(WorldPacket* data, ObjectGuid guid)
{
    MakeNotifyPacket(data, CHAT_ANNOUNCEMENTS_ON_NOTICE);
    *data << uint64(guid);
}

void Channel::MakeAnnouncementsOff(WorldPacket* data, ObjectGuid guid)
{
    MakeNotifyPacket(data, CHAT_ANNOUNCEMENTS_OFF_NOTICE);
    *data << uint64(guid);
}

void Channel::MakeMuted(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_MUTED_NOTICE);
}

void Channel::MakePlayerKicked(WorldPacket* data, uint64 bad, uint64 good)
{
    MakeNotifyPacket(data, CHAT_PLAYER_KICKED_NOTICE);
    *data << uint64(bad);
    *data << uint64(good);
}

void Channel::MakeBanned(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_BANNED_NOTICE);
}

void Channel::MakePlayerBanned(WorldPacket* data, uint64 bad, uint64 good)
{
    MakeNotifyPacket(data, CHAT_PLAYER_BANNED_NOTICE);
    *data << uint64(bad);
    *data << uint64(good);
}

void Channel::MakePlayerUnbanned(WorldPacket* data, uint64 bad, uint64 good)
{
    MakeNotifyPacket(data, CHAT_PLAYER_UNBANNED_NOTICE);
    *data << uint64(bad);
    *data << uint64(good);
}

void Channel::MakePlayerNotBanned(WorldPacket* data, const std::string &name)
{
    MakeNotifyPacket(data, CHAT_PLAYER_NOT_BANNED_NOTICE);
    *data << name;
}

void Channel::MakePlayerAlreadyMember(WorldPacket* data, uint64 guid)
{
    MakeNotifyPacket(data, CHAT_PLAYER_ALREADY_MEMBER_NOTICE);
    *data << uint64(guid);
}

void Channel::MakeInvite(WorldPacket* data, uint64 guid)
{
    MakeNotifyPacket(data, CHAT_INVITE_NOTICE);
    *data << uint64(guid);
}

void Channel::MakeInviteWrongFaction(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_INVITE_WRONG_FACTION_NOTICE);
}

void Channel::MakeWrongFaction(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_WRONG_FACTION_NOTICE);
}

void Channel::MakeInvalidName(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_INVALID_NAME_NOTICE);
}

void Channel::MakeNotModerated(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_NOT_MODERATED_NOTICE);
}

void Channel::MakePlayerInvited(WorldPacket* data, const std::string& name)
{
    MakeNotifyPacket(data, CHAT_PLAYER_INVITED_NOTICE);
    *data << name;
}

void Channel::MakePlayerInviteBanned(WorldPacket* data, const std::string& name)
{
    MakeNotifyPacket(data, CHAT_PLAYER_INVITE_BANNED_NOTICE);
    *data << name;
}

void Channel::MakeThrottled(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_THROTTLED_NOTICE);
}

void Channel::MakeNotInArea(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_NOT_IN_AREA_NOTICE);
}

void Channel::MakeNotInLfg(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_NOT_IN_LFG_NOTICE);
}

void Channel::MakeVoiceOn(WorldPacket* data, uint64 guid)
{
    MakeNotifyPacket(data, CHAT_VOICE_ON_NOTICE);
    *data << uint64(guid);
}

void Channel::MakeVoiceOff(WorldPacket* data, uint64 guid)
{
    MakeNotifyPacket(data, CHAT_VOICE_OFF_NOTICE);
    *data << uint64(guid);
}

void Channel::JoinNotify(ObjectGuid guid)
{
    ObjectGuid plrGuid = guid;
    if (IsConstant())
    {
        WorldPacket data(SMSG_USERLIST_ADD);
        data << uint32(GetNumPlayers());
        data << uint8(GetFlags());
        data << uint8(GetPlayerFlags(guid));

        data.WriteBit(plrGuid[7]);
        data.WriteBits(GetName().size(), 7);
        data.WriteBit(plrGuid[0]);
        data.WriteBit(plrGuid[5]);
        data.WriteBit(plrGuid[4]);
        data.WriteBit(plrGuid[6]);
        data.WriteBit(plrGuid[1]);
        data.WriteBit(plrGuid[3]);
        data.WriteBit(plrGuid[2]);

        data.FlushBits();

        data.WriteByteSeq(plrGuid[4]);
        data.WriteByteSeq(plrGuid[5]);
        data.WriteByteSeq(plrGuid[7]);
        data.WriteByteSeq(plrGuid[1]);
        data.WriteByteSeq(plrGuid[2]);
        data.WriteByteSeq(plrGuid[3]);
        data.WriteByteSeq(plrGuid[6]);
        data.WriteByteSeq(plrGuid[0]);
        data.WriteString(GetName());

        SendToAllButOne(&data, guid);
    }
    else
    {
        WorldPacket data(SMSG_USERLIST_UPDATE);
        data.WriteBit(plrGuid[2]);
        data.WriteBit(plrGuid[6]);
        data.WriteBit(plrGuid[3]);
        data.WriteBit(plrGuid[7]);
        data.WriteBit(plrGuid[5]);
        data.WriteBit(plrGuid[1]);
        data.WriteBit(plrGuid[0]);
        data.WriteBits(GetName().size(), 7);
        data.WriteBit(plrGuid[4]);

        data.FlushBits();

        data.WriteByteSeq(plrGuid[0]);
        data.WriteByteSeq(plrGuid[2]);
        data.WriteByteSeq(plrGuid[6]);
        data.WriteByteSeq(plrGuid[5]);
        data << uint8(GetFlags());
        data.WriteByteSeq(plrGuid[7]);
        data.WriteByteSeq(plrGuid[3]);
        data << uint32(GetNumPlayers());
        data.WriteString(GetName());
        data.WriteByteSeq(plrGuid[1]);
        data.WriteByteSeq(plrGuid[4]);
        data << uint8(GetPlayerFlags(guid));

        SendToAll(&data);
    }
}

void Channel::LeaveNotify(ObjectGuid guid)
{
    ObjectGuid plrGuid = guid;

    WorldPacket data(SMSG_USERLIST_REMOVE);
    data.WriteBits(GetName().size(), 7);
    data.WriteBit(plrGuid[6]);
    data.WriteBit(plrGuid[0]);
    data.WriteBit(plrGuid[5]);
    data.WriteBit(plrGuid[4]);
    data.WriteBit(plrGuid[3]);
    data.WriteBit(plrGuid[1]);
    data.WriteBit(plrGuid[7]);
    data.WriteBit(plrGuid[2]);

    data.FlushBits();

    data.WriteByteSeq(plrGuid[1]);
    data.WriteByteSeq(plrGuid[4]);
    data << uint8(GetFlags());
    data.WriteByteSeq(plrGuid[6]);
    data.WriteByteSeq(plrGuid[5]);
    data << uint32(GetNumPlayers());
    data.WriteByteSeq(plrGuid[2]);
    data.WriteByteSeq(plrGuid[7]);
    data.WriteByteSeq(plrGuid[0]);
    data.WriteString(GetName());
    data.WriteByteSeq(plrGuid[3]);

    if (IsConstant())
        SendToAllButOne(&data, guid);
    else
        SendToAll(&data);
}
