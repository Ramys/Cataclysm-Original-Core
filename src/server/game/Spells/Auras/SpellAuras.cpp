/*
 * Copyright (C) 2008-2013 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
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

#include "Common.h"
#include "WorldPacket.h"
#include "Opcodes.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "Player.h"
#include "Unit.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "DynamicObject.h"
#include "ObjectAccessor.h"
#include "Util.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "ScriptMgr.h"
#include "SpellScript.h"
#include "Vehicle.h"
#include "Pet.h"

AuraApplication::AuraApplication(Unit* target, Unit* caster, Aura* aura, uint8 effMask):
_target(target), _base(aura), _removeMode(AURA_REMOVE_NONE), _slot(MAX_AURAS),
_flags(AFLAG_NONE), _effectsToApply(effMask), _needClientUpdate(false)
{
    ASSERT(GetTarget() && GetBase());

    if (GetBase()->CanBeSentToClient())
    {
        // Try find slot for aura
        uint8 slot = MAX_AURAS;
        // Lookup for auras already applied from spell
        if (AuraApplication * foundAura = GetTarget()->GetAuraApplication(GetBase()->GetId(), GetBase()->GetCasterGUID(), GetBase()->GetCastItemGUID()))
        {
            // allow use single slot only by auras from same caster
            slot = foundAura->GetSlot();
        }
        else
        {
            Unit::VisibleAuraMap const* visibleAuras = GetTarget()->GetVisibleAuras();
            // lookup for free slots in units visibleAuras
            Unit::VisibleAuraMap::const_iterator itr = visibleAuras->find(0);
            for (uint32 freeSlot = 0; freeSlot < MAX_AURAS; ++itr, ++freeSlot)
            {
                if (itr == visibleAuras->end() || itr->first != freeSlot)
                {
                    slot = freeSlot;
                    break;
                }
            }
        }

        // Register Visible Aura
        if (slot < MAX_AURAS)
        {
            _slot = slot;
            GetTarget()->SetVisibleAura(slot, this);
            SetNeedClientUpdate();
            TC_LOG_DEBUG("spells", "Aura: %u Effect: %d put to unit visible auras slot: %u", GetBase()->GetId(), GetEffectMask(), slot);
        }
        else
            TC_LOG_DEBUG("spells", "Aura: %u Effect: %d could not find empty unit visible slot", GetBase()->GetId(), GetEffectMask());
    }

    _InitFlags(caster, effMask);
}

void AuraApplication::_Remove()
{
    uint8 slot = GetSlot();

    if (slot >= MAX_AURAS)
        return;

    if (AuraApplication * foundAura = _target->GetAuraApplication(GetBase()->GetId(), GetBase()->GetCasterGUID(), GetBase()->GetCastItemGUID()))
    {
        // Reuse visible aura slot by aura which is still applied - prevent storing dead pointers
        if (slot == foundAura->GetSlot())
        {
            if (GetTarget()->GetVisibleAura(slot) == this)
            {
                GetTarget()->SetVisibleAura(slot, foundAura);
                foundAura->SetNeedClientUpdate();
            }
            // set not valid slot for aura - prevent removing other visible aura
            slot = MAX_AURAS;
        }
    }

    // update for out of range group members
    if (slot < MAX_AURAS)
    {
        GetTarget()->RemoveVisibleAura(slot);
        ClientUpdate(true);
    }
}

void AuraApplication::_InitFlags(Unit* caster, uint8 effMask)
{
    // mark as selfcasted if needed
    _flags |= (GetBase()->GetCasterGUID() == GetTarget()->GetGUID()) ? AFLAG_CASTER : AFLAG_NONE;

    // aura is casted by self or an enemy
    // one negative effect and we know aura is negative
    if (IsSelfcasted() || !caster || !caster->IsFriendlyTo(GetTarget()))
    {
        bool negativeFound = false;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (((1<<i) & effMask) && !GetBase()->GetSpellInfo()->IsPositiveEffect(i))
            {
                negativeFound = true;
                break;
            }
        }
        _flags |= negativeFound ? AFLAG_NEGATIVE : AFLAG_POSITIVE;
    }
    // aura is casted by friend
    // one positive effect and we know aura is positive
    else
    {
        bool positiveFound = false;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (((1<<i) & effMask) && GetBase()->GetSpellInfo()->IsPositiveEffect(i))
            {
                positiveFound = true;
                break;
            }
        }
        _flags |= positiveFound ? AFLAG_POSITIVE : AFLAG_NEGATIVE;
    }

    if (GetBase()->GetSpellInfo()->AttributesEx8 & SPELL_ATTR8_AURA_SEND_AMOUNT || GetBase()->HasEffectType(SPELL_AURA_OVERRIDE_ACTIONBAR_SPELLS))
        _flags |= AFLAG_ANY_EFFECT_AMOUNT_SENT;
}

void AuraApplication::_HandleEffect(uint8 effIndex, bool apply)
{
    AuraEffect* aurEff = GetBase()->GetEffect(effIndex);
    ASSERT(aurEff);
    ASSERT(HasEffect(effIndex) == (!apply));
    ASSERT((1<<effIndex) & _effectsToApply);
    TC_LOG_DEBUG("spells", "AuraApplication::_HandleEffect: %u, apply: %u: amount: %d", aurEff->GetAuraType(), apply, aurEff->GetAmount());

    if (apply)
    {
        ASSERT(!(_flags & (1<<effIndex)));
        _flags |= 1<<effIndex;
        aurEff->HandleEffect(this, AURA_EFFECT_HANDLE_REAL, true);
    }
    else
    {
        ASSERT(_flags & (1<<effIndex));
        _flags &= ~(1<<effIndex);
        aurEff->HandleEffect(this, AURA_EFFECT_HANDLE_REAL, false);

        // Remove all triggered by aura spells vs unlimited duration
        aurEff->CleanupTriggeredSpells(GetTarget());
    }
    SetNeedClientUpdate();
}

void AuraApplication::BuildUpdatePacket(ByteBuffer& data, bool remove) const
{
    data << uint8(_slot);

    if (remove)
    {
        ASSERT(!_target->GetVisibleAura(_slot));
        data << uint32(0);
        return;
    }
    ASSERT(_target->GetVisibleAura(_slot));

    Aura const* aura = GetBase();
    data << uint32(aura->GetId());
    uint32 flags = _flags;
    if (aura->GetMaxDuration() > 0 && !(aura->GetSpellInfo()->AttributesEx5 & SPELL_ATTR5_HIDE_DURATION))
        flags |= AFLAG_DURATION;
    data << uint16(flags);
    data << uint8(aura->GetCasterLevel());
    // send stack amount for aura which could be stacked (never 0 - causes incorrect display) or charges
    // stack amount has priority over charges (checked on retail with spell 50262)
    data << uint8(aura->GetSpellInfo()->StackAmount ? aura->GetStackAmount() : aura->GetCharges());

    if (!(flags & AFLAG_CASTER))
        data.appendPackGUID(aura->GetCasterGUID());

    if (flags & AFLAG_DURATION)
    {
        data << uint32(aura->GetMaxDuration());
        data << uint32(aura->GetDuration());
    }

    if (flags & AFLAG_ANY_EFFECT_AMOUNT_SENT)
        for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (AuraEffect const* eff = aura->GetEffect(i)) // NULL if effect flag not set
                if (flags & (1 << i))
                    data << int32(eff->GetAmount());
}

void AuraApplication::ClientUpdate(bool remove)
{
    _needClientUpdate = false;

    WorldPacket data(SMSG_AURA_UPDATE);
    data.append(GetTarget()->GetPackGUID());
    BuildUpdatePacket(data, remove);

    if (Player *pl = GetTarget()->ToPlayer())
        pl->SendArenaSpectatorAura(this, remove);

    _target->SendMessageToSet(&data, true);
}

uint8 Aura::BuildEffectMaskForOwner(SpellInfo const* spellProto, uint8 avalibleEffectMask, WorldObject* owner)
{
    ASSERT(spellProto);
    ASSERT(owner);
    uint8 effMask = 0;
    switch (owner->GetTypeId())
    {
        case TYPEID_UNIT:
        case TYPEID_PLAYER:
            for (uint8 i = 0; i< MAX_SPELL_EFFECTS; ++i)
            {
                if (spellProto->Effects[i].IsUnitOwnedAuraEffect())
                    effMask |= 1 << i;
            }
            break;
        case TYPEID_DYNAMICOBJECT:
            for (uint8 i = 0; i< MAX_SPELL_EFFECTS; ++i)
            {
                if (spellProto->Effects[i].Effect == SPELL_EFFECT_PERSISTENT_AREA_AURA)
                    effMask |= 1 << i;
            }
            break;
        default:
            break;
    }
    return effMask & avalibleEffectMask;
}

Aura* Aura::TryRefreshStackOrCreate(SpellInfo const* spellproto, uint8 tryEffMask, WorldObject* owner, Unit* caster, int32* baseAmount /*= NULL*/, Item* castItem /*= NULL*/, uint64 casterGUID /*= 0*/, bool* refresh /*= NULL*/)
{
    ASSERT(spellproto);
    ASSERT(owner);
    ASSERT(caster || casterGUID);
    if (tryEffMask > MAX_EFFECT_MASK)
        return NULL;

    if (refresh)
        *refresh = false;
    uint8 effMask = Aura::BuildEffectMaskForOwner(spellproto, tryEffMask, owner);
    if (!effMask)
        return NULL;
    if (Aura* foundAura = owner->ToUnit()->_TryStackingOrRefreshingExistingAura(spellproto, effMask, caster, baseAmount, castItem, casterGUID))
    {
        // we've here aura, which script triggered removal after modding stack amount
        // check the state here, so we won't create new Aura object
        if (foundAura->IsRemoved())
            return NULL;

        if (refresh)
            *refresh = true;
        return foundAura;
    }
    else
        return Create(spellproto, effMask, owner, caster, baseAmount, castItem, casterGUID);
}

Aura* Aura::TryCreate(SpellInfo const* spellproto, uint8 tryEffMask, WorldObject* owner, Unit* caster, int32* baseAmount /*= NULL*/, Item* castItem /*= NULL*/, uint64 casterGUID /*= 0*/)
{
    ASSERT(spellproto);
    ASSERT(owner);
    ASSERT(caster || casterGUID);
    ASSERT(tryEffMask <= MAX_EFFECT_MASK);
    uint8 effMask = Aura::BuildEffectMaskForOwner(spellproto, tryEffMask, owner);
    if (!effMask)
        return NULL;
    return Create(spellproto, effMask, owner, caster, baseAmount, castItem, casterGUID);
}

Aura* Aura::Create(SpellInfo const* spellproto, uint8 effMask, WorldObject* owner, Unit* caster, int32* baseAmount, Item* castItem, uint64 casterGUID)
{
    ASSERT(effMask);
    ASSERT(spellproto);
    ASSERT(owner);
    ASSERT(caster || casterGUID);
    ASSERT(effMask <= MAX_EFFECT_MASK);
    // try to get caster of aura
    if (casterGUID)
    {
        if (owner->GetGUID() == casterGUID)
            caster = owner->ToUnit();
        else
            caster = ObjectAccessor::GetUnit(*owner, casterGUID);
    }
    else
        casterGUID = caster->GetGUID();

    // check if aura can be owned by owner
    if (owner->isType(TYPEMASK_UNIT))
        if (!owner->IsInWorld() || ((Unit*)owner)->IsDuringRemoveFromWorld())
            // owner not in world so don't allow to own not self casted single target auras
            if (casterGUID != owner->GetGUID() && (spellproto->IsSingleTarget() || spellproto->NeedsSpecialTreatment()))
                return NULL;

    Aura* aura = NULL;
    switch (owner->GetTypeId())
    {
        case TYPEID_UNIT:
        case TYPEID_PLAYER:
            aura = new UnitAura(spellproto, effMask, owner, caster, baseAmount, castItem, casterGUID);
            break;
        case TYPEID_DYNAMICOBJECT:
            aura = new DynObjAura(spellproto, effMask, owner, caster, baseAmount, castItem, casterGUID);
            break;
        default:
            ASSERT(false);
            return NULL;
    }
    // aura can be removed in Unit::_AddAura call
    if (aura->IsRemoved())
        return NULL;
    return aura;
}

Aura::Aura(SpellInfo const* spellproto, WorldObject* owner, Unit* caster, Item* castItem, uint64 casterGUID) :
m_spellInfo(spellproto), m_casterGuid(casterGUID ? casterGUID : caster->GetGUID()),
m_castItemGuid(castItem ? castItem->GetGUID() : 0), m_applyTime(time(NULL)),
m_owner(owner), m_timeCla(0), m_updateTargetMapInterval(0),
m_casterLevel(caster ? caster->getLevel() : m_spellInfo->SpellLevel), m_procCharges(0), m_stackAmount(1),
m_isRemoved(false), m_isSingleTarget(false), m_isUsingCharges(false), m_changeBySoulburn(false), m_needsUnregister(true)
{
    if (m_spellInfo->ManaPerSecond)
        m_timeCla = 1 * IN_MILLISECONDS;

    m_maxDuration = CalcMaxDuration(caster);
    m_duration = m_maxDuration;
    m_procCharges = CalcMaxCharges(caster);
    m_isUsingCharges = m_procCharges != 0;
    // m_casterLevel = cast item level/caster level, caster level should be saved to db, confirmed with sniffs
}

void Aura::_InitEffects(uint8 effMask, Unit* caster, int32 *baseAmount)
{
    // shouldn't be in constructor - functions in AuraEffect::AuraEffect use polymorphism
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        if (effMask & (uint8(1) << i))
            m_effects[i] = new AuraEffect(this, i, baseAmount ? baseAmount + i : NULL, caster);
        else
            m_effects[i] = NULL;
    }
}

Aura::~Aura()
{
    // unload scripts
    while (!m_loadedScripts.empty())
    {
        std::list<AuraScript*>::iterator itr = m_loadedScripts.begin();
        (*itr)->_Unload();
        delete (*itr);
        m_loadedScripts.erase(itr);
    }

    // free effects memory
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
         delete m_effects[i];

    ASSERT(m_applications.empty());
    _DeleteRemovedApplications();
}

Unit* Aura::GetCaster() const
{
    if (GetOwner()->GetGUID() == GetCasterGUID())
        return GetUnitOwner();
    if (AuraApplication const* aurApp = GetApplicationOfTarget(GetCasterGUID()))
        return aurApp->GetTarget();

    return ObjectAccessor::GetUnit(*GetOwner(), GetCasterGUID());
}

AuraObjectType Aura::GetType() const
{
    return (m_owner->GetTypeId() == TYPEID_DYNAMICOBJECT) ? DYNOBJ_AURA_TYPE : UNIT_AURA_TYPE;
}

void Aura::_ApplyForTarget(Unit* target, Unit* caster, AuraApplication * auraApp)
{
    ASSERT(target);
    ASSERT(auraApp);
    // aura mustn't be already applied on target
    ASSERT (!IsAppliedOnTarget(target->GetGUID()) && "Aura::_ApplyForTarget: aura musn't be already applied on target");

    m_applications[target->GetGUID()] = auraApp;

    // set infinity cooldown state for spells
    if (caster && caster->GetTypeId() == TYPEID_PLAYER)
    {
        if (m_spellInfo->Attributes & SPELL_ATTR0_DISABLED_WHILE_ACTIVE)
        {
            Item* castItem = m_castItemGuid ? caster->ToPlayer()->GetItemByGuid(m_castItemGuid) : NULL;
            caster->AddSpellAndCategoryCooldowns(m_spellInfo, castItem ? castItem->GetEntry() : 0, NULL, true);
        }
    }
}

void Aura::_UnapplyForTarget(Unit* target, Unit* caster, AuraApplication * auraApp)
{
    ASSERT(target);
    ASSERT(auraApp->GetRemoveMode());
    ASSERT(auraApp);

    ApplicationMap::iterator itr = m_applications.find(target->GetGUID());

    // TODO: Figure out why this happens
    if (itr == m_applications.end())
    {
        TC_LOG_ERROR("spells", "Aura::_UnapplyForTarget, target:%u, caster:%u, spell:%u was not found in owners application map!",
        target->GetGUIDLow(), caster ? caster->GetGUIDLow() : 0, auraApp->GetBase()->GetSpellInfo()->Id);
        return;
    }

    // aura has to be already applied
    ASSERT(itr->second == auraApp);
    m_applications.erase(itr);

    m_removedApplications.push_back(auraApp);

    // reset cooldown state for spells
    if (caster && caster->GetTypeId() == TYPEID_PLAYER)
    {
        if (GetSpellInfo()->Attributes & SPELL_ATTR0_DISABLED_WHILE_ACTIVE)
            // note: item based cooldowns and cooldown spell mods with charges ignored (unknown existed cases)
            caster->ToPlayer()->SendCooldownEvent(GetSpellInfo());
    }
}

// removes aura from all targets
// and marks aura as removed
void Aura::_Remove(AuraRemoveMode removeMode)
{
    ASSERT (!m_isRemoved);
    m_isRemoved = true;
    ApplicationMap::iterator appItr = m_applications.begin();
    for (appItr = m_applications.begin(); appItr != m_applications.end();)
    {
        AuraApplication * aurApp = appItr->second;
        Unit* target = aurApp->GetTarget();
        target->_UnapplyAura(aurApp, removeMode);
        appItr = m_applications.begin();
    }
}

void Aura::UpdateTargetMap(Unit* caster, bool apply)
{
    if (IsRemoved())
        return;

    m_updateTargetMapInterval = UPDATE_TARGET_MAP_INTERVAL;

    // fill up to date target list
    //       target, effMask
    std::map<Unit*, uint8> targets;

    FillTargetMap(targets, caster);

    UnitList targetsToRemove;

    // mark all auras as ready to remove
    for (ApplicationMap::iterator appIter = m_applications.begin(); appIter != m_applications.end();++appIter)
    {
        std::map<Unit*, uint8>::iterator existing = targets.find(appIter->second->GetTarget());
        // not found in current area - remove the aura
        if (existing == targets.end())
            targetsToRemove.push_back(appIter->second->GetTarget());
        else
        {
            // needs readding - remove now, will be applied in next update cycle
            // (dbcs do not have auras which apply on same type of targets but have different radius, so this is not really needed)
            if (appIter->second->GetEffectMask() != existing->second || !CanBeAppliedOn(existing->first))
                targetsToRemove.push_back(appIter->second->GetTarget());
            // nothing todo - aura already applied
            // remove from auras to register list
            targets.erase(existing);
        }
    }

    // register auras for units
    for (std::map<Unit*, uint8>::iterator itr = targets.begin(); itr!= targets.end();)
    {
        // aura mustn't be already applied on target
        if (AuraApplication * aurApp = GetApplicationOfTarget(itr->first->GetGUID()))
        {
            // the core created 2 different units with same guid
            // this is a major failue, which i can't fix right now
            // let's remove one unit from aura list
            // this may cause area aura "bouncing" between 2 units after each update
            // but because we know the reason of a crash we can remove the assertion for now
            if (aurApp->GetTarget() != itr->first)
            {
                // remove from auras to register list
                targets.erase(itr++);
                continue;
            }
            else
            {
                // ok, we have one unit twice in target map (impossible, but...)
                ASSERT(false);
            }
        }

        bool addUnit = true;
        // check target immunities
        for (uint8 effIndex = 0; effIndex < MAX_SPELL_EFFECTS; ++effIndex)
        {
            if (itr->first->IsImmunedToSpellEffect(GetSpellInfo(), effIndex))
                itr->second &= ~(1 << effIndex);
        }
        if (!itr->second
            || itr->first->IsImmunedToSpell(GetSpellInfo())
            || !CanBeAppliedOn(itr->first))
            addUnit = false;

        if (addUnit)
        {
            // persistent area aura does not hit flying targets
            if (GetType() == DYNOBJ_AURA_TYPE)
            {
                if (itr->first->isInFlight())
                    addUnit = false;
            }
            // unit auras can not stack with each other
            else // (GetType() == UNIT_AURA_TYPE)
            {
                // Allow to remove by stack when aura is going to be applied on owner
                if (itr->first != GetOwner())
                {
                    // check if not stacking aura already on target
                    // this one prevents unwanted usefull buff loss because of stacking and prevents overriding auras periodicaly by 2 near area aura owners
                    for (Unit::AuraApplicationMap::iterator iter = itr->first->GetAppliedAuras().begin(); iter != itr->first->GetAppliedAuras().end(); ++iter)
                    {
                        Aura const* aura = iter->second->GetBase();
                        if (!CanStackWith(aura))
                        {
                            addUnit = false;
                            break;
                        }
                    }
                }
            }
        }
        if (!addUnit)
            targets.erase(itr++);
        else
        {
            // owner has to be in world, or effect has to be applied to self
            if (!GetOwner()->IsSelfOrInSameMap(itr->first))
            {
                //TODO: There is a crash caused by shadowfiend load addon
                TC_LOG_FATAL("spells", "Aura %u: Owner %s (map %u) is not in the same map as target %s (map %u).", GetSpellInfo()->Id,
                    GetOwner()->GetName().c_str(), GetOwner()->IsInWorld() ? GetOwner()->GetMap()->GetId() : uint32(-1),
                    itr->first->GetName().c_str(), itr->first->IsInWorld() ? itr->first->GetMap()->GetId() : uint32(-1));
                ASSERT(false);
            }
            itr->first->_CreateAuraApplication(this, itr->second);
            ++itr;
        }
    }

    // remove auras from units no longer needing them
    for (UnitList::iterator itr = targetsToRemove.begin(); itr != targetsToRemove.end();++itr)
        if (AuraApplication * aurApp = GetApplicationOfTarget((*itr)->GetGUID()))
            (*itr)->_UnapplyAura(aurApp, AURA_REMOVE_BY_DEFAULT);

    if (!apply)
        return;

    // apply aura effects for units
    for (std::map<Unit*, uint8>::iterator itr = targets.begin(); itr!= targets.end();++itr)
    {
        if (AuraApplication * aurApp = GetApplicationOfTarget(itr->first->GetGUID()))
        {
            // owner has to be in world, or effect has to be applied to self
            ASSERT((!GetOwner()->IsInWorld() && GetOwner() == itr->first) || GetOwner()->IsInMap(itr->first));
            itr->first->_ApplyAura(aurApp, itr->second);
        }
    }
}

// targets have to be registered and not have effect applied yet to use this function
void Aura::_ApplyEffectForTargets(uint8 effIndex)
{
    // prepare list of aura targets
    UnitList targetList;
    for (ApplicationMap::iterator appIter = m_applications.begin(); appIter != m_applications.end(); ++appIter)
    {
        if ((appIter->second->GetEffectsToApply() & (1<<effIndex)) && !appIter->second->HasEffect(effIndex))
            targetList.push_back(appIter->second->GetTarget());
    }

    // apply effect to targets
    for (UnitList::iterator itr = targetList.begin(); itr != targetList.end(); ++itr)
    {
        if (GetApplicationOfTarget((*itr)->GetGUID()))
        {
            // owner has to be in world, or effect has to be applied to self
            ASSERT((!GetOwner()->IsInWorld() && GetOwner() == *itr) || GetOwner()->IsInMap(*itr));
            (*itr)->_ApplyAuraEffect(this, effIndex);
        }
    }
}
void Aura::UpdateOwner(uint32 diff, WorldObject* owner)
{
    ASSERT(owner == m_owner);

    Unit* caster = GetCaster();
    // Apply spellmods for channeled auras
    // used for example when triggered spell of spell:10 is modded
    Spell* modSpell = NULL;
    Player* modOwner = NULL;
    if (caster)
    {
        modOwner = caster->GetSpellModOwner();
        if (modOwner)
        {
            modSpell = modOwner->FindCurrentSpellBySpellId(GetId());
            if (modSpell)
                modOwner->SetSpellModTakingSpell(modSpell, true);
        }
    }

    Update(diff, caster);

    if (m_updateTargetMapInterval <= int32(diff))
        UpdateTargetMap(caster);
    else
        m_updateTargetMapInterval -= diff;

    // update aura effects
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (m_effects[i])
            m_effects[i]->Update(diff, caster);

    // remove spellmods after effects update
    if (modSpell)
        modOwner->SetSpellModTakingSpell(modSpell, false);

    _DeleteRemovedApplications();
}

void Aura::Update(uint32 diff, Unit* caster)
{
    if (m_duration > 0)
    {
        m_duration -= diff;
        if (m_duration < 0)
            m_duration = 0;

        // handle manaPerSecond/manaPerSecondPerLevel
        if (m_timeCla)
        {
            if (m_timeCla > int32(diff))
                m_timeCla -= diff;
            else if (caster)
            {
                if (int32 manaPerSecond = m_spellInfo->ManaPerSecond)
                {
                    m_timeCla += 1000 - diff;

                    Powers powertype = Powers(m_spellInfo->PowerType);
                    if (powertype == POWER_HEALTH)
                    {
                        if (int32(caster->GetHealth()) > manaPerSecond)
                            caster->ModifyHealth(-manaPerSecond);
                        else
                        {
                            Remove();
                            return;
                        }
                    }
                    else
                    {
                        if (int32(caster->GetPower(powertype)) >= manaPerSecond)
                            caster->ModifyPower(powertype, -manaPerSecond);
                        else
                        {
                            Remove();
                            return;
                        }
                    }
                }
            }
        }
    }
}

int32 Aura::CalcMaxDuration(Unit* caster) const
{
    Player* modOwner = NULL;
    int32 maxDuration;

    if (caster)
    {
        modOwner = caster->GetSpellModOwner();
        maxDuration = caster->CalcSpellDuration(m_spellInfo);
    }
    else
        maxDuration = m_spellInfo->GetDuration();

    if (IsPassive() && !m_spellInfo->DurationEntry)
        maxDuration = -1;

    // IsPermanent() checks max duration (which we are supposed to calculate here)
    if (maxDuration != -1 && modOwner)
        modOwner->ApplySpellMod(GetId(), SPELLMOD_DURATION, maxDuration);

    return maxDuration;
}

void Aura::SetDuration(int32 duration, bool withMods)
{
    if (withMods)
    {
        if (Unit* caster = GetCaster())
            if (Player* modOwner = caster->GetSpellModOwner())
                modOwner->ApplySpellMod(GetId(), SPELLMOD_DURATION, duration);
    }
    m_duration = duration;
    SetNeedClientUpdateForTargets();
}

void Aura::RefreshDuration()
{
    SetDuration(GetMaxDuration());

    for (int i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (HasEffect(i) && GetEffect(i)->IsPeriodic())
            GetEffect(i)->RecalculateAmount(GetCaster());

    if (m_spellInfo->ManaPerSecond)
        m_timeCla = 1 * IN_MILLISECONDS;
}

void Aura::RefreshTimers()
{
    m_maxDuration = CalcMaxDuration();
    bool resetPeriodic = true;
    if (m_spellInfo->AttributesEx8 & SPELL_ATTR8_DONT_RESET_PERIODIC_TIMER)
    {
        int32 minAmplitude = m_maxDuration;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (AuraEffect const* eff = GetEffect(i))
                if (int32 ampl = eff->GetAmplitude())
                    minAmplitude = std::min(ampl, minAmplitude);

        // If only one tick remaining, roll it over into new duration
        if (GetDuration() <= minAmplitude)
        {
            m_maxDuration += GetDuration();
            resetPeriodic = false;
        }
    }
    // Not sure if this is correct but Searing Flames is just plain not working without this
    // Since the stacks refresh way too fast
    if (GetId() == 77661)
        resetPeriodic = false;

    // HACKFIX: WoundPoison - CalcMaxDuration overwrite the PvP duration on refresh thats why maxduration is not set on spellscript
    if (GetId() == 13218 && GetOwner() && GetOwner()->GetTypeId() == TYPEID_PLAYER)
        m_maxDuration = 10 * IN_MILLISECONDS;

    // HACKFIX - Druid T13 Restoration 4P Bonus max duration hack for reapply case
    if (GetId() == 774 || GetId() == 8936)
        if (AuraEffect* t13_4p = GetCaster()->GetAuraEffect(105770, EFFECT_0, GetCasterGUID()))
            if (roll_chance_i(t13_4p->GetAmount()))
                m_maxDuration *= 2;

    RefreshDuration();
    Unit* caster = GetCaster();
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (HasEffect(i))
            GetEffect(i)->CalculatePeriodic(caster, resetPeriodic, false);
    // fix hast refresh
}

void Aura::SetCharges(uint8 charges)
{
    if (m_procCharges == charges)
        return;
    m_procCharges = charges;
    m_isUsingCharges = m_procCharges != 0;
    SetNeedClientUpdateForTargets();
}

uint8 Aura::CalcMaxCharges(Unit* caster) const
{
    uint32 maxProcCharges = m_spellInfo->ProcCharges;

    if (SpellProcEntry const* procEntry = sSpellMgr->GetSpellProcEntry(GetId()))
        maxProcCharges = procEntry->charges;

    if (caster)
    {
        if (Player* modOwner = caster->GetSpellModOwner())
            modOwner->ApplySpellMod(GetId(), SPELLMOD_CHARGES, maxProcCharges);

        // HACKFIX!!! Because didn't worked over SetCharges()
        // 105609 Item - Death Knight T13 DPS 2P Bonus
        if (caster->HasAura(105609) && (GetId() == 59052 || GetId() == 81340))
        {
            if (GetCharges() == 2 ||
                GetId() == 59052 && roll_chance_i(60) ||
                GetId() == 81340 && roll_chance_i(30))
                maxProcCharges = 2;
        }
    }
    return uint8(maxProcCharges);
}

bool Aura::ModCharges(int32 num, AuraRemoveMode removeMode)
{
    if (IsUsingCharges())
    {
        int32 charges = m_procCharges + num;
        int32 maxCharges = CalcMaxCharges();

        // limit charges (only on charges increase, charges may be changed manually)
        if ((num > 0) && (charges > int32(maxCharges)))
            charges = maxCharges;
        // we're out of charges, remove
        else if (charges <= 0)
        {
            Remove(removeMode);
            return true;
        }

        SetCharges(charges);
    }
    return false;
}

void Aura::SetStackAmount(uint32 stackAmount)
{
    m_stackAmount = stackAmount;
    Unit* caster = GetCaster();

    std::list<AuraApplication*> applications;
    GetApplicationList(applications);

    for (std::list<AuraApplication*>::const_iterator apptItr = applications.begin(); apptItr != applications.end(); ++apptItr)
        if (!(*apptItr)->GetRemoveMode())
            HandleAuraSpecificMods(*apptItr, caster, false, true);

    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (HasEffect(i))
            m_effects[i]->ChangeAmount(m_effects[i]->CalculateAmount(caster), false, true);

    for (std::list<AuraApplication*>::const_iterator apptItr = applications.begin(); apptItr != applications.end(); ++apptItr)
        if (!(*apptItr)->GetRemoveMode())
            HandleAuraSpecificMods(*apptItr, caster, true, true);

    SetNeedClientUpdateForTargets();
}

bool Aura::ModStackAmount(int32 num, AuraRemoveMode removeMode)
{
    int32 stackAmount = m_stackAmount + num;

    // limit the stack amount (only on stack increase, stack amount may be changed manually)
    if ((num > 0) && (stackAmount > int32(m_spellInfo->StackAmount)))
    {
        // not stackable aura - set stack amount to 1
        if (!m_spellInfo->StackAmount)
            stackAmount = 1;
        else
            stackAmount = m_spellInfo->StackAmount;
    }
    // we're out of stacks, remove
    else if (stackAmount <= 0)
    {
        Remove(removeMode);
        return true;
    }

    bool refresh = stackAmount >= int32(GetStackAmount());
    bool changeStacks = true;

    switch (m_spellInfo->Id)
    {
        case 44614: // Frostfire bolt
            if (GetCaster())
                changeStacks = GetCaster()->HasAura(61205);
            break;
    }


    // Update stack amount
    if (!changeStacks)
    {
        RefreshTimers();
        return stackAmount - num;
    }

    SetStackAmount(stackAmount);
    if (refresh)
    {
        RefreshSpellMods();
        RefreshTimers();

        // reset charges
        SetCharges(CalcMaxCharges());
        // FIXME: not a best way to synchronize charges, but works
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (AuraEffect* aurEff = GetEffect(i))
                if (aurEff->GetAuraType() == SPELL_AURA_ADD_FLAT_MODIFIER || aurEff->GetAuraType() == SPELL_AURA_ADD_PCT_MODIFIER)
                    if (SpellModifier* mod = aurEff->GetSpellModifier())
                        mod->charges = GetCharges();
    }
    SetNeedClientUpdateForTargets();
    return false;
}

void Aura::RefreshSpellMods()
{
    for (Aura::ApplicationMap::const_iterator appIter = m_applications.begin(); appIter != m_applications.end(); ++appIter)
        if (Player* player = appIter->second->GetTarget()->ToPlayer())
            player->RestoreAllSpellMods(0, this);
}

bool Aura::IsArea() const
{
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        if (HasEffect(i) && GetSpellInfo()->Effects[i].IsAreaAuraEffect())
            return true;
    }
    return false;
}

bool Aura::IsPassive() const
{
    return GetSpellInfo()->IsPassive();
}

bool Aura::IsDeathPersistent() const
{
    return GetSpellInfo()->IsDeathPersistent();
}

bool Aura::CanBeSaved() const
{
    if (IsPassive())
        return false;

    if (GetCasterGUID() != GetOwner()->GetGUID())
        if (GetSpellInfo()->IsSingleTarget() || GetSpellInfo()->NeedsSpecialTreatment())
            return false;

    // Can't be saved - aura handler relies on calculated amount and changes it
    if (HasEffectType(SPELL_AURA_CONVERT_RUNE))
        return false;

    // No point in saving this, since the stable dialog can't be open on aura load anyway.
    if (HasEffectType(SPELL_AURA_OPEN_STABLE))
        return false;

    // Can't save vehicle auras, it requires both caster & target to be in world
    if (HasEffectType(SPELL_AURA_CONTROL_VEHICLE))
        return false;

    // Incanter's Absorbtion - considering the minimal duration and problems with aura stacking
    // we skip saving this aura
    // Also for some reason other auras put as MultiSlot crash core on keeping them after restart,
    // so put here only these for which you are sure they get removed
    switch (GetId())
    {
        case 44413: // Incanter's Absorption
        case 40075: // Fel Flak Fire
        case 55849: // Power Spark
        case 83676: // Resistance is Futile
        case 80326: // Camouflage - perodic
        case 80325: // Camouflage - Stealth
        case 82946: // Trap Launcher - Triggered
        case 93683: // Shadow Orb - Marker
        case 63622: // Improved Unholy presence
        case 63611: // Improved Blood presence
        case 63621: // Improved Frost presence
        case 83582: // Pyromaniac
        case 96206: // Nature's bounty
        case 81098: // Tree of life passive
        case 80879: // Primal Madness
        case 80886: // Primal Madness
        case 99252: // Blaze of Glory
        case 99158: // Dark Flames
        case 81114: // Magma
        case 96932: // Eyes of Occu'thar
        case 98229: // Majordomo Heroic aura
        case 98254: // Majordomo Heroic aura
        case 98253: // Majordomo Heroic aura
        case 98252: // Majordomo Heroic aura
        case 98245: // Majordomo Heroic aura
        case 106464: // Enter the Dream
        case 105900: // Essence of Dreams
        case 105903: // Source of Magic
        case 105984: // Timeloop
        case 106182: // Last Defender of Azeroth
        case 106080: // Last Defender of Azeroth
        case 106224: // Last Defender of Azeroth
        case 106226: // Last Defender of Azeroth
        case 106227: // Last Defender of Azeroth
        case 104377: // Black Blood of Go'rath
        case 110306: // Black Blood of Go'rath
        case 104378: // Black Blood of Go'rath
        case 110322: // Black Blood of Go'rath 
        case 102951:
        case 103018:
        case 103420:
        case 103004:
        case 102994:
        case 103020:
            return false;
            break;
    }

    // When a druid logins, he doesnt have either eclipse power, nor the marker auras, nor the eclipse buffs. Dont save them.
    if (GetId() == 67483 || GetId() == 67484 || GetId() == 48517 || GetId() == 48518)
        return false;

    // don't save auras removed by proc system
    if (IsUsingCharges() && !GetCharges())
        return false;

    return true;
}

bool Aura::CanBeSentToClient() const
{
    // Maybe every aura should be sent to client :)?
    return !IsPassive() || GetSpellInfo()->HasAreaAuraEffect() || HasEffectType(SPELL_AURA_ABILITY_IGNORE_AURASTATE)
        || HasEffectType(SPELL_AURA_CAST_WHILE_WALKING) || HasEffectType(SPELL_AURA_OVERRIDE_ACTIONBAR_SPELLS)
        || (GetSpellInfo()->AttributesEx8 & SPELL_ATTR8_MASTERY_SPECIALIZATION) || GetSpellInfo()->Id == 25956; // Sanctity of Battle isn't send to client
}

bool Aura::IsSingleTargetWith(Aura const* aura) const
{
    // Same spell?
    if (GetSpellInfo()->IsRankOf(aura->GetSpellInfo()))
        return true;

    SpellSpecificType spec = GetSpellInfo()->GetSpellSpecific();
    // spell with single target specific types
    switch (spec)
    {
    case SPELL_SPECIFIC_JUDGEMENT:
    case SPELL_SPECIFIC_MAGE_POLYMORPH:
        if (aura->GetSpellInfo()->GetSpellSpecific() == spec)
            return true;
        break;
    default:
        break;
    }

    if (HasEffectType(SPELL_AURA_CONTROL_VEHICLE) && aura->HasEffectType(SPELL_AURA_CONTROL_VEHICLE))
        return true;

    return false;
}

void Aura::UnregisterSingleTarget()
{
    ASSERT(m_isSingleTarget);
    Unit* caster = GetCaster();
    // TODO: find a better way to do this.
    if (!caster)
        caster = ObjectAccessor::GetObjectInOrOutOfWorld(GetCasterGUID(), (Unit*)NULL);
    ASSERT(caster);
    caster->GetSingleCastAuras().remove(this);
    SetIsSingleTarget(false);
}

int32 Aura::CalcDispelChance(Unit* auraTarget, bool offensive) const
{
    // we assume that aura dispel chance is 100% on start
    // need formula for level difference based chance
    int32 resistChance = 0;

    // Apply dispel mod from aura caster
    if (Unit* caster = GetCaster())
        if (Player* modOwner = caster->GetSpellModOwner())
            modOwner->ApplySpellMod(GetId(), SPELLMOD_RESIST_DISPEL_CHANCE, resistChance);

    // Dispel resistance from target SPELL_AURA_MOD_DISPEL_RESIST
    // Only affects offensive dispels
    if (offensive && auraTarget)
        resistChance += auraTarget->GetTotalAuraModifier(SPELL_AURA_MOD_DISPEL_RESIST);

    resistChance = resistChance < 0 ? 0 : resistChance;
    resistChance = resistChance > 100 ? 100 : resistChance;
    return 100 - resistChance;
}

void Aura::SetLoadedState(int32 maxduration, int32 duration, int32 charges, uint32 stackamount, uint8 recalculateMask, int32 * amount)
{
    m_maxDuration = maxduration;
    m_duration = duration;
    m_procCharges = charges;
    m_isUsingCharges = m_procCharges != 0;
    m_stackAmount = stackamount;
    Unit* caster = GetCaster();
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (m_effects[i])
        {
            m_effects[i]->SetAmount(amount[i]);
            m_effects[i]->SetCanBeRecalculated(recalculateMask & (1<<i));
            m_effects[i]->CalculatePeriodic(caster, false, true);
            m_effects[i]->CalculateSpellMod();
            m_effects[i]->RecalculateAmount(caster);
        }
}

bool Aura::HasEffectType(AuraType type) const
{
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        if (HasEffect(i) && m_effects[i]->GetAuraType() == type)
            return true;
    }
    return false;
}

AuraEffect* Aura::GetFirstEffectOfType(AuraType type) const
{
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        if (HasEffect(i) && m_effects[i]->GetAuraType() == type)
            return m_effects[i];
    }
    return NULL;
}

void Aura::RecalculateAmountOfEffects()
{
    ASSERT (!IsRemoved());
    Unit* caster = GetCaster();
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (HasEffect(i))
            m_effects[i]->RecalculateAmount(caster);
}

void Aura::HandleAllEffects(AuraApplication * aurApp, uint8 mode, bool apply)
{
    ASSERT (!IsRemoved());
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (m_effects[i] && !IsRemoved())
            m_effects[i]->HandleEffect(aurApp, mode, apply);
}

void Aura::GetApplicationList(std::list<AuraApplication*> & applicationList) const
{
    for (Aura::ApplicationMap::const_iterator appIter = m_applications.begin(); appIter != m_applications.end(); ++appIter)
    {
        if (appIter->second->GetEffectMask())
            applicationList.push_back(appIter->second);
    }
}

void Aura::SetNeedClientUpdateForTargets() const
{
    for (ApplicationMap::const_iterator appIter = m_applications.begin(); appIter != m_applications.end(); ++appIter)
        appIter->second->SetNeedClientUpdate();
}

// trigger effects on real aura apply/remove
void Aura::HandleAuraSpecificMods(AuraApplication const* aurApp, Unit* caster, bool apply, bool onReapply)
{
    Unit* target = aurApp->GetTarget();
    AuraRemoveMode removeMode = aurApp->GetRemoveMode();
    // handle spell_area table
    SpellAreaForAreaMapBounds saBounds = sSpellMgr->GetSpellAreaForAuraMapBounds(GetId());
    if (saBounds.first != saBounds.second)
    {
        uint32 zone, area;
        target->GetZoneAndAreaId(zone, area);

        for (SpellAreaForAreaMap::const_iterator itr = saBounds.first; itr != saBounds.second; ++itr)
        {
            // some auras remove at aura remove
            if (!itr->second->IsFitToRequirements((Player*)target, zone, area))
                target->RemoveAurasDueToSpell(itr->second->spellId);
            // some auras applied at aura apply
            else if (itr->second->autocast)
            {
                if (!target->HasAura(itr->second->spellId))
                    target->CastSpell(target, itr->second->spellId, true);
            }
        }
    }

    // handle spell_linked_spell table
    if (!onReapply)
    {
        // apply linked auras
        if (apply)
        {
            if (std::vector<int32> const* spellTriggered = sSpellMgr->GetSpellLinked(GetId() + SPELL_LINK_AURA))
            {
                for (std::vector<int32>::const_iterator itr = spellTriggered->begin(); itr != spellTriggered->end(); ++itr)
                {
                    if (*itr < 0)
                        target->ApplySpellImmune(GetId(), IMMUNITY_ID, -(*itr), true);
                    else if (caster)
                        caster->AddAura(*itr, target);
                }
            }
        }
        else
        {
            // remove linked auras
            if (std::vector<int32> const* spellTriggered = sSpellMgr->GetSpellLinked(-(int32)GetId()))
            {
                for (std::vector<int32>::const_iterator itr = spellTriggered->begin(); itr != spellTriggered->end(); ++itr)
                {
                    if (*itr < 0)
                        target->RemoveAurasDueToSpell(-(*itr));
                    else if (removeMode != AURA_REMOVE_BY_DEATH  && target->IsInWorld() && !target->IsDuringRemoveFromWorld())
                        target->CastSpell(target, *itr, true, NULL, NULL, GetCasterGUID());
                }
            }
            if (std::vector<int32> const* spellTriggered = sSpellMgr->GetSpellLinked(GetId() + SPELL_LINK_AURA))
            {
                for (std::vector<int32>::const_iterator itr = spellTriggered->begin(); itr != spellTriggered->end(); ++itr)
                {
                    if (*itr < 0)
                        target->ApplySpellImmune(GetId(), IMMUNITY_ID, -(*itr), false);
                    else
                        target->RemoveAura(*itr, GetCasterGUID(), 0, removeMode);
                }
            }
        }
    }
    else if (apply)
    {
        // modify stack amount of linked auras
        if (std::vector<int32> const* spellTriggered = sSpellMgr->GetSpellLinked(GetId() + SPELL_LINK_AURA))
        {
            for (std::vector<int32>::const_iterator itr = spellTriggered->begin(); itr != spellTriggered->end(); ++itr)
                if (*itr > 0)
                    if (Aura* triggeredAura = target->GetAura(*itr, GetCasterGUID()))
                        triggeredAura->ModStackAmount(GetStackAmount() - triggeredAura->GetStackAmount());
        }
    }

    // mods at aura apply
    if (apply)
    {
        switch (GetSpellInfo()->SpellFamilyName)
        {
            case SPELLFAMILY_GENERIC:
                switch (GetId())
                {
                    case 30019:
                        target->InitCharmInfo();
                        target->GetCharmInfo()->InitCharmCreateSpells(false);
                        target->SetCharmedBy(caster,CHARM_TYPE_CHARM);
                        break;
                    case 32474: // Buffeting Winds of Susurrus
                        if (target->GetTypeId() == TYPEID_PLAYER)
                            target->ToPlayer()->ActivateTaxiPathTo(506, GetId());
                        break;
                    case 33572: // Gronn Lord's Grasp, becomes stoned
                        if (GetStackAmount() >= 5 && !target->HasAura(33652))
                            target->CastSpell(target, 33652, true);
                        break;
                    case 50836: //Petrifying Grip, becomes stoned
                        if (GetStackAmount() >= 5 && !target->HasAura(50812))
                            target->CastSpell(target, 50812, true);
                        break;
                }
                break;
            case SPELLFAMILY_WARRIOR:
                switch (GetId())
                {
                    case 60970: // Heroic Fury (remove Intercept cooldown)
                        if (target->GetTypeId() == TYPEID_PLAYER)
                            target->ToPlayer()->RemoveSpellCooldown(20252, true);
                        break;
                }
                break;
            case SPELLFAMILY_DRUID:
                if (!caster)
                    break;
                // Rejuvenation
                if (GetSpellInfo()->SpellFamilyFlags[0] & 0x10 && GetEffect(EFFECT_0))
                {
                    // Druid T8 Restoration 4P Bonus
                    if (caster->HasAura(64760))
                    {
                        int32 heal = GetEffect(EFFECT_0)->GetAmount();
                        caster->CastCustomSpell(target, 64801, &heal, NULL, NULL, true, NULL, GetEffect(EFFECT_0));
                    }
                }
                break;
            case SPELLFAMILY_HUNTER:
            {
                switch (GetSpellInfo()->Id)
                {
                    case 82925: // Ready, Set, Aim
                        if (aurApp->GetBase()->GetStackAmount() == m_spellInfo->StackAmount)
                            if (caster)
                            {
                                caster->CastSpell(caster, 82926, true);
                                Remove();
                            }
                        break;
                }
                break;
            }
            case SPELLFAMILY_MAGE:
                if (!caster)
                    break;
                // Todo: This should be moved to similar function in spell::hit
                if (GetSpellInfo()->SpellFamilyFlags[0] & 0x01000000)
                {
                    // Polymorph Sound - Sheep && Penguin
                    if (GetSpellInfo()->SpellIconID == 82 && GetSpellInfo()->SpellVisual[0] == 12978)
                    {
                        // Glyph of the Penguin
                        if (caster->HasAura(52648))
                            caster->CastSpell(target, 61635, true);
                        else
                            caster->CastSpell(target, 61634, true);
                    }
                }
                switch (GetId())
                {
                    case 12536: // Clearcasting
                    case 12043: // Presence of Mind
                        // Arcane Potency
                        if (AuraEffect const* aurEff = caster->GetAuraEffect(SPELL_AURA_DUMMY, SPELLFAMILY_MAGE, 2120, 0))
                        {
                            uint32 spellId = 0;

                            switch (aurEff->GetId())
                            {
                                case 31571: spellId = 57529; break;
                                case 31572: spellId = 57531; break;
                                default:
                                    TC_LOG_ERROR("spells", "Aura::HandleAuraSpecificMods: Unknown rank of Arcane Potency (%d) found", aurEff->GetId());
                            }
                            if (spellId)
                                caster->CastSpell(caster, spellId, true);
                        }
                        break;
                    default:
                        break;
                }
                break;
            case SPELLFAMILY_PRIEST:
                if (!caster)
                    break;
                // Devouring Plague
                if (GetSpellInfo()->SpellFamilyFlags[0] & 0x02000000 && GetEffect(0))
                {
                    // Improved Devouring Plague
                    if (AuraEffect const* aurEff = caster->GetDummyAuraEffect(SPELLFAMILY_PRIEST, 3790, 0))
                    {
                        // Base Ticks = 8, 1% Haste = 128.05701f Haste Rating
                        // Extra Tick Breakpoint 1 = 1600 haste
                        // Extra Tick Breakpoint 2 = 3200 haste
                        uint32 damage = GetEffect(0)->GetAmount();
                        float haste = 1.0f + float(GetCaster()->ToPlayer()->GetCombatRating(CR_HASTE_SPELL) / (128.05701f * 100.0f));
                        uint8 baseTicks = 8;
                        uint32 ticks = baseTicks * haste;
                        int32 basepoints0 = aurEff->GetAmount() * ticks * int32(damage) / 100;
                        int32 heal = int32(CalculatePct(basepoints0, 15));

                        caster->CastCustomSpell(target, 63675, &basepoints0, NULL, NULL, true, NULL, GetEffect(0));
                        caster->CastCustomSpell(caster, 75999, &heal, NULL, NULL, true, NULL, GetEffect(0));
                    }
                }
                // Power Word: Shield
                else if (m_spellInfo->Id == 17 && GetEffect(0))
                {
                    // Glyph of Power Word: Shield
                    if (AuraEffect* glyph = caster->GetAuraEffect(55672, 0))
                    {
                        // instantly heal m_amount% of the absorb-value
                        int32 heal = glyph->GetAmount() * GetEffect(0)->GetAmount()/100;
                        caster->CastCustomSpell(GetUnitOwner(), 56160, &heal, NULL, NULL, true, 0, GetEffect(0));
                    }
                }
                break;
            case SPELLFAMILY_ROGUE:
                // Sprint (skip non player casted spells by category)
                if (GetSpellInfo()->SpellFamilyFlags[0] & 0x40 && GetSpellInfo()->Category == 44)
                    // in official maybe there is only one icon?
                    if (target->HasAura(58039)) // Glyph of Blurred Speed
                        target->CastSpell(target, 61922, true); // Sprint (waterwalk)
                break;
            case SPELLFAMILY_PALADIN:
                if (m_spellInfo->Id == 105819) // Zeal of the Crusader - Synch timer with zealotry
                {
                    if (Aura* zealotry = target->GetAura(85696)) // Zealotry
                    {
                        aurApp->GetBase()->SetMaxDuration(zealotry->GetMaxDuration());
                        aurApp->GetBase()->SetDuration(zealotry->GetDuration());
                    }
                }
                break;
        }
    }
    // mods at aura remove
    else
    {
        // Big hack to fix http://wowzealot.com/bugtracker/issue/461
        // Don't judge me... previous creature reset was big enough to get fixed.
        // Atm we get console error when performing this, but it does the trick: Player <GUID: X> attempt targeted home - 
        // Kept it in this function as it handles removal which is needed for SPELL_AURA_MOD_POSSESS and after trying to use MoveTargetedHome on player it will be on unit so it does the trick.
        // @ToDo, replace this with a less hacky fix?, move it to another function where we can actually call MoveTargetHome on just the creature, and not the player also.

        //Testing experiments, CREATUREONLY->GetMotionMaster()->MoveTargetedHome()
        //target->RemoveCharmedBy(caster);
        if (caster && caster->GetTypeId() == TYPEID_PLAYER && GetSpellInfo()->HasAura(SPELL_AURA_MOD_POSSESS))
        target->GetMotionMaster()->MoveTargetedHome();

        switch (GetSpellInfo()->SpellFamilyName)
        {
            case SPELLFAMILY_GENERIC:
                switch (GetId())
                {
                    case 30019:// Control Piece - Chess Event
                    {
                        //target->RemoveAllAuras();
                        target->RemoveCharmedBy(caster);
                        //target->RemoveBindSightAuras();
                        //target->RemoveCharmAuras();
                        target->AttackStop();
                        Unit *charm = caster->GetCharm();
                        if(charm)
                            charm->RemoveAurasDueToSpell(30019,0);  // Also remove aura from charmed creature, not only from us :]
                        break;
                    }
                    case 74002: // Combat readiness should fall after 10 seconds of no attacks
                        if (removeMode == AURA_REMOVE_BY_EXPIRE)
                            target->RemoveAurasDueToSpell(74001);
                        break;
						case 91296: // Egg Shell, Corrupted Egg Shell
                     if (caster)
                         caster->CastSpell(caster, 91305, true);
                     break;
                    case 91308: // Egg Shell, Corrupted Egg Shell (H)
                     if (caster)
                         caster->CastSpell(caster, 91310, true);
                     break;
                    case 72368: // Shared Suffering
                    case 72369:
                        if (caster)
                        {
                            if (AuraEffect* aurEff = GetEffect(0))
                            {
                                int32 remainingDamage = aurEff->GetAmount() * (aurEff->GetTotalTicks() - aurEff->GetTickNumber());
                                if (remainingDamage > 0)
                                    caster->CastCustomSpell(caster, 72373, NULL, &remainingDamage, NULL, true);
                            }
                        }
                        break;
                }
                break;
            case SPELLFAMILY_MAGE:
                switch (GetId())
                {
                    case 66: // Invisibility
                        if (removeMode != AURA_REMOVE_BY_EXPIRE)
                            break;
                        target->CastSpell(target, 32612, true, NULL, GetEffect(1));
                        target->CombatStop();
                        if (target->GetTypeId() == TYPEID_PLAYER)
                            if (Unit* pet = target->ToPlayer()->GetPet())
                            {
                                pet->CastSpell(pet, 96243, true);
                                pet->CombatStop();
                            }
                        break;
                    case 83239: // Early Frost
                    case 83162: // Early Frost
                        target->CastSpell(target, 94315, true);
                        break;
                    default:
                        break;
                }
                break;
            case SPELLFAMILY_WARLOCK:
                if (!caster)
                    break;
                // Improved Fear
                if (GetSpellInfo()->SpellFamilyFlags[1] & 0x00000400)
                {
                    if (AuraEffect* aurEff = caster->GetAuraEffect(SPELL_AURA_DUMMY, SPELLFAMILY_WARLOCK, 98, 0))
                    {
                        uint32 spellId = 0;
                        switch (aurEff->GetId())
                        {
                            case 53759: spellId = 60947; break;
                            case 53754: spellId = 60946; break;
                            default:
                                TC_LOG_ERROR("spells", "Aura::HandleAuraSpecificMods: Unknown rank of Improved Fear (%d) found", aurEff->GetId());
                        }
                        if (spellId)
                            caster->CastSpell(target, spellId, true);
                    }
                }
                break;
            case SPELLFAMILY_PRIEST:
                if (!caster)
                    break;
                // Power word: shield
                if (removeMode == AURA_REMOVE_BY_ENEMY_SPELL && GetSpellInfo()->SpellFamilyFlags[0] & 0x00000001)
                {
                    // Rapture
                    if (AuraEffect const* aurEff = caster->GetDummyAuraEffect(SPELLFAMILY_PRIEST, 2894, EFFECT_0))
                    {
                        float multiplier = float(aurEff->GetAmount());
                        int32 basepoints0 = int32(CalculatePct(caster->GetMaxPower(POWER_MANA), multiplier));
                        TriggerCastFlags triggerFlags = TriggerCastFlags(TRIGGERED_IGNORE_GCD | TRIGGERED_IGNORE_CAST_IN_PROGRESS | TRIGGERED_CAST_DIRECTLY);

                        // Priest T13 Healer 4P Bonus (Holy Word and Power Word: Shield)
                        if (AuraEffect* t13_4p = caster->GetAuraEffect(105832, EFFECT_0, GetCasterGUID()))
                            if (roll_chance_i(t13_4p->GetAmount()))
                                basepoints0 *= 2;

                        caster->CastCustomSpell(caster, 47755, &basepoints0, NULL, NULL, triggerFlags);
                    }
                }
                break;
            case SPELLFAMILY_ROGUE:
                switch (GetId())
                {
                    case 11327: // Cast Stealth on vanish removal
                        if (removeMode == AURA_REMOVE_BY_EXPIRE)
                            target->CastSpell(target, 1784, true);
                        break;
                }
                break;
            case SPELLFAMILY_PALADIN:
                // Remove the immunity shield marker on Forbearance removal if AW marker is not present
                if (GetId() == 25771 && target->HasAura(61988))
                    target->RemoveAura(61988);
                // Zeal of the crusader removal if the plaeyr decided to troll and cancel its own zealotry buff lol
                if (GetId() == 85696 && target->HasAura(105819))
                    target->RemoveAura(105819);
                break;
            case SPELLFAMILY_DEATHKNIGHT:
                // Blood of the North
                // Reaping
                // Death Rune Mastery
                if (GetSpellInfo()->SpellIconID == 3041 || GetSpellInfo()->SpellIconID == 22 || GetSpellInfo()->SpellIconID == 2622 || GetSpellInfo()->Id == 50034)
                {
                    if (!GetEffect(0) || GetEffect(0)->GetAuraType() != SPELL_AURA_PERIODIC_DUMMY)
                        break;
                    if (target->GetTypeId() != TYPEID_PLAYER)
                        break;
                    if (target->ToPlayer()->getClass() != CLASS_DEATH_KNIGHT)
                        break;

                     // aura removed - remove death runes
                    target->ToPlayer()->RemoveRunesByAuraEffect(GetEffect(0));
                }
                break;
            case SPELLFAMILY_HUNTER:
                // Glyph of Freezing Trap
                if (GetSpellInfo()->SpellFamilyFlags[0] & 0x00000008)
                    if (caster && caster->HasAura(56845))
                        target->CastSpell(target, 61394, true);
                break;
        }
    }

    // mods at aura apply or remove
    switch (GetSpellInfo()->SpellFamilyName)
    {
        case SPELLFAMILY_GENERIC:
            switch (GetId())
            {
                // Druid of the Flames
                case 99245:
                    // Shouldn't be needing to check for cat form but triggered spells ignore shape requirements atm
                    if (apply && !target->HasAura(99244) && target->GetShapeshiftForm() == FORM_CAT)
                        target->CastSpell(target, 99244, true);
                    else if (!onReapply)
                        target->RemoveAurasDueToSpell(99244);
                    break;
            }
            break;
        case SPELLFAMILY_DRUID:
            // Enrage
            if ((GetSpellInfo()->SpellFamilyFlags[0] & 0x80000) && GetSpellInfo()->SpellIconID == 961)
            {
                if (target->HasAura(70726)) // Item - Druid T10 Feral 4P Bonus
                    if (apply)
                        target->CastSpell(target, 70725, true);
                break;
            }
            break;
        case SPELLFAMILY_ROGUE:
            // Stealth
            if (GetSpellInfo()->SpellFamilyFlags[0] & 0x00400800)
            {
                // Master of subtlety
                if (AuraEffect const* aurEff = target->GetAuraEffect(31223, 0))
                {
                    if (!apply)
                    {
                        if (removeMode == AURA_REMOVE_BY_EXPIRE && GetSpellInfo()->Id == 11327) // don't add duration if the caster is switching from vanish to stealth
                            break;

                        if (Aura* aura = target->GetAura(31665, target->GetGUID()))
                        {
                            aura->SetDuration(6000);
                            aura->SetMaxDuration(6000);
                        }
                        target->CastSpell(target, 31666, true);
                    }
                    else
                    {
                        int32 basepoints0 = aurEff->GetAmount();
                        target->CastCustomSpell(target, 31665, &basepoints0, NULL, NULL, true);
                    }
                }
                break;
            }
            break;
        case SPELLFAMILY_HUNTER:
            switch (GetId())
            {
                case 19574: // Bestial Wrath
                    // The Beast Within cast on owner if talent present
                    if (Unit* owner = target->GetOwner())
                    {
                        // Search talent
                        if (owner->HasAura(34692))
                        {
                            if (apply && GetDuration() == GetMaxDuration())
                            {
                                owner->CastSpell(owner, 70029, true);
                                owner->CastSpell(owner, 34471, true, 0, GetEffect(0));
                                owner->CastSpell(owner, 72752, true); // Will of the Forsaken Cooldown Trigger
                            }
                            else
                                owner->RemoveAurasDueToSpell(34471);
                        }
                    }
                    break;
                case 51755: // Camouflage
                {
                    if (apply)
                        target->CastSpell(target, 80326, true);
                    else
                    {
                        target->RemoveAurasDueToSpell(80326);
                        target->RemoveAurasDueToSpell(80325);
                        if (target->GetTypeId() == TYPEID_PLAYER)
                            if (Pet* playerPet = target->ToPlayer()->GetPet())
                                playerPet->RemoveAurasDueToSpell(51755);
                    }
                    break;
                }
            }
            break;
        case SPELLFAMILY_PALADIN:
            if (caster && caster == target && GetSpellInfo()->GetSpellSpecific() == SPELL_SPECIFIC_AURA)
            {
                // Communion
                if (AuraEffect* communion = caster->GetAuraEffect(31876, EFFECT_1, caster->GetGUID()))
                {
                    if (apply)
                        caster->CastSpell(caster, 63531, true);
                    else
                        target->RemoveAura(63531);
                }
            }
            switch (GetId())
            {
                case 31821:
                    // Aura Mastery Triggered Spell Handler
                    // If apply Concentration Aura -> trigger -> apply Aura Mastery Immunity
                    // If remove Concentration Aura -> trigger -> remove Aura Mastery Immunity
                    // If remove Aura Mastery -> trigger -> remove Aura Mastery Immunity
                    // Do effects only on aura owner
                    if (GetCasterGUID() != target->GetGUID())
                        break;

                    if (apply)
                    {
                        if ((GetSpellInfo()->Id == 31821 && target->HasAura(19746, GetCasterGUID())) || (GetSpellInfo()->Id == 19746 && target->HasAura(31821)))
                            target->CastSpell(target, 64364, true);
                    }
                    else
                        target->RemoveAurasDueToSpell(64364, GetCasterGUID());
                    break;
                case 31842: // Divine Favor
                    // Item - Paladin T10 Holy 2P Bonus
                    if (target->HasAura(70755))
                    {
                        if (apply)
                            target->CastSpell(target, 71166, true);
                        else
                            target->RemoveAurasDueToSpell(71166);
                    }
                    break;
            }
            break;
        case SPELLFAMILY_MAGE:
        {
            switch (GetId())
            {
                // Arcane Missiles!
                case 79683:
                    if (apply)
                        target->CastSpell(target, 79808, true);
                    else
                        target->RemoveAurasDueToSpell(79808);
                    break;
                // Brain Freeze
                case 44546:
                case 44548:
                case 44549:
                // Hot Streak
                case 44445:
                    if (apply)
                        target->RemoveAurasDueToSpell(79684);
                    else
                        target->AddAura(79684, target);
                    break;
                default:
                    break;
            }
        }
    }
}

bool Aura::CanBeAppliedOn(Unit* target)
{
    // unit not in world or during remove from world
    if (!target->IsInWorld() || target->IsDuringRemoveFromWorld())
    {
        // area auras mustn't be applied
        if (GetOwner() != target)
            return false;
        // not selfcasted single target auras mustn't be applied
        if (GetCasterGUID() != GetOwner()->GetGUID() && (GetSpellInfo()->IsSingleTarget() || GetSpellInfo()->NeedsSpecialTreatment()))
            return false;
        return true;
    }
    else
        return CheckAreaTarget(target);
}

bool Aura::CheckAreaTarget(Unit* target)
{
    return CallScriptCheckAreaTargetHandlers(target);
}

bool Aura::CanStackWith(Aura const* existingAura) const
{
    // Can stack with self
    if (this == existingAura)
        return true;

    // Dynobj auras always stack
    if (existingAura->GetType() == DYNOBJ_AURA_TYPE)
        return true;

    SpellInfo const* existingSpellInfo = existingAura->GetSpellInfo();
    bool sameCaster = GetCasterGUID() == existingAura->GetCasterGUID();

    // passive auras don't stack with another rank of the spell cast by same caster
    if (IsPassive() && sameCaster && m_spellInfo->IsDifferentRankOf(existingSpellInfo))
        return false;

    // arena 2p bonus...probably this is for all passive auras which are hidden clientside?
    if (sameCaster && existingSpellInfo->Id == 92254 && m_spellInfo->Id == 92254)
        return false;

    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        // prevent remove triggering aura by triggered aura
        if (existingSpellInfo->Effects[i].TriggerSpell == GetId()
            // prevent remove triggered aura by triggering aura refresh
            || m_spellInfo->Effects[i].TriggerSpell == existingAura->GetId())
            return true;
    }

    // check spell specific stack rules
    if (m_spellInfo->IsAuraExclusiveBySpecificWith(existingSpellInfo)
        || (sameCaster && m_spellInfo->IsAuraExclusiveBySpecificPerCasterWith(existingSpellInfo)))
        return false;

    // check spell group stack rules
    SpellGroupStackRule stackRule = sSpellMgr->CheckSpellGroupStackRules(m_spellInfo, existingSpellInfo);
    if (stackRule)
    {
        if (stackRule == SPELL_GROUP_STACK_RULE_EXCLUSIVE)
            return false;

        if (sameCaster && stackRule == SPELL_GROUP_STACK_RULE_EXCLUSIVE_FROM_SAME_CASTER)
            return false;
    }

    if (m_spellInfo->SpellFamilyName != existingSpellInfo->SpellFamilyName)
        return true;

    if (!sameCaster)
    {
        // Channeled auras can stack if not forbidden by db or aura type
        if (existingAura->GetSpellInfo()->IsChanneled())
            return true;

        if (m_spellInfo->AttributesEx3 & SPELL_ATTR3_STACK_FOR_DIFF_CASTERS)
            return true;

        // check same periodic auras
        for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            switch (m_spellInfo->Effects[i].ApplyAuraName)
            {
                // DOT or HOT from different casters will stack
                case SPELL_AURA_PERIODIC_DAMAGE:
                case SPELL_AURA_PERIODIC_DUMMY:
                case SPELL_AURA_PERIODIC_HEAL:
                case SPELL_AURA_PERIODIC_TRIGGER_SPELL:
                case SPELL_AURA_PERIODIC_ENERGIZE:
                case SPELL_AURA_PERIODIC_MANA_LEECH:
                case SPELL_AURA_PERIODIC_LEECH:
                case SPELL_AURA_POWER_BURN:
                case SPELL_AURA_OBS_MOD_POWER:
                case SPELL_AURA_OBS_MOD_HEALTH:
                case SPELL_AURA_PERIODIC_TRIGGER_SPELL_WITH_VALUE:
                    // periodic auras which target areas are not allowed to stack this way (replenishment for example)
                    if (m_spellInfo->Effects[i].IsTargetingArea() || existingSpellInfo->Effects[i].IsTargetingArea())
                        break;
                    return true;
                default:
                    break;
            }
        }
    }

    if (HasEffectType(SPELL_AURA_CONTROL_VEHICLE) && existingAura->HasEffectType(SPELL_AURA_CONTROL_VEHICLE))
    {
        Vehicle* veh = NULL;
        if (GetOwner()->ToUnit())
            veh = GetOwner()->ToUnit()->GetVehicleKit();

        if (!veh)           // We should probably just let it stack. Vehicle system will prevent undefined behaviour later
            return true;

        if (!veh->GetAvailableSeatCount())
            return false;   // No empty seat available

        return true; // Empty seat available (skip rest)
    }

    // spell of same spell rank chain
    if (m_spellInfo->IsRankOf(existingSpellInfo))
    {
        // don't allow passive area auras to stack
        if (m_spellInfo->IsMultiSlotAura() && !IsArea())
            return true;
        if (GetCastItemGUID() && existingAura->GetCastItemGUID())
            if (GetCastItemGUID() != existingAura->GetCastItemGUID() && (m_spellInfo->AttributesCu & SPELL_ATTR0_CU_ENCHANT_PROC))
                return true;

        // same spell with same caster should not stack
        return false;
    }

    return true;
}

bool Aura::IsProcOnCooldown() const
{
    /*if (m_procCooldown)
    {
        if (m_procCooldown > time(NULL))
            return true;
    }*/
    return false;
}

void Aura::AddProcCooldown(uint32 /*msec*/)
{
    //m_procCooldown = time(NULL) + msec;
}

void Aura::PrepareProcToTrigger(AuraApplication* aurApp, ProcEventInfo& eventInfo)
{
    bool prepare = CallScriptPrepareProcHandlers(aurApp, eventInfo);
    if (!prepare)
        return;

    // take one charge, aura expiration will be handled in Aura::TriggerProcOnEvent (if needed)
    if (IsUsingCharges())
    {
        --m_procCharges;
        SetNeedClientUpdateForTargets();
    }

    SpellProcEntry const* procEntry = sSpellMgr->GetSpellProcEntry(GetId());

    ASSERT(procEntry);

    // cooldowns should be added to the whole aura (see 51698 area aura)
    AddProcCooldown(procEntry->cooldown);
}

bool Aura::IsProcTriggeredOnEvent(AuraApplication* aurApp, ProcEventInfo& eventInfo) const
{
    SpellProcEntry const* procEntry = sSpellMgr->GetSpellProcEntry(GetId());
    // only auras with spell proc entry can trigger proc
    if (!procEntry)
        return false;

    // check if we have charges to proc with
    if (IsUsingCharges() && !GetCharges())
        return false;

    // check proc cooldown
    if (IsProcOnCooldown())
        return false;

    // TODO:
    // something about triggered spells triggering, and add extra attack effect

    // do checks against db data
    if (!sSpellMgr->CanSpellTriggerProcOnEvent(*procEntry, eventInfo))
        return false;

    // do checks using conditions table
    ConditionList conditions = sConditionMgr->GetConditionsForNotGroupedEntry(CONDITION_SOURCE_TYPE_SPELL_PROC, GetId());
    ConditionSourceInfo condInfo = ConditionSourceInfo(eventInfo.GetActor(), eventInfo.GetActionTarget());
    if (!sConditionMgr->IsObjectMeetToConditions(condInfo, conditions))
        return false;

    // AuraScript Hook
    bool check = const_cast<Aura*>(this)->CallScriptCheckProcHandlers(aurApp, eventInfo);
    if (!check)
        return false;

    // TODO:
    // do allow additional requirements for procs
    // this is needed because this is the last moment in which you can prevent aura charge drop on proc
    // and possibly a way to prevent default checks (if there're going to be any)

    // Check if current equipment meets aura requirements
    // do that only for passive spells
    // TODO: this needs to be unified for all kinds of auras
    Unit* target = aurApp->GetTarget();
    if (IsPassive() && target->GetTypeId() == TYPEID_PLAYER)
    {
        if (GetSpellInfo()->EquippedItemClass == ITEM_CLASS_WEAPON)
        {
            if (target->ToPlayer()->IsInFeralForm())
                return false;

            if (eventInfo.GetDamageInfo())
            {
                WeaponAttackType attType = eventInfo.GetDamageInfo()->GetAttackType();
                Item* item = NULL;
                if (attType == BASE_ATTACK)
                    item = target->ToPlayer()->GetUseableItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
                else if (attType == OFF_ATTACK)
                    item = target->ToPlayer()->GetUseableItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
                else
                    item = target->ToPlayer()->GetUseableItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);

                if (!item || item->IsBroken() || item->GetTemplate()->Class != ITEM_CLASS_WEAPON || !((1<<item->GetTemplate()->SubClass) & GetSpellInfo()->EquippedItemSubClassMask))
                    return false;
            }
        }
        else if (GetSpellInfo()->EquippedItemClass == ITEM_CLASS_ARMOR)
        {
            // Check if player is wearing shield
            Item* item = target->ToPlayer()->GetUseableItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
            if (!item || item->IsBroken() || item->GetTemplate()->Class != ITEM_CLASS_ARMOR || !((1<<item->GetTemplate()->SubClass) & GetSpellInfo()->EquippedItemSubClassMask))
                return false;
        }
    }

    return roll_chance_f(CalcProcChance(*procEntry, eventInfo));
}

float Aura::CalcProcChance(SpellProcEntry const& procEntry, ProcEventInfo& eventInfo) const
{
    float chance = procEntry.chance;
    // calculate chances depending on unit with caster's data
    // so talents modifying chances and judgements will have properly calculated proc chance
    if (Unit* caster = GetCaster())
    {
        // calculate ppm chance if present and we're using weapon
        if (eventInfo.GetDamageInfo() && procEntry.ratePerMinute != 0)
        {
            uint32 WeaponSpeed = caster->GetAttackTime(eventInfo.GetDamageInfo()->GetAttackType());
            chance = caster->GetPPMProcChance(WeaponSpeed, procEntry.ratePerMinute, GetSpellInfo());
        }
        // apply chance modifer aura, applies also to ppm chance (see improved judgement of light spell)
        if (Player* modOwner = caster->GetSpellModOwner())
            modOwner->ApplySpellMod(GetId(), SPELLMOD_CHANCE_OF_SUCCESS, chance);
    }
    return chance;
}

void Aura::TriggerProcOnEvent(AuraApplication* aurApp, ProcEventInfo& eventInfo)
{
    CallScriptProcHandlers(aurApp, eventInfo);

    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (aurApp->HasEffect(i))
            // OnEffectProc / AfterEffectProc hooks handled in AuraEffect::HandleProc()
            GetEffect(i)->HandleProc(aurApp, eventInfo);

    CallScriptAfterProcHandlers(aurApp, eventInfo);

    // Remove aura if we've used last charge to proc
    if (IsUsingCharges() && !GetCharges())
        Remove();
}

void Aura::_DeleteRemovedApplications()
{
    while (!m_removedApplications.empty())
    {
        delete m_removedApplications.front();
        m_removedApplications.pop_front();
    }
}

void Aura::LoadScripts()
{
    sScriptMgr->CreateAuraScripts(m_spellInfo->Id, m_loadedScripts);
    for (std::list<AuraScript*>::iterator itr = m_loadedScripts.begin(); itr != m_loadedScripts.end();)
    {
        if (!(*itr)->_Load(this))
        {
            std::list<AuraScript*>::iterator bitr = itr;
            ++itr;
            delete (*bitr);
            m_loadedScripts.erase(bitr);
            continue;
        }
        TC_LOG_DEBUG("spells", "Aura::LoadScripts: Script `%s` for aura `%u` is loaded now", (*itr)->_GetScriptName()->c_str(), m_spellInfo->Id);
        (*itr)->Register();
        ++itr;
    }
}

bool Aura::CallScriptCheckAreaTargetHandlers(Unit* target)
{
    for (std::list<AuraScript*>::iterator scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_CHECK_AREA_TARGET);
        std::list<AuraScript::CheckAreaTargetHandler>::iterator hookItrEnd = (*scritr)->DoCheckAreaTarget.end(), hookItr = (*scritr)->DoCheckAreaTarget.begin();
        for (; hookItr != hookItrEnd; ++hookItr)
            if (!(*hookItr).Call(*scritr, target))
                return false;
        (*scritr)->_FinishScriptCall();
    }
    return true;
}

void Aura::CallScriptDispel(DispelInfo* dispelInfo)
{
    for (std::list<AuraScript*>::iterator scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_DISPEL);
        std::list<AuraScript::AuraDispelHandler>::iterator hookItrEnd = (*scritr)->OnDispel.end(), hookItr = (*scritr)->OnDispel.begin();
        for (; hookItr != hookItrEnd; ++hookItr)
            (*hookItr).Call(*scritr, dispelInfo);
        (*scritr)->_FinishScriptCall();
    }
}

void Aura::CallScriptAfterDispel(DispelInfo* dispelInfo)
{
    for (std::list<AuraScript*>::iterator scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_AFTER_DISPEL);
        std::list<AuraScript::AuraDispelHandler>::iterator hookItrEnd = (*scritr)->AfterDispel.end(), hookItr = (*scritr)->AfterDispel.begin();
        for (; hookItr != hookItrEnd; ++hookItr)
            (*hookItr).Call(*scritr, dispelInfo);
        (*scritr)->_FinishScriptCall();
    }
}

bool Aura::CallScriptEffectApplyHandlers(AuraEffect const* aurEff, AuraApplication const* aurApp, AuraEffectHandleModes mode)
{
    bool preventDefault = false;
    for (std::list<AuraScript*>::iterator scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_APPLY, aurApp);
        std::list<AuraScript::EffectApplyHandler>::iterator effEndItr = (*scritr)->OnEffectApply.end(), effItr = (*scritr)->OnEffectApply.begin();
        for (; effItr != effEndItr; ++effItr)
        {
            if ((*effItr).IsEffectAffected(m_spellInfo, aurEff->GetEffIndex()))
                (*effItr).Call(*scritr, aurEff, mode);
        }
        if (!preventDefault)
            preventDefault = (*scritr)->_IsDefaultActionPrevented();
        (*scritr)->_FinishScriptCall();
    }
    return preventDefault;
}

bool Aura::CallScriptEffectRemoveHandlers(AuraEffect const* aurEff, AuraApplication const* aurApp, AuraEffectHandleModes mode)
{
    bool preventDefault = false;
    for (std::list<AuraScript*>::iterator scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_REMOVE, aurApp);
        std::list<AuraScript::EffectApplyHandler>::iterator effEndItr = (*scritr)->OnEffectRemove.end(), effItr = (*scritr)->OnEffectRemove.begin();
        for (; effItr != effEndItr; ++effItr)
        {
            if ((*effItr).IsEffectAffected(m_spellInfo, aurEff->GetEffIndex()))
                (*effItr).Call(*scritr, aurEff, mode);
        }
        if (!preventDefault)
            preventDefault = (*scritr)->_IsDefaultActionPrevented();
        (*scritr)->_FinishScriptCall();
    }
    return preventDefault;
}

void Aura::CallScriptAfterEffectApplyHandlers(AuraEffect const* aurEff, AuraApplication const* aurApp, AuraEffectHandleModes mode)
{
    for (std::list<AuraScript*>::iterator scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_AFTER_APPLY, aurApp);
        std::list<AuraScript::EffectApplyHandler>::iterator effEndItr = (*scritr)->AfterEffectApply.end(), effItr = (*scritr)->AfterEffectApply.begin();
        for (; effItr != effEndItr; ++effItr)
        {
            if ((*effItr).IsEffectAffected(m_spellInfo, aurEff->GetEffIndex()))
                (*effItr).Call(*scritr, aurEff, mode);
        }
        (*scritr)->_FinishScriptCall();
    }
}

void Aura::CallScriptAfterEffectRemoveHandlers(AuraEffect const* aurEff, AuraApplication const* aurApp, AuraEffectHandleModes mode)
{
    for (std::list<AuraScript*>::iterator scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_AFTER_REMOVE, aurApp);
        std::list<AuraScript::EffectApplyHandler>::iterator effEndItr = (*scritr)->AfterEffectRemove.end(), effItr = (*scritr)->AfterEffectRemove.begin();
        for (; effItr != effEndItr; ++effItr)
        {
            if ((*effItr).IsEffectAffected(m_spellInfo, aurEff->GetEffIndex()))
                (*effItr).Call(*scritr, aurEff, mode);
        }
        (*scritr)->_FinishScriptCall();
    }
}

bool Aura::CallScriptEffectPeriodicHandlers(AuraEffect const* aurEff, AuraApplication const* aurApp)
{
    bool preventDefault = false;
    for (std::list<AuraScript*>::iterator scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_PERIODIC, aurApp);
        std::list<AuraScript::EffectPeriodicHandler>::iterator effEndItr = (*scritr)->OnEffectPeriodic.end(), effItr = (*scritr)->OnEffectPeriodic.begin();
        for (; effItr != effEndItr; ++effItr)
        {
            if ((*effItr).IsEffectAffected(m_spellInfo, aurEff->GetEffIndex()))
                (*effItr).Call(*scritr, aurEff);
        }
        if (!preventDefault)
            preventDefault = (*scritr)->_IsDefaultActionPrevented();
        (*scritr)->_FinishScriptCall();
    }
    return preventDefault;
}

void Aura::CallScriptEffectUpdatePeriodicHandlers(AuraEffect* aurEff)
{
    for (std::list<AuraScript*>::iterator scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_UPDATE_PERIODIC);
        std::list<AuraScript::EffectUpdatePeriodicHandler>::iterator effEndItr = (*scritr)->OnEffectUpdatePeriodic.end(), effItr = (*scritr)->OnEffectUpdatePeriodic.begin();
        for (; effItr != effEndItr; ++effItr)
        {
            if ((*effItr).IsEffectAffected(m_spellInfo, aurEff->GetEffIndex()))
                (*effItr).Call(*scritr, aurEff);
        }
        (*scritr)->_FinishScriptCall();
    }
}

void Aura::CallScriptEffectUpdateHandlers(AuraEffect* aurEff, const uint32 diff)
{
    for (std::list<AuraScript*>::iterator scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_UPDATE);
        std::list<AuraScript::EffectUpdateHandler>::iterator effEndItr = (*scritr)->OnEffectUpdate.end(), effItr = (*scritr)->OnEffectUpdate.begin();
        for (; effItr != effEndItr; ++effItr)
        {
            if ((*effItr).IsEffectAffected(m_spellInfo, aurEff->GetEffIndex()))
                (*effItr).Call(*scritr, aurEff, diff);
        }
        (*scritr)->_FinishScriptCall();
    }
}

void Aura::CallScriptEffectCalcAmountHandlers(AuraEffect const* aurEff, int32 & amount, bool & canBeRecalculated)
{
    for (std::list<AuraScript*>::iterator scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_CALC_AMOUNT);
        std::list<AuraScript::EffectCalcAmountHandler>::iterator effEndItr = (*scritr)->DoEffectCalcAmount.end(), effItr = (*scritr)->DoEffectCalcAmount.begin();
        for (; effItr != effEndItr; ++effItr)
        {
            if ((*effItr).IsEffectAffected(m_spellInfo, aurEff->GetEffIndex()))
                (*effItr).Call(*scritr, aurEff, amount, canBeRecalculated);
        }
        (*scritr)->_FinishScriptCall();
    }
}

void Aura::CallScriptEffectCalcPeriodicHandlers(AuraEffect const* aurEff, bool & isPeriodic, int32 & amplitude)
{
    for (std::list<AuraScript*>::iterator scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_CALC_PERIODIC);
        std::list<AuraScript::EffectCalcPeriodicHandler>::iterator effEndItr = (*scritr)->DoEffectCalcPeriodic.end(), effItr = (*scritr)->DoEffectCalcPeriodic.begin();
        for (; effItr != effEndItr; ++effItr)
        {
            if ((*effItr).IsEffectAffected(m_spellInfo, aurEff->GetEffIndex()))
                (*effItr).Call(*scritr, aurEff, isPeriodic, amplitude);
        }
        (*scritr)->_FinishScriptCall();
    }
}

void Aura::CallScriptEffectCalcSpellModHandlers(AuraEffect const* aurEff, SpellModifier* & spellMod)
{
    for (std::list<AuraScript*>::iterator scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_CALC_SPELLMOD);
        std::list<AuraScript::EffectCalcSpellModHandler>::iterator effEndItr = (*scritr)->DoEffectCalcSpellMod.end(), effItr = (*scritr)->DoEffectCalcSpellMod.begin();
        for (; effItr != effEndItr; ++effItr)
        {
            if ((*effItr).IsEffectAffected(m_spellInfo, aurEff->GetEffIndex()))
                (*effItr).Call(*scritr, aurEff, spellMod);
        }
        (*scritr)->_FinishScriptCall();
    }
}

void Aura::CallScriptEffectAbsorbHandlers(AuraEffect* aurEff, AuraApplication const* aurApp, DamageInfo & dmgInfo, uint32 & absorbAmount, bool& defaultPrevented)
{
    for (std::list<AuraScript*>::iterator scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_ABSORB, aurApp);
        std::list<AuraScript::EffectAbsorbHandler>::iterator effEndItr = (*scritr)->OnEffectAbsorb.end(), effItr = (*scritr)->OnEffectAbsorb.begin();
        for (; effItr != effEndItr; ++effItr)
        {
            if ((*effItr).IsEffectAffected(m_spellInfo, aurEff->GetEffIndex()))
                (*effItr).Call(*scritr, aurEff, dmgInfo, absorbAmount);
        }
        defaultPrevented = (*scritr)->_IsDefaultActionPrevented();
        (*scritr)->_FinishScriptCall();
    }
}

void Aura::CallScriptEffectAfterAbsorbHandlers(AuraEffect* aurEff, AuraApplication const* aurApp, DamageInfo & dmgInfo, uint32 & absorbAmount)
{
    for (std::list<AuraScript*>::iterator scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_AFTER_ABSORB, aurApp);
        std::list<AuraScript::EffectAbsorbHandler>::iterator effEndItr = (*scritr)->AfterEffectAbsorb.end(), effItr = (*scritr)->AfterEffectAbsorb.begin();
        for (; effItr != effEndItr; ++effItr)
        {
            if ((*effItr).IsEffectAffected(m_spellInfo, aurEff->GetEffIndex()))
                (*effItr).Call(*scritr, aurEff, dmgInfo, absorbAmount);
        }
        (*scritr)->_FinishScriptCall();
    }
}

void Aura::CallScriptEffectManaShieldHandlers(AuraEffect* aurEff, AuraApplication const* aurApp, DamageInfo & dmgInfo, uint32 & absorbAmount, bool & /*defaultPrevented*/)
{
    for (std::list<AuraScript*>::iterator scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_MANASHIELD, aurApp);
        std::list<AuraScript::EffectManaShieldHandler>::iterator effEndItr = (*scritr)->OnEffectManaShield.end(), effItr = (*scritr)->OnEffectManaShield.begin();
        for (; effItr != effEndItr; ++effItr)
        {
            if ((*effItr).IsEffectAffected(m_spellInfo, aurEff->GetEffIndex()))
                (*effItr).Call(*scritr, aurEff, dmgInfo, absorbAmount);
        }
        (*scritr)->_FinishScriptCall();
    }
}

void Aura::CallScriptEffectAfterManaShieldHandlers(AuraEffect* aurEff, AuraApplication const* aurApp, DamageInfo & dmgInfo, uint32 & absorbAmount)
{
    for (std::list<AuraScript*>::iterator scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_AFTER_MANASHIELD, aurApp);
        std::list<AuraScript::EffectManaShieldHandler>::iterator effEndItr = (*scritr)->AfterEffectManaShield.end(), effItr = (*scritr)->AfterEffectManaShield.begin();
        for (; effItr != effEndItr; ++effItr)
        {
            if ((*effItr).IsEffectAffected(m_spellInfo, aurEff->GetEffIndex()))
                (*effItr).Call(*scritr, aurEff, dmgInfo, absorbAmount);
        }
        (*scritr)->_FinishScriptCall();
    }
}

void Aura::CallScriptEffectSplitHandlers(AuraEffect* aurEff, AuraApplication const* aurApp, DamageInfo & dmgInfo, uint32 & splitAmount)
{
    for (std::list<AuraScript*>::iterator scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_SPLIT, aurApp);
        std::list<AuraScript::EffectSplitHandler>::iterator effEndItr = (*scritr)->OnEffectSplit.end(), effItr = (*scritr)->OnEffectSplit.begin();
        for (; effItr != effEndItr; ++effItr)
        {
            if ((*effItr).IsEffectAffected(m_spellInfo, aurEff->GetEffIndex()))
                (*effItr).Call(*scritr, aurEff, dmgInfo, splitAmount);
        }
        (*scritr)->_FinishScriptCall();
    }
}

bool Aura::CallScriptCheckProcHandlers(AuraApplication const* aurApp, ProcEventInfo& eventInfo)
{
    for (std::list<AuraScript*>::iterator scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_CHECK_PROC, aurApp);
        std::list<AuraScript::CheckProcHandler>::iterator hookItrEnd = (*scritr)->DoCheckProc.end(), hookItr = (*scritr)->DoCheckProc.begin();
        for (; hookItr != hookItrEnd; ++hookItr)
            if (!(*hookItr).Call(*scritr, eventInfo))
                return false;
        (*scritr)->_FinishScriptCall();
    }
    return true;
}

bool Aura::CallScriptPrepareProcHandlers(AuraApplication const* aurApp, ProcEventInfo& eventInfo)
{
    bool prepare = true;
    for (std::list<AuraScript*>::iterator scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_PREPARE_PROC, aurApp);
        std::list<AuraScript::AuraProcHandler>::iterator effEndItr = (*scritr)->DoPrepareProc.end(), effItr = (*scritr)->DoPrepareProc.begin();
        for (; effItr != effEndItr; ++effItr)
            (*effItr).Call(*scritr, eventInfo);

        if (prepare && (*scritr)->_IsDefaultActionPrevented())
            prepare = false;
        (*scritr)->_FinishScriptCall();
    }
    return prepare;
}

void Aura::CallScriptProcHandlers(AuraApplication const* aurApp, ProcEventInfo& eventInfo)
{
    for (std::list<AuraScript*>::iterator scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_PROC, aurApp);
        std::list<AuraScript::AuraProcHandler>::iterator hookItrEnd = (*scritr)->OnProc.end(), hookItr = (*scritr)->OnProc.begin();
        for (; hookItr != hookItrEnd; ++hookItr)
            (*hookItr).Call(*scritr, eventInfo);
        (*scritr)->_FinishScriptCall();
    }
}

void Aura::CallScriptAfterProcHandlers(AuraApplication const* aurApp, ProcEventInfo& eventInfo)
{
    for (std::list<AuraScript*>::iterator scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_AFTER_PROC, aurApp);
        std::list<AuraScript::AuraProcHandler>::iterator hookItrEnd = (*scritr)->AfterProc.end(), hookItr = (*scritr)->AfterProc.begin();
        for (; hookItr != hookItrEnd; ++hookItr)
            (*hookItr).Call(*scritr, eventInfo);
        (*scritr)->_FinishScriptCall();
    }
}

bool Aura::CallScriptEffectProcHandlers(AuraEffect const* aurEff, AuraApplication const* aurApp, ProcEventInfo& eventInfo)
{
    bool preventDefault = false;
    for (std::list<AuraScript*>::iterator scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_PROC, aurApp);
        std::list<AuraScript::EffectProcHandler>::iterator effEndItr = (*scritr)->OnEffectProc.end(), effItr = (*scritr)->OnEffectProc.begin();
        for (; effItr != effEndItr; ++effItr)
        {
            if ((*effItr).IsEffectAffected(m_spellInfo, aurEff->GetEffIndex()))
                (*effItr).Call(*scritr, aurEff, eventInfo);
        }
        if (!preventDefault)
            preventDefault = (*scritr)->_IsDefaultActionPrevented();
        (*scritr)->_FinishScriptCall();
    }
    return preventDefault;
}

void Aura::CallScriptAfterEffectProcHandlers(AuraEffect const* aurEff, AuraApplication const* aurApp, ProcEventInfo& eventInfo)
{
    for (std::list<AuraScript*>::iterator scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_AFTER_PROC, aurApp);
        std::list<AuraScript::EffectProcHandler>::iterator effEndItr = (*scritr)->AfterEffectProc.end(), effItr = (*scritr)->AfterEffectProc.begin();
        for (; effItr != effEndItr; ++effItr)
        {
            if ((*effItr).IsEffectAffected(m_spellInfo, aurEff->GetEffIndex()))
                (*effItr).Call(*scritr, aurEff, eventInfo);
        }
        (*scritr)->_FinishScriptCall();
    }
}

UnitAura::UnitAura(SpellInfo const* spellproto, uint8 effMask, WorldObject* owner, Unit* caster, int32 *baseAmount, Item* castItem, uint64 casterGUID)
    : Aura(spellproto, owner, caster, castItem, casterGUID)
{
    LoadScripts();
    _InitEffects(effMask, caster, baseAmount);
    GetUnitOwner()->_AddAura(this, caster);
}

void UnitAura::_ApplyForTarget(Unit* target, Unit* caster, AuraApplication * aurApp)
{
    Aura::_ApplyForTarget(target, caster, aurApp);

    // register aura diminishing on apply
    for (std::list<DiminishingGroup>::const_iterator itr = GetDiminishGroups().begin(); itr != GetDiminishGroups().end(); itr++)
        target->ApplyDiminishingAura(*itr, true);
}

void UnitAura::_UnapplyForTarget(Unit* target, Unit* caster, AuraApplication * aurApp)
{
    Aura::_UnapplyForTarget(target, caster, aurApp);

    // register aura diminishing on apply
    for (std::list<DiminishingGroup>::const_iterator itr = GetDiminishGroups().begin(); itr != GetDiminishGroups().end(); itr++)
        target->ApplyDiminishingAura(*itr, false);
}

void UnitAura::Remove(AuraRemoveMode removeMode)
{
    if (IsRemoved())
        return;
    GetUnitOwner()->RemoveOwnedAura(this, removeMode);
}

void UnitAura::FillTargetMap(std::map<Unit*, uint8> & targets, Unit* caster)
{
    for (uint8 effIndex = 0; effIndex < MAX_SPELL_EFFECTS; ++effIndex)
    {
        if (!HasEffect(effIndex))
            continue;
        UnitList targetList;
        // non-area aura
        if (GetSpellInfo()->Effects[effIndex].Effect == SPELL_EFFECT_APPLY_AURA)
            targetList.push_back(GetUnitOwner());
        else
        {
            float radius = GetSpellInfo()->Effects[effIndex].CalcRadius(caster);
            // Some Effects have no radius defined but the spell contains the same effects with an existing radius
            // Use that one instead
            if (!radius)
            {
                 for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                     if (i != effIndex && GetSpellInfo()->Effects[i].Effect == GetSpellInfo()->Effects[effIndex].Effect)
                     {
                         radius = GetSpellInfo()->Effects[i].CalcRadius(caster);
                         break;
                     }
            }

            bool isolated = GetUnitOwner()->HasUnitState(UNIT_STATE_ISOLATED);
            switch (GetSpellInfo()->Effects[effIndex].Effect)
            {
                case SPELL_EFFECT_APPLY_AREA_AURA_PARTY:
                case SPELL_EFFECT_APPLY_AREA_AURA_RAID:
                {
                    if (!GetUnitOwner()->isAlive() && !GetSpellInfo()->IsDeathPersistent())
                        break;

                    targetList.push_back(GetUnitOwner());
                    if (!isolated)
                    {
                        Trinity::AnyGroupedUnitInObjectRangeCheck u_check(GetUnitOwner(), GetUnitOwner(), radius, GetSpellInfo()->Effects[effIndex].Effect == SPELL_EFFECT_APPLY_AREA_AURA_RAID);
                        Trinity::UnitListSearcher<Trinity::AnyGroupedUnitInObjectRangeCheck> searcher(GetUnitOwner(), targetList, u_check);
                        GetUnitOwner()->VisitNearbyObject(radius, searcher);
                    }
                    break;
                }
                case SPELL_EFFECT_APPLY_AREA_AURA_FRIEND:
                {
                    targetList.push_back(GetUnitOwner());
                    if (!isolated)
                    {
                        Trinity::AnyFriendlyUnitInObjectRangeCheck u_check(GetUnitOwner(), GetUnitOwner(), radius);
                        Trinity::UnitListSearcher<Trinity::AnyFriendlyUnitInObjectRangeCheck> searcher(GetUnitOwner(), targetList, u_check);
                        GetUnitOwner()->VisitNearbyObject(radius, searcher);
                    }
                    break;
                }
                case SPELL_EFFECT_APPLY_AREA_AURA_ENEMY:
                {
                    Trinity::AnyAoETargetUnitInObjectRangeCheck u_check(GetUnitOwner(), GetUnitOwner(), radius); // No GetCharmer in searcher
                    Trinity::UnitListSearcher<Trinity::AnyAoETargetUnitInObjectRangeCheck> searcher(GetUnitOwner(), targetList, u_check);
                    GetUnitOwner()->VisitNearbyObject(radius, searcher);
                    break;
                }
                case SPELL_EFFECT_APPLY_AREA_AURA_PET:
                case SPELL_EFFECT_APPLY_AREA_AURA_OWNER:
                {
                    targetList.push_back(GetUnitOwner());
                    if (Unit* owner = GetUnitOwner()->GetCharmerOrOwner())
                        if (!isolated && GetUnitOwner()->IsWithinDistInMap(owner, radius))
                            targetList.push_back(owner);
                    break;
                }
            }
        }

        for (UnitList::iterator itr = targetList.begin(); itr!= targetList.end();++itr)
        {
            if (GetSpellInfo()->HasAttribute(SPELL_ATTR5_CAN_NOT_TARGET_PETS))
                if ((*itr)->isPet() || (*itr)->isGuardian())
                    continue;
            
            std::map<Unit*, uint8>::iterator existing = targets.find(*itr);
            if (existing != targets.end())
                existing->second |= 1<<effIndex;
            else
                targets[*itr] = 1<<effIndex;
        }
    }
}

DynObjAura::DynObjAura(SpellInfo const* spellproto, uint8 effMask, WorldObject* owner, Unit* caster, int32 *baseAmount, Item* castItem, uint64 casterGUID)
    : Aura(spellproto, owner, caster, castItem, casterGUID)
{
    LoadScripts();
    ASSERT(GetDynobjOwner());
    ASSERT(GetDynobjOwner()->IsInWorld());
    ASSERT(GetDynobjOwner()->GetMap() == caster->GetMap());
    _InitEffects(effMask, caster, baseAmount);
    GetDynobjOwner()->SetAura(this);
}

void DynObjAura::Remove(AuraRemoveMode removeMode)
{
    if (IsRemoved())
        return;
    _Remove(removeMode);
}

void DynObjAura::FillTargetMap(std::map<Unit*, uint8> & targets, Unit* /*caster*/)
{
    Unit* dynObjOwnerCaster = GetDynobjOwner()->GetCaster();
    float radius = GetDynobjOwner()->GetRadius();

    for (uint8 effIndex = 0; effIndex < MAX_SPELL_EFFECTS; ++effIndex)
    {
        if (!HasEffect(effIndex))
            continue;
        UnitList targetList;
        if (GetSpellInfo()->Effects[effIndex].TargetB.GetTarget() == TARGET_DEST_DYNOBJ_ALLY
            || GetSpellInfo()->Effects[effIndex].TargetB.GetTarget() == TARGET_UNIT_DEST_AREA_ALLY)
        {
            Trinity::AnyFriendlyUnitInObjectRangeCheck u_check(GetDynobjOwner(), dynObjOwnerCaster, radius,  GetSpellInfo()->Effects[effIndex].ImplicitTargetConditions);
            Trinity::UnitListSearcher<Trinity::AnyFriendlyUnitInObjectRangeCheck> searcher(GetDynobjOwner(), targetList, u_check);
            GetDynobjOwner()->VisitNearbyObject(radius, searcher);
        }
        else
        {
            Trinity::AnyAoETargetUnitInObjectRangeCheck u_check(GetDynobjOwner(), dynObjOwnerCaster, radius, GetSpellInfo()->Effects[effIndex].ImplicitTargetConditions);
            Trinity::UnitListSearcher<Trinity::AnyAoETargetUnitInObjectRangeCheck> searcher(GetDynobjOwner(), targetList, u_check);
            GetDynobjOwner()->VisitNearbyObject(radius, searcher);
        }

        for (UnitList::iterator itr = targetList.begin(); itr!= targetList.end();++itr)
        {
			// Mainaz: check z level and los dependencies
			//For example flare will not affect units when the flare is on top of the bridge and unit is under it
			Unit* target = *itr;
			float zLevel = GetDynobjOwner()->GetPositionZ();
			if (target->GetPositionZ() + 3.0f < zLevel || target->GetPositionZ() - 5.0f > zLevel)
				if (!target->IsWithinLOSInMap(GetDynobjOwner()))
					continue;

            std::map<Unit*, uint8>::iterator existing = targets.find(*itr);
            if (existing != targets.end())
                existing->second |= 1<<effIndex;
            else
                targets[*itr] = 1<<effIndex;
        }
    }
}