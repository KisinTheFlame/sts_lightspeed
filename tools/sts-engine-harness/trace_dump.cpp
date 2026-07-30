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
           << "," << q("alive") << ":" << (m.isDeadOrEscaped() ? "false" : "true")
           << "," << q("move") << ":" << q(monsterMoveStrings[static_cast<int>(m.moveHistory[0])])
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
struct RelicSpec { RelicId id; const char *name; };
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

static bool isReplayablePotion(Potion p) {
    switch (p) {
        case Potion::BLOCK_POTION: case Potion::FIRE_POTION: case Potion::STRENGTH_POTION:
        case Potion::WEAK_POTION: case Potion::FEAR_POTION: case Potion::ENERGY_POTION:
        case Potion::SWIFT_POTION: case Potion::DEXTERITY_POTION: case Potion::BLOOD_POTION:
        case Potion::ANCIENT_POTION: case Potion::EXPLOSIVE_POTION: case Potion::FRUIT_JUICE:
        case Potion::ENTROPIC_BREW:
            return true;
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

static bool pickAction(const BattleContext &bc, int targetPolicy, Step &out) {
    const int target = policyTarget(bc, targetPolicy);
    // Drink before attacking, so potion buffs are visible in the same turn's card maths.
    for (int i = 0; i < bc.potionCapacity; ++i) {
        if (bc.potions[i] == Potion::EMPTY_POTION_SLOT) continue;
        // Entropic Brew refills slots with arbitrary potions, some of which open a
        // card-select screen the trace format cannot express. Only drink what
        // sts-combat has registered.
        if (!isReplayablePotion(bc.potions[i])) continue;
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

                // Two relics per trace, rotating, so every trace exercises a pair.
                std::vector<std::string> relicNames;
                for (int k = 0; k < 2; ++k) {
                    const auto &rs = RELIC_ROTATION[(traceIdx * 2 + static_cast<size_t>(k)) % RELIC_ROTATION.size()];
                    gc.relics.add({rs.id, 0});
                    relicNames.push_back(rs.name);
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
                    const size_t k = (traceIdx * 3 + static_cast<size_t>(i)) % POTION_ROTATION.size();
                    bc.potions[i] = POTION_ROTATION[k];
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
                        pickAction(bc, variant.targetPolicy, s);
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

    std::cout << "]}" << std::endl;
    return 0;
}
