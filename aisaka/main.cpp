#include "helper.hpp"
#include <chrono>
#include <queue>
#include <deque>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <cmath>
#include <algorithm>
#include <optional>
#include <tuple>

// Custom hash for Direction to use in unordered_map
struct DirectionHash {
    std::size_t operator()(unswbc::Direction d) const {
        return static_cast<std::size_t>(d.value);
    }
};

// Helper to map direction to index 0-3 safely
inline int direction_to_index(unswbc::Direction d) {
    switch (d.value) {
        case unswbc::Direction::NORTH: return 0;
        case unswbc::Direction::EAST:  return 1;
        case unswbc::Direction::SOUTH: return 2;
        default:               return 3;
    }
}

#include "state.hpp"
#include "memory.hpp"
#include "search.hpp"
#include "kamikaze.hpp"
#include "decision.hpp"

// ============================================================================
// MAIN
// ============================================================================

bool stable_roll(int key, double chance) {
    uint32_t seed = (static_cast<uint32_t>(key) * 1103515245 + 12345);
    return (seed / static_cast<double>(0xFFFFFFFF)) < chance;
}

std::vector<unswbc::DragonPart*> other_dragon_parts(const std::vector<unswbc::Tile>& tiles) {
    int my_id = unswbc::ct->get_id();
    std::vector<unswbc::DragonPart*> parts;
    for (auto& t : tiles) {
        if (t.get_dragon() && t.get_dragon()->get_id() != my_id) parts.push_back(const_cast<unswbc::DragonPart*>(t.get_dragon()));
    }
    return parts;
}

void execute_turn() {
    int round_num = unswbc::game->get_round_num();
    double start_time = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    double deadline = start_time + State::TURN_TIME_BUDGET;

    State::reset_turn_caches();
    auto tiles = unswbc::ct->get_tiles();
    auto others = other_dragon_parts(tiles);
    

    State::EXPLORATION_VISITED.insert(unswbc::ct->get_position());
    Memory::update_own_body();
    Memory::remember_vision(tiles);
    Memory::read_sonar(round_num);

    if (Decision::try_split(others)) return;
    if (Kamikaze::kamikaze_charge(others, deadline)) return;
    if (Kamikaze::kamikaze_dodge_split(others)) return;

    if (round_num > 440 && !Decision::enemy_nearby(others) && round_num < 490 && State::IS_QUEEN != true) {
        if (Decision::seppuku_conditions(others)) {
            Decision::suicide_bot();
            return;
        }
    }

    if (round_num > 350){
        Kamikaze::KAMIKAZE_MIN_LENGTH_RATIO = 0;
        State::SPLIT_MIN_LENGTH = 7;
    }
    // choose direction theoretically should just do everything now

    Decision::choose_move(others, round_num, tiles, &deadline);
}

int main() {
    auto init_tuple = unswbc::init();
    State::WIDTH = unswbc::game->get_map_size().first;
    State::HEIGHT = unswbc::game->get_map_size().second;
    bool rolled = false;

    while (unswbc::update(*unswbc::ct, *unswbc::game)) {
        int round_num = unswbc::game->get_round_num();
        int id = unswbc::ct->get_id();

        if (!rolled) {
            double progress = (round_num - 1) / 500.0;
            double aggro_chance = std::max(1.0 - 0.5 * progress, 0.2);
            State::AGGRESSIVE = stable_roll(unswbc::ct->get_id(), aggro_chance);
            rolled = true;
            if (round_num < 100) State::AGGRESSIVE = true;
            if (id == 0 || id == 1){
                State::AGGRESSIVE = false;
                State::SAFE_DANGER_RADIUS = 2;
                State::DEAD_END_PEARL_THRESHOLD = 999;
                State::IS_QUEEN = true;
            }
        }
        
        if (round_num > 50 && unswbc::ct->get_length() > 6){
            State::AGGRESSIVE = false;
        }

        execute_turn();
        Memory::send_sonar_report();
        unswbc::end_turn();
    }
    return 0;
}