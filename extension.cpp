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
#include <sh_memory.h>

static struct SrcdsPatch
{
	const char *pSignature;
	const unsigned char *pPatchSignature;
	const char *pPatchPattern;
	const unsigned char *pPatch;

	unsigned char *pOriginal;
	uintptr_t pAddress;
	uintptr_t pPatchAddress;
} gs_Patches[] = {
	{
		"_ZN7CGameUI5ThinkEv",
		(unsigned char *)"\xC7\x44\x24\x04\x10\x00\x00\x00\x89\x34\x24\xE8\x00\x00\x00\x00",
		"xxxxxxxxxxxx????",
		(unsigned char *)"\xC7\x44\x24\x04\x10\x00\x00\x00\x89\x34\x24\x90\x90\x90\x90\x90",
		0, 0, 0
	},
	{
		"_ZN17CMovementSpeedMod13InputSpeedModER11inputdata_t",
		(unsigned char *)"\xFF\x90\x8C\x05\x00\x00\x85\xC0\x0F\x85\x75\x02\x00\x00",
		"xxxxxxxxxxxxxx",
		(unsigned char *)"\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90",
		0, 0, 0
	},
	{
		"_ZN9CCSPlayer19EntSelectSpawnPointEv",
		(unsigned char *)"\x89\x1C\x24\xE8\x00\x00\x00\x00\x83\xF8\x03\x74\x4B",
		"xxxx????xxxxx",
		(unsigned char *)"\x89\x1C\x24\x90\x90\x90\x90\x90\x90\x90\x90\xEB\x4B",
		0, 0, 0
	},
	{
		"_ZN12CCSGameRules18NeededPlayersCheckERb",
		(unsigned char *)"\x74\x0E\x8B\x83\x80\x02\x00\x00\x85\xC0\x0F\x85\x9E\x00\x00\x00\xC7\x04\x24\xAC\xF7\x87\x00\xE8\xC2\x82\x91\x00",
		"xxxxxxxxxxxxxxxx????????????",
		(unsigned char *)"\x0F\x85\xA8\x00\x00\x00\x8B\x83\x80\x02\x00\x00\x85\xC0\x0F\x85\x9A\x00\x00\x00\x90\x90\x90\x90\x90\x90\x90\x90",
		0, 0, 0
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

uintptr_t FindPattern(uintptr_t BaseAddr, const unsigned char *pData, const char *pPattern, size_t MaxSize);

/**
 * @file extension.cpp
 * @brief Implement extension code here.
 */

CSSFixes g_Interface;
SMEXT_LINK(&g_Interface);

IGameConfig *g_pGameConf = NULL;

IForward *g_pOnRunThinkFunctions = NULL;
IForward *g_pOnRunThinkFunctionsPost = NULL;

CDetour *detourInputTestActivator = NULL;
CDetour *detourPostConstructor = NULL;
CDetour *detourFindUseEntity = NULL;
CDetour *detourCTraceFilterSimple = NULL;
CDetour *detourMultiWaitOver = NULL;
CDetour *detourRunThinkFunctions = NULL;
CDetour *detourKeyValue = NULL;

DETOUR_DECL_MEMBER1(InputTestActivator, void, inputdata_t *, inputdata)
{
	if(!inputdata || !inputdata->pActivator || !inputdata->pCaller)
		return;

	DETOUR_MEMBER_CALL(InputTestActivator)(inputdata);
}

char *g_pInfoPlayerCounterterroristStr = NULL;
DETOUR_DECL_MEMBER1(PostConstructor, void, const char *, szClassname)
{
	if(strncasecmp(szClassname, "info_player_", 12) == 0)
	{
		CBaseEntity *pEntity = (CBaseEntity *)this;

		datamap_t *pMap = gamehelpers->GetDataMap(pEntity);
		typedescription_t *td = gamehelpers->FindInDataMap(pMap, "m_iEFlags");

		*(uint32 *)((intptr_t)pEntity + td->fieldOffset[TD_OFFSET_NORMAL]) |= (1<<9); // EFL_SERVER_ONLY

		if(strcasecmp(szClassname, "info_player_terrorist") == 0)
			szClassname = g_pInfoPlayerCounterterroristStr;
	}

	DETOUR_MEMBER_CALL(PostConstructor)(szClassname);
}

DETOUR_DECL_MEMBER2(KeyValue, bool, const char *, szKeyName, const char *, szValue)
{
	if(strcasecmp(szKeyName, "angle") == 0)
		szKeyName = "angles";

	else if(strcasecmp(szKeyName, "classname") == 0 &&
		strcasecmp(szValue, "info_player_terrorist") == 0)
	{
		szValue = g_pInfoPlayerCounterterroristStr;
	}

	return DETOUR_MEMBER_CALL(KeyValue)(szKeyName, szValue);
}

volatile bool gv_InFindUseEntity = false;
DETOUR_DECL_MEMBER0(FindUseEntity, CBaseEntity *)
{
	// Signal CTraceFilterSimple that we are in FindUseEntity
	gv_InFindUseEntity = true;
	CBaseEntity *pEntity = DETOUR_MEMBER_CALL(FindUseEntity)();
	gv_InFindUseEntity = false;
	return pEntity;
}

uintptr_t g_CTraceFilterNoNPCsOrPlayer = 0;
typedef bool (*ShouldHitFunc_t)( IHandleEntity *pHandleEntity, int contentsMask );
DETOUR_DECL_MEMBER3(CTraceFilterSimple, void, const IHandleEntity *, passedict, int, collisionGroup, ShouldHitFunc_t, pExtraShouldHitFunc)
{
	DETOUR_MEMBER_CALL(CTraceFilterSimple)(passedict, collisionGroup, pExtraShouldHitFunc);

	// If we're in FindUseEntity right now then switch out the VTable
	if(gv_InFindUseEntity)
		*(uintptr_t *)this = g_CTraceFilterNoNPCsOrPlayer;
}

DETOUR_DECL_MEMBER0(MultiWaitOver, void)
{
	CBaseEntity *pEntity = (CBaseEntity *)this;
	edict_t *pEdict = gamehelpers->EdictOfIndex(gamehelpers->EntityToBCompatRef(pEntity));
	if(pEdict)
		engine->TriggerMoved(pEdict, true);

	DETOUR_MEMBER_CALL(MultiWaitOver)();
}

DETOUR_DECL_STATIC1(RunThinkFunctions, void, bool, simulating)
{
	g_pOnRunThinkFunctions->PushCell(simulating);
	g_pOnRunThinkFunctions->Execute();

	DETOUR_STATIC_CALL(RunThinkFunctions)(simulating);

	g_pOnRunThinkFunctionsPost->PushCell(simulating);
	g_pOnRunThinkFunctionsPost->Execute();
}

bool CSSFixes::SDK_OnLoad(char *error, size_t maxlength, bool late)
{
	char conf_error[255] = "";
	if(!gameconfs->LoadGameConfigFile("CSSFixes", &g_pGameConf, conf_error, sizeof(conf_error)))
	{
		if(conf_error[0])
			snprintf(error, maxlength, "Could not read CSSFixes.txt: %s", conf_error);

		return false;
	}

	CDetourManager::Init(g_pSM->GetScriptingEngine(), g_pGameConf);

	detourInputTestActivator = DETOUR_CREATE_MEMBER(InputTestActivator, "CBaseFilter_InputTestActivator");
	if(detourInputTestActivator == NULL)
	{
		snprintf(error, maxlength, "Could not create detour for CBaseFilter_InputTestActivator");
		SDK_OnUnload();
		return false;
	}

	detourPostConstructor = DETOUR_CREATE_MEMBER(PostConstructor, "CBaseEntity_PostConstructor");
	if(detourPostConstructor == NULL)
	{
		snprintf(error, maxlength, "Could not create detour for CBaseEntity_PostConstructor");
		SDK_OnUnload();
		return false;
	}

	detourFindUseEntity = DETOUR_CREATE_MEMBER(FindUseEntity, "CBasePlayer_FindUseEntity");
	if(detourFindUseEntity == NULL)
	{
		snprintf(error, maxlength, "Could not create detour for CBasePlayer_FindUseEntity");
		SDK_OnUnload();
		return false;
	}

	detourCTraceFilterSimple = DETOUR_CREATE_MEMBER(CTraceFilterSimple, "CTraceFilterSimple_CTraceFilterSimple");
	if(detourCTraceFilterSimple == NULL)
	{
		snprintf(error, maxlength, "Could not create detour for CTraceFilterSimple_CTraceFilterSimple");
		SDK_OnUnload();
		return false;
	}

	detourMultiWaitOver = DETOUR_CREATE_MEMBER(MultiWaitOver, "CTriggerMultiple_MultiWaitOver");
	if(detourMultiWaitOver == NULL)
	{
		snprintf(error, maxlength, "Could not create detour for CTriggerMultiple_MultiWaitOver");
		SDK_OnUnload();
		return false;
	}

	detourRunThinkFunctions = DETOUR_CREATE_STATIC(RunThinkFunctions, "Physics_RunThinkFunctions");
	if(detourRunThinkFunctions == NULL)
	{
		snprintf(error, maxlength, "Could not create detour for Physics_RunThinkFunctions");
		SDK_OnUnload();
		return false;
	}

	detourKeyValue = DETOUR_CREATE_MEMBER(KeyValue, "CBaseEntity_KeyValue");
	if(detourKeyValue == NULL)
	{
		snprintf(error, maxlength, "Could not create detour for CBaseEntity_KeyValue");
		SDK_OnUnload();
		return false;
	}

	detourInputTestActivator->EnableDetour();
	detourPostConstructor->EnableDetour();
	detourFindUseEntity->EnableDetour();
	detourCTraceFilterSimple->EnableDetour();
	detourMultiWaitOver->EnableDetour();
	detourRunThinkFunctions->EnableDetour();
	detourKeyValue->EnableDetour();

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

	void *pServerSo = dlopen("cstrike/bin/server_srv.so", RTLD_NOW);
	if(!pServerSo)
	{
		snprintf(error, maxlength, "Could not dlopen server_srv.so");
		SDK_OnUnload();
		return false;
	}

	// Apply all patches
	for(size_t i = 0; i < sizeof(gs_Patches) / sizeof(*gs_Patches); i++)
	{
		struct SrcdsPatch *pPatch = &gs_Patches[i];
		int PatchLen = strlen(pPatch->pPatchPattern);

		pPatch->pAddress = (uintptr_t)memutils->ResolveSymbol(pServerSo, pPatch->pSignature);
		if(!pPatch->pAddress)
		{
			snprintf(error, maxlength, "Could not find symbol: %s", pPatch->pSignature);
			dlclose(pServerSo);
			SDK_OnUnload();
			return false;
		}

		pPatch->pPatchAddress = FindPattern(pPatch->pAddress, pPatch->pPatchSignature, pPatch->pPatchPattern, 1024);
		if(!pPatch->pPatchAddress)
		{
			snprintf(error, maxlength, "Could not find patch signature for symbol: %s", pPatch->pSignature);
			dlclose(pServerSo);
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

	g_pOnRunThinkFunctions = forwards->CreateForward("OnRunThinkFunctions", ET_Ignore, 1, NULL, Param_Cell);
	g_pOnRunThinkFunctionsPost = forwards->CreateForward("OnRunThinkFunctionsPost", ET_Ignore, 1, NULL, Param_Cell);

	const char aStr[] = "info_player_counterterrorist";
	g_pInfoPlayerCounterterroristStr = (char *)malloc(sizeof(aStr));
	memcpy(g_pInfoPlayerCounterterroristStr, aStr, sizeof(aStr));

	return true;
}

void CSSFixes::SDK_OnUnload()
{
	if(detourInputTestActivator != NULL)
	{
		detourInputTestActivator->Destroy();
		detourInputTestActivator = NULL;
	}

	if(detourPostConstructor != NULL)
	{
		detourPostConstructor->Destroy();
		detourPostConstructor = NULL;
	}

	if(detourFindUseEntity != NULL)
	{
		detourFindUseEntity->Destroy();
		detourFindUseEntity = NULL;
	}

	if(detourCTraceFilterSimple != NULL)
	{
		detourCTraceFilterSimple->Destroy();
		detourCTraceFilterSimple = NULL;
	}

	if(detourMultiWaitOver != NULL)
	{
		detourMultiWaitOver->Destroy();
		detourMultiWaitOver = NULL;
	}

	if(detourRunThinkFunctions != NULL)
	{
		detourRunThinkFunctions->Destroy();
		detourRunThinkFunctions = NULL;
	}

	if(detourKeyValue != NULL)
	{
		detourKeyValue->Destroy();
		detourKeyValue = NULL;
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
