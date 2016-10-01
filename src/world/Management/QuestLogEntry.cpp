/*
* AscEmu Framework based on ArcEmu MMORPG Server
* Copyright (C) 2014-2016 AscEmu Team <http://www.ascemu.org/>
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU Affero General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU Affero General Public License for more details.
*
* You should have received a copy of the GNU Affero General Public License
* along with this program. If not, see <http://www.gnu.org/licenses/>.
*
*/
#include "StdAfx.h"
#include <Log.h>
#include "QuestLogEntry.hpp"
#include "Server/WorldSession.h"
#include "Server/MainServerDefines.h"
#include "Database/Database.h"
#include "Management/ItemInterface.h"
#include "QuestMgr.h"

//////////////////////////////////////////////////////////////////////////////////////////
/// QuestLogEntry
//////////////////////////////////////////////////////////////////////////////////////////
QuestLogEntry::QuestLogEntry()
{
    mInitialized = false;
    m_quest = nullptr;
    mDirty = false;
    m_slot = -1;
    completed = 0;
    m_plr = nullptr;
    iscastquest = false;
    isemotequest = false;
    expirytime = 0;
}

QuestLogEntry::~QuestLogEntry()
{
    
}

void QuestLogEntry::Init(QuestProperties const* quest, Player* plr, uint32 slot)
{
    ARCEMU_ASSERT(quest != nullptr);
    ARCEMU_ASSERT(plr != nullptr);

    m_quest = quest;
    m_plr = plr;
    m_slot = slot;

    iscastquest = false;
    isemotequest = false;
    for (uint8 i = 0; i < 4; ++i)
    {
        if (quest->ReqSpell[i] != 0)
        {
            iscastquest = true;
            if (!plr->HasQuestSpell(quest->ReqSpell[i]))
                plr->quest_spells.insert(quest->ReqSpell[i]);
        }

        if (quest->ReqCreatureOrGOId[i] != 0)
        {
            if (!plr->HasQuestMob(quest->ReqCreatureOrGOId[i]))
                plr->quest_mobs.insert(quest->ReqCreatureOrGOId[i]);
        }
    }

    if (quest->ReqEmoteId)
    {
        isemotequest = true;
    }

    // update slot
    plr->SetQuestLogSlot(this, slot);

    mDirty = true;

    /*memset(m_mobcount, 0, 4 * 4);
    memset(m_explored_areas, 0, 3 * 3);*/

    if (m_quest->GetLimitTime() > 0)
        expirytime = UNIXTIME + m_quest->GetLimitTime() / 1000;
    else
        expirytime = 0;

    if (!plr->GetSession()->m_loggingInPlayer)                  //quest script should not be called on login
        CALL_QUESTSCRIPT_EVENT(this, OnQuestStart)(plr, this);
}

void QuestLogEntry::ClearAffectedUnits()
{
    if (m_affected_units.size() > 0)
        m_affected_units.clear();
}

void QuestLogEntry::AddAffectedUnit(Unit* target)
{
    if (!target)
        return;
    if (!IsUnitAffected(target))
        m_affected_units.insert(target->GetGUID());
}

bool QuestLogEntry::IsUnitAffected(Unit* target)
{
    if (!target)
        return true;
    if (m_affected_units.find(target->GetGUID()) != m_affected_units.end())
        return true;
    return false;
}

void QuestLogEntry::SaveToDB(QueryBuffer* buf)
{
    ARCEMU_ASSERT(m_slot != -1);
    if (!mDirty)
        return;

    std::stringstream ss;

    ss << "DELETE FROM questlog WHERE player_guid = ";
    ss << m_plr->GetLowGUID();
    ss << " AND quest_id = ";
    ss << m_quest->GetQuestId();
    ss << ";";

    if (buf == nullptr)
        CharacterDatabase.Execute(ss.str().c_str());
    else
        buf->AddQueryStr(ss.str());

    ss.rdbuf()->str("");

    ss << "INSERT INTO questlog VALUES(";
    ss << m_plr->GetLowGUID() << "," << m_quest->GetQuestId() << "," << m_slot << "," << expirytime;
    for (uint8 i = 0; i < QUEST_REQUIRED_AREA_TRIGGERS; ++i)
        ss << "," << m_explored_areas[i];

    ss << "," << 0; //unused 4th areatrigger value!

    for (uint8 i = 0; i < 4; ++i)
        ss << "," << m_mobcount[i];

    ss << "," << uint32(completed);

    ss << ")";

    if (buf == nullptr)
        CharacterDatabase.Execute(ss.str().c_str());
    else
        buf->AddQueryStr(ss.str());
}

bool QuestLogEntry::LoadFromDB(Field* fields)
{
    // playerguid,questid,timeleft,area0,area1,area2,area3,kill0,kill1,kill2,kill3
    ARCEMU_ASSERT(m_plr && m_quest);
    expirytime = fields[3].GetUInt32();

    for (uint8 i = 0; i < QUEST_REQUIRED_AREA_TRIGGERS; ++i)
    {
        m_explored_areas[i] = fields[4 + i].GetUInt32();
        CALL_QUESTSCRIPT_EVENT(this, OnExploreArea)(m_explored_areas[i], m_plr, this);
    }

    for (uint8 i = 0; i < 4; ++i)
    {
        m_mobcount[i] = fields[8 + i].GetUInt32();
        if (GetQuest()->m_reqMobType[i] == QUEST_MOB_TYPE_CREATURE)
        {
            CALL_QUESTSCRIPT_EVENT(this, OnCreatureKill)(GetQuest()->ReqCreatureOrGOId[i], m_plr, this);
        }
        else
        {
            CALL_QUESTSCRIPT_EVENT(this, OnGameObjectActivate)(GetQuest()->ReqCreatureOrGOId[i], m_plr, this);
        }
    }

    completed = fields[12].GetUInt32();

    mDirty = false;
    return true;
}

bool QuestLogEntry::CanBeFinished()
{
    //\todo danko
    /*if (m_quest->iscompletedbyspelleffect && (completed == QUEST_INCOMPLETE))
        return false;*/

    if (completed == QUEST_FAILED)
        return false;
    else
        if (completed == QUEST_COMPLETE)
            return true;

    for (uint8 i = 0; i < 4; ++i)
    {
        if (m_quest->ReqCreatureOrGOId[i])
        {
            if (m_mobcount[i] < m_quest->ReqCreatureOrGOCount[i])
            {
                return false;
            }
        }
        if (m_quest->ReqSpell[i])   // requires spell cast, with no required target
        {
            if (m_mobcount[i] == 0 || m_mobcount[i] < m_quest->ReqCreatureOrGOCount[i])
            {
                return false;
            }
        }

        if (m_quest->ReqEmoteId)   // requires emote, with no required target
        {
            if (m_mobcount[i] == 0 || m_mobcount[i] < m_quest->ReqCreatureOrGOCount[i])
            {
                return false;
            }
        }
    }

    for (uint8 i = 0; i < MAX_REQUIRED_QUEST_ITEM; ++i)
    {
        if (m_quest->ReqItemId[i])
        {
            if (m_plr->GetItemInterface()->GetItemCount(m_quest->ReqItemId[i]) < m_quest->ReqItemCount[i])
            {
                return false;
            }
        }
    }

    //Check for Gold & AreaTrigger Requirements
    if (m_quest->GetRewOrReqMoney() < 0 && m_plr->GetGold() < uint32(-m_quest->GetRewOrReqMoney()))
        return false;

    for (uint8 i = 0; i < QUEST_REQUIRED_AREA_TRIGGERS; ++i)
    {
        if (m_quest->m_reqExploreTrigger[i])
        {
            if (m_explored_areas[i] == 0)
                return false;
        }
    }

    return true;
}

void QuestLogEntry::SetMobCount(uint32 i, uint32 count)
{
    ARCEMU_ASSERT(i < 4);
    m_mobcount[i] = count;
    mDirty = true;
}

void QuestLogEntry::IncrementMobCount(uint32 i)
{
    ARCEMU_ASSERT(i < 4);
    ++m_mobcount[i];
    mDirty = true;
}

void QuestLogEntry::SetTrigger(uint32 i)
{
    ARCEMU_ASSERT(i < QUEST_REQUIRED_AREA_TRIGGERS);
    m_explored_areas[i] = 1;
    mDirty = true;
}

void QuestLogEntry::SetSlot(int32 i)
{
    ARCEMU_ASSERT(i != -1);
    m_slot = i;
}

void QuestLogEntry::Finish()
{
    sEventMgr.RemoveEvents(m_plr, EVENT_TIMED_QUEST_EXPIRE);

    uint32 base = GetBaseField(m_slot);
    m_plr->SetUInt32Value(base + 0, 0);
    m_plr->SetUInt32Value(base + 1, 0);
    m_plr->SetUInt64Value(base + 2, 0);
    m_plr->SetUInt32Value(base + 4, 0);

    // clear from player log
    m_plr->SetQuestLogSlot(nullptr, m_slot);
    m_plr->PushToRemovedQuests(m_quest->GetQuestId());
    m_plr->UpdateNearbyGameObjects();

    // delete ourselves
    delete this;
}

void QuestLogEntry::Fail(bool timerexpired)
{
    sEventMgr.RemoveEvents(m_plr, EVENT_TIMED_QUEST_EXPIRE);

    completed = QUEST_FAILED;
    expirytime = 0;
    mDirty = true;

    uint32 base = GetBaseField(m_slot);
    m_plr->SetUInt32Value(base + 1, 2);

    if (timerexpired)
        sQuestMgr.SendQuestUpdateFailedTimer(m_quest, m_plr);
    else
        sQuestMgr.SendQuestUpdateFailed(m_quest, m_plr);
}

bool QuestLogEntry::HasFailed()
{
    return false;
}

void QuestLogEntry::UpdatePlayerFields()
{
    if (!m_plr)
        return;

    uint32 base = GetBaseField(m_slot);
    m_plr->SetUInt32Value(base + 0, m_quest->GetQuestId());
    uint32 field0 = 0;          // 0x01000000 = "Objective Complete" - 0x02 = Quest Failed - 0x04 = Quest Accepted

    // next field is count (kills, etc)
    uint64 field1 = 0;

    // explored areas
    uint32 count = 0;
    for (uint8 i = 0; i < QUEST_REQUIRED_AREA_TRIGGERS; ++i)
    {
        if (m_quest->m_reqExploreTrigger[i])
        {
            if (m_explored_areas[i] == 1)
            {
                ++count;
            }
        }
    }

    if (count > 0)
    {
        field1 |= 0x01000000;
    }


    // spell casts / emotes
    if (iscastquest)
    {
        bool cast_complete = true;
        for (uint8 i = 0; i < 4; ++i)
        {
            if (m_quest->ReqSpell[i] && m_quest->ReqCreatureOrGOCount[i] > m_mobcount[i])
            {
                cast_complete = false;
                break;
            }
        }
        if (cast_complete)
        {
            field0 |= 0x01000000;       // "Objective Complete"
        }
    }
    else if (isemotequest)
    {
        bool emote_complete = true;
        for (uint8 i = 0; i < 4; ++i)
        {
            if (m_quest->ReqEmoteId && m_quest->ReqCreatureOrGOCount[i] > m_mobcount[i])
            {
                emote_complete = false;
                break;
            }
        }
        if (emote_complete)
        {
            field0 |= 0x01000000;       // "Objective Complete"
        }
    }

    // mob hunting / counter
    if (m_quest->ReqCurrencyCount[0])
    {
        uint8* p = (uint8*)&field1;
        for (uint8 i = 0; i < 4; ++i)
        {
            if (m_quest->ReqCreatureOrGOId[i] && m_mobcount[i] > 0)
                p[2 * i] |= (uint8)m_mobcount[i];
        }
    }

    if ((m_quest->GetLimitTime() != 0) && (expirytime < UNIXTIME))
        completed = QUEST_FAILED;

    if (completed == QUEST_FAILED)
        field0 |= 2;

    m_plr->SetUInt32Value(base + 1, field0);
    m_plr->SetUInt64Value(base + 2, field1);

    if ((m_quest->GetLimitTime() != 0) && (completed != QUEST_FAILED))
    {
        m_plr->SetUInt32Value(base + 4, expirytime);
        sEventMgr.AddEvent(m_plr, &Player::EventTimedQuestExpire, m_quest->GetQuestId(), EVENT_TIMED_QUEST_EXPIRE, (expirytime - UNIXTIME) * 1000, 1, EVENT_FLAG_DO_NOT_EXECUTE_IN_WORLD_CONTEXT);
    }
    else
        m_plr->SetUInt32Value(base + 4, 0);
}

void QuestLogEntry::SendQuestComplete()
{
    WorldPacket data(4);

    data.SetOpcode(SMSG_QUESTUPDATE_COMPLETE);
    data << m_quest->GetQuestId();

    m_plr->GetSession()->SendPacket(&data);
    m_plr->UpdateNearbyGameObjects();
    CALL_QUESTSCRIPT_EVENT(this, OnQuestComplete)(m_plr, this);
}

void QuestLogEntry::SendUpdateAddKill(uint32 i)
{
    sQuestMgr.SendQuestUpdateAddKill(m_plr, m_quest->GetQuestId(), m_quest->ReqCreatureOrGOId[i], m_mobcount[i], m_quest->ReqCreatureOrGOCount[i], 0);
}

void QuestLogEntry::Complete()
{
    completed = QUEST_COMPLETE;
}
