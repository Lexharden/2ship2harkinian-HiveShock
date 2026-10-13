# HiveShock (2Ship)

HiveShock is a TCP server built into the game. An external controller (the HiveShock companion app, a script, a phone)
connects to it and sends commands that spawn enemies or apply effects to the live game.

Source: `mm/2s2h/Network/HiveShock/`. Menu: **Network > HiveShock**.

## Connection

- Listens on `0.0.0.0:<Port>` (default `43000`). On the same PC use `127.0.0.1`; from another device use the LAN
  address shown in the menu.
- One JSON object per line (`\n` terminated). Lines over 8192 bytes are dropped.
- One client at a time. A new connection replaces the current one.
- Optional access token (`gRemote.HiveShock.Token`). When set, the first line must be
  `{"action":"auth","token":"<token>"}`, otherwise the connection is closed.
- The game sends events to the companion's event port `43002` on `127.0.0.1` (see below).

Works on Windows, Linux and macOS.

## Commands

`{"action":"<name>", "user":"<donor>", ...}`. Names are case-insensitive; `-` and spaces become `_`.

### Enemies and spawns

`enemy`/`like_like`, `guay`, `keese`, `fire_keese`, `ice_keese`, `chuchu`, `redead`, `wolfos`, `stalchild`, `bubble`,
`skulltula`, `bat`, `tektite`, `freezard`, `cucco`, `dinolfos`, `iron_knuckle`, `garo`, `garo_master`, `wallmaster`,
`floormaster`, `eyegore`, `fairy`, `bombchu`, plus `random_enemy` and `cucco_army`. `{"action":"spawn","enemy":"wolfos"}`
(also `name` / `actor` / `type`) is equivalent to `{"action":"wolfos"}`.

Spawns never run immediately: they go to a queue that survives scene changes, death and the title screen, and is saved
to `hiveshock_pending.json` (entries older than 12 h are dropped on load). A spawn enters the game when it fits in the
budget, nothing is ever removed to make room:

- **Enemy Load Limit** (`gRemote.HiveShock.MaxLoad`, default 14): sum of enemy weights alive (1-4 each; fairy and
  bombchu are free).
- **Elite Head Start** (`EliteReserve`, default 10): extra points only elite enemies (weight 3+: dinolfos, garo,
  garo_master, iron_knuckle, floormaster, eyegore) can use. **Elite Enemies First** (`ElitePriority`) lets them go ahead
  of ordinary ones in the queue. The rules are in `HiveShockAdmission.h` and checked with `static_assert`.
- **Hard limits** (`MaxAlive` 32, `MaxSceneEnemies` 40): headcount caps.
- Enemies alive when the scene or room changes are queued again so they follow Link. An enemy left more than 700 units
  away for 20 s is moved next to Link.
- Options: `DefeatOnDeath` (remove alive enemies when Link dies), `DespawnOnZoneChange` (leave them behind and count them
  as defeated).

`clear_enemies` kills the alive injected enemies. `clear_queue` (alias `clear_pending`) forgets the waiting ones.

### Player effects

`impulse`, `knockback`, `freeze`, `shock`, `fire`, `heal`, `damage`, `insta_kill` (`instakill`, `kill`), `rupees`,
`magic`, `bomb`, `blast` (`explode`, `bomb_explode`), `keg`, `rupee_rain`, `hearts`, `invert`, `jinx`, `rescue`.

Dangerous effects wait while Link cannot fight (cutscene, shop, minigame, horse, swimming, talking, ocarina, fade).

### Buffs (`duration` in seconds, `strength` optional)

`speed` (x2), `slow` (alias `slow_down`, x0.5, shares the speed slot), `invincible` (`invencible`), `infinite_magic`,
`damage_up` (x3), `haste` (speed + damage).

### Time and world

`freeze_time`, `resume_time`, `time_skip`, `time_up` (`times_up`, `time_zero`: moon crash), `warp` / `warp_random` /
`warp_<owl>` (owl statue warps), `delete_save` (`new_run`).

## Events sent to the companion (port 43002)

`player_death` / reset events (`deaths`), `capacity` (`used`, `max`, `queued`), `slot_freed` (`count`),
`spawn_queue` (`pending`, `active`, `load`, `max`, `eliteLoad`, `eliteReserve`, `eliteWaiting`) and `spawn_rejected`
(queue full: the new spawn is refused, older ones are kept).

## CVars

| CVar | Default | Meaning |
|---|---|---|
| `gCheats.HiveShockEnabled` | 1 | Server on/off |
| `gRemote.HiveShock.Port` | 43000 | Listen port (applied when enabling) |
| `gRemote.HiveShock.Token` | empty | Access token |
| `gRemote.HiveShock.MaxLoad` | 14 | Enemy load limit |
| `gRemote.HiveShock.EliteReserve` | 10 | Elite head start |
| `gRemote.HiveShock.ElitePriority` | 1 | Elites first in the queue |
| `gRemote.HiveShock.MaxAlive` / `MaxSceneEnemies` | 32 / 40 | Hard caps |
| `gRemote.HiveShock.DefeatOnDeath` | 0 | Remove alive enemies on death |
| `gRemote.HiveShock.DespawnOnZoneChange` | 0 | Leave enemies behind on zone change |
| `gWindows.HiveShockCounter` | 0 | On-screen enemy counter |
