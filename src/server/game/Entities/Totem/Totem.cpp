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

#include "Totem.h"
#include "Group.h"
#include "Opcodes.h"
#include "Player.h"
#include "SpellMgr.h"
#include "SpellInfo.h"

Totem::Totem(SummonPropertiesEntry const* properties, Unit* owner) : Minion(properties, owner, false)
{
    m_unitTypeMask |= UNIT_MASK_TOTEM;
    m_duration = 0;
    m_type = TOTEM_PASSIVE;
}

void Totem::Update(uint32 time)
{
    if (!GetOwner()->IsAlive() || !IsAlive())
    {
        UnSummon();                                         // remove self
        return;
    }

    Creature::Update(time);

    if (m_duration <= time)
    {
        UnSummon();                                         // remove self
        return;
    }
    else
        m_duration -= time;
}

void Totem::InitStats(uint32 duration)
{
    // client requires SMSG_TOTEM_CREATED to be sent before adding to world and before removing old totem
    if (GetOwner()->GetTypeId() == TYPEID_PLAYER && m_slot >= SUMMON_SLOT_TOTEM && m_slot < SUMMON_SLOT_MAX_TOTEM)
    {
        // set display id depending on caster's race
        if (GetOwner()->GetClass() == CLASS_SHAMAN)
        {
            uint32 modelId = GetOwner()->GetModelForTotem(PlayerTotemType(m_Properties->Id));
            if (modelId)
                SetDisplayId(modelId);
        }
    }

    Minion::InitStats(duration);

    // Get spell cast by totem
    if (SpellInfo const* totemSpell = sSpellMgr->GetSpellInfo(GetSpell()))
        if (totemSpell->CalcCastTime(GetLevel()))   // If spell has cast time -> its an active totem
            m_type = TOTEM_ACTIVE;

    m_duration = duration;

    SetLevel(GetOwner()->GetLevel());
}

void Totem::InitSummon()
{
    LoadCreaturesAddon();

    if (GetSpell() == 8178)
        if (Unit* shaman = GetSummoner())
            if (shaman->HasAura(55441)) // Glyph of Grounding Totem
                m_spells[0] = 89523;

    if (m_type == TOTEM_PASSIVE && GetSpell())
        CastSpell(this, GetSpell(), true);

    // Some totems can have both instant effect and passive spell
    if (GetSpell(1))
        CastSpell(this, GetSpell(1), true);
}

void Totem::UnSummon(uint32 msTime)
{
    if (msTime)
    {
        m_Events.AddEvent(new ForcedUnsummonDelayEvent(*this), m_Events.CalculateTime(msTime));
        return;
    }

    CombatStop();
    RemoveAurasDueToSpell(GetSpell(), GetGUID());

    // clear owner's totem slot
    for (uint8 i = SUMMON_SLOT_TOTEM; i < SUMMON_SLOT_MAX_TOTEM; ++i)
    {
        if (GetOwner()->m_SummonSlot[i] == GetGUID())
        {
            GetOwner()->m_SummonSlot[i].Clear();
            break;
        }
    }

    GetOwner()->RemoveAurasDueToSpell(GetSpell(), GetGUID());

    // remove aura all party members too
    if (Player* owner = GetOwner()->ToPlayer())
    {
        owner->SendAutoRepeatCancel(this);

        // This is probably uselss
        if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(GetUInt32Value(UNIT_FIELD_CREATED_BY_SPELL)))
            if (spell->IsCooldownStartedOnEvent())
                owner->SendCooldownEvent(spell);

        if (Group* group = owner->GetGroup())
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
            {
                Player* target = itr->GetSource();
                if (target && group->SameSubGroup(owner, target))
                    target->RemoveAurasDueToSpell(GetSpell(), GetGUID());
            }
        }
    }

    AddObjectToRemoveList();
}

bool Totem::IsImmunedToSpellEffect(SpellInfo const* spellInfo, uint32 index) const
{
    /// @todo possibly all negative auras immune?
    if (GetEntry() == 5925)
        return false;

    switch (spellInfo->Effects[index].ApplyAuraName)
    {
        case SPELL_AURA_PERIODIC_DAMAGE:
        case SPELL_AURA_PERIODIC_LEECH:
        case SPELL_AURA_PERIODIC_HEAL:
        case SPELL_AURA_MOD_FEAR:
        case SPELL_AURA_MOD_FEAR_2:
        case SPELL_AURA_TRANSFORM:
        case SPELL_AURA_MOD_STUN:
        case SPELL_AURA_MOD_CONFUSE:
        case SPELL_AURA_MOD_SILENCE:
        case SPELL_AURA_MOD_ROOT:
            return true;
        default:
            break;
    }

    switch (spellInfo->Effects[index].Effect)
    {
        case SPELL_EFFECT_HEAL:
        case SPELL_EFFECT_HEAL_PCT:
        case SPELL_EFFECT_HEAL_MAX_HEALTH:
            return true;
        default:
            break;
    }

    return Creature::IsImmunedToSpellEffect(spellInfo, index);
}
