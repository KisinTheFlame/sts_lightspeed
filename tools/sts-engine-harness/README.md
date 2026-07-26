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

## Deck variants: coverage is append-only

Every trace records the deck it was generated with, and the replayer replays whatever
each line states. So a new batch of registered cards does **not** require regenerating
the existing ~23MB — it adds a deck variant, which only ever appends lines.

`variants` in `main()`:

| # | deck | seeds | upgraded |
| - | ---- | ----- | -------- |
| 0 | starter + `BATCH_1` | all 125 | no |
| 1 | starter + `BATCH_1` + `BATCH_2` | first 40 | no |
| 2 | starter + `BATCH_1` + `BATCH_2` | first 40 | yes |

Two rules keep regeneration honest:

- **Variant 0 stays first and unchanged.** `traceIdx` drives the relic/potion rotation,
  so keeping variant 0's iteration order intact reproduces the committed files
  byte-for-byte. Verify that after any change to this file: split the output and `cmp`
  the five committed encounters before trusting the new lines.
- **`deckUpgraded` is emitted only when something is upgraded**, so variant 0/1 lines
  stay byte-identical to what was committed before that field existed. The TS side reads
  `t.deckUpgraded?.[i] ?? false`.

Variant 2 exists because the un-upgraded pass never exercises the `up ? x : y` branch of
a card rule — that is half of every rule, and it was entirely unverified before. Only
cards the reference says `canUpgrade()` get the flag: forcing it onto a card with no
upgraded form would make the two sides disagree about its cost for a reason that has
nothing to do with the card's rule.

Adding a card to a variant's deck does not verify it — the policy plays the leftmost
playable card, so a card can sit in a deck and never be exercised. Count actual plays
per card after regenerating; 0 plays means a registered rule with no oracle behind it.
At 40 seeds each batch-2 card lands 89–207 un-upgraded and 65–170 upgraded plays across
the five shipped encounters, which is the margin `seedLimit` is tuned for.

**Batch 3 onward: replace variants 1/2, do not stack more pairs.** A later batch's deck is
a superset of this one's, so its variant pair subsumes these — appending a third pair
would re-verify the same cards at ~20MB a batch and put the engine repo past 100MB after
a few rounds. Keep variant 0 (the original committed baseline, cheap and already there)
plus exactly one current-full-deck pair, and let the pair's lines be rewritten in place.
The rewrite stays confined to the file's tail because ordering is variant-major.

## Splitting into the engine repo layout

The engine stores one JSONL file per encounter under `test/golden/traces/`:

```js
const by = {};
for (const t of JSON.parse(fs.readFileSync("traces.json","utf8")).traces) (by[t.encounter] ||= []).push(t);
// TOTAL order, not merely stable: several variants share a (seed, floor), and leaning on
// V8's sort stability would make byte-identical regeneration an implementation detail.
const key = (t) => [t.seed, t.floor, t.deck.length, t.deckUpgraded === undefined ? 0 : 1];
for (const [enc, list] of Object.entries(by)) {
  list.sort((a, b) => { const ka = key(a), kb = key(b);
    for (let i = 0; i < ka.length; i++) if (ka[i] !== kb[i]) return ka[i] < kb[i] ? -1 : 1;
    return 0; });
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
