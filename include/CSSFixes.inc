#if defined _cssfixes_included
 #endinput
#endif
#define _cssfixes_included

forward void OnRunThinkFunctions(bool simulating);
forward void OnRunThinkFunctionsPost(bool simulating);
forward Action OnBroadcastSound(int entity, char sample[PLATFORM_MAX_PATH], int clients[MAXPLAYERS], int &numClients);

public Extension __ext_CSSFixes =
{
	name = "CSSFixes",
	file = "CSSFixes.ext",
#if defined AUTOLOAD_EXTENSIONS
	autoload = 1,
#else
	autoload = 0,
#endif
#if defined REQUIRE_EXTENSIONS
	required = 1,
#else
	required = 0,
#endif
};

#if !defined REQUIRE_EXTENSIONS
public __ext_CSSFixes_SetNTVOptional()
{
}
#endif
