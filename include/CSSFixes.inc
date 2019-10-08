#if defined _cssfixes_included
 #endinput
#endif
#define _cssfixes_included

forward void OnRunThinkFunctions(bool simulating);
forward void OnPrePlayerThinkFunctions();
forward void OnPostPlayerThinkFunctions();
forward void OnRunThinkFunctionsPost(bool simulating);

// -1 = Ignore, normal operation.
// 0 = Block ALL SV_TriggerMoved.
// >0 = Entity index for which to allow EnumElement to be called.
// REMEMBER TO CALL THIS AGAIN WITH -1 AFTER USING IT !!!
native void FilterTriggerMoved(int entity);

// -1 = Ignore, normal operation.
// 0 = Block ALL SV_SolidMoved.
// >0 = Entity index for which to allow SV_SolidMoved to be called.
// REMEMBER TO CALL THIS AGAIN WITH -1 AFTER USING IT !!!
native void BlockSolidMoved(int entity);

// Block clients SolidMoved from touching an entity by setting it to 1 in the clients map.
native void FilterClientEntityMap(char map[MAXPLAYERS + 1][2048], bool set);

// Block triggers TriggerMoved from touching any client by setting it to 1 for the entity index.
native void FilterTriggerTouchPlayers(char map[2048], bool set);

// Map entities to client entity in FireBullets/SwingOrStab ShouldHitEntity.
// Aka. shoot and knife through physboxes that are parented to teammates (white knight, gandalf, horse, etc.)
native void PhysboxToClientMap(char map[2048], bool set);

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
