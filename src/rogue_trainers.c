#include "global.h"
#include "constants/battle.h"
#include "constants/event_objects.h"
#include "constants/items.h"
#include "constants/moves.h"
#include "constants/weather.h"
#include "gba/isagbprint.h"

#include "battle.h"
#include "event_data.h"
#include "event_object_movement.h"
#include "malloc.h"
#include "party_menu.h"
#include "random.h"

#include "rogue.h"
#include "rogue_adventurepaths.h"
#include "rogue_controller.h"
#include "rogue_multiplayer.h"
#include "rogue_pokedex.h"
#include "rogue_query.h"
#include "rogue_query_script.h"
#include "rogue_settings.h"
#include "rogue_trainers.h"

#define TRAINER_SHINY_PERC 25

struct TrainerHeldItemScratch
{
    bool8 hasLeftovers : 1;
    bool8 hasShellbell : 1;
    bool8 hasChoiceItem : 1;
#ifdef ROGUE_EXPANSION
    bool8 hasMegaStone : 1;
    bool8 hasZCrystal : 1;
#endif
};

struct TrainerPartyScratch
{
    struct TrainerHeldItemScratch heldItems;
    struct Pokemon* party;
    u16 trainerNum;
    bool8 shouldRegenerateQuery;
    bool8 allowItemEvos;
    bool8 allowWeakLegends;
    bool8 allowStrongLegends;
    bool8 forceLegends;
    bool8 preferStrongSpecies;
    u8 evoLevel;
    u8 partyCapacity;
    u8 partyCount;
    u8 subsetIndex;
    u8 subsetSampleCount;
    u8 fallbackCount;
};

static u16 SampleNextSpecies(struct TrainerPartyScratch* scratch);

static u8 CreateTrainerPartyInternal(u16 trainerNum, struct Pokemon* party, u8 monCount, u8 monCapacity, bool8 firstTrainer, u8 startIndex);
static u8 CreateRivalPartyInternal(u16 trainerNum, struct Pokemon* party, u8 monCapacity);
static bool8 UseCompetitiveMoveset(struct TrainerPartyScratch* scratch, u8 monIdx, u8 totalMonCount);
static bool8 SelectNextPreset(struct TrainerPartyScratch* scratch, u16 species, u8 monIdx, struct RoguePokemonCompetitiveSet* outPreset);
static void ModifyTrainerMonPreset(u16 trainerNum, struct RoguePokemonCompetitiveSet* preset, struct RoguePokemonCompetitiveSetRules* presetRules);
static void ReorderPartyMons(u16 trainerNum, struct Pokemon *party, u8 monCount);
static bool8 IsChoiceItem(u16 itemId);

bool8 Rogue_IsBossTrainer(u16 trainerNum)
{
    const struct RogueTrainer* trainer = Rogue_GetTrainer(trainerNum);
    return (trainer->trainerFlags & TRAINER_FLAG_CLASS_ANY_MAIN_BOSS) != 0;
}

bool8 Rogue_IsRivalTrainer(u16 trainerNum)
{
    const struct RogueTrainer* trainer = Rogue_GetTrainer(trainerNum);
    return (trainer->trainerFlags & TRAINER_FLAG_CLASS_RIVAL) != 0;
}

bool8 Rogue_IsAnyBossTrainer(u16 trainerNum)
{
    return Rogue_IsBossTrainer(trainerNum) || Rogue_IsRivalTrainer(trainerNum);
}

bool8 Rogue_IsKeyTrainer(u16 trainerNum)
{
    return Rogue_IsBossTrainer(trainerNum) || Rogue_IsRivalTrainer(trainerNum);
}

static u8 GetTrainerLevel(u16 trainerNum)
{
    if(Rogue_IsBossTrainer(trainerNum))
    {
        return Rogue_CalculateBossMonLvl();
    }

    if(Rogue_IsRivalTrainer(trainerNum))
    {
        return Rogue_CalculateRivalMonLvl();
    }

    {
        const struct RogueTrainer* trainer = Rogue_GetTrainer(trainerNum);
        if(trainer->levelOverride != 0)
            return trainer->levelOverride;
    }

    return Rogue_CalculateTrainerMonLvl();
}

const struct RogueTrainer* Rogue_GetTrainer(u16 trainerNum)
{
    AGB_ASSERT(trainerNum < gRogueTrainerCount);
    return &gRogueTrainers[trainerNum];
}

struct RogueBattleMusic const* Rogue_GetTrainerMusic(u16 trainerNum)
{
    const struct RogueTrainer* trainer = Rogue_GetTrainer(trainerNum);
    return &gRogueTrainerMusic[trainer->musicPlayer];
}

const u8* Rogue_GetTrainerString(u16 trainerNum, u8 textId)
{
    const struct RogueTrainer* trainer = Rogue_GetTrainer(trainerNum);
    const u8* str = NULL;

    if(trainer->encounterText == NULL || trainer->encounterTextCount == 0)
        return NULL;

    // For rival trainer we have initial battle, middle battles, final pre E4 battle then E4 battle
    if(Rogue_IsRivalTrainer(trainerNum))
    {
        u8 i;
        u8 offset = 0;

        if(Rogue_GetCurrentDifficulty() >= ROGUE_FINAL_CHAMP_DIFFICULTY || Rogue_AssumeFinalQuestFakeChamp())
            offset = 3;
        else if(Rogue_GetCurrentDifficulty() <= gRogueRun.rivalEncounterDifficulties[0])
            offset = 0;
        else if(Rogue_GetCurrentDifficulty() >= gRogueRun.rivalEncounterDifficulties[ROGUE_RIVAL_MAX_ROUTE_ENCOUNTERS - 1])
            offset = 2;
        else
            offset = 1; // Assume in middle of run

        offset = min(offset, trainer->encounterTextCount - 1);

        for(; offset > 0; --offset)
        {
            str = trainer->encounterText[TRAINER_STRING_COUNT * offset + textId];
            if(str != NULL)
                return str;
        }

        // Fallback to offset 0
        return trainer->encounterText[textId];
    }
    // For boss trainers we're going to predictably jump up the string tables, so custom text can optionally be added for later states
    // In order: gyms, e4, champ, final champ
    else if(Rogue_IsAnyBossTrainer(trainerNum))
    {
        u8 offset = 0;

        if(Rogue_GetCurrentDifficulty() >= ROGUE_FINAL_CHAMP_DIFFICULTY)
            offset = 3;
        else if(Rogue_GetCurrentDifficulty() >= ROGUE_CHAMP_START_DIFFICULTY)
            offset = 2;
        else if(Rogue_GetCurrentDifficulty() >= ROGUE_ELITE_START_DIFFICULTY)
            offset = 1;

        offset = min(offset, trainer->encounterTextCount - 1);

        for(; offset > 0; --offset)
        {
            str = trainer->encounterText[TRAINER_STRING_COUNT * offset + textId];
            if(str != NULL)
                return str;
        }

        // Fallback to offset 0 (TODO should fallback to a sensible default tbh)
        return trainer->encounterText[textId];
    }
    else
    {
        // TODO - Predictably randomise per trainer id instance?
        return trainer->encounterText[textId];
    }

    return NULL;
}

bool8 Rogue_GetTrainerFlag(u16 trainerNum)
{
    return FlagGet(TRAINER_FLAGS_START + trainerNum);
}

u16 Rogue_GetTrainerObjectEventGfx(u16 trainerNum)
{
    const struct RogueTrainer* trainer = Rogue_GetTrainer(trainerNum);
    return trainer->objectEventGfx;
}

u16 Rogue_GetTrainerNumFromObjectEvent(struct ObjectEvent *curObject)
{
    u8 i;

    // TODO - Could check the script?? (Was having issues though)

    // Grab the trainer which matches the gfx
    for(i = 0; i < gRogueTrainerCount; ++i)
    {
        if(curObject->graphicsId == Rogue_GetTrainerObjectEventGfx(i))
        {
            return i;
        }
    }

    return gRogueTrainerCount;
}

u16 Rogue_GetTrainerNumFromLastInteracted()
{
    u8 lastTalkedId = VarGet(VAR_LAST_TALKED);
    u8 objEventId = GetObjectEventIdByLocalIdAndMap(lastTalkedId, gSaveBlock1Ptr->location.mapNum, gSaveBlock1Ptr->location.mapGroup);

    if(objEventId < OBJECT_EVENTS_COUNT)
    {
        return Rogue_GetTrainerNumFromObjectEvent(&gObjectEvents[objEventId]);
    }

    return TRAINER_NONE;
}

u8 Rogue_GetTrainerWeather(u16 trainerNum)
{
    const struct RogueTrainer* trainer = Rogue_GetTrainer(trainerNum);
    u8 weatherType = WEATHER_NONE;

    if(Rogue_IsAnyBossTrainer(trainerNum) && trainer != NULL)
    {
        switch (Rogue_GetConfigRange(CONFIG_RANGE_TRAINER))
        {
        case DIFFICULTY_LEVEL_EASY:
            weatherType = WEATHER_NONE;
            break;
        
        case DIFFICULTY_LEVEL_MEDIUM:
            if(Rogue_GetCurrentDifficulty() > 2)
                weatherType = trainer->preferredWeather;
            break;
        
        case DIFFICULTY_LEVEL_HARD:
            if(Rogue_GetCurrentDifficulty() > 0)
                weatherType = trainer->preferredWeather;
            break;
        
        case DIFFICULTY_LEVEL_BRUTAL:
            weatherType = trainer->preferredWeather;
            break;
        }
    }

    if(weatherType == WEATHER_DEFAULT)
    {
        weatherType = gRogueTypeWeatherTable[trainer->typeAssignment];
    }

    return weatherType;
}

static u8 CalculateLvlFor(u8 difficulty)
{
    if(FlagGet(FLAG_ROGUE_GAUNTLET_MODE))
    {
        return MAX_LEVEL;
    }

    // Gym leaders lvs 10 -> 80
    if(difficulty < ROGUE_ELITE_START_DIFFICULTY)
    {
        return 10 * (difficulty + 1);

        return min(100, 15 + 10 * difficulty);
    }
    else if(difficulty < ROGUE_CHAMP_START_DIFFICULTY)
    {
        difficulty -= ROGUE_ELITE_START_DIFFICULTY;
        return 80 + 4 * (difficulty + 1);
    }
    else
    {
        return MAX_LEVEL;
    }
    
}

static u8 Rogue_CalculateTrainerLvlCap(bool8 keyBattle)
{
    // We need trainer battles to be consistent
    if(RogueMP_IsActive())
    {
        u8 levelOffset = 0;

        if(!keyBattle)
        {
            // Offset levels based on column rather than player level
            switch (RogueAdv_GetTileNum())
            {
            case 0:
                levelOffset = 10;
                break;
            
            case 1:
                levelOffset = 5;
                break;
            }
        }

        return Rogue_CalculateBossMonLvl() - levelOffset;
    }
    else
    {
        // In single player scale battles based on the current player soft level cap
        return Rogue_CalculatePlayerMonLvl();
    }
}

u8 Rogue_CalculatePlayerMonLvl()
{
    return Rogue_CalculateBossMonLvl() - gRogueRun.currentLevelOffset;
}

u8 Rogue_CalculateTrainerMonLvl()
{
    u8 difficultyModifier = Rogue_GetEncounterDifficultyModifier();
    u8 startLvl;
    u8 playerLvl;

    if(Rogue_GetCurrentDifficulty() == 0)
    {
        startLvl = 5;
        playerLvl = max(5, Rogue_CalculateTrainerLvlCap(FALSE) / 2); // climb slowly for difficulty 1
    }
    else
    {
        startLvl = CalculateLvlFor(Rogue_GetCurrentDifficulty() - 1);
        playerLvl = Rogue_CalculateTrainerLvlCap(FALSE);
    }

    switch (difficultyModifier)
    {
    case ADVPATH_SUBROOM_ROUTE_CALM:
        // Lag behind
        return startLvl;

    case ADVPATH_SUBROOM_ROUTE_AVERAGE:
        // Average of 2 so gap becomes larger as you reach level cap
        return (startLvl + playerLvl) / 2;

    case ADVPATH_SUBROOM_ROUTE_TOUGH:
        // Scale with player level
        return max(startLvl, playerLvl > 5 ? playerLvl - 5 : 5);

    default:
        AGB_ASSERT(FALSE);
        return playerLvl;
    }
}

u8 Rogue_CalculateMiniBossMonLvl()
{
    return Rogue_CalculateTrainerLvlCap(TRUE) - 5;
}

u8 Rogue_CalculateRivalMonLvl()
{
    return Rogue_CalculateTrainerLvlCap(TRUE);
}

u8 Rogue_CalculateBossMonLvl()
{
    return CalculateLvlFor(Rogue_GetCurrentDifficulty());
}

u8 Rogue_GetTrainerTypeAssignment(u16 trainerNum)
{
    const struct RogueTrainer* trainer = Rogue_GetTrainer(trainerNum);
    return trainer->typeAssignment;
}

u16 Rogue_GetTrainerPokeballId(u16 trainerNum)
{
    const struct RogueTrainer* trainer = Rogue_GetTrainer(trainerNum);

    if(Rogue_IsAnyBossTrainer(trainerNum))
    {
        if(trainer->preferredPokeballItem != ITEM_NONE)
        {
            return trainer->preferredPokeballItem;
        }

        // Default pokeballs based on type
        switch (Rogue_GetTrainerTypeAssignment(trainerNum))
        {
            case TYPE_BUG:
                return ITEM_NET_BALL;
            case TYPE_DRAGON:
                return ITEM_MASTER_BALL;
            case TYPE_FIGHTING:
                return ITEM_ULTRA_BALL;
            case TYPE_FIRE:
                return ITEM_REPEAT_BALL;
            case TYPE_FLYING:
                return ITEM_PREMIER_BALL;
            case TYPE_GRASS:
                return ITEM_NEST_BALL;
            case TYPE_WATER:
                return ITEM_DIVE_BALL;
            case TYPE_ICE:
                return ITEM_GREAT_BALL;
            case TYPE_ROCK:
                return ITEM_TIMER_BALL;
            case TYPE_MYSTERY:
                return ITEM_LUXURY_BALL;

#ifdef ROGUE_EXPANSION
            case TYPE_FAIRY:
                return ITEM_LOVE_BALL;
            case TYPE_GHOST:
                return ITEM_DUSK_BALL;
            case TYPE_STEEL:
                return ITEM_HEAVY_BALL;
            case TYPE_PSYCHIC:
                return ITEM_DREAM_BALL;
            case TYPE_DARK:
                return ITEM_DUSK_BALL;
            case TYPE_ELECTRIC:
                return ITEM_QUICK_BALL;
            case TYPE_GROUND:
                return ITEM_FRIEND_BALL;
            case TYPE_POISON:
                return ITEM_MOON_BALL;
#endif
        }
    }

    return ITEM_POKE_BALL;
}

u16 Rogue_GetTrainerTypeGroupId(u16 trainerNum)
{
    if(Rogue_IsBossTrainer(trainerNum))
    {
        const struct RogueTrainer* trainer = Rogue_GetTrainer(trainerNum);

        // We're gonna always use the trainer's assigned type to prevent dupes
        // The history buffer will be wiped between stages to allow for types to re-appear later e.g. juan can appear as gym and wallace can appear as champ
        u8 type = trainer->typeAssignmentGroup;

        // None type trainers are unqiue, so we don't care about the type repeat
        if(type != TYPE_NONE)
            return type;
    }

    // Just avoid repeating this trainer
    return NUMBER_OF_MON_TYPES + trainerNum;
}

bool8 Rogue_IsValidTrainerShinySpecies(u16 trainerNum, u16 species)
{
    const struct RogueTrainer* trainer = Rogue_GetTrainer(trainerNum);

#ifdef ROGUE_EXPANSION
    species = GET_BASE_SPECIES_ID(species);
#endif
    species = Rogue_GetEggSpecies(species);

    return trainer->potentialShinySpecies == species;
}

bool8 Rogue_UseCustomPartyGenerator(u16 trainerNum)
{
    return TRUE;
}


static u16 GetTrainerHistoryKey(u16 trainerNum)
{
    if(Rogue_IsBossTrainer(trainerNum))
    {
        // We're gonna always use the trainer's assigned type to prevent dupes
        // The history buffer will be wiped between stages to allow for types to re-appear later e.g. juan can appear as gym and wallace can appear as champ
        u16 type = Rogue_GetTrainerTypeAssignment(trainerNum);

        // None type trainers are unqiue, so we don't care about the type repeat
        if(type == TYPE_NONE)
            return NUMBER_OF_MON_TYPES + trainerNum;

        return type;
    }

    // Just avoid repeating this trainer
    return trainerNum;
}

static void GetGlobalFilterFlags(u32* includeFlags, u32* excludeFlags)
{
    *includeFlags = TRAINER_FLAG_NONE;
    *excludeFlags = TRAINER_FLAG_NONE;

    if(Rogue_GetConfigToggle(CONFIG_TOGGLE_TRAINER_ROGUE))
        *includeFlags |= TRAINER_FLAG_REGION_ROGUE;

    if(Rogue_GetConfigToggle(CONFIG_TOGGLE_TRAINER_KANTO))
        *includeFlags |= TRAINER_FLAG_REGION_KANTO;

    if(Rogue_GetConfigToggle(CONFIG_TOGGLE_TRAINER_JOHTO))
        *includeFlags |= TRAINER_FLAG_REGION_JOHTO;

    if(Rogue_GetConfigToggle(CONFIG_TOGGLE_TRAINER_HOENN))
        *includeFlags |= TRAINER_FLAG_REGION_HOENN;

#ifdef ROGUE_EXPANSION
    if(Rogue_GetConfigToggle(CONFIG_TOGGLE_TRAINER_SINNOH))
        *includeFlags |= TRAINER_FLAG_REGION_SINNOH;

    if(Rogue_GetConfigToggle(CONFIG_TOGGLE_TRAINER_UNOVA))
        *includeFlags |= TRAINER_FLAG_REGION_UNOVA;

    if(Rogue_GetConfigToggle(CONFIG_TOGGLE_TRAINER_KALOS))
        *includeFlags |= TRAINER_FLAG_REGION_KALOS;

    if(Rogue_GetConfigToggle(CONFIG_TOGGLE_TRAINER_ALOLA))
        *includeFlags |= TRAINER_FLAG_REGION_ALOLA;

    if(Rogue_GetConfigToggle(CONFIG_TOGGLE_TRAINER_GALAR))
        *includeFlags |= TRAINER_FLAG_REGION_GALAR;
#endif

    // TODO - Rework this flag
    if(Rogue_GetConfigRange(CONFIG_RANGE_TRAINER_ORDER) == TRAINER_ORDER_RAINBOW)
        *excludeFlags |= TRAINER_FLAG_MISC_RAINBOW_EXCLUDE;
    else
        *excludeFlags |= TRAINER_FLAG_MISC_RAINBOW_ONLY;

    if(*includeFlags == TRAINER_FLAG_NONE)
    {
        // Safety fallback (Should never reach here)
        AGB_ASSERT(FALSE);
        *includeFlags = TRAINER_FLAG_REGION_DEFAULT;
    }
}

static u16 Rogue_ChooseTrainerId(u32 includeFlags, u32 excludeFlags, u16* historyBuffer, u16 historyBufferCapacity)
{
    u8 i;
    u16 trainerNum = gRogueTrainerCount;

    RogueTrainerQuery_Begin();

    while(trainerNum == gRogueTrainerCount)
    {
        // Populate query
        //
        RogueTrainerQuery_Reset(QUERY_FUNC_INCLUDE);

        // Only include trainers we want
        RogueTrainerQuery_ContainsTrainerFlag(QUERY_FUNC_INCLUDE, includeFlags);
        RogueTrainerQuery_ContainsTrainerFlag(QUERY_FUNC_EXCLUDE, excludeFlags);

        GetGlobalFilterFlags(&includeFlags, &excludeFlags);
        RogueTrainerQuery_ContainsTrainerFlag(QUERY_FUNC_INCLUDE, includeFlags);
        RogueTrainerQuery_ContainsTrainerFlag(QUERY_FUNC_EXCLUDE, excludeFlags);

        // Exclude any types we've already encountered
        for(i = 0; i < historyBufferCapacity; ++i)
        {
            if(historyBuffer[i] != INVALID_HISTORY_ENTRY)
                RogueTrainerQuery_IsOfTypeGroup(QUERY_FUNC_EXCLUDE, historyBuffer[i]);
        }

        // Select random
        //
        RogueWeightQuery_Begin();
        {
            RogueWeightQuery_FillWeights(1);

            if(RogueWeightQuery_HasAnyWeights())
            {
                trainerNum = RogueWeightQuery_SelectRandomFromWeights(RogueRandom());
            }
            else
            {
                // Usually, this isn't intentional, so assert here
                AGB_ASSERT(FALSE);

                if(historyBuffer != NULL)
                {
                    // We've exhausted the options, so wipe and try again
                    memset(&historyBuffer[0], INVALID_HISTORY_ENTRY, sizeof(u16) * historyBufferCapacity);
                }
            }
        }
        RogueWeightQuery_End();
    }

    RogueTrainerQuery_End();

    if(historyBuffer != NULL)
        HistoryBufferPush(&historyBuffer[0], historyBufferCapacity, Rogue_GetTrainerTypeGroupId(trainerNum));
    return trainerNum;
}

static u16 Rogue_ChooseBossTrainerId(u16 difficulty, u16* historyBuffer, u16 historyBufferCapacity)
{
    u32 includeFlags = 0;
    u32 excludeFlags = 0;

    switch (Rogue_GetConfigRange(CONFIG_RANGE_TRAINER_ORDER))
    {
    case TRAINER_ORDER_DEFAULT:
        {
            // Only include trainers we want
            includeFlags = TRAINER_FLAG_NONE;
            if(difficulty >= ROGUE_CHAMP_START_DIFFICULTY)
                includeFlags |= TRAINER_FLAG_CLASS_CHAMP;
            else if(difficulty >= ROGUE_ELITE_START_DIFFICULTY)
                includeFlags |= TRAINER_FLAG_CLASS_ANY_ELITE;
            else
                includeFlags |= TRAINER_FLAG_CLASS_ANY_GYM;
        }
        break;
    
    case TRAINER_ORDER_RAINBOW:
        includeFlags = TRAINER_FLAG_CLASS_ANY_MAIN_BOSS;
        break;
    
    case TRAINER_ORDER_OFFICIAL:
        {
            switch (difficulty)
            {
            case ROGUE_GYM_START_DIFFICULTY + 0:
                includeFlags |= TRAINER_FLAG_CLASS_GYM_1;
                break;
            case ROGUE_GYM_START_DIFFICULTY + 1:
                includeFlags |= TRAINER_FLAG_CLASS_GYM_2;
                break;
            case ROGUE_GYM_START_DIFFICULTY + 2:
                includeFlags |= TRAINER_FLAG_CLASS_GYM_3;
                break;
            case ROGUE_GYM_START_DIFFICULTY + 3:
                includeFlags |= TRAINER_FLAG_CLASS_GYM_4;
                break;
            case ROGUE_GYM_START_DIFFICULTY + 4:
                includeFlags |= TRAINER_FLAG_CLASS_GYM_5;
                break;
            case ROGUE_GYM_START_DIFFICULTY + 5:
                includeFlags |= TRAINER_FLAG_CLASS_GYM_6;
                break;
            case ROGUE_GYM_START_DIFFICULTY + 6:
                includeFlags |= TRAINER_FLAG_CLASS_GYM_7;
                break;
            case ROGUE_GYM_START_DIFFICULTY + 7:
                includeFlags |= TRAINER_FLAG_CLASS_GYM_8;
                break;

            case ROGUE_ELITE_START_DIFFICULTY + 0:
                includeFlags |= TRAINER_FLAG_CLASS_ELITE_1;
                break;
            case ROGUE_ELITE_START_DIFFICULTY + 1:
                includeFlags |= TRAINER_FLAG_CLASS_ELITE_2;
                break;
            case ROGUE_ELITE_START_DIFFICULTY + 2:
                includeFlags |= TRAINER_FLAG_CLASS_ELITE_3;
                break;
            case ROGUE_ELITE_START_DIFFICULTY + 3:
                includeFlags |= TRAINER_FLAG_CLASS_ELITE_4;
                break;

            case ROGUE_CHAMP_START_DIFFICULTY + 0:
            case ROGUE_CHAMP_START_DIFFICULTY + 1:
                includeFlags |= TRAINER_FLAG_CLASS_CHAMP;
                break;

            default:
                AGB_ASSERT(FALSE);
                break;
            }
        }
        break;

    default:
        AGB_ASSERT(FALSE);
        includeFlags = TRAINER_FLAG_CLASS_ANY_MAIN_BOSS;
        break;
    }

    return Rogue_ChooseTrainerId(includeFlags, excludeFlags, historyBuffer, historyBufferCapacity);
}

void Rogue_ChooseBossTrainersForNewAdventure()
{
    u8 difficulty;
    u16 trainerNum;
    u16 historyBuffer[ROGUE_MAX_BOSS_COUNT];

    memset(&gRogueRun.bossTrainerNums[0], TRAINER_NONE, sizeof(u16) * ARRAY_COUNT(gRogueRun.bossTrainerNums));
    memset(&historyBuffer[0], INVALID_HISTORY_ENTRY, sizeof(u16) * ARRAY_COUNT(historyBuffer));

    DebugPrint("Picking trainers");

    for(difficulty = 0; difficulty < ROGUE_MAX_BOSS_COUNT; ++difficulty)
    {
        if(Rogue_UseFinalQuestEffects() && difficulty >= ROGUE_CHAMP_START_DIFFICULTY)
        {
            if(difficulty == ROGUE_FINAL_CHAMP_DIFFICULTY)
            {
                trainerNum = Rogue_ChooseBossTrainerId(difficulty, historyBuffer, ARRAY_COUNT(historyBuffer));
            }
            else
            {
                AGB_ASSERT(gRogueRun.rivalTrainerNum != TRAINER_NONE);
                trainerNum = gRogueRun.rivalTrainerNum; // Rival is the fake final boss
            }
        }
        else if(difficulty == ROGUE_FINAL_CHAMP_DIFFICULTY)
        {
            AGB_ASSERT(gRogueRun.rivalTrainerNum != TRAINER_NONE);
            trainerNum = gRogueRun.rivalTrainerNum; // Rival is always final boss
        }
        else
        {
            // Clear the history buffer, as we track based on types
            // In rainbow mode, the type can only appear once though
            if(Rogue_GetConfigRange(CONFIG_RANGE_TRAINER_ORDER) != TRAINER_ORDER_RAINBOW)
            {
                switch(difficulty)
                {
                    case ROGUE_ELITE_START_DIFFICULTY:
                    case ROGUE_CHAMP_START_DIFFICULTY:
                        memset(&historyBuffer[0], INVALID_HISTORY_ENTRY, sizeof(u16) * ARRAY_COUNT(historyBuffer));
                        break;
                }
            }

            trainerNum = Rogue_ChooseBossTrainerId(difficulty, historyBuffer, ARRAY_COUNT(historyBuffer));
        }

        gRogueRun.bossTrainerNums[difficulty] = trainerNum;
        DebugPrintf("    [%d] = %d", difficulty, trainerNum);
    }
}

#define RIVAL_STARTER_INDEX 1
#define RIVAL_BASE_PARTY_SIZE 5

static u16 Rogue_ChooseRivalTrainerId()
{
    u32 includeFlags;
    u32 excludeFlags;

    // Only include trainers we want
    includeFlags = TRAINER_FLAG_CLASS_RIVAL;
    excludeFlags = 0;

    return Rogue_ChooseTrainerId(includeFlags, excludeFlags, NULL, 0);
}

static u8 SelectRivalWeakestMon(u16* speciesBuffer, u8 partySize)
{
    u8 i;
    u16 species;
    u8 lowestBstIdx;
    u16 lowestBst;
    u16 currBst;

    lowestBstIdx = 255;
    lowestBst = 255;

    for(i = 0; i < partySize; ++i)
    {
        // Never remove the starter
        if(i == RIVAL_STARTER_INDEX)
            continue;

        species = speciesBuffer[i];
        if(species != SPECIES_NONE)
        {
            currBst = RoguePokedex_GetSpeciesBST(species);
            if(lowestBstIdx == 255 || currBst < lowestBstIdx)
            {
                lowestBstIdx = currBst;
                lowestBstIdx = i;
            }
        }
    }

    AGB_ASSERT(lowestBstIdx != 255);
    return lowestBstIdx;
}

void Rogue_ChooseRivalTrainerForNewAdventure()
{
    u8 i;
    u16 trainerNum = Rogue_ChooseRivalTrainerId();
    DebugPrintf("Picking rival = %d", trainerNum);

    gRogueRun.rivalTrainerNum = trainerNum;
    gRogueRun.rivalHasShiny = RogueRandomChance(TRAINER_SHINY_PERC, 0);
    memset(gRogueRun.rivalSpecies, SPECIES_NONE, sizeof(gRogueRun.rivalSpecies));

    // We can encounter the rival up to the first E4 encounter (Technically not entered the E4 so I'll allow it)
    // Set this up to assume 4 encounters for now to ensure they are evenly spaced
    AGB_ASSERT(ROGUE_RIVAL_MAX_ROUTE_ENCOUNTERS == 4);
    
    // First encounter just before or just after 1st badge
    gRogueRun.rivalEncounterDifficulties[0] = RogueRandom() % 2;

    // Around middle of run
    gRogueRun.rivalEncounterDifficulties[1] = ROGUE_GYM_MID_DIFFICULTY - 1 + (RogueRandom() % 3);

    // Going to very occasionally have 2 mid run encounters with rival
    if((RogueRandom() % 4) == 0)
    {
        gRogueRun.rivalEncounterDifficulties[1] = 3 + (RogueRandom() % 2);
        gRogueRun.rivalEncounterDifficulties[2] = ROGUE_GYM_MID_DIFFICULTY + 1 + (RogueRandom() % 2);
    }
    // Only have 1 mid run encounter
    else
    {
        // Around middle of run
        gRogueRun.rivalEncounterDifficulties[1] = ROGUE_GYM_MID_DIFFICULTY - 1 + (RogueRandom() % 3);
        gRogueRun.rivalEncounterDifficulties[2] = gRogueRun.rivalEncounterDifficulties[1];
    }

    // Last encounter just before or just after last gym
    gRogueRun.rivalEncounterDifficulties[3] = ROGUE_ELITE_START_DIFFICULTY - (RogueRandom() % 2);
}

static void SortByBst(u16* speciesBuffer, u16 bufferSize)
{
    u8 i, j;
    u16 temp;

    for(i = 0; i < bufferSize; ++i)
    for(j = 1; j < bufferSize; ++j)
    {
        if(i == j)
            continue;

        if(RoguePokedex_GetSpeciesBST(speciesBuffer[j]) < RoguePokedex_GetSpeciesBST(speciesBuffer[j - 1]))
        {
            SWAP(speciesBuffer[j], speciesBuffer[j - 1], temp);
        }
    }
}

static void SelectAndMoveStarterSpecies(u16 trainerNum, u16* speciesBuffer, u16 bufferSize)
{
    u8 i;
    u16 score;
    u16 highestScore = 0;
    u8 highestIndex = RIVAL_STARTER_INDEX;
    bool8 preferManualChoice = (RogueRandom() % 5) == 0;
    
    // Find the most desireable starter which is the mon with the highest BST and ideally 3 evos
    // Then move it into the starter index (This is so it will appear all fights and that slot is exempt from being replaced for a later mon)
    //
    for(i = 0; i < bufferSize; ++i)
    {
        score = RoguePokedex_GetSpeciesBST(speciesBuffer[i]) + 1000 * Rogue_GetActiveEvolutionCount(speciesBuffer[i]);

        // occassionally we're going to prefer our manually chosen ace/shiny mon (if we have it)
        if(preferManualChoice && Rogue_IsValidTrainerShinySpecies(trainerNum, speciesBuffer[i]))
        {
            score = 30000;
        }

        if(score > highestScore)
        {
            highestIndex = i;
            highestScore = score;
        }
    }

    if(highestIndex != RIVAL_STARTER_INDEX)
    {
        u16 temp;
        SWAP(speciesBuffer[RIVAL_STARTER_INDEX], speciesBuffer[highestIndex], temp);
    }
}

void Rogue_GenerateRivalBaseTeamIfNeeded()
{
    if(gRogueRun.rivalSpecies[0] == SPECIES_NONE)
    {
        u8 i, j;
        u32 savedRng = gRngRogueValue;

        // Fake the difficulty for the generator
        u16 tempDifficulty = Rogue_GetCurrentDifficulty();
        Rogue_SetCurrentDifficulty(ROGUE_ELITE_START_DIFFICULTY - 2); // Generate base party as if we're about midway through

        // Apply some base seed for anything which needs to be randomly setup
        SeedRogueRng(gRogueRun.baseSeed * 8071 + 6632);

        DebugPrint("Generating Rival Base Team");

        // Create initial base team
        //
        {
            CreateTrainerPartyInternal(gRogueRun.rivalTrainerNum, &gEnemyParty[0], RIVAL_BASE_PARTY_SIZE, RIVAL_BASE_PARTY_SIZE, FALSE, 0);

            for(i = 0; i < RIVAL_BASE_PARTY_SIZE; ++i)
            {
                AGB_ASSERT(gRogueRun.rivalSpecies[i] == SPECIES_NONE);
                gRogueRun.rivalSpecies[i] = GetMonData(&gEnemyParty[i], MON_DATA_SPECIES);
                AGB_ASSERT(gRogueRun.rivalSpecies[i] != SPECIES_NONE);
                DebugPrintf("    [%d] = %d", i, gRogueRun.rivalSpecies[i]);
            }
        }

        // Restore difficulty
        Rogue_SetCurrentDifficulty(tempDifficulty);
        gRngRogueValue = savedRng;

        // For just the base species we're going to sort based on BST so weakest mons appear first
        SortByBst(gRogueRun.rivalSpecies, RIVAL_BASE_PARTY_SIZE);

        // Assign the starter to stick with the player throughout
        SelectAndMoveStarterSpecies(gRogueRun.rivalTrainerNum, gRogueRun.rivalSpecies, RIVAL_BASE_PARTY_SIZE);

        // Zero mons to avoid conflicts if called during team generation
        ZeroEnemyPartyMons();
    }
}

void Rogue_GenerateRivalSwapTeamIfNeeded()
{
    if(gRogueRun.rivalSpecies[RIVAL_BASE_PARTY_SIZE] == SPECIES_NONE)
    {
        u8 i, j;
        u32 savedRng = gRngRogueValue;

        // Fake the difficulty for the generator
        u16 tempDifficulty = Rogue_GetCurrentDifficulty();
        Rogue_SetCurrentDifficulty(ROGUE_ELITE_START_DIFFICULTY);

        // Apply some base seed for anything which needs to be randomly setup
        SeedRogueRng(gRogueRun.baseSeed * 6632 + 8073);

        AGB_ASSERT(gRogueRun.rivalSpecies[0] != SPECIES_NONE);

        DebugPrint("Generating Rival Swap Team");

        // Handle replacement late game mons
        //
        // Remove the weakest species
        {
            u8 write;
            u32 hp = 0;
            u16 speciesBuffer[PARTY_SIZE];

            memcpy(speciesBuffer, gRogueRun.rivalSpecies, sizeof(speciesBuffer));

            for(i = PARTY_SIZE; i < ROGUE_RIVAL_TOTAL_MON_COUNT; ++i)
            {
                j = SelectRivalWeakestMon(speciesBuffer, RIVAL_BASE_PARTY_SIZE);
                speciesBuffer[j] = SPECIES_NONE;
            }

            // Create a placeholder party (TODO remove this after refactoring the rest of the generator not to read from party struct)
            ZeroEnemyPartyMons();
            write = 0;

            for(i = 0; i < PARTY_SIZE; ++i)
            {
                if(speciesBuffer[i] != SPECIES_NONE)
                    CreateMon(&gEnemyParty[write++], speciesBuffer[i], MAX_LEVEL, 0, FALSE, 0, OT_ID_RANDOM_NO_SHINY, 0);
            }
        }

        // Add the new mons
        {
            u8 replacingPartySize = PARTY_SIZE - (ROGUE_RIVAL_TOTAL_MON_COUNT - RIVAL_BASE_PARTY_SIZE);
            AGB_ASSERT(replacingPartySize == CalculateEnemyPartyCount());
            
            // First swap mon is as if we're starting E4
            Rogue_SetCurrentDifficulty(ROGUE_ELITE_START_DIFFICULTY);
            CreateTrainerPartyInternal(gRogueRun.rivalTrainerNum, &gEnemyParty[0], replacingPartySize + 1, PARTY_SIZE, FALSE, replacingPartySize);

            for(i = replacingPartySize; i < PARTY_SIZE; ++i)
            {
                u8 offset = RIVAL_BASE_PARTY_SIZE + (i - replacingPartySize);
                AGB_ASSERT(offset >= RIVAL_BASE_PARTY_SIZE);
                AGB_ASSERT(offset < ROGUE_RIVAL_TOTAL_MON_COUNT);

                if(i == replacingPartySize + 1)
                {
                    // Second swap mon is as if we're champ
                    Rogue_SetCurrentDifficulty(ROGUE_FINAL_CHAMP_DIFFICULTY);
                    CreateTrainerPartyInternal(gRogueRun.rivalTrainerNum, &gEnemyParty[0], PARTY_SIZE, PARTY_SIZE, FALSE, replacingPartySize + 1);
                }

                AGB_ASSERT(gRogueRun.rivalSpecies[offset] == SPECIES_NONE);
                gRogueRun.rivalSpecies[offset] = GetMonData(&gEnemyParty[i], MON_DATA_SPECIES);
                AGB_ASSERT(gRogueRun.rivalSpecies[offset] != SPECIES_NONE);
                DebugPrintf("    [%d] = %d", offset, gRogueRun.rivalSpecies[offset]);
            }
        }

        // Restore difficulty
        Rogue_SetCurrentDifficulty(tempDifficulty);
        gRngRogueValue = savedRng;

        // Sort new mons by BST so we save the strongest mons to the fianl fights
        SortByBst(&gRogueRun.rivalSpecies[RIVAL_BASE_PARTY_SIZE], (ROGUE_RIVAL_TOTAL_MON_COUNT - RIVAL_BASE_PARTY_SIZE));

        // Zero mons to avoid conflicts if called during team generation
        ZeroEnemyPartyMons();
    }
}

static u16 Rogue_RouteTrainerId(u16* historyBuffer, u16 historyBufferCapacity)
{
    u32 includeFlags;
    u32 excludeFlags;

    // Only include trainers we want
    includeFlags = TRAINER_FLAG_CLASS_ROUTE;
    excludeFlags = TRAINER_FLAG_NONE;

    return Rogue_ChooseTrainerId(includeFlags, excludeFlags, historyBuffer, historyBufferCapacity);
}

void Rogue_ChooseRouteTrainers(u16* writeBuffer, u16 bufferCapacity)
{
    u8 i;
    u16 trainerNum;
    u16 historyBuffer[ROGUE_MAX_BOSS_COUNT];

    memset(writeBuffer, TRAINER_NONE, sizeof(u16) * bufferCapacity);
    memset(&historyBuffer[0], INVALID_HISTORY_ENTRY, sizeof(u16) * ARRAY_COUNT(historyBuffer));

    for(i = 0; i < bufferCapacity; ++i)
    {
        trainerNum = Rogue_RouteTrainerId(historyBuffer, ARRAY_COUNT(historyBuffer));
        writeBuffer[i] = trainerNum;
    }
}

u16 Rogue_NextMinibossTrainerId()
{
    // TODO
    return gRogueRun.rivalTrainerNum;
    //u16 trainerNum = NextTrainerNum(TRAINER_NUM_MINIBOSS_START, TRAINER_NUM_MINIBOSS_END, &gRogueAdvPath.miniBossHistoryBuffer[0], ARRAY_COUNT(gRogueAdvPath.miniBossHistoryBuffer));
    //const struct RogueTrainer* trainer = Rogue_GetTrainer(trainerNum);
//
    //if(trainer != NULL)
    //{
    //    HistoryBufferPush(&gRogueAdvPath.miniBossHistoryBuffer[0], ARRAY_COUNT(gRogueAdvPath.miniBossHistoryBuffer), GetTrainerHistoryKey(trainerNum));
    //}
//
    //return trainerNum;
}

void Rogue_GetPreferredElite4Map(u16 trainerNum, s8* mapGroup, s8* mapNum)
{
    u8 type = Rogue_GetTrainerTypeAssignment(trainerNum);
    *mapGroup = gRogueTypeToEliteRoom[type].group;
    *mapNum = gRogueTypeToEliteRoom[type].num;
}

static void ConfigurePartyScratchSettings(u16 trainerNum, struct TrainerPartyScratch* scratch)
{
    // Configure evos, strong presets and legend settings
    switch (Rogue_GetConfigRange(CONFIG_RANGE_TRAINER))
    {
    case DIFFICULTY_LEVEL_EASY:
        if(Rogue_GetCurrentDifficulty() >= 8)
        {
            scratch->allowItemEvos = TRUE;
            scratch->allowWeakLegends = TRUE;
        }
        break;

    case DIFFICULTY_LEVEL_MEDIUM:
        if(Rogue_GetCurrentDifficulty() >= 8)
        {
            scratch->allowStrongLegends = TRUE;
            scratch->preferStrongSpecies = TRUE;
        }
        else if(Rogue_GetCurrentDifficulty() >= 7)
        {
            scratch->allowWeakLegends = TRUE;
        }
        if(Rogue_GetCurrentDifficulty() >= 4)
        {
            scratch->allowItemEvos = TRUE;
        }
        break;

    case DIFFICULTY_LEVEL_HARD:
        if(Rogue_GetCurrentDifficulty() >= 5)
        {
            scratch->allowStrongLegends = TRUE;
            scratch->preferStrongSpecies = TRUE;
        }
        else if(Rogue_GetCurrentDifficulty() >= 2)
        {
            scratch->allowWeakLegends = TRUE;
            scratch->allowItemEvos = TRUE;
        }
        break;

    case DIFFICULTY_LEVEL_BRUTAL:
        if(Rogue_GetCurrentDifficulty() >= 2)
        {
            scratch->allowStrongLegends = TRUE;
            scratch->preferStrongSpecies = TRUE;
        }
        else if(Rogue_GetCurrentDifficulty() >= 1)
        {
            scratch->allowWeakLegends = TRUE;
            scratch->allowItemEvos = TRUE;
        }
        break;
    }
}

static u8 CalculateMonFixedIV(u16 trainerNum)
{
    u8 fixedIV;

    switch (Rogue_GetConfigRange(CONFIG_RANGE_TRAINER))
    {
    case DIFFICULTY_LEVEL_EASY:
        fixedIV = 0;
        break;

    case DIFFICULTY_LEVEL_MEDIUM:
        if(Rogue_IsKeyTrainer(trainerNum))
        {
            if(Rogue_GetCurrentDifficulty() >= ROGUE_CHAMP_START_DIFFICULTY)
                fixedIV = 16;
            else if(Rogue_GetCurrentDifficulty() >= ROGUE_ELITE_START_DIFFICULTY)
                fixedIV = 10;
            else if(Rogue_GetCurrentDifficulty() >= ROGUE_GYM_MID_DIFFICULTY)
                fixedIV = 8;
            else if(Rogue_GetCurrentDifficulty() >= ROGUE_GYM_MID_DIFFICULTY - 1)
                fixedIV = 6;
            else
                fixedIV = 0;
        }
        else
        {
            fixedIV = 0;
        }
        break;

    case DIFFICULTY_LEVEL_HARD:
        if(Rogue_IsKeyTrainer(trainerNum))
        {
            if(Rogue_GetCurrentDifficulty() >= ROGUE_CHAMP_START_DIFFICULTY)
                fixedIV = 31;
            else if(Rogue_GetCurrentDifficulty() >= ROGUE_ELITE_START_DIFFICULTY)
                fixedIV = 21;
            else if(Rogue_GetCurrentDifficulty() >= ROGUE_GYM_MID_DIFFICULTY + 2)
                fixedIV = 19;
            else if(Rogue_GetCurrentDifficulty() >= ROGUE_GYM_MID_DIFFICULTY - 1)
                fixedIV = 15;
            else if(Rogue_GetCurrentDifficulty() >= 1)
                fixedIV = 11;
            else
                fixedIV = 5;
        }
        else
        {
            fixedIV = (Rogue_GetCurrentDifficulty() > 8) ? 13 : 5;
        }
        break;

    case DIFFICULTY_LEVEL_BRUTAL:
        if(Rogue_IsKeyTrainer(trainerNum))
        {
            // Bosses are cracked a LOT sooner
            if(Rogue_GetCurrentDifficulty() >= ROGUE_GYM_MID_DIFFICULTY + 1)
                fixedIV = 31;
            else if(Rogue_GetCurrentDifficulty() >= ROGUE_GYM_MID_DIFFICULTY - 1)
                fixedIV = 21;
            else if(Rogue_GetCurrentDifficulty() >= 1)
                fixedIV = 19;
            else
                fixedIV = 15;
        }
        else
        {
            // Regular trainers scale like hard mode bosses
            if(Rogue_GetCurrentDifficulty() >= ROGUE_CHAMP_START_DIFFICULTY)
                fixedIV = 31;
            else if(Rogue_GetCurrentDifficulty() >= ROGUE_ELITE_START_DIFFICULTY)
                fixedIV = 21;
            else if(Rogue_GetCurrentDifficulty() >= ROGUE_GYM_MID_DIFFICULTY + 2)
                fixedIV = 19;
            else if(Rogue_GetCurrentDifficulty() >= ROGUE_GYM_MID_DIFFICULTY - 1)
                fixedIV = 15;
            else if(Rogue_GetCurrentDifficulty() >= 1)
                fixedIV = 11;
            else
                fixedIV = 5;
        }
        break;
    }

    return fixedIV;
}

static u8 ShouldTrainerOptimizeCoverage(u16 trainerNum)
{
    switch (Rogue_GetConfigRange(CONFIG_RANGE_TRAINER))
    {
    case DIFFICULTY_LEVEL_EASY:
        return FALSE;

    case DIFFICULTY_LEVEL_MEDIUM:
        if(Rogue_IsRivalTrainer(trainerNum))
            return TRUE;
        else if(Rogue_IsKeyTrainer(trainerNum))
        {
            if(Rogue_GetCurrentDifficulty() >= ROGUE_ELITE_START_DIFFICULTY - 2)
                return TRUE;
            else
                return FALSE;
        }
        else
        {
            // Misc trainers just have any mons they can
            return FALSE;
        }

    case DIFFICULTY_LEVEL_HARD:
        if(Rogue_IsRivalTrainer(trainerNum))
            return TRUE;
        else if(Rogue_IsKeyTrainer(trainerNum))
        {
            if(Rogue_GetCurrentDifficulty() >= ROGUE_GYM_MID_DIFFICULTY)
                return TRUE;
            else
                return FALSE;
        }
        else
        {
            // Normal trainers start to optimize coverage from E4 onward
            if(Rogue_GetCurrentDifficulty() >= ROGUE_ELITE_START_DIFFICULTY)
                return TRUE;
            else
                return FALSE;
        }

    case DIFFICULTY_LEVEL_BRUTAL:
        return TRUE;
    }

    // Should never get here
    AGB_ASSERT(FALSE);
    return FALSE;
}

static u8 CalculatePartyMonCount(u16 trainerNum, u8 monCapacity, u8 monLevel)
{
    u8 monCount;

    // Hack for EXP trainer
    if(monLevel == 1)
        return 1;

    if(Rogue_IsKeyTrainer(trainerNum))
    {
        if(FlagGet(FLAG_ROGUE_GAUNTLET_MODE))
            monCount = 6;
        else
        {
            switch (Rogue_GetConfigRange(CONFIG_RANGE_TRAINER))
            {
            case DIFFICULTY_LEVEL_EASY:
            case DIFFICULTY_LEVEL_MEDIUM:
                if(Rogue_GetCurrentDifficulty() == 0)
                    monCount = Rogue_IsRivalTrainer(trainerNum) ? 2 : 3;
                else if(Rogue_GetCurrentDifficulty() <= 1)
                    monCount = 3;
                else if(Rogue_GetCurrentDifficulty() <= ROGUE_GYM_MID_DIFFICULTY)
                    monCount = 4;
                else if(Rogue_GetCurrentDifficulty() <= ROGUE_GYM_MID_DIFFICULTY + 2)
                    monCount = 5;
                else
                    monCount = 6;
                break;
            
            case DIFFICULTY_LEVEL_HARD:
                if(Rogue_GetCurrentDifficulty() == 0)
                    monCount = Rogue_IsRivalTrainer(trainerNum) ? 3 : 4;
                else if(Rogue_GetCurrentDifficulty() == 1)
                    monCount = 5;
                else
                    monCount = 6;
                break;
            
            case DIFFICULTY_LEVEL_BRUTAL:
                if(Rogue_GetCurrentDifficulty() == 0)
                    monCount = Rogue_IsRivalTrainer(trainerNum) ? RIVAL_BASE_PARTY_SIZE : 6; // Haven't generate the rest of the party by this point
                else
                    monCount = 6;
                break;
            }
        }
    }
    else
    {
        u8 minMonCount;
        u8 maxMonCount;

        if(Rogue_GetCurrentDifficulty() <= 1)
        {
            minMonCount = 1;
            maxMonCount = 2;
        }
        else if(Rogue_GetCurrentDifficulty() <= 2)
        {
            minMonCount = 1;
            maxMonCount = 3;
        }
        else if(Rogue_GetCurrentDifficulty() <= ROGUE_CHAMP_START_DIFFICULTY - 1)
        {
            minMonCount = 2;
            maxMonCount = 4;
        }
        else
        {
            minMonCount = 3;
            maxMonCount = 4;
        }

        monCount = minMonCount + RogueRandomRange(maxMonCount - minMonCount, FLAG_SET_SEED_TRAINERS);
    }

    //if(trainerPtr->monGenerators->generatorFlags & TRAINER_GENERATOR_FLAG_MIRROR_ANY)
    //{
    //    monCount = gPlayerPartyCount;
    //}

    monCount = min(monCount, monCapacity);
    return monCount;
}

static bool8 ShouldTrainerUseValidNatures(u16 trainerNum)
{
    if(!Rogue_IsKeyTrainer(trainerNum))
        return FALSE;

    switch (Rogue_GetConfigRange(CONFIG_RANGE_TRAINER))
    {
    case DIFFICULTY_LEVEL_EASY:
        return FALSE;

    case DIFFICULTY_LEVEL_MEDIUM:
        if(Rogue_GetCurrentDifficulty() >= ROGUE_FINAL_CHAMP_DIFFICULTY)
            return TRUE;
        return FALSE;

    case DIFFICULTY_LEVEL_HARD:
        if(Rogue_GetCurrentDifficulty() >= ROGUE_ELITE_START_DIFFICULTY)
            return TRUE;
        return FALSE;

    case DIFFICULTY_LEVEL_BRUTAL:
        return TRUE;
    }

    return FALSE;
}

u8 Rogue_CreateTrainerParty(u16 trainerNum, struct Pokemon* party, u8 monCapacity, bool8 firstTrainer)
{
    u8 monCount;
    u32 tempSeed = gRngRogueValue;

    SeedRogueRng(RogueRandom() + trainerNum * RogueRandom()); // todo - move this sooner? Need to sync doubles/singles mixed mode to seed

    if(Rogue_IsRivalTrainer(trainerNum))
        monCount = CreateRivalPartyInternal(trainerNum, party, monCapacity);
    else
        monCount = CreateTrainerPartyInternal(trainerNum, party, 0, monCapacity, firstTrainer, 0);

    // Adjust mons
    {
        // Assign the pokeball based on the trainer
        u8 i;
        u32 pokeballId = Rogue_GetTrainerPokeballId(trainerNum);

        for(i = 0; i < monCount; ++i)
            SetMonData(&party[i], MON_DATA_POKEBALL, &pokeballId);
    }

    ReorderPartyMons(trainerNum, party, monCount);

    // Debug steal team
    if(RogueDebug_GetConfigToggle(DEBUG_TOGGLE_STEAL_TEAM))
    {
        u8 i;
        u16 exp = Rogue_ModifyExperienceTables(1, 100);

        for(i = 0; i < PARTY_SIZE; ++i)
        {
            ZeroMonData(&gPlayerParty[i]);
        }

        gPlayerPartyCount = monCount;

        for(i = 0; i < gPlayerPartyCount; ++i)
        {
            CopyMon(&gPlayerParty[i], &party[i], sizeof(gPlayerParty[i]));

            SetMonData(&gPlayerParty[i], MON_DATA_EXP, &exp);
            CalculateMonStats(&gPlayerParty[i]);
        }
    }

    gRngRogueValue = tempSeed;
    return monCount;
}

static u8 CreateTrainerPartyInternal(u16 trainerNum, struct Pokemon* party, u8 monCount, u8 monCapacity, bool8 firstTrainer, u8 startIndex)
{
    u8 i;
    u8 level;
    u8 fixedIV;
    u16 species;
    struct TrainerPartyScratch scratch;

    level = GetTrainerLevel(trainerNum);
    fixedIV = CalculateMonFixedIV(trainerNum);

    if(monCount == 0)
    {
        monCount = CalculatePartyMonCount(trainerNum, monCapacity, level);
    }

    // Fill defaults before we configure the scratch
    scratch.trainerNum = trainerNum;
    scratch.party = party;
    scratch.partyCapacity = monCapacity;
    scratch.partyCount = startIndex;
    scratch.shouldRegenerateQuery = TRUE;
    scratch.subsetIndex = 0;
    scratch.subsetSampleCount = 0;
    scratch.fallbackCount = 0;
    scratch.forceLegends = FALSE;
    scratch.evoLevel = level;
    scratch.allowItemEvos = FALSE;
    scratch.allowStrongLegends = FALSE;
    scratch.allowWeakLegends = FALSE;
    scratch.preferStrongSpecies = FALSE;

    ConfigurePartyScratchSettings(trainerNum, &scratch);

    // Generate team
    {
        struct RoguePokemonCompetitiveSet preset;
        struct RoguePokemonCompetitiveSetRules presetRules;

        RogueMonQuery_Begin();

        for(i = startIndex; i < monCount; ++i)
        {
            species = SampleNextSpecies(&scratch);

            if(RogueDebug_GetConfigToggle(DEBUG_TOGGLE_TRAINER_LVL_5))
                CreateMon(&party[i], species, 5, fixedIV, FALSE, 0, OT_ID_RANDOM_NO_SHINY, 0);
            else
                CreateMon(&party[i], species, level, fixedIV, FALSE, 0, OT_ID_RANDOM_NO_SHINY, 0);

            if(Rogue_IsValidTrainerShinySpecies(trainerNum, species))
            {
                if(RogueRandomChance(TRAINER_SHINY_PERC, 0))
                {
                    u32 flag = TRUE;
                    SetMonData(&party[i], MON_DATA_IS_SHINY, &flag);
                }
            }

            if(UseCompetitiveMoveset(&scratch, i, monCount) && SelectNextPreset(&scratch, species, i, &preset))
            {
                memset(&presetRules, 0, sizeof(presetRules));
                ModifyTrainerMonPreset(trainerNum, &preset, &presetRules);
                Rogue_ApplyMonCompetitiveSet(&party[i], level, &preset, &presetRules);
            }

            ++scratch.partyCount;
        }

        RogueMonQuery_End();
    }

    return monCount;
}

static u8 SelectEvoChainMon_CalculateWeight(u16 index, u16 species, void* data)
{
    u16* targetSpecies = (u16*)data;
    return Rogue_DoesEvolveInto(species, *targetSpecies) ? 1 : 0;
}

static u8 CreateRivalPartyInternal(u16 trainerNum, struct Pokemon* party, u8 monCapacity)
{
    u8 i;
    u8 level;
    u8 monCount;
    u8 fixedIV;
    u16 species;
    struct TrainerPartyScratch scratch;

    level = GetTrainerLevel(trainerNum);
    fixedIV = CalculateMonFixedIV(trainerNum);
    monCount = CalculatePartyMonCount(trainerNum, monCapacity, level);

    Rogue_GenerateRivalBaseTeamIfNeeded();

    if(monCount > RIVAL_BASE_PARTY_SIZE)
        Rogue_GenerateRivalSwapTeamIfNeeded();

    // Fill defaults before we configure the scratch
    scratch.trainerNum = trainerNum;
    scratch.party = party;
    scratch.partyCapacity = monCapacity;
    scratch.partyCount = 0;
    scratch.shouldRegenerateQuery = TRUE;
    scratch.subsetIndex = 0;
    scratch.subsetSampleCount = 0;
    scratch.fallbackCount = 0;
    scratch.forceLegends = FALSE;
    scratch.evoLevel = level;
    scratch.allowItemEvos = FALSE;
    scratch.allowStrongLegends = FALSE;
    scratch.allowWeakLegends = FALSE;
    scratch.preferStrongSpecies = FALSE;

    ConfigurePartyScratchSettings(trainerNum, &scratch);

    // Generate team
    {
        u8 i, j;
        u8 swapAmount;
        u16 species;
        struct RoguePokemonCompetitiveSet preset;
        struct RoguePokemonCompetitiveSetRules presetRules;
        u16 speciesBuffer[PARTY_SIZE];

        memcpy(speciesBuffer, gRogueRun.rivalSpecies, sizeof(speciesBuffer));

        // Swap out some of the mons at specific point in the run
        swapAmount = 0;

        // Only begin to swap if we're at max party size
        if(monCount == PARTY_SIZE)
        {
            if(Rogue_GetCurrentDifficulty() >= ROGUE_FINAL_CHAMP_DIFFICULTY)
                swapAmount = 3;
            else if(Rogue_GetCurrentDifficulty() >= ROGUE_ELITE_START_DIFFICULTY - 1)
                swapAmount = 2;
            else if(Rogue_GetCurrentDifficulty() >= ROGUE_ELITE_START_DIFFICULTY - 2)
                swapAmount = 1;
        }

        // Remove all lowest scorers first (Mimic selection behaviour performed during generation)
        for(i = 0; i < swapAmount; ++i)
        {
            j = SelectRivalWeakestMon(speciesBuffer, RIVAL_BASE_PARTY_SIZE);
            speciesBuffer[j] = SPECIES_NONE;
        }

        // Now replace
        j = 0;
        for(i = 0; i < PARTY_SIZE; ++i)
        {
            if(speciesBuffer[i] == SPECIES_NONE)
            {
                AGB_ASSERT(PARTY_SIZE + j < ROGUE_RIVAL_TOTAL_MON_COUNT);
                speciesBuffer[i] = gRogueRun.rivalSpecies[PARTY_SIZE + j++];
            }
        }

        for(i = 0; i < monCount; ++i)
        {
            species = speciesBuffer[i];
            AGB_ASSERT(species != SPECIES_NONE);

            // Now we need to use the query to check we're allowed to have evolved by now
            RogueMonQuery_Begin();
            {
                RogueMonQuery_Reset(QUERY_FUNC_EXCLUDE);
                RogueMiscQuery_EditElement(QUERY_FUNC_INCLUDE, Rogue_GetEggSpecies(species));

                RogueMonQuery_TransformIntoEvos(scratch.evoLevel, scratch.allowItemEvos, FALSE);

                if(RogueMiscQuery_CheckState(species))
                {
                    // The species is allowed to exist by this point, so just continue on
                }
                else
                {
                    // We haven't evolved yet, so we need to take a pre-evo
                    RogueWeightQuery_Begin();
                    RogueWeightQuery_FillWeights(1);

                    if(RogueWeightQuery_HasMultipleWeights())
                    {
                        // Ensure we're only picking the mon which evolves into the correct species
                        // e.g. Handle Silcoon/Cascoon
                        RogueWeightQuery_CalculateWeights(SelectEvoChainMon_CalculateWeight, &species);
                    }

                    AGB_ASSERT(RogueWeightQuery_HasAnyWeights());
                    AGB_ASSERT(!RogueWeightQuery_HasMultipleWeights());
                    if(RogueWeightQuery_HasAnyWeights())
                    {
                        species = RogueWeightQuery_SelectRandomFromWeights(0);
                    }

                    RogueWeightQuery_End();
                }
            }
            RogueMonQuery_End();


            if(RogueDebug_GetConfigToggle(DEBUG_TOGGLE_TRAINER_LVL_5))
                CreateMon(&party[i], species, 5, fixedIV, FALSE, 0, OT_ID_RANDOM_NO_SHINY, 0);
            else
                CreateMon(&party[i], species, level, fixedIV, FALSE, 0, OT_ID_RANDOM_NO_SHINY, 0);

            if(Rogue_IsValidTrainerShinySpecies(trainerNum, species))
            {
                if(gRogueRun.rivalHasShiny)
                {
                    u32 flag = TRUE;
                    SetMonData(&party[i], MON_DATA_IS_SHINY, &flag);
                }
            }

            if(UseCompetitiveMoveset(&scratch, i, monCount) && SelectNextPreset(&scratch, species, i, &preset))
            {
                memset(&presetRules, 0, sizeof(presetRules));
                ModifyTrainerMonPreset(trainerNum, &preset, &presetRules);
                Rogue_ApplyMonCompetitiveSet(&party[i], level, &preset, &presetRules);
            }
        }
    }

    return monCount;
}

static u16 GetSimilarCheckSpecies(u16 species)
{
#ifdef ROGUE_EXPANSION
    u16 baseSpecies = GET_BASE_SPECIES_ID(species);

    switch (baseSpecies)
    {
    case SPECIES_DEOXYS:
    case SPECIES_BURMY:
    case SPECIES_WORMADAM:
    case SPECIES_SHELLOS:
    case SPECIES_GASTRODON:
    case SPECIES_ROTOM:
    case SPECIES_DIALGA:
    case SPECIES_PALKIA:
    case SPECIES_GIRATINA:
    case SPECIES_SHAYMIN:
    case SPECIES_ARCEUS:
    case SPECIES_BASCULIN:
    case SPECIES_DARMANITAN:
    case SPECIES_DARMANITAN_GALARIAN:
    case SPECIES_DEERLING:
    case SPECIES_SAWSBUCK:
    case SPECIES_TORNADUS:
    case SPECIES_THUNDURUS:
    case SPECIES_LANDORUS:
    case SPECIES_KYUREM:
    case SPECIES_KELDEO:
    case SPECIES_MELOETTA:
    case SPECIES_GENESECT:
    case SPECIES_VIVILLON:
    case SPECIES_FLABEBE:
    case SPECIES_FLOETTE:
    case SPECIES_FLORGES:
    case SPECIES_FURFROU:
    case SPECIES_MEOWSTIC:
    case SPECIES_HOOPA:
    case SPECIES_ORICORIO:
    case SPECIES_LYCANROC:
    case SPECIES_SILVALLY:
    case SPECIES_NECROZMA:
    case SPECIES_TOXTRICITY:
    case SPECIES_SINISTEA:
    case SPECIES_POLTEAGEIST:
    case SPECIES_ALCREMIE:
    case SPECIES_INDEEDEE:
    case SPECIES_ZACIAN:
    case SPECIES_ZAMAZENTA:
    case SPECIES_URSHIFU:
    case SPECIES_CALYREX:
        return baseSpecies;
    }
#endif

    return species;
}

bool8 PartyContainsBaseSpecies(struct Pokemon *party, u8 partyCount, u16 species)
{
    u8 i;
    u16 s;

#ifdef ROGUE_EXPANSION
    species = GET_BASE_SPECIES_ID(species);
#endif

    for(i = 0; i < partyCount; ++i)
    {
        s = GetMonData(&party[i], MON_DATA_SPECIES);
#ifdef ROGUE_EXPANSION
        s = GET_BASE_SPECIES_ID(s);
#endif

        if(s == species)
            return TRUE;
    }

    return FALSE;
}

bool8 PartyContainsSimilarSpecies(struct TrainerPartyScratch* scratch, u16 species)
{
    u8 i;
    u16 s;

    species = GetSimilarCheckSpecies(species);

    // For the rival we also want to check any past species (We still have to check the party as we may not have updated this buffer during the selection)
    if(gRogueRun.rivalTrainerNum == scratch->trainerNum)
    {
        for(i = 0; i < ROGUE_RIVAL_TOTAL_MON_COUNT; ++i)
        {
            if(gRogueRun.rivalSpecies[i] == SPECIES_NONE)
                continue;

            s = GetSimilarCheckSpecies(gRogueRun.rivalSpecies[i]);

            if(s == species)
                return TRUE;
        }
    }

    for(i = 0; i < scratch->partyCount; ++i)
    {
        s = GetSimilarCheckSpecies(GetMonData(&scratch->party[i], MON_DATA_SPECIES));

        if(s == species)
            return TRUE;
    }

    return FALSE;
}

static bool8 FilterOutDuplicateMons(u16 elem, void* usrData)
{
    struct TrainerPartyScratch* scratch = (struct TrainerPartyScratch*)usrData;
    return !PartyContainsSimilarSpecies(scratch, elem);
}

static void SetupQueryScriptVars(struct QueryScriptContext* context, struct TrainerPartyScratch* scratch)
{
    if(ShouldTrainerOptimizeCoverage(scratch->trainerNum))
    {
        RogueQueryScript_SetupVarsForParty(context, scratch->party, scratch->partyCount);
    }
    else
    {
        RogueQueryScript_SetupVarsForParty(context, NULL, 0);
    }
}

static u8 SelectFallbackTypeFor(u8 type, u8 counter)
{
    switch(type)
    {
        case TYPE_DARK:
            switch (counter % 2)
            {
            case 0:
                return TYPE_FIGHTING;
            
            case 1:
                return TYPE_PSYCHIC;
            }
            break;

        case TYPE_PSYCHIC:
            switch (counter % 2)
            {
            case 0:
                return TYPE_GHOST;
            
            case 1:
                return TYPE_DARK;
            }
            break;

        case TYPE_STEEL:
            switch (counter % 2)
            {
            case 0:
                return TYPE_GROUND;
            
            case 1:
                return TYPE_DRAGON;
            }
            break;

        case TYPE_FIGHTING:
            switch (counter % 2)
            {
            case 0:
                return TYPE_ROCK;
            
            case 1:
                return TYPE_NORMAL;
            }
            break;

        case TYPE_GHOST:
            switch (counter % 2)
            {
            case 0:
                return TYPE_POISON;
            
            case 1:
                return TYPE_BUG;
            }
            break;

        case TYPE_DRAGON:
            switch (counter % 2)
            {
            case 0:
                return TYPE_FIRE;
            
            case 1:
                return TYPE_WATER;
            }
            break;

        case TYPE_FIRE:
            switch (counter % 2)
            {
            case 0:
                return TYPE_GROUND;
            
            case 1:
                return TYPE_ROCK;
            }
            break;

        case TYPE_FLYING:
            switch (counter % 2)
            {
            case 0:
                return TYPE_NORMAL;
            
            case 1:
                return TYPE_ELECTRIC;
            }
            break;

        case TYPE_ICE:
            switch (counter % 2)
            {
            case 0:
                return TYPE_WATER;
            
            case 1:
                return TYPE_PSYCHIC;
            }
            break;

        case TYPE_NORMAL:
            switch (counter % 2)
            {
            case 0:
                return TYPE_FIGHTING;
            
            case 1:
                return TYPE_GHOST;
            }
            break;
    }

    return TYPE_NONE;
}

static u32 CalculateFallbackTypeFlags(struct TrainerPartyScratch* scratch)
{
    struct RogueTrainer const* trainer = &gRogueTrainers[scratch->trainerNum];
    u8 currentType = trainer->typeAssignment;

    // If we have a mystery type we want to just pick 1 type
    if(currentType == TYPE_MYSTERY)
    {
        while(currentType == TYPE_MYSTERY)
        {
            currentType = RogueRandom() % NUMBER_OF_MON_TYPES;
        }

        return MON_TYPE_VAL_TO_FLAGS(currentType);
    }

    if(scratch->fallbackCount < 20)
    {
        u8 i;

        // Predictably fallback to the next nearest type
        for(i = 0; i < scratch->fallbackCount; ++i)
            currentType = SelectFallbackTypeFor(currentType, scratch->trainerNum + i);
    }
    else
    {
        // If we've gotten this far, yikes!
        AGB_ASSERT(FALSE);
        currentType = TYPE_NONE;
    }

    // Allow everything
    if(currentType == TYPE_NONE)
        return MON_TYPE_VAL_TO_FLAGS(NUMBER_OF_MON_TYPES) - 1;

    // Only allow current type
    return MON_TYPE_VAL_TO_FLAGS(currentType);
}

static bool8 CanEntirelyAvoidWeakSpecies()
{
    return RoguePokedex_GetCurrentDexLimit() >= 380;
}

static u16 SampleNextSpeciesInternal(struct TrainerPartyScratch* scratch)
{
    u16 species;
    struct RogueTrainer const* trainer = &gRogueTrainers[scratch->trainerNum];

    if(scratch->shouldRegenerateQuery)
    {
        u32 strongFlags;
        u32 fallbackTypeFlags = 0;
        bool8 customScript = FALSE;
        struct RogueTeamGeneratorSubset const* currentSubset = NULL;

        scratch->shouldRegenerateQuery = FALSE;

        if(scratch->subsetIndex < trainer->teamGenerator.subsetCount)
        {
            currentSubset = &trainer->teamGenerator.subsets[scratch->subsetIndex];
        }

        // Execute initialisation
        if(trainer->teamGenerator.queryScriptOverride != NULL)
        {
            struct QueryScriptContext scriptContext;

            // Start with empty, expect override script to set valid species
            RogueMonQuery_Reset(QUERY_FUNC_EXCLUDE);

            RogueQueryScript_SetupScript(&scriptContext, trainer->teamGenerator.queryScriptOverride);
            SetupQueryScriptVars(&scriptContext, scratch);
            RogueQueryScript_Execute(&scriptContext);
            customScript = TRUE;
        }
        else
        {
            RogueMonQuery_IsSpeciesActive();
        }

        // Rival won't have the same legends as us (Maybe this should extend to the entire run?)
        if(Rogue_IsRivalTrainer(scratch->trainerNum))
        {
            u8 i;

            for(i = 0; i < ARRAY_COUNT(gRogueRun.legendarySpecies); ++i)
            {
                if(gRogueRun.legendarySpecies[i] != SPECIES_NONE)
                    RogueMiscQuery_EditElement(QUERY_FUNC_EXCLUDE, gRogueRun.legendarySpecies[i]);
            }
        }

        if(currentSubset != NULL)
        {
            RogueMonQuery_EvosContainType(QUERY_FUNC_INCLUDE, currentSubset->includedTypeMask);
        }
        else
        {
            fallbackTypeFlags = CalculateFallbackTypeFlags(scratch);
            RogueMonQuery_EvosContainType(QUERY_FUNC_INCLUDE, fallbackTypeFlags);
        }

        // Transform and evolve mons to valid evos (Don't do this for custom scripts for now, as our only use case is glitch mode)
        if(!customScript)
        {
            RogueMonQuery_TransformIntoEggSpecies();
            RogueMonQuery_TransformIntoEvos(scratch->evoLevel, scratch->allowItemEvos, FALSE);
        }

        if(scratch->preferStrongSpecies && CanEntirelyAvoidWeakSpecies())
        {
            RogueMonQuery_ContainsPresetFlags(QUERY_FUNC_INCLUDE, MON_FLAG_SINGLES_STRONG);
        }

        if(scratch->forceLegends)
        {
            RogueMonQuery_IsLegendary(QUERY_FUNC_INCLUDE);
        }

        // Specific legendary filter
        {
            // Not allowed any legendary
            if(!scratch->forceLegends && !scratch->allowWeakLegends && !scratch->allowStrongLegends)
                RogueMonQuery_IsLegendary(QUERY_FUNC_EXCLUDE);

            // Only allowed strong legends
            else if(!scratch->allowWeakLegends && scratch->allowStrongLegends)
                RogueMonQuery_IsLegendaryWithPresetFlags(QUERY_FUNC_INCLUDE, MON_FLAG_SINGLES_STRONG);

            // Only allowed weak legends
            else if(scratch->allowWeakLegends && !scratch->allowStrongLegends)
                RogueMonQuery_IsLegendaryWithPresetFlags(QUERY_FUNC_EXCLUDE, MON_FLAG_SINGLES_STRONG);
        }


        if(currentSubset != NULL)
        {
            RogueMonQuery_IsOfType(QUERY_FUNC_INCLUDE, currentSubset->includedTypeMask);
            RogueMonQuery_IsOfType(QUERY_FUNC_EXCLUDE, currentSubset->excludedTypeMask);
        }
        else
        {
            RogueMonQuery_IsOfType(QUERY_FUNC_INCLUDE, fallbackTypeFlags);
        }

        // Execute post process script
        if(trainer->teamGenerator.queryScriptPost != NULL)
        {
            struct QueryScriptContext scriptContext;
            RogueQueryScript_SetupScript(&scriptContext, trainer->teamGenerator.queryScriptPost);
            SetupQueryScriptVars(&scriptContext, scratch);
            RogueQueryScript_Execute(&scriptContext);
        }
    }

    // Allow duplicates if we've gone far into fallbacks
    if(scratch->fallbackCount < 10)
    {
        // Remove any mons already in the party
        RogueMonQuery_CustomFilter(FilterOutDuplicateMons, scratch);
    }

    species = SPECIES_NONE;

    RogueWeightQuery_Begin();

    if(trainer->teamGenerator.weightScript != NULL)
    {
        struct QueryScriptContext scriptContext;
        RogueQueryScript_SetupScript(&scriptContext, trainer->teamGenerator.weightScript);
        SetupQueryScriptVars(&scriptContext, scratch);
        RogueWeightQuery_CalculateWeights(RogueQueryScript_CalculateWeightsCallback, &scriptContext);
    }
    else
    {
        RogueWeightQuery_FillWeights(1);
    }
    
    //if(scratch->preferStrongSpecies && !CanEntirelyAvoidWeakSpecies())
    //{
    //    // TODO - Should we prefer strong presets in weight query?
    //}

    if(RogueWeightQuery_HasAnyWeights())
    {
        species = RogueWeightQuery_SelectRandomFromWeights(RogueRandom());
    }

    RogueWeightQuery_End();

    return species;
}

static u16 SampleNextSpecies(struct TrainerPartyScratch* scratch)
{
    u16 species;
    struct RogueTrainer const* trainer = &gRogueTrainers[scratch->trainerNum];

    // We don't have any subsets, so immediately start using fallback behaviour
    if(trainer->teamGenerator.subsetCount == 0)
    {
        ++scratch->fallbackCount;
    }

    do
    {
        // If we have valid subsets remaining and we're a boss, force the final mons to be legends
        if(scratch->subsetIndex < trainer->teamGenerator.subsetCount && (Rogue_IsBossTrainer(scratch->trainerNum) || Rogue_IsRivalTrainer(scratch->trainerNum)))
        {
            if(Rogue_GetCurrentDifficulty() == ROGUE_MAX_BOSS_COUNT - 1 && scratch->partyCount == 4)
            {
                scratch->forceLegends = TRUE;
                scratch->shouldRegenerateQuery = TRUE;
            }
            else if(Rogue_GetCurrentDifficulty() == ROGUE_MAX_BOSS_COUNT - 2 && scratch->partyCount == 5)
            {
                scratch->forceLegends = TRUE;
                scratch->shouldRegenerateQuery = TRUE;
            }
        }
        else
        {
            scratch->forceLegends = FALSE;
        }

        species = SampleNextSpeciesInternal(scratch);

        if(species == SPECIES_NONE)
        {
            // Just put it to some really high number if we failed, as we need to move to the next subset
            scratch->subsetSampleCount = 128;
        }

        if(scratch->subsetIndex < trainer->teamGenerator.subsetCount)
        {
            ++scratch->subsetSampleCount;
            if(scratch->subsetSampleCount >= trainer->teamGenerator.subsets[scratch->subsetIndex].maxSamples)
            {
                ++scratch->subsetIndex;
                scratch->subsetSampleCount = 0;
                scratch->shouldRegenerateQuery = TRUE;

                if(scratch->subsetIndex >= trainer->teamGenerator.subsetCount)
                {
                    ++scratch->fallbackCount;
                }
            }
        }
        else
        {
            if(scratch->fallbackCount == 255)
            {
                // If we've got here, we must've ran out in options in the fallback/all type subset
                AGB_ASSERT(FALSE);
                return SPECIES_MAGIKARP;
            }
            else if(scratch->fallbackCount != 0)
            {
                ++scratch->fallbackCount;
                scratch->shouldRegenerateQuery = TRUE;
            }
        }
    }
    while(species == SPECIES_NONE);

    return species;
}

static bool8 UseCompetitiveMoveset(struct TrainerPartyScratch* scratch, u8 monIdx, u8 totalMonCount)
{
    bool8 preferCompetitive = FALSE;
    bool8 result = FALSE;
    u8 difficultyLevel = Rogue_GetCurrentDifficulty();
    u8 difficultyModifier = Rogue_GetEncounterDifficultyModifier();

    //if(sTrainerScratch->monGenerator.generatorFlags & TRAINER_GENERATOR_FLAG_MIRROR_EXACT)
    //{
    //    // Exact mirror force competitive set and we'll override it later
    //    return TRUE;
    //}

    if(gRogueAdvPath.currentRoomType == ADVPATH_ROOM_LEGENDARY || difficultyModifier == ADVPATH_SUBROOM_ROUTE_TOUGH)
    {
        // For regular trainers, Last and first mon can have competitive sets
        preferCompetitive = (monIdx == 0 || monIdx == (totalMonCount - 1));
    }

#ifdef ROGUE_FEATURE_AUTOMATION
    if(Rogue_AutomationGetFlag(AUTO_FLAG_TRAINER_FORCE_COMP_MOVESETS))
    {
        return TRUE;
    }
#endif

    if(FlagGet(FLAG_ROGUE_GAUNTLET_MODE))
    {
        return Rogue_IsAnyBossTrainer(scratch->trainerNum);
    }

    switch (Rogue_GetConfigRange(CONFIG_RANGE_TRAINER))
    {
    case DIFFICULTY_LEVEL_EASY:
        return FALSE;

    case DIFFICULTY_LEVEL_MEDIUM:
        // Start using competitive movesets on 3rd gym
        if(difficultyLevel == 0) // Last mon has competitive set
            return FALSE;
        else if(difficultyLevel == 1)
            return (preferCompetitive || Rogue_IsAnyBossTrainer(scratch->trainerNum)) && monIdx == (totalMonCount - 1);
        else
            return (preferCompetitive || Rogue_IsAnyBossTrainer(scratch->trainerNum));

    case DIFFICULTY_LEVEL_HARD:
        if(difficultyLevel == 0) // Last mon has competitive set
            return (preferCompetitive || Rogue_IsAnyBossTrainer(scratch->trainerNum)) && monIdx == (totalMonCount - 1);
        else if(difficultyLevel == 1)
            return (preferCompetitive || Rogue_IsAnyBossTrainer(scratch->trainerNum));
        else
            return TRUE;

    case DIFFICULTY_LEVEL_BRUTAL:
        return TRUE;
    }

    return FALSE;
}

static bool8 HasDamagingMove(struct RoguePokemonCompetitiveSet const* preset)
{
    u8 i;
    u16 move;

    for(i = 0; i <MAX_MON_MOVES; ++i)
    {
        move = preset->moves[i];
        if(move != MOVE_NONE && gBattleMoves[move].power != 0)
            return TRUE;
    }

    return FALSE;
}

static bool8 SelectNextPreset(struct TrainerPartyScratch* scratch, u16 species, u8 monIdx, struct RoguePokemonCompetitiveSet* outPreset)
{
    u8 i;
    u16 presetCount = gRoguePokemonProfiles[species].competitiveSetCount;

    // Exact mirror copy trainer party
    //if(sTrainerScratch->monGenerator.generatorFlags & TRAINER_GENERATOR_FLAG_MIRROR_EXACT)
    //{
    //    outPreset->allowMissingMoves = TRUE;
    //    outPreset->heldItem = GetMonData(&gPlayerParty[monIdx], MON_DATA_HELD_ITEM);
    //    outPreset->abilityNum = GetMonAbility(&gPlayerParty[monIdx]);
    //    outPreset->hiddenPowerType = CalcMonHiddenPowerType(&gPlayerParty[monIdx]);
    //    outPreset->flags = 0;
//1
    //    for(i = 0; i < MAX_MON_MOVES; ++i)
    //        outPreset->moves[i] = GetMonData(&gPlayerParty[monIdx], MON_DATA_MOVE1 + i);
//
    //    return TRUE;
    //}

    if(presetCount != 0)
    {
        {
            u16 currentScore;
            u16 bestScore = 0; // higher is better
            const struct RoguePokemonCompetitiveSet* currPreset = NULL;
            const struct RoguePokemonCompetitiveSet* bestPreset = NULL;
            u8 randOffset = (presetCount == 1 ? 0 : RogueRandomRange(presetCount, FLAG_SET_SEED_TRAINERS));
            
            // Work from random offset and attempt to find the best preset which slots into this team
            // If none is found, we will use the last option and adjust below
            for(i = 0; i < presetCount; ++i)
            {
                currPreset = &gRoguePokemonProfiles[species].competitiveSets[((randOffset + i) % presetCount)];
                currentScore = 1024;

                // Avoid presets which don't have any damaging moves (e.g. Giratina)
                if(!HasDamagingMove(currPreset))
                {
                    currentScore /= 2;
                }

                // Slightly prefer sets which are intended for this format
                if(gBattleTypeFlags & BATTLE_TYPE_DOUBLE)
                {
                    if((currPreset->flags & MON_FLAG_DOUBLES_STRONG) != 0)
                    {
                        currentScore += 32;
                    }
                }
                else
                {
                    if((currPreset->flags & MON_FLAG_SINGLES_STRONG) != 0)
                    {
                        currentScore += 32;
                    }
                }

                // Avoid duplicate items (If this preset is used, we'll just replace the item)
                //
                if(currPreset->heldItem == ITEM_LEFTOVERS && scratch->heldItems.hasLeftovers)
                {
                    currentScore /= 2;
                }

                if(currPreset->heldItem == ITEM_SHELL_BELL && scratch->heldItems.hasShellbell)
                {
                    currentScore /= 2;
                }

                if(IsChoiceItem(currPreset->heldItem) && scratch->heldItems.hasChoiceItem)
                {
                    currentScore /= 2;
                }

#ifdef ROGUE_EXPANSION
                // Special case for primal reversion
                if(!IsMegaEvolutionEnabled())
                {
                    if(currPreset->heldItem == ITEM_RED_ORB || currPreset->heldItem == ITEM_BLUE_ORB)
                    {
                        currentScore /= 4;
                    }
                }

                if(scratch->heldItems.hasMegaStone || !IsMegaEvolutionEnabled())
                {
                    if(currPreset->heldItem >= ITEM_VENUSAURITE && currPreset->heldItem <= ITEM_DIANCITE)
                    {
                        currentScore /= 4;
                    }
                }

                if(scratch->heldItems.hasZCrystal || !IsZMovesEnabled())
                {
                    if(currPreset->heldItem >= ITEM_NORMALIUM_Z && currPreset->heldItem <= ITEM_ULTRANECROZIUM_Z)
                    {
                        currentScore /= 4;
                    }
                }
#endif
                // Handle identical scores by adding on some random amount
                // so we will essentially randomlly choose between the best sets and get more variety
                currentScore += RogueRandom() % 64;

                if(bestPreset == NULL)
                {
                    // This is first option
                    bestScore = currentScore;
                    bestPreset = currPreset;
                }
                else if(currentScore > bestScore)
                {
                    bestScore = currentScore;
                    bestPreset = currPreset;
                }
            }

            if(bestPreset != NULL)
            {
                memcpy(outPreset, bestPreset, sizeof(struct RoguePokemonCompetitiveSet));
            }
            else
            {
                return FALSE;
            }
        }

        // Swap out limited count items, if they already exist
        //
        if(outPreset->heldItem == ITEM_LEFTOVERS && scratch->heldItems.hasLeftovers)
        {
            // Swap left overs to shell bell
            outPreset->heldItem = ITEM_SHELL_BELL;
        }

        if(outPreset->heldItem == ITEM_SHELL_BELL && scratch->heldItems.hasShellbell)
        {
            // Swap shell bell to NONE (i.e. berry)
            outPreset->heldItem = ITEM_NONE;
        }
        
        if(IsChoiceItem(outPreset->heldItem) && scratch->heldItems.hasChoiceItem)
        {
            // Swap choice items for weaker versions
            switch (outPreset->heldItem)
            {
#ifdef ROGUE_EXPANSION
            case ITEM_CHOICE_BAND:
                outPreset->heldItem = ITEM_MUSCLE_BAND;
                break;
            
            case ITEM_CHOICE_SPECS:
                outPreset->heldItem = ITEM_WISE_GLASSES;
                break;

            case ITEM_CHOICE_SCARF:
                outPreset->heldItem = ITEM_QUICK_CLAW;
                break;
#endif

            default:
                outPreset->heldItem = ITEM_NONE;
                break;
            }
        }

#ifdef ROGUE_EXPANSION
        if(!IsMegaEvolutionEnabled())
        {
            // Special case for primal reversion
            if(outPreset->heldItem == ITEM_RED_ORB || outPreset->heldItem == ITEM_BLUE_ORB)
            {
                outPreset->heldItem = ITEM_NONE;
            }
        }

        if(scratch->heldItems.hasMegaStone || !IsMegaEvolutionEnabled())
        {
            if(outPreset->heldItem >= ITEM_VENUSAURITE && outPreset->heldItem <= ITEM_DIANCITE)
            {
                outPreset->heldItem = ITEM_NONE;
            }
        }

        if(scratch->heldItems.hasZCrystal || !IsZMovesEnabled())
        {
            if(outPreset->heldItem >= ITEM_NORMALIUM_Z && outPreset->heldItem <= ITEM_ULTRANECROZIUM_Z)
            {
                outPreset->heldItem = ITEM_NONE;
            }
        }
#endif

        // Give an item if we're missing one
        //
        if(outPreset->heldItem == ITEM_NONE)
        {
            // Swap empty item to a berry either lum or sitrus
            outPreset->heldItem = RogueRandomRange(2, FLAG_SET_SEED_TRAINERS) == 0 ? ITEM_LUM_BERRY : ITEM_SITRUS_BERRY;
        }
        else if(outPreset->heldItem == ITEM_LEFTOVERS)
        {
            scratch->heldItems.hasLeftovers = TRUE;
        }
        else if(outPreset->heldItem == ITEM_SHELL_BELL)
        {
            scratch->heldItems.hasShellbell = TRUE;
        }
        else if(IsChoiceItem(outPreset->heldItem))
        {
            scratch->heldItems.hasChoiceItem = TRUE;
        }
#ifdef ROGUE_EXPANSION
        else if(outPreset->heldItem >= ITEM_VENUSAURITE && outPreset->heldItem <= ITEM_DIANCITE)
        {
            scratch->heldItems.hasMegaStone = TRUE;
        }
        else if(outPreset->heldItem >= ITEM_NORMALIUM_Z && outPreset->heldItem <= ITEM_ULTRANECROZIUM_Z)
        {
            scratch->heldItems.hasZCrystal = TRUE;
        }
#endif

        return TRUE;
    }

    return FALSE;
}


static bool8 IsChoiceItem(u16 itemId)
{
    switch (itemId)
    {
    case ITEM_CHOICE_BAND:
#ifdef ROGUE_EXPANSION
    case ITEM_CHOICE_SPECS:
    case ITEM_CHOICE_SCARF:
#endif
        return TRUE;
    }

    return FALSE;
}

static u8 MonPresetCountMoves(struct RoguePokemonCompetitiveSet* preset)
{
    u8 i;
    u8 count = 0;

    for(i = 0; i < MAX_MON_MOVES; ++i)
    {
        if(preset->moves[i] != MOVE_NONE)
            ++count;
    }

    return count;
}

static bool8 MonPresetReplaceMove(struct RoguePokemonCompetitiveSet* preset, u16 fromMove, u16 toMove)
{
    u8 i;

    for(i = 0; i < MAX_MON_MOVES; ++i)
    {
        if(preset->moves[i] == fromMove)
            preset->moves[i] == toMove;
    }
}

static void ModifyTrainerMonPreset(u16 trainerNum, struct RoguePokemonCompetitiveSet* preset, struct RoguePokemonCompetitiveSetRules* presetRules)
{
#ifndef ROGUE_EXPANSION
    // Vanilla only: AI can't use trick
    if(MonPresetReplaceMove(preset, MOVE_TRICK, MOVE_NONE))
        presetRules->allowMissingMoves = TRUE;
#endif

    // Edge case to handle scarfed ditto
    if(IsChoiceItem(preset->heldItem) && (MonPresetCountMoves(preset) > 2))
    {
        // Need to make sure this mon only has attacking moves
        u8 i = 0;
        presetRules->allowMissingMoves = TRUE;

        for(i = 0; i < MAX_MON_MOVES; ++i)
        {
            if(gBattleMoves[preset->moves[i]].power == 0)
                preset->moves[i] = MOVE_NONE;
        }
    }

    if(!ShouldTrainerUseValidNatures(trainerNum))
        presetRules->skipNature = TRUE;
}

static void SwapMons(u8 aIdx, u8 bIdx, struct Pokemon *party)
{
    if(aIdx != bIdx)
    {
        struct Pokemon tempMon;
        CopyMon(&tempMon, &party[aIdx], sizeof(struct Pokemon));

        CopyMon(&party[aIdx], &party[bIdx], sizeof(struct Pokemon));
        CopyMon(&party[bIdx], &tempMon, sizeof(struct Pokemon));
    }
}

// + go to the front - go to the back
s16 CalulcateMonSortScore(struct Pokemon* mon)
{
    s16 score = 0;
    u16 species = GetMonData(mon, MON_DATA_SPECIES);
    u16 item = GetMonData(mon, MON_DATA_HELD_ITEM);

#ifdef ROGUE_EXPANSION
    if(((item >= ITEM_VENUSAURITE && item <= ITEM_DIANCITE) || (item >= ITEM_NORMALIUM_Z && item <= ITEM_ULTRANECROZIUM_Z)))
    {
        score -= 20;
    }
#endif

    if(RoguePokedex_IsSpeciesLegendary(species))
    {
        score -= 20;
    }

    if(RoguePokedex_GetSpeciesBST(species) >= 540)
    {
        // Put high BST mons in the back of the party
        score -= 10;
    }

    // Early pri moves
    //
    if(MonKnowsMove(mon, MOVE_FAKE_OUT))
    {
        score += 1;
    }
    if(MonKnowsMove(mon, MOVE_LIGHT_SCREEN))
    {
        score += 1;
    }
    if(MonKnowsMove(mon, MOVE_REFLECT))
    {
        score += 1;
    }
    if(MonKnowsMove(mon, MOVE_SPIKES))
    {
        score += 1;
    }

    if(MonKnowsMove(mon, MOVE_TAUNT))
    {
        score += 1;
    }

    if(MonKnowsMove(mon, MOVE_TRICK))
    {
        score += 1;
    }

    if(MonKnowsMove(mon, MOVE_TOXIC))
    {
        score += 1;
    }

    if(MonKnowsMove(mon, MOVE_BATON_PASS))
    {
        score += 1;

        // Only prioritse sub if we want to baton pass out
        if(MonKnowsMove(mon, MOVE_SUBSTITUTE))
        {
            score += 1;
        }
    }

#ifdef ROGUE_EXPANSION
    if(MonKnowsMove(mon, MOVE_U_TURN))
    {
        score += 1;
    }

    if(MonKnowsMove(mon, MOVE_FLIP_TURN))
    {
        score += 1;
    }

    if(MonKnowsMove(mon, MOVE_PARTING_SHOT))
    {
        score += 1;
    }

    if(MonKnowsMove(mon, MOVE_VOLT_SWITCH))
    {
        score += 1;
    }

    if(MonKnowsMove(mon, MOVE_TOXIC_SPIKES))
    {
        score += 1;
    }

    if(MonKnowsMove(mon, MOVE_STEALTH_ROCK))
    {
        score += 1;
    }

    if(MonKnowsMove(mon, MOVE_STICKY_WEB))
    {
        score += 1;
    }

    if(MonKnowsMove(mon, MOVE_TRICK_ROOM))
    {
        score += 1;
    }
#endif

    return score;
}

static void ReorderPartyMons(u16 trainerNum, struct Pokemon *party, u8 monCount)
{
    bool8 keepExistingLead = FALSE;
    bool8 reorganiseParty = FALSE;
    bool8 clampLeadScore = FALSE;

    if(Rogue_IsAnyBossTrainer(trainerNum))
    {
        if(!FlagGet(FLAG_ROGUE_GAUNTLET_MODE) && Rogue_GetConfigRange(CONFIG_RANGE_TRAINER) < DIFFICULTY_LEVEL_HARD && Rogue_GetCurrentDifficulty() < 8)
        {
            // Prior to E4 we don't want to force forward the best lead mon
            // We just want to push final mons to the back
            clampLeadScore = TRUE;
        }

        reorganiseParty = TRUE;
    }
    else
    {
        // Basic trainers don't care and can do whatever with their team order
        reorganiseParty = FALSE;
    }

    if(reorganiseParty)
    {
        u16 i;
        s16 scoreA, scoreB;
        bool8 anySwaps;
        u16 startIndex = keepExistingLead ? 1 : 0;
        u16 sortLength = monCount - 1;

        // Bubble sort party
        while(sortLength != 0)
        {
            anySwaps = FALSE;

            for(i = startIndex; i < monCount - 1; ++i)
            {
                scoreA = CalulcateMonSortScore(&party[i]);
                scoreB = CalulcateMonSortScore(&party[i + 1]);

                if(clampLeadScore)
                {
                    scoreA = min(scoreA, 0);
                    scoreB = min(scoreB, 0);
                }
                
                if(scoreB > scoreA)
                {
                    anySwaps = TRUE;
                    SwapMons(i, i + 1, party);
                }
            }
        
            if(anySwaps)
                --sortLength;
            else
                sortLength = 0;
        }
    }
}