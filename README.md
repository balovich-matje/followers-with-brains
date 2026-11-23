# Followers With Brains

An all-in-one follower behaviour mod for Skyrim Special Edition (SKSE plugin + MCM).
Work in progress.

## Planned features

**Keeping up**
- Follow distance setting: *Close*, *Medium* or *Far*.
  - *Close*: followers stay within interaction range and start moving the moment you do.
  - *Medium*: a looser follow.
  - *Far*: the game's own follow behaviour.
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
- Backing off to heal does not count as a retreat while you are healing your followers.

**Smarter targeting**
- Followers go for a summoner instead of the summon when they can reach them, unless the
  summon keeps hitting them.
- A melee follower who misses twice while another enemy hits them switches to that enemy.
- A melee follower stuck short of its target tries another way, or fights whoever is hitting it.

## Roadmap

The goal: a follower that plays like a person, not like a script.

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

**Teamwork**
- Focus fire: when you keep hitting the same enemy, followers help finish it.
- Protect you: when you are low on health, melee followers go for whoever is hitting you.
- Interrupt casters with a shield bash or a quick attack.
- Regroup: a badly hurt follower falls back to you instead of fighting alone.
- Stealth: while you sneak, followers sneak, keep further back and wait for your first shot.

**Upkeep**
- Use their own potions at sensible health thresholds.
- Optional callouts using the follower's own voice lines ("enemy ahead").

## Building
`tools/build.sh` builds the SKSE plugin (CommonLibSSE-NG, clang-cl + xwin) and the Papyrus
scripts (Caprica). `tools/make_esp.py` writes the plugin file that hosts the MCM.

## Requirements
- SKSE64 and Address Library for SKSE Plugins
- SkyUI (for the MCM)
- Recommended: [NPC Traversal NG](https://www.nexusmods.com/skyrimspecialedition/mods/196059),
  so followers can jump and climb after you.

## License
MIT
