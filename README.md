# Followers With Brains

An all-in-one follower behaviour mod for Skyrim Special Edition (SKSE plugin + MCM).
Work in progress.

## Planned features

**Keeping up**
- Separate follow distance (out of combat) and combat distance: *Close*, *Medium* or *Far*.
  - *Close*: followers stay within interaction range and start moving the moment you do.
  - *Medium*: a looser follow.
  - *Far*: the game's own follow behaviour.
- Placement for Close following: behind you, at your side, or ahead of you (off to the side, out
  of your line of sight).
- The exact spot next to you is kept only when the follower is close with a clear line to it;
  otherwise the game's own pathfinding brings them back (round fences and rocks, facing forward).
- A follower stuck below a rock you are on, above a drop you went down, or at a fence jumps:
  up, down or over, with the game's own jump animations.
- How close they keep depends on the moment: tightest with weapons drawn, looser with weapons
  sheathed, loosest in cities, towns, inns and your homes. Turning on the spot, strafing or
  backpedalling doesn't send them running round you.
- In safe places, when you stay put for a while or are busy talking or crafting, followers relax:
  they wander off to sit, lean and use furniture, and come back when you move on.
- Followers that fall behind get a short speed boost. If they are still far away and out of
  sight, they are moved behind you.

**Combat awareness**
- Followers notice what you notice: aim at, or watch, a hostile for a moment and your
  followers get ready to fight it.
- When you attack, they attack at the same moment.
- When their target dies, they switch to the next enemy fighting you without standing idle.

**Retreating together**
- If you disengage from a fight, followers disengage with you.
- Ranged followers keep shooting while backing off as long as they are not too far from you,
  then just run.
- Melee followers turn and run with you instead of backpedalling.
- Backing off to heal does not count as a retreat. A follower you just healed may fight one
  distance step further from you for a few seconds.

**Fighting style**
- Each follower fights with a private, tuned copy of their combat style, so the game's own combat
  AI does the moves with its normal in-combat movement: dodging whatever can be dodged, strafing
  instead of running straight at archers, circling and stepping back in melee, hitting staggered
  enemies and bashing power attacks (when they can block). No stats, skills or perks change, and
  the original style comes back when they leave.
- On top of that, the moment an enemy starts an attack on them, they weigh how hard it will hit
  and step out of reach of the big ones, then strike back.

**Weapons**
- Followers who carry both ranged and melee weapons compare their damage per second, first as the
  game calculates a hit (perks, buffs, tempering, enchantments, attack and draw speed), then from
  the damage they actually deal with each during the session: fighters
  charge, and shoot only at enemies they can't reach or that are too far from you to chase;
  archers and mages keep shooting and switch to melee only when an enemy is in their face.
- If the game's own combat AI keeps undoing the choice, the follower is left alone for a while
  instead of swapping back and forth.
- Out of combat, followers draw their ranged weapon when you draw yours.

**Smarter targeting**
- Followers go for a summoner instead of the summon when they can reach them, unless the
  summon keeps hitting them.
- A melee follower who misses twice while another enemy hits them switches to that enemy.
- A melee follower stuck short of its target tries another way, or fights whoever is hitting it.

## Roadmap

The goal: a follower that plays like a person, not like a script, with a player's reflexes.
Followers read the enemy's intent straight from the game (an attack starting, a bow being
drawn) instead of waiting to see an animation, and react at once.

**Positioning**
- Bait archers: peek out to draw a shot from an archer without line of sight, then sprint in while
  it reloads (not when melee enemies are close).
- Hold a choke point: against several melee enemies, back into a doorway or narrow path so only
  one or two can reach them.
- Dragons: without a ranged weapon, stay out of the dragon's sight while it flies and engage when
  it lands.
- Take cover from archers and casters they can't reach.
- Flank: melee followers attack your target from the other side instead of standing behind you.
- Step out of area damage (fire, frost and poison clouds, runes).
- Get out of your way in narrow corridors and doorways.
- Ranged followers keep their distance from melee enemies and don't shoot through you.
- Kiting: archers and mages use the terrain like a player does (get up on rocks, circle tables
  and pillars, hop a fence, hit and step back out of reach).
- Zig-zag when closing in on an archer or caster, so their shots miss.

**Teamwork**
- Focus fire: when you keep hitting the same enemy, followers help finish it.
- Protect you: when you are low on health, melee followers go for whoever is hitting you.
- Interrupt casters with a shield bash or a quick attack.

**Reflexes**
- The moment an enemy starts an attack, weigh it: how hard it will hit and whether it can be
  interrupted. Then take it, bash it, block it, or step out of reach and punish.
- Bob and weave: bait a melee swing, step out of reach, step back in and land a hit (optional).
- Follow up instantly on openings, like a staggered or bashed enemy.
- Regroup: a badly hurt follower falls back to you instead of fighting alone.
- Stealth: while you sneak, followers sneak, keep further back and wait for your first shot.

**Feeling alive**
- Look where you look while your weapon is drawn: right away when you sneak, more slowly when
  you don't.
- Keep watch facing outward while you loot or craft in dangerous places; relax in towns.

**Upkeep**
- Use their own potions at sensible health thresholds.
- Optional callouts using the follower's own voice lines ("enemy ahead").

## Building
`tools/build.sh` builds the SKSE plugin (CommonLibSSE-NG, clang-cl + xwin) and the Papyrus
scripts (Caprica). `tools/make_esp.py` writes the plugin file that hosts the MCM.

Test mode: put `Data/SKSE/Plugins/FollowersWithBrains_Test.ini` next to the plugin
(`Save=`, `Scenario=mixed|melee_rush|archers`, `Runs=`, `Duration=`). From the main menu the plugin
loads that save, spawns the scenario around the player, traces the first follower, writes
`FollowersWithBrains_Test/report.txt` and `trace.csv`, repeats, then quits the game.

## Requirements
- SKSE64 and Address Library for SKSE Plugins
- SkyUI (for the MCM)
- Recommended: [NPC Traversal NG](https://www.nexusmods.com/skyrimspecialedition/mods/196059),
  so followers can jump and climb after you.

## License
MIT
