# Bloodborne multiplayer in bbhost: what was found (2026-10-08)

Research done while trying to build an equivalent of Dark Souls III's Wex Dust:
invasions anywhere in the world, not only in your own area. Everything below
comes from the game's traffic with the `thehuntersdream.com` server
(`BBHOST_HTTP_TRACE`) and bbhost's diagnostics (`BBHOST_NP_TRACE`,
`BBHOST_NP_TEST=probe`). The experiment's code is in `src/hle/http.cpp`, the
scripts in `tools/win/wex/`.

## The sign server's protocol

- The game talks to `https://thehuntersdream.com:18671` with plain HTTP
  requests whose bodies are **plain JSON**, unencrypted. bbhost sees and can
  change every request before it goes out (`src/hle/http.cpp`, `perform`).
- The main requests:
  - `summon_messenger/create`: put up a sign;
  - `summon_messenger/get`: ask for signs;
  - `summon_messenger/request`: summon a sign that was found;
  - `summon_messenger/delete`: take one's own sign down;
  - `wandering_ghost/create|get`, `blood_messenger/*`, `tomb_messenger/*`:
    ghosts, messages, bloodstains (about once a minute anywhere online);
  - `PUT /<date>/<id>` every ~5 s: the play log.
- The fields that place a request:
  - `AreaId` = `(map << 24) | (submap << 16)`: `m24_01` (Central Yharnam) is
    402718720, `m26_00` (Nightmare of Mensis) is 436207616;
  - `AreaRegionId`: a region within the map, shaped `MMNRRR`, e.g. 241010 or
    260000. Mensis has 11 (260000 to 260150, listed in
    `blood_messenger/message_area`);
  - `PosX/PosY/PosZ`: the player's position. Searches also carry
    `DistanceThreshold: 100`.
- `SummonType`: 0 is a cooperator's sign (the host looks for these after the
  Beckoning Bell), 2 an invader's (the Sinister Bell). `MatchingLevel` is the
  matchmaking level (544 for both of our characters). `CharaId` is the same for
  everyone (`9223372036854775808`) and is a 64-bit integer, so a body must not
  be rewritten through a JSON parser that holds numbers as `double`, only by
  editing the text.

## How an invasion happens

1. The **invader** rings the Sinister Resonant Bell, and their game posts a
   `create` with `SummonType 2` for its own area and position about once a
   minute.
2. A **host** whose world has a ringing Chime Maiden posts a `get` with
   `SummonType 2` for **its own** area about **every 70 seconds**. **The host
   searches, not the invader**, unlike DS2 and DS3. Hosts put nothing on the
   server, so an invader has nobody to search for.
3. Having found a sign, the host sends `request` and creates a room
   (`sceNpMatching2CreateJoinRoom`, bbhost's session service).
4. The server sends the invader a `guest_invite` event (through
   `/np/events/poll`).
5. The two games connect: STUN on `:3478`, and the server's relay when that
   fails. The host sends the session item (~267 bytes) over P2P (vport 40).
6. The invader's game should take the item and join the room (`JoinRoom`).

The maiden:
- her effect on the host is SpEffect 9020 (`stateInfo 191`). Central Yharnam's
  flags are 12414220 to 12414223 (watched with `BBHOST_NP_WATCH_FLAGS`);
- after the Beckoning Bell in Central Yharnam she appeared about 3.5 minutes
  later once, and not at all within 3 minutes another time;
- in the Nightmare of Mensis she always rings;
- when both worlds have a maiden and both players ring the Sinister Bell, the
  roles get mixed up: each is host and invader at once.

## What worked and what did not

- **Matching works reliably.** A host with a maiden found the invader's sign
  more than seven times, in Central Yharnam and in Mensis.
- **A connection was never made** (two instances on one PC, one public
  address, both through the server's relay, about 14 fps each). It broke in
  three different ways:
  1. the handshake started and stalled;
  2. `guest_invite` arrived and the session item reached the invader, but the
     invader's game did not take it (`passive entries 1` but
     `staged member 0`; the host waited for an `ack`);
  3. the host found the sign and created a room, but the server sent the
     invader no `guest_invite` (its event polling ran without errors).

  Co-op (Beckoning and Small Resonant Bell) did not connect in the same setup
  either, so the cause is not the invasion mechanics but connecting two
  instances on one PC. Why exactly is not visible from the client; it needs the
  server's log.
- Two instances on 16 GB of RAM need a large page file (each reserves about
  6 GB for the PS4's memory) and run into CPU or GPU limits. Lowering the
  render resolution to 800x600 did not raise the frame rate.

## What is in the code

- `BBHOST_HTTP_TRACE=1|<folder>`: every request logged, bodies written to files.
- `BBHOST_INVADE_AREA=AreaId,AreaRegionId[,X,Y,Z]`: the sign in one fixed area.
- `BBHOST_WEX=all`: the sign in its own area plus copies in every area of
  `wex-areas.txt` at once. `BBHOST_WEX=1`: round the areas, one per sign.
- `wex-areas.txt` fills itself from `wandering_ghost/create` (the position
  from the game's memory, `FrpgNetMan`) and from the sign requests.
- **Wex has not been tried in a game**: there was not a single run with it.

## Where it could go

**Wex (invasions anywhere).** Doable entirely on the invader's side, without
breaking into the server and without hooks in the game's code, by rewriting
JSON fields. One test on two PCs answers the open questions:
- does the server keep several signs per player (one per area)? If so, `all`
  is found at the next search of any host (within 70 s, about 35 s on average).
  If not, only the rotation is left, with a 1/N chance per search;
- does the server match on `AreaId`, `AreaRegionId`, or distance as well
  (`DistanceThreshold`)? That decides which positions to put in.

**Faster than about every 70 s** is not possible from the invader's side: the
host's search sets the pace. Only the host's client can speed it up (reversing
the search timer), so only for hosts running this fork.

**Always invadable, without a maiden.** The maiden is checked on the host's
client (SpEffect 9020). Her effect could be on all the time, by the same
event-script rewriting the "two invaders" option already does
(`src/engine/maiden_events.cpp`) or by a hook. It only works for hosts running
the fork, and such a mode should be announced to the server through
`X-BBHost-Ruleset` so it stays apart from vanilla players.

**Something like Seamless Coop.** Technically possible (bbhost owns the whole
network stack and the session service, and has the engine SDK and the
decompilation), but months of reverse engineering: no session break on a boss,
a death or a lamp; map changes and event flags kept in step; most likely server
work as well.

## Next steps, if the topic is picked up again

1. Check matching and the connection on **two different PCs**. That removes
   the relay, the shared address and the performance limits at once.
2. Ask the bbhost and server authors whether the server's logs have rooms
   1303, 1307, 1311, 1312 and 1315 (2026-10-08, user_id 655 with 671/701), why
   no `guest_invite` went out, and whether two clients from one address through
   the relay are supported.
3. Try Wex: host in Mensis, invader with the Sinister Bell in another area,
   `BBHOST_WEX=all`; look for the `wex:` lines in the invader's log and a
   non-empty `get` answer on the host.
