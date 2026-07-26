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

// Every non-zero player status, as {"NAME": amount}.
static std::string playerStatuses(const Player &p) {
    std::ostringstream os;
    os << "{";
    bool first = true;
    for (int i = 0; i < static_cast<int>(PlayerStatus::THE_BOMB) + 1; ++i) {
        const auto s = static_cast<PlayerStatus>(i);
        if (!p.hasStatusRuntime(s)) continue;
        const int v = p.getStatusRuntime(s);
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

static std::string snapshot(const BattleContext &bc) {
    std::ostringstream os;
    os << "{";
    os << q("turn") << ":" << bc.turn;
    os << "," << q("outcome") << ":" << q(outcomeName(bc.outcome));

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
        // Multi-select carries a list; the trace format records the chosen indices
        // verbatim in `idxs` rather than a single idx.
        out = {"select_cards", -1, -1};
        out.idxs.clear();
        for (int i : a.getSelectedIdxs()) out.idxs.push_back(i);
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
    // whatever each line states. That makes coverage APPEND-ONLY: a new batch of
    // registered cards adds new variants instead of regenerating the existing 23MB.
    //
    // Variant 0 MUST stay first and unchanged — its traceIdx values (0..N) drive the
    // relic/potion rotation, so keeping it first reproduces the committed files
    // byte-for-byte. New variants continue the counter and only ever append lines.
    //
    // `upgradeAll` covers the other half of every card rule. The un-upgraded pass
    // alone never exercises the `up ? x : y` branch, which is exactly where a
    // transcription slip hides.
    struct DeckVariant {
        std::vector<CardId> extra;
        size_t seedLimit;   // first N seeds only, to bound file growth
        bool upgradeAll;
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

    std::vector<DeckVariant> variants { {BATCH_1, seeds.size(), false} };
    if (!BATCH_2.empty()) {
        std::vector<CardId> all = BATCH_1;
        all.insert(all.end(), BATCH_2.begin(), BATCH_2.end());
        variants.push_back({all, 40, false});
        variants.push_back({all, 40, true});
    }

    std::cout << "{" << q("traces") << ":[";
    bool firstTrace = true;
    size_t traceIdx = 0;

    for (const auto &variant : variants) {
    for (const auto &enc : encounters) {
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
                for (auto cid : variant.extra) {
                    const bool up = variant.upgradeAll && Card(cid).canUpgrade();
                    gc.deck.obtain(gc, Card(cid, up ? 1 : 0));
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
