#pragma once

#include "helper.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Decision{
    /* ==========================================================
        FEEDING LOGIC
    ===========================================================*/ 
    bool seppuku_conditions(const std::vector<unswbc::DragonPart*>& others) {
        int max_length = 0;
        int max_visible = 0;
        for (const auto& kv : State::TEAMMATES) {
            max_length = std::max(
                max_length,
                kv.second.length
            );
        }
        bool ally_found = false;
        unswbc::Team my_team =
            unswbc::ct->get_team();

        for (auto* p : others) {
            if (p->get_team() == my_team &&
                p->is_head()) {
                ally_found = true;
                int masked =
                    p->get_id() & Memory::ID_MASK;
                if (State::TEAMMATES.count(masked)) {
                    max_visible = std::max(
                        max_visible,
                        State::TEAMMATES[masked].length
                    );
                }
            }
        }

        double max_avg = (max_length + max_visible + max_visible) / 3.0;
        if (unswbc::ct->get_length() < max_visible) {
            return ally_found;
        }
        if (unswbc::ct->get_length() < max_avg * 0.8) {
            return ally_found;
        }
        return false;
    }

    void suicide_bot() {
        unswbc::ct->make_move(
            unswbc::ct->get_dir().get_opposite()
        );
    }

    bool enemy_nearby(const std::vector<unswbc::DragonPart*>& others) {
        unswbc::Team my_team = unswbc::ct->get_team();
        for (auto* p : others) {
            if (p->get_team() != my_team)
                return true;
        }
        return false;
    }

    /* ==========================================================
        LAST RESORT
    ===========================================================*/

    bool desperate_split(
        const std::vector<unswbc::DragonPart*>& others
    ) {
        int length =
            unswbc::ct->get_length();

        if (length <
            2 * unswbc::Constants::MIN_SIZE) {
            return false;
        }

        int child = std::max(
            unswbc::Constants::MIN_SIZE,
            length - unswbc::Constants::MIN_SIZE
        );

        if (unswbc::ct->can_split(child)){
            unswbc::ct->do_split(child);
            return true;
        }
        return false;
    }


    void last_resort(int round_num, const std::vector<unswbc::DragonPart*>& others) {
        unswbc::Position here = unswbc::ct->get_position();

        unswbc::Direction final_dir = unswbc::ct->get_dir(); // kamikaze or just go straight 

        bool suicide = false;
        bool kamikaze = false;

        auto portals = Search::portal_directions(here, round_num);

        if (!portals.empty()){
            Memory::remember_portal_crossing(portals[0].second, round_num);
            unswbc::ct->make_move(portals[0].first);
            return;
        }


        if (desperate_split(others))
            return;

        for (auto d : unswbc::Direction::get_direction_list()) {
            unswbc::Position nxt =
                here.add_dir(d);

            auto* tile =
                unswbc::ct->get_tile(nxt);

            if (tile) {

                auto* dragon =
                    tile->get_dragon();

                if (dragon != nullptr &&
                    dragon->get_team() != unswbc::ct->get_team() &&
                    dragon->is_head()) {
                        final_dir = d;
                        kamikaze = true;
                    } 

                if (dragon != nullptr &&
                    dragon->get_team() == unswbc::ct->get_team() &&
                    dragon->is_head()) {
                    suicide = true;
                }
            }
        }
        if (kamikaze) { 
            unswbc::ct->make_move(final_dir);
            return;
        } 

        if (suicide){
            suicide_bot();
            return;
        }

        unswbc::ct->make_move(final_dir);
    }
    /* =======================================================
    * SPLITTING LOGIC
    ========================================================*/

    bool try_split(const std::vector<unswbc::DragonPart*>& others) {
        if (!State::AGGRESSIVE ||unswbc::ct->get_length() < State::SPLIT_MIN_LENGTH) {
            return false;
        }

        int length =
            unswbc::ct->get_length();

        int child = 2;

        if (length % 2 == 0) {

            if ((length / 2) % 2 != 0) {
                child = 2;
            } else {
                child = length / 2;
            }
        } else {
            child = 2;
        }

        if (!unswbc::ct->can_split(child))
            return false;
        unswbc::ct->do_split(child);

        return true;
    }

    /*===============================================================
    choose move helpers
    ===============================================================*/
    std::vector<std::pair<unswbc::Position, int>> get_targets(  // gets pearls if exists, if dont get frontier, if aggresive go for portal
        unswbc::Position here,
        const std::vector<unswbc::Tile>& tiles,
        int round_num
    ) {
        auto targets =
            Search::gather_pearl_targets(
                here,
                tiles
            );

        auto cluster =
            Search::find_pearl_cluster(targets);

        if (!cluster.empty()) {
            targets = cluster;
        }

        // No suitable pearl targets -> explore
        if (targets.empty()) {

            // Normal exploration
            auto frontier_target =
                Search::nearest_unexplored(
                    here,
                    round_num
                );

            if (frontier_target.has_value()) {
                targets.push_back({
                    frontier_target.value(),
                    0
                });
            }

            // Aggressive dragons can explore unknown portals
            if (State::AGGRESSIVE) {

                for (const auto& [portal_id, edges] :
                    State::PORTALS) {

                    // Only one endpoint known -> destination unknown
                    if (edges.size() != 1)
                        continue;

                    uint64_t key = edges.front();

                    auto portal_it =
                        State::PORTAL_EDGES.find(key);

                    if (portal_it == State::PORTAL_EDGES.end())
                        continue;

                    // Recover the position used by edge_key().
                    unswbc::Position entrance(
                        static_cast<int>(key >> 32),
                        static_cast<int>((key >> 1) & 0x7FFFFFFF)
                    );

                    if (Memory::known_occupied(entrance))
                        continue;

                    targets.push_back({
                        entrance,
                        0
                    });
                }
            }
        }

        return targets;
    }

    std::vector<unswbc::Position> get_target_positions(
        const std::vector<std::pair<unswbc::Position, int>>& targets
    ) {
        std::vector<unswbc::Position> target_positions;

        for (auto& t : targets) {
            target_positions.push_back(t.first);
        }

        return target_positions;
    }

    bool has_available_move(unswbc::Position here, const std::vector<unswbc::Tile>& tiles){
        for (auto d :
            unswbc::Direction::get_direction_list()) {

            auto nxt =
                Search::open_step(here, d);

            if (nxt.has_value()) {
                return true;
            }
        }
        return false;
    }

    void execute_path(const Search::PathResult& path) {
        int length = unswbc::ct->get_length();
        int max_moves = (length + 3) / 4;

        int moves = std::min(
            max_moves,
            static_cast<int>(path.directions.size())
        );

        if (moves <= 0){
            return;
        }

        std::vector<unswbc::Direction> directions(
            path.directions.begin(),
            path.directions.begin() + moves
        );

        unswbc::ct->make_moves(directions);
    }

    /*
        CHOOSE MOVE STRUCTURE:

            1. Gather pearl targets

            2. If there are no suitable pearl targets:
                - SAFE:
                    nearest unexplored frontier
                - AGGRESSIVE:
                    unexplored frontier + unknown portal entrances
                    choose an exploration target

            3. Safety check:
                - inspect all current possible moves
                - if there are no legal moves -> last_resort()

            4. Establish target:
                - pearls
                - otherwise exploration target
                    - unexplored frontier
                    - unknown portal (AGGRESSIVE only)

            5. Pathfind to target:
                - normal movement edges
                - known portal transitions

                AGGRESSIVE:
                    - shortest possible legal path
                    - does not care about enemies near the path

                SAFE:
                    - avoid "radius around enemies"
                    - effectively a stricter fatal zone

                Portal rules:
                    - known portals are normal pathfinding transitions
                    - SAFE can use known portals if destination is safe
                    - AGGRESSIVE can use known portals regardless of
                    enemy proximity, unless hard-dangerous
                    - unknown portals are not part of normal A*
                    - unknown portal entrances can be exploration targets
                    for AGGRESSIVE dragons

            6. Validate path:
                - check for ally blocking
                - check for obvious dead ends
                - if invalid:
                    add bad node to blocked set
                    retry pathfinding

                - blocked set resets each turn

            7. Retry pathfinding until:
                - valid path found
                - OR path attempt/time budget exhausted

            8. If timeout / pathfinding budget exhausted:
                - resort to cheap fallback

            9. If valid path found:
                - execute_path()
                - execute up to max_steps movement budget
    */

    /* ===================================================
        CHOOSE MOVE
    ====================================================*/

   void choose_move(
        const std::vector<unswbc::DragonPart*>& others,
        int round_num,
        const std::vector<unswbc::Tile>& tiles,
        double* deadline
    ){  
        unswbc::Position here = unswbc::ct->get_position();

        auto targets = get_targets(here,tiles,round_num);

        // auto target_positions = get_target_positions(targets);

        if (!has_available_move(here, tiles)){
            last_resort(round_num, others);
            return;
        } 

        auto fatal_zone = Search::build_fatal_zone(others);
        auto safe_zone = Search::build_safe_zone(others);

        Search::SearchMode mode =
            State::AGGRESSIVE
                ? Search::SearchMode::AGGRESSIVE
                : Search::SearchMode::SAFE;

        int moves_this_turn = (unswbc::ct->get_length() + 3) / 4;

        auto path = Search::find_valid_path(
            here, 
            targets, 
            mode, 
            fatal_zone, 
            safe_zone, 
            State::BFS_CAP, 
            deadline, 
            round_num, 
            moves_this_turn, 
            others);
        
        if (!path.has_value()) {
            for (auto d :unswbc::Direction::get_direction_list()) {

                auto nxt =
                    Search::open_step(here, d);

                if (nxt.has_value()) {
                    unswbc::ct->make_move(d);
                }
            }
            return;
        }

        execute_path(*path);
    }
} // decision closer