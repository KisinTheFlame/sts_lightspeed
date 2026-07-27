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

// Deterministic: play the lowest-index playable card at the lowest-index living
// monster; when nothing is playable, end the turn. Recorded verbatim, so the TS side
// never re-derives it.
static int firstAliveMonster(const BattleContext &bc) {
    for (int i = 0; i < bc.monsters.monsterCount; ++i) {
        if (!bc.monsters.arr[i].isDeadOrEscaped()) return i;
    }
    return 0;
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

static bool pickAction(const BattleContext &bc, Step &out) {
    const int target = firstAliveMonster(bc);
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
    }
    for (const auto &v : variants) {
        // 10 starter cards are added by the GameContext constructor.
        if (static_cast<int>(v.extra.size()) + 10 > Deck::MAX_SIZE) {
            std::cerr << "deck variant exceeds Deck::MAX_SIZE (" << Deck::MAX_SIZE << "): "
                      << (v.extra.size() + 10) << " cards" << std::endl;
            return 1;
        }
    }

    std::cout << "{" << q("traces") << ":[";
    bool firstTrace = true;
    size_t traceIdx = 0;

    for (const auto &variant : variants) {
    for (const auto &enc : encounters) {
        if (!variant.encounters.empty() &&
            std::find(variant.encounters.begin(), variant.encounters.end(), enc.first)
                == variant.encounters.end()) {
            continue;
        }
        for (size_t seedIdx = 0; seedIdx < seeds.size() && seedIdx < variant.seedLimit; ++seedIdx) {
            const auto &sd = seeds[seedIdx];
            for (int floor : floors) {
                GameContext gc(CharacterClass::IRONCLAD, sd.value, 0);
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
                          << "," << q("encounter") << ":" << q(enc.second)
                          << "," << q("character") << ":" << q("ironclad")
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
                        pickAction(bc, s);
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

    std::cout << "]}" << std::endl;
    return 0;
}
