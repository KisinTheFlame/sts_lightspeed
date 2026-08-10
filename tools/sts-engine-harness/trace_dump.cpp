// Real-oracle trace dumper for sts-engine.
//
// This links the reference project's ACTUAL BattleContext (not a transcription of it),
// drives real battles with a deterministic policy, and emits a replayable trace:
//   { action, stateAfter } * N
// The TypeScript side replays the identical action sequence and compares every frame.
// Because both sides consume the same recorded action list, the policy itself can never
// be a source of divergence — only the engine behaviour can.
//
// Build (no submodules / CMake needed; SaveFile.cpp is the only nlohmann-json user):
//   cd ~/Workspace/sts_lightspeed
//   SRCS=$(find src -name '*.cpp' ! -name 'SaveFile.cpp' | tr '\n' ' ')
//   clang++ -std=c++17 -O2 -w -Iinclude -I. /tmp/stsprobe/trace_dump.cpp ${=SRCS} -o /tmp/stsprobe/trace_dump

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "game/GameContext.h"
#include "combat/BattleContext.h"
#include "sim/search/Action.h"

using namespace sts;

// ---------------------------------------------------------------- json helpers

static std::string q(const std::string &s) { return "\"" + s + "\""; }

template <typename T>
static std::string arr(const std::vector<T> &v) {
    std::ostringstream os;
    os << "[";
    for (size_t i = 0; i < v.size(); ++i) { if (i) os << ","; os << v[i]; }
    os << "]";
    return os.str();
}

static std::string strArr(const std::vector<std::string> &v) {
    std::ostringstream os;
    os << "[";
    for (size_t i = 0; i < v.size(); ++i) { if (i) os << ","; os << q(v[i]); }
    os << "]";
    return os.str();
}

// ---------------------------------------------------------------- snapshots

// Amount of one player status, avoiding getStatusRuntime's throw on bool-only statuses.
//
// BARRICADE / CORRUPTION / CONFUSED / PEN_NIB / SURROUNDED / BLASPHEMER / ELECTRO /
// MASTER_REALITY / WRATH_NEXT_TURN are applied via setHasStatus, which only flips a bit
// in statusBits and NEVER writes statusMap (Player.h:233). getStatusRuntime's default
// branch then does statusMap.at(s) on a missing key and throws std::out_of_range — so
// dumping a snapshot with Barricade up would abort the whole run.
//
// Report those as 1, which is what the engine side stores for a bool status.
static int playerStatusValue(const Player &p, PlayerStatus s) {
    switch (s) {
        // These four live in their own int fields and are not in statusMap at all.
        case PlayerStatus::ARTIFACT:
        case PlayerStatus::DEXTERITY:
        case PlayerStatus::FOCUS:
        case PlayerStatus::STRENGTH:
            return p.getStatusRuntime(s);
        default:
            break;
    }
    const auto it = p.statusMap.find(s);
    return it == p.statusMap.end() ? 1 : it->second;
}

// Every non-zero player status, as {"NAME": amount}.
static std::string playerStatuses(const Player &p) {
    std::ostringstream os;
    os << "{";
    bool first = true;
    for (int i = 0; i < static_cast<int>(PlayerStatus::THE_BOMB) + 1; ++i) {
        const auto s = static_cast<PlayerStatus>(i);
        if (!p.hasStatusRuntime(s)) continue;
        const int v = playerStatusValue(p, s);
        if (v == 0) continue;
        if (!first) os << ",";
        first = false;
        os << q(playerStatusEnumStrings[i]) << ":" << v;
    }
    os << "}";
    return os.str();
}

static std::string monsterStatuses(const Monster &m) {
    std::ostringstream os;
    os << "{";
    bool first = true;
    for (int i = 0; i < static_cast<int>(MonsterStatus::INVALID); ++i) {
        const auto s = static_cast<MonsterStatus>(i);
        // Do NOT gate on hasStatusInternal: STRENGTH lives in its own int field and is
        // never recorded in statusBits, so gating would silently drop it. getStatusInternal
        // special-cases it and returns 0 for anything genuinely unset.
        const int v = m.getStatusInternal(s);
        if (v == 0) continue;
        if (!first) os << ",";
        first = false;
        os << q(monsterStatusEnumStrings[i]) << ":" << v;
    }
    os << "}";
    return os.str();
}

static std::vector<std::string> cardNamesOf(const std::vector<CardInstance> &pile) {
    std::vector<std::string> out;
    for (const auto &c : pile) out.push_back(getCardEnumName(c.getId()));
    return out;
}

static const char *outcomeName(Outcome o) {
    switch (o) {
        case Outcome::PLAYER_VICTORY: return "player_victory";
        case Outcome::PLAYER_LOSS:    return "player_loss";
        default:                      return "undecided";
    }
}

// Gold at the moment this battle started, so snapshots can report a DELTA.
//
// Player::gold is a run-level resource copied in by BattleContext::init
// (`player.gold = gc.gold`, BattleContext.cpp:55) and copied back out by exitBattle
// (`g.gold = player.gold`, :484). Inside a battle exactly two things touch it:
// HAND_OF_GREED's kill bonus (Player::gainGold) and the Looter/Mugger theft
// (Monster::stealGoldFromPlayer). Neither can happen in the five encounters this repo
// keeps, so before batch 11 gold was the constant 99 in every committed line.
//
// Hence a DELTA, emitted only when non-zero — the same trick `deckUpgraded` uses below.
// Dumping gold unconditionally would rewrite every line of every file including the
// FROZEN variant 0, which is precisely what tools/regen-traces.sh refuses.
static int s_goldBaseline = 0;

static std::string snapshot(const BattleContext &bc) {
    std::ostringstream os;
    os << "{";
    os << q("turn") << ":" << bc.turn;
    os << "," << q("outcome") << ":" << q(outcomeName(bc.outcome));
    if (static_cast<int>(bc.player.gold) != s_goldBaseline) {
        os << "," << q("goldGained") << ":" << (static_cast<int>(bc.player.gold) - s_goldBaseline);
    }

    os << "," << q("player") << ":{"
       << q("hp") << ":" << bc.player.curHp
       << "," << q("maxHp") << ":" << bc.player.maxHp
       << "," << q("block") << ":" << bc.player.block
       << "," << q("energy") << ":" << bc.player.energy
       << "," << q("powers") << ":" << playerStatuses(bc.player)
       << "}";

    os << "," << q("monsters") << ":[";
    for (int i = 0; i < bc.monsters.monsterCount; ++i) {
        const auto &m = bc.monsters.arr[i];
        if (i) os << ",";
        os << "{" << q("id") << ":" << q(monsterIdStrings[static_cast<int>(m.id)])
           << "," << q("hp") << ":" << m.curHp
           << "," << q("maxHp") << ":" << m.maxHp
           << "," << q("block") << ":" << m.block
           << "," << q("alive") << ":" << (m.isDeadOrEscaped() ? "false" : "true");
        // isDeadOrEscaped() is three bits ORed together (Monster.cpp:253) and `alive` above
        // collapses them, but ONE of the three changes what happens next:
        // MonsterGroup::doMonsterTurn's gate is `(!isDeadOrEscaped() || isHalfDead())`
        // (MonsterGroup.cpp:572), so a HALF-DEAD monster still takes its turn while a dead or
        // escaped one does not. Darkling's Regrow and the Awakened One's fake death are the
        // two producers; a Darkling sits half-dead for two monster turns (Regrow, then
        // Reincarnate) with curHp 0 and alive=false, which is indistinguishable from a corpse
        // unless this bit is emitted.
        //
        // Emitted ONLY when true, exactly like `deckUpgraded` / `ascension` / `goldGained` /
        // `playerHp`: no monster in any previously committed encounter can be half-dead
        // (nothing registered before batch 34 carries MS::REGROW and the Awakened One is not
        // installed), so every frozen file stays byte-identical. regen-traces.sh verifies that.
        if (m.isHalfDead()) {
            os << "," << q("halfDead") << ":true";
        }
        os << "," << q("move") << ":" << q(monsterMoveStrings[static_cast<int>(m.moveHistory[0])])
           << "," << q("powers") << ":" << monsterStatuses(m)
           << "}";
    }
    os << "]";

    // Hand is a fixed-size array sliced by cardsInHand; the piles are vectors.
    std::vector<std::string> hand;
    for (int i = 0; i < bc.cards.cardsInHand; ++i) hand.push_back(getCardEnumName(bc.cards.hand[i].getId()));
    os << "," << q("hand") << ":" << strArr(hand);
    os << "," << q("draw") << ":" << strArr(cardNamesOf(bc.cards.drawPile));
    os << "," << q("discard") << ":" << strArr(cardNamesOf(bc.cards.discardPile));
    os << "," << q("exhaust") << ":" << strArr(cardNamesOf(bc.cards.exhaustPile));

    std::vector<std::string> pots;
    for (int i = 0; i < bc.potionCapacity; ++i) {
        pots.push_back(bc.potions[i] == Potion::EMPTY_POTION_SLOT
                           ? "EMPTY"
                           : getPotionName(bc.potions[i]));
    }
    os << "," << q("potions") << ":" << strArr(pots);

    os << "," << q("rng") << ":{"
       << q("ai") << ":" << bc.aiRng.counter
       << "," << q("hp") << ":" << bc.monsterHpRng.counter
       << "," << q("shuffle") << ":" << bc.shuffleRng.counter
       << "," << q("cardRandom") << ":" << bc.cardRandomRng.counter
       << "," << q("misc") << ":" << bc.miscRng.counter
       << "," << q("potion") << ":" << bc.potionRng.counter
       << "}";

    os << "}";
    return os.str();
}

// ---------------------------------------------------------------- policy

// Deterministic: play the lowest-index playable card at the living monster the variant's
// TARGET POLICY names; when nothing is playable, end the turn. Recorded verbatim, so the
// TS side never re-derives it.
static int firstAliveMonster(const BattleContext &bc) {
    for (int i = 0; i < bc.monsters.monsterCount; ++i) {
        if (!bc.monsters.arr[i].isDeadOrEscaped()) return i;
    }
    return 0;
}

// The MIRROR of firstAliveMonster: the HIGHEST-index living monster.
//
// WHY THIS EXISTS (batch 31 of the engine repo). Until now every trace in the corpus
// attacked slot 0 first, so in a multi-monster group the LAST monster to die was always the
// highest-index one. Whole families of reference behaviour are gated on "a companion died
// before me" and were therefore structurally unreachable at ANY deck, ANY encounter, ANY
// ascension — the purest case being CENTURION_FURY, which the Centurion only rolls when the
// Mystic is already dead while CENTURION_AND_HEALER (the only encounter in the whole
// reference that contains a Centurion) puts the Centurion at slot 0.
//
// Picking the LAST living monster is the exact opposite of the existing policy, which
// maximises the information gained: it reverses the death order in every multi-monster
// group rather than merely perturbing it.
//
// Same predicate (isDeadOrEscaped) and same `return 0` fallback as firstAliveMonster, so
// the two agree by construction whenever exactly one monster is alive — which is why
// single-monster encounters are deliberately NOT run under this policy (see the variant
// below): the two policies would emit byte-identical traces.
//
// ⚠ Reserved-but-never-constructed slots (Gremlin Leader's slot 0, the Automaton's slots
// 0/2, the Collector's slots 0/1) hold a default Monster whose curHp is 0, so
// isDeadOrEscaped() is true for them and the scan skips them from EITHER end. That is what
// makes a backwards scan safe without a separate `idx != -1` test.
static int lastAliveMonster(const BattleContext &bc) {
    for (int i = bc.monsters.monsterCount - 1; i >= 0; --i) {
        if (!bc.monsters.arr[i].isDeadOrEscaped()) return i;
    }
    return 0;
}

// Target policy, as stored on a DeckVariant. 0 is the historical behaviour, so every
// variant declared before this axis existed keeps emitting byte-identical traces.
enum class TargetPolicy { FIRST_ALIVE = 0, LAST_ALIVE = 1 };

static int policyTarget(const BattleContext &bc, int targetPolicy) {
    return targetPolicy == static_cast<int>(TargetPolicy::LAST_ALIVE) ? lastAliveMonster(bc)
                                                                     : firstAliveMonster(bc);
}

struct Step { std::string type; int idx; int target; std::vector<int> idxs; };

// First batch of combat relics sts-combat has registered. Rotated per trace so the
// data covers both initRelics passes (immediate stat buffs, and the queued
// atBattleStart effects that land after the opening draw).
//
// `data` is RelicInstance::data, the per-relic counter the run layer keeps. It is 0 for
// every rotation entry (none of them reads it), but it is NOT cosmetic in general:
// BattleContext::initRelics reads it for a handful of relics, and for two of them
// (OMAMORI, LIZARD_TAIL) it writes `p.setHasRelic<X>(r.data)` — i.e. a relic added with
// data 0 is INVISIBLE to `player.hasRelic<>()` inside combat. Batch 40 needs Omamori's
// gate to be TRUE, so the field exists and defaults to 0 (which keeps every existing
// `{RelicId::X, "name"}` initialiser valid and every committed line byte-identical).
struct RelicSpec { RelicId id; const char *name; int data = 0; };

// Batch 44. `RelicInstance::data` carries TWO different meanings in initRelics, and the
// trace format carries them on two different channels because only one of them is a number:
//
//   (a) NUMERIC — a cross-combat counter that initRelics copies into a Player field and
//       updateRelicsOnExit copies back: HAPPY_FLOWER (+1!), INCENSE_BURNER, INK_BOTTLE,
//       INSERTER, NUNCHAKU, PEN_NIB (data == 9 takes a different branch), SUNDIAL, and
//       NEOWS_LAMENT (`if (r.data > 0)`, decremented on exit). The replayer cannot guess
//       these, so they go into a new `relicData` array, emitted ONLY when some numeric
//       relic carries a non-zero value — which no committed variant does, so all 150
//       committed files stay byte-identical (proved with --check before any new variant
//       was added, same two-step as `ascension` / `targetPolicy` / `relicSet`).
//
//   (b) BOOLEAN — `p.setHasRelic<X>(r.data)` for OMAMORI and LIZARD_TAIL. Combat only ever
//       reads the truthiness, and `data == 0` means "this relic does not exist inside the
//       fight", i.e. handing one out with data 0 is strictly worse than not handing it out
//       at all. The check below rejects that, so "listed in `relics`" ⟺ "data non-zero"
//       holds by construction and the replayer needs no field for it. That is what keeps
//       writhing_mass@relic3 (Omamori, data 2, committed in batch 40) byte-identical.
static bool relicDataIsNumeric(RelicId id) {
    return id != RelicId::OMAMORI && id != RelicId::LIZARD_TAIL;
}

static const std::vector<RelicSpec> RELIC_ROTATION {
    {RelicId::VAJRA,               "vajra"},
    {RelicId::ANCHOR,              "anchor"},
    {RelicId::BRONZE_SCALES,       "bronze_scales"},
    {RelicId::ODDLY_SMOOTH_STONE,  "oddly_smooth_stone"},
    {RelicId::BLOOD_VIAL,          "blood_vial"},
    {RelicId::LANTERN,             "lantern"},
    {RelicId::BAG_OF_MARBLES,      "bag_of_marbles"},
    {RelicId::BAG_OF_PREPARATION,  "bag_of_preparation"},
};

// Potions sts-combat has a POTION_RULES entry for. Drinking anything else would make the trace
// UNREPLAYABLE (the TS side throws "sts-combat 暂未登记药水") rather than merely unverified.
//
// ⚠⚠ TWO LISTS, AND THE SPLIT IS LOAD-BEARING (batch 45). This gate does not only decide what
// an explicitly-loadout-ed variant may drink: ENTROPIC_BREW refills the slots with potions drawn
// from the WHOLE pool, and ~3/13 of every rotation-fed trace holds a brew. So widening the list
// globally would make the policy start drinking brew-conjured potions it used to skip — i.e. it
// would rewrite a large fraction of the 163 COMMITTED files. Measured consequence, not a worry:
// the committed corpus is the oracle, and `--check` compares it byte for byte.
//
// The widened list is therefore reachable only from a variant with a non-zero `potionSet`, which
// by construction is a variant whose rows land in a brand-new `@potN` file. Every pre-batch-45
// variant has potionSet 0 and keeps exactly the 13 potions it always had.
//
// ⚠ Corollary for future batches: to hand out a newly registered potion you must open a `@potN`
// variant. Adding one to an `@relicN` variant is NOT possible without changing committed data.
static bool isReplayablePotion(Potion p, bool extended) {
    switch (p) {
        // ---- the original 13 (batches 1-13; frozen, see above) --------------------------
        case Potion::BLOCK_POTION: case Potion::FIRE_POTION: case Potion::STRENGTH_POTION:
        case Potion::WEAK_POTION: case Potion::FEAR_POTION: case Potion::ENERGY_POTION:
        case Potion::SWIFT_POTION: case Potion::DEXTERITY_POTION: case Potion::BLOOD_POTION:
        case Potion::ANCIENT_POTION: case Potion::EXPLOSIVE_POTION: case Potion::FRUIT_JUICE:
        case Potion::ENTROPIC_BREW:
            return true;

        // ---- batch 45's 15 (only for potionSet != 0 variants) ---------------------------
        // Pure player-status buffs whose Power the engine already models:
        case Potion::FLEX_POTION: case Potion::SPEED_POTION: case Potion::HEART_OF_IRON:
        case Potion::LIQUID_BRONZE: case Potion::ESSENCE_OF_STEEL: case Potion::GHOST_IN_A_JAR:
        case Potion::FOCUS_POTION: case Potion::REGEN_POTION: case Potion::CULTIST_POTION:
        case Potion::DUPLICATION_POTION:
        // Actions the engine already has (Armaments+ / Purity / Havoc) or that only rewrite
        // EXISTING cards' cost — never conjure a definition out of CardPools.h:
        case Potion::BLESSING_OF_THE_FORGE: case Potion::ELIXIR_POTION:
        case Potion::SNECKO_OIL: case Potion::DISTILLED_CHAOS:
        // Single-card select over the discard pile; the enumerator supports the task.
        case Potion::LIQUID_MEMORIES:
            return extended;

        default:
            return false;
    }
}

// Only cards sts-combat has a CARD_RULES entry for. Same criterion, and the same reason, as
// isReplayablePotion above: playing an unregistered card makes the trace UNREPLAYABLE (the TS
// side throws "暂未登记卡牌行为") rather than merely unverified.
//
// Before batch 8 this gate was unnecessary. Every card that could reach the hand came either
// from the deck — and the deck variants below only ever hold registered cards — or from a
// status/curse generator (Burn / Wound / Dazed), none of which is playable in the first place.
// Batch 8's five cards break that: Chrysalis / Metamorphosis / Discovery / Jack of All Trades /
// Infernal Blade pull card DEFINITIONS out of CombatTypeCardPool, CombatCardPool and
// CombatColorlessCardPool (CardPools.h), which between them name 104 distinct cards. 18 of
// those are not registered in sts-combat, and they are listed here.
//
// This is a BLOCKLIST rather than a whitelist on purpose. The set of cards that can enter
// combat is exactly (deck ∪ those three pools ∪ the status generators), so enumerating the
// unregistered members of the pools is complete by construction; and if a future batch ever
// makes the list stale, the failure is LOUD (the TS replay throws on the very first trace that
// plays one) instead of silently shrinking coverage the way a missing whitelist entry would.
//
// Adding the gate is a no-op for variants 0-6: their decks contain only registered cards, and
// this was verified by running tools/regen-traces.sh --check with the gate in place and the
// batch-8 variants not yet added — the whole file reproduced byte for byte.
static bool isReplayableCard(CardId id) {
    switch (id) {
        // What is left is the cards the reference itself never implemented well enough to be
        // an oracle, plus four that need mechanisms outside the Ironclad+colourless scope.
        // (Batch 9's HAVOC / MAYHEM / DOUBLE_TAP / DUAL_WIELD, batch 10's WHIRLWIND /
        // TRANSMUTATION / APOTHEOSIS, batch 11's PERFECTED_STRIKE / CLASH / HAND_OF_GREED /
        // THE_BOMB and batch 12's DARK_SHACKLES / VIOLENCE were removed from this list when
        // they were registered.)
        case CardId::FORETHOUGHT:       // reference's upgraded branch is commented out
        case CardId::MAGNETISM:
        case CardId::ENLIGHTENMENT:
        case CardId::PANACHE:
        case CardId::SADISTIC_NATURE:
            return false;
        default:
            return true;
    }
}

// ---- second half of the gate: cards that make the REFERENCE pick a card ------------------
//
// isReplayableCard above only filters what the POLICY picks out of hand. That is not enough
// once batch 9 registered HAVOC and MAYHEM: both hand the choice to
// BattleContext::playTopCardInDrawPile, which takes whatever is on top of the draw pile and
// consults no gate at all. Batch 10 hit this for real — in variants 7/8 (batch 8's pool cards)
// a CHRYSALIS-conjured HAVOC and a JACK_OF_ALL_TRADES-conjured MAYHEM each played a CLASH that
// the engine has no rule for, and the replay threw "暂未登记卡牌行为" mid-trace. It was latent
// from batch 9 on; only the reshuffled RNG stream of a new batch made it surface.
//
// The two cards need different tests because one is instantaneous and the other persists.
static bool anyUnreplayableIn(const std::vector<CardInstance> &pile) {
    for (const auto &c : pile) {
        if (!isReplayableCard(c.getId())) return true;
    }
    return false;
}

// Cards that conjure a card DEFINITION out of CardPools.h, i.e. the only way an unregistered
// card can appear in a combat whose deck holds none.
static bool isPoolConjuringCard(CardId id) {
    switch (id) {
        case CardId::CHRYSALIS:
        case CardId::METAMORPHOSIS:
        case CardId::DISCOVERY:
        case CardId::JACK_OF_ALL_TRADES:
        case CardId::INFERNAL_BLADE:
        case CardId::TRANSMUTATION:
            return true;
        default:
            return false;
    }
}

static bool anyPoolConjuringIn(const std::vector<CardInstance> &pile) {
    for (const auto &c : pile) {
        if (isPoolConjuringCard(c.getId())) return true;
    }
    return false;
}

// Whether the policy may play hand[handIdx]. Wraps isReplayableCard with the autoplay rules.
static bool mayPlayHandCard(const BattleContext &bc, int handIdx) {
    const auto id = bc.cards.hand[handIdx].getId();
    if (!isReplayableCard(id)) return false;

    if (id == CardId::HAVOC) {
        // Havoc plays the draw pile's top card immediately, and if the draw pile is EMPTY it
        // shuffles the discard pile in first and plays the new top — so both piles must be
        // clean. Nothing conjures between the play and the action, so "clean right now" is
        // sound here. Deliberately NOT gated on the draw pile being non-empty: the
        // empty-draw-pile branch of playTopCardInDrawPile is one of the things batch 9's
        // variants 9/10 exist to cover.
        return !anyUnreplayableIn(bc.cards.drawPile) && !anyUnreplayableIn(bc.cards.discardPile);
    }

    if (id == CardId::MAYHEM) {
        // Mayhem is a POWER: it keeps playing the draw pile's top card at the start of every
        // remaining turn, so "clean right now" is NOT enough. Nothing may be able to conjure an
        // unregistered card later either — and that includes a conjuring card being played by
        // Mayhem itself, which no policy-side check can refuse. Hence the stronger test: no
        // unregistered card and no pool-conjuring card anywhere in the combat.
        for (const auto *pile : {&bc.cards.drawPile, &bc.cards.discardPile, &bc.cards.exhaustPile}) {
            if (anyUnreplayableIn(*pile) || anyPoolConjuringIn(*pile)) return false;
        }
        for (int i = 0; i < bc.cards.cardsInHand; ++i) {
            const auto hid = bc.cards.hand[i].getId();
            if (!isReplayableCard(hid) || isPoolConjuringCard(hid)) return false;
        }
        return true;
    }

    return true;
}

// Card-select screens use a different action space. Enumerate what the reference
// itself offers and take the lowest-index option, so the choice is deterministic and
// expressible in the trace.
static bool pickCardSelectAction(const BattleContext &bc, Step &out) {
    const auto actions = search::Action::enumerateCardSelectActions(bc);
    for (const auto &a : actions) {
        if (!a.isValidAction(bc)) continue;
        if (a.getActionType() == search::ActionType::SINGLE_CARD_SELECT) {
            out = {"select_card", a.getSelectIdx(), -1};
            return true;
        }

        // Multi-select: fill the picks ourselves rather than taking the enumerator's
        // offering.
        //
        // enumerateCardSelectActions only ever emits ONE multi action — the empty
        // selection (`Action(MULTI_CARD_SELECT, 0)`, whose bitmask has no bits set), with
        // the comment "just dont deal with this right now". Replaying that alone means
        // BattleContext::chooseExhaustCards never runs with a non-empty list, so Purity's
        // actual effect would have no oracle behind it at all.
        //
        // The enumerator is only a search helper; the oracle is BattleContext. So pick the
        // lowest `pickCount` hand indices, which isValidMultiCardSelectAction accepts and
        // Action::execute feeds straight into the real chooseExhaustCards.
        const int take = std::min(bc.cardSelectInfo.pickCount, bc.cards.cardsInHand);
        search::Action filled(a.bits);
        for (int i = 0; i < take; ++i) filled = search::Action(filled.bits | (1u << i));
        if (!filled.isValidAction(bc)) continue;

        // The trace format records the chosen indices verbatim in `idxs`.
        out = {"select_cards", -1, -1};
        out.idxs.clear();
        for (int i : filled.getSelectedIdxs()) out.idxs.push_back(i);
        return true;
    }
    return false;
}

// ---- when the policy is willing to drink (batch 45) ------------------------------------
//
// potionPolicy 0 is the historical behaviour and the reason this axis exists: `pickAction`
// offers potions BEFORE cards and never refuses, so all three are gone on turn 1, at FULL
// health, before a single card is played. Three whole families of behaviour are invisible
// under it and every one of them was measured, not guessed:
//
//   * heal amounts. Player::heal ends with `min(maxHp, ...)`, so Blood Potion's 40% and the
//     20% the reference gives Sacred Bark carriers collapse onto the same number. That is
//     exactly why TODOS' "血之药水 × 神圣树皮" ruling fails criterion ① (no divergence in the
//     data) rather than ③ — see the note there.
//   * anything gated on the player being hurt.
//   * "drink after the monsters have put block up" (batch 42's Hand Drill note).
//
// potionPolicy 1 = BLOODIED: hold every potion until the player is at or below half of maxHp.
//   * The predicate is `curHp * 2 <= maxHp`, deliberately the same integer form the reference
//     itself uses for "bloodied" (Player::heal's `wasBloodied`, Red Skull, Blood Vial).
//   * It cannot fire on turn 1: GameContext::initPlayer enters combat at full HP below asc 6.
//   * With Ironclad's maxHp 80 it guarantees curHp <= 40, so Blood Potion's as-built 32 lands
//     UNCLAMPED — the 40%/20% pair is separated by 16 HP rather than folded together.
static bool mayDrinkNow(const BattleContext &bc, int potionPolicy) {
    if (potionPolicy == 0) return true;
    return bc.player.curHp * 2 <= bc.player.maxHp;
}

static bool pickAction(const BattleContext &bc, int targetPolicy, int potionPolicy,
                       bool extendedPotions, Step &out) {
    const int target = policyTarget(bc, targetPolicy);
    // Drink before attacking, so potion buffs are visible in the same turn's card maths.
    for (int i = 0; mayDrinkNow(bc, potionPolicy) && i < bc.potionCapacity; ++i) {
        if (bc.potions[i] == Potion::EMPTY_POTION_SLOT) continue;
        // Entropic Brew refills slots with arbitrary potions, some of which open a
        // card-select screen the trace format cannot express. Only drink what
        // sts-combat has registered.
        if (!isReplayablePotion(bc.potions[i], extendedPotions)) continue;
        search::Action a(search::ActionType::POTION, i, target);
        if (a.isValidAction(bc)) { out = {"potion", i, target}; return true; }
    }
    for (int i = 0; i < bc.cards.cardsInHand; ++i) {
        if (!mayPlayHandCard(bc, i)) continue;
        search::Action a(search::ActionType::CARD, i, target);
        if (a.isValidAction(bc)) { out = {"card", i, target}; return true; }
    }
    out = {"end_turn", -1, -1};
    return false;
}

// Only potions sts-combat has registered; anything else would make the trace
// unreplayable rather than merely unverified.
static const std::vector<Potion> POTION_ROTATION {
    Potion::BLOCK_POTION,   Potion::FIRE_POTION,      Potion::STRENGTH_POTION,
    Potion::WEAK_POTION,    Potion::FEAR_POTION,      Potion::ENERGY_POTION,
    Potion::SWIFT_POTION,   Potion::DEXTERITY_POTION, Potion::BLOOD_POTION,
    Potion::ANCIENT_POTION, Potion::EXPLOSIVE_POTION, Potion::FRUIT_JUICE,
    Potion::ENTROPIC_BREW,
};

// ---------------------------------------------------------------- driver

struct Case {
    const char *seed;
    unsigned long long seedLong;
    int floor;
    MonsterEncounter encounter;
    const char *encounterName;
};

int main() {
    const std::vector<std::pair<MonsterEncounter, const char *>> encounters {
        {MonsterEncounter::CULTIST,         "CULTIST"},
        {MonsterEncounter::JAW_WORM,        "JAW_WORM"},
        {MonsterEncounter::JAW_WORM_HORDE,  "JAW_WORM_HORDE"},
        {MonsterEncounter::TWO_LOUSE,       "TWO_LOUSE"},
        {MonsterEncounter::THREE_LOUSE,     "THREE_LOUSE"},
        {MonsterEncounter::SMALL_SLIMES,    "SMALL_SLIMES"},
        {MonsterEncounter::LOTS_OF_SLIMES,  "LOTS_OF_SLIMES"},
        {MonsterEncounter::LARGE_SLIME,     "LARGE_SLIME"},
        {MonsterEncounter::BLUE_SLAVER,     "BLUE_SLAVER"},
        {MonsterEncounter::RED_SLAVER,      "RED_SLAVER"},
        {MonsterEncounter::GREMLIN_GANG,    "GREMLIN_GANG"},
        {MonsterEncounter::LOOTER,          "LOOTER"},
        {MonsterEncounter::EXORDIUM_THUGS,  "EXORDIUM_THUGS"},
        {MonsterEncounter::EXORDIUM_WILDLIFE, "EXORDIUM_WILDLIFE"},
        {MonsterEncounter::THREE_SENTRIES,  "THREE_SENTRIES"},
        {MonsterEncounter::GREMLIN_NOB,     "GREMLIN_NOB"},
        {MonsterEncounter::LAGAVULIN,       "LAGAVULIN"},
        {MonsterEncounter::THE_GUARDIAN,    "THE_GUARDIAN"},
        {MonsterEncounter::HEXAGHOST,       "HEXAGHOST"},
        {MonsterEncounter::SLIME_BOSS,      "SLIME_BOSS"},
    };

    struct SeedSpec { std::string name; unsigned long long value; };
    std::vector<SeedSpec> seeds {
        {"1RGBGHNF7L",   138414915365391ULL},
        {"SLAYTHESPIRE", 2665621045298406349ULL},
        {"0",            0ULL},
        {"3IX8N7ZPAA5",  9766940983340980ULL},
        {"NEOWLIVES",    52737824750267ULL},
    };
    // Spread deterministically over the 64-bit space so runs are not clustered.
    for (unsigned long long i = 0; i < 120; ++i) {
        const unsigned long long v = i * 6364136223846793005ULL + 1442695040888963407ULL;
        seeds.push_back({"GEN" + std::to_string(i), v});
    }
    const std::vector<int> floors {1, 3, 7};

    const int MAX_STEPS = 400;

    // ---- deck variants -------------------------------------------------------
    //
    // Every trace records the deck it was generated with, so the replayer replays
    // whatever each line states.
    //
    // Layout: variant 0 is FROZEN; variants 1/2 carry the current full deck and are
    // REGENERATED every batch. Variant 0 stays first and unchanged because its traceIdx
    // values (0..N) drive the relic/potion rotation — keeping it first reproduces its
    // committed lines byte-for-byte, which is what tools/regen-traces.sh checks.
    //
    // `upgradeAll` covers the other half of every card rule. The un-upgraded pass
    // alone never exercises the `up ? x : y` branch, which is exactly where a
    // transcription slip hides.
    struct DeckVariant {
        std::vector<CardId> extra;
        size_t seedLimit;   // first N seeds only, to bound file growth
        bool upgradeAll;
        // Which encounters this variant runs against. EMPTY MEANS ALL, so variants 0-8 keep
        // emitting exactly the traces they emitted before this field existed.
        //
        // Why per-variant filtering exists from batch 9 on: jaw_worm_horde.jsonl is already
        // 48MB and GitHub's hard per-file limit is 100MB. Three Jaw Worms make the longest
        // battles and therefore the fattest snapshots, while a FOCUSED variant's whole point
        // is to walk one batch's CARD branches — which barely depend on what is standing in
        // front of you. One single-monster encounter (Cultist) plus one multi-monster
        // encounter (Two Louse) covers the target-selection differences that do matter
        // (getRandomMonsterIdx, AttackAllEnemy, monsters dying mid-queue) at a fraction of
        // the bytes.
        std::vector<MonsterEncounter> encounters;
        // Ascension level handed to the GameContext constructor. DEFAULTS TO 0, so every
        // variant declared before this field existed keeps emitting byte-identical traces.
        //
        // Why this axis exists (batch 21): MonsterSpecific.cpp has 185 `ascension >= N`
        // conditions and every one of them was dead code while this was hardcoded to 0.
        // The engine repo picks 19 and only 19: all its conditions are of the form
        // `asc >= N` with N in {2,3,4,7,9,17,18,19}, so one run at 19 lights up every
        // branch's "high" side while the existing asc-0 corpus already covers the "low"
        // side. (What that does NOT prove is that the threshold is exactly N rather than
        // N±1 — that needs a *pair* of levels and is deliberately out of scope.)
        //
        // Ascension is not only a monster knob. GameContext::initPlayer gives asc>=10 an
        // extra ASCENDERS_BANE *before* the class's starting cards (so it sorts first in
        // the deck array), asc>=14 drops Ironclad maxHp 80 -> 75, and asc>=11 cuts
        // potionCapacity 3 -> 2 (the potion loop below reads bc.potionCapacity, so that
        // one follows automatically).
        int ascension = 0;
        // Which living monster the policy attacks. DEFAULTS TO 0 (= firstAliveMonster,
        // the historical behaviour), so every variant declared before this field existed
        // keeps emitting byte-identical traces — exactly the trick `ascension` uses.
        //
        // Why this axis exists (batch 31): see lastAliveMonster above. 0 = lowest-index
        // living monster, 1 = highest-index living monster.
        //
        // ⚠ It is a per-VARIANT knob rather than a per-trace one for the same reason
        // ascension is: it has to be constant for a whole file, because the file is what
        // the engine repo's tools freeze and compare.
        int targetPolicy = 0;

        // ---- explicit relic / potion loadout (batch 40) ---------------------------------
        //
        // EMPTY MEANS "use the rotation", so every variant declared before these fields
        // existed keeps emitting byte-identical traces — the same trick as `ascension`,
        // `targetPolicy`, `playerHp`, `deckUpgraded` and `halfDead`.
        //
        // ⚠⚠ WHY NOT JUST GROW `RELIC_ROTATION`. Both rotations index with the GLOBAL trace
        // counter modulo the table's own length:
        //     RELIC_ROTATION[(traceIdx*2 + k) % RELIC_ROTATION.size()]
        //     POTION_ROTATION[(traceIdx*3 + i) % POTION_ROTATION.size()]
        // so THE LENGTH IS THE MODULUS. Appending a 9th relic was measured, not reasoned
        // about: tools/regen-traces.sh --check then reported 116 / 116 files CHANGED, with
        // the very first line of the very first file already different (traceIdx 4 is the
        // first index where % 8 and % 9 disagree). Replacing an entry in place is no safer:
        // the modulus is unchanged but every trace that lands on that slot swaps relics.
        //
        // ⚠ The intuition "appending to the end of a list is free" is TRUE for the encounter
        // lists (a variant that does not name an encounter `continue`s BEFORE the seed loop
        // and therefore burns no traceIdx) and FALSE for these two tables. Do not mix them
        // up. RELIC_ROTATION and POTION_ROTATION are FROZEN from here on, at the same level
        // as act 1's `encounters` list and variant 0.
        //
        // A per-variant loadout is also simply more useful than a rotation: a relic batch
        // wants "this variant carries exactly Philosopher's Stone + Gremlin Horn + Hand
        // Drill", not "whatever the counter happened to deal out".
        std::vector<RelicSpec> relics;
        std::vector<Potion> potions;
        // Identifies the loadout in the FILE NAME (`<encounter>@relic<N>.jsonl`), emitted
        // into the trace header only when non-zero.
        //
        // ⚠⚠ THIS IS NOT COSMETIC. split-traces.mjs / variant0-rows.mjs fingerprint a
        // variant by (deck contents + upgrade bits + ascension + targetPolicy). Two variants
        // with the SAME deck and DIFFERENT relics would therefore collide: their rows would
        // be dropped into one file and treated as a single block, and variant0-rows.mjs
        // would silently report a LONGER frozen prefix. Adding relicSet to both the
        // fingerprint and the group key removes the collision by construction.
        //
        // ⚠ Give every explicit-loadout variant its OWN number. Sharing a number is only
        // safe under the usual rule (identical deck ⇒ disjoint encounter lists), and there
        // is no reason to take that risk here.
        int relicSet = 0;

        // ---- potion axis (batch 45) ------------------------------------------------------
        //
        // Both default to 0 and are emitted into the trace header only when non-zero, so every
        // variant declared before they existed keeps emitting byte-identical traces — the fifth
        // use of the `ascension` / `targetPolicy` / `relicSet` / `relicData` trick. Proved
        // rather than assumed: tools/regen-traces.sh --check reproduced all 163 committed files
        // byte-for-byte with these two fields (and the whole widened potion list) in place and
        // `potionVariants` still empty.
        //
        // `potionSet` is the IDENTITY, exactly like `relicSet`: it is the `@potN` file-name
        // suffix AND a fingerprint dimension in split-traces.mjs / variant0-rows.mjs, and it
        // additionally decides whether `isReplayablePotion` uses the widened list (see there
        // for why that cannot be a global switch).
        //
        // ⚠⚠ WHY TWO FIELDS AND NOT ONE. The obvious design — "the timing knob is also the
        // file name", the shape `targetPolicy`/`@tgtN` has — does not survive contact with the
        // second half of this line of work: covering 15 new potions needs SEVERAL variants that
        // all share one timing rule, and each of them needs its own file. So the identity and
        // the policy are separated, the same way `relicSet` is separate from the `relics` list
        // it names.
        int potionSet = 0;
        // 0 = drink everything on turn 1 at full health (historical), 1 = only once the player
        // is bloodied. See mayDrinkNow above for what each one is for.
        int potionPolicy = 0;
    };

    // Batch 1 (already registered, verified by the committed variant-0 traces).
    const std::vector<CardId> BATCH_1 {
        CardId::ANGER, CardId::CLEAVE, CardId::CLOTHESLINE, CardId::HEAVY_BLADE,
        CardId::IRON_WAVE, CardId::POMMEL_STRIKE, CardId::SHRUG_IT_OFF,
        CardId::THUNDERCLAP, CardId::TWIN_STRIKE, CardId::BODY_SLAM,
        CardId::INFLAME,
    };
    // Batch 2 — the 29 cards registered in CARD_RULES alongside BATCH_1.
    const std::vector<CardId> BATCH_2 {
        // attacks
        CardId::BITE, CardId::BLUDGEON, CardId::DROPKICK, CardId::FEED,
        CardId::FIEND_FIRE, CardId::FLASH_OF_STEEL, CardId::HEMOKINESIS,
        CardId::PUMMEL, CardId::REAPER, CardId::SEVER_SOUL,
        CardId::SWORD_BOOMERANG, CardId::SWIFT_STRIKE,
        // skills
        CardId::BANDAGE_UP, CardId::BLIND, CardId::BLOODLETTING, CardId::DEEP_BREATH,
        CardId::ENTRENCH, CardId::FINESSE, CardId::GOOD_INSTINCTS, CardId::IMPERVIOUS,
        CardId::INTIMIDATE, CardId::JAX, CardId::MASTER_OF_STRATEGY, CardId::OFFERING,
        CardId::PANACEA, CardId::SECOND_WIND, CardId::SHOCKWAVE, CardId::SPOT_WEAKNESS,
        // powers
        CardId::BERSERK,
    };

    // Batch 3 — the 12 cards the turn-boundary Power framework unlocked.
    const std::vector<CardId> BATCH_3 {
        CardId::UPPERCUT, CardId::BATTLE_TRANCE, CardId::DISARM, CardId::FLEX,
        CardId::IMPATIENCE, CardId::LIMIT_BREAK, CardId::SEEING_RED, CardId::TRIP,
        CardId::BARRICADE, CardId::COMBUST, CardId::DEMON_FORM, CardId::METALLICIZE,
    };

    // Batch 4 — the 10 cards the card-select screen unlocked.
    //
    // EXHUME appears TWICE on purpose. ExhumeAction filters the exhaust pile down to
    // non-Exhume cards, and Exhume exhausts itself — but only in OnAfterCardUsed, which
    // runs after ExhumeAction. With a single copy, no Exhume is ever in the pile while
    // ExhumeAction is looking at it, so that filter is unreachable and a transcription
    // slip in it would go unnoticed. A second copy makes it reachable.
    const std::vector<CardId> BATCH_4 {
        CardId::ARMAMENTS, CardId::BURNING_PACT, CardId::EXHUME, CardId::EXHUME,
        CardId::HEADBUTT, CardId::PURITY, CardId::SECRET_TECHNIQUE, CardId::SECRET_WEAPON,
        CardId::THINKING_AHEAD, CardId::TRUE_GRIT, CardId::WARCRY,
    };

    // Batch 5 — the card-lifecycle batch: exhaust triggers, status-card creation,
    // ethereal, innate placement.
    //
    // RECKLESS_CHARGE appears TWICE on purpose. It is the only registered source of DAZED,
    // and DAZED is both unplayable (so it *stays* in hand until end of turn) and ethereal
    // (so it is what exercises discardAtEndOfTurn's descending-index exhaust ordering).
    // One copy yields at most one Dazed per reshuffle cycle, which rarely puts two ethereal
    // cards in hand at the same turn end; two copies make that routine. It also doubles the
    // sample for MakeTempCardInDrawPile's cardRandomRng draw.
    //
    // MIND_BLAST is deliberately ABSENT from the FULL deck even though innate placement now
    // works. Its damage is the draw pile's size and it is innate, so with the 85-card deck it
    // would sit in every trace's opening hand and hit for ~80 — one-shotting Cultist (48-54)
    // and Jaw Worm (40-44) on turn 1 and collapsing 1230 traces from ~40 steps to 1. It lives
    // in the focused variant below instead, where the draw pile is ~18.
    const std::vector<CardId> BATCH_5 {
        // exhaust triggers
        CardId::DARK_EMBRACE, CardId::FEEL_NO_PAIN,
        // status-card creation (discard / draw pile / hand)
        CardId::IMMOLATE, CardId::RECKLESS_CHARGE, CardId::RECKLESS_CHARGE,
        CardId::WILD_STRIKE, CardId::POWER_THROUGH,
        // ethereal
        CardId::CARNAGE, CardId::GHOSTLY_ARMOR,
        // innate (DRAMATIC_ENTRANCE always, BRUTALITY only when upgraded)
        CardId::DRAMATIC_ENTRANCE, CardId::BRUTALITY,
        // triggers on drawing a status card
        CardId::EVOLVE,
    };

    // Batch 6 — the event-hook batch: powers that fire "when X happens" rather than on a
    // turn boundary.
    //
    //   FLAME_BARRIER  -> Player::attacked          (retaliate when attacked)
    //   FIRE_BREATHING -> CardManager::draw         (drew a Status/Curse card)
    //   RAGE           -> onUseAttackCard           (played an Attack)
    //   JUGGERNAUT     -> Player::gainBlock         (gained Block; consumes cardRandomRng)
    //   RUPTURE        -> Player::hpWasLost         (lost HP from a card)
    //   SENTINEL       -> triggerAndMoveToExhaustPile (was exhausted; sync gainEnergy)
    //   PANIC_BUTTON   -> calculateCardBlock        (NO_BLOCK gates card-sourced block)
    //
    // RUPTURE's oracle needs a self-damage source in the same battle. Batches 2/3/5 already
    // supply several (HEMOKINESIS, BLOODLETTING, JAX, OFFERING, COMBUST, plus BURN from
    // IMMOLATE), so no extra copies are needed for it.
    //
    // FIRE_BREATHING needs a Status/Curse card to be *drawn*, not merely created. WILD_STRIKE
    // (Wound) and RECKLESS_CHARGE x2 (Dazed) shuffle theirs into the DRAW pile, so they are
    // drawn as a matter of course; IMMOLATE's Burn goes to the discard pile and only reaches
    // hand after a reshuffle (see the variant 3/4 note above).
    //
    // FIRE_BREATHING appears TWICE. Measured, not assumed — and the measurement is worth
    // recording because it did NOT do what was expected:
    //   * 1 copy (92-card deck): played 47/29 times; deleting the status-draw hook failed
    //     7 replays, making its damage synchronous failed 1.
    //   * 2 copies (93-card deck): played 101/69 times; the same two mutations fail 5 and 1.
    // Doubling the copies doubled how often the power is UP, but not how often a Status card
    // is drawn WHILE it is up: more Fire Breathing damage ends battles sooner, which removes
    // the later turns that would have drawn one. The copy is kept because the card's own
    // coverage doubled (and the "played an Attack" mis-transcription guard went 61 -> 128),
    // but the status-draw hook stays thin at ~5 and is recorded as such in the engine repo's
    // TODOS.md. Do not "fix" it by adding a third copy; the limit is battle length.
    const std::vector<CardId> BATCH_6 {
        CardId::FLAME_BARRIER, CardId::FIRE_BREATHING, CardId::FIRE_BREATHING, CardId::RAGE,
        CardId::JUGGERNAUT, CardId::RUPTURE, CardId::SENTINEL, CardId::PANIC_BUTTON,
    };

    // Batch 7 — the per-card-instance state batch (CardInstance::cost / costForTurn /
    // specialData).
    //
    //   RAMPAGE         -> specialData grows +5/+8 per play
    //   SEARING_BLOW    -> damage is n(n+7)/2+12 where n = specialData (upgrade count)
    //   BLOOD_FOR_BLOOD -> CardManager::onTookDamage lowers this instance's cost by 1
    //   MADNESS         -> zeroes a random hand card's cost (rejection-sampling loop)
    //   CORRUPTION      -> all Skills cost 0 and exhaust on play
    //   APPARITION      -> Intangible 1, and is ethereal only while un-upgraded
    //
    // This batch does NOT go into the full deck (variants 1/2) — see the variant list below.
    const std::vector<CardId> BATCH_7 {
        CardId::RAMPAGE, CardId::SEARING_BLOW, CardId::BLOOD_FOR_BLOOD,
        CardId::MADNESS, CardId::CORRUPTION, CardId::APPARITION,
    };

    // Batch 8 — the random-card-pool batch. All five pick a card DEFINITION out of a combat
    // card pool (CardPools.h) and therefore consume cardRandomRng:
    //
    //   CHRYSALIS          -> PutRandomCardsInDrawPile(SKILL,  up?5:3), cost 0 for the COMBAT
    //   METAMORPHOSIS      -> PutRandomCardsInDrawPile(ATTACK, up?5:3), cost 0 for the COMBAT
    //   DISCOVERY          -> DiscoveryAction(INVALID, 1): a 3-card select screen, cost 0 this TURN
    //   JACK_OF_ALL_TRADES -> 1 (2) random colorless cards to hand, at FULL cost
    //   INFERNAL_BLADE     -> 1 random Attack to hand, cost 0 this TURN
    //
    // This is the batch that finally makes CardInstance::cost and ::costForTurn diverge — see
    // the isReplayableCard note above for why the policy needs a gate now.
    const std::vector<CardId> BATCH_8 {
        CardId::CHRYSALIS, CardId::METAMORPHOSIS, CardId::DISCOVERY,
        CardId::JACK_OF_ALL_TRADES, CardId::INFERNAL_BLADE,
    };

    // Batch 9 — the play-from-pile / play-a-copy batch. All four make the CARD QUEUE nest:
    // a card being used pushes ANOTHER card-queue item, so useCard re-enters.
    //
    //   HAVOC       -> PlayTopCard(getRandomMonsterIdx(cardRandomRng), exhausts=true)
    //   MAYHEM      -> BuffPlayer<MAYHEM>: same PlayTopCard, exhausts=false, every turn START
    //   DOUBLE_TAP  -> BuffPlayer<DOUBLE_TAP>: onUseAttackCard queuePurgeCard's a COPY
    //   DUAL_WIELD  -> DualWieldAction: copies an Attack/Power in hand (card-select screen)
    //
    // Copies are deliberate, and each one targets a branch a single copy cannot reach:
    //   HAVOC x2       cheap (1 / upgraded 0), and more plays means more chances to hit the
    //                  empty-draw-pile branch (addToTop EmptyDeckShuffle then re-run).
    //   MAYHEM x2      the ONLY way to stack the power to 2, which is in turn the only way to
    //                  (a) have two PlayTopCard actions queued in one turn, so the second one
    //                  pushes onto a NON-EMPTY card queue, and (b) reach
    //                  addPurgeCardToCardQueue's `size > 0` branch, where the purge copy lands
    //                  SECOND rather than first.
    //   DOUBLE_TAP x2  1-cost; two copies make "double tap is up" common enough that the
    //                  attacks below actually get doubled.
    //   DUAL_WIELD x2  separates the two DualWieldAction branches: `validCount == 1` copies
    //                  straight away (no hand reorder, no new uid on the original) while
    //                  `>= 2` opens the screen. One copy alone leaves the shortcut thin.
    const std::vector<CardId> BATCH_9 {
        CardId::HAVOC, CardId::HAVOC,
        CardId::MAYHEM, CardId::MAYHEM,
        CardId::DOUBLE_TAP, CardId::DOUBLE_TAP,
        CardId::DUAL_WIELD, CardId::DUAL_WIELD,
    };

    // Batch 10 — the X-cost batch. WHIRLWIND and TRANSMUTATION are the two Ironclad/colorless
    // X-cost cards the reference implements; APOTHEOSIS is not X-cost but is registered in the
    // same batch.
    //
    //   WHIRLWIND      -> WhirlwindAction(base + Vigor, item.energyOnUse, !freeToPlay):
    //                     one damage matrix, then AttackAllMonsterRecursive X times
    //   TRANSMUTATION  -> TransmutationAction(upgraded, item.energyOnUse, !freeToPlay):
    //                     X random COLOURLESS cards to hand at cost 0 FOR THE TURN, and
    //                     upgraded when the Transmutation itself is
    //   APOTHEOSIS     -> ApotheosisAction: upgrade every canUpgrade() card in hand, draw pile,
    //                     discard pile AND exhaust pile
    //
    // Copies:
    //   WHIRLWIND x1    it does NOT exhaust, so one copy recycles and gets played many times
    //                   per battle. A second copy would only starve the other two of energy:
    //                   the policy plays the LEFTMOST playable card and Whirlwind is playable
    //                   at 0 energy (costForTurn is the -1 X sentinel), so every Whirlwind in
    //                   hand drains the turn's energy to 0 before anything to its right runs.
    //   TRANSMUTATION x2 / APOTHEOSIS x2
    //                   both EXHAUST, so one copy is at most one play per battle. Two copies
    //                   double the sample; for Apotheosis the second copy is not redundant —
    //                   after the first one everything is upgraded, so it exercises
    //                   canUpgrade() returning false, plus SEARING_BLOW (below) which is the
    //                   one card canUpgrade() keeps saying yes to.
    //
    // ⚠ THE BATCH IS SPLIT ACROSS TWO VARIANT PAIRS, and the split is forced, not stylistic:
    // TRANSMUTATION conjures out of the colourless pool, 9 of whose 34 cards are unregistered,
    // and MAYHEM plays the draw pile's top card every turn for the rest of the battle with no
    // gate the harness can apply. Put them in one deck and the conjured junk eventually reaches
    // the draw pile top and Mayhem plays it, making the trace unreplayable. So Mayhem lives with
    // Whirlwind/Apotheosis (a pool-free deck) and Transmutation gets its own pair without it.
    // Havoc is fine in both because it is instantaneous and mayPlayHandCard can check the two
    // piles it will read.
    const std::vector<CardId> BATCH_10_WHIRLWIND {
        CardId::WHIRLWIND,
        CardId::APOTHEOSIS, CardId::APOTHEOSIS,
    };
    const std::vector<CardId> BATCH_10_TRANSMUTATION {
        CardId::TRANSMUTATION, CardId::TRANSMUTATION,
    };

    // Batch 11 — four unrelated small mechanisms, one per card:
    //
    //   PERFECTED_STRIKE -> damage is 6 + cards.strikeCount * (up ? 3 : 2), where strikeCount
    //                       is an INCREMENTAL counter maintained by
    //                       CardManager::notifyAddCardToCombat / notifyRemoveFromCombat
    //   CLASH            -> CardInstance::canUse's ATTACK arm: canUseClash requires the whole
    //                       hand to be Attacks
    //   HAND_OF_GREED    -> Monster::damage (not attacked), and Player::gainGold on a kill
    //   THE_BOMB         -> Player::bomb1/2/3, a three-slot timer resolved at the TOP of
    //                       Player::applyEndOfTurnPowers, before the statusMap loop
    //
    // ⚠ SPLIT ACROSS TWO VARIANT PAIRS, and the split is forced by DAMAGE, not by
    // replayability (unlike batch 10's). The Bomb only detonates at the END of the third
    // player turn after it is played, so its oracle needs battles that LAST that long — while
    // Perfected Strike is the single hardest-hitting card in the Ironclad pool once the five
    // starter Strikes are counted (6 + 5*2 = 16 before any other Strike joins). Put them in one
    // deck and Cultist (48-54 HP) dies on turn 2 or 3, and `if (bomb1)` never fires: every
    // shift is observable only through the DamageAllEnemy that follows it. So the Bomb gets a
    // deliberately damage-POOR deck of its own.
    const std::vector<CardId> BATCH_11_STRIKE {
        CardId::PERFECTED_STRIKE, CardId::PERFECTED_STRIKE,
        CardId::CLASH, CardId::CLASH,
        CardId::HAND_OF_GREED, CardId::HAND_OF_GREED,
    };
    const std::vector<CardId> BATCH_11_BOMB {
        CardId::THE_BOMB, CardId::THE_BOMB,
    };

    // Batch 12 — the last two Ironclad/colourless cards the reference can be an oracle for.
    // Both needed a reference-side bug fixed first (see the fix commit that precedes this one):
    //
    //   DARK_SHACKLES -> DebuffEnemy<STRENGTH>(-(up?15:9)) plus, when the target has NO
    //                    Artifact, BuffEnemy<SHACKLED>(up?15:9). The Strength comes back in
    //                    Monster::applyEndOfTurnTriggers (Monster.cpp:63), a whole time step
    //                    this repo had never exercised: BattleContext::applyEndOfRoundPowers
    //                    runs it in a FIRST monster loop, before player.applyAtEndOfRoundPowers
    //                    and before the SECOND monster loop that holds Ritual / Weak / Vuln.
    //   VIOLENCE      -> ViolenceAction: scan the draw pile for Attacks (one cardRandomRng roll
    //                    per attack after the first, and the roll RESULT is used — it picks the
    //                    insert position), then per fetched card shuffle the list's [i, end)
    //                    tail with java::Random(shuffleRng.randomLong()) and take list[i].
    const std::vector<CardId> BATCH_12 {
        CardId::DARK_SHACKLES, CardId::DARK_SHACKLES, CardId::DARK_SHACKLES,
        CardId::VIOLENCE, CardId::VIOLENCE, CardId::VIOLENCE,
    };

    // ---- DECK CAP: Deck::MAX_SIZE (96), not CardManager::MAX_GROUP_SIZE (64) ------
    //
    // The three combat piles are NOT the constraint, even though CardManager::init does
    // `drawPile.resize(gc.deck.size())`. drawPile / discardPile / exhaustPile are
    // fixed_list<CardInstance, MAX_GROUP_SIZE=64> only under
    // `#ifdef sts_card_manager_use_fixed_list` (CardManager.h:34) — and sts_common.h:12 has
    // that #define COMMENTED OUT, with no -D anywhere in the build line above. In this build
    // they are plain std::vector and grow freely.
    //
    // What does cap the deck is the master deck itself: Deck::cards is
    // fixed_list<Card, Deck::MAX_SIZE=96> (Deck.h:26-28), and fixed_list has NO bounds
    // checking whatsoever (`void push_back(T t) { arr[list_size++] = t; }`,
    // `void resize(int size) { list_size = size; }`), so a 97th card is silent memory
    // corruption rather than an assert. CardManager::init's `fixed_list<int, Deck::MAX_SIZE>
    // idxs` and `bool isInnateMemo[Deck::MAX_SIZE]` are sized off the same constant.
    //
    // One reachable MAX_GROUP_SIZE(64) fixed_list does survive, and it is not a pile:
    // ViolenceAction's `attackIdxList` (Actions.cpp:616) collects the draw pile's ATTACK
    // cards, so it needs 65+ attacks *in the draw pile* to overflow — count attacks, not
    // deck size, when VIOLENCE finally gets registered. (UpgradeRandomCardAction's
    // `upgradeableHandIdxs` at Actions.cpp:942 is fixed_list<int,10> and the hand caps at
    // 10, so that one is safe by construction.)
    //
    // Variant 0 MUST stay first and unchanged — its traceIdx values (0..N) drive the
    // relic/potion rotation, so keeping it first reproduces its committed lines
    // byte-for-byte.
    std::vector<DeckVariant> variants { {BATCH_1, seeds.size(), false} };
    {
        // Variants 1/2 carry the CURRENT FULL DECK (10 starter + batches 1-6 = 93 cards).
        // Each new batch REPLACES this pair instead of appending another one: a pair costs
        // ~12MB, so appending would put the repo past 100MB within a few batches. The deck
        // being a superset of every registered card is also what makes one pair enough.
        //
        // The bigger deck's one measured cost: DrawToHandAction's "exactly one candidate,
        // skip the screen" shortcut went from 20 mutation failures to 0, because a 73-card
        // draw pile practically always holds >= 2 skills / attacks. Every registered card is
        // still played in both branches (tools/check-coverage.mjs). Recorded as a blind spot
        // in the engine repo's TODOS.md.
        std::vector<CardId> full = BATCH_1;
        full.insert(full.end(), BATCH_2.begin(), BATCH_2.end());
        full.insert(full.end(), BATCH_3.begin(), BATCH_3.end());
        full.insert(full.end(), BATCH_4.begin(), BATCH_4.end());
        full.insert(full.end(), BATCH_5.begin(), BATCH_5.end());
        full.insert(full.end(), BATCH_6.begin(), BATCH_6.end());
        variants.push_back({full, 40, false});
        variants.push_back({full, 40, true});

        // Variants 3/4: a FOCUSED small deck (10 starter + batch 5 + MIND_BLAST = 23 cards).
        //
        // Two branches of batch 5 are simply unreachable at 85 cards, and both come down to
        // the draw pile never cycling in a single battle:
        //
        //  * BURN's end-of-turn self-damage. Immolate drops Burn in the DISCARD pile, so it
        //    only reaches hand after a reshuffle. Measured on the 85-card data: Burn appears
        //    296 times in the discard pile, 44 in the draw pile, and 0 times in hand — the
        //    whole useNoTriggerCard path had zero oracle behind it.
        //  * MIND_BLAST, whose damage scales with the draw pile (see above).
        //
        // At 23 cards a reshuffle happens every few turns and Mind Blast hits for ~18. This is
        // the "覆盖密度" escape hatch the engine repo's WORKFLOW describes: variants 1/2 stay
        // the current full deck (they are what proves cards coexist), and a focused pair is
        // added only when the full deck makes a branch unreachable.
        std::vector<CardId> focused = BATCH_5;
        focused.push_back(CardId::MIND_BLAST);
        variants.push_back({focused, 40, false});
        variants.push_back({focused, 40, true});

        // Variants 5/6: a FOCUSED deck for batch 7 (10 starter + batch 7 + 8 enablers = 24).
        //
        // Why batch 7 is NOT folded into the full deck: it no longer fits. The full deck is
        // 10 starter + batches 1-6 = 93 cards, and Deck::MAX_SIZE is 96 (see the DECK CAP note
        // above) — 93 + 6 = 99. The cap is a hard structural limit, not a budget choice, and
        // fixed_list has no bounds checking, so overflowing it is silent memory corruption.
        // Shrinking the full deck to fit would mean dropping the deliberate duplicate copies
        // (EXHUME x2, RECKLESS_CHARGE x2, FIRE_BREATHING x2), each of which was added to reach
        // a specific branch, and would still leave zero headroom for batch 8. Leaving
        // variants 0-4 untouched instead keeps every mutation-test figure already measured on
        // them valid, and gives batch 7 far better coverage density than 96 cards would.
        //
        // The enablers are picked to reach specific new code, not to pad the deck. The
        // ENTRENCH pair and the second CORRUPTION / APPARITION were added after a first
        // mutation-test pass came back 0 on the branches noted below.
        //   ARMAMENTS      the only in-combat upgrade source. Drives SEARING_BLOW's
        //                  specialData past 1, and is the only way to reach
        //                  CardInstance::upgrade's tail while CORRUPTION is up. Measured on
        //                  the first pass: 618 plays, 206 of them with Corruption already up.
        //   ENTRENCH x2    ENTRENCH is the ONLY Skill in the whole card table whose upgraded
        //                  energy cost both differs from its base cost AND is non-zero
        //                  (`upgraded ? 1 : 2`). onBuffCorruption zeroes every Skill already
        //                  in a pile, so the only way a Skill can have costForTurn > 0 while
        //                  Corruption is up is to upgrade one afterwards — which makes
        //                  ENTRENCH the sole oracle for useCard's `!(corruption && skill)`
        //                  energy clause, for CardManager::resetAttributesAtEndOfTurn, and
        //                  for MadnessAction's costForTurn-vs-cost branch split. Two copies
        //                  because the chain needs Armaments AND Entrench in the same hand
        //                  with Corruption already up.
        //   BLOODLETTING   self damage on demand, so BLOOD_FOR_BLOOD's cost walks down from 4
        //                  early rather than only after monster hits. It is itself 0-cost,
        //                  which also drives onBuffCorruption's `cost > 0` filter false.
        //   IMMOLATE       puts Burn in the discard pile; a reshuffle brings it to hand within
        //                  a few turns, so Player::damage's Intangible clamp (a different code
        //                  path from Monster::calculateDamageToPlayer's) can be reached while
        //                  APPARITION is up.
        //   SHRUG_IT_OFF   a plain 1-cost Skill, so CORRUPTION has something to zero besides
        //                  the starter Defends.
        //   CORRUPTION #2  Corruption is a bool status (setHasStatus, never in statusMap), so
        //                  playing a second one must NOT stack to 2. With one copy that is
        //                  unobservable — 391 of 1200 traces played it, all exactly once.
        //   APPARITION #2  Intangible was up for zero of the 207 frames that had a Burn in
        //                  hand, so Player::damage's clamp had no oracle. More Apparition
        //                  plays is the only lever on that conjunction.
        std::vector<CardId> batch7 = BATCH_7;
        batch7.push_back(CardId::ARMAMENTS);
        batch7.push_back(CardId::BLOODLETTING);
        batch7.push_back(CardId::IMMOLATE);
        batch7.push_back(CardId::SHRUG_IT_OFF);
        batch7.push_back(CardId::ENTRENCH);
        batch7.push_back(CardId::ENTRENCH);
        batch7.push_back(CardId::CORRUPTION);
        batch7.push_back(CardId::APPARITION);
        variants.push_back({batch7, 40, false});
        variants.push_back({batch7, 40, true});

        // Variants 7/8: a FOCUSED deck for batch 8 (10 starter + batch 8's 5 + 3 enablers = 18 cards).
        //
        // Same reason batch 7 got its own pair: the full deck is 93 cards and Deck::MAX_SIZE is
        // 96, so batch 8 does not fit there either. Variants 0-6 stay untouched, which keeps
        // every mutation figure already measured on them valid.
        //
        // A SMALL deck matters more here than in any previous batch. Chrysalis and Metamorphosis
        // inject 3 (5) extra cards into the draw pile per play, so a large deck would both bury
        // the batch's own cards and make each snapshot enormous. At 18 cards the draw pile
        // cycles every couple of turns, which is also what makes end-of-turn cost RESET
        // observable: a card whose costForTurn was zeroed for one turn has to come back around.
        //
        // The extra copies and the one enabler are picked to reach specific new code:
        //   INFERNAL_BLADE #2   the sole producer of "costForTurn == 0 while cost > 0" on a card
        //                       that is neither a Skill nor deck-owned. It is what separates
        //                       playCard-reads-costForTurn from playCard-reads-cost, and what
        //                       gives CardManager::resetAttributesAtEndOfTurn something to do —
        //                       both were 0-example blind spots after batch 7.
        //   JACK_OF_ALL_TRADES #2  the only card here that adds a card to hand at FULL cost, via
        //                       moveToHandHelper. That is the exact path batch 7 recorded as a
        //                       blind spot ("Corruption's moveToHandHelper hook has nothing to
        //                       do until cards are conjured after Corruption lands"). ~6 of the
        //                       34 colorless cards are Skills costing >= 1, so two copies make
        //                       the conjunction (Corruption up AND a costly Skill conjured)
        //                       routine rather than rare.
        //   CORRUPTION          the other half of that conjunction. It also re-covers useCard's
        //                       `!(corruption && skill)` energy clause, which batch 7 measured
        //                       at only 2 examples — all five batch-8 cards are Skills.
        //
        // Deliberately NOT added: a second CHRYSALIS / METAMORPHOSIS. Their coverage is already
        // high with one copy, and each extra play grows every subsequent snapshot in the trace.
        std::vector<CardId> batch8 = BATCH_8;
        batch8.push_back(CardId::INFERNAL_BLADE);
        batch8.push_back(CardId::JACK_OF_ALL_TRADES);
        batch8.push_back(CardId::CORRUPTION);
        variants.push_back({batch8, 40, false});
        variants.push_back({batch8, 40, true});

        // Variants 9/10: a FOCUSED deck for batch 9 (10 starter + batch 9's 8 + 4 enablers = 22).
        //
        // Same structural reason as batches 7 and 8: the full deck is 93 cards and
        // Deck::MAX_SIZE is 96. Variants 0-8 stay untouched.
        //
        // FIRST VARIANT PAIR WITH AN ENCOUNTER FILTER (Cultist + Two Louse only). See the
        // DeckVariant::encounters comment: jaw_worm_horde.jsonl is at 48MB of a 100MB hard
        // limit, three Jaw Worms give the longest battles and fattest frames, and a focused
        // variant's value lies in card branches, not in which monster is standing there.
        //
        // The four enablers each unlock code the batch's own cards cannot reach alone:
        //   RAMPAGE          the only registered attack carrying per-instance state
        //                    (specialData +5 per play). It is what makes two otherwise
        //                    invisible transcriptions observable through damage numbers:
        //                    (a) queuePurgeCard copies the instance BY VALUE, so a double-tapped
        //                        Rampage hits for +10 on the second swing but the card that
        //                        lands in the discard pile has only grown by +5;
        //                    (b) chooseDualWieldCard copies the whole instance rather than
        //                        rebuilding a prototype, so a Rampage copy starts at the
        //                        original's current growth.
        //   INFLAME          a POWER, and the only non-Attack thing Dual Wield may copy here.
        //                    Without it isDualWieldable's POWER arm is dead code and
        //                    "Dual Wield an already-played Power" never happens.
        //   RECKLESS_CHARGE  shuffles DAZED into the DRAW pile. Dazed is a Status card, so when
        //                    Havoc/Mayhem turn up one on top, CardInstance::canUse rejects it
        //                    and the card is DESTROYED (it left the draw pile inside
        //                    playTopCardInDrawPile and lives only in the queue item). Nothing
        //                    else in this deck can put an unplayable card on top of the draw pile.
        //   HEADBUTT         an ATTACK that opens a card-select screen. Double Tap + Headbutt is
        //                    the only combination here that leaves an item SITTING IN THE CARD
        //                    QUEUE while a screen is open — the state the engine side had to
        //                    start serialising this batch. It also gives Havoc/Mayhem a card
        //                    whose autoplay opens a screen.
        std::vector<CardId> batch9 = BATCH_9;
        batch9.push_back(CardId::RAMPAGE);
        batch9.push_back(CardId::INFLAME);
        batch9.push_back(CardId::RECKLESS_CHARGE);
        batch9.push_back(CardId::HEADBUTT);
        const std::vector<MonsterEncounter> batch9Encounters {
            MonsterEncounter::CULTIST, MonsterEncounter::TWO_LOUSE,
        };
        variants.push_back({batch9, 40, false, batch9Encounters});
        variants.push_back({batch9, 40, true,  batch9Encounters});

        // Encounter filter for both batch-10 pairs (Cultist + Two Louse only), same as batch 9 —
        // jaw_worm_horde.jsonl is at 48MB of a 100MB hard limit. Two Louse is what gives
        // Whirlwind more than one target; Cultist is the single-monster control.
        const std::vector<MonsterEncounter> batch10Encounters {
            MonsterEncounter::CULTIST, MonsterEncounter::TWO_LOUSE,
        };

        // Variants 11/12: WHIRLWIND + APOTHEOSIS (10 starter + 3 + 11 enablers = 24 cards).
        // Same structural reason as batches 7-9 for having its own pair: the full deck is 93
        // cards and Deck::MAX_SIZE is 96. Variants 0-10 stay untouched.
        //
        // POOL-FREE BY CONSTRUCTION — no card here conjures out of CardPools.h — which is what
        // lets MAYHEM be in the deck at all (see mayPlayHandCard).
        //
        // The eleven enablers each unlock code the batch's own cards cannot reach alone. X-cost
        // is not one rule but a small lattice of them, and most cells need a second card:
        //   HAVOC x2         the ONLY way to reach `freeToPlay == true`, i.e.
        //                    `useEnergy = !(item.freeToPlay || c.freeToPlayOnce)` being FALSE.
        //                    Havoc plays the draw pile's top card with
        //                    energyOnUse = player.energy AND freeToPlay = true, so a Havoc'd
        //                    Whirlwind hits X times for FREE. Without it that clause is dead and
        //                    mis-transcribing it (always spend / never spend) is invisible.
        //   MAYHEM x2        the only way to stack MAYHEM to 2, which is in turn the only way to
        //                    have TWO card-queue items pending at once — and therefore the only
        //                    way anything can change player.energy BETWEEN a card being queued
        //                    and being used. That is what makes the asymmetry between the two
        //                    X-cost cards observable: Transmutation RAISES energyOnUse when
        //                    energy grew (`player.energy > item.energyOnUse`) and Whirlwind has
        //                    no such line, so mutating Whirlwind to add one should show up here.
        //   BLOODLETTING x2  the energy source for exactly that. 0-cost, does NOT exhaust (so it
        //                    recycles and Mayhem can turn it up repeatedly), and gains 2 (3)
        //                    energy unconditionally. SEEING_RED was the obvious alternative and
        //                    is worse: it exhausts, so it can be turned up at most once per
        //                    battle. It also widens the X distribution (energy 5, not 3).
        //   DOUBLE_TAP x2    Whirlwind is an ATTACK, so Double Tap queuePurgeCard's a copy whose
        //                    energyOnUse is INHERITED and whose ignoreEnergyTotal is TRUE. By
        //                    then the first swing has spent all energy, so dropping
        //                    ignoreEnergyTotal would clamp X to 0 and the copy would deal
        //                    nothing — this is the only oracle for that flag.
        //   SEARING_BLOW     the one card CardInstance::canUpgrade() keeps returning true for.
        //                    Apotheosis therefore bumps its upgrade COUNT (specialData) every
        //                    time, and its damage formula n(n+7)/2+12 makes that visible. Also
        //                    the reason the second Apotheosis is not a no-op.
        //   ENTRENCH         the only Skill whose upgraded energy cost both differs from its
        //                    base cost and is non-zero (upgraded ? 1 : 2), so Apotheosis
        //                    upgrading one in a pile is what exercises CardInstance::upgrade's
        //                    tail ("cost and costForTurn both move when getEnergyCost changes").
        //   WILD_STRIKE      shuffles WOUND into the DRAW pile. Two uses: Apotheosis must SKIP
        //                    status cards (canUpgrade excludes STATUS/CURSE), and Havoc/Mayhem
        //                    turning up a Wound exercises the canUse rejection path.
        //
        // Deliberately NOT added: a Vigor source (nothing registered has one, and the reference
        // adds Vigor to Whirlwind's base damage *and* again inside calculateCardDamage), and
        // CHEMICAL_X (relic, unregistered — its `+2` is a TODO on both sides).
        std::vector<CardId> batch10a = BATCH_10_WHIRLWIND;
        batch10a.push_back(CardId::HAVOC);
        batch10a.push_back(CardId::HAVOC);
        batch10a.push_back(CardId::MAYHEM);
        batch10a.push_back(CardId::MAYHEM);
        batch10a.push_back(CardId::BLOODLETTING);
        batch10a.push_back(CardId::BLOODLETTING);
        batch10a.push_back(CardId::DOUBLE_TAP);
        batch10a.push_back(CardId::DOUBLE_TAP);
        batch10a.push_back(CardId::SEARING_BLOW);
        batch10a.push_back(CardId::ENTRENCH);
        batch10a.push_back(CardId::WILD_STRIKE);
        variants.push_back({batch10a, 40, false, batch10Encounters});
        variants.push_back({batch10a, 40, true,  batch10Encounters});

        // Variants 13/14: TRANSMUTATION (10 starter + 2 + 6 enablers = 18 cards). No MAYHEM
        // here — see the BATCH_10 comment for why that pairing cannot be made replayable.
        //
        // Small on purpose, for the same reason batch 8's pair was: Transmutation dumps X cards
        // into hand per play, so a large deck would bury them, and a fast-cycling draw pile is
        // what makes END-OF-TURN COST RESET observable (a card zeroed for one turn has to come
        // back around).
        //   HAVOC x2       the only producer of `freeToPlay` for Transmutation specifically —
        //                  a Havoc'd Transmutation conjures X cards and spends NO energy.
        //                  Safe alongside Transmutation because mayPlayHandCard can check the
        //                  draw and discard piles Havoc is about to read.
        //   BLOODLETTING x2 0-cost, +2 (3) energy, no exhaust. Two jobs: it widens X (up to 5),
        //                  and with hand already at 5 an X of 5 overflows the 10-card hand,
        //                  which is the only way to reach moveToHandHelper's "hand full → put it
        //                  in the discard pile instead" arm from Transmutation.
        //   CORRUPTION     conjured colourless SKILLS arrive through moveToHandHelper, so
        //                  Corruption's hook there applies to them. Also re-covers useCard's
        //                  `!(corruption && skill)` energy clause (still only 2 examples).
        //   ENTRENCH       a Skill whose upgraded cost differs and is non-zero, so the conjured
        //                  cards are not the only thing with a non-trivial cost in the deck.
        std::vector<CardId> batch10b = BATCH_10_TRANSMUTATION;
        batch10b.push_back(CardId::HAVOC);
        batch10b.push_back(CardId::HAVOC);
        batch10b.push_back(CardId::BLOODLETTING);
        batch10b.push_back(CardId::BLOODLETTING);
        batch10b.push_back(CardId::CORRUPTION);
        batch10b.push_back(CardId::ENTRENCH);
        variants.push_back({batch10b, 40, false, batch10Encounters});
        variants.push_back({batch10b, 40, true,  batch10Encounters});

        // Encounter filter for both batch-11 pairs (Cultist + Two Louse only), same as
        // batches 9 and 10 — jaw_worm_horde.jsonl is frozen at 46MB of a 100MB hard limit.
        // Cultist is the single-monster control and, at 48-54 HP, the LONGEST of the two,
        // which is what The Bomb's three-turn timer needs; Two Louse is what gives
        // Hand of Greed something cheap enough (10-15 HP) to actually KILL, which is the
        // only way its gold branch fires.
        const std::vector<MonsterEncounter> batch11Encounters {
            MonsterEncounter::CULTIST, MonsterEncounter::TWO_LOUSE,
        };

        // Variants 15/16: PERFECTED_STRIKE + CLASH + HAND_OF_GREED
        // (10 starter + 6 + 10 enablers = 26 cards). Structural reason for its own pair is the
        // same as batches 7-10: the full deck is 93 and Deck::MAX_SIZE is 96.
        //
        // The enablers exist to make strikeCount MOVE. Its two hooks live at very specific
        // places, and a deck that only ever gains cards would leave half of them dead:
        //   FIEND_FIRE x2    exhausts the ENTIRE hand, so it is the heavy lever on
        //                    notifyRemoveFromCombat (moveToExhaustPile is the counter's ONLY
        //                    decrement). Five starter Strikes plus Perfected Strike itself
        //                    means a Fiend Fire routinely drops strikeCount by 2-3 at once,
        //                    and the very next Perfected Strike hits for visibly less.
        //                    Two copies because it exhausts itself (one play per copy per
        //                    battle), and its own damage scales with the cards exhausted.
        //   EXHUME           the ONLY path that puts a card BACK into combat
        //                    (chooseExhumeCard calls notifyAddCardToCombat explicitly after
        //                    removeFromExhaustPile). Fiend Fire fills the exhaust pile with
        //                    Strikes for it to fish out. One copy is enough — batch 4 already
        //                    covers the "candidates exclude Exhume itself" filter with two.
        //   DUAL_WIELD       the only registered card that copies an Attack in HAND via
        //                    createTempCardInHand, i.e. an increment that needs no card pool.
        //                    Copying a Strike / Perfected Strike bumps the counter.
        //   WILD_STRIKE      two jobs: it IS a Strike card (isCardStrikeCard lists it), and it
        //                    shuffles a WOUND into the draw pile. The Wound is what makes
        //                    canUseClash return FALSE while Clash sits in the same hand — a
        //                    Status card cannot be played, so it STAYS in hand all turn and
        //                    blocks Clash for that whole turn. Without an unplayable card in
        //                    the deck the reject arm of the new gate is far rarer.
        //   SWIFT_STRIKE     a 0-cost Strike card. Cheap counter fodder, and being 0-cost it
        //                    never competes for the energy the batch's own cards want.
        //   TRUE_GRIT x2     ADDED AFTER MEASURING, and the measurement is worth recording
        //                    because FIEND_FIRE turned out to be the wrong tool for the
        //                    decrement half. With Fiend Fire as the only strike-exhauster,
        //                    Perfected Strike was played after a strike had left combat only
        //                    15 times out of 125 plays, and deleting notifyRemoveFromCombat
        //                    failed a mere 2 replays. The reason is structural: Fiend Fire
        //                    exhausts the ENTIRE hand, which includes the Perfected Strike
        //                    that would have observed the drop. True Grit exhausts exactly ONE
        //                    card (random un-upgraded, chosen when upgraded) for 1 energy, so
        //                    the counter falls while a Perfected Strike is still sitting in
        //                    hand. It also front-loads the exhaust pile with Strikes on turn 1,
        //                    which is what finally lets EXHUME pull one back out —
        //                    exhumePulledStrike was 0 for 154 Exhume plays before this.
        //                    ⚠ Its own self-exhaust happens in OnAfterCardUsed, i.e. AFTER the
        //                    ExhaustRandomCardInHand it queues, so the hand card really is
        //                    exhaust-pile index 0 and the harness's lowest-index pick finds it.
        //   HAVOC x2         the autoplay path. playTopCardInDrawPile hands the choice to the
        //                    reference, so a Clash on top of the draw pile is run through
        //                    canUse with inAutoplay = true — and when the hand is not all
        //                    Attacks the card is DESTROYED (it already left the draw pile).
        //                    Nothing policy-side can produce that. Havoc also EXHAUSTS what it
        //                    plays, which is extra decrements for free.
        //                    Safe here: mayPlayHandCard can check the two piles Havoc reads,
        //                    and this deck conjures nothing out of CardPools.h.
        std::vector<CardId> batch11a = BATCH_11_STRIKE;
        batch11a.push_back(CardId::FIEND_FIRE);
        batch11a.push_back(CardId::FIEND_FIRE);
        batch11a.push_back(CardId::EXHUME);
        batch11a.push_back(CardId::DUAL_WIELD);
        batch11a.push_back(CardId::WILD_STRIKE);
        batch11a.push_back(CardId::SWIFT_STRIKE);
        batch11a.push_back(CardId::TRUE_GRIT);
        batch11a.push_back(CardId::TRUE_GRIT);
        batch11a.push_back(CardId::HAVOC);
        batch11a.push_back(CardId::HAVOC);
        variants.push_back({batch11a, 40, false, batch11Encounters});
        variants.push_back({batch11a, 40, true,  batch11Encounters});

        // Variants 17/18: THE_BOMB (10 starter + 2 + 8 enablers = 20 cards).
        //
        // Damage-POOR on purpose — that is the whole point of the pair. The only attacks in it
        // are the five starter Strikes and Bash, so ~1.9 attacks per 5-card hand, and the
        // enablers below soak the energy the policy would otherwise spend on them. Cultist
        // then survives four or five player turns, which is what lets `if (bomb1)` fire at all.
        //   IMPERVIOUS x2      2 energy for 30 (40) Block and it EXHAUSTS. The cleanest energy
        //                      sink in the registered set: it adds no damage, keeps the player
        //                      alive through the extra turns, and being one-shot it does not
        //                      make the late game trivial.
        //   GHOSTLY_ARMOR x2   1 energy for 10 (13) Block, ethereal so it exhausts itself
        //                      rather than clogging the hand. Same job, cheaper, recycles.
        //   TRUE_GRIT x2       1 energy for 7 (9) Block AND exhausts a card from hand — at
        //                      random un-upgraded, chosen (a screen) when upgraded. It actively
        //                      REMOVES Strikes from the battle, which slows the clock further,
        //                      and doubles as a second exhaust source.
        //   PURITY x2          ADDED AFTER MEASURING. First pass: the un-upgraded pair covered
        //                      the timer well (deleting the detonation failed 196 replays), but
        //                      the UPGRADED damage (`up ? 50 : 40`) failed only 1 — because by
        //                      the third turn end the Cultist was already low enough that 40
        //                      and 50 are indistinguishable. Purity is 0-cost and exhausts the
        //                      lowest 3 (5) hand cards, and the harness's multi-select always
        //                      picks the lowest indices — so it strips the deck's only damage
        //                      (the five starter Strikes and Bash) out of the battle entirely.
        //                      The Bomb then detonates against a near-full-HP Cultist (48-54),
        //                      where 40 leaves it alive and 50 can kill it.
        //                      ⚠ Second effect, deliberate: between Purity x2, True Grit x2 and
        //                      the self-exhausting Impervious/Ghostly Armor, up to 12 of the 20
        //                      cards leave combat, so hand+draw+discard really can hit ZERO
        //                      with a bomb still ticking. That is the only way to reach
        //                      executeActions' `player.bomb1 || bomb2 || bomb3` clause in the
        //                      can't-win check.
        // Two copies of THE_BOMB so both `bomb3 += amount` twice in one turn (a single 80/100
        // slot, not two timers) and "a second bomb lands while the first is still shifting"
        // actually happen.
        std::vector<CardId> batch11b = BATCH_11_BOMB;
        batch11b.push_back(CardId::IMPERVIOUS);
        batch11b.push_back(CardId::IMPERVIOUS);
        batch11b.push_back(CardId::GHOSTLY_ARMOR);
        batch11b.push_back(CardId::GHOSTLY_ARMOR);
        batch11b.push_back(CardId::TRUE_GRIT);
        batch11b.push_back(CardId::TRUE_GRIT);
        batch11b.push_back(CardId::PURITY);
        batch11b.push_back(CardId::PURITY);
        variants.push_back({batch11b, 40, false, batch11Encounters});
        variants.push_back({batch11b, 40, true,  batch11Encounters});

        // Variants 19/20: BATCH_12 (10 starter + 6 + 4 enablers = 20 cards). Its own pair for
        // the same structural reason as batches 7-11: the full deck is 93 and Deck::MAX_SIZE
        // is 96. Same two encounters as batches 9-11, same reason (size: jaw_worm_horde.jsonl
        // sits at 46MB of a 100MB hard limit, and these are card branches, not monster ones).
        //
        // THREE copies of each of the batch's two cards, and the count for VIOLENCE is the
        // load-bearing part of this deck:
        //   VIOLENCE x3   Every play RIPS 3 (4 upgraded) Attacks OUT of the draw pile, so
        //                 consecutive plays walk the draw pile's attack count DOWN through
        //                 2, 1 and 0. That is the only way to reach the fixed early-exit —
        //                 the `attackIdxList.size()-i <= 0` arm that used to `return` and
        //                 duplicate cards — and the `attackIdxList.empty()` return above it.
        //                 One copy would practically always find >= 3 Attacks (the 10 starter
        //                 cards alone hold 6) and both arms would be structurally dead.
        //   DARK_SHACKLES x3
        //                 Needs to land on MANY DIFFERENT TURNS, because what it proves is a
        //                 time step, not a number: Strength goes to -9 (-15), the Cultist's
        //                 Dark Strike is computed off that reduced value, and then
        //                 applyEndOfTurnTriggers hands the Strength back and clears SHACKLED.
        //                 Three copies in a 20-card deck means roughly one per draw cycle.
        //
        // The deck is deliberately damage-POOR (same tactic as variants 17/18, different
        // reason). Only the 5 starter Strikes and Bash deal damage, so Cultist survives many
        // turns — and the draw pile therefore CYCLES several times, which is what spreads
        // Violence's plays across "draw pile full of Attacks" and "draw pile nearly empty".
        // A fast kill would have every Violence resolve on turn 1 or 2 with 4+ Attacks
        // available, i.e. exactly the case the un-patched code got right.
        //   IMPERVIOUS x2      2 energy for 30 (40) Block, exhausts. Pure survivability with
        //                      no damage, and being one-shot it cannot trivialise the late
        //                      battle.
        //   GHOSTLY_ARMOR x2   1 energy for 10 (13) Block, ethereal so it exhausts itself
        //                      instead of clogging the hand.
        // ⚠ Second, deliberate effect of the four enablers plus the batch's own two exhausting
        // cards: up to 10 of the 20 cards LEAVE combat over a long battle, so the late-battle
        // deck is just the 10 starter cards and the draw pile turns over every two turns. That
        // is what makes the low-attack-count states common rather than freakish.
        //
        // Replayability: no MAYHEM and no HAVOC, and nothing here conjures out of CardPools.h,
        // so mayPlayHandCard's autoplay arms never come into play and the deck is trivially
        // clean for isReplayableCard.
        const std::vector<MonsterEncounter> batch12Encounters {
            MonsterEncounter::CULTIST, MonsterEncounter::TWO_LOUSE,
        };
        std::vector<CardId> batch12 = BATCH_12;
        batch12.push_back(CardId::IMPERVIOUS);
        batch12.push_back(CardId::IMPERVIOUS);
        batch12.push_back(CardId::GHOSTLY_ARMOR);
        batch12.push_back(CardId::GHOSTLY_ARMOR);
        variants.push_back({batch12, 40, false, batch12Encounters});
        variants.push_back({batch12, 40, true,  batch12Encounters});

        // Variant 21: ASCENSION 19, batch-1 deck, the 14 "normal" (non-elite, non-boss)
        // act-1 encounters. Batch 21 of the engine repo.
        //
        // WHY 19 AND ONLY 19. Every ascension condition reachable from these encounters is of
        // the form `ascension >= N` with N in {2,3,4,7,17,19} (MonsterSpecific.cpp,
        // MonsterGroup.cpp, Monster.cpp). One run at 19 therefore takes the HIGH side of every
        // one of them, while the existing 125-seed asc-0 corpus already pins the LOW side.
        // What a single level cannot prove is that the threshold is exactly N rather than
        // N±1 — that needs a *pair* of levels (16/17, 6/7, …) and is deliberately out of
        // scope; it is recorded as a blind spot in the engine repo's TODOS.md.
        //
        // WHY THE SAME DECK AS VARIANT 0. Holding the deck fixed makes ascension the only
        // variable between this variant and the frozen variant-0 lines, so any diff in monster
        // HP, intent, damage or RNG counter is attributable to ascension alone.
        //
        // WHY 40 SEEDS AND NOT 125. Volume. Ascension branches are overwhelmingly constant
        // substitutions (`asc2 ? 12 : 11`) rather than new control flow, so seed diversity
        // buys much less here than it does for the intent rolls that variant 0 exists to
        // cover. 40 seeds x 3 floors x 14 encounters is ~30MB.
        //
        // NOT JUST A MONSTER KNOB — the player side changes too, and all three fall out of
        // GameContext for free (they are why this had to be a GameContext-level parameter
        // rather than a BattleContext one):
        //   * asc >= 10  an extra ASCENDERS_BANE, obtained BEFORE the class's starting cards
        //                (GameContext.cpp:479-481), so it sorts FIRST in the deck array;
        //   * asc >= 14  Ironclad maxHp 80 -> 75 (GameContext.cpp:485);
        //   * asc >= 11  potionCapacity 3 -> 2 (GameContext.cpp:66) — the potion loop below
        //                iterates bc.potionCapacity, so it hands out two potions by itself.
        //
        // Elites and bosses (GREMLIN_NOB, LAGAVULIN, THREE_SENTRIES, THE_GUARDIAN,
        // SLIME_BOSS, HEXAGHOST) are NOT here: their HP threshold is asc>=8 / asc>=9 rather
        // than 7 and they carry their own asc17/18/19 move branches, which is a batch of its
        // own. ⚠ The next batch must APPEND ANOTHER VARIANT for them rather than extend this
        // one's encounter list — growing this list shifts every traceIdx after it.
        // (Batch 22 did exactly that: see variant 22 immediately below.)
        const std::vector<MonsterEncounter> asc19Encounters {
            MonsterEncounter::CULTIST,          MonsterEncounter::JAW_WORM,
            MonsterEncounter::JAW_WORM_HORDE,   MonsterEncounter::TWO_LOUSE,
            MonsterEncounter::THREE_LOUSE,      MonsterEncounter::SMALL_SLIMES,
            MonsterEncounter::LOTS_OF_SLIMES,   MonsterEncounter::LARGE_SLIME,
            MonsterEncounter::BLUE_SLAVER,      MonsterEncounter::RED_SLAVER,
            MonsterEncounter::LOOTER,           MonsterEncounter::EXORDIUM_THUGS,
            MonsterEncounter::EXORDIUM_WILDLIFE, MonsterEncounter::GREMLIN_GANG,
        };
        variants.push_back({BATCH_1, 40, false, asc19Encounters, 19});

        // Variant 22: ASCENSION 19, batch-1 deck, the six act-1 ELITE and BOSS encounters.
        // Batch 22 of the engine repo. Same deck, same seed count, same level as variant 21 —
        // only the encounter list differs.
        //
        // WHY A SEPARATE VARIANT RATHER THAN SIX MORE ENTRIES IN asc19Encounters. traceIdx is
        // assigned by walking variants x encounters in declaration order, and it drives the
        // relic/potion rotation. Growing variant 21's list would shift every traceIdx after
        // it and invalidate all 14 committed @asc19 files. Appending a whole variant only
        // adds indices at the end. Same rule as the frozen `encounters` list above.
        //
        // WHY THEY WERE HELD BACK FROM VARIANT 21. Their ascension tiers are on different
        // thresholds than the hallway monsters', and every one of the three families is a
        // separate number:
        //   * HP:      normal `asc>=7`, ELITE `asc>=8`, BOSS `asc>=9`
        //              (Monster::initHp, MonsterSpecific.cpp:26-128 — three groups of cases
        //              sitting side by side in one switch);
        //   * damage:  hallwayIdx  = getTriIdx(asc, 2, 17)
        //              eliteDiffIdx= getTriIdx(asc, 3, 18)
        //              bossDiffIdx = getTriIdx(asc, 4, 19)   (takeTurn's prologue, :337-348).
        // So this variant is what puts asc3/asc4/asc18/asc19 branches under an oracle at all;
        // level 19 clears the highest of them (bossDiffIdx's 19) by exactly one.
        //
        // ⚠ ONE OF THESE SIX BRANCHES IS SHARP-EDGED: Gremlin Nob's asc18 block in
        // getMoveForRoll (:2434-2447) returns RUSH whenever `!lastTwoMoves(SKULL_BASH)`, so
        // SKULL_BASH becomes structurally unreachable at asc>=18 and both of the block's
        // other returns are dead code. That looks like a slip (the real game's A18 Nob uses
        // Skull Bash *more*), but unlike the ACID_SLIME_L enum fixed in this same batch it is
        // not a one-word restoration — the correct shape cannot be read off this file. Left
        // as-is on purpose; the engine repo records it as an open ruling.
        const std::vector<MonsterEncounter> asc19EliteBossEncounters {
            MonsterEncounter::GREMLIN_NOB,  MonsterEncounter::LAGAVULIN,
            MonsterEncounter::THREE_SENTRIES, MonsterEncounter::THE_GUARDIAN,
            MonsterEncounter::SLIME_BOSS,   MonsterEncounter::HEXAGHOST,
        };
        variants.push_back({BATCH_1, 40, false, asc19EliteBossEncounters, 19});
    }

    // ================================ ACT 2 ==================================
    //
    // A SECOND (variants x encounters) product, emitted AFTER the act-1 one above finishes.
    //
    // ⚠⚠ WHY IT IS A SEPARATE PRODUCT AND NOT NEW ENTRIES IN `encounters`.
    // traceIdx is assigned by walking variants x encounters in declaration order and it drives
    // the relic/potion rotation, so inserting anything into the frozen `encounters` list would
    // shift every index after it and invalidate all 40 committed files at once. Appending a
    // whole product only ever adds indices at the END, which leaves the act-1 numbering
    // untouched. Same rule as variant 21 -> variant 22, one level further out.
    //
    // This list holds ALL NINETEEN act-2 encounters (MonsterEncounterPool, MonsterEncounters.h
    // :153-181 — 5 weak + 8 strong + 3 elite + 3 boss). Listing them all up front is safe
    // BECAUSE each variant filters: an encounter a variant does not name is `continue`d before
    // the seed loop, so it consumes no traceIdx. Growing this list later is therefore free,
    // whereas growing a VARIANT's encounter list is not.
    //
    // ⚠ Floors stay {1,3,7} even though act 2 really spans floors ~17-33. floorNum feeds
    // exactly one thing here: the `Random(seed + floorNum)` that reseeds miscRng / shuffleRng /
    // cardRandomRng on arrival. Keeping the same three floors keeps act-2 lines comparable to
    // act-1 ones and costs nothing. `gc.act` likewise stays 1 (GameContext.h:217; only
    // transitionToAct / a save file ever change it) — the only act-sensitive code in monster
    // construction is MonsterGroup's `bc.act == 3` Jaw Worm buff, which none of these
    // encounters reach.
    const std::vector<std::pair<MonsterEncounter, const char *>> act2Encounters {
        // weak pool
        {MonsterEncounter::SPHERIC_GUARDIAN,          "SPHERIC_GUARDIAN"},
        {MonsterEncounter::CHOSEN,                    "CHOSEN"},
        {MonsterEncounter::SHELL_PARASITE,            "SHELL_PARASITE"},
        {MonsterEncounter::THREE_BYRDS,               "THREE_BYRDS"},
        {MonsterEncounter::TWO_THIEVES,               "TWO_THIEVES"},
        // strong pool
        {MonsterEncounter::CHOSEN_AND_BYRDS,          "CHOSEN_AND_BYRDS"},
        {MonsterEncounter::SENTRY_AND_SPHERE,         "SENTRY_AND_SPHERE"},
        {MonsterEncounter::CULTIST_AND_CHOSEN,        "CULTIST_AND_CHOSEN"},
        {MonsterEncounter::THREE_CULTIST,             "THREE_CULTIST"},
        {MonsterEncounter::SHELLED_PARASITE_AND_FUNGI,"SHELLED_PARASITE_AND_FUNGI"},
        {MonsterEncounter::SNECKO,                    "SNECKO"},
        {MonsterEncounter::SNAKE_PLANT,               "SNAKE_PLANT"},
        {MonsterEncounter::CENTURION_AND_HEALER,      "CENTURION_AND_HEALER"},
        // elites
        {MonsterEncounter::GREMLIN_LEADER,            "GREMLIN_LEADER"},
        {MonsterEncounter::SLAVERS,                   "SLAVERS"},
        {MonsterEncounter::BOOK_OF_STABBING,          "BOOK_OF_STABBING"},
        // bosses
        {MonsterEncounter::CHAMP,                     "CHAMP"},
        {MonsterEncounter::COLLECTOR,                 "COLLECTOR"},
        {MonsterEncounter::AUTOMATON,                 "AUTOMATON"},
    };

    std::vector<DeckVariant> act2Variants;
    {
        // Variant 23: the first act-2 variant. Batch 23 of the engine repo.
        //
        // Deck / seeds / ascension are variant 0's exactly (BATCH_1, all 125 seeds,
        // un-upgraded, ascension 0), for variant 0's reasons: monster behaviour barely depends
        // on the deck, what pulls the branches apart is SEED COUNT, and the 21-card deck makes
        // the longest fights (= the most monster turns). The engine repo keeps only this one
        // variant's lines per file (`ENC_V0` policy), so the file is frozen from the moment it
        // is installed.
        //
        // ⚠ ASCENSION 0 ONLY. None of the 40 act-2/3 monsters has had its ascension tiers
        // calibrated against an oracle, and the engine's `ascCalibrated` gate throws for them,
        // so an asc-19 act-2 variant would be data with nothing to check it. That is a batch of
        // its own, and per the rule above it must be ANOTHER APPENDED VARIANT, never a longer
        // encounter list on this one.
        //
        // The three encounters are the act-2 SINGLE-MONSTER ones with no summons and no card
        // insertion, so this batch exercises the new act-2 plumbing without stacking new
        // mechanics on top of it:
        //   SPHERIC_GUARDIAN  Barricade (first monster-side Barricade in the project) +
        //                     Artifact 3 + a 40-point starting block; all four of its moves
        //                     end with a SYNCHRONOUS setMove + bc.noOpRollMove().
        //   CHOSEN            Hex (first PS::HEX producer) + a five-move roll table.
        //   SNAKE_PLANT       Malleable 3 (only Snake Plant and Writhing Mass have it in the
        //                     whole project).
        const std::vector<MonsterEncounter> batch23Encounters {
            MonsterEncounter::SPHERIC_GUARDIAN,
            MonsterEncounter::CHOSEN,
            MonsterEncounter::SNAKE_PLANT,
        };
        act2Variants.push_back({BATCH_1, seeds.size(), false, batch23Encounters});

        // Variant 24: batch 24 of the engine repo — FLIGHT (Byrd) and the Mugger.
        //
        // Everything is variant 23's (all 125 seeds, un-upgraded, ascension 0, appended rather
        // than folded into variant 23's encounter list) EXCEPT the deck, which is
        // BATCH_1 + ONE COPY OF SPOT_WEAKNESS. That one card is deliberate and is the only
        // reason this variant's deck differs from variant 23's:
        //
        //   Monster::isAttacking() -> isMoveAttack(moveHistory[0]) (MonsterMoves.h:414-535) has
        //   exactly ONE reader in the whole project, SPOT_WEAKNESS. Every act-1 monster batch
        //   from 13 on kept only the 21-card variant-0 deck (`ENC_V0`), which does not contain
        //   it, so the attack/not-attack classification of the 21 monsters registered since
        //   then has NO ORACLE — flipping the predicate to constant-true only reddens the five
        //   `ENC_ALL` encounters that still carry the 93-card deck (46 cases, measured).
        //   Adding one Spot Weakness here costs 3 extra deck slots' worth of nothing and gives
        //   every act-2 monster from this batch on a real oracle for that predicate.
        //
        // The three encounters:
        //   THREE_BYRDS       three Byrds. FLIGHT: halves incoming card damage
        //                     (BattleContext.cpp:2764), decrements on each unblocked ATTACK
        //                     (Monster.cpp:362-368, its own slot in the attackedUnblockedHelper
        //                     else-if chain between CURL_UP and MALLEABLE), sets BYRD_STUNNED
        //                     when it hits 0, and is restored to 3 at the start of the byrd's
        //                     own turn (Monster.cpp:28-30).
        //   TWO_THIEVES       Looter + Mugger. Same family (escape + gold theft) but different
        //                     numbers AND different dialog-RNG placement: MUGGER_MUG burns an
        //                     aiRng.random(2) on EVERY mug and its 0.6f dialog roll is on
        //                     monster turn 2, where the Looter's is on turn 1.
        //                     It is also the closer for two long-standing blind spots: a
        //                     thief escaping while a COMPANION IS STILL ALIVE (so the escape
        //                     does not end the fight and its `none` turn-end is observable),
        //                     and stealGoldFromPlayer's min(player.gold, amount) clamp (two
        //                     thieves at 15 each can actually empty the player's purse).
        //   CHOSEN_AND_BYRDS  Byrd + Chosen. Reuses batch 23's Chosen and puts a Byrd next to a
        //                     companion, so FLIGHT is exercised outside a mono-species group.
        std::vector<CardId> batch24 = BATCH_1;
        batch24.push_back(CardId::SPOT_WEAKNESS);
        const std::vector<MonsterEncounter> batch24Encounters {
            MonsterEncounter::THREE_BYRDS,
            MonsterEncounter::TWO_THIEVES,
            MonsterEncounter::CHOSEN_AND_BYRDS,
        };
        act2Variants.push_back({batch24, seeds.size(), false, batch24Encounters});

        // Variant 25: batch 25 of the engine repo — PLATED_ARMOR (Shelled Parasite) and
        // CONFUSED (Snecko).
        //
        // Deck / seeds / ascension are variant 24's exactly (BATCH_1 + one SPOT_WEAKNESS, all
        // 125 seeds, un-upgraded, ascension 0), and it is APPENDED rather than folded into
        // variant 24's encounter list for the usual reason (traceIdx drives the relic/potion
        // rotation; growing an existing variant's list shifts every index after it).
        //
        // The Spot Weakness copy carries forward from batch 24 deliberately: it is the only
        // reader of Monster::isAttacking() -> isMoveAttack (MonsterMoves.h:414-535), so keeping
        // it in every new act-2 variant is what gives each batch's new monsters an oracle for
        // their attack/not-attack classification.
        //
        // ⚠ NOTE FOR THE NEXT BATCH: variants 24 and 25 have the BYTE-IDENTICAL deck, and the
        // engine repo's tools/{split-traces,variant0-rows}.mjs fingerprint a variant by its
        // DECK CONTENTS. That is safe here only because the two encounter lists are DISJOINT,
        // so no output file ever contains rows from both. If a future variant reuses this deck
        // AND names an encounter an earlier variant already covers, both variants' rows would
        // land in one file and be treated as a single block — the "frozen prefix" would grow
        // silently. Either keep the encounter lists disjoint (the norm: each batch installs
        // encounters no earlier batch had) or give the new variant a distinguishable deck.
        //
        // The three encounters:
        //   SHELL_PARASITE   one Shelled Parasite (note the enum has no "ED" while the monster
        //                    id does). PLATED_ARMOR 14 + 14 block: adds block equal to its
        //                    stacks at end of turn (Monster.cpp:51-52) and loses one stack per
        //                    unblocked ATTACK, in its OWN slot of the attackedUnblockedHelper
        //                    else-if chain — the SECOND one, between INVINCIBLE and CURL_UP
        //                    (Monster.cpp:352-355). Hitting 0 clears the statusBit (unlike
        //                    FLIGHT) and, gated on `id == SHELLED_PARASITE`, rewrites the intent
        //                    to SHELLED_PARASITE_STUNNED. That stunned turn does nothing and
        //                    ends with a SYNCHRONOUS `setMove(FELL); rollMove(bc);` — the first
        //                    real (non-no-op) synchronous rollMove in the project.
        //                    Its SUCK move is the whole project's only Actions::VampireAttack
        //                    user: it heals min(damage, player.lastAttackUnblockedDamage).
        //   SHELLED_PARASITE_AND_FUNGI
        //                    Shelled Parasite (slot 0) + Fungi Beast (slot 1). Two jobs at
        //                    once: it puts the parasite next to a companion (VampireAttack
        //                    hardcodes arr[0], so the slot ordering matters), and it is the
        //                    closer for batch 16's Spore Cloud blind spot — Monster::die
        //                    returns early when the dying monster was the LAST one, so the
        //                    death trigger only ever runs with a companion alive.
        //   SNECKO           one Snecko. CONFUSED is the batch's other new mechanic and lives
        //                    entirely in CardManager::draw (CardManager.cpp:403-412): for EVERY
        //                    card drawn while confused it burns one cardRandomRng.random(3) —
        //                    OUTSIDE the `if (cost != newCost)` guard — and writes BOTH `cost`
        //                    and `costForTurn`, i.e. PERMANENTLY, not just for the turn. Since
        //                    cardRandomRng is a shared stream, this shifts every later consumer;
        //                    the rng.cardRandom counter in each frame is the oracle.
        const std::vector<MonsterEncounter> batch25Encounters {
            MonsterEncounter::SHELL_PARASITE,
            MonsterEncounter::SHELLED_PARASITE_AND_FUNGI,
            MonsterEncounter::SNECKO,
        };
        act2Variants.push_back({batch24, seeds.size(), false, batch25Encounters});

        // Variant 26: batch 26 of the engine repo — MONSTERS BUFFING/HEALING THEIR ALLIES
        // (Centurion + Mystic), plus three encounters that are new COMBINATIONS of monsters
        // that are already registered.
        //
        // Deck / seeds / ascension are variant 24/25's exactly (BATCH_1 + one SPOT_WEAKNESS,
        // all 125 seeds, un-upgraded, ascension 0), appended rather than folded into an
        // existing variant's encounter list (traceIdx drives the relic/potion rotation).
        // The Spot Weakness copy carries forward for the usual reason: it is the only reader of
        // Monster::isAttacking() -> isMoveAttack (MonsterMoves.h:414-535), so every new
        // monster move gets its attack/not-attack classification pinned. New moves this batch:
        // CENTURION_SLASH / CENTURION_FURY / MYSTIC_ATTACK_DEBUFF are in the whitelist,
        // CENTURION_DEFEND / MYSTIC_BUFF / MYSTIC_HEAL are not.
        //
        // ⚠ Decks 24/25/26 are byte-identical, and split-traces.mjs / variant0-rows.mjs
        //   fingerprint a variant by its DECK CONTENTS — safe only because the three encounter
        //   lists are pairwise DISJOINT (see the note on variant 25). All four encounters here
        //   are ones no earlier variant covers.
        //
        // The four encounters:
        //   CENTURION_AND_HEALER  Centurion (slot 0) + Mystic (slot 1). The two new monsters,
        //                         and the first monster-to-ally HEAL and monster-to-ally BUFF
        //                         in the project. All three of the "help my friend" moves
        //                         target a HARDCODED slot, not a random ally like the Shield
        //                         Gremlin's GainBlockRandomEnemy: CENTURION_DEFEND blocks
        //                         arr[1] and nothing for itself (MonsterSpecific.cpp:562-569),
        //                         MYSTIC_HEAL / MYSTIC_BUFF hit arr[0] AND then itself
        //                         (:600-608 / :588-598). Their guards differ too —
        //                         getAliveCount() > 1 vs monstersAlive > 1 (same value, two
        //                         accessors). Centurion's roll table branches on whether the
        //                         Mystic is alive (defend when it is, fury when it is not,
        //                         :2192-2216) and the Mystic's is the only one in the project
        //                         that reads HP: it force-heals when itself or arr[0] is
        //                         >= healNeedAmt below max (:2223-2229). All three of those
        //                         "help" cases end with a SYNCHRONOUS real rollMove(bc), so the
        //                         heal is already applied when the next intent is chosen.
        //   THREE_CULTIST         three Cultists. Same species x3: three interleaved rollMoves
        //                         on one aiRng stream, and Ritual stacking on three monsters at
        //                         once. The Cultist has been registered since batch 1 but only
        //                         ever as a lone monster (or one of six candidates in
        //                         EXORDIUM_THUGS).
        //   CULTIST_AND_CHOSEN    Cultist (slot 0) + Chosen (slot 1). Puts batch 23's Chosen
        //                         next to a companion, so its HEX + drain are exercised while
        //                         another monster is taking turns.
        //   SENTRY_AND_SPHERE     Sentry (slot 0) + Spheric Guardian (slot 1). Two payoffs:
        //                         the Sentry's first move is `idx % 2 == 0 ? BOLT : BEAM`
        //                         (:2683-2691) and this is the first encounter where a Sentry
        //                         sits at slot 0 in a group whose OTHER member is not a Sentry;
        //                         and the Spheric Guardian (Barricade + Artifact 3 + 40 block,
        //                         batch 23) finally has a companion, so the "clear block unless
        //                         Barricade" loop in applyStartOfTurnPowers runs over a group
        //                         where one member keeps its block and the other does not.
        const std::vector<MonsterEncounter> batch26Encounters {
            MonsterEncounter::CENTURION_AND_HEALER,
            MonsterEncounter::THREE_CULTIST,
            MonsterEncounter::CULTIST_AND_CHOSEN,
            MonsterEncounter::SENTRY_AND_SPHERE,
        };
        act2Variants.push_back({batch24, seeds.size(), false, batch26Encounters});

        // Variant 27: batch 27 of the engine repo — SUMMONING (Gremlin Leader) and the two
        // act-2 elite encounters that need it.
        //
        // Deck / seeds / ascension are variant 24/25/26's exactly (BATCH_1 + one SPOT_WEAKNESS,
        // all 125 seeds, un-upgraded, ascension 0), appended rather than folded into an
        // existing variant's encounter list (traceIdx drives the relic/potion rotation).
        // The Spot Weakness copy carries forward for the usual reason: it is the only reader of
        // Monster::isAttacking() -> isMoveAttack (MonsterMoves.h:414-535). New moves this batch:
        // GREMLIN_LEADER_STAB (:459) and TASKMASTER_SCOURING_WHIP (:512) are in the whitelist,
        // GREMLIN_LEADER_ENCOURAGE / GREMLIN_LEADER_RALLY are not.
        //
        // ⚠ Decks 24/25/26/27 are byte-identical and split-traces.mjs / variant0-rows.mjs
        //   fingerprint a variant by its DECK CONTENTS — safe only because the four encounter
        //   lists are pairwise DISJOINT (see the note on variant 25). Neither encounter here is
        //   covered by an earlier variant.
        //
        // The two encounters:
        //   GREMLIN_LEADER   two random gremlins in slots 1 and 2 (getGremlin(miscRng), 8-entry
        //                    table with duplicates) plus the Leader in slot 3, and
        //                    `monsterCount = 4` with `monstersAlive = 3` — SLOT 0 IS NEVER
        //                    CONSTRUCTED (MonsterGroup.cpp:248-259). That reserved hole is what
        //                    Actions::SummonGremlins (Actions.cpp:459-497) fills: it scans for
        //                    dying slots in the order 1, 2, 0, takes the first TWO, rebuilds
        //                    them from scratch (`= Monster()`), picks each species off
        //                    getGremlin(**aiRng**, not miscRng), bumps monstersAlive by 2, marks
        //                    both MINION and rollMoves each. monsterCount never moves, and
        //                    preBattleAction is NOT re-run — a summoned Mad Gremlin therefore
        //                    has no ANGRY, unlike one built by GREMLIN_GANG.
        //                    The Leader itself carries MINION_LEADER (MonsterSpecific.cpp:168),
        //                    which Monster::die reads: `monstersAlive == 0 ||
        //                    hasStatus<MINION_LEADER>()` -> PLAYER_VICTORY and an immediate
        //                    return (Monster.cpp:293-297), so killing the Leader ends the fight
        //                    with its minions still standing.
        //                    ENCOURAGE burns one aiRng.random(0,2) for an in-game quote, then
        //                    walks slots 0..2 synchronously (buff + block), then buffs itself.
        //   SLAVERS          Blue Slaver (slot 0), TASKMASTER (slot 1), Red Slaver (slot 2)
        //                    — note the ORDER (MonsterGroup.cpp:366-370): the Taskmaster is in
        //                    the MIDDLE, not first. Its only move whips for 7 and queues
        //                    MakeTempCardInDiscard({WOUND}), then ends with a SYNCHRONOUS
        //                    bc.noOpRollMove() (:1234-1247). WOUND is unplayable
        //                    (CardInstance.cpp:329 excepts only SLIMED), so it only ever sits in
        //                    the discard pile — which the snapshot dumps. It is also the first
        //                    three-monster act-2 group, so the Red Slaver's entangle and the
        //                    Blue Slaver's rake are exercised alongside a third body.
        const std::vector<MonsterEncounter> batch27Encounters {
            MonsterEncounter::GREMLIN_LEADER,
            MonsterEncounter::SLAVERS,
        };
        act2Variants.push_back({batch24, seeds.size(), false, batch27Encounters});

        // Variant 28: batch 28 of the engine repo — the Book of Stabbing (act 2's last elite)
        // and the Bronze Automaton (the SECOND summon host, with its own dedicated code path).
        //
        // Deck / seeds / ascension are variant 24/25/26/27's exactly (BATCH_1 + one
        // SPOT_WEAKNESS, all 125 seeds, un-upgraded, ascension 0), appended rather than folded
        // into an existing variant's encounter list (traceIdx drives the relic/potion rotation).
        // The Spot Weakness copy carries forward for the usual reason: it is the only reader of
        // Monster::isAttacking() -> isMoveAttack (MonsterMoves.h:414-535). New moves this batch:
        // BOOK_OF_STABBING_MULTI_STAB / _SINGLE_STAB, BRONZE_AUTOMATON_FLAIL / _HYPER_BEAM and
        // BRONZE_ORB_BEAM are in the whitelist; BRONZE_AUTOMATON_BOOST / _SPAWN_ORBS / _STUNNED
        // and BRONZE_ORB_STASIS / _SUPPORT_BEAM are not.
        //
        // ⚠ Decks 24..28 are byte-identical and split-traces.mjs / variant0-rows.mjs
        //   fingerprint a variant by its DECK CONTENTS — safe only because the five encounter
        //   lists are pairwise DISJOINT (see the note on variant 25). Neither encounter here is
        //   covered by an earlier variant.
        //
        // The two encounters:
        //   BOOK_OF_STABBING  one Book of Stabbing (160-164 HP, elite tier asc>=8). Two things:
        //                     PAINFUL_STABS (preBattleAction, MonsterSpecific.cpp:177-180) puts
        //                     a WOUND in the discard pile for every instance of UNBLOCKED
        //                     attack damage (Player::attacked, Player.cpp:250-252 — inside the
        //                     `damage > 0` branch, so a fully blocked hit inserts nothing), and
        //                     MULTI_STAB's hit count is `miscInfo` — the same one field the
        //                     Louse's bite damage, the Red Slaver's usedEntangle and the Gremlin
        //                     Wizard's charge counter live in. It starts at 1 (`++miscInfo` in
        //                     preBattleAction, which runs AFTER the opening rollMove) and the
        //                     roll table increments it on every MULTI_STAB it hands out, so the
        //                     stab count grows monotonically across the fight — a multi-hit
        //                     attack whose LENGTH is state, which nothing registered so far has.
        //   AUTOMATON         Bronze Automaton in slot 1, with slots 0 AND 2 RESERVED EMPTY.
        //                     MonsterGroup.cpp:173-177 writes `monsterCount = 1;
        //                     createMonster(BRONZE_AUTOMATON); ++monsterCount;` — the assignment
        //                     BEFORE createMonster is what pushes the automaton off slot 0, and
        //                     the bare `++` afterwards is what makes the group three slots long
        //                     with only one monster in it. A different shape from the Gremlin
        //                     Leader's (which reserves slot 0 only and writes monstersAlive /
        //                     monsterCount by hand): copy it, do not tidy it.
        //                     Monster::spawnBronzeOrbs (MonsterSpecific.cpp:3443-3464) is NOT
        //                     Actions::SummonGremlins: it is called SYNCHRONOUSLY from takeTurn
        //                     (:503, no addToBot), the two slots are HARDCODED 0 and 2 (no
        //                     search, no isDying test), there is no `= Monster()` reset, the
        //                     species is fixed so nothing is rolled off aiRng for it, and it
        //                     ends with `++bc.monsterTurnIdx` so the orb in slot 2 does not act
        //                     on the turn it appears. Species aside, each orb still burns TWO
        //                     monsterHpRng calls (BRONZE_ORB is in initHp's "discard the first
        //                     roll" family, :109-112).
        //                     The automaton also carries MINION_LEADER + ARTIFACT 3
        //                     (:217-221) and its whole intent chain is synchronous setMove +
        //                     synchronous noOpRollMove, keyed off `miscInfo` used as
        //                     `lastBoostWasFlail` (:471-511) — so HYPER_BEAM every other BOOST,
        //                     and (below asc19) a STUNNED turn after it that does nothing at all.
        //                     The orbs bring STASIS: BRONZE_ORB_STASIS pulls one card out of the
        //                     draw pile (or the discard pile when the draw pile is empty) via
        //                     cardRandomRng, biased to the highest rarity present
        //                     (RARE > UNCOMMON > COMMON, else a flat random pick), parks it, and
        //                     Monster::die hands it back to the hand (Monster.cpp:308-309).
        //                     Both halves are visible in the pile snapshots.
        const std::vector<MonsterEncounter> batch28Encounters {
            MonsterEncounter::BOOK_OF_STABBING,
            MonsterEncounter::AUTOMATON,
        };
        act2Variants.push_back({batch24, seeds.size(), false, batch28Encounters});

        // Variant 29: batch 29 of the engine repo — the last two act-2 bosses, which closes
        // act 2 (19/19 encounters).
        //
        // Deck / seeds / ascension are variant 24..28's exactly (BATCH_1 + one SPOT_WEAKNESS,
        // all 125 seeds, un-upgraded, ascension 0), APPENDED rather than folded into any
        // existing variant's encounter list (traceIdx drives the relic/potion rotation).
        // The Spot Weakness copy carries forward for the usual reason: it is the only reader of
        // Monster::isAttacking() -> isMoveAttack (MonsterMoves.h:414-535). New moves this batch
        // that ARE in the whitelist: TORCH_HEAD_TACKLE, THE_CHAMP_FACE_SLAP,
        // THE_CHAMP_HEAVY_SLASH, THE_CHAMP_EXECUTE, THE_COLLECTOR_FIREBALL. New moves that are
        // NOT: THE_CHAMP_DEFENSIVE_STANCE / _TAUNT / _GLOAT / _ANGER, THE_COLLECTOR_BUFF /
        // _MEGA_DEBUFF / _SPAWN.
        //
        // ⚠ Decks 24..29 are byte-identical and split-traces.mjs / variant0-rows.mjs
        //   fingerprint a variant by its DECK CONTENTS — safe only because the six encounter
        //   lists are pairwise DISJOINT. Neither encounter here appears in an earlier variant.
        //
        // The two encounters:
        //   COLLECTOR  The Collector (282 HP, boss tier asc>=9) in SLOT 2, with slots 0 AND 1
        //              RESERVED EMPTY. MonsterGroup.cpp:198-201 is `monsterCount = 2;
        //              createMonster(THE_COLLECTOR);` — a THIRD way of reserving slots, distinct
        //              from the Gremlin Leader's (build 1/2/3, then hand-assign
        //              monstersAlive = 3 / monsterCount = 4) and from the Automaton's
        //              (`monsterCount = 1; createMonster(...); ++monsterCount`). Here the bare
        //              assignment before createMonster is the whole trick: createMonster writes
        //              arr[monsterCount], then does ++monsterCount and ++monstersAlive, so the
        //              group ends up monsterCount = 3 / monstersAlive = 1 with the boss LAST.
        //              Actions::SpawnTorchHeads (Actions.cpp:500-527) is the THIRD summon family
        //              and shares no code with the other two:
        //                * how many: `3 - bc.monsters.monstersAlive` — the ONLY place in the
        //                  whole reference that reads monstersAlive to size a spawn, so it is
        //                  also the oracle for "reserved empty slots must NOT count as alive";
        //                * where: `spawnIdxs[2] {(arr[1].isDying() ? 1 : 0), 0}` — slot 1 first
        //                  when it is free, then slot 0. Not a search over 1,2,0 (Gremlins) and
        //                  not hardcoded 0/2 (Orbs);
        //                * `torchHead = Monster()` full reset (Gremlins yes, Orbs no);
        //                * initHp is called a SECOND time right after construct(), which already
        //                  called it — the reference notes `// bug somewhere in game`. So each
        //                  torch head burns TWO monsterHpRng calls and keeps the SECOND roll.
        //                  This is NOT the hpDiscardRoll family (that discard lives inside
        //                  initHp and always uses the low HP band);
        //                * intent comes from `setMove(TORCH_HEAD_TACKLE)`, NOT rollMove — and
        //                  TORCH_HEAD's getMoveForRoll falls through to `default` returning
        //                  INVALID (MonsterSpecific.cpp:3364), so it is never rolled at all;
        //                * the aiRng cost is paid at the END instead: `for (i < spawnCount)
        //                  bc.noOpRollMove();` — one discarded random(99) PER SPAWNED HEAD;
        //                * no ++monsterTurnIdx (the Collector is the LAST slot, so the new heads
        //                  cannot act this turn anyway).
        //              The Collector also carries MINION_LEADER (preBattleAction,
        //              MonsterSpecific.cpp:272-274) — its THIRD host after the Gremlin Leader and
        //              the Automaton — so killing it wins the fight outright even with torch
        //              heads standing. TORCH_HEAD_TACKLE's takeTurn case is `attackPlayerHelper
        //              (bc, 7);` and NOTHING ELSE: the fourth turn-end shape ("none"), so a torch
        //              head tackles forever and never touches aiRng again.
        //   CHAMP      The Champ (420 HP, boss tier asc>=9), single monster, seven moves. Its
        //              PHASE 2 is a HP-THRESHOLD LATCH and it does NOT live in Monster::onHpLost
        //              (that switch has no THE_CHAMP case at all — unlike the Guardian's mode
        //              shift, which does). It is inside getMoveForRoll
        //              (MonsterSpecific.cpp:2892-2945): `if (monsterData & 0x4) …` else
        //              `if (curHp < maxHp/2) { monsterData |= 0x4; return ANGER; }`. So the
        //              transition is only noticed on the NEXT roll after the fight drops it below
        //              half, never at the instant of the hit. miscInfo does double duty here:
        //              bits 0..1 are the DEFENSIVE_STANCE use count (capped at 2), bit 2 is the
        //              phase-2 flag, and `++monsterData` on a stance is what increments the
        //              count. ANGER also calls Monster::removeDebuffs() (Monster.cpp:522-535),
        //              the first monster-side debuff wipe in the project — Bash / Thunderclap /
        //              Fear Potion and Clothesline / Weak Potion make VULNERABLE and WEAK on the
        //              boss reachable, so it has a real oracle. TAUNT is also the first monster
        //              move that debuffs the player SYNCHRONOUSLY (`bc.player.debuff<PS::WEAK>
        //              (2, true);`, not addToBot(Actions::DebuffPlayer)), and DEFENSIVE_STANCE's
        //              two numbers use getTriIdx(asc, 9, 19) rather than the bossDiffIdx
        //              (asc 4/19) that ANGER and GLOAT use — two different tier families on one
        //              monster.
        const std::vector<MonsterEncounter> batch29Encounters {
            MonsterEncounter::CHAMP,
            MonsterEncounter::COLLECTOR,
        };
        act2Variants.push_back({batch24, seeds.size(), false, batch29Encounters});

        // Variant 30: ASCENSION 19 across ALL NINETEEN act-2 encounters. Batch 30 of the engine
        // repo — the act-2 counterpart of variants 21/22 (which did act 1 at 19).
        //
        // WHY A WHOLE NEW VARIANT RATHER THAN AN `ascension` FLIP ON 23..29. Same rule as
        // everywhere else in this file: traceIdx is assigned by walking variants x encounters in
        // declaration order and it drives the relic/potion rotation, so touching an existing
        // variant would shift every index after it. Appending only adds indices at the end, so
        // all 19 committed act-2 asc-0 files stay byte-identical (verified: `--check` reproduced
        // all 59 files before this variant was added, and again after).
        //
        // WHY THE ENCOUNTER LIST MAY OVERLAP 23..29 HERE, WHEN 24..29 HAD TO BE DISJOINT.
        // split-traces.mjs fingerprints a variant by DECK CONTENTS **plus ascension**, and it
        // groups rows by `encounter + (ascension ? "@asc"+N : "")`. So this variant's rows land
        // in nineteen NEW files (`<enc>@asc19.jsonl`) with a distinct fingerprint. The
        // disjointness rule bit variants 24..29 only because their decks AND ascension were all
        // identical; here ascension differs, so the collision cannot happen.
        //
        // WHY ASCENSION 19 AND ONLY 19 — and what that deliberately does NOT buy.
        // Every ascension condition these 17 monsters read is `asc >= N` with
        // N in {2,3,4,7,8,9,17,18,19}, so one run at 19 takes the HIGH side of all of them while
        // the committed 125-seed asc-0 corpus pins the LOW side. Two things stay out of scope,
        // and BOTH need a level in the middle rather than a second high one:
        //   * "the boundary is exactly N, not N±1" — needs a PAIR of adjacent levels;
        //   * "the middle of a three-tier `{a,b,c}[getTriIdx(...)]`" — at {0,19} the middle
        //     tier is unreachable by construction.
        // In particular THE CHAMP carries two different tier families at once:
        // DEFENSIVE_STANCE uses getTriIdx(asc, 9, 19) while ANGER/GLOAT use bossDiffIdx
        // (asc 4/19). At 19 both return 2, so 19 cannot tell them apart — separating them needs
        // a level in [4, 9). The engine repo schedules that as its own batch (asc7 + asc16,
        // which also lights the middle tier of the {2,17}/{3,18}/{4,19}/{9,19} families across
        // BOTH acts) rather than smuggling a second level in here.
        //
        // WHY THE SAME DECK AND SEED COUNT AS THE ACT-2 ASC-0 VARIANTS. Deck is variants 24..29's
        // byte-for-byte (BATCH_1 + one SPOT_WEAKNESS) so that ascension is the ONLY variable
        // between an `<enc>.jsonl` line and an `<enc>@asc19.jsonl` line; the Spot Weakness copy
        // also keeps Monster::isAttacking() under an oracle at this level. 40 seeds rather than
        // 125 for variant 21's reason: ascension branches are overwhelmingly constant
        // substitutions (`asc2 ? 12 : 11`), so seed diversity buys much less here than it does
        // for the intent rolls that the asc-0 variants exist to cover.
        //
        // ⚠ THREE STRUCTURAL CONSEQUENCES OF LEVEL 19 ON THESE ENCOUNTERS (all as-built, and all
        // recorded in the engine repo's TODOS so nobody reads a 0 as a transcription slip):
        //   * BRONZE_AUTOMATON_STUNNED becomes UNREACHABLE — HYPER_BEAM's tail is
        //     `if (asc19) setMove(BOOST); else setMove(STUNNED);` (MonsterSpecific.cpp:492-500).
        //     Its asc-0 evidence stands; there simply is no stun at 19.
        //   * TASKMASTER_SCOURING_WHIP gains an EXTRA queued self-buff at asc18
        //     (`addToBot(Actions::BuffEnemy<MS::STRENGTH>(idx, 1))`, :1237) — a whole extra
        //     statement, not a changed number, and the only queued self-buff in the data tables.
        //   * BOOK_OF_STABBING's two `if (asc18) { ++stabCount; }` statements sit AFTER a
        //     `return` (:2304-2307 / :2310-2313) and stay dead at 19. This corpus therefore
        //     backs the as-built behaviour (no increment); it says nothing about whether the real
        //     game increments, which is still an open ruling in the engine repo.
        const std::vector<MonsterEncounter> asc19Act2Encounters {
            // weak pool
            MonsterEncounter::SPHERIC_GUARDIAN, MonsterEncounter::CHOSEN,
            MonsterEncounter::SHELL_PARASITE,   MonsterEncounter::THREE_BYRDS,
            MonsterEncounter::TWO_THIEVES,
            // strong pool
            MonsterEncounter::CHOSEN_AND_BYRDS, MonsterEncounter::SENTRY_AND_SPHERE,
            MonsterEncounter::CULTIST_AND_CHOSEN, MonsterEncounter::THREE_CULTIST,
            MonsterEncounter::SHELLED_PARASITE_AND_FUNGI, MonsterEncounter::SNECKO,
            MonsterEncounter::SNAKE_PLANT,      MonsterEncounter::CENTURION_AND_HEALER,
            // elites
            MonsterEncounter::GREMLIN_LEADER,   MonsterEncounter::SLAVERS,
            MonsterEncounter::BOOK_OF_STABBING,
            // bosses
            MonsterEncounter::CHAMP,            MonsterEncounter::COLLECTOR,
            MonsterEncounter::AUTOMATON,
        };
        act2Variants.push_back({batch24, 40, false, asc19Act2Encounters, 19});
    }

    // ========================= TARGET POLICY (batch 31) =======================
    //
    // A THIRD (variants x encounters) product, emitted AFTER both of the above.
    //
    // ⚠⚠ WHY A THIRD PRODUCT RATHER THAN A VARIANT APPENDED TO `variants`.
    // The act-1 encounters belong to the FIRST product, so the obvious move — push a
    // variant onto `variants` — is exactly the wrong one: traceIdx is captured by
    // reference and the act-1 product runs FIRST, so growing it shifts every traceIdx
    // the act-2 product hands out and invalidates all 38 committed act-2 files.
    // (Variant 22 could be appended to `variants` in its day only because act 1 was still
    // the LAST product; that stopped being true in batch 23.)
    //
    // A third product sidesteps that: appending here only ever hands out indices past the
    // end of everything already committed. It also lets ONE variant span both acts, which
    // is what this axis wants — "multi-monster" is a property of the encounter, not of the
    // act.
    //
    // The encounter list is act 1's ++ act 2's, i.e. ALL THIRTY-NINE installed encounters.
    // Listing them all up front is safe for the same reason act2Encounters lists all 19:
    // an encounter a variant does not name is `continue`d BEFORE the seed loop and
    // therefore consumes no traceIdx. So growing THIS list later is free, whereas growing
    // a VARIANT's encounter list is not.
    std::vector<std::pair<MonsterEncounter, const char *>> tgtEncounters = encounters;
    tgtEncounters.insert(tgtEncounters.end(), act2Encounters.begin(), act2Encounters.end());

    std::vector<DeckVariant> tgtVariants;
    {
        // Variant 31: TARGET POLICY 1 (lastAliveMonster) over every MULTI-MONSTER
        // encounter installed so far, 11 from act 1 and 12 from act 2.
        //
        // WHY ONLY MULTI-MONSTER ONES. lastAliveMonster and firstAliveMonster use the same
        // predicate and the same fallback, so with exactly one monster on the field they
        // return the same index and the whole trace would be byte-identical to the asc-0
        // one already committed — pure volume for zero information. The 16 encounters left
        // out are listed in the engine repo's TODOS with the reason.
        //
        // ⚠ "MULTI-MONSTER" MEANS "CAN EVER HAVE TWO TARGETABLE MONSTERS AT ONCE", not
        // "starts with two". Four of the 23 start as a single monster and become a group:
        //   LARGE_SLIME / SLIME_BOSS  split (Monster::largeSlimeSplit :3392 writes the
        //                             mother's slot AND the one to its right;
        //                             Monster::slimeBossSplit :3419 writes slots 0 and 2);
        //   AUTOMATON                 spawns two Bronze Orbs into the reserved slots 0/2;
        //   COLLECTOR                 spawns Torch Heads into the reserved slots 1/0.
        // Leaving those out would have missed exactly the situations this axis exists for.
        //
        // ⚠ THE ACCEPTANCE TARGET IS CENTURION_AND_HEALER. The Centurion sits at slot 0 and
        // the Mystic at slot 1, and CENTURION_FURY is only ever rolled when
        // `getAliveCount() > 1` is FALSE (MonsterSpecific.cpp:2192-2216). Under the old
        // policy the Centurion always died first, so the Mystic could never predecease it
        // and CENTURION_FURY's takeTurn case (:571-574) had zero examples across the whole
        // corpus — it is the reason this axis was scheduled at all. Under policy 1 the
        // player kills the Mystic first, which is also the standard human line ("kill the
        // healer"), so this is not a contrived state.
        //
        // WHY THE SAME DECK / SEEDS / ASCENSION AS THE ACT-2 ASC-0 VARIANTS.
        //   * deck = BATCH_1 + one SPOT_WEAKNESS, byte-identical to variants 24..30, so the
        //     TARGET POLICY is the only variable between an `<enc>.jsonl` line and an
        //     `<enc>@tgt1.jsonl` line. The Spot Weakness copy also keeps
        //     Monster::isAttacking() -> isMoveAttack under an oracle here.
        //   * 40 seeds, matching the ascension variants: what this axis buys is a different
        //     DEATH ORDER, which shows up in the first handful of seeds, not seed diversity.
        //   * ASCENSION 0 deliberately. Stacking the two axes in one batch would make "which
        //     axis broke the data" undiagnosable, and `<enc>@asc19@tgt1` is a shape nothing
        //     needs yet. The group-key suffix order (asc first, tgt second) is fixed so the
        //     combination can be added later without renaming anything.
        //
        // ⚠ Deck AND ascension both match variants 24..29, so the fingerprint
        // (split-traces.mjs / variant0-rows.mjs use deck contents + ascension + TARGET
        // POLICY) is distinguished ONLY by targetPolicy — and the group key carries a
        // `@tgt1` suffix, so these rows land in 23 NEW files. That is the same escape the
        // asc-19 variants use, and it is why this variant's encounter list MAY overlap
        // variants 21..30's. It would NOT be safe to reuse this deck at asc 0 AND policy 0.
        const std::vector<MonsterEncounter> batch31Encounters {
            // ---- act 1: 11 of 20 ----
            MonsterEncounter::JAW_WORM_HORDE,   // 3 Jaw Worms
            MonsterEncounter::TWO_LOUSE,        // 2 Louses
            MonsterEncounter::THREE_LOUSE,      // 3 Louses
            MonsterEncounter::SMALL_SLIMES,     // 2 slimes
            MonsterEncounter::LOTS_OF_SLIMES,   // 5 small slimes
            MonsterEncounter::LARGE_SLIME,      // 1, then 2 after largeSlimeSplit
            MonsterEncounter::GREMLIN_GANG,     // 4 gremlins
            MonsterEncounter::EXORDIUM_THUGS,   // weak wildlife + strong humanoid
            MonsterEncounter::EXORDIUM_WILDLIFE,// strong wildlife + weak wildlife
            MonsterEncounter::THREE_SENTRIES,   // 3 Sentries
            MonsterEncounter::SLIME_BOSS,       // 1, then 2 after slimeBossSplit
            // ---- act 2: 12 of 19 ----
            MonsterEncounter::THREE_BYRDS,
            MonsterEncounter::TWO_THIEVES,
            MonsterEncounter::CHOSEN_AND_BYRDS,
            MonsterEncounter::SENTRY_AND_SPHERE,
            MonsterEncounter::CULTIST_AND_CHOSEN,
            MonsterEncounter::THREE_CULTIST,
            MonsterEncounter::SHELLED_PARASITE_AND_FUNGI,
            MonsterEncounter::CENTURION_AND_HEALER,  // ★ the acceptance target
            MonsterEncounter::GREMLIN_LEADER,   // 3 alive of 4 slots, + SummonGremlins
            MonsterEncounter::SLAVERS,          // 3 slavers
            MonsterEncounter::AUTOMATON,        // 1, then +2 Bronze Orbs
            MonsterEncounter::COLLECTOR,        // 1, then +2 Torch Heads
        };
        std::vector<CardId> batch31Deck = BATCH_1;
        batch31Deck.push_back(CardId::SPOT_WEAKNESS);
        tgtVariants.push_back({batch31Deck, 40, false, batch31Encounters, 0, 1});
    }

    // ============================== ACT 3 (batch 32) ==========================
    //
    // A FOURTH (variants x encounters) product, emitted AFTER all three above.
    //
    // ⚠⚠ WHY A FOURTH PRODUCT AND NOT A VARIANT ON AN EXISTING ONE. traceIdx is captured by
    // reference and the products run in declaration order, so appending a variant to
    // `variants` / `act2Variants` / `tgtVariants` would shift every traceIdx handed out by
    // the products AFTER it and invalidate their committed files wholesale. Appending a whole
    // product only ever adds indices at the END. Same rule as batch 23 (act 2) and batch 31
    // (target policy), one level further out.
    //
    // ⚠⚠ AND THE COROLLARY THAT BINDS EVERY ACT-3 BATCH: this product must stay LAST until
    // act 3 is finished. Every act-3 batch appends one variant HERE, and a variant appended
    // to a product that is not last shifts the indices of everything after it. So: no new
    // product may be hung behind this one while act 3 is still being filled in.
    //
    // This list holds ALL FIFTEEN act-3 encounters that are NOT already installed
    // (MonsterEncounterPool, MonsterEncounters.h:153-181 — act 3 is 3 weak + 8 strong +
    // 3 elite + 3 boss = 17 slots, THREE_DARKLINGS appears in both the weak and the strong
    // pool, and JAW_WORM_HORDE is already in act 1's frozen `encounters`). Listing them all
    // up front is safe for the same reason act2Encounters lists all 19: an encounter a
    // variant does not name is `continue`d BEFORE the seed loop and therefore consumes no
    // traceIdx. Growing THIS list later is free; growing a VARIANT's list is not.
    //
    // ⚠ JAW_WORM_HORDE is deliberately absent. It is act 3's, but it has been in the act-1
    // product since the first commit (`jaw_worm_horde.jsonl`, ENC_ALL policy) and naming it
    // here as well would only be legal if some variant's deck+ascension+policy fingerprint
    // differed from every act-1 variant's — not worth the trap. Act-3 variants must not name it.
    //
    // ⚠ Floors stay {1,3,7} and `gc.act` stays 1, exactly as for act 2. floorNum feeds only the
    // `Random(seed + floorNum)` reseed on arrival. `gc.act` is read by exactly one thing in
    // monster construction — MonsterGroup's `bc.act == 3` Jaw Worm strength buff — and the one
    // encounter that would reach it (JAW_WORM_HORDE) is not in this product. Revisit this when
    // an act-3 variant ever wants Jaw Worms.
    const std::vector<std::pair<MonsterEncounter, const char *>> act3Encounters {
        // weak pool
        {MonsterEncounter::THREE_DARKLINGS,       "THREE_DARKLINGS"},
        {MonsterEncounter::ORB_WALKER,            "ORB_WALKER"},
        {MonsterEncounter::THREE_SHAPES,          "THREE_SHAPES"},
        // strong pool (JAW_WORM_HORDE and THREE_DARKLINGS also live here)
        {MonsterEncounter::SPIRE_GROWTH,          "SPIRE_GROWTH"},
        {MonsterEncounter::TRANSIENT,             "TRANSIENT"},
        {MonsterEncounter::FOUR_SHAPES,           "FOUR_SHAPES"},
        {MonsterEncounter::MAW,                   "MAW"},
        {MonsterEncounter::SPHERE_AND_TWO_SHAPES, "SPHERE_AND_TWO_SHAPES"},
        {MonsterEncounter::WRITHING_MASS,         "WRITHING_MASS"},
        // elites
        {MonsterEncounter::GIANT_HEAD,            "GIANT_HEAD"},
        {MonsterEncounter::NEMESIS,               "NEMESIS"},
        {MonsterEncounter::REPTOMANCER,           "REPTOMANCER"},
        // bosses
        {MonsterEncounter::AWAKENED_ONE,          "AWAKENED_ONE"},
        {MonsterEncounter::TIME_EATER,            "TIME_EATER"},
        {MonsterEncounter::DONU_AND_DECA,         "DONU_AND_DECA"},
    };

    std::vector<DeckVariant> act3Variants;
    {
        // Variant 32: the first act-3 variant. Batch 32 of the engine repo.
        //
        // THE THREE "SHAPES" ENCOUNTERS. Act 3 opens with the cheapest structural step
        // available: three encounters that between them introduce exactly three monsters
        // (REPULSOR / EXPLODER / SPIKER) and no new turn-structure machinery. The heavy act-3
        // mechanics — the Awakened One's half-death, Time Eater's TIME_WARP, Reptomancer's
        // fourth summon family — each get their own batch, per the engine repo's "one
        // mechanic per batch" rule.
        //
        // ⚠ THE TWO GROUPS ARE BUILT BY TWO DIFFERENT FUNCTIONS, which is the whole reason
        // all three ship together:
        //   THREE_SHAPES / FOUR_SHAPES  -> MonsterGroup::createShapes(bc, n)
        //       (MonsterGroup.cpp:508-530) draws WITHOUT REPLACEMENT from a SIX-entry pool
        //       {REPULSOR, REPULSOR, EXPLODER, EXPLODER, SPIKER, SPIKER}, shifting the tail
        //       left after each pick — same family as GREMLIN_GANG's 8-choose-4, not the
        //       same code.
        //   SPHERE_AND_TWO_SHAPES       -> two calls to MonsterGroup::getAncientShape
        //       (:532-539) which picks WITH REPLACEMENT from a THREE-entry table
        //       {SPIKER, REPULSOR, EXPLODER} — different length, different order, no
        //       duplicates. So that encounter really can field two of the same shape,
        //       and THREE_SHAPES really cannot field three of a kind.
        // Shipping only one of the two would leave the other transcription unbacked.
        //
        // ⚠ SPHERE_AND_TWO_SHAPES also re-uses SPHERIC_GUARDIAN (batch 23) in a NEW slot:
        // it sits LAST (index 2) behind the two shapes, and it is an `hpNoRoll` monster, so
        // that encounter burns exactly 2 miscRng + 2 monsterHpRng rolls at construction.
        //
        // WHY THE SAME DECK / SEEDS / ASCENSION AS THE ACT-2 VARIANTS.
        //   * deck = BATCH_1 + one SPOT_WEAKNESS, byte-identical to variants 24..31, which
        //     keeps Monster::isAttacking() -> isMoveAttack under an oracle for the three new
        //     monsters. That matters more than usual here: EXPLODER_EXPLODE deals 30 damage
        //     and is NOT in the isMoveAttack whitelist (it goes through Actions::DamagePlayer
        //     rather than attackPlayerHelper), so the whitelist's most counter-intuitive
        //     entry to date lands in this very batch.
        //   * 40 seeds, matching variants 30/31. The three shapes have short, largely
        //     deterministic intent chains (the Exploder's is fully scripted), so seed
        //     diversity buys less here than the act-2 asc-0 variants' 125.
        //   * ASCENSION 0 and TARGET POLICY 0 deliberately: none of the three monsters has
        //     `ascCalibrated` set on the engine side yet (the Spiker's {3,4,7} Thorns tiers
        //     have no oracle at either endpoint), and stacking axes in one batch makes "which
        //     axis broke the data" undiagnosable.
        //
        // ⚠⚠ FINGERPRINT COLLISION RULE. split-traces.mjs fingerprints a variant by
        // (deck contents + ascension + targetPolicy). This variant's fingerprint is
        // IDENTICAL to variants 24..29's, so its encounter list MUST stay disjoint from
        // theirs or the rows would land in one file and be treated as a single block. Act-3
        // encounters are named by no act-2 variant, so that holds — and it keeps holding as
        // long as act-3 batches only ever name act-3 encounters.
        //
        // ⚠ JAW_WORM_HORDE is act 3's too, but it has been in the act-1 product since the
        // first commit; naming it here would collide by the rule just above. Act-3 variants
        // must never name it.
        const std::vector<MonsterEncounter> batch32Encounters {
            MonsterEncounter::THREE_SHAPES,           // createShapes(bc, 3)
            MonsterEncounter::FOUR_SHAPES,            // createShapes(bc, 4)
            MonsterEncounter::SPHERE_AND_TWO_SHAPES,  // getAncientShape x2 + SPHERIC_GUARDIAN
        };
        std::vector<CardId> batch32Deck = BATCH_1;
        batch32Deck.push_back(CardId::SPOT_WEAKNESS);
        act3Variants.push_back({batch32Deck, 40, false, batch32Encounters});

        // Variant 33: batch 33 of the engine repo — three act-3 SINGLE-MONSTER encounters
        // whose monsters are assembled entirely out of PRIMITIVES THAT ARE ALREADY REGISTERED
        // engine-side. Nothing here changes turn structure, so it is the cheapest act-3 batch
        // still on the table after the shapes.
        //
        //   ORB_WALKER     -> Monster::initHp's "roll once and THROW IT AWAY, then roll for
        //                     real" family (MonsterSpecific.cpp:32-35). The reference comments
        //                     `// first call is discarded by game` on THIS monster and no
        //                     other, so it is the canonical host of `hpDiscardRoll`
        //                     (already shipped for TASKMASTER/BRONZE_ORB in batches 27/28).
        //                     Also the ONLY host of MS::GENERIC_STRENGTH_UP in the whole
        //                     project (Monster.cpp:103-105: +N strength at end of every
        //                     round). Its laser stuffs one BURN into the DRAW pile and one
        //                     into the DISCARD pile — two different card actions in one case.
        //   SPIRE_GROWTH   -> plain setRandomHp. Brings PS::CONSTRICTED, which is a pure
        //                     number: Player::applyEndOfTurnPowers just does
        //                     `addToBot(DamagePlayer(amount))` (Player.cpp:374-376). No decay,
        //                     no removal, so its move can fire at most once per battle.
        //   MAW            -> the SECOND host of `hpNoRoll` (MonsterSpecific.cpp:119-124,
        //                     alongside SPHERIC_GUARDIAN from batch 23) — it burns zero
        //                     monsterHpRng. Its Nom is the project's first multi-hit whose
        //                     HIT COUNT is derived from the turn number:
        //                     `attackPlayerHelper(bc, 5, (getMonsterTurnNumber()+1)/2)`.
        //
        // ⚠ The Maw's intent chain is what makes all four of its moves reachable in one
        // batch: NOM ends with `setMove(DROOL); noOpRollMove();` (sync, MonsterSpecific.cpp:
        // 1439-1445), so DROOL is forced right after every NOM, while DROOL and ROAR both end
        // with a bare synchronous `rollMove(bc)` and SLAM with the ordinary queued RollMove.
        // Three of the six turn-end shapes on one monster.
        //
        // ⚠⚠ FINGERPRINT COLLISION RULE (same as variant 32): deck + ascension +
        // targetPolicy here are byte-identical to variants 24..29 and 32, so this encounter
        // list must stay disjoint from all of theirs. ORB_WALKER / SPIRE_GROWTH / MAW are
        // named by no other variant, and JAW_WORM_HORDE is deliberately not here.
        //
        // Deck / seeds / ascension / target policy are variant 32's exactly, for variant 32's
        // reasons — in particular the SPOT_WEAKNESS keeps Monster::isAttacking() under an
        // oracle, which this batch needs twice over: SPIRE_GROWTH_CONSTRICT, THE_MAW_ROAR and
        // THE_MAW_DROOL are NOT in the isMoveAttack whitelist while THE_MAW_NOM is, and the
        // whitelist is hand-transcribed.
        const std::vector<MonsterEncounter> batch33Encounters {
            MonsterEncounter::ORB_WALKER,    // hpDiscardRoll + GENERIC_STRENGTH_UP + BURN x2
            MonsterEncounter::SPIRE_GROWTH,  // CONSTRICTED
            MonsterEncounter::MAW,           // hpNoRoll + turn-scaled multi-hit
        };
        std::vector<CardId> batch33Deck = BATCH_1;
        batch33Deck.push_back(CardId::SPOT_WEAKNESS);
        act3Variants.push_back({batch33Deck, 40, false, batch33Encounters});

        // Variant 34: batch 34 of the engine repo — the two act-3 encounters that carry
        // DEATH-AND-TURN-BOUNDARY machinery, paired because they do not interact at all.
        //
        //   THREE_DARKLINGS -> MS::REGROW and, through it, `Monster::halfDead` — the THIRD
        //                     bit of isDeadOrEscaped() (Monster.cpp:253), which has had zero
        //                     oracle since batch 15 skipped it while doing escape. The
        //                     Regrow branch is the MIDDLE slot of Monster::die's else-if
        //                     chain (`SPORE_CLOUD -> REGROW -> STASIS`, Monster.cpp:299-310);
        //                     batch 28 installed STASIS and deliberately left that gap, so
        //                     this batch is what pins the chain's SHAPE, not just one arm.
        //                     ⚠ Three Darklings are each other's companions, so "dies while
        //                     a companion is still up" is the COMMON case here — the exact
        //                     precondition every death trigger needs (Monster::die returns
        //                     early on the victory branch). A single-Darkling encounter does
        //                     not exist, and with one monster the Regrow arm would be
        //                     structurally unreachable.
        //                     ⚠ Its getMoveForRoll is also the only one in the whole
        //                     reference that reads the monster's OWN INDEX: `idx != 1` gates
        //                     Chomp, so the middle Darkling behaves differently from its two
        //                     siblings. And it is the only one that RECURSES
        //                     (`getMoveForRoll(bc, monsterData, bc.aiRng.random(0, 99))`),
        //                     so a single rollMove can burn 1, 2 or more aiRng draws.
        //                     ⚠ Darkling is also the SECOND monster with a species special
        //                     case in Monster::construct (`miscInfo = monsterHpRng.random(
        //                     7, 11)`, Monster.cpp:124-130) — the Louse is the first, and the
        //                     RANGES DIFFER, so copying the neighbour is wrong.
        //   TRANSIENT       -> MS::SHIFTING (the LAST slot of attackedUnblockedHelper's
        //                     else-if chain, Monster.cpp:393-396: `addDebuff<STRENGTH>
        //                     (-damage); buff<SHACKLED>(damage);`) and MS::FADING (a plain
        //                     counter that ONLY decrements in the monster's own move, and
        //                     whose value 1 queues `SuicideAction(idx, FALSE)` — the other
        //                     arm of that action, which skips Monster::die entirely).
        //                     ⚠ The engine repo has implemented the first SEVEN slots of that
        //                     else-if chain (Invincible/Plated Armor/Curl Up/Flight/Malleable
        //                     +Reactive/Thorns/Asleep). Shifting is slot eight, and position
        //                     on that chain is the entire observable surface of any member.
        //                     ⚠ TRANSIENT is also the THIRD and final host of `hpNoRoll`
        //                     (MonsterSpecific.cpp:119-124) — 999 HP, zero monsterHpRng —
        //                     and the ONLY monster named by the main loop's can't-win check
        //                     (`monsters.arr[0].id != MonsterId::TRANSIENT`,
        //                     BattleContext.cpp:790).
        //
        // ⚠ WHY THESE TWO TOGETHER: Regrow lives in Monster::die, Shifting in
        // attackedUnblockedHelper, Fading in one takeTurn case. No shared statement, so a
        // red diff still points at exactly one of them. They do share the `halfDead`/`alive`
        // bookkeeping ONLY in the sense that both change how a monster leaves the field, and
        // the engine models both against the same `isDeadOrEscaped` decomposition — which is
        // precisely why it is worth checking them against one oracle run.
        //
        // ⚠⚠ FINGERPRINT COLLISION RULE (same as variants 32/33): deck + ascension +
        // targetPolicy here are byte-identical to variants 24..29, 32 and 33, so this
        // encounter list must stay disjoint from all of theirs. THREE_DARKLINGS and TRANSIENT
        // are named by no other variant, and JAW_WORM_HORDE is deliberately not here.
        //
        // Deck / seeds / ascension / target policy are variant 32's exactly, for variant 32's
        // reasons — the SPOT_WEAKNESS in particular keeps Monster::isAttacking() under an
        // oracle, which this batch needs: of the six new moves only DARKLING_NIP,
        // DARKLING_CHOMP and TRANSIENT_ATTACK are in the isMoveAttack whitelist, while
        // DARKLING_HARDEN / DARKLING_REGROW / DARKLING_REINCARNATE are not.
        const std::vector<MonsterEncounter> batch34Encounters {
            MonsterEncounter::THREE_DARKLINGS,  // REGROW + halfDead, idx-aware move rules
            MonsterEncounter::TRANSIENT,        // SHIFTING + FADING, hpNoRoll #3
        };
        std::vector<CardId> batch34Deck = BATCH_1;
        batch34Deck.push_back(CardId::SPOT_WEAKNESS);
        act3Variants.push_back({batch34Deck, 40, false, batch34Encounters});

        // Variant 35: batch 35 of the engine repo — the two act-3 encounters that each finish
        // an INCOMPLETE PIECE OF SHARED MACHINERY rather than adding a self-contained monster.
        //
        //   WRITHING_MASS -> the ONLY monster in the whole reference that carries BOTH
        //                    MS::MALLEABLE and MS::REACTIVE, and those two share ONE SLOT of
        //                    attackedUnblockedHelper's else-if chain:
        //                      `} else if (hasStatus<MALLEABLE>() || hasStatus<REACTIVE>()) {`
        //                    with two independent `if`s inside (Monster.cpp:369-383). Batch 23
        //                    installed SNAKE_PLANT and could only transcribe the Malleable
        //                    half; nothing else can pin the slot's shape, because no other
        //                    monster has Reactive at all. Splitting the slot into two else-ifs
        //                    is a silent no-op for every encounter EXCEPT this one.
        //                    ⚠ Reactive also brings the project's first "getting hit re-rolls
        //                    the intent" action (`Actions::ReactiveRollMove`, Actions.cpp:
        //                    133-142), which hard-codes `monsters.arr[0]`, rolls the move
        //                    getStatus<REACTIVE>() TIMES and only then zeroes the counter —
        //                    so one multi-hit card queues ONE action that rolls THREE moves.
        //                    ⚠ And its getMoveForRoll (MonsterSpecific.cpp:3119-3202) is the
        //                    most involved one in the reference: a `while (true)` around FIVE
        //                    NON-EXCLUSIVE `if`s, each of which either returns or RE-ROLLS
        //                    myRoll into the next band and FALLS THROUGH, plus two `continue`s
        //                    and three distinct randomBoolean probabilities (0.1 / 0.4 / 0.3,
        //                    with 0.1 appearing twice as two independent literals). A single
        //                    rollMove can burn anywhere from 1 to seven-odd aiRng draws, so
        //                    the aiRng counter is an unusually sharp oracle here.
        //                    ⚠ Its Implant is the only move whose entire combat-side body is
        //                    `miscInfo = true;` — the reference deliberately does NOT model
        //                    the Parasite curse (a run-level deck change), keeping only the
        //                    "already implanted" latch its move rules read.
        //   GIANT_HEAD    -> MS::SLOW, the first monster power that hangs off the SHARED
        //                    card-play path: `BattleContext::onAfterUseCard`'s
        //                    `if (item.triggerOnUse) { ... if (m.hasStatus<SLOW>())
        //                    m.buff<SLOW>(1); ... }` (BattleContext.cpp:1971-1991). Three
        //                    cooperating sites, and missing any one drifts silently:
        //                    +1 per card played, `damage *= 1 + slow * 0.1f` in
        //                    calculateCardDamage BEFORE Vulnerable (:2748-2750), and
        //                    `setStatus<SLOW>(0)` — NOT decrement, NOT remove — as the SECOND
        //                    statement of Monster::applyEndOfRoundPowers (Monster.cpp:79-81).
        //                    ⚠ Like Reactive it starts at `setHasStatus(true); setStatus(0);`
        //                    (MonsterSpecific.cpp:163-165), i.e. BIT ON / VALUE ZERO, so it is
        //                    invisible in the opening snapshot and only shows up once the
        //                    player starts playing cards. That asymmetry is exactly what a
        //                    `buff(0)` transcription would get wrong in the other direction.
        //                    ⚠ IT_IS_TIME is the project's first CAPPED turn ramp:
        //                    `std::min(getMonsterTurnNumber()-5, 6) * 5` added to
        //                    `(asc3 ? 40 : 30)` — and the first use is at monster turn 4, where
        //                    that term is NEGATIVE (-5). Clamping it at zero costs 5 damage on
        //                    exactly the first Time.
        //                    ⚠ Its damage tier is asc3 (the ELITE band getTriIdx(asc, 3, 18)),
        //                    not the hallway band asc2 — copying the neighbouring monster's
        //                    tiers would be wrong.
        //
        // ⚠ WHY THESE TWO TOGETHER: Reactive lives in attackedUnblockedHelper, Slow in
        // onAfterUseCard / calculateCardDamage / applyEndOfRoundPowers. They share no
        // statement and no monster, so a red diff still points at exactly one of them.
        // Both are single-monster encounters, which also keeps this batch cheap right after
        // the 5.2MB THREE_DARKLINGS file.
        //
        // ⚠⚠ FINGERPRINT COLLISION RULE (same as variants 32/33/34): deck + ascension +
        // targetPolicy here are byte-identical to variants 24..29 and 32..34, so this
        // encounter list must stay disjoint from all of theirs. WRITHING_MASS and GIANT_HEAD
        // are named by no other variant, and JAW_WORM_HORDE is deliberately not here.
        //
        // Deck / seeds / ascension / target policy are variant 32's exactly, for variant 32's
        // reasons — and the SPOT_WEAKNESS matters more than usual this time: of the eight new
        // moves, WRITHING_MASS_WITHER goes through attackPlayerHelper yet was NOT in the
        // isMoveAttack whitelist. ⚠ THAT HAS SINCE BEEN PATCHED (batch 36 of the engine repo,
        // see the FIXED comment on MonsterMoves.h's isMoveAttack), so writhing_mass.jsonl was
        // legitimately regenerated in that batch under ALLOW_CHANGED="writhing_mass".
        const std::vector<MonsterEncounter> batch35Encounters {
            MonsterEncounter::WRITHING_MASS,  // MALLEABLE + REACTIVE share one chain slot
            MonsterEncounter::GIANT_HEAD,     // SLOW: +1 per card, x(1+0.1N), zeroed each round
        };
        std::vector<CardId> batch35Deck = BATCH_1;
        batch35Deck.push_back(CardId::SPOT_WEAKNESS);
        act3Variants.push_back({batch35Deck, 40, false, batch35Encounters});

        // Variant 36: batch 36 of the engine repo — the two act-3 ELITES, paired because
        // between them they close the last two "shape" gaps act 3 still had.
        //
        //   NEMESIS      -> MS::INTANGIBLE on the MONSTER side. Four cooperating sites, and
        //                   the reference's own enum comment ("differs from the game in that
        //                   it always decrements at end of round") says the shape is not the
        //                   player's:
        //                     * Monster::attacked  (Monster.cpp:418-422) and Monster::damage
        //                       (:477-481) each clamp `damage = 1` BEFORE block absorption,
        //                       and BEFORE the Angry hook — position is the whole observable
        //                       surface of an onAttacked-family clause;
        //                     * BattleContext::calculateCardDamage (:2768-2770) FLOORS the
        //                       precomputed number at 1 — `std::max`, not `std::min`, and it
        //                       sits AFTER Flight's x0.5;
        //                     * Monster::applyEndOfTurnTriggers (:55-57) decrements it, the
        //                       FOURTH statement (Metallicize / Malleable / Plated Armor /
        //                       INTANGIBLE / Regen / Shackled).
        //                   ⚠ And all three Nemesis moves re-apply it behind `if
        //                   (!hasStatus<INTANGIBLE>())`, but ATTACK/SCYTHE do it with
        //                   `addToBot(BuffEnemy)` AFTER `addToBot(RollMove)` while DEBUFF does
        //                   it with a plain synchronous `buff<>()` AFTER a synchronous
        //                   `rollMove(bc)` (MonsterSpecific.cpp:1585-1607). Same guard, three
        //                   different queueing shapes.
        //                   ⚠ NEMESIS_DEBUFF is also the project's first
        //                   `MakeTempCardInDiscard(..., n).actFunc(bc)` on the MONSTER side
        //                   (5/3 Burns, synchronous).
        //   REPTOMANCER  -> THE FOURTH SUMMON FAMILY, and the fourth way of reserving slots:
        //                     `++monsterCount; createMonster(DAGGER);
        //                      createMonster(REPTOMANCER); ++monsterCount;
        //                      createMonster(DAGGER);`      (MonsterGroup.cpp:339-345)
        //                   leaves slots 0 and 3 empty, daggers at 1 and 4, the caster in the
        //                   MIDDLE at 2, monsterCount = 5 / monstersAlive = 3. No other
        //                   encounter in the reference has a 5-wide group or two separate
        //                   holes with live monsters on both sides of them.
        //                   ⚠ Monster::reptomancerSummon (MonsterSpecific.cpp:3589-3608)
        //                   disagrees with all three earlier hosts on every axis: it is a
        //                   member function called SYNCHRONOUSLY, the count is
        //                   `asc18 ? 2 : 1` (the only ascension-dependent summon count), the
        //                   search order is the hard-coded {4, 1, 3, 0}, the intent comes from
        //                   `setMove(DAGGER_STAB)` while the aiRng is repaid by a
        //                   `bc.noOpRollMove()` INSIDE the loop, and — uniquely — it sets
        //                   `monsters.skipTurn` so a dagger dropped into a slot the turn
        //                   cursor has not reached yet does NOT act this round. That bitset
        //                   (MonsterGroup.h:24, read in MonsterGroup::doMonsterTurn :572-578
        //                   and cleared in BattleContext.cpp:805 once every monster has acted)
        //                   has exactly one writer in the whole reference and this is it.
        //                   ⚠ Unlike BRONZE_ORB and TORCH_HEAD, DAGGER is BOTH pre-placed and
        //                   summoned — two of them start on the field. So "the summon does not
        //                   re-run preBattleAction" is provably a no-op here rather than a
        //                   difference: DAGGER's preBattleAction is `buff<MINION>()`, which
        //                   the summon already does by hand.
        //                   ⚠ REPTOMANCER is also the LAST unregistered host of
        //                   `hpDiscardRoll` (`hpRng.random(180, 190)` then
        //                   `setRandomHp(hpRng, asc >= 8)`, MonsterSpecific.cpp:104-107).
        //
        // ⚠ WHY THESE TWO TOGETHER: Intangible lives in the damage-entry clamps and the
        // end-of-turn triggers; the fourth summon family lives in createMonsters / one
        // takeTurn case / the monster-turn loop. No shared statement, so a red diff still
        // points at exactly one of them. And DAGGER_STAB / DAGGER_EXPLODE are both in the
        // isMoveAttack whitelist while NEMESIS_DEBUFF and REPTOMANCER_SUMMON are not — the
        // SPOT_WEAKNESS in the deck keeps that classification under an oracle, and
        // DAGGER_EXPLODE in particular is the exact mirror of batch 32's EXPLODER_EXPLODE
        // (same "hit the player, then SuicideAction" shape, opposite whitelist membership,
        // because this one goes through attackPlayerHelper).
        //
        // ⚠⚠ FINGERPRINT COLLISION RULE (same as variants 32..35): deck + ascension +
        // targetPolicy here are byte-identical to variants 24..29 and 32..35, so this
        // encounter list must stay disjoint from all of theirs. NEMESIS and REPTOMANCER are
        // named by no other variant, and JAW_WORM_HORDE is deliberately not here.
        const std::vector<MonsterEncounter> batch36Encounters {
            MonsterEncounter::NEMESIS,      // monster-side INTANGIBLE, 4 cooperating sites
            MonsterEncounter::REPTOMANCER,  // summon family #4 + the skipTurn bitset
        };
        std::vector<CardId> batch36Deck = BATCH_1;
        batch36Deck.push_back(CardId::SPOT_WEAKNESS);
        act3Variants.push_back({batch36Deck, 40, false, batch36Encounters});

        // Variant 37: batch 37 of the engine repo — THE AWAKENED ONE, alone, because it is the
        // last unregistered writer of `Monster::halfDead` and it writes it from the ONE branch
        // of Monster::die that no other monster can reach.
        //
        //   AWAKENED_ONE -> `Monster::die`'s FIRST branch (Monster.cpp:285-292):
        //                     if (id == AWAKENED_ONE && !miscInfo) {   // stage 1
        //                         halfDead = true; removeDebuffs();
        //                         removeStatus<MS::CURIOSITY>();
        //                         setMove(AWAKENED_ONE_REBIRTH);
        //                         bc.cardQueue.clear();
        //                     } else if (monstersAlive == 0 || hasStatus<MINION_LEADER>()) {
        //                         bc.outcome = PLAYER_VICTORY; return;
        //                     }
        //                   ⚠ It sits BEFORE the victory `return`, so it is the only path in
        //                   the whole reference that can take a monster off the field without
        //                   ever consulting monstersAlive — killing the last monster on the
        //                   board does NOT end the fight if that monster is a stage-1 Awakened
        //                   One. Darkling's REGROW (batch 34) lives in the else-if chain AFTER
        //                   that return and therefore can only fire while a friend is alive;
        //                   this one is the exact opposite shape. Same field, opposite gate.
        //                   ⚠ It is keyed on `id == AWAKENED_ONE && !miscInfo`, not on a
        //                   status bit — the reference's own `// todo change to status` says
        //                   so. Transcribe the id test, not a REGROW-style power.
        //                   ⚠ `bc.cardQueue.clear()` is the reference's only monster-side
        //                   card-queue wipe (the other caller is Actions::ClearCardQueue).
        //                   ⚠ AWAKENED_ONE_REBIRTH then rewrites maxHp/curHp, flips halfDead
        //                   back, latches `miscInfo = true`, `strength = std::max(0,strength)`,
        //                   `++monstersAlive`, `buff<MINION_LEADER>()` — so stage 2 leaves the
        //                   fight through the OTHER branch (MINION_LEADER wins on the spot even
        //                   with both Cultists still standing).
        //                   ⚠ MS::CURIOSITY is new, and it is a PURE MARKER in the reference:
        //                   its only reader (BattleContext.cpp:1909-1912, "+strength when the
        //                   player plays a Power card") is COMMENTED OUT. It still shows up in
        //                   the monster snapshot and is still removed by die, so it has to be
        //                   modelled — but its effect must NOT be, there is no oracle for it.
        //                   ⚠ MS::REGEN is new too and is NOT a marker: Monster::
        //                   applyEndOfTurnTriggers heals `getStatus<REGEN>()` as its FIFTH
        //                   statement (Metallicize / Malleable / Plated Armor / Intangible /
        //                   REGEN / Shackled). 10 HP per round at ascension 0.
        //                   ⚠ AWAKENED_ONE_SLUDGE shuffles a VOID into the draw pile — the
        //                   first status card in the corpus whose ON-DRAW effect matters
        //                   (`bc.player.energy = std::max(0, energy-1)`, CardManager.cpp:426).
        //
        // ⚠ WHY THIS ENCOUNTER RATHER THAN THE MONSTER ALONE: MonsterGroup.cpp:179-184 builds
        // CULTIST, CULTIST, AWAKENED_ONE — the boss is at index 2, so the default policy
        // (firstAliveMonster) chews through both Cultists first. That is what makes the fight
        // long enough for REGEN and for the stage-1 death to be reached at all.
        //
        // ⚠⚠ FINGERPRINT COLLISION RULE (same as variants 32..36): deck + ascension +
        // targetPolicy here are byte-identical to variants 24..29 and 32..36, so this
        // encounter list must stay disjoint from all of theirs. AWAKENED_ONE is named by no
        // other variant, and JAW_WORM_HORDE is deliberately not here.
        //
        // ⚠⚠ THIS IS THE FIRST ACT-3 VARIANT THAT DOES **NOT** REUSE `BATCH_1 + SPOT_WEAKNESS`,
        // and the reason is measured, not stylistic: with that 22-card deck the Awakened One's
        // ENTIRE SECOND PHASE IS STRUCTURALLY UNREACHABLE. Measured over the same 120 traces
        // (40 seeds x 3 floors):
        //
        //   deck                                    avg turns   stage-1 deaths   REBIRTH exec
        //   BATCH_1 + SPOT_WEAKNESS (22 cards)          3.6           0 / 120            0
        //   + 6 strong cards, un-upgraded (28)          3.6           0 / 120            0
        //   this deck (45 cards, upgradeAll)            8.7          45 / 120           44
        //
        // The player is dead on turn 3-4 with the standard deck: two Cultists ramp +3 Strength
        // a round while the boss hits for 20-24, and 300 HP of stage-1 boss is simply not
        // reachable in four turns. REBIRTH / DARK_ECHO / SLUDGE / TACKLE all came back
        // "appeared 0 / executed 0" from check-coverage.mjs, i.e. no oracle at all for the
        // half-death branch this batch exists to install. This is the "focused deck" escape
        // hatch the engine repo's WORKFLOW.md describes (batch 5 / batch 7 used it for card
        // coverage); here it is used for MONSTER coverage, which is new.
        //
        // ⚠ TWO THINGS DRIVE THE DECK'S SHAPE, and both come from the policy, not from taste:
        //  ① `pickAction` plays the LEFTMOST playable card and then rescans from 0, i.e. it
        //     spends energy strictly left to right. A 3-cost card is therefore played only if
        //     it happens to be at hand index 0 at the start of a turn — DEMON_FORM and
        //     BARRICADE measured as near-dead cards (attempt 2 above changed nothing at all).
        //     So everything added here costs 0 or 1, except IMPERVIOUS/REAPER at 2.
        //  ② `upgradeAll = true`. Upgrades are the cheapest available power boost and they
        //     change one card qualitatively: LIMIT_BREAK+ does NOT exhaust, so doubling
        //     Strength recurs every deck cycle. Combined with four SPOT_WEAKNESS (+4 each,
        //     also non-exhausting) that is the whole damage engine.
        // ⚠ SPOT_WEAKNESS is in here four times rather than once. It is still the
        //   `Monster::isAttacking()` oracle every act-2/3 variant carries, but here it doubles
        //   as the Strength engine, so its coverage goes UP rather than down.
        // ⚠ IMPERVIOUS+ is exactly 40 Block and AWAKENED_ONE_DARK_ECHO is exactly 40 damage.
        //   That is what turns "the boss reborn, then the player died" into two or three more
        //   stage-2 turns — SLUDGE/TACKLE executions went 4/4 -> 18/17 when the third and
        //   fourth copies went in.
        //
        // ⚠ The deck's fingerprint (contents + upgraded flag) therefore differs from every
        // other variant, so the disjointness rule above is satisfied twice over.
        const std::vector<MonsterEncounter> batch37Encounters {
            MonsterEncounter::AWAKENED_ONE,  // half-death via Monster::die's FIRST branch
        };
        std::vector<CardId> batch37Deck = BATCH_1;
        batch37Deck.push_back(CardId::SPOT_WEAKNESS);
        for (CardId c : {CardId::SPOT_WEAKNESS, CardId::SPOT_WEAKNESS, CardId::SPOT_WEAKNESS,
                         CardId::LIMIT_BREAK, CardId::LIMIT_BREAK,
                         CardId::SWORD_BOOMERANG, CardId::SWORD_BOOMERANG,
                         CardId::GHOSTLY_ARMOR, CardId::GHOSTLY_ARMOR,
                         CardId::GHOSTLY_ARMOR, CardId::GHOSTLY_ARMOR,
                         CardId::GOOD_INSTINCTS, CardId::GOOD_INSTINCTS,
                         CardId::IMPERVIOUS, CardId::IMPERVIOUS,
                         CardId::IMPERVIOUS, CardId::IMPERVIOUS,
                         CardId::REAPER, CardId::REAPER,
                         CardId::FINESSE, CardId::FINESSE,
                         CardId::FLASH_OF_STEEL, CardId::FLASH_OF_STEEL}) {
            batch37Deck.push_back(c);
        }
        act3Variants.push_back({batch37Deck, 40, true, batch37Encounters});

        // Variant 38: batch 38 of the engine repo — TIME EATER, act 3's second boss and the
        // first monster in the whole project that changes the TURN STRUCTURE.
        //
        // 40 seeds, ascension 0, target policy 0, appended rather than folded into variant
        // 37's encounter list (traceIdx drives the relic/potion rotation).
        //
        //   TIME_EATER -> one monster, 456 HP (MonsterGroup.cpp:441-443, a bare
        //                 createMonster). preBattleAction is a single `buff<MS::TIME_WARP>(0)`
        //                 (MonsterSpecific.cpp:223-226), i.e. the "present, amount 0" shape
        //                 that Writhing Mass's REACTIVE and Giant Head's SLOW also use — it is
        //                 therefore INVISIBLE in the opening snapshot.
        //
        //   ⚠⚠ TIME_WARP settles in BattleContext::onAfterUseCard (BattleContext.cpp:1974-1985),
        //   INSIDE the `if (item.triggerOnUse)` gate, reading a hardcoded `monsters.arr[0]`:
        //       if (timeWarp == 11) { setStatus(0); buff<STRENGTH>(2); callEndTurnEarlySequence(); }
        //       else                { setStatus(timeWarp + 1); ++timeWarp; }
        //   Three things to copy verbatim: the threshold is `== 11` (so it fires on the 12th
        //   card played, not the 11th); `++timeWarp` is a LOCAL that nobody reads afterwards
        //   (dead code, transcribe as nothing); and callEndTurnEarlySequence (:2152-2161) ENDS
        //   THE PLAYER'S TURN IN THE MIDDLE OF A CARD PLAY. That last one is the batch's whole
        //   blast radius — it drains the card queue, turning `autoplay && !purgeOnUse` items
        //   into Actions::TimeEaterPlayCardQueueItem (which re-runs onAfterUseCard with
        //   triggerOnUse = false, so the card is DISCARDED WITHOUT BEING PLAYED) and dropping
        //   everything else, then pushes an end-turn item to the FRONT of the card queue.
        //
        //   The other three moves bring one new player-side debuff and two shapes:
        //     REVERBERATE  attackPlayerHelper(bc, asc4 ? 8 : 7, 3), queued RollMove.
        //     HEAD_SLAM    attackPlayerHelper(bc, asc4 ? 32 : 26) + queued
        //                  DebuffPlayer<PS::DRAW_REDUCTION>(1, true). DRAW_REDUCTION is new:
        //                  Player::debuff IGNORES the amount and just does `--cardDrawPerTurn`
        //                  + setHasStatus (Player.h:385-390), and afterMonsterTurns gives it
        //                  back one turn later, AFTER the DrawCards action has already been
        //                  queued with the reduced count (BattleContext.cpp:2227-2233).
        //     RIPPLE       synchronous addBlock(20) + three bare `bc.player.debuff<...>(1,true)`
        //                  calls, ending with a SYNCHRONOUS REAL rollMove(bc) — the sixth
        //                  turn-end shape (same as Maw's roar, not the noOpRollMove family).
        //     HASTE        `miscInfo = true; curHp = maxHp/2; if (asc19) addBlock(32);
        //                  removeDebuffs(); rollMove(bc);` — an ASSIGNMENT to curHp, not
        //                  Monster::heal, and a one-shot (its own latch closes the roll-table
        //                  gate `!usedHaste && curHp < maxHp/2`).
        //   isMoveAttack (MonsterMoves.h:524-525) holds REVERBERATE and HEAD_SLAM only.
        //
        // ⚠⚠ THE DECK IS THE SECOND ONE CHOSEN BY MEASUREMENT RATHER THAN HABIT (batch 37 was
        // the first), and it was measured over the same 120 traces (40 seeds x 3 floors):
        //
        //   deck                                   avg turns  TIME_WARP fires  < half HP  HASTE appear/exec
        //   BATCH_1 + SPOT_WEAKNESS (22)              4.89          122          1 / 120        0 / 0
        //   batch 37's focused deck (45, upgraded)    7.70          240         66 / 120      154 / 25
        //   + 4 FLASH_OF_STEEL + 4 FINESSE (53)       7.82          312         71 / 120      242 / 35
        //   this deck (59, upgraded)                  7.28          333         70 / 120      196 / 24
        //
        // The 22-card deck is not merely thin, it is STRUCTURALLY UNBACKED: the Time Eater
        // never reaches 228 HP, so HASTE comes back "appeared 0 / executed 0" and
        // check-coverage.mjs refuses the install. (TIME_WARP itself does fire there — 122
        // times — because it counts CARDS PLAYED, not damage.)
        //
        // ⚠ Two deck decisions beyond raw power, both aimed at code that would otherwise have
        // no oracle at all:
        //  ① 0-COST CANTRIPS (FLASH_OF_STEEL / FINESSE, 6 each). TIME_WARP counts cards, so
        //     cards-per-turn is the knob, and both of these draw a replacement. That is what
        //     takes the fire count from 240 to 330-ish and, with it, the number of turns that
        //     end mid-play (201 -> 288 turn-advancing card steps).
        //  ② HAVOC x4 and DOUBLE_TAP x2. These are the ONLY two producers in the whole project
        //     of a NON-EMPTY CARD QUEUE at the moment onAfterUseCard runs — Havoc pushes an
        //     `autoplay` item (playTopCardInDrawPile, :2555-2559) and Double Tap pushes a
        //     `purgeOnUse && autoplay` item (queuePurgeCard, :2792-2801). Without at least one
        //     of them, callEndTurnEarlySequence's `while` loop body is unreachable and both
        //     halves of its `item.autoplay && !item.purgeOnUse` filter are blind spots.
        //     Neither can conjure an unregistered card here (mayPlayHandCard's Havoc gate is
        //     satisfied: the deck is all-registered and the Time Eater adds no status cards
        //     below ascension 19), so the traces stay replayable.
        //
        // ⚠ Deck shape still obeys the two policy-derived rules from batch 37: everything
        // costs 0-2 (pickAction spends energy strictly left to right, so 3-cost cards are
        // near-dead), and upgradeAll is on (LIMIT_BREAK+ does not exhaust).
        //
        // ⚠⚠ FINGERPRINT COLLISION RULE: the deck contents differ from every other variant's,
        // so the disjointness requirement is satisfied twice over (TIME_EATER is also named by
        // no other variant, and JAW_WORM_HORDE is deliberately absent from this product).
        const std::vector<MonsterEncounter> batch38Encounters {
            MonsterEncounter::TIME_EATER,  // TIME_WARP + callEndTurnEarlySequence
        };
        std::vector<CardId> batch38Deck = BATCH_1;
        for (CardId c : {CardId::SPOT_WEAKNESS, CardId::SPOT_WEAKNESS,
                         CardId::SPOT_WEAKNESS, CardId::SPOT_WEAKNESS,
                         CardId::LIMIT_BREAK, CardId::LIMIT_BREAK,
                         CardId::SWORD_BOOMERANG, CardId::SWORD_BOOMERANG,
                         CardId::GHOSTLY_ARMOR, CardId::GHOSTLY_ARMOR,
                         CardId::GHOSTLY_ARMOR, CardId::GHOSTLY_ARMOR,
                         CardId::GOOD_INSTINCTS, CardId::GOOD_INSTINCTS,
                         CardId::IMPERVIOUS, CardId::IMPERVIOUS,
                         CardId::IMPERVIOUS, CardId::IMPERVIOUS,
                         CardId::REAPER, CardId::REAPER,
                         CardId::FINESSE, CardId::FINESSE,
                         CardId::FINESSE, CardId::FINESSE,
                         CardId::FINESSE, CardId::FINESSE,
                         CardId::FLASH_OF_STEEL, CardId::FLASH_OF_STEEL,
                         CardId::FLASH_OF_STEEL, CardId::FLASH_OF_STEEL,
                         CardId::FLASH_OF_STEEL, CardId::FLASH_OF_STEEL,
                         CardId::HAVOC, CardId::HAVOC, CardId::HAVOC, CardId::HAVOC,
                         CardId::DOUBLE_TAP, CardId::DOUBLE_TAP}) {
            batch38Deck.push_back(c);
        }
        act3Variants.push_back({batch38Deck, 40, true, batch38Encounters});

        // Variant 39: batch 39 of the engine repo — DONU AND DECA, act 3's third boss and
        // THE LAST ACT-3 ENCOUNTER. 40 seeds, ascension 0, target policy 0, appended rather
        // than folded into variant 38's encounter list (traceIdx drives the relic/potion
        // rotation, so growing an earlier variant's list would invalidate everything after it).
        //
        //   DONU_AND_DECA -> MonsterGroup.cpp:235-238, two bare createMonster calls, and the
        //                 ORDER IS THE REVERSE OF THE ENCOUNTER'S NAME:
        //                     createMonster(bc, MonsterId::DECA);   // arr[0]
        //                     createMonster(bc, MonsterId::DONU);   // arr[1]
        //                 250 HP each ({{250,250},{265,265}}, setRandomHp at the asc>=9 BOSS
        //                 tier), and they share ONE preBattleAction case
        //                 (MonsterSpecific.cpp:195-198): `buff<MS::ARTIFACT>(asc19 ? 3 : 2)`.
        //
        //   ⚠⚠ THE PAIR IS THE POINT: these are the reference's only two monsters that buff
        //   EACH OTHER, and the two halves have DIFFERENT SHAPES from the batch-26 pair
        //   (Centurion / Mystic) they superficially resemble:
        //     DECA_SQUARE_OF_PROTECTION (:1689-1699)
        //         auto &deca = *this; auto &donu = bc.monsters.arr[1];
        //         deca.addBlock(16); donu.addBlock(16);      // NO monstersAlive GATE AT ALL
        //         if (asc19) { both buff<PLATED_ARMOR>(3); }
        //         setMove(DECA_BEAM);
        //     DONU_CIRCLE_OF_POWER (:1677-1681)
        //         bc.monsters.arr[0].buff<MS::STRENGTH>(3); // "shouldn't matter if deca is dead"
        //         buff<MS::STRENGTH>(3);                    // NO GATE either
        //         setMove(DONU_BEAM);
        //   Centurion's defend is `if (getAliveCount() > 1) arr[1].addBlock(...)` and Mystic's
        //   heal/buff are `if (monstersAlive > 1) arr[0]...` — i.e. GATED. These two are not,
        //   and the reference's own comment on the Donu line says so. That difference is
        //   observable exactly when DECA is dead and DONU is not, which is what the deck below
        //   was chosen to produce.
        //
        //   ⚠ All four moves are on a strict 2-cycle with NO rollMove after the opening one:
        //   getMoveForRoll returns DECA_BEAM / DONU_CIRCLE_OF_POWER unconditionally
        //   (:3272-3278, the roll is drawn and thrown away), and every takeTurn case ends with
        //   a synchronous setMove. So coverage of the four moves is satisfied by turn 2 with
        //   ANY deck — the deck question here is NOT "do the moves execute" (batches 37/38's
        //   problem) but "does DECA ever die while DONU still lives".
        //
        // ⚠⚠ DECK: batch 38's 59-card upgraded deck, REUSED VERBATIM, and that was measured
        // rather than assumed. Seven candidates over the same 120 traces (40 seeds x 3 floors):
        //
        //   deck                                    avg turns  wins  DECA died  frames where a
        //                                                                        DEAD DECA's
        //                                                                        Strength grew
        //   BATCH_1 + SPOT_WEAKNESS (22, plain)        4.67       0     0 / 120         0
        //   batch 37's focused deck (45, upgraded)     6.88       9    22 / 120        20
        //   batch 38's deck (59, upgraded)  <-- THIS   7.44      32    56 / 120        58
        //   single-target tuned (53, upgraded)         7.17       7    43 / 120        35
        //   batch 38 + 4 HEAVY_BLADE + 2 IMPERVIOUS    7.52      23    44 / 120        53
        //   batch 38 - HAVOC/DOUBLE_TAP + 4 H.BLADE    6.87      14    35 / 120        28
        //   batch 38 + 4 GHOSTLY + 4 IMPERVIOUS        8.83      13    37 / 120        34
        //
        // The 22-card standard is structurally unbacked here for the same reason as batches
        // 37/38 — 500 HP of boss behind 16 Block every other turn while both of them ramp +3
        // Strength a round — but note WHAT is unbacked: not the moves (all four execute even
        // there, 1637/1041/973/1705 intent frames) but the ungated ally buff. Adding cards
        // beyond batch 38's 59 makes things WORSE: every extra card dilutes the draw, and the
        // two survival-leaning variants trade DECA kills for turns.
        //
        // ⚠⚠ FINGERPRINT COLLISION RULE, and this variant is the case the rule exists for:
        // its deck + ascension + targetPolicy are BYTE-IDENTICAL to variant 38's, so the two
        // encounter lists MUST be disjoint or split-traces.mjs would drop both variants' rows
        // into one file and variant0-rows.mjs would silently report a longer frozen prefix.
        // They are: variant 38 names TIME_EATER only, this one names DONU_AND_DECA only.
        // (Same situation as variants 24..29, which all shared BATCH_1 + SPOT_WEAKNESS.)
        const std::vector<MonsterEncounter> batch39Encounters {
            MonsterEncounter::DONU_AND_DECA,  // the ungated mutual buff/block pair
        };
        act3Variants.push_back({batch38Deck, 40, true, batch39Encounters});
    }

    // ---- product 5: the relic / potion line (batch 40) ---------------------------------
    //
    // A FIFTH PRODUCT, appended after act 3. That became possible only when batch 39 filled
    // act 3 in: while act 3 was still growing, `act3Variants` had to stay LAST (appending to
    // a product that is not last shifts every traceIdx the products behind it hand out).
    // `act3Variants` no longer grows, so the relic line hangs off its end.
    //
    // ⚠ The rule itself has not changed, only which product it names: new work always goes
    // into a product appended AFTER the current last one, never into an earlier product's
    // variant list.
    //
    // Why a new product rather than more act-3 variants: the relic line needs encounters
    // from ALL THREE ACTS in one place (LARGE_SLIME / SLIME_BOSS in act 1, GREMLIN_LEADER /
    // AUTOMATON / COLLECTOR in act 2, REPTOMANCER / THREE_DARKLINGS / WRITHING_MASS in act 3
    // — the reference's seven Philosopher's Stone call sites are spread across exactly those),
    // and a product is bound to one encounter list.
    //
    // Listing all 54 encounters up front is free for the same reason act2Encounters lists
    // all 19: an encounter no variant names is `continue`d before the seed loop and burns no
    // traceIdx.
    std::vector<std::pair<MonsterEncounter, const char *>> relicEncounters = encounters;
    relicEncounters.insert(relicEncounters.end(), act2Encounters.begin(), act2Encounters.end());
    relicEncounters.insert(relicEncounters.end(), act3Encounters.begin(), act3Encounters.end());

    std::vector<DeckVariant> relicVariants;
    {
        // The deck is the act-2/act-3 standard, VERBATIM: BATCH_1 + SPOT_WEAKNESS, 22 cards,
        // un-upgraded. That is deliberate — this batch's question is "does the reference react
        // to a relic", not "can the player get through the fight", and every encounter named
        // below already has a committed file proving its moves execute under exactly this deck
        // (variants 23..29 for act 2, 32..36 for act 3, variant 0 for act 1).
        //
        // ⚠ Philosopher's Stone does change the fight — initRelics gives it `energyPerTurn++`
        // — so the traces are NOT a re-run of the existing files with a relic bolted on. That
        // is fine and expected; what matters is that the same encounters were already known to
        // reach the code being tested with a WEAKER player.
        std::vector<CardId> relicDeck = BATCH_1;
        relicDeck.push_back(CardId::SPOT_WEAKNESS);

        // ---- relic set 1: Philosopher's Stone + Gremlin Horn + Hand Drill ----------------
        //
        // THREE RELICS AT ONCE, because relics stack and each one wants a different situation.
        // `gc.relics.add` is a bare `std::vector::push_back` + a bit set (Game.cpp:14-17) —
        // no cap, no side effect — and none of these three is an `atBattleStart` relic, so the
        // fixed_list<RelicId,8> in initRelics is untouched.
        //
        //   PHILOSOPHERS_STONE — BattleContext.cpp:198-204 (initRelics: EVERY slot up to
        //     monsterCount gets buff<STRENGTH>(1), and energyPerTurn++), plus SEVEN more call
        //     sites that buff monsters which appear LATER. The encounter list below is exactly
        //     "one host per call site":
        //       largeSlimeSplit     :3406  -> LARGE_SLIME  (and SLIME_BOSS's children re-split)
        //       slimeBossSplit      :3433  -> SLIME_BOSS
        //       SummonGremlins Actions.cpp:488 -> GREMLIN_LEADER
        //       spawnBronzeOrbs     :3454  -> AUTOMATON
        //       SpawnTorchHeads Actions.cpp:517 -> COLLECTOR
        //       reptomancerSummon   :3601  -> REPTOMANCER
        //       DARKLING_REINCARNATE:1494  -> THREE_DARKLINGS
        //     ⚠⚠ The initRelics loop has NO isTargetable()/isAlive() filter, so the
        //     RESERVED-BUT-NEVER-CONSTRUCTED slots get Strength too (Gremlin Leader's slot 0,
        //     the Automaton's 0/2, the Collector's 0/1, Reptomancer's 0/3) and it shows up in
        //     the snapshot on an `"id":"INVALID = 0"` row. Four of the eight encounters below
        //     have such a slot on purpose. (The relic one line above, BRIMSTONE, DOES filter —
        //     they are two cases of the same switch.)
        //
        //   GREMLIN_HORN — Monster.cpp:317-320, two separate addToBot (GainEnergy(1), then
        //     DrawCards(1)) at the very END of Monster::die, i.e. AFTER the victory `return`.
        //     It therefore needs a monster to die WHILE A COMPANION LIVES — the same constraint
        //     as Spore Cloud (batch 16). Every encounter below except SPHERIC_GUARDIAN is
        //     multi-monster, or becomes multi-monster by splitting/summoning.
        //
        //   HAND_DRILL — Monster.cpp:433-435 and :488-490, the SAME three lines on both damage
        //     paths. Needs a monster that HAS block and has it chipped to exactly 0. Two hosts
        //     here: SPHERIC_GUARDIAN (Barricade, so its block accumulates and is the only thing
        //     in the corpus that reliably sits above zero) and GREMLIN_LEADER (Shield Gremlin's
        //     PROTECT hands block to a random ally). ⚠ The gate is cumulative, not "one hit
        //     bigger than the block" — each hit takes min(block, damage) off, so the hit that
        //     brings it to 0 is the one that fires.
        const std::vector<MonsterEncounter> relicSet1Encounters {
            MonsterEncounter::LARGE_SLIME,      // largeSlimeSplit
            MonsterEncounter::SLIME_BOSS,       // slimeBossSplit (+ its children re-split)
            MonsterEncounter::SPHERIC_GUARDIAN, // Barricade block -> Hand Drill
            MonsterEncounter::GREMLIN_LEADER,   // SummonGremlins + Shield Gremlin block
            MonsterEncounter::AUTOMATON,        // spawnBronzeOrbs
            MonsterEncounter::COLLECTOR,        // SpawnTorchHeads
            MonsterEncounter::THREE_DARKLINGS,  // DARKLING_REINCARNATE
            MonsterEncounter::REPTOMANCER,      // reptomancerSummon
        };
        relicVariants.push_back({relicDeck, 40, false, relicSet1Encounters, 0, 0,
                                 {{RelicId::PHILOSOPHERS_STONE, "philosophers_stone"},
                                  {RelicId::GREMLIN_HORN,       "gremlin_horn"},
                                  {RelicId::HAND_DRILL,         "hand_drill"}},
                                 {}, 1});

        // ---- relic sets 2 and 3: the Omamori A/B pair -------------------------------------
        //
        // WRITHING_MASS_IMPLANT (MonsterSpecific.cpp:1539-1548) is the reference's only reader
        // of either relic:
        //     miscInfo = true;
        //     if (!bc.player.hasRelic<R::OMAMORI>()) {
        //         if (bc.player.hasRelic<R::DARKSTONE_PERIAPT>()) { bc.player.increaseMaxHp(6); }
        //     }
        // The committed writhing_mass.jsonl already pins the "neither relic" side (the whole
        // branch is a no-op there). These two variants pin the other two corners:
        //
        //   set 2 = Darkstone Periapt (+ Hand Drill)          -> maxHp/curHp both +6 on implant
        //   set 3 = set 2 PLUS Omamori                        -> nothing happens
        //
        // ⚠⚠ SET 3 IS SET 2 PLUS EXACTLY ONE RELIC, and the potions are PINNED to the same
        // value in both, so the two files are identical inputs apart from Omamori. That makes
        // the evidence a direct diff rather than a mutation: any line that differs between
        // writhing_mass@relic2 and writhing_mass@relic3 is a line where the Omamori gate
        // changed the outcome. Without pinning, the rotation would hand the two variants
        // DIFFERENT potions (their traceIdx values differ) and the comparison would be worthless.
        //
        // ⚠⚠ OMAMORI NEEDS data >= 1. initRelics' case for it is
        // `p.setHasRelic<R::OMAMORI>(r.data)` (BattleContext.cpp:185-186) — it OVERWRITES the
        // bit copied from the run-level container, so an Omamori added with data 0 is invisible
        // to `player.hasRelic<>()` and set 3 would be a byte-for-byte copy of set 2. 2 is the
        // number of charges the real game hands out. (LIZARD_TAIL is the only other relic with
        // this shape.)
        //
        // ⚠ Hand Drill rides along in both because Writhing Mass gives ITSELF 16/18 block with
        // WRITHING_MASS_FLAIL and the policy attacks it directly — the cheapest second host for
        // the block-broken gate, at zero extra files.
        const std::vector<MonsterEncounter> writhingMassOnly {
            MonsterEncounter::WRITHING_MASS,
        };
        const std::vector<Potion> pinnedPotions { Potion::BLOCK_POTION };
        relicVariants.push_back({relicDeck, 40, false, writhingMassOnly, 0, 0,
                                 {{RelicId::DARKSTONE_PERIAPT, "darkstone_periapt"},
                                  {RelicId::HAND_DRILL,        "hand_drill"}},
                                 pinnedPotions, 2});
        relicVariants.push_back({relicDeck, 40, false, writhingMassOnly, 0, 0,
                                 {{RelicId::DARKSTONE_PERIAPT, "darkstone_periapt"},
                                  {RelicId::HAND_DRILL,        "hand_drill"},
                                  {RelicId::OMAMORI,           "omamori", 2}},
                                 pinnedPotions, 3});

        // ---- relic set 4 (batch 41): Brimstone + the four in-turn-counter relics ---------
        //
        // Two unrelated things ride in one variant because relics stack for free
        // (RelicContainer::add is push_back + a bit, Game.cpp:14-17) and NONE of these five
        // is an atBattleStart relic, so initRelics' fixed_list<RelicId,8> is untouched.
        //
        //   BRIMSTONE — the DELIBERATE COUNTERPART to batch 40's Philosopher's Stone. Both
        //     relics run the same shaped loop over `monsters.arr[i]` for `i < monsterCount`,
        //     in the SAME switch, and only one of them filters:
        //         BattleContext.cpp:126-134  BRIMSTONE           `if (m.isTargetable())`
        //         BattleContext.cpp:198-204  PHILOSOPHERS_STONE  no filter at all
        //     Batch 40 pinned the unfiltered side (adding an alive filter to the Stone reddens
        //     480 traces). This variant pins the OTHER side, so the encounter list below is
        //     exactly the four encounters that RESERVE A SLOT THAT IS NEVER CONSTRUCTED —
        //     the only place in the corpus where "filter" and "no filter" differ at all:
        //         GREMLIN_LEADER  slot 0
        //         AUTOMATON       slots 0 and 2
        //         COLLECTOR       slots 0 and 1
        //         REPTOMANCER     slots 0 and 3
        //     ⚠ BRIMSTONE HAS A SECOND CALL SITE, and it is the one the real game's card text
        //     describes: Player::applyStartOfTurnRelics (Player.cpp:498-505) repeats the exact
        //     same body at the start of EVERY player turn. initRelics covers turn 1, the
        //     start-of-turn hook covers turns 2+. Both are needed; either alone is wrong.
        //
        //   KUNAI (:1702) / ORNAMENTAL_FAN (:1714) / SHURIKEN (:1718) — three separate `if`s
        //     in onUseAttackCard, all reading `p.attacksPlayedThisTurn % 3 == 0`, with other
        //     relics interleaved between them (ORANGE_PELLETS sits between KUNAI and the FAN).
        //     LETTER_OPENER (:1828) is the skill-side twin and reads p.skillsPlayedThisTurn.
        //     ⚠ Both counters are incremented at the TOP of their handler (:1643 / :1769),
        //     BEFORE any reader, so the first attack of a turn reads 1 rather than 0. Reversing
        //     that order would make `% 3 == 0` fire on the turn's first card.
        //
        // ⚠⚠ THE DECK IS NOT THE ACT-2/3 STANDARD, AND THAT WAS MEASURED, NOT GUESSED.
        // LETTER_OPENER needs THREE SKILLS IN ONE TURN, which is a different gate from "three
        // attacks in one turn" — a batch-38 style measurement had to be run for each. Three
        // candidate decks, same five relics, same four encounters, 480 traces each:
        //
        //   deck                                        avg turns  attack-gate  skill-gate
        //   BATCH_1 + SPOT_WEAKNESS (22)                    6.64     969 / 449   59 /  56
        //   + 4 FLEX + 4 GOOD_INSTINCTS + 4 FINESSE (34)    7.17     722 / 418 1966 / 479
        //   the same 34 WITHOUT Brimstone                   9.54    1075 / 440 2628 / 479
        //                                                            (fires / traces reached)
        //
        // The 22-card deck does reach the skill gate (59 is not 0), but only in 56 of 480
        // traces; twelve 0-cost, non-exhausting skills raise it 33x at the cost of ~25% of the
        // attack gate. The third row is not a candidate — it only shows WHY the fights are
        // shorter here than in the @relic1 files: Brimstone hands the PLAYER +2 Strength every
        // turn, so the player kills faster and there are fewer turns, not more.
        // ⚠ Everything added costs 0 (pickAction spends energy strictly left to right, so a
        // 1-cost skill competes with the attacks) and NONE of the three exhausts, so they come
        // back every shuffle.
        const std::vector<MonsterEncounter> reservedSlotEncounters {
            MonsterEncounter::GREMLIN_LEADER,   // reserved slot 0
            MonsterEncounter::AUTOMATON,        // reserved slots 0, 2
            MonsterEncounter::COLLECTOR,        // reserved slots 0, 1
            MonsterEncounter::REPTOMANCER,      // reserved slots 0, 3
        };
        std::vector<CardId> batch41Deck = relicDeck;
        for (CardId c : {CardId::FLEX, CardId::FLEX, CardId::FLEX, CardId::FLEX,
                         CardId::GOOD_INSTINCTS, CardId::GOOD_INSTINCTS,
                         CardId::GOOD_INSTINCTS, CardId::GOOD_INSTINCTS,
                         CardId::FINESSE, CardId::FINESSE, CardId::FINESSE, CardId::FINESSE}) {
            batch41Deck.push_back(c);
        }
        relicVariants.push_back({batch41Deck, 40, false, reservedSlotEncounters, 0, 0,
                                 {{RelicId::BRIMSTONE,      "brimstone"},
                                  {RelicId::KUNAI,          "kunai"},
                                  {RelicId::ORNAMENTAL_FAN, "ornamental_fan"},
                                  {RelicId::SHURIKEN,       "shuriken"},
                                  {RelicId::LETTER_OPENER,  "letter_opener"}},
                                 {}, 4});

        // ---- relic set 5 (batch 42): Hand Drill + Bronze Scales, potions PINNED -----------
        //
        // ONE JOB: close batch 40's "Hand Drill on the Monster::damage path = 0 traces" blind
        // spot. The relic's three lines are duplicated VERBATIM on both damage paths
        // (Monster.cpp:433-435 attacked, :488-490 damage), and @relic1 could only reach the
        // first one because nothing in that loadout deals NON-ATTACK damage.
        //
        // BRONZE_SCALES is the cheapest producer of non-attack damage: Thorns is
        // `addToTop(Actions::DamageEnemy(enemyIdx, thorns))` from Player::attacked
        // (Player.cpp:227-229) and DamageEnemy calls Monster::damage (Actions.cpp:63-71).
        //
        // ⚠⚠ THE ENCOUNTER LIST WAS MEASURED, NOT GUESSED, AND THE GATE IS TWO GATES.
        // Thorns fires DURING THE MONSTER PHASE (it is a reaction to being attacked), and
        // MonsterGroup::applyPreTurnLogic zeroes every non-Barricade monster's block at the
        // START of that phase (Monster.cpp:19-21). So a monster only has block while thorns
        // is hitting it if it has BARRICADE, or if an ALLY that acts earlier in the same
        // phase handed it some. Then, separately, 3 damage has to bring that block to
        // EXACTLY 0 (`hadBlock && block == 0`). Instrumented run over ten candidates,
        // 120 traces each, Hand Drill + Bronze Scales, potions pinned so the only
        // Monster::damage caller is Thorns:
        //
        //   encounter              thorns hits  gate A (had block)  gate B (broke it)  traces
        //   SPHERIC_GUARDIAN            535            426                  27           25
        //   GREMLIN_LEADER             1373             94                  27           23
        //   SENTRY_AND_SPHERE           973            797                  21           21
        //   DONU_AND_DECA              1005            477                   0            0
        //   CENTURION_AND_HEALER        629             36                   0            0
        //   AUTOMATON                   899             30                   0            0
        //   CHAMP / JAW_WORM_HORDE / LAGAVULIN / WRITHING_MASS: gate A is 0
        //
        // Only three encounters clear BOTH gates, and they are the three below.
        //   * SPHERIC_GUARDIAN / SENTRY_AND_SPHERE — Barricade + 40 block at preBattleAction
        //     (MonsterSpecific.cpp:253-258), so its block survives into its own turn and the
        //     player grinds it down through the 1..3 window.
        //   * GREMLIN_LEADER — GREMLIN_LEADER_ENCOURAGE hands every minion 6 block
        //     synchronously (:715-724) and the leader is arr[3], i.e. acts AFTER them... but
        //     SHIELD_GREMLIN_PROTECT (arr[0..2]) hands an ally block mid-phase, and that ally
        //     may still be to act. Both gates measured non-zero, so it stays.
        // ⚠ DONU_AND_DECA has the FATTEST gate A (477) and still zero gate B: Deca's Square
        //   of Protection hands 16 block, and 3-per-hit never lands on 0 before the phase ends.
        //   "Has block" and "thorns finishes the block" really are two separate gates.
        //
        // ⚠⚠ POTIONS ARE PINNED TO BLOCK_POTION ON PURPOSE, and it is not cosmetic. The
        // rotation hands out Fire/Explosive potions, which ALSO call Monster::damage, and
        // pickAction drinks all three on turn 1 — that is enough to break e.g. sleeping
        // Lagavulin's 8 starting block and light this same gate for a reason that has nothing
        // to do with Thorns. Pinning a potion that never touches a monster makes the claim
        // "every Monster::damage block-break in these three files is Thorns" true BY
        // CONSTRUCTION. It has a second, free benefit: with both relics AND potions pinned,
        // nothing in this variant reads traceIdx, so its traces are identical no matter where
        // in the product order it sits — the measured numbers above carry over exactly.
        const std::vector<MonsterEncounter> thornsBlockEncounters {
            MonsterEncounter::SPHERIC_GUARDIAN,
            MonsterEncounter::SENTRY_AND_SPHERE,
            MonsterEncounter::GREMLIN_LEADER,
        };
        relicVariants.push_back({relicDeck, 40, false, thornsBlockEncounters, 0, 0,
                                 {{RelicId::HAND_DRILL,    "hand_drill"},
                                  {RelicId::BRONZE_SCALES, "bronze_scales"}},
                                 pinnedPotions, 5});

        // ---- relic set 6 (batch 42): Ink Bottle + Orange Pellets + the three counters -----
        //
        //   INK_BOTTLE — FOUR handlers, one per card type, with the SAME five lines each:
        //       onUseAttackCard        BattleContext.cpp:1694
        //       onUseSkillCard                        :1811
        //       onUsePowerCard                        :1889
        //       onUseStatusOrCurseCard                :1958   <- and this one is LAST in its
        //     function, after the Blue Candle / Medical Kit branch, and has NO Orange Pellets
        //     beside it. Plus initRelics :164 (`p.inkBottleCounter = r.data`).
        //
        //   ORANGE_PELLETS — THREE handlers (:1706 / :1819 / :1897) that only SET a bit in
        //     `orangePelletsCardTypesPlayed`, plus a FOURTH site that is a different thing
        //     entirely: Player::applyStartOfTurnRelics' `orangePelletsCardTypesPlayed.reset()`
        //     (Player.cpp:559), the LAST statement of that function.
        //
        // Both relics are here because they are the two that sit BETWEEN Kunai / Ornamental
        // Fan / Shuriken in onUseAttackCard, which batch 41 recorded as the closing condition
        // for its "relative order of the three counter relics" blind spot.
        //
        // ⚠⚠ THAT PREDICTION IS WRONG AND THIS VARIANT IS WHAT MEASURED IT. Reordering the
        // three only becomes observable if something between them READS strength/dexterity,
        // and the only such line in Player::removeDebuffs is the `if (< 0) set 0` clamp. Two
        // gates, both measured on the deck below (five encounters, 120 traces each):
        //   * Orange Pellets' `.all()` and `attacksPlayedThisTurn % 3 == 0` must fire on the
        //     SAME card. `.all()` needs Attack+Skill+Power since the last reset, so this needs
        //     roughly 3 attacks AND 3 skills AND 3 powers in one turn: measured 0..3 per
        //     encounter (11 in 1440 traces on the most 0-cost-heavy deck tried).
        //   * At that instant strength or dexterity must be NEGATIVE. The only registered
        //     source is LAGAVULIN_SIPHON_SOUL (-1/-1, MonsterSpecific.cpp:882-883), and the
        //     first Pellets fire after it clamps both back to 0. Measured 0 EVERYWHERE.
        // So that blind spot stays open; see TODOS. What this variant DOES close is a
        // neighbouring one nobody had written down: INK BOTTLE vs ORANGE PELLETS. Ink Bottle's
        // `DrawCards(1)` is queued first and Pellets' `RemovePlayerDebuffs` second, and
        // removeDebuffs clears PS::NO_DRAW — so with No Draw up, as-built draws NOTHING and
        // the swapped order draws a card. Measured 10..21 such cards per encounter once
        // BATTLE_TRANCE is in the deck (it is the only registered producer of NO_DRAW).
        //
        // ⚠⚠ THE DECK IS MEASURED (fifth time). Three candidates, same five relics, same five
        // encounters, 600 traces each:
        //
        //   deck                                   ink(A/S/P/Status)   pellets(A/S/P)  ink+pel  ink+pel
        //                                                                              same card  &NoDraw
        //   34 = batch 41's                        823/1473/55/55        136/99/176      50        0
        //   38 = + 4 BERSERK                       856/938/190/10        533/382/822    149        0
        //   42 = + 4 BERSERK + 4 BATTLE_TRANCE     812/1215/224/18       592/304/1038   153       81
        //
        // 42 wins on every axis that matters. BERSERK is the only registered 0-cost POWER, and
        // without it `.all()` almost never completes (176 -> 1038 power-side fires).
        // BATTLE_TRANCE is 0-cost, draws 3, and is the only NO_DRAW producer.
        // ⚠ The status/curse handler is the one that constrains the ENCOUNTER list rather than
        // the deck: SLIMED is the only status card the policy can play (CardInstance.cpp:329's
        // `id != SLIMED` exception), and of everything measured only SLIME_BOSS produces it in
        // quantity (96 plays across 42 traces on this deck; LARGE_SLIME 0..9, LOTS_OF_SLIMES
        // and SMALL_SLIMES 0). ⚠ Note the deck fights BACK here: the more 0-cost cards, the
        // faster the Slime Boss dies and the fewer Slimed get played (360 -> 96).
        const std::vector<MonsterEncounter> inkPelletEncounters {
            MonsterEncounter::SLIME_BOSS,  // the corpus' only real source of played SLIMED
            MonsterEncounter::CHAMP,       // longest fight -> most ink/pellet fires
            MonsterEncounter::LAGAVULIN,   // the only registered source of negative str/dex
        };
        std::vector<CardId> batch42Deck = relicDeck;
        for (CardId c : {CardId::FLEX, CardId::FLEX, CardId::FLEX, CardId::FLEX,
                         CardId::GOOD_INSTINCTS, CardId::GOOD_INSTINCTS,
                         CardId::GOOD_INSTINCTS, CardId::GOOD_INSTINCTS,
                         CardId::FINESSE, CardId::FINESSE, CardId::FINESSE, CardId::FINESSE,
                         CardId::BERSERK, CardId::BERSERK, CardId::BERSERK, CardId::BERSERK,
                         CardId::BATTLE_TRANCE, CardId::BATTLE_TRANCE,
                         CardId::BATTLE_TRANCE, CardId::BATTLE_TRANCE}) {
            batch42Deck.push_back(c);
        }
        relicVariants.push_back({batch42Deck, 40, false, inkPelletEncounters, 0, 0,
                                 {{RelicId::INK_BOTTLE,     "ink_bottle"},
                                  {RelicId::ORANGE_PELLETS, "orange_pellets"},
                                  {RelicId::KUNAI,          "kunai"},
                                  {RelicId::ORNAMENTAL_FAN, "ornamental_fan"},
                                  {RelicId::SHURIKEN,       "shuriken"}},
                                 {}, 6});

        // ---- relic sets 7..11 (batch 43): the 38 SINGLE-CALL-SITE relics ----------------
        //
        // THE BATCH IS 5-8x the size of the previous three, and the shape is different: instead
        // of one mechanism per variant, this is one FAMILY of relic per variant. What makes it
        // possible is that every relic below has exactly ONE read site in src/combat + include/
        // combat (measured: `grep -rn '\bNAME\b' src/combat include/combat` returns one line),
        // so each is one or two lines to transcribe. The ~49 multi-hook relics are NOT here.
        //
        // ⚠⚠ WHY FIVE VARIANTS AND NOT ONE. `RelicContainer::add` really is free (push_back +
        // a bit), but COVERAGE DENSITY IS NOT. Batch 41 measured Brimstone shortening fights
        // from 9.54 to 7.17 turns; stacking 38 relics would collapse every gate's denominator
        // at once, and several relics would never be observed. The grouping rules used:
        //   (a) effects must not mask each other  -> Runic Pyramid (never discard) and
        //       Unceasing Top (draw when hand is empty) are in DIFFERENT variants, and so are
        //       Orichalcum (`block <= 0` at end of turn) and the four block relics;
        //   (b) the fight must stay long enough  -> the five `energyPerTurn++` relics, which
        //       cut fights from ~6 to ~2.5 turns, are quarantined in @relic7 where every
        //       registered effect is visible in the FIRST snapshot anyway;
        //   (c) the encounter list must expose that group's gates.
        // ⚠ Also a hard cap: the harness refuses more than 8 relics per variant (initRelics'
        //   atBattleStart fixed_list<RelicId,8>), so 38 relics need >= 5 variants regardless.
        //
        // ⚠⚠ ENCOUNTERS AND GROUPS WERE MEASURED, NOT GUESSED (sixth time). An instrumented
        // build counted every gate over 22 candidate encounters x 120 traces, then again with
        // the five groups actually installed. The per-group tables are in TODOS; the headline
        // numbers that DECIDED something:
        //   * Odd Mushroom needs the PLAYER to be Vulnerable: CHAMP 577, COLLECTOR 378,
        //     HEXAGHOST 0, THREE_BYRDS 0. Two of the four obvious candidates are dead.
        //   * Ice Cream needs LEFTOVER energy: TIME_EATER 99/79 traces, everything else 1..22.
        //   * Bloody Idol needs HAND_OF_GREED to land a KILL: THREE_SENTRIES 85, SLIME_BOSS 82,
        //     CHAMP 9, DONU_AND_DECA 0.
        //   * Charon's Ashes / Strange Spoon need an EXHAUST event, and the 22-card deck only
        //     produces them in THREE_SENTRIES / SLIME_BOSS / DONU_AND_DECA (0 everywhere else).
        //   * Stone Calendar needs turn 6 to be reached: 117..120 of 120 in the long fights,
        //     1/120 in AWAKENED_ONE and 0/120 in DONU_AND_DECA.
        //
        // ⚠ Potions are pinned to BLOCK_POTION in all five, for the batch-42 reason: with both
        // relics and potions fixed, a variant reads no traceIdx at all, so the measured numbers
        // above carry over to the installed data EXACTLY, and re-ordering products later cannot
        // silently change these files.

        // ---- relic set 7: the initRelics one-liners --------------------------------------
        //
        // All nine fire unconditionally in BattleContext::initRelics' FIRST pass, so every one
        // of them is visible in the `initial` snapshot and none needs a long fight.
        //   BUSTED_CROWN :244 / COFFEE_DRIPPER :248 / CURSED_KEY :256 / FUSION_HAMMER :276 /
        //   RUNIC_DOME :206 — FIVE separate cases whose bodies are the identical
        //     `p.energyPerTurn++`. They are kept as five cases (not folded into one) so that
        //     dropping any single one is observable; +5 energy/turn is exactly why they are
        //     alone in this variant.
        //   MUTAGENIC_STRENGTH :288 — `buff<STRENGTH>(3)` + `debuff<LOSE_STRENGTH>(3)`; the
        //     second goes through the Artifact gate, the first does not.
        //   FOSSILIZED_HELIX :272 — `buff<BUFFER>(1)`; its two readers are in Player::damage
        //     (:196) and Player::attacked (:220), at DIFFERENT positions in the two functions.
        //   DATA_DISK :264 — `buff<FOCUS>(1)`. Focus is never READ in combat (its only reader,
        //     BIAS, has no producer), so this relic's entire observable surface is the
        //     `"FOCUS":1` row in the player snapshot.
        // ⚠ THREAD_AND_NEEDLE is deliberately NOT here: player-side Plated Armor needs unblocked
        //   ATTACK damage to decrement, and 8 energy per turn ends fights before that happens
        //   often. It rides in @relic11 instead.
        const std::vector<MonsterEncounter> batch43Set7 {
            MonsterEncounter::THE_GUARDIAN,
            MonsterEncounter::CHAMP,
        };
        relicVariants.push_back({relicDeck, 40, false, batch43Set7, 0, 0,
                                 {{RelicId::BUSTED_CROWN,       "busted_crown"},
                                  {RelicId::COFFEE_DRIPPER,     "coffee_dripper"},
                                  {RelicId::CURSED_KEY,         "cursed_key"},
                                  {RelicId::FUSION_HAMMER,      "fusion_hammer"},
                                  {RelicId::RUNIC_DOME,         "runic_dome"},
                                  {RelicId::MUTAGENIC_STRENGTH, "mutagenic_strength"},
                                  {RelicId::FOSSILIZED_HELIX,   "fossilized_helix"},
                                  {RelicId::DATA_DISK,          "data_disk"}},
                                 pinnedPotions, 7});

        // ---- relic set 8: the turn-boundary block / energy family -------------------------
        //
        //   ART_OF_WAR      Player.cpp:492  reads attacksPlayedThisTurn == 0 — and the counter
        //                                   reset sits AFTER this function (BattleContext.cpp
        //                                   :2240, self-annotated "this has to be here because
        //                                   some relics check this info"), so it reads LAST
        //                                   turn's count. Fires 21..34 times per encounter.
        //   CAPTAINS_WHEEL  :507  `bc.turn == 2`  -> 120/120 traces
        //   HORN_CLEAT      :529  `bc.turn == 1`  -> 120/120 traces
        //   CLOAK_CLASP     BattleContext.cpp:2067  block = hand size, queued
        //   CALIPERS        :2214  THIRD ARM of the block-clearing if/else-if chain
        //   THE_ABACUS      :2827  +6 block on every shuffle (147..357 per encounter)
        //   ICE_CREAM       Player.cpp:726  energy CARRIES OVER instead of being overwritten
        //   POCKETWATCH     Player.cpp:663  draw 3 if <= 3 cards were played LAST turn
        // ⚠ ORICHALCUM is NOT here on purpose: its gate is `block <= 0` at end of turn, and
        //   four block relics in one variant would starve it. It is in @relic10 instead.
        // ⚠ TIME_EATER is in the list for ICE_CREAM alone: it is the only measured encounter
        //   where the player regularly ends a turn with energy left (99 fires / 79 traces;
        //   next best is 22).
        const std::vector<MonsterEncounter> batch43Set8 {
            MonsterEncounter::THREE_SENTRIES,
            MonsterEncounter::TIME_EATER,
            MonsterEncounter::THREE_DARKLINGS,
        };
        relicVariants.push_back({relicDeck, 40, false, batch43Set8, 0, 0,
                                 {{RelicId::ART_OF_WAR,     "art_of_war"},
                                  {RelicId::CAPTAINS_WHEEL, "captains_wheel"},
                                  {RelicId::HORN_CLEAT,     "horn_cleat"},
                                  {RelicId::CLOAK_CLASP,    "cloak_clasp"},
                                  {RelicId::CALIPERS,       "calipers"},
                                  {RelicId::THE_ABACUS,     "the_abacus"},
                                  {RelicId::ICE_CREAM,      "ice_cream"},
                                  {RelicId::POCKETWATCH,    "pocketwatch"}},
                                 pinnedPotions, 8});

        // ---- relic set 9: the damage-modifier family --------------------------------------
        //
        //   STRIKE_DUMMY   BattleContext.cpp:2708  +3 on Strike-named cards, BEFORE Strength
        //   PAPER_PHROG    :2753  monster Vulnerable multiplier 1.5f -> 1.75f
        //   PAPER_KRANE    Monster.cpp:573  monster Weak multiplier 0.75f -> 0.6f
        //   ODD_MUSHROOM   Monster.cpp:581  PLAYER Vulnerable multiplier 1.5f -> 1.25f
        //   THE_BOOT       Monster.cpp:340  unblocked 1..4 -> 5   (note: `< 5`)
        //   TORII          Player.cpp:235   unblocked 1..5 -> 1   (note: `<= 5`)
        //   CHAMPION_BELT  BattleContext.h:294  Vulnerable on a monster also applies 1 Weak
        //   UNCEASING_TOP  BattleContext.cpp:825  draw 1 when the hand empties mid-turn
        // ⚠ Two pairs here look alike and are NOT: Paper Phrog / Odd Mushroom are two different
        //   functions (cards-hit-monster vs monster-hits-player) that happen to share the 1.5f
        //   default; The Boot's bound is exclusive and Torii's is inclusive.
        // ⚠ CHAMP is mandatory: it is the only measured encounter where the player is made
        //   Vulnerable often (577 hits vs 0 in HEXAGHOST and THREE_BYRDS), and it is also the
        //   only one where Unceasing Top fires more than a dozen times (81 / 50 traces).
        //   THREE_BYRDS carries The Boot (984) and Torii (1441); COLLECTOR is the second
        //   Vulnerable source (378) and the second Weak/Frail source.
        const std::vector<MonsterEncounter> batch43Set9 {
            MonsterEncounter::CHAMP,
            MonsterEncounter::THREE_BYRDS,
            MonsterEncounter::COLLECTOR,
        };
        relicVariants.push_back({relicDeck, 40, false, batch43Set9, 0, 0,
                                 {{RelicId::STRIKE_DUMMY,  "strike_dummy"},
                                  {RelicId::PAPER_PHROG,   "paper_phrog"},
                                  {RelicId::PAPER_KRANE,   "paper_krane"},
                                  {RelicId::ODD_MUSHROOM,  "odd_mushroom"},
                                  {RelicId::THE_BOOT,      "the_boot"},
                                  {RelicId::TORII,         "torii"},
                                  {RelicId::CHAMPION_BELT, "champion_belt"},
                                  {RelicId::UNCEASING_TOP, "unceasing_top"}},
                                 pinnedPotions, 9});

        // ---- relic set 10: the card / exhaust / heal family --------------------------------
        //
        //   BIRD_FACED_URN   BattleContext.cpp:1885  heal 2 on every POWER card
        //   MAGIC_FLOWER     Player.cpp:161          heal amount * 3 / 2 (INTEGER division)
        //   DUALITY          BattleContext.cpp:1736  +1 Dex and 1 LOSE_DEXTERITY per attack
        //   CHARONS_ASHES    :2850                   addToTop 3 damage to all, on every exhaust
        //   STRANGE_SPOON    :2016                   50% not to exhaust — burns cardRandomRng
        //   RUNIC_PYRAMID    :2519                   the hand is never discarded
        //   BLOODY_IDOL      Player.cpp:92           heal 5 whenever gold is gained
        //   ORICHALCUM       BattleContext.cpp:2081  +6 block if block is 0 at end of turn
        //
        // ⚠⚠ THE DECK IS DIFFERENT AND EVERY ADDITION HAS A NAMED JOB (sixth "measure first"):
        //   +4 BERSERK       the only registered 0-cost POWER; without it Bird-Faced Urn fires
        //                    ~80 times per encounter (only INFLAME is a power in the base deck)
        //                    and Magic Flower has almost nothing to amplify. With it: 448..600
        //                    urn heals and 345..441 amplified heals per encounter.
        //   +2 OFFERING      0-cost, EXHAUSTS, and costs 6 HP. It does three jobs at once:
        //                    feeds Charon's Ashes and Strange Spoon an exhaust event in every
        //                    encounter (not just the three that generate status cards), and
        //                    puts the player BELOW max HP so the heals are observable at all.
        //   +2 HAND_OF_GREED the ONLY in-combat caller of Player::gainGold, i.e. the only way
        //                    Bloody Idol can ever fire. It needs to land a KILL
        //                    (Actions.cpp:1123-1131), which is why the encounter list has two
        //                    multi-monster fights.
        // ⚠ Unceasing Top is deliberately in @relic9 instead: Runic Pyramid keeps the hand, so
        //   the two together would make the "hand is empty" gate structurally unreachable.
        const std::vector<MonsterEncounter> batch43Set10 {
            MonsterEncounter::SLIME_BOSS,
            MonsterEncounter::THREE_SENTRIES,
            MonsterEncounter::CHAMP,
        };
        std::vector<CardId> batch43Deck10 = relicDeck;
        for (CardId c : {CardId::BERSERK, CardId::BERSERK, CardId::BERSERK, CardId::BERSERK,
                         CardId::OFFERING, CardId::OFFERING,
                         CardId::HAND_OF_GREED, CardId::HAND_OF_GREED}) {
            batch43Deck10.push_back(c);
        }
        relicVariants.push_back({batch43Deck10, 40, false, batch43Set10, 0, 0,
                                 {{RelicId::BIRD_FACED_URN, "bird_faced_urn"},
                                  {RelicId::MAGIC_FLOWER,   "magic_flower"},
                                  {RelicId::DUALITY,        "duality"},
                                  {RelicId::CHARONS_ASHES,  "charons_ashes"},
                                  {RelicId::STRANGE_SPOON,  "strange_spoon"},
                                  {RelicId::RUNIC_PYRAMID,  "runic_pyramid"},
                                  {RelicId::BLOODY_IDOL,    "bloody_idol"},
                                  {RelicId::ORICHALCUM,     "orichalcum"}},
                                 pinnedPotions, 10});

        // ---- relic set 11: hp-loss hooks, debuff immunity, long fights ---------------------
        //
        //   RUNIC_CUBE        Player.cpp:307  addToTop DrawCards(1) on every hp loss
        //   SELF_FORMING_CLAY Player.cpp:303  buff<NEXT_TURN_BLOCK>(3) on every hp loss
        //   TURNIP            Player.h:371    Frail immunity  — and BEFORE the Artifact gate
        //   GINGER            Player.h:367    Weak immunity   — same, one line above
        //   THREAD_AND_NEEDLE BattleContext.cpp:354  buff<PLATED_ARMOR>(4) — PLAYER-side plated
        //                     armor, decremented in Player::attacked (:245) and turned into
        //                     block in callEndOfTurnActions (:2099). Two sites, one relic case.
        //   STONE_CALENDAR    :2087           `turn == 6` -> 52 damage to all
        // ⚠ Only six: the other two hp-loss relics (Centennial Puzzle, Red Skull) are multi-site
        //   and out of this batch's scope, and adding more block would starve Stone Calendar's
        //   "the fight is still going on turn 7" gate.
        // ⚠ Encounters chosen for the intersection of all four gates: CHAMP (frail 294 / weak
        //   224 / turn-6 120 / hp-loss 704), MAW (120 / 120 / 120 / 864), SLIME_BOSS
        //   (101 / 78 / 119 / 294). COLLECTOR also clears all four but is already carrying
        //   @relic9.
        const std::vector<MonsterEncounter> batch43Set11 {
            MonsterEncounter::CHAMP,
            MonsterEncounter::MAW,
            MonsterEncounter::SLIME_BOSS,
        };
        relicVariants.push_back({relicDeck, 40, false, batch43Set11, 0, 0,
                                 {{RelicId::RUNIC_CUBE,        "runic_cube"},
                                  {RelicId::SELF_FORMING_CLAY, "self_forming_clay"},
                                  {RelicId::TURNIP,            "turnip"},
                                  {RelicId::GINGER,            "ginger"},
                                  {RelicId::THREAD_AND_NEEDLE, "thread_and_needle"},
                                  {RelicId::STONE_CALENDAR,    "stone_calendar"}},
                                 pinnedPotions, 11});

        // ================= BATCH 44: the multi-site families ==============================
        //
        // Batch 43 ate every single-call-site relic. What is left in the combat directory is
        // ~40 MULTI-site relics (2..9 read points each), so a variant is no longer "eight
        // one-liners" — Pen Nib alone has nine sites and four of them are in different files.
        // Six variants, 29 relics.
        //
        // ⚠⚠ THE ENABLER IS `RelicInstance::data`. `initRelics` copies it into a Player
        // counter for eight relics and `updateRelicsOnExit` copies it back; before this batch
        // the engine's `bc.relics` was `string[]`, so that whole family degenerated. The trace
        // format now carries the NUMERIC half in `relicData` (emitted only when non-zero — no
        // committed variant has one, which is why all 150 files still reproduce byte-for-byte)
        // and leaves the BOOLEAN half (OMAMORI / LIZARD_TAIL, `setHasRelic<X>(r.data)`) to the
        // "listed implies non-zero" invariant enforced by the check in main(). See
        // `relicDataIsNumeric` at the top of this file.

        // ---- relic set 12: the cross-combat counters, with NON-ZERO data ------------------
        //
        //   HAPPY_FLOWER    BattleContext.cpp:148 (`= r.data + 1`!)  / Player.cpp:521
        //   INCENSE_BURNER  :156 (`= r.data` then `if (++x == 6)`)   / Player.cpp:535
        //   SUNDIAL         :218                                     / BattleContext.cpp:2835
        //   NUNCHAKU        :181                                     / :1740
        //   PEN_NIB         :189 (`if (r.data == 9)` takes a DIFFERENT branch) / :1686 / :1728
        //                                                            / :2729 / exit :550
        // ⚠⚠ THE DATA VALUES ARE THE POINT, not decoration. With data 0 every one of these
        //   reads is "= 0", i.e. indistinguishable from hard-coding 0 — exactly the reason
        //   batch 43 EXCLUDED Du-Vu Doll and Girya. The values here are chosen so that each
        //   read lands on a different observable:
        //     happy flower  1 -> counter starts at 2, so energy arrives on the player's 2nd
        //                        turn instead of the 3rd. Also pins the `+1`.
        //     incense burner 3 -> counter starts at 4, Intangible on the 3rd player turn
        //                        (a 22-card fight rarely reaches six turns from 0).
        //     sundial       1 -> energy on the 2nd shuffle instead of the 3rd.
        //     nunchaku      5 -> energy after 5 more attacks instead of 10.
        //     pen nib       9 -> the OTHER branch: PEN_NIB is buffed at frame 0 and the
        //                        counter starts at -1. That branch is unreachable with data 0.
        // ⚠ Encounters are the two longest 22-card fights measured in batch 43 (CHAMP 9.8
        //   turns, THREE_DARKLINGS 10.0): the sundial/nunchaku/incense gates all need turns.
        const std::vector<MonsterEncounter> batch44Set12 {
            MonsterEncounter::CHAMP,
            MonsterEncounter::THREE_DARKLINGS,
        };
        // ⚠ SACRED_BARK rides along here rather than with Neow's Lament (where it started):
        //   it is the one relic in the batch whose oracle is the POTIONS, and this variant is
        //   the only long-fight one that can afford three different pinned potions. Measured:
        //   with Neow's Lament the monsters are at 1 HP, so Fire Potion's `hasBark ? 40 : 20`
        //   kills either way and that constant becomes unobservable. Here CHAMP has 420 HP.
        // ⚠ Three DIFFERENT potions on purpose — three independent probes into the 33 `hasBark`
        //   ternaries: Fire 40/20 (monster hp), Block 24/12 (player block), Swift 6/3 (hand).
        const std::vector<Potion> barkPotions {
            Potion::FIRE_POTION, Potion::BLOCK_POTION, Potion::SWIFT_POTION,
        };
        relicVariants.push_back({relicDeck, 40, false, batch44Set12, 0, 0,
                                 {{RelicId::HAPPY_FLOWER,   "happy_flower",   1},
                                  {RelicId::INCENSE_BURNER, "incense_burner", 3},
                                  {RelicId::SUNDIAL,        "sundial",        1},
                                  {RelicId::NUNCHAKU,       "nunchaku",       5},
                                  {RelicId::PEN_NIB,        "pen_nib",        9},
                                  {RelicId::SACRED_BARK,    "sacred_bark"}},
                                 barkPotions, 12});

        // ---- relic set 13: the "player is losing HP / dying" family -----------------------
        //
        //   LIZARD_TAIL       BattleContext.cpp:177 (`setHasRelic<X>(r.data)`) / Player.cpp:339
        //   RED_SKULL         BattleContext.cpp:436 / Player.cpp:169 / :311   — THREE sites
        //   TUNGSTEN_ROD      Player.cpp:201 / :239 / :266                    — THREE sites
        //   CENTENNIAL_PUZZLE Player.cpp:294 (one-shot `setHasRelic(false)`)
        //   BIRD_FACED_URN    (already registered in batch 43) — it is here as Red Skull's
        //                     ONLY mid-fight heal source, see below.
        //
        // ⚠⚠ CHAMP is mandatory and the reason is Lizard Tail: its 375 committed asc-0 traces
        //   end in a player death EVERY time (TODOS, batch 29), and a relic that only fires
        //   `if (curHp <= 0)` has no other way to be observed.
        // ⚠⚠ RED SKULL'S THREE SITES NEED THREE DIFFERENT SITUATIONS, and the middle one is
        //   the hard one: `Player::heal` only pays the 3 Strength back when the player crosses
        //   from bloodied to un-bloodied. Potions cannot do it — `pickAction` drinks everything
        //   on turn 1, at full HP, where every heal is clamped away. The 4 BERSERK are here so
        //   that Bird-Faced Urn heals 2 over and over WHILE the player is bloodied.
        // ⚠ Tungsten Rod is deliberately in the same variant even though it makes the player
        //   survive longer: its three sites are on all three damage paths, and CHAMP/HEXAGHOST
        //   hit all three (attack, non-attack `DamagePlayer`, and loseHp from BLOODLETTING).
        const std::vector<MonsterEncounter> batch44Set13 {
            MonsterEncounter::CHAMP,
            MonsterEncounter::HEXAGHOST,
        };
        std::vector<CardId> batch44Deck13 = relicDeck;
        for (CardId c : {CardId::BERSERK, CardId::BERSERK, CardId::BERSERK, CardId::BERSERK}) {
            batch44Deck13.push_back(c);
        }
        relicVariants.push_back({batch44Deck13, 40, false, batch44Set13, 0, 0,
                                 {{RelicId::LIZARD_TAIL,       "lizard_tail", 1},
                                  {RelicId::RED_SKULL,         "red_skull"},
                                  {RelicId::TUNGSTEN_ROD,      "tungsten_rod"},
                                  {RelicId::CENTENNIAL_PUZZLE, "centennial_puzzle"},
                                  {RelicId::BIRD_FACED_URN,    "bird_faced_urn"}},
                                 pinnedPotions, 13});

        // ---- relic set 14: everything visible in the opening frames -----------------------
        //
        //   CLOCKWORK_SOUVENIR  :104 + :403  queued BuffPlayer<ARTIFACT>(1)
        //   GREMLIN_VISAGE      :105 + :407  SYNCHRONOUS `p.debuff<PS::WEAK>(1)`
        //   RED_MASK            :106 + :415  DebuffAllEnemy<WEAK>(1)
        //   RING_OF_THE_SNAKE   :107 + :419  DrawCards(2)
        //   RING_OF_THE_SERPENT :66 (init!) + :325 (an EMPTY case)  cardDrawPerTurn + 1
        //   SNECKO_EYE          :63 (init!) + :210  cardDrawPerTurn + 2, and Confused
        //   AKABEKO             :122        buff<VIGOR>(8)
        //   MERCURY_HOURGLASS   :432 + Player.cpp:551  DamageAllEnemy(3), TWO sites
        //
        // ⚠⚠ GINGER IS NOT HERE ON PURPOSE. Gremlin Visage's whole observable surface is one
        //   stack of Weak on the player, and Ginger is Weak immunity — together it would have
        //   ZERO evidence. (Ginger's own blind spot is closed in @relic15 instead.)
        // ⚠ Clockwork Souvenir's Artifact does NOT eat Gremlin Visage's Weak: the Visage case
        //   is synchronous and runs inside the loop, the Souvenir case is `addToBot`. Order is
        //   the whole difference and it is observable.
        // ⚠ Snecko Eye + Ring of the Serpent + Ring of the Snake stack to a 5+2+1 turn draw
        //   plus 2 on the first turn. That is deliberate: `cardDrawPerTurn` is read by
        //   `cards.init` BEFORE initRelics runs, so getting the two `init`-site relics wrong
        //   shifts the opening hand of every trace.
        // ⚠ THREE_SENTRIES is here for the monsters' ARTIFACT (Red Mask's Weak has to be eaten
        //   by it), CHAMP for the length.
        const std::vector<MonsterEncounter> batch44Set14 {
            MonsterEncounter::THREE_SENTRIES,
            MonsterEncounter::CHAMP,
        };
        relicVariants.push_back({relicDeck, 40, false, batch44Set14, 0, 0,
                                 {{RelicId::CLOCKWORK_SOUVENIR,  "clockwork_souvenir"},
                                  {RelicId::GREMLIN_VISAGE,      "gremlin_visage"},
                                  {RelicId::RED_MASK,            "red_mask"},
                                  {RelicId::RING_OF_THE_SNAKE,   "ring_of_the_snake"},
                                  {RelicId::RING_OF_THE_SERPENT, "ring_of_the_serpent"},
                                  {RelicId::SNECKO_EYE,          "snecko_eye"},
                                  {RelicId::AKABEKO,             "akabeko"},
                                  {RelicId::MERCURY_HOURGLASS,   "mercury_hourglass"}},
                                 pinnedPotions, 14});

        // ---- relic set 15: Artifact x (Ginger / Turnip / Champion Belt) + Mark of the Bloom -
        //
        // ⚠⚠ THIS VARIANT EXISTS TO CLOSE THREE BLIND SPOTS BATCH 43 LEFT OPEN, and the closing
        //   condition it wrote down is exactly this: "a variant that also carries an Artifact
        //   source". Clockwork Souvenir is that source (batch 44 registers it).
        //     GINGER / TURNIP  sit BEFORE the Artifact gate in Player::debuff (Player.h:367/371)
        //                      -> a Weak/Frail they block must cost ZERO Artifact charges.
        //     CHAMPION_BELT    sits at the end of `debuffEnemy` (BattleContext.h:294) -> the
        //                      extra Weak goes through Monster::addDebuff's own Artifact gate.
        //   MARK_OF_THE_BLOOM  Player.cpp:156 (heal returns early) / :331 (no fairy, no tail)
        //   MARK_OF_PAIN       :112 (energyPerTurn++) + :411 (two WOUNDs into the DRAW pile)
        //                      — the only relic in the reference that is in both initRelics
        //                      passes.
        //   BIRD_FACED_URN     again the heal source: with Mark of the Bloom every one of those
        //                      heals must become a no-op. Without a heal source that relic has
        //                      no observable surface at all.
        // ⚠ CHAMP is the encounter that applies BOTH Weak and Frail to the player often
        //   (batch 43 measured 224 / 294); THREE_SENTRIES carries the monster-side ARTIFACT 1.
        const std::vector<MonsterEncounter> batch44Set15 {
            MonsterEncounter::CHAMP,
            MonsterEncounter::THREE_SENTRIES,
        };
        std::vector<CardId> batch44Deck15 = relicDeck;
        for (CardId c : {CardId::BERSERK, CardId::BERSERK, CardId::BERSERK, CardId::BERSERK}) {
            batch44Deck15.push_back(c);
        }
        relicVariants.push_back({batch44Deck15, 40, false, batch44Set15, 0, 0,
                                 {{RelicId::CLOCKWORK_SOUVENIR, "clockwork_souvenir"},
                                  {RelicId::GINGER,             "ginger"},
                                  {RelicId::TURNIP,             "turnip"},
                                  {RelicId::CHAMPION_BELT,      "champion_belt"},
                                  {RelicId::MARK_OF_THE_BLOOM,  "mark_of_the_bloom"},
                                  {RelicId::MARK_OF_PAIN,       "mark_of_pain"},
                                  {RelicId::BIRD_FACED_URN,     "bird_faced_urn"}},
                                 pinnedPotions, 15});

        // ---- relic set 16: energy / cost / card-play family --------------------------------
        //
        //   ECTOPLASM      :136 + Player.cpp:87    energyPerTurn++, and gainGold returns early
        //   SOZU           :214 + :2249            energyPerTurn++, and obtainPotion refuses
        //   VELVET_CHOKER  :222 + :726             energyPerTurn++, and the 7th card is illegal
        //   WRIST_BLADE    :2712                   +4 damage on a card whose costForTurn is 0
        //   MUMMIFIED_HAND :1905 (+ :1833, an EMPTY `// todo`)  a random non-0-cost hand card
        //                                          becomes 0 for the turn — burns cardRandomRng
        //   WARPED_TONGS   :450 + Player.cpp:669   upgrade a random hand card — burns shuffleRng
        //   NECRONOMICON   :1722 + Player.cpp:555  the turn's first >=2-cost attack is replayed
        //   CHEMICAL_X     Actions.cpp:1267        Whirlwind hits `energy + 2` times
        //
        // ⚠⚠ POTIONS ARE PINNED TO ENTROPIC BREW, not to Block Potion, and that is Sozu's only
        //   oracle: `BattleContext::obtainPotion` is called from exactly one place in combat
        //   (the brew refilling the slots). With Sozu the refill is refused, so the potion
        //   slots stay empty for the rest of the fight — a diff you can see frame by frame.
        // ⚠⚠ THE DECK IS DIFFERENT AND EVERY ADDITION HAS A NAMED JOB:
        //   +2 WHIRLWIND     the only registered X-cost attack, i.e. Chemical X's only oracle.
        //   +2 HAND_OF_GREED the only in-combat `Player::gainGold` caller, i.e. Ectoplasm's.
        //   +4 BERSERK       the only registered 0-cost POWER card, i.e. the only way to make
        //                    Mummified Hand fire more than once or twice a fight.
        //   (ANGER is already in BATCH_1 and is a 0-cost ATTACK, which is what Wrist Blade
        //    needs; Mummified Hand manufactures more 0-cost cards as it goes.)
        // ⚠ Three `energyPerTurn++` stack to 6 energy. Batch 43 measured five of them dropping
        //   the average fight to 2.5 turns, so three is the ceiling here — and Velvet Choker's
        //   6-card cap pushes back in the other direction, which is itself the thing to observe.
        // ⚠ Three encounters, and each one is carrying a different gate (measured, 120 each):
        //   THREE_SENTRIES  Hand of Greed lands 46 KILLS / 41 traces -> Ectoplasm's only gate.
        //                   (CHAMP only manages 9 / 9: one 420-HP monster is hard to finish
        //                   with a 20-damage card.) Its fights are short (1.16 turns), which is
        //                   fine because Ectoplasm fires on the kill, not per turn.
        //   CHAMP           5.10 turns, 244 Whirlwinds, and the 6-card cap hit 68 times / 60
        //                   traces -> Velvet Choker and Chemical X.
        //   THREE_DARKLINGS the long one (10.0 turns on the plain deck) -> Warped Tongs and
        //                   Necronomicon's once-per-turn reset need turns, and three
        //                   `energyPerTurn++` make everything else end early.
        const std::vector<MonsterEncounter> batch44Set16 {
            MonsterEncounter::THREE_SENTRIES,
            MonsterEncounter::CHAMP,
            MonsterEncounter::THREE_DARKLINGS,
        };
        const std::vector<Potion> brewPotions { Potion::ENTROPIC_BREW };
        std::vector<CardId> batch44Deck16 = relicDeck;
        for (CardId c : {CardId::WHIRLWIND, CardId::WHIRLWIND,
                         CardId::HAND_OF_GREED, CardId::HAND_OF_GREED,
                         CardId::BERSERK, CardId::BERSERK, CardId::BERSERK, CardId::BERSERK}) {
            batch44Deck16.push_back(c);
        }
        relicVariants.push_back({batch44Deck16, 40, false, batch44Set16, 0, 0,
                                 {{RelicId::ECTOPLASM,      "ectoplasm"},
                                  {RelicId::SOZU,           "sozu"},
                                  {RelicId::VELVET_CHOKER,  "velvet_choker"},
                                  {RelicId::WRIST_BLADE,    "wrist_blade"},
                                  {RelicId::MUMMIFIED_HAND, "mummified_hand"},
                                  {RelicId::WARPED_TONGS,   "warped_tongs"},
                                  {RelicId::NECRONOMICON,   "necronomicon"},
                                  {RelicId::CHEMICAL_X,     "chemical_x"}},
                                 brewPotions, 16});

        // ---- relic set 17: Neow's Lament, on its own ---------------------------------------
        //
        //   NEOWS_LAMENT  :293 (`if (r.data > 0)` -> every monster's curHp = 1) + exit :540
        //
        // ⚠⚠ IT HAS TO BE ALONE, and that was measured: with every monster at 1 HP the fight is
        //   over before the first end-of-turn (0.00 turns, 1 step per trace), so any relic
        //   sharing this variant would have no turns to fire in. Sacred Bark started here and
        //   was moved to @relic12 for exactly that reason — Fire Potion's `hasBark ? 40 : 20`
        //   kills a 1-HP monster either way.
        // ⚠⚠ THE ENCOUNTERS ARE THE RESERVED-SLOT ONES ON PURPOSE. Neow's loop is a bare
        //   `i < monsterCount` with NO filter (same family as Philosopher's Stone, the opposite
        //   of Brimstone's `isTargetable()`), so the slots that were NEVER CONSTRUCTED —
        //   AUTOMATON's 0 and 2, COLLECTOR's 0 and 1 — also get `curHp = 1`. Measured: 360
        //   monster entries at hp 1 per file, of which **240 are those empty slots**, and they
        //   flip to `alive: true` because `isDying()` is `curHp <= 0`. That is the only place
        //   in the corpus where the missing filter is observable.
        // ⚠⚠⚠ AND THAT COMBINATION IS ALSO A LANDMINE — the potions below are load-bearing.
        //   A resurrected empty slot is targetable, so `firstAliveMonster` is slot 0 and the
        //   very first action kills a monster that was never part of `monstersAlive`. With a
        //   Fire Potion that lands on step 1 and the fight ends (`--monstersAlive` takes the
        //   real count 1 -> 0 = victory). Measured with a Block Potion instead, i.e. letting
        //   the player attack over several turns, COLLECTOR aborts the whole generator on its
        //   SECOND trace: `assert(false)` in BattleContext::executeActions:753, reached via
        //   `monsters.monstersAlive < 0`. That is not a reference bug to patch — it is a
        //   configuration the real game cannot produce (Neow's Lament never meets a reserved
        //   slot before the host is built). Keep the Fire Potion.
        const std::vector<MonsterEncounter> batch44Set17 {
            MonsterEncounter::AUTOMATON,
            MonsterEncounter::COLLECTOR,
        };
        relicVariants.push_back({relicDeck, 40, false, batch44Set17, 0, 0,
                                 {{RelicId::NEOWS_LAMENT, "neows_lament", 1}},
                                 barkPotions, 17});

        // ⚠⚠ THE 21 SINGLE-SITE RELICS THAT ARE **NOT** REGISTERED, and why (batch 43 screened
        // all 70 single-site relics; 11 were already done, 38 are above, these 21 are out):
        //   * needs orbs (no orb model anywhere): CRACKED_CORE, NUCLEAR_BATTERY,
        //     SYMBIOTIC_VIRUS, RUNIC_CAPACITOR, FROZEN_CORE.
        //   * needs a Stance (not modelled, and not in the snapshot): TEARDROP_LOCKET.
        //   * reads `r.data`, which the engine's `bc.relics` does not carry, and whose value
        //     from this harness is 0 -> the case is a NO-OP with no observable surface:
        //     DU_VU_DOLL (`buff<STRENGTH>(r.data)`), GIRYA (same).
        //   * reads `gc.curRoom` / `gc.lastRoom`, which this harness never sets (both stay
        //     Room::INVALID), so the branch is structurally unreachable: PANTOGRAPH (BOSS),
        //     PRESERVED_INSECT (ELITE), SLAVERS_COLLAR (ELITE|BOSS), SLING_OF_COURAGE (ELITE),
        //     ANCIENT_TEA_SET (lastRoom == REST). ⚠ CLOSING CONDITION: a `Room` field on
        //     DeckVariant (same default-preserves-bytes trick as `ascension`) plus a `room`
        //     field in the trace header would open all five at once.
        //   * conjures a card from the WHOLE POOL, which can be an unregistered card and would
        //     make the trace unreplayable: ENCHIRIDION, DEAD_BRANCH, NILRYS_CODEX (the last one
        //     also opens a CARD_SELECT screen).
        //   * the reference's implementation is COMMENTED OUT, so there is no oracle: MELANGE
        //     (`// addToBot(Actions::SetState(InputState::SCRY, 3))`, BattleContext.cpp:2832).
        //   * run-level, not combat: BLACK_BLOOD / BURNING_BLOOD / MEAT_ON_THE_BONE all live in
        //     the end-of-battle heal switch (BattleContext.cpp:569/575/581), which the trace
        //     format does not cover.
        //   * wedges the battle: THE_SPECIMEN, see the note below.

        // ⚠⚠ NOT REGISTERED, and the reason is NOT "it was not in the rotation": THE_SPECIMEN.
        // Monster::die ends with
        //     if (bc.player.hasRelic<RelicId::THE_SPECIMEN>()) {
        //         bc.addToBot( Actions::SetState(InputState::SELECT_ENEMY_THE_SPECIMEN_APPLY_POISON) );
        //     }
        // and that InputState appears exactly TWICE in the whole reference: this write and its
        // declaration in InputState.h:48. Nothing validates, enumerates or answers it, while
        // BattleContext::executeActions leaves its loop the moment inputState stops being
        // EXECUTING_ACTIONS (BattleContext.cpp:756-758). Handing this relic to a variant would
        // wedge every battle at the first monster death and truncate the traces. It belongs
        // with the cards the reference never implemented (SEEK et al): no oracle exists.
    }

    // ================= potion axis (batch 45), the SIXTH product =========================
    //
    // Same encounter list as the relic product — the potions being covered are not tied to any
    // particular monster, and listing all 54 up front is free (a variant that does not name an
    // encounter is `continue`d before the seed loop and burns no traceIdx).
    std::vector<std::pair<MonsterEncounter, const char *>> potionEncounters = relicEncounters;

    std::vector<DeckVariant> potionVariants;
    {
        // Same 22-card act-2/act-3 standard deck the relic line uses, verbatim. Nothing here
        // needs a special deck: what a potion does is (with two exceptions noted below) a
        // function of the player, not of what is standing in front of them.
        std::vector<CardId> potionDeck = BATCH_1;
        potionDeck.push_back(CardId::SPOT_WEAKNESS);

        // ⚠ Every variant below pins its relics too (the harness rejects an empty list; see
        // the check above for why). VAJRA is the inert choice: +1 Strength in initRelics and
        // not one further call site, so it cannot interact with any potion. The `@potN` B-side
        // of each pair adds SACRED_BARK and CHANGES NOTHING ELSE, which turns each pair into a
        // line-by-line A/B for the `hasBark ? A : B` ternary of all three potions it carries.
        const std::vector<RelicSpec> PLAIN {{RelicId::VAJRA, "vajra"}};
        const std::vector<RelicSpec> BARK  {{RelicId::VAJRA,       "vajra"},
                                            {RelicId::SACRED_BARK, "sacred_bark"}};

        const auto add = [&](int set, int policy, std::vector<Potion> pots,
                             const std::vector<RelicSpec> &relics,
                             std::vector<MonsterEncounter> encs) {
            potionVariants.push_back({potionDeck, 40, false, std::move(encs), 0, 0, relics,
                                      std::move(pots), 0, set, policy});
        };

        // ---- group 1: the potions that are DEAD under potionPolicy 0 ---------------------
        //
        // This is the group the timing axis exists for, and both members were measured to be
        // structurally unobservable under the historical policy (840 traces x 7 encounters,
        // numbers in TODOS):
        //
        //   BLOOD_POTION — `Player::heal` ends with `min(maxHp, ...)`. Under policy 0 all
        //     three are drunk on turn 0 at 80/80, so the heal applied was 0 in 360 / 360
        //     drinks. Under policy 1 it is 32 every single time (40% of 80, unclamped because
        //     the gate guarantees curHp <= 40) and the SACRED_BARK B-side is 16. That pair is
        //     what TODOS' "血之药水 × 神圣树皮" ruling was waiting for.
        //   LIQUID_MEMORIES — `BetterDiscardPileToHandAction` returns immediately on an empty
        //     discard pile, and on turn 0 before any card is played the discard pile is ALWAYS
        //     empty: 360 / 360 drinks did nothing. Under policy 1 the average discard pile is
        //     8.4 cards and the card-select branch fires 1361 times.
        //   GHOST_IN_A_JAR rides along to fill the third slot; it is one of the two potions
        //     Ironclad cannot roll from its own pool (Silent-only), so an explicit list is the
        //     only way to reach it at all.
        //
        // ⚠ Encounters are measured, not guessed. Under policy 1 the player must actually get
        // bloodied, and in 3 of the 7 candidates it usually does not: three_sentries left 112
        // of 120 traces with ZERO drinks, gremlin_nob 95, lagavulin 66. champ (0) and
        // three_darklings (0) are the two that always deliver.
        const std::vector<MonsterEncounter> G1_ENC {
            MonsterEncounter::CHAMP,
            MonsterEncounter::THREE_DARKLINGS,
        };
        const std::vector<Potion> G1 {
            Potion::BLOOD_POTION, Potion::LIQUID_MEMORIES, Potion::GHOST_IN_A_JAR,
        };
        add(1, 1, G1, PLAIN, G1_ENC);
        add(2, 1, G1, BARK,  G1_ENC);

        // ---- group 2: the three "stat + payback" potions ---------------------------------
        //
        //   FLEX_POTION  Strength +5 and LOSE_STRENGTH 5 (the debuff half goes through
        //                Artifact, so Artifact carriers keep the Strength for free).
        //   SPEED_POTION the same shape one enum apart, Dexterity / LOSE_DEXTERITY.
        //   ESSENCE_OF_STEEL player-side Plated Armor 4 — decremented in Player::attacked and
        //                turned into block in callEndOfTurnActions, so it needs a monster that
        //                actually hits. champ does; three_darklings adds the half-dead frames.
        const std::vector<Potion> G2 {
            Potion::FLEX_POTION, Potion::SPEED_POTION, Potion::ESSENCE_OF_STEEL,
        };
        add(3, 0, G2, PLAIN, {MonsterEncounter::CHAMP, MonsterEncounter::THREE_DARKLINGS});
        add(4, 0, G2, BARK,  {MonsterEncounter::CHAMP});

        // ---- group 3: turn-boundary and reaction Powers -----------------------------------
        //
        //   HEART_OF_IRON  Metallicize 6 — block at every turn end, so it wants a long fight.
        //   LIQUID_BRONZE  Thorns 3 — only visible when a monster attacks.
        //   FOCUS_POTION   Focus 2 — the reference never READS Focus in combat, so its whole
        //                  observable surface is the snapshot row. (Defect-only potion, same
        //                  "explicit list is the only way" note as Ghost In A Jar.)
        const std::vector<Potion> G3 {
            Potion::HEART_OF_IRON, Potion::LIQUID_BRONZE, Potion::FOCUS_POTION,
        };
        add(5, 0, G3, PLAIN, {MonsterEncounter::CHAMP, MonsterEncounter::HEXAGHOST});
        add(6, 0, G3, BARK,  {MonsterEncounter::CHAMP});

        // ---- group 4: the three Powers that need TURNS or CARDS after the drink ----------
        //
        //   REGEN_POTION       player-side Regen 5 — heals and decrements at every turn end
        //                      (the monster-side Regen does NOT decrement; two code paths, one
        //                      PowerId).
        //   CULTIST_POTION     player-side Ritual 1 — +1 Strength at every turn end, and
        //                      unlike the monster-side Ritual it has NO skipFirst.
        //   DUPLICATION_POTION 1 layer -> the next card played is queued a second time.
        //
        // ⚠ SLIME_BOSS is here on purpose and it is the only encounter that can do this job:
        // Duplication has a copy of its `if` in ALL FOUR onUseXxxCard handlers, and the
        // status/curse one is only reachable through Slimed — the one status card the policy
        // is allowed to play (CardInstance.cpp:329's `id != SLIMED` exception). Same reasoning
        // batch 42 used to pick slime_boss for Ink Bottle's fourth handler.
        const std::vector<Potion> G4 {
            Potion::REGEN_POTION, Potion::CULTIST_POTION, Potion::DUPLICATION_POTION,
        };
        add(7, 0, G4, PLAIN, {MonsterEncounter::HEXAGHOST, MonsterEncounter::SLIME_BOSS});
        add(8, 0, G4, BARK,  {MonsterEncounter::SLIME_BOSS});

        // ---- group 5: the three that rewrite the HAND ------------------------------------
        //
        //   BLESSING_OF_THE_FORGE  upgrade every card in hand (no bark ternary at all).
        //   ELIXIR_POTION          ExhaustMany(10) — opens the multi-select screen, which the
        //                          harness answers with min(pickCount, handSize) = the WHOLE
        //                          hand. Also no bark ternary.
        //   SNECKO_OIL             draw 5, then RandomizeHandCost — the only potion that
        //                          rewrites `cost` itself rather than `costForTurn`.
        //
        // ⚠ Slot order is the drink order, and it matters here: Forge upgrades the hand, then
        // Elixir empties it, then Snecko Oil draws a fresh five and re-prices them. All three
        // stay observable precisely because Snecko Oil comes last.
        // ⚠ Two of the three have NO `hasBark` ternary, so the B-side below only carries
        // Snecko Oil's 10/5 — it is still worth its one file, that is the only bark branch of
        // the three.
        const std::vector<Potion> G5 {
            Potion::BLESSING_OF_THE_FORGE, Potion::ELIXIR_POTION, Potion::SNECKO_OIL,
        };
        add(9,  0, G5, PLAIN, {MonsterEncounter::THREE_SENTRIES, MonsterEncounter::SLIME_BOSS});
        add(10, 0, G5, BARK,  {MonsterEncounter::SLIME_BOSS});

        // ---- group 6: Distilled Chaos, alone, because it eats the fight -------------------
        //
        // Three copies play 3 (bark: 6) draw-pile cards EACH on turn 0, i.e. 9 or 18 free
        // cards before the first manual play. Measured effect on fight length: the average
        // drops from ~7.2 to 4.38 turns, and gremlin_nob ends in 0.42 turns. That is the same
        // "energyPerTurn++ starves everything else" lesson batch 43 learned about relics, so
        // it gets its own variant instead of starving a turn-boundary Power.
        //
        // ⚠ It is the third member of the Havoc / Mayhem family: it plays the draw pile's top
        // card through NO gate at all. The deck check above is what keeps that safe.
        const std::vector<Potion> G6 { Potion::DISTILLED_CHAOS };
        add(11, 0, G6, PLAIN, {MonsterEncounter::HEXAGHOST, MonsterEncounter::SLIME_BOSS});
        add(12, 0, G6, BARK,  {MonsterEncounter::HEXAGHOST});

        // ⚠⚠ THE POTIONS THAT ARE **NOT** REGISTERED, and why. 42 potions exist
        // (Potions.h's enum minus INVALID / EMPTY_POTION_SLOT); 13 were already registered,
        // 15 are above, and these 14 are out. Three of the reasons are new to this batch.
        //
        //   * CONJURES A CARD OUT OF THE WHOLE POOL -> the trace becomes unreplayable, exactly
        //     the family batch 43 excluded Enchiridion / Dead Branch / Nilry's Codex for:
        //     ATTACK_POTION / SKILL_POTION / POWER_POTION / COLORLESS_POTION (all four are
        //     `Actions::DiscoveryAction`).
        //     ⚠ Do NOT confuse this with Snecko Oil or Blessing of the Forge: rewriting an
        //     EXISTING card's cost or upgrade bit is not conjuring (batch 44's criterion).
        //   * MAKES A CARD THE REFERENCE NEVER IMPLEMENTED: BOTTLED_MIRACLE (MIRACLE) and
        //     CUNNING_POTION (SHIV). Both card ids are in the same "no case in any of the three
        //     switches" family as SEEK — no oracle can exist for them.
        //   * NEEDS A STANCE (not modelled, and `changeStance` chains actions):
        //     AMBROSIA, STANCE_POTION.
        //   * ⚠⚠ THE ACTION IS A DEFAULT-CONSTRUCTED `Action`, i.e. an EMPTY std::function.
        //     `BattleContext::executeActions` pops it and calls `a(*this)`
        //     (BattleContext.cpp:766-767), which throws std::bad_function_call and takes the
        //     whole generator down. This is a HARD exclusion, not a soft one — the same
        //     category as THE_SPECIMEN wedging the battle:
        //       ESSENCE_OF_DARKNESS  `Actions::EssenceOfDarkness` -> `return sts::Action();`
        //       POTION_OF_CAPACITY   `Actions::IncreaseOrbSlots`  -> same
        //       POISON_POTION        applies MS::POISON fine, but then
        //                            `Monster::applyStartOfTurnPowers` (Monster.cpp:36-38)
        //                            enqueues `Actions::PoisonLoseHpAction()` — also an empty
        //                            Action — on that monster's NEXT turn. The crash is merely
        //                            DEFERRED, which makes this one the most dangerous of the
        //                            three to "just try".
        //     ⚠ ESSENCE_OF_DARKNESS is now the engine repo's permanent "unregistered potion"
        //     test sample, the potion-side counterpart of SEEK.
        //   * THE CASE BODY IS `// todo` (BattleContext.cpp:2426-2428): SMOKE_BOMB. Registering
        //     it would mean shipping "drinking it does nothing", which is what the REAL game
        //     does not do (it ends the combat) — the Melange family: no oracle, never register.
        //   * NO CASE AT ALL, falls into `default:` -> `assert(false)`: FAIRY_POTION. It is not
        //     a drinkable potion in the reference either — `search::Action::isValidAction`
        //     refuses it (Action.cpp:86) and it is consumed passively in `Player::wouldDie`.
        //   * ⚠ GAMBLERS_BREW is excluded for a reason worth writing down separately:
        //     `Actions::GambleAction` sets `inputState` and `cardSelectTask` but NEVER SETS
        //     `pickCount` (Actions.cpp:987-992), and `isValidMultiCardSelectAction`'s GAMBLE
        //     arm does not read it either (Action.cpp:211-219). So how many cards get
        //     discarded is decided by whatever `pickCount` the PREVIOUS card-select screen
        //     happened to leave behind. That is deterministic and therefore replayable, but it
        //     is a reference defect, and registering a potion whose amount comes from stale
        //     state is exactly the kind of thing this project must not do quietly. Reported,
        //     not patched.
    }

    // ============== act-3 ascension axis (batch 46), the SEVENTH product ==================
    //
    // Act 1 and act 2 have had {asc 0, asc 19} since batches 21/22 and 30. Act 3 has only had
    // asc 0, and the engine repo's `EnemyDef.ascCalibrated` gate is unset on all 17 of its
    // monsters, so `constructMonster` still throws for ascension > 0. This product closes that
    // — the single largest structural hole in the corpus.
    //
    // Same encounter list as the act-3 product, verbatim: this axis is exactly "the act-3
    // encounters again, one level up". ⚠ JAW_WORM_HORDE is act 3's sixteenth encounter but has
    // lived in the act-1 product since the first commit, so `jaw_worm_horde@asc19` was already
    // produced by variant 21 back in batch 21 — naming it here would collide.
    std::vector<std::pair<MonsterEncounter, const char *>> act3AscEncounters = act3Encounters;

    std::vector<DeckVariant> act3AscVariants;
    {
        // Filled in by step 2 of this batch. Adding the product with the list still empty is a
        // no-op, and that is PROVED rather than assumed — see the emitProduct call at the
        // bottom of main().
    }

    for (const auto &v : variants) {
        // 10 starter cards are added by the GameContext constructor.
        if (static_cast<int>(v.extra.size()) + 10 > Deck::MAX_SIZE) {
            std::cerr << "deck variant exceeds Deck::MAX_SIZE (" << Deck::MAX_SIZE << "): "
                      << (v.extra.size() + 10) << " cards" << std::endl;
            return 1;
        }
    }
    for (const auto &v : act2Variants) {
        if (static_cast<int>(v.extra.size()) + 10 > Deck::MAX_SIZE) {
            std::cerr << "act2 deck variant exceeds Deck::MAX_SIZE (" << Deck::MAX_SIZE << "): "
                      << (v.extra.size() + 10) << " cards" << std::endl;
            return 1;
        }
    }
    for (const auto &v : tgtVariants) {
        if (static_cast<int>(v.extra.size()) + 10 > Deck::MAX_SIZE) {
            std::cerr << "target-policy deck variant exceeds Deck::MAX_SIZE (" << Deck::MAX_SIZE
                      << "): " << (v.extra.size() + 10) << " cards" << std::endl;
            return 1;
        }
    }
    for (const auto &v : act3Variants) {
        if (static_cast<int>(v.extra.size()) + 10 > Deck::MAX_SIZE) {
            std::cerr << "act3 deck variant exceeds Deck::MAX_SIZE (" << Deck::MAX_SIZE << "): "
                      << (v.extra.size() + 10) << " cards" << std::endl;
            return 1;
        }
    }
    for (const auto &v : relicVariants) {
        if (static_cast<int>(v.extra.size()) + 10 > Deck::MAX_SIZE) {
            std::cerr << "relic deck variant exceeds Deck::MAX_SIZE (" << Deck::MAX_SIZE << "): "
                      << (v.extra.size() + 10) << " cards" << std::endl;
            return 1;
        }
        // atBattleStart in BattleContext::initRelics is a fixed_list<RelicId, 8> and
        // fixed_list has NO bounds checking anywhere in the reference, so overfilling it is
        // silent memory corruption rather than an assert. None of batch 40's relics is an
        // atBattleStart one, but an explicit loadout is exactly the place a future batch
        // could hand out nine of them by accident.
        if (v.relics.size() > 8) {
            std::cerr << "relic variant names more than 8 relics (initRelics' atBattleStart "
                         "fixed_list holds 8): " << v.relics.size() << std::endl;
            return 1;
        }
        if (v.relicSet == 0) {
            std::cerr << "relic variant must carry a non-zero relicSet (it is the file-name "
                         "suffix AND the fingerprint dimension)" << std::endl;
            return 1;
        }
        // Batch 44. OMAMORI / LIZARD_TAIL are the two relics initRelics feeds through
        // `p.setHasRelic<X>(r.data)`: data 0 clears the player's bit, so the relic is
        // INVISIBLE for the whole fight. Handing one out that way is never what a variant
        // means, and it is also the invariant the trace format leans on — the replayer
        // reconstructs their data as "listed ⇒ non-zero" instead of carrying a field.
        for (const auto &rs : v.relics) {
            if (!relicDataIsNumeric(rs.id) && rs.data == 0) {
                std::cerr << "relic variant hands out " << rs.name
                          << " with data 0, which makes it invisible inside combat "
                             "(setHasRelic<X>(r.data)); give it its real charge count"
                          << std::endl;
                return 1;
            }
        }
    }
    {
        // Every explicit-loadout variant must own its relicSet number: two variants sharing
        // one would share a group key, so their rows would land in one file and
        // variant0-rows.mjs would report a longer frozen prefix than the data actually has.
        std::vector<int> sets;
        for (const auto &v : relicVariants) sets.push_back(v.relicSet);
        std::sort(sets.begin(), sets.end());
        if (std::adjacent_find(sets.begin(), sets.end()) != sets.end()) {
            std::cerr << "two relic variants share a relicSet number" << std::endl;
            return 1;
        }
    }
    for (const auto &v : potionVariants) {
        if (static_cast<int>(v.extra.size()) + 10 > Deck::MAX_SIZE) {
            std::cerr << "potion deck variant exceeds Deck::MAX_SIZE (" << Deck::MAX_SIZE << "): "
                      << (v.extra.size() + 10) << " cards" << std::endl;
            return 1;
        }
        if (v.relics.size() > 8) {
            std::cerr << "potion variant names more than 8 relics" << std::endl;
            return 1;
        }
        if (v.potionSet == 0) {
            std::cerr << "potion variant must carry a non-zero potionSet (it is the file-name "
                         "suffix, the fingerprint dimension AND the switch that widens "
                         "isReplayablePotion)" << std::endl;
            return 1;
        }
        // ⚠⚠ THE INVARIANT THAT KEEPS TWO ACTIVE LINES FROM COLLIDING. `traceIdx` drives
        // exactly two things: the relic rotation and the potion rotation. A variant that pins
        // BOTH therefore emits traces that do not depend on where it sits in the product order
        // at all (the batch-42 note in WORKFLOW). The relic line still appends a variant to
        // `relicVariants` every batch, and `relicVariants` is emitted BEFORE this product — so
        // without this check the next relic batch would shift every traceIdx handed out here
        // and invalidate every committed `@potN` file.
        if (v.relics.empty() || v.potions.empty()) {
            std::cerr << "potion variant must pin BOTH its relics and its potions, otherwise it "
                         "reads traceIdx and the next relic batch invalidates its files"
                      << std::endl;
            return 1;
        }
        for (const auto &rs : v.relics) {
            if (!relicDataIsNumeric(rs.id) && rs.data == 0) {
                std::cerr << "potion variant hands out " << rs.name << " with data 0" << std::endl;
                return 1;
            }
        }
        // DISTILLED_CHAOS plays the draw pile's top card with no gate at all (same hole HAVOC
        // opened in batch 10). The deck is the only place an unregistered card could come from
        // here, so check it rather than trusting the encounter list.
        for (auto cid : v.extra) {
            if (!isReplayableCard(cid) || isPoolConjuringCard(cid)) {
                std::cerr << "potion variant deck holds a card Distilled Chaos must not be able "
                             "to auto-play: " << getCardEnumName(cid) << std::endl;
                return 1;
            }
        }
    }
    {
        std::vector<int> sets;
        for (const auto &v : potionVariants) sets.push_back(v.potionSet);
        std::sort(sets.begin(), sets.end());
        if (std::adjacent_find(sets.begin(), sets.end()) != sets.end()) {
            std::cerr << "two potion variants share a potionSet number" << std::endl;
            return 1;
        }
    }
    for (const auto &v : act3AscVariants) {
        if (static_cast<int>(v.extra.size()) + 10 > Deck::MAX_SIZE) {
            std::cerr << "act3-asc deck variant exceeds Deck::MAX_SIZE (" << Deck::MAX_SIZE
                      << "): " << (v.extra.size() + 10) << " cards" << std::endl;
            return 1;
        }
        if (v.relics.size() > 8) {
            std::cerr << "act3-asc variant names more than 8 relics" << std::endl;
            return 1;
        }
        // The whole point of the product: a zero here would emit `<encounter>.jsonl`, i.e. it
        // would try to overwrite a FROZEN act-3 file instead of creating `<encounter>@asc19`.
        if (v.ascension == 0) {
            std::cerr << "act3-asc variant must carry a non-zero ascension (it is the file-name "
                         "suffix AND the fingerprint dimension)" << std::endl;
            return 1;
        }
        // ⚠⚠ SAME INVARIANT AS THE POTION PRODUCT, and for the same reason. `traceIdx` drives
        // exactly two things — the relic rotation and the potion rotation — so a variant that
        // pins BOTH emits traces that do not depend on where it sits in the product order at
        // all. That is what lets the relic line (`relicVariants`, still growing) and the potion
        // line (`potionVariants`, still growing) both sit AHEAD of this product without ever
        // invalidating a committed `@asc19` act-3 file, and it lets THIS product keep growing
        // too (a second ascension level is a plain append). Batch 31's rule was "the new axis
        // must be last"; batch 45 replaced it with this check, which is strictly better because
        // it is order-independent in both directions.
        if (v.relics.empty() || v.potions.empty()) {
            std::cerr << "act3-asc variant must pin BOTH its relics and its potions, otherwise "
                         "it reads traceIdx and the next relic/potion batch invalidates its "
                         "files" << std::endl;
            return 1;
        }
        for (const auto &rs : v.relics) {
            if (!relicDataIsNumeric(rs.id) && rs.data == 0) {
                std::cerr << "act3-asc variant hands out " << rs.name << " with data 0"
                          << std::endl;
                return 1;
            }
        }
        // potionSet stays 0 here (the file-name suffix is `@asc19`, not `@potN`), so
        // isReplayablePotion runs on the NARROW list — pinning anything outside the frozen 13
        // would make the trace unreplayable rather than merely unverified. Check it rather
        // than trusting the comment next to the list.
        for (auto p : v.potions) {
            if (!isReplayablePotion(p, v.potionSet != 0)) {
                std::cerr << "act3-asc variant pins a potion the replayer does not know"
                          << std::endl;
                return 1;
            }
        }
    }

    std::cout << "{" << q("traces") << ":[";
    bool firstTrace = true;
    size_t traceIdx = 0;

    // One (variants x encounters) product. This used to sit inline right here; it became a
    // lambda so that ACT 2 can be appended as a SECOND product without duplicating ~180 lines
    // of body. `traceIdx` and `firstTrace` are captured BY REFERENCE, so the second call keeps
    // numbering where the first stopped and the act-1 indices are untouched.
    //
    // Extracting it is a pure refactor, and that was proved rather than assumed: with
    // `act2Variants` still empty, tools/regen-traces.sh --check reproduced all 40 committed
    // files byte-for-byte. Only after that did variant 23 get filled in.
    const auto emitProduct = [&](const std::vector<DeckVariant> &variantList,
                                 const std::vector<std::pair<MonsterEncounter, const char *>> &encounterList) {
    for (const auto &variant : variantList) {
    for (const auto &enc : encounterList) {
        if (!variant.encounters.empty() &&
            std::find(variant.encounters.begin(), variant.encounters.end(), enc.first)
                == variant.encounters.end()) {
            continue;
        }
        for (size_t seedIdx = 0; seedIdx < seeds.size() && seedIdx < variant.seedLimit; ++seedIdx) {
            const auto &sd = seeds[seedIdx];
            for (int floor : floors) {
                GameContext gc(CharacterClass::IRONCLAD, sd.value, variant.ascension);
                gc.floorNum = floor;
                // Arriving at a floor reseeds these three (GameContext::transitionToMapNode:
                // `const auto r = Random(seed + floorNum); miscRng = shuffleRng = cardRandomRng = r;`).
                // Setting floorNum alone leaves miscRng on the constructor's Random(seed),
                // which silently makes every floor roll the same variant encounter.
                {
                    const auto r = Random(sd.value + static_cast<unsigned long long>(floor));
                    gc.miscRng = r;
                    gc.shuffleRng = r;
                    gc.cardRandomRng = r;
                }

                // potionRng is the one genuinely run-persistent stream, so pin it to a
                // stated value the replayer can reconstruct instead of inheriting whatever
                // the GameContext constructor left behind.
                gc.potionRng = Random(sd.value);

                // Two relics per trace, rotating, so every trace exercises a pair — unless
                // the variant names its own loadout, in which case it gets exactly that.
                // ⚠ `traceIdx` is advanced identically either way (see `++traceIdx` below),
                // which is what keeps the rotation's assignment to every OTHER variant
                // unchanged.
                std::vector<std::string> relicNames;
                std::vector<int> relicData;   // parallel to relicNames; see relicDataIsNumeric
                bool anyNumericData = false;
                const auto addRelic = [&](const RelicSpec &rs) {
                    gc.relics.add({rs.id, rs.data});
                    relicNames.push_back(rs.name);
                    const int d = relicDataIsNumeric(rs.id) ? rs.data : 0;
                    relicData.push_back(d);
                    if (d != 0) anyNumericData = true;
                };
                if (variant.relics.empty()) {
                    for (int k = 0; k < 2; ++k) {
                        const auto &rs = RELIC_ROTATION[(traceIdx * 2 + static_cast<size_t>(k)) % RELIC_ROTATION.size()];
                        addRelic(rs);
                    }
                } else {
                    for (const auto &rs : variant.relics) {
                        addRelic(rs);
                    }
                }

                // Richer deck so the traces exercise the newly registered cards, not just
                // the starter three. Added deterministically (no RNG) before cards.init.
                //
                // Only upgrade what the reference itself says can be upgraded: forcing the
                // flag onto a card with no upgraded form would make the two sides disagree
                // about its cost for a reason that has nothing to do with the card's rule.
                //
                // Upgrade via Card::upgrade() rather than the Card(id, upgraded) constructor.
                // They differ for exactly one card: SEARING_BLOW keeps its upgrade COUNT in
                // Card::misc, and only upgrade() increments it (Card.cpp:9). The two-arg
                // constructor sets `upgraded = true` but leaves misc at 0, so
                // CardInstance(const Card&) would read specialData = getUpgraded() = 0 and the
                // "upgraded" Searing Blow would deal the un-upgraded 12 damage while claiming
                // to be upgraded. Identical for every other card, and variant 0 is
                // un-upgraded, so this changes nothing already committed.
                for (auto cid : variant.extra) {
                    const bool up = variant.upgradeAll && Card(cid).canUpgrade();
                    Card c(cid);
                    if (up) c.upgrade();
                    gc.deck.obtain(gc, c);
                }

                BattleContext bc;
                bc.init(gc, enc.first);

                // DELIBERATE DEVIATION from the reference as-built.
                // Player::cc has no initialiser and nothing in the whole reference source
                // ever assigns player.cc (`grep -rn 'player\.cc *='` finds nothing), so
                // BattleContext::init leaves it indeterminate. Entropic Brew reads it via
                // returnRandomPotion(potionRng, player.cc, true) and therefore rolls from a
                // garbage character's potion pool. That is undefined behaviour, not
                // behaviour worth reproducing, so pin it to the actual character.
                bc.player.cc = gc.cc;

                // Baseline for the snapshot's `goldGained` delta (see s_goldBaseline).
                // BattleContext::init has already copied gc.gold into player.gold.
                s_goldBaseline = gc.gold;

                // Hand out three potions, rotating through the set so different traces
                // exercise different effects (including Entropic Brew's potionRng draws).
                for (int i = 0; i < bc.potionCapacity; ++i) {
                    if (variant.potions.empty()) {
                        const size_t k = (traceIdx * 3 + static_cast<size_t>(i)) % POTION_ROTATION.size();
                        bc.potions[i] = POTION_ROTATION[k];
                    } else {
                        // Cycles when the variant names fewer potions than the capacity, so a
                        // one-element list means "all three slots hold this".
                        bc.potions[i] = variant.potions[static_cast<size_t>(i) % variant.potions.size()];
                    }
                    ++bc.potionCount;
                }
                ++traceIdx;

                if (!firstTrace) std::cout << ",";
                firstTrace = false;

                std::cout << "{" << q("seed") << ":" << q(sd.name)
                          << "," << q("seedLong") << ":" << q(std::to_string(sd.value))
                          << "," << q("floor") << ":" << floor
                          << "," << q("encounter") << ":" << q(enc.second);
                // Emitted only when non-zero — same trick as `deckUpgraded` / `goldGained`.
                // Every asc-0 line therefore stays byte-identical to what is committed,
                // which is what lets tools/regen-traces.sh --check prove this whole axis is
                // a no-op for the existing corpus before any new variant is added.
                if (variant.ascension != 0) {
                    std::cout << "," << q("ascension") << ":" << variant.ascension;
                }
                // Same trick again, for the target-policy axis (batch 31). Emitted only
                // when non-zero, so every line generated under the historical
                // firstAliveMonster policy stays byte-identical to what is committed —
                // which is what lets tools/regen-traces.sh --check prove this axis is a
                // no-op for the existing corpus BEFORE any new variant is added.
                //
                // ⚠ The replayer does NOT need to read this field: the chosen target is
                // already recorded verbatim in every `card` / `potion` step. It exists so
                // that split-traces.mjs can put these traces in their own files and so a
                // reader can tell which policy produced a line.
                if (variant.targetPolicy != 0) {
                    std::cout << "," << q("targetPolicy") << ":" << variant.targetPolicy;
                }
                // Same trick a third time, for the explicit relic/potion loadout axis
                // (batch 40). Emitted only when non-zero, so every line generated under the
                // rotation stays byte-identical — which is what lets
                // tools/regen-traces.sh --check prove this axis is a no-op for the existing
                // corpus BEFORE any new variant is added.
                //
                // ⚠ The replayer does NOT need to read it: the actual relics are already
                // listed verbatim in `relics` and the potions in every snapshot. It exists so
                // split-traces.mjs can give these traces their own files, and so that the
                // variant fingerprint has a dimension that distinguishes two loadouts sharing
                // one deck.
                if (variant.relicSet != 0) {
                    std::cout << "," << q("relicSet") << ":" << variant.relicSet;
                }
                // Same trick a fourth time, for the potion axis (batch 45). `potionSet` is the
                // `@potN` file-name suffix + fingerprint dimension; `potionPolicy` says WHEN the
                // policy was willing to drink. Both emitted only when non-zero.
                //
                // ⚠ The replayer needs NEITHER: every drink is already recorded verbatim as a
                // `potion` step and the slots are in every snapshot. They exist so
                // split-traces.mjs can give these traces their own files, and so a reader can
                // tell which policy produced a line.
                if (variant.potionSet != 0) {
                    std::cout << "," << q("potionSet") << ":" << variant.potionSet;
                }
                if (variant.potionPolicy != 0) {
                    std::cout << "," << q("potionPolicy") << ":" << variant.potionPolicy;
                }
                // Player HP *entering* the fight, i.e. gc.curHp BEFORE BattleContext::init.
                //
                // The `initial` snapshot is taken AFTER init, and init already ran
                // initRelics (BattleContext.cpp:73) — Blood Vial's `p.heal(2)` among them.
                // So `initial.player.hp` is a POST-heal value and cannot be fed back in as
                // the replayer's starting HP without double-healing.
                //
                // Nobody noticed until ascension arrived because `GameContext::initPlayer`
                // ends with `curHp = ascension < 6 ? maxHp : round(maxHp * 0.9f)`
                // (GameContext.cpp:522): below asc 6 the player enters at FULL health, so the
                // heal is clamped away and post == pre. At asc 19 it is 68/75, the heal lands,
                // and every trace whose relic pair contains Blood Vial diverges by exactly 2.
                //
                // Emitted only when it differs from maxHp — same trick as `ascension` above,
                // so every asc-0 line stays byte-identical.
                if (gc.curHp != gc.maxHp) {
                    std::cout << "," << q("playerHp") << ":" << gc.curHp;
                }
                std::cout << "," << q("character") << ":" << q("ironclad")
                          << "," << q("potionRngSeed") << ":" << q(std::to_string(sd.value))
                          << "," << q("relics") << ":" << strArr(relicNames);
                // Parallel to `relics`; see relicDataIsNumeric above. Emitted only when some
                // relic carries a non-zero NUMERIC data, which no committed variant does —
                // so every line written before this field existed stays byte-identical.
                if (anyNumericData) std::cout << "," << q("relicData") << ":" << arr(relicData);

                std::vector<std::string> deck;
                std::vector<int> deckUpgraded;
                bool anyUpgraded = false;
                for (int i = 0; i < gc.deck.size(); ++i) {
                    deck.push_back(getCardEnumName(gc.deck.cards[i].getId()));
                    const bool up = gc.deck.cards[i].isUpgraded();
                    deckUpgraded.push_back(up ? 1 : 0);
                    anyUpgraded = anyUpgraded || up;
                }
                std::cout << "," << q("deck") << ":" << strArr(deck);
                // Emitted only when it carries information, so an all-un-upgraded variant's
                // lines stay byte-identical to what was committed before this field existed.
                if (anyUpgraded) std::cout << "," << q("deckUpgraded") << ":" << arr(deckUpgraded);

                std::cout << "," << q("initial") << ":" << snapshot(bc);
                std::cout << "," << q("steps") << ":[";

                bool firstStep = true;
                for (int step = 0; step < MAX_STEPS; ++step) {
                    if (bc.outcome != Outcome::UNDECIDED) break;

                    Step s;
                    if (bc.inputState == InputState::CARD_SELECT) {
                        // A card-select screen is open — answer it before anything else.
                        if (!pickCardSelectAction(bc, s)) break;
                    } else if (bc.inputState == InputState::PLAYER_NORMAL) {
                        pickAction(bc, variant.targetPolicy, variant.potionPolicy,
                                   variant.potionSet != 0, s);
                    } else {
                        // Stance / gambling / scry screens still have no trace encoding;
                        // stop rather than emit something the replayer cannot express.
                        break;
                    }

                    if (!firstStep) std::cout << ",";
                    firstStep = false;

                    if (s.type == "card") {
                        std::cout << "{" << q("action") << ":{" << q("type") << ":" << q("card")
                                  << "," << q("idx") << ":" << s.idx
                                  << "," << q("target") << ":" << s.target << "}";
                        search::Action(search::ActionType::CARD, s.idx, s.target).execute(bc);
                    } else if (s.type == "select_card") {
                        std::cout << "{" << q("action") << ":{" << q("type") << ":" << q("select_card")
                                  << "," << q("idx") << ":" << s.idx << "}";
                        search::Action(search::ActionType::SINGLE_CARD_SELECT, s.idx).execute(bc);
                    } else if (s.type == "select_cards") {
                        std::cout << "{" << q("action") << ":{" << q("type") << ":" << q("select_cards")
                                  << "," << q("idxs") << ":" << arr(s.idxs) << "}";
                        search::Action a(search::ActionType::MULTI_CARD_SELECT);
                        // rebuild the same selection the enumerator handed us
                        std::uint32_t bits = a.bits;
                        for (int i : s.idxs) bits |= (1u << i);
                        search::Action(bits).execute(bc);
                    } else if (s.type == "potion") {
                        std::cout << "{" << q("action") << ":{" << q("type") << ":" << q("potion")
                                  << "," << q("idx") << ":" << s.idx
                                  << "," << q("target") << ":" << s.target << "}";
                        search::Action(search::ActionType::POTION, s.idx, s.target).execute(bc);
                    } else {
                        std::cout << "{" << q("action") << ":{" << q("type") << ":" << q("end_turn") << "}";
                        search::Action(search::ActionType::END_TURN).execute(bc);
                    }
                    std::cout << "," << q("after") << ":" << snapshot(bc) << "}";
                }

                std::cout << "]}";
            }
        }
    }
    }
    };

    emitProduct(variants, encounters);
    // ⚠ MUST stay after the act-1 product: appending here only ever hands out traceIdx values
    // past the end of the act-1 range, which is what keeps every committed act-1 file
    // byte-identical.
    emitProduct(act2Variants, act2Encounters);
    // ⚠ MUST stay ahead of the act-3 product only, and behind the two above it. Note the
    // corollary: NOTHING may be appended to `variants` or `act2Variants` — either would shift
    // the traceIdx values this product and the act-3 one hand out. New axes get their own
    // product at the end; new act-1/act-2 variants are no longer free.
    emitProduct(tgtVariants, tgtEncounters);
    // ⚠ MUST stay last, and must STAY last for as long as act 3 is being filled in: every
    // act-3 batch appends one variant to `act3Variants`, and appending to a product that is
    // not last shifts every traceIdx the products behind it hand out. So no fifth product
    // until act 3 is done. Adding this call with `act3Variants` still empty is a no-op, and
    // that was proved rather than assumed: tools/regen-traces.sh --check reproduced all 101
    // committed files byte-for-byte before the first act-3 variant was filled in.
    emitProduct(act3Variants, act3Encounters);
    // ⚠ MUST stay last from here on: batch 40 opened this product and the relic / potion line
    // appends a variant to it every batch. Adding this call with `relicVariants` still empty
    // is a no-op, and that was proved rather than assumed — tools/regen-traces.sh --check
    // reproduced all 116 committed files byte-for-byte before the first relic variant was
    // filled in.
    emitProduct(relicVariants, relicEncounters);
    // ⚠ MUST stay last from here on (batch 45). Adding this call with `potionVariants` still
    // empty is a no-op, and that was proved rather than assumed: tools/regen-traces.sh --check
    // reproduced all 163 committed files byte-for-byte before the first potion variant was
    // filled in.
    //
    // ⚠⚠ Unlike every earlier "must stay last", this one does NOT freeze `relicVariants`:
    // every variant in this product is required to pin both its relics and its potions (see
    // the check above), so it reads no traceIdx and appending to the relic product ahead of it
    // stays free. That check is what buys the relic line the right to keep growing.
    emitProduct(potionVariants, potionEncounters);
    // ⚠ Batch 46. Like the potion product above, this one does NOT freeze anything ahead of it:
    // every variant in it is required to pin both its relics and its potions (see the check
    // above), so it reads no traceIdx and its position in the product order is irrelevant.
    // Adding this call with `act3AscVariants` still empty is a no-op, and that was proved
    // rather than assumed: tools/regen-traces.sh --check reproduced all 182 committed files
    // byte-for-byte before the first act-3 ascension variant was filled in.
    emitProduct(act3AscVariants, act3AscEncounters);

    std::cout << "]}" << std::endl;
    return 0;
}
