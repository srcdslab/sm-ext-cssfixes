/**
 * vim: set ts=4 :
 * =============================================================================
 * SourceMod Sample Extension
 * Copyright (C) 2004-2008 AlliedModders LLC.  All rights reserved.
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, AlliedModders LLC gives you permission to link the
 * code of this program (as well as its derivative works) to "Half-Life 2," the
 * "Source Engine," the "SourcePawn JIT," and any Game MODs that run on software
 * by the Valve Corporation.  You must obey the GNU General Public License in
 * all respects for all other code used.  Additionally, AlliedModders LLC grants
 * this exception to all derivative works.  AlliedModders LLC defines further
 * exceptions, found in LICENSE.txt (as of this writing, version JULY-31-2007),
 * or <http://www.sourcemod.net/license.php>.
 *
 * Version: $Id$
 */

#include "extension.h"
#include "CDetour/detours.h"
#include "iplayerinfo.h"
#include <sourcehook.h>
#include <sh_memory.h>
#include <IEngineTrace.h>
#include <server_class.h>
#include <ispatialpartition.h>

bool UTIL_ContainsDataTable(SendTable *pTable, const char *name)
{
	const char *pname = pTable->GetName();
	int props = pTable->GetNumProps();
	SendProp *prop;
	SendTable *table;

	if (pname && strcmp(name, pname) == 0)
		return true;

	for (int i=0; i<props; i++)
	{
		prop = pTable->GetProp(i);

		if ((table = prop->GetDataTable()) != NULL)
		{
			pname = table->GetName();
			if (pname && strcmp(name, pname) == 0)
			{
				return true;
			}

			if (UTIL_ContainsDataTable(table, name))
			{
				return true;
			}
		}
	}

	return false;
}

class CTraceFilterSimple : public CTraceFilter
{
public:
	virtual bool ShouldHitEntity( IHandleEntity *pHandleEntity, int contentsMask ) = 0;
	virtual void SetPassEntity( const IHandleEntity *pPassEntity ) = 0;
	virtual void SetCollisionGroup( int iCollisionGroup ) = 0;
};

class CTraceFilterSkipTwoEntities : public CTraceFilterSimple
{
public:
	virtual bool ShouldHitEntity( IHandleEntity *pHandleEntity, int contentsMask ) = 0;
	virtual void SetPassEntity2( const IHandleEntity *pPassEntity2 ) = 0;
};

class CTriggerMoved : public IPartitionEnumerator
{
public:
	virtual IterationRetval_t EnumElement( IHandleEntity *pHandleEntity ) = 0;
};

class CTouchLinks : public IPartitionEnumerator
{
public:
	virtual IterationRetval_t EnumElement( IHandleEntity *pHandleEntity ) = 0;
};

static struct SrcdsPatch
{
	const char *pSignature;
	const unsigned char *pPatchSignature;
	const char *pPatchPattern;
	const unsigned char *pPatch;

	unsigned char *pOriginal;
	uintptr_t pAddress;
	uintptr_t pPatchAddress;
	bool engine;
} gs_Patches[] = {
	// 0: game_ui should not apply FL_ONTRAIN flag, else client prediction turns off
	{
		"_ZN7CGameUI5ThinkEv",
		(unsigned char *)"\xC7\x44\x24\x04\x10\x00\x00\x00\x89\x34\x24\xE8\x00\x00\x00\x00",
		"xxxxxxxxxxxx????",
		(unsigned char *)"\xC7\x44\x24\x04\x10\x00\x00\x00\x89\x34\x24\x90\x90\x90\x90\x90",
		0, 0, 0, false
	},
	// 1: player_speedmod should not turn off flashlight
	{
		"_ZN17CMovementSpeedMod13InputSpeedModER11inputdata_t",
		(unsigned char *)"\xFF\x90\x8C\x05\x00\x00\x85\xC0\x0F\x85\x75\x02\x00\x00",
		"xxxxxxxxxxxxxx",
		(unsigned char *)"\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90",
		0, 0, 0, false
	},
	// 2: only select CT spawnpoints
	{
		"_ZN9CCSPlayer19EntSelectSpawnPointEv",
		(unsigned char *)"\x89\x1C\x24\xE8\x00\x00\x00\x00\x83\xF8\x03\x74\x4B",
		"xxxx????xxxxx",
		(unsigned char *)"\x89\x1C\x24\x90\x90\x90\x90\x90\x90\x90\x90\xEB\x4B",
		0, 0, 0, false
	},
	// 3: don't check if we have T spawns
	{
		"_ZN12CCSGameRules18NeededPlayersCheckERb",
		(unsigned char *)"\x74\x0E\x8B\x83\x80\x02\x00\x00\x85\xC0\x0F\x85\x9E\x00\x00\x00\xC7\x04\x24\xAC\xF7\x87\x00\xE8\xC2\x82\x91\x00",
		"xxxxxxxxxxxxxxxx????????????",
		(unsigned char *)"\x0F\x85\xA8\x00\x00\x00\x8B\x83\x80\x02\x00\x00\x85\xC0\x0F\x85\x9A\x00\x00\x00\x90\x90\x90\x90\x90\x90\x90\x90",
		0, 0, 0, false
	},
	// 4: Special
	{
		"_Z25Physics_RunThinkFunctionsb",
		(unsigned char *)"\x8B\x04\x9E\x85\xC0\x74\x13\xA1\x00\x00\x00\x00\x89\x78\x0C\x8B\x04\x9E\x89\x04\x24\xE8\x00\x00\x00\x00",
		"xxxxxxxx????xxxxxxxxxx????",
		NULL,
		0, 0, 0, false
	},
	// 5: disable alive check in point_viewcontrol->Disable
	{
		"_ZN14CTriggerCamera7DisableEv",
		(unsigned char *)"\x8B\x10\x89\x04\x24\xFF\x92\x08\x01\x00\x00\x84\xC0\x0F\x84\x58\xFF\xFF\xFF",
		"xxxxxxx??xxxxxx?xxx",
		(unsigned char *)"\x8B\x10\x89\x04\x24\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90",
		0, 0, 0, false
	},
	// 6: disable player->m_takedamage = DAMAGE_NO in point_viewcontrol->Enable
	{
		"_ZN14CTriggerCamera6EnableEv",
		(unsigned char *)"\x31\xFF\x80\xBF\xFD\x00\x00\x00\x00\x0F\x85\x96\x03\x00\x00",
		"xxxx?xxxxxx??xx",
		(unsigned char *)"\x31\xFF\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90",
		0, 0, 0, false
	},
	// 7: disable player->m_takedamage = m_nOldTakeDamage in point_viewcontrol->Disable
	{
		"_ZN14CTriggerCamera7DisableEv",
		(unsigned char *)"\x89\xF9\x38\x8E\xFD\x00\x00\x00\x0F\x84\xAC\xFD\xFF\xFF",
		"xxxx?xxxxxxxxx",
		(unsigned char *)"\x89\xF9\x38\x8E\xFD\x00\x00\x00\x90\xE9\xAC\xFD\xFF\xFF",
		0, 0, 0, false
	}
};

class CBaseEntity;
struct variant_hax
{
	const char *pszValue;
};

struct inputdata_t
{
	// The entity that initially caused this chain of output events.
	CBaseEntity *pActivator;
	// The entity that fired this particular output.
	CBaseEntity *pCaller;
	// The data parameter for this output.
	variant_hax value;
	// The unique ID of the output that was fired.
	int nOutputID;
};

typedef bool (*ShouldHitFunc_t)( IHandleEntity *pHandleEntity, int contentsMask );

uintptr_t FindPattern(uintptr_t BaseAddr, const unsigned char *pData, const char *pPattern, size_t MaxSize);
uintptr_t FindFunctionCall(uintptr_t BaseAddr, uintptr_t Function, size_t MaxSize);

/**
 * @file extension.cpp
 * @brief Implement extension code here.
 */

CSSFixes g_Interface;
SMEXT_LINK(&g_Interface);

CGlobalVars *gpGlobals = NULL;
IGameConfig *g_pGameConf = NULL;

IForward *g_pOnRunThinkFunctions = NULL;
IForward *g_pOnPrePlayerThinkFunctions = NULL;
IForward *g_pOnPostPlayerThinkFunctions = NULL;
IForward *g_pOnRunThinkFunctionsPost = NULL;

CDetour *g_pDetour_InputTestActivator = NULL;
CDetour *g_pDetour_PostConstructor = NULL;
CDetour *g_pDetour_FindUseEntity = NULL;
CDetour *g_pDetour_CTraceFilterSimple = NULL;
CDetour *g_pDetour_RunThinkFunctions = NULL;
CDetour *g_pDetour_KeyValue = NULL;
CDetour *g_pDetour_FireBullets = NULL;
CDetour *g_pDetour_SwingOrStab = NULL;
int g_SH_SkipTwoEntitiesShouldHitEntity = 0;
int g_SH_SimpleShouldHitEntity = 0;
int g_SH_TriggerMoved = 0;
int g_SH_TouchLinks = 0;

uintptr_t g_CTraceFilterNoNPCsOrPlayer = 0;
CTraceFilterSkipTwoEntities *g_CTraceFilterSkipTwoEntities = NULL;
CTraceFilterSimple *g_CTraceFilterSimple = NULL;
CTriggerMoved *g_CTriggerMoved = 0;
CTouchLinks *g_CTouchLinks = 0;

/* Fix crash in CBaseFilter::InputTestActivator */
DETOUR_DECL_MEMBER1(DETOUR_InputTestActivator, void, inputdata_t *, inputdata)
{
	if(!inputdata || !inputdata->pActivator || !inputdata->pCaller)
		return;

	DETOUR_MEMBER_CALL(DETOUR_InputTestActivator)(inputdata);
}

DETOUR_DECL_MEMBER1(DETOUR_PostConstructor, void, const char *, szClassname)
{
	if(strncasecmp(szClassname, "info_player_", 12) == 0)
	{
		CBaseEntity *pEntity = (CBaseEntity *)this;

		datamap_t *pMap = gamehelpers->GetDataMap(pEntity);
		typedescription_t *td = gamehelpers->FindInDataMap(pMap, "m_iEFlags");

		// Spawnpoints don't need edicts...
		*(uint32 *)((intptr_t)pEntity + td->fieldOffset[TD_OFFSET_NORMAL]) |= (1<<9); // EFL_SERVER_ONLY

		// Only CT spawnpoints
		if(strcasecmp(szClassname, "info_player_terrorist") == 0)
			szClassname = "info_player_counterterrorist";
	}

	DETOUR_MEMBER_CALL(DETOUR_PostConstructor)(szClassname);
}

DETOUR_DECL_MEMBER2(DETOUR_KeyValue, bool, const char *, szKeyName, const char *, szValue)
{
	// Fix crash bug in engine
	if(strcasecmp(szKeyName, "angle") == 0)
		szKeyName = "angles";

	else if(strcasecmp(szKeyName, "classname") == 0 &&
		strcasecmp(szValue, "info_player_terrorist") == 0)
	{
		// Only CT spawnpoints
		szValue = "info_player_counterterrorist";
	}
	else if(strcasecmp(szKeyName, "teamnum") == 0 || strcasecmp(szKeyName, "teamnum") == 0 )
	{
		CBaseEntity *pEntity = (CBaseEntity *)this;
		const char *pClassname = gamehelpers->GetEntityClassname(pEntity);

		// All buyzones should be CT buyzones
		if(pClassname && strcasecmp(pClassname, "func_buyzone") == 0)
			szValue = "3";
	}

	return DETOUR_MEMBER_CALL(DETOUR_KeyValue)(szKeyName, szValue);
}

/* Ignore players in +USE trace */
bool g_InFindUseEntity = false;
DETOUR_DECL_MEMBER0(DETOUR_FindUseEntity, CBaseEntity *)
{
	// Signal CTraceFilterSimple that we are in FindUseEntity
	g_InFindUseEntity = true;
	CBaseEntity *pEntity = DETOUR_MEMBER_CALL(DETOUR_FindUseEntity)();
	g_InFindUseEntity = false;
	return pEntity;
}
DETOUR_DECL_MEMBER3(DETOUR_CTraceFilterSimple, void, const IHandleEntity *, passedict, int, collisionGroup, ShouldHitFunc_t, pExtraShouldHitFunc)
{
	DETOUR_MEMBER_CALL(DETOUR_CTraceFilterSimple)(passedict, collisionGroup, pExtraShouldHitFunc);

	// If we're in FindUseEntity right now then switch out the VTable
	if(g_InFindUseEntity)
		*(uintptr_t *)this = g_CTraceFilterNoNPCsOrPlayer;
}

/* Make bullets ignore teammates */
char *g_pPhysboxToClientMap = NULL;
bool g_InFireBullets = false;
int g_FireBulletPlayerTeam = 0;
SH_DECL_HOOK2(CTraceFilterSkipTwoEntities, ShouldHitEntity, SH_NOATTRIB, 0, bool, IHandleEntity *, int);
SH_DECL_HOOK2(CTraceFilterSimple, ShouldHitEntity, SH_NOATTRIB, 0, bool, IHandleEntity *, int);
bool ShouldHitEntity(IHandleEntity *pHandleEntity, int contentsMask)
{
	if(!g_InFireBullets)
		RETURN_META_VALUE(MRES_IGNORED, true);

	if(META_RESULT_ORIG_RET(bool) == false)
		RETURN_META_VALUE(MRES_IGNORED, false);

	IServerUnknown *pUnk = (IServerUnknown *)pHandleEntity;
	CBaseHandle hndl = pUnk->GetRefEHandle();
	int index = hndl.GetEntryIndex();

	int iTeam = 0;

	if(index > SM_MAXPLAYERS && g_pPhysboxToClientMap && index < 2048)
	{
		index = g_pPhysboxToClientMap[index];
	}

	if(index >= -3 && index <= -1)
	{
		iTeam = -index;
	}
	else if(index < 1 || index > SM_MAXPLAYERS)
	{
		RETURN_META_VALUE(MRES_IGNORED, true);
	}

	if(!iTeam)
	{
		IGamePlayer *pPlayer = playerhelpers->GetGamePlayer(index);
		if(!pPlayer || !pPlayer->GetEdict())
			RETURN_META_VALUE(MRES_IGNORED, true);

		IPlayerInfo *pInfo = pPlayer->GetPlayerInfo();
		if(!pInfo)
			RETURN_META_VALUE(MRES_IGNORED, true);

		iTeam = pInfo->GetTeamIndex();
	}

	if(iTeam == g_FireBulletPlayerTeam)
		RETURN_META_VALUE(MRES_SUPERCEDE, false);

	RETURN_META_VALUE(MRES_IGNORED, true);
}

DETOUR_DECL_STATIC9(DETOUR_FireBullets, void, int, iPlayerIndex, const Vector *, vOrigin, const QAngle *, vAngles, int, iWeaponID, int, iMode, int, iSeed, float, flSpread, float, _f1, float, _f2)
{
	if(iPlayerIndex <= 0 || iPlayerIndex > playerhelpers->GetMaxClients())
		return DETOUR_STATIC_CALL(DETOUR_FireBullets)(iPlayerIndex, vOrigin, vAngles, iWeaponID, iMode, iSeed, flSpread, _f1, _f2);

	IGamePlayer *pPlayer = playerhelpers->GetGamePlayer(iPlayerIndex);
	if(!pPlayer || !pPlayer->GetEdict())
		return DETOUR_STATIC_CALL(DETOUR_FireBullets)(iPlayerIndex, vOrigin, vAngles, iWeaponID, iMode, iSeed, flSpread, _f1, _f2);

	IPlayerInfo *pInfo = pPlayer->GetPlayerInfo();
	if(!pInfo)
		return DETOUR_STATIC_CALL(DETOUR_FireBullets)(iPlayerIndex, vOrigin, vAngles, iWeaponID, iMode, iSeed, flSpread, _f1, _f2);

	g_FireBulletPlayerTeam = pInfo->GetTeamIndex();

	g_InFireBullets = true;
	DETOUR_STATIC_CALL(DETOUR_FireBullets)(iPlayerIndex, vOrigin, vAngles, iWeaponID, iMode, iSeed, flSpread, _f1, _f2);
	g_InFireBullets = false;
}

DETOUR_DECL_MEMBER1(DETOUR_SwingOrStab, bool, bool, bStab)
{
	static int offset = 0;
	if(!offset)
	{
		IServerUnknown *pUnk = (IServerUnknown *)this;
		IServerNetworkable *pNet = pUnk->GetNetworkable();

		if (!UTIL_ContainsDataTable(pNet->GetServerClass()->m_pTable, "DT_BaseCombatWeapon"))
			return DETOUR_MEMBER_CALL(DETOUR_SwingOrStab)(bStab);

		sm_sendprop_info_t spi;
		if (!gamehelpers->FindSendPropInfo("CBaseCombatWeapon", "m_hOwnerEntity", &spi))
			return DETOUR_MEMBER_CALL(DETOUR_SwingOrStab)(bStab);

		offset = spi.actual_offset;
	}

	CBaseHandle &hndl = *(CBaseHandle *)((uint8_t *)this + offset);

	edict_t *pEdict = gamehelpers->GetHandleEntity(hndl);
	if(!pEdict)
		return DETOUR_MEMBER_CALL(DETOUR_SwingOrStab)(bStab);

	IGamePlayer *pPlayer = playerhelpers->GetGamePlayer(pEdict);
	if(!pPlayer || !pPlayer->GetEdict())
		return DETOUR_MEMBER_CALL(DETOUR_SwingOrStab)(bStab);

	IPlayerInfo *pInfo = pPlayer->GetPlayerInfo();
	if(!pInfo)
		return DETOUR_MEMBER_CALL(DETOUR_SwingOrStab)(bStab);

	g_FireBulletPlayerTeam = pInfo->GetTeamIndex();

	g_InFireBullets = true;
	bool bRet = DETOUR_MEMBER_CALL(DETOUR_SwingOrStab)(bStab);
	g_InFireBullets = false;

	return bRet;
}

cell_t PhysboxToClientMap(IPluginContext *pContext, const cell_t *params)
{
	if(params[2])
		pContext->LocalToPhysAddr(params[1], (cell_t **)&g_pPhysboxToClientMap);
	else
		g_pPhysboxToClientMap = NULL;

	return 0;
}

DETOUR_DECL_STATIC1(DETOUR_RunThinkFunctions, void, bool, simulating)
{
	if(g_pOnRunThinkFunctions->GetFunctionCount())
	{
		g_pOnRunThinkFunctions->PushCell(simulating);
		g_pOnRunThinkFunctions->Execute();
	}

	if(g_pOnPrePlayerThinkFunctions->GetFunctionCount())
	{
		g_pOnPrePlayerThinkFunctions->Execute();
	}

	DETOUR_STATIC_CALL(DETOUR_RunThinkFunctions)(simulating);

	if(g_pOnRunThinkFunctionsPost->GetFunctionCount())
	{
		g_pOnRunThinkFunctionsPost->PushCell(simulating);
		g_pOnRunThinkFunctionsPost->Execute();
	}
}

void (*g_pPhysics_SimulateEntity)(CBaseEntity *pEntity) = NULL;
void Physics_SimulateEntity_CustomLoop(CBaseEntity **ppList, int Count, float Startime)
{
	CBaseEntity *apPlayers[SM_MAXPLAYERS];
	int iPlayers = 0;

	// Remove players from list and put into apPlayers
	for(int i = 0; i < Count; i++)
	{
		CBaseEntity *pEntity = ppList[i];
		if(!pEntity)
			continue;

		edict_t *pEdict = gamehelpers->EdictOfIndex(gamehelpers->EntityToBCompatRef(pEntity));
		if(!pEdict)
			continue;

		int Entity = gamehelpers->IndexOfEdict(pEdict);
		if(Entity >= 1 && Entity <= SM_MAXPLAYERS)
		{
			apPlayers[iPlayers++] = pEntity;
			ppList[i] = NULL;
		}
	}

	// Shuffle players array
	for(int i = iPlayers - 1; i > 0; i--)
	{
		int j = rand() % (i + 1);
		CBaseEntity *pTmp = apPlayers[j];
		apPlayers[j] = apPlayers[i];
		apPlayers[i] = pTmp;
	}

	// Simulate players first
	for(int i = 0; i < iPlayers; i++)
	{
		gpGlobals->curtime = Startime;
		g_pPhysics_SimulateEntity(apPlayers[i]);
	}

	// Post Player simulation done
	if(g_pOnPostPlayerThinkFunctions->GetFunctionCount())
	{
		g_pOnPostPlayerThinkFunctions->Execute();
	}

	// Now simulate the rest
	for(int i = 0; i < Count; i++)
	{
		CBaseEntity *pEntity = ppList[i];
		if(!pEntity)
			continue;

		gpGlobals->curtime = Startime;
		g_pPhysics_SimulateEntity(pEntity);
	}
}

int g_TriggerEntityMoved;
char *g_pFilterTriggerTouchPlayers = NULL;
int g_FilterTriggerMoved = -1;
// void IVEngineServer::TriggerMoved( edict_t *pTriggerEnt, bool testSurroundingBoundsOnly ) = 0;
SH_DECL_HOOK2_void(IVEngineServer, TriggerMoved, SH_NOATTRIB, 0, edict_t *, bool);
void TriggerMoved(edict_t *pTriggerEnt, bool testSurroundingBoundsOnly)
{
	g_TriggerEntityMoved = gamehelpers->IndexOfEdict(pTriggerEnt);

	// Allow all
	if(g_FilterTriggerMoved == -1)
	{
		RETURN_META(MRES_IGNORED);
	}
	// Block All
	else if(g_FilterTriggerMoved == 0)
	{
		RETURN_META(MRES_SUPERCEDE);
	}

	// Decide per entity in TriggerMoved_EnumElement
	RETURN_META(MRES_IGNORED);
}

// IterationRetval_t CTriggerMoved::EnumElement( IHandleEntity *pHandleEntity ) = 0;
SH_DECL_HOOK1(CTriggerMoved, EnumElement, SH_NOATTRIB, 0, IterationRetval_t, IHandleEntity *);
IterationRetval_t TriggerMoved_EnumElement(IHandleEntity *pHandleEntity)
{
	if(g_FilterTriggerMoved <= 0 && !g_pFilterTriggerTouchPlayers)
	{
		RETURN_META_VALUE(MRES_IGNORED, ITERATION_CONTINUE);
	}

	IServerUnknown *pUnk = static_cast< IServerUnknown* >( pHandleEntity );
	CBaseHandle hndl = pUnk->GetRefEHandle();
	int index = hndl.GetEntryIndex();

	// We only care about players
	if(index > SM_MAXPLAYERS)
	{
		RETURN_META_VALUE(MRES_IGNORED, ITERATION_CONTINUE);
	}

	// block touching any clients here if map exists and evaluates to true
	if(g_pFilterTriggerTouchPlayers && g_pFilterTriggerTouchPlayers[g_TriggerEntityMoved])
	{
		RETURN_META_VALUE(MRES_SUPERCEDE, ITERATION_CONTINUE);
	}

	// if filter is active block touch from all other players
	if(g_FilterTriggerMoved > 0 && index != g_FilterTriggerMoved)
	{
		RETURN_META_VALUE(MRES_SUPERCEDE, ITERATION_CONTINUE);
	}

	// allow touch
	RETURN_META_VALUE(MRES_IGNORED, ITERATION_CONTINUE);
}

cell_t FilterTriggerMoved(IPluginContext *pContext, const cell_t *params)
{
	g_FilterTriggerMoved = params[1];
	return 0;
}

cell_t FilterTriggerTouchPlayers(IPluginContext *pContext, const cell_t *params)
{
	if(params[2])
		pContext->LocalToPhysAddr(params[1], (cell_t **)&g_pFilterTriggerTouchPlayers);
	else
		g_pFilterTriggerTouchPlayers = NULL;

	return 0;
}

int g_SolidEntityMoved;
int g_BlockSolidMoved = -1;
char *g_pFilterClientEntityMap = NULL;
// void IVEngineServer::SolidMoved( edict_t *pSolidEnt, ICollideable *pSolidCollide, const Vector* pPrevAbsOrigin, bool testSurroundingBoundsOnly ) = 0;
SH_DECL_HOOK4_void(IVEngineServer, SolidMoved, SH_NOATTRIB, 0, edict_t *, ICollideable *, const Vector *, bool);
void SolidMoved(edict_t *pSolidEnt, ICollideable *pSolidCollide, const Vector *pPrevAbsOrigin, bool testSurroundingBoundsOnly)
{
	g_SolidEntityMoved = gamehelpers->IndexOfEdict(pSolidEnt);

	// Allow all
	if(g_BlockSolidMoved == -1)
	{
		RETURN_META(MRES_IGNORED);
	}
	// Block all
	else if(g_BlockSolidMoved == 0)
	{
		RETURN_META(MRES_SUPERCEDE);
	}

	// Block filtered entity
	if(g_SolidEntityMoved == g_BlockSolidMoved)
	{
		RETURN_META(MRES_SUPERCEDE);
	}

	// Allow all others
	RETURN_META(MRES_IGNORED);
}

// IterationRetval_t CTouchLinks::EnumElement( IHandleEntity *pHandleEntity ) = 0;
SH_DECL_HOOK1(CTouchLinks, EnumElement, SH_NOATTRIB, 0, IterationRetval_t, IHandleEntity *);
IterationRetval_t TouchLinks_EnumElement(IHandleEntity *pHandleEntity)
{
	IServerUnknown *pUnk = static_cast< IServerUnknown* >( pHandleEntity );
	CBaseHandle hndl = pUnk->GetRefEHandle();
	int index = hndl.GetEntryIndex();

	// Optimization: Players shouldn't touch other players
	if(g_SolidEntityMoved <= SM_MAXPLAYERS && index <= SM_MAXPLAYERS)
	{
		RETURN_META_VALUE(MRES_SUPERCEDE, ITERATION_CONTINUE);
	}

	// We only care about players
	if(g_SolidEntityMoved > SM_MAXPLAYERS)
	{
		RETURN_META_VALUE(MRES_IGNORED, ITERATION_CONTINUE);
	}

	// Do we have our filter map and is the entity within it? (can it even be > 2048?)
	if(!g_pFilterClientEntityMap || index >= 2048)
	{
		RETURN_META_VALUE(MRES_IGNORED, ITERATION_CONTINUE);
	}

	// SourcePawn char map[MAXPLAYERS + 1][2048]
	// Contiguous memory, shift array by client idx * 2048
	int arrayIdx = g_SolidEntityMoved * 2048 + index;

	// Block player from touching this entity if it's filtered
	if(g_pFilterClientEntityMap[arrayIdx])
	{
		RETURN_META_VALUE(MRES_SUPERCEDE, ITERATION_CONTINUE);
	}

	// Allow otherwise
	RETURN_META_VALUE(MRES_IGNORED, ITERATION_CONTINUE);
}

cell_t BlockSolidMoved(IPluginContext *pContext, const cell_t *params)
{
	g_BlockSolidMoved = params[1];
	return 0;
}

cell_t FilterClientEntityMap(IPluginContext *pContext, const cell_t *params)
{
	if(params[2])
		pContext->LocalToPhysAddr(params[1], (cell_t **)&g_pFilterClientEntityMap);
	else
		g_pFilterClientEntityMap = NULL;

	return 0;
}

bool CSSFixes::SDK_OnLoad(char *error, size_t maxlength, bool late)
{
	srand((unsigned int)time(NULL));

	char conf_error[255] = "";
	if(!gameconfs->LoadGameConfigFile("CSSFixes", &g_pGameConf, conf_error, sizeof(conf_error)))
	{
		if(conf_error[0])
			snprintf(error, maxlength, "Could not read CSSFixes.txt: %s", conf_error);

		return false;
	}

	CDetourManager::Init(g_pSM->GetScriptingEngine(), g_pGameConf);

	g_pDetour_InputTestActivator = DETOUR_CREATE_MEMBER(DETOUR_InputTestActivator, "CBaseFilter_InputTestActivator");
	if(g_pDetour_InputTestActivator == NULL)
	{
		snprintf(error, maxlength, "Could not create detour for CBaseFilter_InputTestActivator");
		SDK_OnUnload();
		return false;
	}

	g_pDetour_PostConstructor = DETOUR_CREATE_MEMBER(DETOUR_PostConstructor, "CBaseEntity_PostConstructor");
	if(g_pDetour_PostConstructor == NULL)
	{
		snprintf(error, maxlength, "Could not create detour for CBaseEntity_PostConstructor");
		SDK_OnUnload();
		return false;
	}

	g_pDetour_FindUseEntity = DETOUR_CREATE_MEMBER(DETOUR_FindUseEntity, "CBasePlayer_FindUseEntity");
	if(g_pDetour_FindUseEntity == NULL)
	{
		snprintf(error, maxlength, "Could not create detour for CBasePlayer_FindUseEntity");
		SDK_OnUnload();
		return false;
	}

	g_pDetour_CTraceFilterSimple = DETOUR_CREATE_MEMBER(DETOUR_CTraceFilterSimple, "CTraceFilterSimple_CTraceFilterSimple");
	if(g_pDetour_CTraceFilterSimple == NULL)
	{
		snprintf(error, maxlength, "Could not create detour for CTraceFilterSimple_CTraceFilterSimple");
		SDK_OnUnload();
		return false;
	}

	g_pDetour_RunThinkFunctions = DETOUR_CREATE_STATIC(DETOUR_RunThinkFunctions, "Physics_RunThinkFunctions");
	if(g_pDetour_RunThinkFunctions == NULL)
	{
		snprintf(error, maxlength, "Could not create detour for Physics_RunThinkFunctions");
		SDK_OnUnload();
		return false;
	}

	g_pDetour_KeyValue = DETOUR_CREATE_MEMBER(DETOUR_KeyValue, "CBaseEntity_KeyValue");
	if(g_pDetour_KeyValue == NULL)
	{
		snprintf(error, maxlength, "Could not create detour for CBaseEntity_KeyValue");
		SDK_OnUnload();
		return false;
	}

	g_pDetour_FireBullets = DETOUR_CREATE_STATIC(DETOUR_FireBullets, "FX_FireBullets");
	if(g_pDetour_FireBullets == NULL)
	{
		snprintf(error, maxlength, "Could not create detour for FX_FireBullets");
		SDK_OnUnload();
		return false;
	}

	g_pDetour_SwingOrStab = DETOUR_CREATE_MEMBER(DETOUR_SwingOrStab, "CKnife_SwingOrStab");
	if(g_pDetour_SwingOrStab == NULL)
	{
		snprintf(error, maxlength, "Could not create detour for CKnife_SwingOrStab");
		SDK_OnUnload();
		return false;
	}

	g_pDetour_InputTestActivator->EnableDetour();
	g_pDetour_PostConstructor->EnableDetour();
	g_pDetour_FindUseEntity->EnableDetour();
	g_pDetour_CTraceFilterSimple->EnableDetour();
	g_pDetour_RunThinkFunctions->EnableDetour();
	g_pDetour_KeyValue->EnableDetour();
	g_pDetour_FireBullets->EnableDetour();
	g_pDetour_SwingOrStab->EnableDetour();

	// Find VTable for CTraceFilterSkipTwoEntities
	uintptr_t pCTraceFilterSkipTwoEntities;
	if(!g_pGameConf->GetMemSig("CTraceFilterSkipTwoEntities", (void **)(&pCTraceFilterSkipTwoEntities)) || !pCTraceFilterSkipTwoEntities)
	{
		snprintf(error, maxlength, "Failed to find CTraceFilterSkipTwoEntities.\n");
		SDK_OnUnload();
		return false;
	}
	// First function in VTable
	g_CTraceFilterSkipTwoEntities = (CTraceFilterSkipTwoEntities *)(pCTraceFilterSkipTwoEntities + 8);

	// Find VTable for CTraceFilterSimple
	uintptr_t pCTraceFilterSimple;
	if(!g_pGameConf->GetMemSig("CTraceFilterSimple", (void **)(&pCTraceFilterSimple)) || !pCTraceFilterSimple)
	{
		snprintf(error, maxlength, "Failed to find CTraceFilterSimple.\n");
		SDK_OnUnload();
		return false;
	}
	// First function in VTable
	g_CTraceFilterSimple = (CTraceFilterSimple *)(pCTraceFilterSimple + 8);

	// Find VTable for CTraceFilterNoNPCsOrPlayer
	uintptr_t pCTraceFilterNoNPCsOrPlayer;
	if(!g_pGameConf->GetMemSig("CTraceFilterNoNPCsOrPlayer", (void **)(&pCTraceFilterNoNPCsOrPlayer)) || !pCTraceFilterNoNPCsOrPlayer)
	{
		snprintf(error, maxlength, "Failed to find CTraceFilterNoNPCsOrPlayer.\n");
		SDK_OnUnload();
		return false;
	}
	// First function in VTable
	g_CTraceFilterNoNPCsOrPlayer = pCTraceFilterNoNPCsOrPlayer + 8;

	g_SH_SkipTwoEntitiesShouldHitEntity = SH_ADD_DVPHOOK(CTraceFilterSkipTwoEntities, ShouldHitEntity, g_CTraceFilterSkipTwoEntities, SH_STATIC(ShouldHitEntity), true);
	g_SH_SimpleShouldHitEntity = SH_ADD_DVPHOOK(CTraceFilterSimple, ShouldHitEntity, g_CTraceFilterSimple, SH_STATIC(ShouldHitEntity), true);

	// Find VTable for CTriggerMoved
	uintptr_t pCTriggerMoved;
	if(!g_pGameConf->GetMemSig("CTriggerMoved", (void **)(&pCTriggerMoved)) || !pCTriggerMoved)
	{
		snprintf(error, maxlength, "Failed to find CTriggerMoved.\n");
		SDK_OnUnload();
		return false;
	}
	// First function in VTable
	g_CTriggerMoved = (CTriggerMoved *)(pCTriggerMoved + 8);

	// Find VTable for CTouchLinks
	uintptr_t pCTouchLinks;
	if(!g_pGameConf->GetMemSig("CTouchLinks", (void **)(&pCTouchLinks)) || !pCTouchLinks)
	{
		snprintf(error, maxlength, "Failed to find CTouchLinks.\n");
		SDK_OnUnload();
		return false;
	}
	// First function in VTable
	g_CTouchLinks = (CTouchLinks *)(pCTouchLinks + 8);

	g_SH_TriggerMoved = SH_ADD_DVPHOOK(CTriggerMoved, EnumElement, g_CTriggerMoved, SH_STATIC(TriggerMoved_EnumElement), false);
	g_SH_TouchLinks = SH_ADD_DVPHOOK(CTouchLinks, EnumElement, g_CTouchLinks, SH_STATIC(TouchLinks_EnumElement), false);

	SH_ADD_HOOK(IVEngineServer, TriggerMoved, engine, SH_STATIC(TriggerMoved), false);
	SH_ADD_HOOK(IVEngineServer, SolidMoved, engine, SH_STATIC(SolidMoved), false);

	if(!g_pGameConf->GetMemSig("Physics_SimulateEntity", (void **)(&g_pPhysics_SimulateEntity)) || !g_pPhysics_SimulateEntity)
	{
		snprintf(error, maxlength, "Failed to find Physics_SimulateEntity.\n");
		SDK_OnUnload();
		return false;
	}

	void *pServerSo = dlopen("cstrike/bin/server_srv.so", RTLD_NOW);
	if(!pServerSo)
	{
		snprintf(error, maxlength, "Could not dlopen server_srv.so");
		SDK_OnUnload();
		return false;
	}

	void *pEngineSo = dlopen("bin/engine_srv.so", RTLD_NOW);
	if(!pEngineSo)
	{
		snprintf(error, maxlength, "Could not dlopen engine_srv.so");
		SDK_OnUnload();
		return false;
	}

	/* 4: Special */
	uintptr_t pAddress = (uintptr_t)memutils->ResolveSymbol(pServerSo, gs_Patches[4].pSignature);
	if(!pAddress)
	{
		snprintf(error, maxlength, "Could not find symbol: %s", gs_Patches[4].pSignature);
		dlclose(pServerSo);
		dlclose(pEngineSo);
		SDK_OnUnload();
		return false;
	}

	uintptr_t pPatchAddress = FindPattern(pAddress, gs_Patches[4].pPatchSignature, gs_Patches[4].pPatchPattern, 1024);
	if(!pPatchAddress)
	{
		snprintf(error, maxlength, "Could not find patch signature for symbol: %s", gs_Patches[4].pSignature);
		dlclose(pServerSo);
		dlclose(pEngineSo);
		SDK_OnUnload();
		return false;
	}

	// mov [esp+8], edi ; startime
	// mov [esp+4], eax ; count
	// mov [esp], esi ; **list
	// call NULL ; <- our func here
	// mov eax, ds:gpGlobals ; restore
	// jmp +11 ; jump over useless instructions
	static unsigned char aPatch[] = "\x89\x7C\x24\x08\x89\x44\x24\x04\x89\x34\x24\xE8\x00\x00\x00\x00\xA1\x00\x00\x00\x00\xEB\x0B\x90\x90\x90";
	gs_Patches[4].pPatch = aPatch;

	// put our function address into the relative call instruction
	// relative call: new PC = PC + imm1
	// call is at + 11 after pPatchAddress
	// PC will be past our call instruction so + 5
	*(uintptr_t *)&aPatch[12] = (uintptr_t)Physics_SimulateEntity_CustomLoop - (pPatchAddress + 11 + 5);

	// restore "mov eax, ds:gpGlobals" from original to our patch after the call
	*(uintptr_t *)&aPatch[17] = *(uintptr_t *)(pPatchAddress + 8);

	// Apply all patches
	for(size_t i = 0; i < sizeof(gs_Patches) / sizeof(*gs_Patches); i++)
	{
		struct SrcdsPatch *pPatch = &gs_Patches[i];
		int PatchLen = strlen(pPatch->pPatchPattern);

		void *pBinary = pPatch->engine ? pEngineSo : pServerSo;
		pPatch->pAddress = (uintptr_t)memutils->ResolveSymbol(pBinary, pPatch->pSignature);
		if(!pPatch->pAddress)
		{
			snprintf(error, maxlength, "Could not find symbol: %s", pPatch->pSignature);
			dlclose(pServerSo);
			dlclose(pEngineSo);
			SDK_OnUnload();
			return false;
		}

		pPatch->pPatchAddress = FindPattern(pPatch->pAddress, pPatch->pPatchSignature, pPatch->pPatchPattern, 1024);
		if(!pPatch->pPatchAddress)
		{
			snprintf(error, maxlength, "Could not find patch signature for symbol: %s", pPatch->pSignature);
			dlclose(pServerSo);
			dlclose(pEngineSo);
			SDK_OnUnload();
			return false;
		}

		pPatch->pOriginal = (unsigned char *)malloc(PatchLen * sizeof(unsigned char));

		SourceHook::SetMemAccess((void *)pPatch->pPatchAddress, PatchLen, SH_MEM_READ|SH_MEM_WRITE|SH_MEM_EXEC);
		for(int j = 0; j < PatchLen; j++)
		{
			pPatch->pOriginal[j] = *(unsigned char *)(pPatch->pPatchAddress + j);
			*(unsigned char *)(pPatch->pPatchAddress + j) = pPatch->pPatch[j];
		}
		SourceHook::SetMemAccess((void *)pPatch->pPatchAddress, PatchLen, SH_MEM_READ|SH_MEM_EXEC);
	}

	dlclose(pServerSo);
	dlclose(pEngineSo);

	g_pOnRunThinkFunctions = forwards->CreateForward("OnRunThinkFunctions", ET_Ignore, 1, NULL, Param_Cell);
	g_pOnPrePlayerThinkFunctions = forwards->CreateForward("OnPrePlayerThinkFunctions", ET_Ignore, 0, NULL);
	g_pOnPostPlayerThinkFunctions = forwards->CreateForward("OnPostPlayerThinkFunctions", ET_Ignore, 0, NULL);
	g_pOnRunThinkFunctionsPost = forwards->CreateForward("OnRunThinkFunctionsPost", ET_Ignore, 1, NULL, Param_Cell);

	return true;
}

const sp_nativeinfo_t MyNatives[] =
{
	{ "FilterTriggerMoved", FilterTriggerMoved },
	{ "BlockSolidMoved", BlockSolidMoved },
	{ "FilterClientEntityMap", FilterClientEntityMap },
	{ "FilterTriggerTouchPlayers", FilterTriggerTouchPlayers },
	{ "PhysboxToClientMap", PhysboxToClientMap },
	{ NULL, NULL }
};

void CSSFixes::SDK_OnAllLoaded()
{
	sharesys->AddNatives(myself, MyNatives);
	sharesys->RegisterLibrary(myself, "CSSFixes");
}

void CSSFixes::SDK_OnUnload()
{
	if(g_pDetour_InputTestActivator != NULL)
	{
		g_pDetour_InputTestActivator->Destroy();
		g_pDetour_InputTestActivator = NULL;
	}

	if(g_pDetour_PostConstructor != NULL)
	{
		g_pDetour_PostConstructor->Destroy();
		g_pDetour_PostConstructor = NULL;
	}

	if(g_pDetour_FindUseEntity != NULL)
	{
		g_pDetour_FindUseEntity->Destroy();
		g_pDetour_FindUseEntity = NULL;
	}

	if(g_pDetour_CTraceFilterSimple != NULL)
	{
		g_pDetour_CTraceFilterSimple->Destroy();
		g_pDetour_CTraceFilterSimple = NULL;
	}

	if(g_pDetour_RunThinkFunctions != NULL)
	{
		g_pDetour_RunThinkFunctions->Destroy();
		g_pDetour_RunThinkFunctions = NULL;
	}

	if(g_pDetour_KeyValue != NULL)
	{
		g_pDetour_KeyValue->Destroy();
		g_pDetour_KeyValue = NULL;
	}

	if(g_pDetour_FireBullets != NULL)
	{
		g_pDetour_FireBullets->Destroy();
		g_pDetour_FireBullets = NULL;
	}

	if(g_pDetour_SwingOrStab != NULL)
	{
		g_pDetour_SwingOrStab->Destroy();
		g_pDetour_SwingOrStab = NULL;
	}

	if(g_pOnRunThinkFunctions != NULL)
	{
		forwards->ReleaseForward(g_pOnRunThinkFunctions);
		g_pOnRunThinkFunctions = NULL;
	}

	if(g_pOnRunThinkFunctionsPost != NULL)
	{
		forwards->ReleaseForward(g_pOnRunThinkFunctionsPost);
		g_pOnRunThinkFunctionsPost = NULL;
	}

	if(g_pOnPrePlayerThinkFunctions != NULL)
	{
		forwards->ReleaseForward(g_pOnPrePlayerThinkFunctions);
		g_pOnPrePlayerThinkFunctions = NULL;
	}

	if(g_pOnPostPlayerThinkFunctions != NULL)
	{
		forwards->ReleaseForward(g_pOnPostPlayerThinkFunctions);
		g_pOnPostPlayerThinkFunctions = NULL;
	}

	if(g_SH_SkipTwoEntitiesShouldHitEntity)
		SH_REMOVE_HOOK_ID(g_SH_SkipTwoEntitiesShouldHitEntity);

	if(g_SH_SimpleShouldHitEntity)
		SH_REMOVE_HOOK_ID(g_SH_SimpleShouldHitEntity);

	if(g_SH_TriggerMoved)
		SH_REMOVE_HOOK_ID(g_SH_TriggerMoved);

	if(g_SH_TouchLinks)
		SH_REMOVE_HOOK_ID(g_SH_TouchLinks);

	SH_REMOVE_HOOK(IVEngineServer, TriggerMoved, engine, SH_STATIC(TriggerMoved), false);
	SH_REMOVE_HOOK(IVEngineServer, SolidMoved, engine, SH_STATIC(SolidMoved), false);

	gameconfs->CloseGameConfigFile(g_pGameConf);

	// Revert all applied patches
	for(size_t i = 0; i < sizeof(gs_Patches) / sizeof(*gs_Patches); i++)
	{
		struct SrcdsPatch *pPatch = &gs_Patches[i];
		int PatchLen = strlen(pPatch->pPatchPattern);

		if(!pPatch->pOriginal)
			continue;

		SourceHook::SetMemAccess((void *)pPatch->pPatchAddress, PatchLen, SH_MEM_READ|SH_MEM_WRITE|SH_MEM_EXEC);
		for(int j = 0; j < PatchLen; j++)
		{
			*(unsigned char *)(pPatch->pPatchAddress + j) = pPatch->pOriginal[j];
		}
		SourceHook::SetMemAccess((void *)pPatch->pPatchAddress, PatchLen, SH_MEM_READ|SH_MEM_EXEC);

		free(pPatch->pOriginal);
		pPatch->pOriginal = NULL;
	}
}

bool CSSFixes::SDK_OnMetamodLoad(ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer, INTERFACEVERSION_VENGINESERVER);
	gpGlobals = ismm->GetCGlobals();
	return true;
}

uintptr_t FindPattern(uintptr_t BaseAddr, const unsigned char *pData, const char *pPattern, size_t MaxSize)
{
	unsigned char *pMemory;
	uintptr_t PatternLen = strlen(pPattern);

	pMemory = reinterpret_cast<unsigned char *>(BaseAddr);

	for(uintptr_t i = 0; i < MaxSize; i++)
	{
		uintptr_t Matches = 0;
		while(*(pMemory + i + Matches) == pData[Matches] || pPattern[Matches] != 'x')
		{
			Matches++;
			if(Matches == PatternLen)
				return (uintptr_t)(pMemory + i);
		}
	}

	return 0x00;
}

uintptr_t FindFunctionCall(uintptr_t BaseAddr, uintptr_t Function, size_t MaxSize)
{
	unsigned char *pMemory;
	pMemory = reinterpret_cast<unsigned char *>(BaseAddr);

	for(uintptr_t i = 0; i < MaxSize; i++)
	{
		if(pMemory[i] == 0xE8) // CALL
		{
			uintptr_t CallAddr = *(uintptr_t *)(pMemory + i + 1);

			CallAddr += (uintptr_t)(pMemory + i + 5);

			if(CallAddr == Function)
				return (uintptr_t)(pMemory + i);

			i += 4;
		}
	}

	return 0x00;
}
