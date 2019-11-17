#if defined _cssfixes_included
 #endinput
#endif
#define _cssfixes_included

forward void OnRunThinkFunctions(bool simulating);
forward void OnPrePlayerThinkFunctions();
forward void OnPostPlayerThinkFunctions();
forward void OnRunThinkFunctionsPost(bool simulating);

// Block TriggerMoved from being called at all for an entity by setting the bit to 1.
native void BlockTriggerMoved(int map[2048 / 32], bool set);

// Block SolidMoved from being called at all for an entity by setting the bit to 1.
native void BlockSolidMoved(int map[2048 / 32], bool set);

// Block clients SolidMoved from touching an entity by setting the bit to 1 in the clients map.
native void FilterClientEntityMap(int map[((MAXPLAYERS + 1) * 2048) / 32], bool set);

// Block triggers TriggerMoved from touching any client by setting the bit to 1 for the entity index.
native void FilterTriggerTouchPlayers(int map[2048 / 32], bool set);

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
