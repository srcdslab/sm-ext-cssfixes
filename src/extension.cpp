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
#include "convarhelper.h"
#include "CDetour/detours.h"
#include "iplayerinfo.h"
#include <sourcehook.h>
#include <sh_memory.h>
#include <IEngineTrace.h>
#include <server_class.h>
#include <ispatialpartition.h>
#include <utlvector.h>
#include <string_t.h>

#define VPROF_ENABLED
#include <tier0/vprof.h>

#define SetBit(A,I)		((A)[(I) >> 5] |= (1 << ((I) & 31)))
#define ClearBit(A,I)	((A)[(I) >> 5] &= ~(1 << ((I) & 31)))
#define CheckBit(A,I)	!!((A)[(I) >> 5] & (1 << ((I) & 31)))

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

void UTIL_StringToVector( float *pVector, const char *pString )
{
	char *pstr, *pfront, tempString[128];
	int	j;

	Q_strncpy( tempString, pString, sizeof(tempString) );
	pstr = pfront = tempString;

	for ( j = 0; j < 3; j++ )			// lifted from pr_edict.c
	{
		pVector[j] = atof( pfront );

		// skip any leading whitespace
		while ( *pstr && *pstr <= ' ' )
			pstr++;

		// skip to next whitespace
		while ( *pstr && *pstr > ' ' )
			pstr++;

		if (!*pstr)
			break;

		pstr++;
		pfront = pstr;
	}
	for ( j++; j < 3; j++ )
	{
		pVector[j] = 0;
	}
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

struct SrcdsPatch
{
	const char *pSignature; // function symbol
	const unsigned char *pPatchSignature; // original opcode signature | function symbol for functionCall = true
	const char *pPatchPattern; // pattern = x/?, ? = ignore signature
	const unsigned char *pPatch; // replace with bytes
	const char *pLibrary; // library of function symbol pSignature

	int range = 0x400; // search range: scan up to this many bytes for the signature
	int occurrences = 1; // maximum(!) number of occurences to patch
	bool functionCall = false; // true = FindFunctionCall (pPatchSignature = function symbol) | false = FindPattern
	const char *pFunctionLibrary = ""; // library of function symbol pPatchSignature for functionCall = true

	struct Restore
	{
		unsigned char *pOriginal = NULL;
		uintptr_t pPatchAddress = 0;
		struct Restore *pNext = NULL;
	} *pRestore = NULL;

	uintptr_t pAddress = 0;
	uintptr_t pSignatureAddress = 0;
};

class CBaseEntity;
struct variant_hax
{
	const char *pszValue;
};

struct ResponseContext_t
{
	string_t m_iszName;
	string_t m_iszValue;
	float m_fExpirationTime;
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

ConVar *g_SvForceCTSpawn = CreateConVar("sv_cssfixes_force_ct_spawnpoints", "1", FCVAR_NOTIFY, "Forces all spawnpoints to be on CT side");
ConVar *g_SvSkipCashReset = CreateConVar("sv_cssfixes_skip_cash_reset", "1", FCVAR_NOTIFY, "Skip reset cash to 16000 when buying an item");
ConVar *g_SvGameEndUnFreeze = CreateConVar("sv_cssfixes_gameend_unfreeze", "1", FCVAR_NOTIFY, "Allow people to run around freely after game end");
ConVar *g_SvAlwaysTransmitPointViewControl = CreateConVar("sv_cssfixes_always_transmit_point_viewcontrol", "0", FCVAR_NOTIFY, "Always transmit point_viewcontrol for debugging purposes");
ConVar *g_SvLogs = CreateConVar("sv_cssfixes_logs", "0", FCVAR_NOTIFY, "Add extra logs of action performed");

std::vector<SrcdsPatch> gs_Patches = {};

IGameConfig *g_pGameConf = NULL;

CDetour *g_pDetour_InputTestActivator = NULL;
CDetour *g_pDetour_PostConstructor = NULL;
CDetour *g_pDetour_CreateEntityByName = NULL;
CDetour *g_pDetour_PassesFilterImpl = NULL;
CDetour *g_pDetour_FindUseEntity = NULL;
CDetour *g_pDetour_CTraceFilterSimple = NULL;
CDetour *g_pDetour_KeyValue = NULL;
CDetour *g_pDetour_FireBullets = NULL;
CDetour *g_pDetour_SwingOrStab = NULL;
int g_SH_SkipTwoEntitiesShouldHitEntity = 0;
int g_SH_SimpleShouldHitEntity = 0;

int g_iMaxPlayers = 0;

uintptr_t g_CTraceFilterNoNPCsOrPlayer = 0;
CTraceFilterSkipTwoEntities *g_CTraceFilterSkipTwoEntities = NULL;
CTraceFilterSimple *g_CTraceFilterSimple = NULL;

/* Fix crash in CBaseFilter::InputTestActivator */
DETOUR_DECL_MEMBER1(DETOUR_InputTestActivator, void, inputdata_t *, inputdata)
{
	if(!inputdata || !inputdata->pActivator || !inputdata->pCaller)
		return;

	DETOUR_MEMBER_CALL(DETOUR_InputTestActivator)(inputdata);
}

const char *pszNonEdicts[] =
{
	"game_score",
	"game_text",
	"game_ui",
	"logic_auto",	// bruh
	"phys_thruster",
	"phys_keepupright",
	"player_speedmod",
	"player_weaponstrip",
	"point_clientcommand",
	"point_servercommand",
	"point_teleport",
};

DETOUR_DECL_MEMBER1(DETOUR_PostConstructor, void, const char *, szClassname)
{
	VPROF_ENTER_SCOPE("CSSFixes::DETOUR_PostConstructor");

	CBaseEntity *pEntity = (CBaseEntity *)this;

	static datamap_t *pMap = gamehelpers->GetDataMap(pEntity);
	static typedescription_t *td = gamehelpers->FindInDataMap(pMap, "m_iEFlags");
	static uint32 offset = td->fieldOffset[TD_OFFSET_NORMAL];

	if(strncasecmp(szClassname, "info_player_", 12) == 0)
	{
		// Spawnpoints don't need edicts...
		*(uint32 *)((intptr_t)pEntity + offset) |= (1<<9); // EFL_SERVER_ONLY

		// Only CT spawnpoints
		if(g_SvForceCTSpawn->GetInt() && strcasecmp(szClassname, "info_player_terrorist") == 0)
		{
			if (g_SvLogs->GetInt())
			{
				g_pSM->LogMessage(myself, "Forcing CT spawn");
			}
			szClassname = "info_player_counterterrorist";
		}

		DETOUR_MEMBER_CALL(DETOUR_PostConstructor)(szClassname);
		return;
	}

	// Remove edicts for a bunch of entities that REALLY don't need them
	for (int i = 0; i < sizeof(pszNonEdicts)/sizeof(*pszNonEdicts); i++)
	{
		if (!strcasecmp(szClassname, pszNonEdicts[i]))
		{
			*(uint32 *)((intptr_t)pEntity + offset) |= (1<<9); // EFL_SERVER_ONLY
		}
	}

	DETOUR_MEMBER_CALL(DETOUR_PostConstructor)(szClassname);

	VPROF_EXIT_SCOPE();
}

// Implementation for custom filter entities
DETOUR_DECL_MEMBER2(DETOUR_PassesFilterImpl, bool, CBaseEntity*, pCaller, CBaseEntity*, pEntity)
{
	CBaseEntity* pThisEnt = (CBaseEntity*)this;

	// filter_activator_context: filters activators based on whether they have a given context with a nonzero value
	// https://developer.valvesoftware.com/wiki/Filter_activator_context
	// Implemented here because CUtlVectors are not supported in sourcepawn
	if (!strcasecmp(gamehelpers->GetEntityClassname(pThisEnt), "filter_activator_context"))
	{
		static int m_ResponseContexts_offset = 0, m_iszResponseContext_offset = 0;

		if (!m_ResponseContexts_offset && !m_iszResponseContext_offset)
		{
			datamap_t *pDataMap = gamehelpers->GetDataMap(pEntity);
			sm_datatable_info_t info;

			// Both are CBaseEntity members, so the offsets will always be the same across different entity classes
			gamehelpers->FindDataMapInfo(pDataMap, "m_ResponseContexts", &info);
			m_ResponseContexts_offset = info.actual_offset;

			gamehelpers->FindDataMapInfo(pDataMap, "m_iszResponseContext", &info);
			m_iszResponseContext_offset = info.actual_offset;
		}

		CUtlVector<ResponseContext_t> vecResponseContexts;
		vecResponseContexts = *(CUtlVector<ResponseContext_t>*)((uint8_t*)pEntity + m_ResponseContexts_offset);

		const char *szFilterContext = (*(string_t*)((uint8_t*)pThisEnt + m_iszResponseContext_offset)).ToCStr();
		const char *szContext;
		int iContextValue;

		for (int i = 0; i < vecResponseContexts.Count(); i++)
		{
			szContext = vecResponseContexts[i].m_iszName.ToCStr();
			iContextValue = atoi(vecResponseContexts[i].m_iszValue.ToCStr());

			if (!strcasecmp(szFilterContext, szContext) && iContextValue > 0)
				return true;
		}

		return false;
	}

	// CBaseFilter::PassesFilterImpl just returns true so no need to call it
	return true;
}

// Switch new entity classnames to ones that can be instantiated while keeping the classname keyvalue intact so it can be used later
DETOUR_DECL_STATIC2(DETOUR_CreateEntityByName, CBaseEntity*, const char*, className, int, iForceEdictIndex)
{
	VPROF_ENTER_SCOPE("CSSFixes::DETOUR_CreateEntityByName");

	// Nice of valve to expose CBaseFilter as filter_base :)
	if (strcasecmp(className, "filter_activator_context") == 0)
		className = "filter_base";

	CBaseEntity *pEntity = DETOUR_STATIC_CALL(DETOUR_CreateEntityByName)(className, iForceEdictIndex);

	VPROF_EXIT_SCOPE();

	return pEntity;
}

DETOUR_DECL_MEMBER2(DETOUR_KeyValue, bool, const char *, szKeyName, const char *, szValue)
{
	VPROF_ENTER_SCOPE("CSSFixes::DETOUR_KeyValue");

	CBaseEntity *pEntity = (CBaseEntity *)this;

	// Fix crash bug in engine
	if(strcasecmp(szKeyName, "angle") == 0)
	{
		szKeyName = "angles";
	}
	else if(g_SvForceCTSpawn->GetInt() &&
		strcasecmp(szKeyName, "classname") == 0 &&
		strcasecmp(szValue, "info_player_terrorist") == 0)
	{
		if (g_SvLogs->GetInt())
		{
			g_pSM->LogMessage(myself, "Forcing CT spawn");
		}

		// Only CT spawnpoints
		szValue = "info_player_counterterrorist";
	}
	else if(g_SvForceCTSpawn->GetInt() && (strcasecmp(szKeyName, "teamnum") == 0 || strcasecmp(szKeyName, "teamnum") == 0))
	{
		const char *pClassname = gamehelpers->GetEntityClassname(pEntity);

		if (g_SvLogs->GetInt())
		{
			g_pSM->LogMessage(myself, "Forcing CT buyzone");
		}

		// All buyzones should be CT buyzones
		if(pClassname && strcasecmp(pClassname, "func_buyzone") == 0)
			szValue = "3";
	}
	else if(strcasecmp(szKeyName, "absvelocity") == 0)
	{
		static int m_AbsVelocity_offset = 0;

		if (!m_AbsVelocity_offset)
		{
			datamap_t *pDataMap = gamehelpers->GetDataMap(pEntity);
			sm_datatable_info_t info;

			gamehelpers->FindDataMapInfo(pDataMap, "m_vecAbsVelocity", &info);
			m_AbsVelocity_offset = info.actual_offset;
		}

		float tmp[3];
		UTIL_StringToVector(tmp, szValue);

		Vector *vecAbsVelocity = (Vector*)((uint8_t*)pEntity + m_AbsVelocity_offset);
		vecAbsVelocity->Init(tmp[0], tmp[1], tmp[2]);
	}

	bool bHandled = DETOUR_MEMBER_CALL(DETOUR_KeyValue)(szKeyName, szValue);

	VPROF_EXIT_SCOPE();

	return bHandled;
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
	if (g_InFindUseEntity)
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

	if(index > g_iMaxPlayers && g_pPhysboxToClientMap && index < 2048)
	{
		index = g_pPhysboxToClientMap[index];
	}

	if(index >= -3 && index <= -1)
	{
		iTeam = -index;
	}
	else if(index < 1 || index > g_iMaxPlayers)
	{
		RETURN_META_VALUE(MRES_IGNORED, true);
	}

	char lifeState = 0;
	if(!iTeam)
	{
		IGamePlayer *pPlayer = playerhelpers->GetGamePlayer(index);
		if(!pPlayer || !pPlayer->GetEdict())
			RETURN_META_VALUE(MRES_IGNORED, true);

		IPlayerInfo *pInfo = pPlayer->GetPlayerInfo();
		if(!pInfo)
			RETURN_META_VALUE(MRES_IGNORED, true);

		iTeam = pInfo->GetTeamIndex();

		static int offset = 0;
		if(!offset)
		{
			sm_sendprop_info_t spi;
			if (!gamehelpers->FindSendPropInfo("CBasePlayer", "m_lifeState", &spi))
				RETURN_META_VALUE(MRES_IGNORED, true);

			offset = spi.actual_offset;
		}

		lifeState = *(char *)((uint8_t *)pHandleEntity + offset);
	}

	if(iTeam == g_FireBulletPlayerTeam || lifeState != 0)
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

bool CSSFixes::SDK_OnLoad(char *error, size_t maxlength, bool late)
{
	AutoExecConfig(g_pCVar, true);

	srand((unsigned int)time(NULL));

	g_iMaxPlayers = playerhelpers->GetMaxClients();

	char conf_error[255] = "";
	if(!gameconfs->LoadGameConfigFile("CSSFixes.games", &g_pGameConf, conf_error, sizeof(conf_error)))
	{
		if(conf_error[0])
			snprintf(error, maxlength, "Could not read CSSFixes.games.txt: %s", conf_error);

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

	g_pDetour_CreateEntityByName = DETOUR_CREATE_STATIC(DETOUR_CreateEntityByName, "CreateEntityByName");
	if (g_pDetour_CreateEntityByName == NULL)
	{
		snprintf(error, maxlength, "Could not create detour for CreateEntityByName");
		SDK_OnUnload();
		return false;
	}

	g_pDetour_PassesFilterImpl = DETOUR_CREATE_MEMBER(DETOUR_PassesFilterImpl, "CBaseFilter_PassesFilterImpl");
	if (g_pDetour_PassesFilterImpl == NULL)
	{
		snprintf(error, maxlength, "Could not create detour for CBaseFilter_PassesFilterImpl");
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
	g_pDetour_CreateEntityByName->EnableDetour();
	g_pDetour_PassesFilterImpl->EnableDetour();
	g_pDetour_FindUseEntity->EnableDetour();
	g_pDetour_CTraceFilterSimple->EnableDetour();
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

	bool bSuccess = true;

	gs_Patches = {
		// 0: game_ui should not apply FL_ONTRAIN flag, else client prediction turns off
		{
			"_ZN7CGameUI5ThinkEv",
			(unsigned char *)"\x0F\x82\xC4\x03\x00\x00\x83\xEC\x08\x6A\x10\x53\xE8\x91\x00\xF5\xFF",
			"xx????xx?x?xx????",
			(unsigned char *)"\x0F\x82\xC4\x03\x00\x00\x83\xEC\x08\x6A\x10\x53\x90\x90\x90\x90\x90",
			"cstrike/bin/server_srv.so"
		},
		// 1: player_speedmod should not turn off flashlight
		{
			"_ZN17CMovementSpeedMod13InputSpeedModER11inputdata_t",
			(unsigned char *)"\x0F\x85\x00\x00\x00\x00\x83\xEC\x0C\x57\xE8\x1D\xFF\xFF\xFF\x83\xC4\x10\x09\x83",
			"xx????xx?xx????xx?xx",
			(unsigned char *)"\x90\x90\x90\x90\x90\x90\x83\xEC\x0C\x57\xE8\x1D\xFF\xFF\xFF\x83\xC4\x10\x09\x83",
			"cstrike/bin/server_srv.so"
		},
		// 5: disable alive check in point_viewcontrol->Disable
		{
			"_ZN14CTriggerCamera7DisableEv",
			(unsigned char *)"\x0F\x84\x47\x02\x00\x00\xF6\x83\x40\x01\x00\x00\x20\x0F\x85",
			"xx????xx?????xx",
			(unsigned char *)"\x90\x90\x90\x90\x90\x90\xF6\x83\x40\x01\x00\x00\x20\x0F\x85",
			"cstrike/bin/server_srv.so"
		},
		// 6: disable player->m_takedamage = DAMAGE_NO in point_viewcontrol->Enable
		{
			"_ZN14CTriggerCamera6EnableEv",
			(unsigned char *)"\xC6\x80\xFD\x00\x00\x00\x00\x8B\x83",
			"xxxxxxxxx",
			(unsigned char *)"\x90\x90\x90\x90\x90\x90\x90\x8B\x83",
			"cstrike/bin/server_srv.so",
			0x600
		},
		// 7: disable player->m_takedamage = m_nOldTakeDamage in point_viewcontrol->Disable
		{
			"_ZN14CTriggerCamera7DisableEv",
			(unsigned char *)"\x74\x1A\x8B\x16\x8B\x92\x04\x02\x00\x00\x81\xFA\x30\xF9\x29\x00\x0F\x85",
			"x?xxxx????xx????xx",
			(unsigned char *)"\xEB\x1A\x8B\x16\x8B\x92\x04\x02\x00\x00\x81\xFA\x30\xF9\x29\x00\x0F\x85",
			"cstrike/bin/server_srv.so"
		},
		// 8: userinfo stringtable don't write fakeclient field
		{
			"_ZN11CBaseClient12FillUserInfoER13player_info_s",
			(unsigned char *)"\x88\x46\x6C",
			"xxx",
			(unsigned char *)"\x90\x90\x90",
			"bin/engine_srv.so"
		},
		// 10: fix server lagging resulting from too many ConMsgs due to packet spam
		{
			"_ZN8CNetChan19ProcessPacketHeaderEP11netpacket_s",
			(unsigned char *)"_Z6ConMsgPKcz",
			"xxxxx",
			(unsigned char *)"\x90\x90\x90\x90\x90",
			"bin/engine_srv.so",
			0x7d1, 100,
			true, "bin/libtier0_srv.so"
		},
		// 11: fix server lagging resulting from too many ConMsgs due to packet spam
		{
			"_Z11NET_GetLongiP11netpacket_s",
			(unsigned char *)"Msg",
			"xxxxx",
			(unsigned char *)"\x90\x90\x90\x90\x90",
			"bin/engine_srv.so",
			0x800, 100,
			true, "bin/libtier0_srv.so"
		},
		// 13: CTriggerCamera::FollowTarget: Don't early return when the player handle is null
		{
			"_ZN14CTriggerCamera12FollowTargetEv",
			(unsigned char *)"\x0F\x84\xD6\x02\x00\x00\x83\xFA\xFF",
			"xxxxxxxxx",
			(unsigned char *)"\x90\x90\x90\x90\x90\x90\x83\xFA\xFF",
			"cstrike/bin/server_srv.so"
		},
		// 14: CGameMovement::LadderMove NOP out player->SetGravity( 0 );
		// This is in a cloned function which has a weird symbol (_ZN13CGameMovement10LadderMoveEv_part_0) so I went with the function right before it
		{
			"_ZN13CGameMovement12CheckFallingEv",
			(unsigned char *)"\xC7\x80\xA4\x02\x00\x00\x00\x00\x00\x00\x8B\x03\x8B\x80",
			"xx????????xxxx",
			(unsigned char *)"\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x8B\x03\x8B\x80",
			"cstrike/bin/server_srv.so"
		},
		// 18: Remove weird filename handle check in CZipPackFile::GetFileInfo that broke loading mixed case files in bsp pakfiles
		{
			"_ZN12CZipPackFile11GetFileInfoEPKcRiRxS2_S2_Rt",
			(unsigned char *)"\x75\x00\x8B\x09",
			"x?xx",
			(unsigned char *)"\x90\x90\x8B\x09",
			"bin/dedicated_srv.so"
		}
	};

	if (g_SvForceCTSpawn->GetInt())
	{
		gs_Patches.push_back({
			// 2: only select CT spawnpoints
			"_ZN9CCSPlayer19EntSelectSpawnPointEv",
			(unsigned char *)"\x74\x57\x83\xEC\x0C\x53\xE8\x6E\x34\xCA\xFF\x83\xC4\x10\x83\xF8\x02\x0F\x84",
			"x?xx?xx????xx?xx?xx",
			(unsigned char *)"\xEB\x57\x83\xEC\x0C\x53\xE8\x6E\x34\xCA\xFF\x83\xC4\x10\x83\xF8\x02\x0F\x84",
			"cstrike/bin/server_srv.so"
		});
		gs_Patches.push_back({
			// 3: don't check if we have T spawns
			"_ZN12CCSGameRules18NeededPlayersCheckERb",
			(unsigned char *)"\x74\x0A\x8B\x83\x94\x02\x00\x00\x85\xC0\x75\x4A\x83\xEC\x0C\x68\xE8\xCF\x93\x00\xE8\xA9\x46\x52\x00\x5A\x59",
			"xxxx????xxx?xx?x????x????xx",
			(unsigned char *)"\x75\x54\x8B\x83\x94\x02\x00\x00\x85\xC0\x75\x4A\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90",
			"cstrike/bin/server_srv.so"
		});

		if (g_SvLogs->GetInt())
		{
			g_pSM->LogMessage(myself, "Forcing CT spawn");
		}
	}

	if (g_SvSkipCashReset->GetInt())
	{
		gs_Patches.push_back({
			// 9: dont reset cash to 16000 when buying an item
			"_ZN9CCSPlayer10AddAccountEibbPKc",
			(unsigned char *)"\x3D\x80\x3E\x00\x00\x0F\x8F\x00\x00\x00\x00\x8D\x65",
			"x????xx????xx",
			(unsigned char *)"\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x8D\x65",
			"cstrike/bin/server_srv.so"
		});
	}

	if (g_SvGameEndUnFreeze->GetInt())
	{
		gs_Patches.push_back({
			// 16: allow people to run around freely after game end, by overwriting pPlayer->AddFlag( FL_FROZEN ); line 3337 in cs_gamerules.cpp
			// this change is desired for the new mapvoting feature so that people can still freely move at the end of the map while the vote is running.
			"_ZN12CCSGameRules16GoToIntermissionEv",
			(unsigned char *)"\x74\x0E\x83\xEC\x08\x6A\x40\x50",
			"xxxxxxxx",
			(unsigned char *)"\xEB\x0E\x83\xEC\x08\x6A\x40\x50",
			"cstrike/bin/server_srv.so"
		});
		gs_Patches.push_back({
			//17 also jump over boolean = true // freeze players while in intermission		m_bFreezePeriod = true;
			"_ZN12CCSGameRules16GoToIntermissionEv",
			(unsigned char *)"\x75\x0F\xE8\x69\xCE\xDA\xFF\x8B\x45\x08",
			"xxxxxxxxxx",
			(unsigned char *)"\xEB\x0F\xE8\x69\xCE\xDA\xFF\x8B\x45\x08",
			"cstrike/bin/server_srv.so"
		});
	}

	if (g_SvAlwaysTransmitPointViewControl->GetInt())
	{
		gs_Patches.push_back({
			// 12: Always transmit point_viewcontrol (for debugging)
			"_ZN14CTriggerCamera19UpdateTransmitStateEv",
			(unsigned char *)"\x74\x16",
			"xx",
			(unsigned char *)"\xEB\x16",
			"cstrike/bin/server_srv.so"
		});
	}

	// Apply all patches
	for(size_t i = 0; i < gs_Patches.size(); i++)
	{
		struct SrcdsPatch *pPatch = &gs_Patches[i];
		int PatchLen = strlen(pPatch->pPatchPattern);

#ifdef _WIN32
		HMODULE pBinary = LoadLibrary(pPatch->pLibrary);
#else
		void *pBinary = dlopen(pPatch->pLibrary, RTLD_NOW);
#endif
		if(!pBinary)
		{
			g_pSM->LogError(myself, "Could not dlopen %s", pPatch->pLibrary);
			bSuccess = false;
			continue;
		}

		pPatch->pAddress = (uintptr_t)memutils->ResolveSymbol(pBinary, pPatch->pSignature);
#ifdef _WIN32
		FreeLibrary(pBinary);
#else
		dlclose(pBinary);
#endif
		if(!pPatch->pAddress)
		{
			g_pSM->LogError(myself, "Could not find symbol: %s in %s (%p)",
				pPatch->pSignature, pPatch->pLibrary, pBinary);
			bSuccess = false;
			continue;
		}

		SrcdsPatch::Restore **ppRestore = &pPatch->pRestore;

		if(pPatch->functionCall)
		{
#ifdef _WIN32
			HMODULE pFunctionBinary = LoadLibrary(pPatch->pFunctionLibrary);
#else
			void* pFunctionBinary = dlopen(pPatch->pFunctionLibrary, RTLD_NOW);
#endif
			if(!pFunctionBinary)
			{
				g_pSM->LogError(myself, "Could not dlopen %s", pPatch->pFunctionLibrary);
				bSuccess = false;
				continue;
			}

			pPatch->pSignatureAddress = (uintptr_t)memutils->ResolveSymbol(pFunctionBinary, (char *)pPatch->pPatchSignature);
#ifdef _WIN32
			FreeLibrary(pFunctionBinary);
#else
			dlclose(pFunctionBinary);
#endif
			if(!pPatch->pSignatureAddress)
			{
				g_pSM->LogError(myself, "Could not find patch signature symbol: %s in %s (%p)",
					(char *)pPatch->pPatchSignature, pPatch->pFunctionLibrary, pFunctionBinary);
				bSuccess = false;
				continue;
			}
		}

		uintptr_t ofs = 0;
		int found;
		for(found = 0; found < pPatch->occurrences; found++)
		{
			uintptr_t pPatchAddress;
			if(pPatch->functionCall)
				pPatchAddress = FindFunctionCall(pPatch->pAddress + ofs, pPatch->pSignatureAddress, pPatch->range - ofs);
			else
				pPatchAddress = FindPattern(pPatch->pAddress + ofs, pPatch->pPatchSignature, pPatch->pPatchPattern, pPatch->range - ofs);

			if(!pPatchAddress)
			{
				if(found)
					break;

				g_pSM->LogError(myself, "Could not find patch signature for symbol: %s", pPatch->pSignature);
				bSuccess = false;
				continue;
			}
			ofs = pPatchAddress - pPatch->pAddress + PatchLen;

			// Create restore object
			*ppRestore = (SrcdsPatch::Restore *)new SrcdsPatch::Restore();
			SrcdsPatch::Restore *pRestore = *ppRestore;
			pRestore->pPatchAddress = pPatchAddress;
			pRestore->pOriginal = (unsigned char *)malloc(PatchLen * sizeof(unsigned char));

			SourceHook::SetMemAccess((void *)pPatchAddress, PatchLen, SH_MEM_READ|SH_MEM_WRITE|SH_MEM_EXEC);
			for(int j = 0; j < PatchLen; j++)
			{
				pRestore->pOriginal[j] = *(unsigned char *)(pPatchAddress + j);
				*(unsigned char *)(pPatchAddress + j) = pPatch->pPatch[j];
			}
			SourceHook::SetMemAccess((void *)pPatchAddress, PatchLen, SH_MEM_READ|SH_MEM_EXEC);

			ppRestore = &((*ppRestore)->pNext);
		}
	}

	if (!bSuccess)
	{
		SDK_OnUnload();
		return false;
	}

	return true;
}

const sp_nativeinfo_t MyNatives[] =
{
	{ "PhysboxToClientMap", PhysboxToClientMap },
	{ NULL, NULL }
};

void CSSFixes::SDK_OnAllLoaded()
{
	sharesys->AddNatives(myself, MyNatives);
	sharesys->RegisterLibrary(myself, "CSSFixes");
}

bool CSSFixes::RegisterConCommandBase(ConCommandBase *pVar)
{
	/* Always call META_REGCVAR instead of going through the engine. */
	return META_REGCVAR(pVar);
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

	if (g_pDetour_CreateEntityByName != NULL)
	{
		g_pDetour_CreateEntityByName->Destroy();
		g_pDetour_CreateEntityByName = NULL;
	}

	if (g_pDetour_PassesFilterImpl != NULL)
	{
		g_pDetour_PassesFilterImpl->Destroy();
		g_pDetour_PassesFilterImpl = NULL;
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

	if(g_SH_SkipTwoEntitiesShouldHitEntity)
		SH_REMOVE_HOOK_ID(g_SH_SkipTwoEntitiesShouldHitEntity);

	if(g_SH_SimpleShouldHitEntity)
		SH_REMOVE_HOOK_ID(g_SH_SimpleShouldHitEntity);

	gameconfs->CloseGameConfigFile(g_pGameConf);

	// Revert all applied patches
	for(size_t i = 0; i < gs_Patches.size(); i++)
	{
		struct SrcdsPatch *pPatch = &gs_Patches[i];
		int PatchLen = strlen(pPatch->pPatchPattern);

		SrcdsPatch::Restore *pRestore = pPatch->pRestore;
		while(pRestore)
		{
			if(!pRestore->pOriginal)
				break;

			SourceHook::SetMemAccess((void *)pRestore->pPatchAddress, PatchLen, SH_MEM_READ|SH_MEM_WRITE|SH_MEM_EXEC);
			for(int j = 0; j < PatchLen; j++)
			{
				*(unsigned char *)(pRestore->pPatchAddress + j) = pRestore->pOriginal[j];
			}
			SourceHook::SetMemAccess((void *)pRestore->pPatchAddress, PatchLen, SH_MEM_READ|SH_MEM_EXEC);

			free(pRestore->pOriginal);
			pRestore->pOriginal = NULL;

			void *freeMe = pRestore;
			pRestore = pRestore->pNext;
			free(freeMe);
		}
	}
}

bool CSSFixes::SDK_OnMetamodLoad(ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	ConVar_Register(0, this);
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
