# Counter-Strike: Source bug fixes
### Fixes crash in filter_activator_*->TestActivator() when activator isn't valid anymore.
### Fixes game_ui lag and player_speedmod turning off your flashlight.

# Patchnotes
### Thanks to the leaked Source Engine 2007 sourcecode.

#### File: se2007/game/server/game_ui.cpp
##### Line 292:

```C
void CGameUI::Think( void )
[...]
	pPlayer->AddFlag( FL_ONTRAIN );
[...]
```
Replaced **pPlayer->AddFlag( FL_ONTRAIN );** with **NOP** to fix prediction issues
while having game_ui active.

#### File: se2007/game/server/player.cpp
##### Line: 7587

```C
void CMovementSpeedMod::InputSpeedMod(inputdata_t &data)
[...]
			// Turn off the flashlight
			if ( pPlayer->FlashlightIsOn() )
			{
				pPlayer->FlashlightTurnOff();
			}
[...]
```

**NOP**'d out the block.

#### File: game/server/cstrike/bot/cs_bot_vision.cpp

```C
CCSPlayer *CCSBot::FindMostDangerousThreat( void )
[...]
	enum { MAX_THREATS = 16 };
[...]
					for( int k=threatCount-1; k>=j; --k )
						threat[k+1] = threat[k];
[...]
					if (threatCount < MAX_THREATS)
						++threatCount;
[...]
```
With 16 threats stored, inserting a closer one writes **threat[16]**, one past the end of the stack array.
On x86_64 that overwrites the caller's saved `rbx`, and `CCSBot::UpdateReactionQueue` crashes once a bot sees 17 or more enemies.
The count is capped at **15** instead, so every write stays inside the array.

# Configuration
In cstrike/cfg/server.cfg add this
```
//============================================================================
// Late load support - since multiples things need to be init before load it
// The upcoming extensions can not use autoload file

sm exts load CSSFixes
```
