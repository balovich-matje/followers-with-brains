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
