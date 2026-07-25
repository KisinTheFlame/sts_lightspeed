# sts-engine trace harness

Generates replayable combat traces for [@kisinwen/sts-engine](https://github.com/KisinTheFlame/sts-engine)
by driving this project's **real** `BattleContext` and recording
`{ action, stateAfter }` per step. The TypeScript side replays the identical
recorded action list and diffs every frame, so the policy can never be a source
of divergence — only engine behaviour can.

Lives on this branch (not `master`) so it stays out of the upstream PR.

## Build

No submodules or CMake needed — `SaveFile.cpp` is the only nlohmann-json user,
so exclude it:

```sh
SRCS=$(find src -name '*.cpp' ! -name 'SaveFile.cpp' | tr '\n' ' ')
clang++ -std=c++17 -O2 -w -Iinclude -I. tools/sts-engine-harness/trace_dump.cpp ${=SRCS} -o /tmp/trace_dump
/tmp/trace_dump > traces.json
```

(`${=SRCS}` is zsh word splitting; use `$SRCS` in bash.)

## Splitting into the engine repo layout

The engine stores one JSONL file per encounter under `test/golden/traces/`:

```js
const by = {};
for (const t of JSON.parse(fs.readFileSync("traces.json","utf8")).traces) (by[t.encounter] ||= []).push(t);
for (const [enc, list] of Object.entries(by)) {
  list.sort((a,b) => a.seed === b.seed ? a.floor - b.floor : (a.seed < b.seed ? -1 : 1)); // stable => byte-identical regeneration
  fs.writeFileSync(`test/golden/traces/${enc.toLowerCase()}.jsonl`, list.map(t => JSON.stringify(t)).join("\n") + "\n");
}
```

## Gotchas that silently corrupt the data

1. **Reseed per floor.** Setting `gc.floorNum` alone leaves `miscRng` on the
   constructor's `Random(seed)`; the game reseeds `miscRng = shuffleRng =
   cardRandomRng = Random(seed + floorNum)` on every floor
   (`GameContext::transitionToMapNode`). Without it every floor rolls the same
   variant encounter.
2. **Monster `STRENGTH` is not in `statusBits`.** It lives in its own int field,
   so gating the dump on `hasStatusInternal` drops it entirely. Use
   `getStatusInternal` and keep non-zero values.
3. **`Player::cc` is never assigned anywhere in this project** and has no
   initialiser, so Entropic Brew reads an indeterminate character class in
   `returnRandomPotion(potionRng, player.cc, true)`. The harness sets
   `bc.player.cc = gc.cc` explicitly — that is a deliberate deviation from the
   code as-built, since UB is not worth reproducing.
4. **Entropic Brew refills slots with arbitrary potions**, some of which open a
   card-select screen. The drinking policy uses a whitelist of potions the
   engine has registered.
