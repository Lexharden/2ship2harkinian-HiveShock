# Changelog

All notable changes to 2Ship-HiveShock. Format based on [Keep a Changelog](https://keepachangelog.com/).
Each `## [vX.Y.Z]` section is published as the release notes of that tag.

## [v1.0.0] - 2026-10-14

First release of 2Ship-HiveShock, based on 2 Ship 2 Harkinian `develop` (`f1dda4c28`).

### Added
- **HiveShock server**: built-in TCP server (default port `43000`, optional access token) so an external companion
  app can control the live game. Windows, Linux and macOS.
- **Enemy spawns** with a persistent queue (survives scene changes, death and the title screen) and an admission
  system: enemy load limit, elite head start, elite priority and hard caps.
- **Player effects**: impulse, knockback, freeze, shock, fire, heal, damage, instant kill, rupees, magic, bombs,
  blast, keg, rupee rain, hearts, invert, jinx and rescue.
- **Buffs**: speed, slow, invincibility, infinite magic, damage up and haste, with tint feedback.
- **Time and world**: freeze/resume time, time skip, moon crash, owl statue warps, delete save.
- **Events to the companion** (port `43002`): deaths, capacity, freed slots, spawn queue and rejected spawns.
- **Menu** `Network > HiveShock` (port, token, limits, on-screen enemy counter) and `PlayerRescue` cheat.
- Documentation: `docs/HIVESHOCK.md` (protocol, commands, CVars).
- Release workflow publishing Windows (`.zip`), macOS (`.dmg`) and Linux (`.appimage`) builds, and a no-tabs CI check.
- Fork credit on the title screen.

### Changed
- Engine hooks for buff tint, invincibility and the damage multiplier target (`z_actor.c`, `z_parameter.c`,
  `z_collision_check.c`).
- Spawned enemies (Dinofos, Egol, Firefly, Floormaster, Iron Knuckle, Jso, Redead, Wallmaster, Attack Niw) can fight
  without their usual parent, path or cutscene.

### Fixed
- `En_Attack_Niw` no longer dereferences a missing parent when spawned directly.
- `scripts/check-no-tabs.sh` is now executable so the no-tabs workflow can run it.
