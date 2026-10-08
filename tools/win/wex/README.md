# Wex: invading across the whole world (experiment)

An experiment in the spirit of Dark Souls III's Wex Dust, for Bloodborne.
The code is in `src/hle/http.cpp` (search for `BBHOST_WEX`).

What the server traffic shows: the Sinister Resonant Bell makes the invader's
game post a sign (`summon_messenger/create`, `SummonType 2`) for its own area,
about once a minute; a host whose world has a ringing Chime Maiden asks
(`summon_messenger/get`, `SummonType 2`) for the signs of its own area about
every 70 s, finds one and summons it. The host searches, not the invader.

- `BBHOST_WEX=all`: the game's sign stays in its area and a copy goes to every
  other open area of `wex-areas.txt` at once.
- `BBHOST_WEX=1`: the sign moves round the areas, one per sign the game posts.
- `BBHOST_INVADE_AREA=AreaId,AreaRegionId[,X,Y,Z]`: one fixed area (wins).
- `wex-areas.txt` fills itself: every area either instance is in (from the
  game's `wandering_ghost/create`, about once a minute online) is appended
  with the player's position. A line starting with `#` is not visited.
- `BBHOST_HTTP_TRACE=1|<dir>`: every request logged, bodies written to files.

The scripts here run two instances on one PC (`run-host.bat`,
`run-invader.bat` with its own config folder); copy them beside `bbhost.exe`.

Status (2026-10-08): matching works - a host with a maiden found the invader's
sign every time, in Central Yharnam and the Nightmare of Mensis. The
connection after it did not complete with both instances on one PC behind one
address through the server's relay (and at ~14 fps each); the invasion was
never seen through, and `BBHOST_WEX` itself was not tested in a game.
