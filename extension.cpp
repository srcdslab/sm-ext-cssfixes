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
#include <utlvector.h>
#include <string_t.h>

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

static struct SrcdsPatch
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
} gs_Patches[] = {
	// 0: game_ui should not apply FL_ONTRAIN flag, else client prediction turns off
	{
		"_ZN7CGameUI5ThinkEv",
		(unsigned char *)"\xC7\x44\x24\x04\x10\x00\x00\x00\x89\x34\x24\xE8\x00\x00\x00\x00",
		"xxxxxxxxxxxx????",
		(unsigned char *)"\xC7\x44\x24\x04\x10\x00\x00\x00\x89\x34\x24\x90\x90\x90\x90\x90",
		"cstrike/bin/server_srv.so"
	},
	// 1: player_speedmod should not turn off flashlight
	{
		"_ZN17CMovementSpeedMod13InputSpeedModER11inputdata_t",
		(unsigned char *)"\xFF\x90\x8C\x05\x00\x00\x85\xC0\x0F\x85\x85\x02\x00\x00",
		"xxxxxxxxxxxxxx",
		(unsigned char *)"\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90",
		"cstrike/bin/server_srv.so"
	},
	// 2: only select CT spawnpoints
	{
		"_ZN9CCSPlayer19EntSelectSpawnPointEv",
		(unsigned char *)"\x89\x1C\x24\xE8\x00\x00\x00\x00\x83\xF8\x03\x74\x6B",
		"xxxx????xxxxx",
		(unsigned char *)"\x89\x1C\x24\x90\x90\x90\x90\x90\x90\x90\x90\xEB\x6B",
		"cstrike/bin/server_srv.so"
	},
	// 3: don't check if we have T spawns
	{
		"_ZN12CCSGameRules18NeededPlayersCheckERb",
		(unsigned char *)"\x74\x0E\x8B\x83\x80\x02\x00\x00\x85\xC0\x0F\x85\x9E\x00\x00\x00\xC7\x04\x24\xAC\xF7\x87\x00\xE8\xC2\x82\x91\x00",
		"xxxxxxxxxxxxxxxx????????????",
		(unsigned char *)"\x0F\x85\xA8\x00\x00\x00\x8B\x83\x80\x02\x00\x00\x85\xC0\x0F\x85\x9A\x00\x00\x00\x90\x90\x90\x90\x90\x90\x90\x90",
		"cstrike/bin/server_srv.so"
	},
	// 5: disable alive check in point_viewcontrol->Disable
	{
		"_ZN14CTriggerCamera7DisableEv",
		(unsigned char *)"\x8B\x10\x89\x04\x24\xFF\x92\x08\x01\x00\x00\x84\xC0\x0F\x84\x58\xFF\xFF\xFF",
		"xxxxxxx??xxxxxx?xxx",
		(unsigned char *)"\x8B\x10\x89\x04\x24\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90",
		"cstrike/bin/server_srv.so"
	},
	// 6: disable player->m_takedamage = DAMAGE_NO in point_viewcontrol->Enable
	{
		"_ZN14CTriggerCamera6EnableEv",
		(unsigned char *)"\xC6\x86\x00\x00\x00\x00\x00\x8B\x83\x00\x00\x00\x00\xA8\x20",
		"xx?????xx????xx",
		(unsigned char *)"\x90\x90\x90\x90\x90\x90\x90\x8B\x83\x00\x00\x00\x00\xA8\x20",
		"cstrike/bin/server_srv.so",
		0x600
	},
	// 7: disable player->m_takedamage = m_nOldTakeDamage in point_viewcontrol->Disable
	{
		"_ZN14CTriggerCamera7DisableEv",
		(unsigned char *)"\x89\xF9\x38\x8E\xFD\x00\x00\x00\x0F\x84\xCA\xFD\xFF\xFF",
		"xxxx?xxxxxxxxx",
		(unsigned char *)"\x89\xF9\x38\x8E\xFD\x00\x00\x00\x90\xE9\xCA\xFD\xFF\xFF",
		"cstrike/bin/server_srv.so"
	},
	// 8: userinfo stringtable don't write fakeclient field
	{
		"_ZN11CBaseClient12FillUserInfoER13player_info_s",
		(unsigned char *)"\xFF\x50\x70\x88\x46\x6C",
		"xxxxxx",
		(unsigned char *)"\x90\x90\x90\x90\x90\x90",
		"bin/engine_srv.so"
	},
	// 9: fix server lagging resulting from too many ConMsgs due to packet spam
	{
		"_ZN8CNetChan19ProcessPacketHeaderEP11netpacket_s",
		(unsigned char *)"_Z6ConMsgPKcz",
		"xxxxx",
		(unsigned char *)"\x90\x90\x90\x90\x90",
		"bin/engine_srv.so",
		0x7d1, 100,
		true, "bin/libtier0_srv.so"
	},
	// 10: fix server lagging resulting from too many ConMsgs due to packet spam
	{
		"_Z11NET_GetLongiP11netpacket_s",
		(unsigned char *)"Msg",
		"xxxxx",
		(unsigned char *)"\x90\x90\x90\x90\x90",
		"bin/engine_srv.so",
		0x800, 100,
		true, "bin/libtier0_srv.so"
	},
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
	"ambient_generic",
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
	CBaseEntity *pEntity = (CBaseEntity *)this;

	static datamap_t *pMap = gamehelpers->GetDataMap(pEntity);
	static typedescription_t *td = gamehelpers->FindInDataMap(pMap, "m_iEFlags");
	static uint32 offset = td->fieldOffset[TD_OFFSET_NORMAL];

	if(strncasecmp(szClassname, "info_player_", 12) == 0)
	{
		// Spawnpoints don't need edicts...
		*(uint32 *)((intptr_t)pEntity + offset) |= (1<<9); // EFL_SERVER_ONLY

		// Only CT spawnpoints
		if(strcasecmp(szClassname, "info_player_terrorist") == 0)
			szClassname = "info_player_counterterrorist";

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
	// Nice of valve to expose CBaseFilter as filter_base :)
	if (strcasecmp(className, "filter_activator_context") == 0)
		className = "filter_base";

	return DETOUR_STATIC_CALL(DETOUR_CreateEntityByName)(className, iForceEdictIndex);
}

DETOUR_DECL_MEMBER2(DETOUR_KeyValue, bool, const char *, szKeyName, const char *, szValue)
{
    CBaseEntity *pEntity = (CBaseEntity *)this;

	// Fix crash bug in engine
	if(strcasecmp(szKeyName, "angle") == 0)
	{
		szKeyName = "angles";
	}
	else if(strcasecmp(szKeyName, "classname") == 0 &&
		strcasecmp(szValue, "info_player_terrorist") == 0)
	{
		// Only CT spawnpoints
		szValue = "info_player_counterterrorist";
	}
	else if(strcasecmp(szKeyName, "teamnum") == 0 || strcasecmp(szKeyName, "teamnum") == 0 )
	{
		const char *pClassname = gamehelpers->GetEntityClassname(pEntity);

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
	srand((unsigned int)time(NULL));

    g_iMaxPlayers = playerhelpers->GetMaxClients();

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

	// Apply all patches
	for(size_t i = 0; i < sizeof(gs_Patches) / sizeof(*gs_Patches); i++)
	{
		struct SrcdsPatch *pPatch = &gs_Patches[i];
		int PatchLen = strlen(pPatch->pPatchPattern);

		void *pBinary = dlopen(pPatch->pLibrary, RTLD_NOW);
		if(!pBinary)
		{
			snprintf(error, maxlength, "Could not dlopen %s", pPatch->pLibrary);
			SDK_OnUnload();
			return false;
		}

		pPatch->pAddress = (uintptr_t)memutils->ResolveSymbol(pBinary, pPatch->pSignature);
		dlclose(pBinary);
		if(!pPatch->pAddress)
		{
			snprintf(error, maxlength, "Could not find symbol: %s in %s (%p)",
				pPatch->pSignature, pPatch->pLibrary, pBinary);
			SDK_OnUnload();
			return false;
		}

		SrcdsPatch::Restore **ppRestore = &pPatch->pRestore;

		if(pPatch->functionCall)
		{
			void *pFunctionBinary = dlopen(pPatch->pFunctionLibrary, RTLD_NOW);
			if(!pFunctionBinary)
			{
				snprintf(error, maxlength, "Could not dlopen %s", pPatch->pFunctionLibrary);
				SDK_OnUnload();
				return false;
			}

			pPatch->pSignatureAddress = (uintptr_t)memutils->ResolveSymbol(pFunctionBinary, (char *)pPatch->pPatchSignature);
			dlclose(pFunctionBinary);
			if(!pPatch->pSignatureAddress)
			{
				snprintf(error, maxlength, "Could not find patch signature symbol: %s in %s (%p)",
					(char *)pPatch->pPatchSignature, pPatch->pFunctionLibrary, pFunctionBinary);
				SDK_OnUnload();
				return false;
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

				snprintf(error, maxlength, "Could not find patch signature for symbol: %s", pPatch->pSignature);
				SDK_OnUnload();
				return false;
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
	for(size_t i = 0; i < sizeof(gs_Patches) / sizeof(*gs_Patches); i++)
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
