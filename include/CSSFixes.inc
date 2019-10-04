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
// 0 = Block ALL SV_TriggerMoved.
// >0 = Entity index for which to allow SV_TriggerMoved to be called.
// REMEMBER TO CALL THIS AGAIN WITH -1 AFTER USING IT !!!
native void BlockSolidMoved(int entity);

// entities is an array which maps from entindex to a truth value
// if the value is true then SolidMoved will not see this entity
// REMEMBER TO CALL THIS WITH SIZE 0 0N PLUGIN END !!!
native void FilterSolidMoved(int[] entities, int size);

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
