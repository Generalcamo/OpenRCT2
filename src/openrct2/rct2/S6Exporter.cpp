/*****************************************************************************
 * Copyright (c) 2014-2019 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "S6Exporter.h"

#include "../Context.h"
#include "../Game.h"
#include "../OpenRCT2.h"
#include "../common.h"
#include "../config/Config.h"
#include "../core/FileStream.hpp"
#include "../core/IStream.hpp"
#include "../core/String.hpp"
#include "../interface/Viewport.h"
#include "../interface/Window.h"
#include "../localisation/Date.h"
#include "../localisation/Localisation.h"
#include "../management/Award.h"
#include "../management/Finance.h"
#include "../management/Marketing.h"
#include "../management/NewsItem.h"
#include "../management/Research.h"
#include "../object/Object.h"
#include "../object/ObjectLimits.h"
#include "../object/ObjectManager.h"
#include "../object/ObjectRepository.h"
#include "../peep/Staff.h"
#include "../rct12/SawyerChunkWriter.h"
#include "../ride/Ride.h"
#include "../ride/RideRatings.h"
#include "../ride/ShopItem.h"
#include "../ride/Station.h"
#include "../ride/TrackData.h"
#include "../scenario/Scenario.h"
#include "../util/SawyerCoding.h"
#include "../util/Util.h"
#include "../world/Climate.h"
#include "../world/MapAnimation.h"
#include "../world/Park.h"
#include "../world/Sprite.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <iterator>

S6Exporter::S6Exporter()
{
    RemoveTracklessRides = false;
    std::memset(&_s6, 0x00, sizeof(_s6));
}

void S6Exporter::SaveGame(const utf8* path)
{
    auto fs = FileStream(path, FILE_MODE_WRITE);
    SaveGame(&fs);
}

void S6Exporter::SaveGame(IStream* stream)
{
    Save(stream, false);
}

void S6Exporter::SaveScenario(const utf8* path)
{
    auto fs = FileStream(path, FILE_MODE_WRITE);
    SaveScenario(&fs);
}

void S6Exporter::SaveScenario(IStream* stream)
{
    Save(stream, true);
}

void S6Exporter::Save(IStream* stream, bool isScenario)
{
    _s6.header.type = isScenario ? S6_TYPE_SCENARIO : S6_TYPE_SAVEDGAME;
    _s6.header.classic_flag = 0;
    _s6.header.num_packed_objects = uint16_t(ExportObjectsList.size());
    _s6.header.version = S6_RCT2_VERSION;
    _s6.header.magic_number = S6_MAGIC_NUMBER;
    _s6.game_version_number = 201028;

    auto chunkWriter = SawyerChunkWriter(stream);

    // 0: Write header chunk
    chunkWriter.WriteChunk(&_s6.header, SAWYER_ENCODING::ROTATE);

    // 1: Write scenario info chunk
    if (_s6.header.type == S6_TYPE_SCENARIO)
    {
        chunkWriter.WriteChunk(&_s6.info, SAWYER_ENCODING::ROTATE);
    }

    // 2: Write packed objects
    if (_s6.header.num_packed_objects > 0)
    {
        auto& objRepo = OpenRCT2::GetContext()->GetObjectRepository();
        objRepo.WritePackedObjects(stream, ExportObjectsList);
    }

    // 3: Write available objects chunk
    chunkWriter.WriteChunk(_s6.objects, sizeof(_s6.objects), SAWYER_ENCODING::ROTATE);

    // 4: Misc fields (data, rand...) chunk
    chunkWriter.WriteChunk(&_s6.elapsed_months, 16, SAWYER_ENCODING::RLECOMPRESSED);

    // 5: Map elements + sprites and other fields chunk
    chunkWriter.WriteChunk(&_s6.tile_elements, 0x180000, SAWYER_ENCODING::RLECOMPRESSED);

    if (_s6.header.type == S6_TYPE_SCENARIO)
    {
        // 6 to 13:
        chunkWriter.WriteChunk(&_s6.next_free_tile_element_pointer_index, 0x27104C, SAWYER_ENCODING::RLECOMPRESSED);
        chunkWriter.WriteChunk(&_s6.guests_in_park, 4, SAWYER_ENCODING::RLECOMPRESSED);
        chunkWriter.WriteChunk(&_s6.last_guests_in_park, 8, SAWYER_ENCODING::RLECOMPRESSED);
        chunkWriter.WriteChunk(&_s6.park_rating, 2, SAWYER_ENCODING::RLECOMPRESSED);
        chunkWriter.WriteChunk(&_s6.active_research_types, 1082, SAWYER_ENCODING::RLECOMPRESSED);
        chunkWriter.WriteChunk(&_s6.current_expenditure, 16, SAWYER_ENCODING::RLECOMPRESSED);
        chunkWriter.WriteChunk(&_s6.park_value, 4, SAWYER_ENCODING::RLECOMPRESSED);
        chunkWriter.WriteChunk(&_s6.completed_company_value, 0x761E8, SAWYER_ENCODING::RLECOMPRESSED);
    }
    else
    {
        // 6: Everything else...
        chunkWriter.WriteChunk(&_s6.next_free_tile_element_pointer_index, 0x2E8570, SAWYER_ENCODING::RLECOMPRESSED);
    }

    // Determine number of bytes written
    size_t fileSize = stream->GetLength();

    // Read all written bytes back into a single buffer
    stream->SetPosition(0);
    auto data = std::unique_ptr<uint8_t, std::function<void(uint8_t*)>>(
        stream->ReadArray<uint8_t>(fileSize), Memory::Free<uint8_t>);
    uint32_t checksum = sawyercoding_calculate_checksum(data.get(), fileSize);

    // Write the checksum on the end
    stream->SetPosition(fileSize);
    stream->WriteValue(checksum);
}

void S6Exporter::Export()
{
    int32_t spatial_cycle = check_for_spatial_index_cycles(false);
    int32_t regular_cycle = check_for_sprite_list_cycles(false);
    int32_t disjoint_sprites_count = fix_disjoint_sprites();
    openrct2_assert(spatial_cycle == -1, "Sprite cycle exists in spatial list %d", spatial_cycle);
    openrct2_assert(regular_cycle == -1, "Sprite cycle exists in regular list %d", regular_cycle);
    // This one is less harmful, no need to assert for it ~janisozaur
    if (disjoint_sprites_count > 0)
    {
        log_error("Found %d disjoint null sprites", disjoint_sprites_count);
    }
    _s6.info = gS6Info;
    {
        auto temp = utf8_to_rct2(gS6Info.name);
        safe_strcpy(_s6.info.name, temp.data(), sizeof(_s6.info.name));
    }
    {
        auto temp = utf8_to_rct2(gS6Info.details);
        safe_strcpy(_s6.info.details, temp.data(), sizeof(_s6.info.details));
    }
    uint32_t researchedTrackPiecesA[128];
    uint32_t researchedTrackPiecesB[128];

    for (int32_t i = 0; i < OBJECT_ENTRY_COUNT; i++)
    {
        const rct_object_entry* entry = get_loaded_object_entry(i);
        void* entryData = get_loaded_object_chunk(i);
        // RCT2 uses (void *)-1 to mark NULL. Make sure it's written in a vanilla-compatible way.
        if (entryData == nullptr || entryData == (void*)-1)
        {
            std::memset(&_s6.objects[i], 0xFF, sizeof(rct_object_entry));
        }
        else
        {
            _s6.objects[i] = *entry;
        }
    }

    _s6.elapsed_months = gDateMonthsElapsed;
    _s6.current_day = gDateMonthTicks;
    _s6.scenario_ticks = gScenarioTicks;

    auto state = scenario_rand_state();
    _s6.scenario_srand_0 = state.s0;
    _s6.scenario_srand_1 = state.s1;

    std::memcpy(_s6.tile_elements, gTileElements, sizeof(_s6.tile_elements));

    _s6.next_free_tile_element_pointer_index = gNextFreeTileElementPointerIndex;

    ExportSprites();

    _s6.park_name = gParkName;
    // pad_013573D6
    _s6.park_name_args = gParkNameArgs;
    _s6.initial_cash = gInitialCash;
    _s6.current_loan = gBankLoan;
    _s6.park_flags = gParkFlags;
    _s6.park_entrance_fee = gParkEntranceFee;
    // rct1_park_entrance_x
    // rct1_park_entrance_y
    // pad_013573EE
    // rct1_park_entrance_z
    ExportPeepSpawns();
    _s6.guest_count_change_modifier = gGuestChangeModifier;
    _s6.current_research_level = gResearchFundingLevel;
    // pad_01357400
    ExportResearchedRideTypes();
    ExportResearchedRideEntries();
    // Not used by OpenRCT2 any more, but left in to keep RCT2 export working.
    for (uint8_t i = 0; i < std::size(RideTypePossibleTrackConfigurations); i++)
    {
        researchedTrackPiecesA[i] = (RideTypePossibleTrackConfigurations[i]) & 0xFFFFFFFFULL;
        researchedTrackPiecesB[i] = (RideTypePossibleTrackConfigurations[i] >> 32ULL) & 0xFFFFFFFFULL;
    }
    std::memcpy(_s6.researched_track_types_a, researchedTrackPiecesA, sizeof(_s6.researched_track_types_a));
    std::memcpy(_s6.researched_track_types_b, researchedTrackPiecesB, sizeof(_s6.researched_track_types_b));

    _s6.guests_in_park = gNumGuestsInPark;
    _s6.guests_heading_for_park = gNumGuestsHeadingForPark;

    std::memcpy(_s6.expenditure_table, gExpenditureTable, sizeof(_s6.expenditure_table));

    _s6.last_guests_in_park = gNumGuestsInParkLastWeek;
    // pad_01357BCA
    _s6.handyman_colour = gStaffHandymanColour;
    _s6.mechanic_colour = gStaffMechanicColour;
    _s6.security_colour = gStaffSecurityColour;

    ExportResearchedSceneryItems();

    _s6.park_rating = gParkRating;

    std::memcpy(_s6.park_rating_history, gParkRatingHistory, sizeof(_s6.park_rating_history));
    std::memcpy(_s6.guests_in_park_history, gGuestsInParkHistory, sizeof(_s6.guests_in_park_history));

    _s6.active_research_types = gResearchPriorities;
    _s6.research_progress_stage = gResearchProgressStage;
    _s6.last_researched_item_subject = gResearchLastItem.rawValue;
    // pad_01357CF8
    _s6.next_research_item = gResearchNextItem.rawValue;
    _s6.research_progress = gResearchProgress;
    _s6.next_research_category = gResearchNextItem.category;
    _s6.next_research_expected_day = gResearchExpectedDay;
    _s6.next_research_expected_month = gResearchExpectedMonth;
    _s6.guest_initial_happiness = gGuestInitialHappiness;
    _s6.park_size = gParkSize;
    _s6.guest_generation_probability = _guestGenerationProbability;
    _s6.total_ride_value_for_money = gTotalRideValueForMoney;
    _s6.maximum_loan = gMaxBankLoan;
    _s6.guest_initial_cash = gGuestInitialCash;
    _s6.guest_initial_hunger = gGuestInitialHunger;
    _s6.guest_initial_thirst = gGuestInitialThirst;
    _s6.objective_type = gScenarioObjectiveType;
    _s6.objective_year = gScenarioObjectiveYear;
    // pad_013580FA
    _s6.objective_currency = gScenarioObjectiveCurrency;
    _s6.objective_guests = gScenarioObjectiveNumGuests;
    ExportMarketingCampaigns();

    std::memcpy(_s6.balance_history, gCashHistory, sizeof(_s6.balance_history));

    _s6.current_expenditure = gCurrentExpenditure;
    _s6.current_profit = gCurrentProfit;
    _s6.weekly_profit_average_dividend = gWeeklyProfitAverageDividend;
    _s6.weekly_profit_average_divisor = gWeeklyProfitAverageDivisor;
    // pad_0135833A

    std::memcpy(_s6.weekly_profit_history, gWeeklyProfitHistory, sizeof(_s6.weekly_profit_history));

    _s6.park_value = gParkValue;

    std::memcpy(_s6.park_value_history, gParkValueHistory, sizeof(_s6.park_value_history));

    _s6.completed_company_value = gScenarioCompletedCompanyValue;
    _s6.total_admissions = gTotalAdmissions;
    _s6.income_from_admissions = gTotalIncomeFromAdmissions;
    _s6.company_value = gCompanyValue;
    std::memcpy(_s6.peep_warning_throttle, gPeepWarningThrottle, sizeof(_s6.peep_warning_throttle));

    // Awards
    for (int32_t i = 0; i < RCT12_MAX_AWARDS; i++)
    {
        Award* src = &gCurrentAwards[i];
        rct12_award* dst = &_s6.awards[i];
        dst->time = src->Time;
        dst->type = src->Type;
    }

    _s6.land_price = gLandPrice;
    _s6.construction_rights_price = gConstructionRightsPrice;
    // unk_01358774
    // pad_01358776
    // _s6.cd_key
    // _s6.game_version_number
    _s6.completed_company_value_record = gScenarioCompanyValueRecord;
    _s6.loan_hash = GetLoanHash(gInitialCash, gBankLoan, gMaxBankLoan);
    _s6.ride_count = gRideCount;
    // pad_013587CA
    _s6.historical_profit = gHistoricalProfit;
    // pad_013587D4
    String::Set(_s6.scenario_completed_name, sizeof(_s6.scenario_completed_name), gScenarioCompletedBy.c_str());
    _s6.cash = ENCRYPT_MONEY(gCash);
    // pad_013587FC
    _s6.park_rating_casualty_penalty = gParkRatingCasualtyPenalty;
    _s6.map_size_units = gMapSizeUnits;
    _s6.map_size_minus_2 = gMapSizeMinus2;
    _s6.map_size = gMapSize;
    _s6.map_max_xy = gMapSizeMaxXY;
    _s6.same_price_throughout = gSamePriceThroughoutPark & 0xFFFFFFFF;
    _s6.suggested_max_guests = _suggestedGuestMaximum;
    _s6.park_rating_warning_days = gScenarioParkRatingWarningDays;
    _s6.last_entrance_style = gLastEntranceStyle;
    // rct1_water_colour
    // pad_01358842
    ExportResearchList();
    _s6.map_base_z = gMapBaseZ;
    String::Set(_s6.scenario_name, sizeof(_s6.scenario_name), gScenarioName.c_str());
    String::Set(_s6.scenario_description, sizeof(_s6.scenario_description), gScenarioDetails.c_str());
    _s6.current_interest_rate = gBankLoanInterestRate;
    // pad_0135934B
    _s6.same_price_throughout_extended = gSamePriceThroughoutPark >> 32;
    // Preserve compatibility with vanilla RCT2's save format.
    for (uint8_t i = 0; i < RCT12_MAX_PARK_ENTRANCES; i++)
    {
        CoordsXYZD entrance = { LOCATION_NULL, LOCATION_NULL, 0, 0 };
        if (gParkEntrances.size() > i)
        {
            entrance = gParkEntrances[i];
        }
        _s6.park_entrance_x[i] = entrance.x;
        _s6.park_entrance_y[i] = entrance.y;
        _s6.park_entrance_z[i] = entrance.z;
        _s6.park_entrance_direction[i] = entrance.direction;
    }
    safe_strcpy(_s6.scenario_filename, gScenarioFileName, sizeof(_s6.scenario_filename));
    std::memcpy(_s6.saved_expansion_pack_names, gScenarioExpansionPacks, sizeof(_s6.saved_expansion_pack_names));
    std::memcpy(_s6.banners, gBanners, sizeof(_s6.banners));
    std::memcpy(_s6.custom_strings, gUserStrings, sizeof(_s6.custom_strings));
    _s6.game_ticks_1 = gCurrentTicks;

    this->ExportRides();

    _s6.saved_age = gSavedAge;
    _s6.saved_view_x = gSavedViewX;
    _s6.saved_view_y = gSavedViewY;
    _s6.saved_view_zoom = gSavedViewZoom;
    _s6.saved_view_rotation = gSavedViewRotation;
    std::memcpy(_s6.map_animations, gAnimatedObjects, sizeof(_s6.map_animations));
    _s6.num_map_animations = gNumMapAnimations;
    // pad_0138B582

    _s6.ride_ratings_calc_data = gRideRatingsCalcData;
    ExportRideMeasurements();
    _s6.next_guest_index = gNextGuestNumber;
    _s6.grass_and_scenery_tilepos = gGrassSceneryTileLoopPosition;
    std::memcpy(_s6.patrol_areas, gStaffPatrolAreas, sizeof(_s6.patrol_areas));
    std::memcpy(_s6.staff_modes, gStaffModes, sizeof(_s6.staff_modes));
    // unk_13CA73E
    // pad_13CA73F
    _s6.byte_13CA740 = gUnk13CA740;
    _s6.climate = gClimate;
    // pad_13CA741;
    // byte_13CA742
    // pad_013CA747
    _s6.climate_update_timer = gClimateUpdateTimer;
    _s6.current_weather = gClimateCurrent.Weather;
    _s6.next_weather = gClimateNext.Weather;
    _s6.temperature = gClimateCurrent.Temperature;
    _s6.next_temperature = gClimateNext.Temperature;
    _s6.current_weather_effect = gClimateCurrent.WeatherEffect;
    _s6.next_weather_effect = gClimateNext.WeatherEffect;
    _s6.current_weather_gloom = gClimateCurrent.WeatherGloom;
    _s6.next_weather_gloom = gClimateNext.WeatherGloom;
    _s6.current_rain_level = gClimateCurrent.RainLevel;
    _s6.next_rain_level = gClimateNext.RainLevel;

    // News items
    for (size_t i = 0; i < RCT12_MAX_NEWS_ITEMS; i++)
    {
        const NewsItem* src = &gNewsItems[i];
        rct12_news_item* dst = &_s6.news_items[i];

        dst->Type = src->Type;
        dst->Flags = src->Flags;
        dst->Assoc = src->Assoc;
        dst->Ticks = src->Ticks;
        dst->MonthYear = src->MonthYear;
        dst->Day = src->Day;
        std::memcpy(dst->Text, src->Text, sizeof(dst->Text));
    }

    // pad_13CE730
    // rct1_scenario_flags
    _s6.wide_path_tile_loop_x = gWidePathTileLoopX;
    _s6.wide_path_tile_loop_y = gWidePathTileLoopY;
    // pad_13CE778

    String::Set(_s6.scenario_filename, sizeof(_s6.scenario_filename), gScenarioFileName);

    if (RemoveTracklessRides)
    {
        scenario_remove_trackless_rides(&_s6);
    }

    scenario_fix_ghosts(&_s6);
    game_convert_strings_to_rct2(&_s6);
}

void S6Exporter::ExportPeepSpawns()
{
    for (size_t i = 0; i < RCT12_MAX_PEEP_SPAWNS; i++)
    {
        if (gPeepSpawns.size() > i)
        {
            _s6.peep_spawns[i] = { (uint16_t)gPeepSpawns[i].x, (uint16_t)gPeepSpawns[i].y, (uint8_t)(gPeepSpawns[i].z / 16),
                                   gPeepSpawns[i].direction };
        }
        else
        {
            _s6.peep_spawns[i] = { PEEP_SPAWN_UNDEFINED, PEEP_SPAWN_UNDEFINED, 0, 0 };
        }
    }
}

uint32_t S6Exporter::GetLoanHash(money32 initialCash, money32 bankLoan, uint32_t maxBankLoan)
{
    int32_t value = 0x70093A;
    value -= initialCash;
    value = ror32(value, 5);
    value -= bankLoan;
    value = ror32(value, 7);
    value += maxBankLoan;
    value = ror32(value, 3);
    return value;
}

void S6Exporter::ExportRides()
{
    for (int32_t index = 0; index < RCT12_MAX_RIDES_IN_PARK; index++)
    {
        auto src = get_ride(index);
        auto dst = &_s6.rides[index];
        *dst = {};
        if (src->type == RIDE_TYPE_NULL)
        {
            dst->type = RIDE_TYPE_NULL;
        }
        else
        {
            ExportRide(dst, src);
        }
    }
}

void S6Exporter::ExportRide(rct2_ride* dst, const Ride* src)
{
    std::memset(dst, 0, sizeof(rct2_ride));

    dst->type = src->type;
    dst->subtype = src->subtype;
    // pad_002;
    dst->mode = src->mode;
    dst->colour_scheme_type = src->colour_scheme_type;

    for (uint8_t i = 0; i < RCT2_MAX_CARS_PER_TRAIN; i++)
    {
        dst->vehicle_colours[i].body_colour = src->vehicle_colours[i].Body;
        dst->vehicle_colours[i].trim_colour = src->vehicle_colours[i].Trim;
    }

    // pad_046;
    dst->status = src->status;
    dst->name = src->name;
    dst->name_arguments = src->name_arguments;

    dst->overall_view = src->overall_view;

    for (int32_t i = 0; i < RCT12_MAX_STATIONS_PER_RIDE; i++)
    {
        dst->station_starts[i] = src->stations[i].Start;
        dst->station_heights[i] = src->stations[i].Height;
        dst->station_length[i] = src->stations[i].Length;
        dst->station_depart[i] = src->stations[i].Depart;
        dst->train_at_station[i] = src->stations[i].TrainAtStation;

        TileCoordsXYZD entrance = ride_get_entrance_location(src, i);
        if (entrance.isNull())
            dst->entrances[i].xy = RCT_XY8_UNDEFINED;
        else
            dst->entrances[i] = { (uint8_t)entrance.x, (uint8_t)entrance.y };

        TileCoordsXYZD exit = ride_get_exit_location(src, i);
        if (exit.isNull())
            dst->exits[i].xy = RCT_XY8_UNDEFINED;
        else
            dst->exits[i] = { (uint8_t)exit.x, (uint8_t)exit.y };

        dst->last_peep_in_queue[i] = src->stations[i].LastPeepInQueue;

        dst->length[i] = src->stations[i].SegmentLength;
        dst->time[i] = src->stations[i].SegmentTime;

        dst->queue_time[i] = src->stations[i].QueueTime;

        dst->queue_length[i] = src->stations[i].QueueLength;
    }

    for (uint8_t i = 0; i < RCT2_MAX_VEHICLES_PER_RIDE; i++)
    {
        dst->vehicles[i] = src->vehicles[i];
    }

    dst->depart_flags = src->depart_flags;

    dst->num_stations = src->num_stations;
    dst->num_vehicles = src->num_vehicles;
    dst->num_cars_per_train = src->num_cars_per_train;
    dst->proposed_num_vehicles = src->proposed_num_vehicles;
    dst->proposed_num_cars_per_train = src->proposed_num_cars_per_train;
    dst->max_trains = src->max_trains;
    dst->min_max_cars_per_train = src->min_max_cars_per_train;
    dst->min_waiting_time = src->min_waiting_time;
    dst->max_waiting_time = src->max_waiting_time;

    // Includes time_limit, num_laps, launch_speed, speed, rotations
    dst->operation_option = src->operation_option;

    dst->boat_hire_return_direction = src->boat_hire_return_direction;
    dst->boat_hire_return_position = src->boat_hire_return_position;

    dst->special_track_elements = src->special_track_elements;
    // pad_0D6[2];

    dst->max_speed = src->max_speed;
    dst->average_speed = src->average_speed;
    dst->current_test_segment = src->current_test_segment;
    dst->average_speed_test_timeout = src->average_speed_test_timeout;
    // pad_0E2[0x2];

    dst->max_positive_vertical_g = src->max_positive_vertical_g;
    dst->max_negative_vertical_g = src->max_negative_vertical_g;
    dst->max_lateral_g = src->max_lateral_g;
    dst->previous_vertical_g = src->previous_vertical_g;
    dst->previous_lateral_g = src->previous_lateral_g;
    // pad_106[0x2];
    dst->testing_flags = src->testing_flags;
    dst->cur_test_track_location = src->cur_test_track_location;
    dst->turn_count_default = src->turn_count_default;
    dst->turn_count_banked = src->turn_count_banked;
    dst->turn_count_sloped = src->turn_count_sloped;
    if (dst->type == RIDE_TYPE_MINI_GOLF)
        dst->inversions = (uint8_t)std::min(src->holes, RCT12_MAX_GOLF_HOLES);
    else
        dst->inversions = (uint8_t)std::min(src->inversions, RCT12_MAX_INVERSIONS);
    dst->inversions |= (src->sheltered_eighths << 5);
    dst->drops = src->drops;
    dst->start_drop_height = src->start_drop_height;
    dst->highest_drop_height = src->highest_drop_height;
    dst->sheltered_length = src->sheltered_length;
    dst->var_11C = src->var_11C;
    dst->num_sheltered_sections = src->num_sheltered_sections;
    dst->cur_test_track_z = src->cur_test_track_z;

    dst->cur_num_customers = src->cur_num_customers;
    dst->num_customers_timeout = src->num_customers_timeout;

    for (uint8_t i = 0; i < RCT2_CUSTOMER_HISTORY_SIZE; i++)
    {
        dst->num_customers[i] = src->num_customers[i];
    }

    dst->price = src->price;

    for (uint8_t i = 0; i < 2; i++)
    {
        dst->chairlift_bullwheel_location[i] = src->chairlift_bullwheel_location[i];
        dst->chairlift_bullwheel_z[i] = src->chairlift_bullwheel_z[i];
    }

    dst->ratings = src->ratings;
    dst->value = src->value;

    dst->chairlift_bullwheel_rotation = src->chairlift_bullwheel_rotation;

    dst->satisfaction = src->satisfaction;
    dst->satisfaction_time_out = src->satisfaction_time_out;
    dst->satisfaction_next = src->satisfaction_next;

    dst->window_invalidate_flags = src->window_invalidate_flags;
    // pad_14E[0x02];

    dst->total_customers = src->total_customers;
    dst->total_profit = src->total_profit;
    dst->popularity = src->popularity;
    dst->popularity_time_out = src->popularity_time_out;
    dst->popularity_next = src->popularity_next;
    dst->num_riders = src->num_riders;
    dst->music_tune_id = src->music_tune_id;
    dst->slide_in_use = src->slide_in_use;
    // Includes maze_tiles
    dst->slide_peep = src->slide_peep;
    // pad_160[0xE];
    dst->slide_peep_t_shirt_colour = src->slide_peep_t_shirt_colour;
    // pad_16F[0x7];
    dst->spiral_slide_progress = src->spiral_slide_progress;
    // pad_177[0x9];
    dst->build_date = src->build_date;
    dst->upkeep_cost = src->upkeep_cost;
    dst->race_winner = src->race_winner;
    // pad_186[0x02];
    dst->music_position = src->music_position;

    dst->breakdown_reason_pending = src->breakdown_reason_pending;
    dst->mechanic_status = src->mechanic_status;
    dst->mechanic = src->mechanic;
    dst->inspection_station = src->inspection_station;
    dst->broken_vehicle = src->broken_vehicle;
    dst->broken_car = src->broken_car;
    dst->breakdown_reason = src->breakdown_reason;

    dst->price_secondary = src->price_secondary;

    dst->reliability = src->reliability;
    dst->unreliability_factor = src->unreliability_factor;
    dst->downtime = src->downtime;
    dst->inspection_interval = src->inspection_interval;
    dst->last_inspection = src->last_inspection;

    for (uint8_t i = 0; i < RCT2_DOWNTIME_HISTORY_SIZE; i++)
    {
        dst->downtime_history[i] = src->downtime_history[i];
    }

    dst->no_primary_items_sold = src->no_primary_items_sold;
    dst->no_secondary_items_sold = src->no_secondary_items_sold;

    dst->breakdown_sound_modifier = src->breakdown_sound_modifier;
    dst->not_fixed_timeout = src->not_fixed_timeout;
    dst->last_crash_type = src->last_crash_type;
    dst->connected_message_throttle = src->connected_message_throttle;

    dst->income_per_hour = src->income_per_hour;
    dst->profit = src->profit;

    for (uint8_t i = 0; i < RCT12_NUM_COLOUR_SCHEMES; i++)
    {
        dst->track_colour_main[i] = src->track_colour[i].main;
        dst->track_colour_additional[i] = src->track_colour[i].additional;
        dst->track_colour_supports[i] = src->track_colour[i].supports;
    }

    dst->music = src->music;
    dst->entrance_style = src->entrance_style;
    dst->vehicle_change_timeout = src->vehicle_change_timeout;
    dst->num_block_brakes = src->num_block_brakes;
    dst->lift_hill_speed = src->lift_hill_speed;
    dst->guests_favourite = src->guests_favourite;
    dst->lifecycle_flags = src->lifecycle_flags;

    for (uint8_t i = 0; i < RCT2_MAX_CARS_PER_TRAIN; i++)
    {
        dst->vehicle_colours_extended[i] = src->vehicle_colours[i].Ternary;
    }

    dst->total_air_time = src->total_air_time;
    dst->current_test_station = src->current_test_station;
    dst->num_circuits = src->num_circuits;
    dst->cable_lift_x = src->cable_lift_x;
    dst->cable_lift_y = src->cable_lift_y;
    dst->cable_lift_z = src->cable_lift_z;
    // pad_1FD;
    dst->cable_lift = src->cable_lift;

    // pad_208[0x58];
}

void S6Exporter::ExportRideMeasurements()
{
    // Get all the ride measurements
    std::vector<const RideMeasurement*> rideMeasurements;
    for (ride_id_t i = 0; i < RCT12_MAX_RIDES_IN_PARK; i++)
    {
        auto ride = get_ride(i);
        if (ride != nullptr && ride->measurement != nullptr)
        {
            rideMeasurements.push_back(ride->measurement.get());
        }
    }

    // If there are more than S6 can hold, trim it by LRU
    if (rideMeasurements.size() > RCT12_RIDE_MEASUREMENT_MAX_ITEMS)
    {
        // Sort in order of last recently used
        std::sort(rideMeasurements.begin(), rideMeasurements.end(), [](const RideMeasurement* a, const RideMeasurement* b) {
            return a->last_use_tick > b->last_use_tick;
        });
        rideMeasurements.resize(RCT12_RIDE_MEASUREMENT_MAX_ITEMS);
    }

    // Convert ride measurements to S6 format
    uint8_t i{};
    for (auto src : rideMeasurements)
    {
        auto& dst = _s6.ride_measurements[i];
        ExportRideMeasurement(_s6.ride_measurements[i], *src);

        auto rideId = src->ride->id;
        dst.ride_index = rideId;
        _s6.rides[rideId].measurement_index = i;
        i++;
    }
}

void S6Exporter::ExportRideMeasurement(RCT12RideMeasurement& dst, const RideMeasurement& src)
{
    dst.flags = src.flags;
    dst.last_use_tick = src.last_use_tick;
    dst.num_items = src.num_items;
    dst.current_item = src.current_item;
    dst.vehicle_index = src.vehicle_index;
    dst.current_station = src.current_station;
    for (size_t i = 0; i < std::size(src.velocity); i++)
    {
        dst.velocity[i] = src.velocity[i];
        dst.altitude[i] = src.altitude[i];
        dst.vertical[i] = src.vertical[i];
        dst.lateral[i] = src.lateral[i];
    }
}

void S6Exporter::ExportResearchedRideTypes()
{
    std::fill(std::begin(_s6.researched_ride_types), std::end(_s6.researched_ride_types), false);

    for (int32_t rideType = 0; rideType < RIDE_TYPE_COUNT; rideType++)
    {
        if (ride_type_is_invented(rideType))
        {
            int32_t quadIndex = rideType >> 5;
            int32_t bitIndex = rideType & 0x1F;
            _s6.researched_ride_types[quadIndex] |= (uint32_t)1 << bitIndex;
        }
    }
}

void S6Exporter::ExportResearchedRideEntries()
{
    std::fill(std::begin(_s6.researched_ride_entries), std::end(_s6.researched_ride_entries), false);

    for (int32_t rideEntryIndex = 0; rideEntryIndex < MAX_RIDE_OBJECTS; rideEntryIndex++)
    {
        if (ride_entry_is_invented(rideEntryIndex))
        {
            int32_t quadIndex = rideEntryIndex >> 5;
            int32_t bitIndex = rideEntryIndex & 0x1F;
            _s6.researched_ride_entries[quadIndex] |= (uint32_t)1 << bitIndex;
        }
    }
}

void S6Exporter::ExportResearchedSceneryItems()
{
    std::fill(std::begin(_s6.researched_scenery_items), std::end(_s6.researched_scenery_items), false);

    for (uint16_t sceneryEntryIndex = 0; sceneryEntryIndex < RCT2_MAX_RESEARCHED_SCENERY_ITEMS; sceneryEntryIndex++)
    {
        if (scenery_is_invented(sceneryEntryIndex))
        {
            int32_t quadIndex = sceneryEntryIndex >> 5;
            int32_t bitIndex = sceneryEntryIndex & 0x1F;
            _s6.researched_scenery_items[quadIndex] |= (uint32_t)1 << bitIndex;
        }
    }
}

void S6Exporter::ExportResearchList()
{
    std::memcpy(_s6.research_items, gResearchItems, sizeof(_s6.research_items));
}

void S6Exporter::ExportMarketingCampaigns()
{
    std::memset(_s6.campaign_weeks_left, 0, sizeof(_s6.campaign_weeks_left));
    std::memset(_s6.campaign_ride_index, 0, sizeof(_s6.campaign_ride_index));
    for (const auto& campaign : gMarketingCampaigns)
    {
        _s6.campaign_weeks_left[campaign.Type] = campaign.WeeksLeft | CAMPAIGN_ACTIVE_FLAG;
        if (campaign.Type == ADVERTISING_CAMPAIGN_RIDE_FREE || campaign.Type == ADVERTISING_CAMPAIGN_RIDE)
        {
            _s6.campaign_ride_index[campaign.Type] = campaign.RideId;
        }
        else if (campaign.Type == ADVERTISING_CAMPAIGN_FOOD_OR_DRINK_FREE)
        {
            _s6.campaign_ride_index[campaign.Type] = campaign.ShopItemType;
        }
    }
}

void S6Exporter::ExportSprites()
{
    // Sprites needs to be reset before they get used.
    // Might as well reset them in here to zero out the space and improve
    // compression ratios. Especially useful for multiplayer servers that
    // use zlib on the sent stream.
    sprite_clear_all_unused();
    for (int32_t i = 0; i < RCT2_MAX_SPRITES; i++)
    {
        ExportSprite(&_s6.sprites[i], get_sprite(i));
    }

    for (int32_t i = 0; i < NUM_SPRITE_LISTS; i++)
    {
        _s6.sprite_lists_head[i] = gSpriteListHead[i];
        _s6.sprite_lists_count[i] = gSpriteListCount[i];
    }
}

void S6Exporter::ExportSprite(RCT2Sprite* dst, const rct_sprite* src)
{
    std::memset(dst, 0, sizeof(rct_sprite));
    switch (src->generic.sprite_identifier)
    {
        case SPRITE_IDENTIFIER_NULL:
            ExportSpriteCommonProperties(&dst->unknown, &src->generic);
            break;
        case SPRITE_IDENTIFIER_VEHICLE:
            ExportSpriteVehicle(&dst->vehicle, &src->vehicle);
            break;
        case SPRITE_IDENTIFIER_PEEP:
            ExportSpritePeep(&dst->peep, &src->peep);
            break;
        case SPRITE_IDENTIFIER_MISC:
            ExportSpriteMisc(&dst->unknown, &src->generic);
            break;
        case SPRITE_IDENTIFIER_LITTER:
            ExportSpriteLitter(&dst->litter, &src->litter);
            break;
        default:
            ExportSpriteCommonProperties(&dst->unknown, &src->generic);
            log_warning("Sprite identifier %d can not be exported.", src->generic.sprite_identifier);
            break;
    }
}

void S6Exporter::ExportSpriteCommonProperties(RCT12SpriteBase* dst, const rct_sprite_common* src)
{
    dst->sprite_identifier = src->sprite_identifier;
    dst->type = src->type;
    dst->next_in_quadrant = src->next_in_quadrant;
    dst->next = src->next;
    dst->previous = src->previous;
    dst->linked_list_type_offset = src->linked_list_type_offset;
    dst->sprite_height_negative = src->sprite_height_negative;
    dst->sprite_index = src->sprite_index;
    dst->flags = src->flags;
    dst->x = src->x;
    dst->y = src->y;
    dst->z = src->z;
    dst->sprite_width = src->sprite_width;
    dst->sprite_height_positive = src->sprite_height_positive;
    dst->sprite_left = src->sprite_left;
    dst->sprite_top = src->sprite_top;
    dst->sprite_right = src->sprite_right;
    dst->sprite_bottom = src->sprite_bottom;
    dst->sprite_direction = src->sprite_direction;
}

void S6Exporter::ExportSpriteVehicle(RCT2SpriteVehicle* dst, const rct_vehicle* src)
{
    ExportSpriteCommonProperties(dst, (const rct_sprite_common*)src);
    dst->vehicle_sprite_type = src->vehicle_sprite_type;
    dst->bank_rotation = src->bank_rotation;
    dst->remaining_distance = src->remaining_distance;
    dst->velocity = src->velocity;
    dst->acceleration = src->acceleration;
    dst->ride = src->ride;
    dst->vehicle_type = src->vehicle_type;
    dst->colours = src->colours;
    dst->track_progress = src->track_progress;
    dst->track_direction = src->track_direction;
    dst->track_type = src->track_type;
    dst->track_x = src->track_x;
    dst->track_y = src->track_y;
    dst->track_z = src->track_z;
    dst->next_vehicle_on_train = src->next_vehicle_on_train;
    dst->prev_vehicle_on_ride = src->prev_vehicle_on_ride;
    dst->next_vehicle_on_ride = src->next_vehicle_on_ride;
    dst->var_44 = src->var_44;
    dst->mass = src->mass;
    dst->update_flags = src->update_flags;
    dst->swing_sprite = src->swing_sprite;
    dst->current_station = src->current_station;
    dst->current_time = src->current_time;
    dst->crash_z = src->crash_z;
    dst->status = src->status;
    dst->sub_state = src->sub_state;
    for (size_t i = 0; i < std::size(src->peep); i++)
    {
        dst->peep[i] = src->peep[i];
        dst->peep_tshirt_colours[i] = src->peep_tshirt_colours[i];
    }
    dst->num_seats = src->num_seats;
    dst->num_peeps = src->num_peeps;
    dst->next_free_seat = src->next_free_seat;
    dst->restraints_position = src->restraints_position;
    dst->crash_x = src->crash_x;
    dst->sound2_flags = src->sound2_flags;
    dst->spin_sprite = src->spin_sprite;
    dst->sound1_id = src->sound1_id;
    dst->sound1_volume = src->sound1_volume;
    dst->sound2_id = src->sound2_id;
    dst->sound2_volume = src->sound2_volume;
    dst->sound_vector_factor = src->sound_vector_factor;
    dst->time_waiting = src->time_waiting;
    dst->speed = src->speed;
    dst->powered_acceleration = src->powered_acceleration;
    dst->dodgems_collision_direction = src->dodgems_collision_direction;
    dst->animation_frame = src->animation_frame;
    dst->var_C8 = src->var_C8;
    dst->var_CA = src->var_CA;
    dst->scream_sound_id = src->scream_sound_id;
    dst->var_CD = src->var_CD;
    dst->var_CE = src->var_CE;
    dst->var_CF = src->var_CF;
    dst->lost_time_out = src->lost_time_out;
    dst->vertical_drop_countdown = src->vertical_drop_countdown;
    dst->var_D3 = src->var_D3;
    dst->mini_golf_current_animation = src->mini_golf_current_animation;
    dst->mini_golf_flags = src->mini_golf_flags;
    dst->ride_subtype = src->ride_subtype;
    dst->colours_extended = src->colours_extended;
    dst->seat_rotation = src->seat_rotation;
    dst->target_seat_rotation = src->target_seat_rotation;
}

void S6Exporter::ExportSpritePeep(RCT2SpritePeep* dst, const Peep* src)
{
    ExportSpriteCommonProperties(dst, (const rct_sprite_common*)src);
    dst->name_string_idx = src->name_string_idx;
    dst->next_x = src->next_x;
    dst->next_y = src->next_y;
    dst->next_z = src->next_z;
    dst->next_flags = src->next_flags;
    dst->outside_of_park = src->outside_of_park;
    dst->state = (uint8_t)src->state;
    dst->sub_state = src->sub_state;
    dst->sprite_type = (uint8_t)src->sprite_type;
    dst->peep_type = (uint8_t)src->type;
    dst->no_of_rides = src->no_of_rides;
    dst->tshirt_colour = src->tshirt_colour;
    dst->trousers_colour = src->trousers_colour;
    dst->destination_x = src->destination_x;
    dst->destination_y = src->destination_y;
    dst->destination_tolerance = src->destination_tolerance;
    dst->var_37 = src->var_37;
    dst->energy = src->energy;
    dst->energy_target = src->energy_target;
    dst->happiness = src->happiness;
    dst->happiness_target = src->happiness_target;
    dst->nausea = src->nausea;
    dst->nausea_target = src->nausea_target;
    dst->hunger = src->hunger;
    dst->thirst = src->thirst;
    dst->toilet = src->toilet;
    dst->mass = src->mass;
    dst->time_to_consume = src->time_to_consume;
    dst->intensity = src->intensity;
    dst->nausea_tolerance = src->nausea_tolerance;
    dst->window_invalidate_flags = src->window_invalidate_flags;
    dst->paid_on_drink = src->paid_on_drink;
    for (size_t i = 0; i < std::size(src->ride_types_been_on); i++)
    {
        dst->ride_types_been_on[i] = src->ride_types_been_on[i];
    }
    dst->item_extra_flags = src->item_extra_flags;
    dst->photo2_ride_ref = src->photo2_ride_ref;
    dst->photo3_ride_ref = src->photo3_ride_ref;
    dst->photo4_ride_ref = src->photo4_ride_ref;
    dst->current_ride = src->current_ride;
    dst->current_ride_station = src->current_ride_station;
    dst->current_train = src->current_train;
    dst->time_to_sitdown = src->time_to_sitdown;
    dst->special_sprite = src->special_sprite;
    dst->action_sprite_type = (uint8_t)src->action_sprite_type;
    dst->next_action_sprite_type = (uint8_t)src->next_action_sprite_type;
    dst->action_sprite_image_offset = src->action_sprite_image_offset;
    dst->action = (uint8_t)src->action;
    dst->action_frame = src->action_frame;
    dst->step_progress = src->step_progress;
    dst->next_in_queue = src->next_in_queue;
    dst->direction = src->direction;
    dst->interaction_ride_index = src->interaction_ride_index;
    dst->time_in_queue = src->time_in_queue;
    for (size_t i = 0; i < std::size(src->rides_been_on); i++)
    {
        dst->rides_been_on[i] = src->rides_been_on[i];
    }
    dst->id = src->id;
    dst->cash_in_pocket = src->cash_in_pocket;
    dst->cash_spent = src->cash_spent;
    dst->time_in_park = src->time_in_park;
    dst->rejoin_queue_timeout = src->rejoin_queue_timeout;
    dst->previous_ride = src->previous_ride;
    dst->previous_ride_time_out = src->previous_ride_time_out;
    for (size_t i = 0; i < std::size(src->thoughts); i++)
    {
        auto srcThought = &src->thoughts[i];
        auto dstThought = &dst->thoughts[i];
        dstThought->type = (uint8_t)srcThought->type;
        dstThought->item = srcThought->item;
        dstThought->freshness = srcThought->freshness;
        dstThought->fresh_timeout = srcThought->fresh_timeout;
    }
    dst->path_check_optimisation = src->path_check_optimisation;
    dst->guest_heading_to_ride_id = src->guest_heading_to_ride_id;
    dst->peep_is_lost_countdown = src->peep_is_lost_countdown;
    dst->photo1_ride_ref = src->photo1_ride_ref;
    dst->peep_flags = src->peep_flags;
    dst->pathfind_goal = src->pathfind_goal;
    for (size_t i = 0; i < std::size(src->pathfind_history); i++)
    {
        dst->pathfind_history[i] = src->pathfind_history[i];
    }
    dst->no_action_frame_num = src->no_action_frame_num;
    dst->litter_count = src->litter_count;
    dst->time_on_ride = src->time_on_ride;
    dst->disgusting_count = src->disgusting_count;
    dst->paid_to_enter = src->paid_to_enter;
    dst->paid_on_rides = src->paid_on_rides;
    dst->paid_on_food = src->paid_on_food;
    dst->paid_on_souvenirs = src->paid_on_souvenirs;
    dst->no_of_food = src->no_of_food;
    dst->no_of_drinks = src->no_of_drinks;
    dst->no_of_souvenirs = src->no_of_souvenirs;
    dst->vandalism_seen = src->vandalism_seen;
    dst->voucher_type = src->voucher_type;
    dst->voucher_arguments = src->voucher_arguments;
    dst->surroundings_thought_timeout = src->surroundings_thought_timeout;
    dst->angriness = src->angriness;
    dst->time_lost = src->time_lost;
    dst->days_in_queue = src->days_in_queue;
    dst->balloon_colour = src->balloon_colour;
    dst->umbrella_colour = src->umbrella_colour;
    dst->hat_colour = src->hat_colour;
    dst->favourite_ride = src->favourite_ride;
    dst->favourite_ride_rating = src->favourite_ride_rating;
    dst->item_standard_flags = src->item_standard_flags;
}

void S6Exporter::ExportSpriteMisc(RCT12SpriteBase* cdst, const rct_sprite_common* csrc)
{
    ExportSpriteCommonProperties(cdst, csrc);
    switch (cdst->type)
    {
        case SPRITE_MISC_STEAM_PARTICLE:
        {
            auto src = (const RCT12SpriteSteamParticle*)csrc;
            auto dst = (rct_steam_particle*)cdst;
            dst->time_to_move = src->time_to_move;
            dst->frame = src->frame;
            break;
        }
        case SPRITE_MISC_MONEY_EFFECT:
        {
            auto src = (const RCT12SpriteMoneyEffect*)csrc;
            auto dst = (rct_money_effect*)cdst;
            dst->move_delay = src->move_delay;
            dst->num_movements = src->num_movements;
            dst->vertical = src->vertical;
            dst->value = src->value;
            dst->offset_x = src->offset_x;
            dst->wiggle = src->wiggle;
            break;
        }
        case SPRITE_MISC_CRASHED_VEHICLE_PARTICLE:
        {
            auto src = (const RCT12SpriteCrashedVehicleParticle*)csrc;
            auto dst = (rct_crashed_vehicle_particle*)cdst;
            dst->frame = src->frame;
            dst->time_to_live = src->time_to_live;
            dst->frame = src->frame;
            dst->colour[0] = src->colour[0];
            dst->colour[1] = src->colour[1];
            dst->crashed_sprite_base = src->crashed_sprite_base;
            dst->velocity_x = src->velocity_x;
            dst->velocity_y = src->velocity_y;
            dst->velocity_z = src->velocity_z;
            dst->acceleration_x = src->acceleration_x;
            dst->acceleration_y = src->acceleration_y;
            dst->acceleration_z = src->acceleration_z;
            break;
        }
        case SPRITE_MISC_EXPLOSION_CLOUD:
        case SPRITE_MISC_EXPLOSION_FLARE:
        case SPRITE_MISC_CRASH_SPLASH:
        {
            auto src = (const rct_sprite_generic*)csrc;
            auto dst = (RCT12SpriteParticle*)cdst;
            dst->frame = src->frame;
            break;
        }
        case SPRITE_MISC_JUMPING_FOUNTAIN_WATER:
        case SPRITE_MISC_JUMPING_FOUNTAIN_SNOW:
        {
            auto src = (const rct_jumping_fountain*)csrc;
            auto dst = (RCT12SpriteJumpingFountain*)cdst;
            dst->num_ticks_alive = src->num_ticks_alive;
            dst->frame = src->frame;
            dst->fountain_flags = src->fountain_flags;
            dst->target_x = src->target_x;
            dst->target_y = src->target_y;
            dst->iteration = src->iteration;
            break;
        }
        case SPRITE_MISC_BALLOON:
        {
            auto src = (const rct_balloon*)csrc;
            auto dst = (RCT12SpriteBalloon*)cdst;
            dst->popped = src->popped;
            dst->time_to_move = src->time_to_move;
            dst->frame = src->frame;
            dst->colour = src->colour;
            break;
        }
        case SPRITE_MISC_DUCK:
        {
            auto src = (const rct_duck*)csrc;
            auto dst = (RCT12SpriteDuck*)cdst;
            dst->frame = src->frame;
            dst->target_x = src->target_x;
            dst->target_y = src->target_y;
            dst->state = src->state;
            break;
        }
        default:
            log_warning("Misc. sprite type %d can not be exported.", cdst->type);
            break;
    }
}

void S6Exporter::ExportSpriteLitter(RCT12SpriteLitter* dst, const rct_litter* src)
{
    ExportSpriteCommonProperties(dst, src);
    dst->creationTick = src->creationTick;
}

enum : uint32_t
{
    S6_SAVE_FLAG_EXPORT = 1 << 0,
    S6_SAVE_FLAG_SCENARIO = 1 << 1,
    S6_SAVE_FLAG_AUTOMATIC = 1u << 31,
};

/**
 *
 *  rct2: 0x006754F5
 * @param flags bit 0: pack objects, 1: save as scenario
 */
int32_t scenario_save(const utf8* path, int32_t flags)
{
    if (flags & S6_SAVE_FLAG_SCENARIO)
    {
        log_verbose("saving scenario");
    }
    else
    {
        log_verbose("saving game");
    }

    if (!(flags & S6_SAVE_FLAG_AUTOMATIC))
    {
        window_close_construction_windows();
    }

    map_reorganise_elements();
    viewport_set_saved_view();

    bool result = false;
    auto s6exporter = new S6Exporter();
    try
    {
        if (flags & S6_SAVE_FLAG_EXPORT)
        {
            auto& objManager = OpenRCT2::GetContext()->GetObjectManager();
            s6exporter->ExportObjectsList = objManager.GetPackableObjects();
        }
        s6exporter->RemoveTracklessRides = true;
        s6exporter->Export();
        if (flags & S6_SAVE_FLAG_SCENARIO)
        {
            s6exporter->SaveScenario(path);
        }
        else
        {
            s6exporter->SaveGame(path);
        }
        result = true;
    }
    catch (const std::exception&)
    {
    }
    delete s6exporter;

    gfx_invalidate_screen();

    if (result && !(flags & S6_SAVE_FLAG_AUTOMATIC))
    {
        gScreenAge = 0;
    }
    return result;
}
