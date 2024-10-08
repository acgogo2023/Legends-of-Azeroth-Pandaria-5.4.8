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

#ifndef SC_ESCORTAI_H
#define SC_ESCORTAI_H

#include "ScriptSystem.h"

#define DEFAULT_MAX_PLAYER_DISTANCE 50

struct Escort_Waypoint
{
    Escort_Waypoint(uint32 _id, float _x, float _y, float _z, uint32 _w, bool _j)
    {
        id = _id;
        x = _x;
        y = _y;
        z = _z;
        WaitTimeMs = _w;
        jump = _j;
    }

    uint32 id;
    float x;
    float y;
    float z;
    uint32 WaitTimeMs;
    bool jump;
};

enum eEscortState : uint32
{
    STATE_ESCORT_NONE       = 0x000,                        //nothing in progress
    STATE_ESCORT_ESCORTING  = 0x001,                        //escort are in progress
    STATE_ESCORT_RETURNING  = 0x002,                        //escort is returning after being in combat
    STATE_ESCORT_PAUSED     = 0x004,                        //will not proceed with waypoints before state is removed
    STATE_ESCORT_JUMPING    = 0x008
};

struct npc_escortAI : public ScriptedAI
{
    public:
        explicit npc_escortAI(Creature* creature);
        ~npc_escortAI() { }

        // CreatureAI functions
        void AttackStart(Unit* who) override;
        void MoveInLineOfSight(Unit* who) override;
        void JustDied(Unit*) override;
        void JustAppeared() override;
        void ReturnToLastPoint();
        void EnterEvadeMode() override;
        void UpdateAI(uint32 diff) override;                   //the "internal" update, calls UpdateEscortAI()
        void MovementInform(uint32, uint32) override;
        
        virtual void UpdateEscortAI(uint32 const diff);     //used when it's needed to add code in update (abilities, scripted events, etc)
        // EscortAI functions
        void AddWaypoint(uint32 id, float x, float y, float z, uint32 waitTime = 0, bool jump = false);    // waitTime is in ms
        //this will set the current position to x/y/z/o, and the current WP to pointId.
        bool SetNextWaypoint(uint32 pointId, float x, float y, float z, float orientation);
        //this will set the current position to WP start position (if setPosition == true),
        //and the current WP to pointId
        bool SetNextWaypoint(uint32 pointId, bool setPosition = true, bool resetWaypointsOnFail = true);

        bool GetWaypointPosition(uint32 pointId, float& x, float& y, float& z);

        virtual void WaypointReached(uint32 pointId) = 0;
        virtual void WaypointStart(uint32 /*pointId*/) { }

        void Start(bool isActiveAttacker = true, bool run = false, ObjectGuid playerGUID = ObjectGuid::Empty, Quest const* quest = NULL, bool instantRespawn = false, bool canLoopPath = false, bool resetWaypoints = true);

        void SetRun(bool on = true);
        void SetEscortPaused(bool on);

        bool HasEscortState(uint32 escortState) { return (m_uiEscortState & escortState); }
        virtual bool IsEscorted() override { return (m_uiEscortState & STATE_ESCORT_ESCORTING); }
        void SetMaxPlayerDistance(float newMax) { MaxPlayerDistance = newMax; }
        float GetMaxPlayerDistance() { return MaxPlayerDistance; }
        void SetDespawnAtEnd(bool despawn) { DespawnAtEnd = despawn; }
        void SetDespawnAtFar(bool despawn) { DespawnAtFar = despawn; }
        bool GetAttack() { return m_bIsActiveAttacker; }//used in EnterEvadeMode override
        void SetCanAttack(bool attack) { m_bIsActiveAttacker = attack; }
        uint64 GetEventStarterGUID() { return m_uiPlayerGUID; }
        void SetSpeedXY(float speed) { speedXY = speed; }
        void SetSpeedZ(float speed) { speedZ = speed; }

    protected:
        Player* GetPlayerForEscort();

    private:
        bool AssistPlayerInCombat(Unit* who);
        bool IsPlayerOrGroupInRange();
        void FillPointMovementListForCreature();

        void AddEscortState(uint32 escortState) { m_uiEscortState |= escortState; }
        void RemoveEscortState(uint32 escortState) { m_uiEscortState &= ~escortState; }

        ObjectGuid m_uiPlayerGUID;
        uint32 m_uiWPWaitTimer;
        uint32 m_uiPlayerCheckTimer;
        uint32 m_uiEscortState;
        float MaxPlayerDistance;
        float speedXY;
        float speedZ;

        Quest const* m_pQuestForEscort;                     //generally passed in Start() when regular escort script.

        std::list<Escort_Waypoint> WaypointList;
        std::list<Escort_Waypoint>::iterator CurrentWP;

        bool m_bIsActiveAttacker;                           //obsolete, determined by faction.
        bool m_bIsRunning;                                  //all creatures are walking by default (has flag MOVEMENTFLAG_WALK)
        bool m_bCanInstantRespawn;                          //if creature should respawn instantly after escort over (if not, database respawntime are used)
        bool m_bCanReturnToStart;                           //if creature can walk same path (loop) without despawn. Not for regular escort quests.
        bool DespawnAtEnd;
        bool DespawnAtFar;
        bool ScriptWP;
        bool HasImmuneToNPCFlags;
};
#endif
