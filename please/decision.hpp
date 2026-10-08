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

        // Queen emergency handling must still obey the two most important
        // survival rules: never hit a wall and never reverse into our own body.
        if (State::IS_QUEEN) {
            auto emergency = Search::choose_emergency_move(
                here,
                round_num,
                others,
                Search::SearchMode::QUEEN
            );

            if (emergency.has_value()) {
                unswbc::ct->make_move(*emergency);
                return;
            }

            // If every scored move was rejected, make one final legal
            // forward/non-reversing move. This is deliberately allowed to
            // ignore the normal dead-end preference because staying alive
            // is more important than the Queen's other objectives.
            auto opposite = unswbc::ct->get_dir().get_opposite();

            for (auto d : unswbc::Direction::get_direction_list()) {
                if (d == opposite)
                    continue;

                auto nxt = Search::open_step(here, d);
                if (!nxt.has_value())
                    continue;

                unswbc::ct->make_move(d);
                return;
            }

            // If movement is impossible, splitting is preferable to issuing
            // an invalid movement command.
            if (desperate_split(others))
                return;

            return;
        }

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
        // The Queen now splits only after its own movement priorities fail.
        if (State::IS_QUEEN)
            return false;

        if (!State::AGGRESSIVE ||
            unswbc::ct->get_length() < State::SPLIT_MIN_LENGTH) {
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

            if (nxt.has_value() && d != unswbc::ct->get_dir().get_opposite()) {
                return true;
            }
        }
        return false;
    }

    bool execute_path(
        const Search::PathResult& path,
        const std::vector<unswbc::Tile>& tiles
    ) {
        int length = unswbc::ct->get_length();
        int max_moves = (length + 3) / 4;

        std::unordered_set<
            unswbc::Position,
            unswbc::PositionHash
        > visible;

        for (auto tile : tiles) {
            visible.insert(tile.get_position());
        }

        int moves = 0;

        // Only execute moves that are:
        // 1. within the sprint limit
        // 2. present in the path
        // 3. inside current vision
        for (int i = 0; i < max_moves; ++i) {

            if (i >= static_cast<int>(path.directions.size()))
                break;

            if (i >= static_cast<int>(path.positions.size()))
                break;

            if (!visible.count(path.positions[i]))
                break;

            ++moves;
        }

        if (moves <= 0)
            return false;

        std::vector<unswbc::Direction> directions(
            path.directions.begin(),
            path.directions.begin() + moves
        );

        std::vector<unswbc::Position> positions(
            path.positions.begin(),
            path.positions.begin() + moves
        );

        Memory::predict_own_body(
            positions,
            length
        );

        unswbc::ct->make_moves(directions);
        return true;
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

    bool queen_try_stall_split() {
        int length = unswbc::ct->get_length();

        if (length < 2 * unswbc::Constants::MIN_SIZE)
            return false;

        int child = unswbc::Constants::MIN_SIZE;

        if (!unswbc::ct->can_split(child))
            return false;

        unswbc::ct->do_split(child);
        return true;
    }

    void queen_choose_move(
        const std::vector<unswbc::DragonPart*>& others,
        int round_num,
        const std::vector<unswbc::Tile>& tiles,
        double* deadline
    ) {
        unswbc::Position here = unswbc::ct->get_position();
        int moves_this_turn = (unswbc::ct->get_length() + 3) / 4;

        if (!State::IS_QUEEN && !has_available_move(here, tiles)) {
            last_resort(round_num, others);
            return;
        }

        auto fatal_zone = Search::build_fatal_zone(others);
        auto queen_safe_zone = Search::build_queen_safe_zone(others);
        Search::SearchMode mode = Search::SearchMode::QUEEN;

        // 1. Pearl priority. The Queen deliberately does not inherit
        // normal exploration or unknown-portal targets here.
        auto pearl_targets = Search::gather_pearl_targets(here, tiles);
        auto cluster = Search::find_pearl_cluster(pearl_targets);

        if (!cluster.empty())
            pearl_targets = cluster;

        if (!pearl_targets.empty()) {
            auto path = Search::find_valid_path(
                here,
                pearl_targets,
                mode,
                fatal_zone,
                queen_safe_zone,
                State::BFS_CAP,
                deadline,
                round_num,
                moves_this_turn,
                others
            );

            if (path.has_value() && execute_path(*path, tiles)) {
                return;
            }
        }

        // 2. Safe-position fallback. These are known, non-dangerous
        // positions that have useful future mobility.
        auto safe_targets = Search::get_queen_safe_targets(
            here,
            others,
            round_num
        );

        if (!safe_targets.empty()) {
            auto safe_path = Search::find_valid_path(
                here,
                safe_targets,
                mode,
                fatal_zone,
                queen_safe_zone,
                State::BFS_CAP,
                deadline,
                round_num,
                moves_this_turn,
                others
            );

            if (safe_path.has_value() && execute_path(*safe_path, tiles)) {
                return;
            }
        }

        // 3. Controlled local exploration. The Queen may use a normal
        // frontier only when the conservative search can reach it.
        auto frontier_targets = Search::get_frontier_targets(
            here,
            round_num
        );

        if (!frontier_targets.empty()) {
            auto frontier_path = Search::find_valid_path(
                here,
                frontier_targets,
                mode,
                fatal_zone,
                queen_safe_zone,
                State::BFS_CAP,
                deadline,
                round_num,
                moves_this_turn,
                others
            );

            if (frontier_path.has_value() && execute_path(*frontier_path, tiles)) {
                return;
            }
        }

        // 4. Stall using the existing emergency move scorer. It already
        // prefers moves with more future physical mobility and rejects
        // obvious dead ends / ally blocking.
        auto emergency = Search::choose_emergency_move(
            here,
            round_num,
            others,
            Search::SearchMode::QUEEN
        );

        if (emergency.has_value()) {
            unswbc::ct->make_move(*emergency);
            return;
        }

        // 5. Queen-specific small split: this is for creating space, not
        // for normal aggressive expansion.
        if (queen_try_stall_split())
            return;

        // 6. Existing emergency fallback remains the final safety net.
        last_resort(round_num, others);
    }

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
        
        if (path.has_value()) { // if it returns a path
            execute_path(*path, tiles);
            return;
        }

        auto physical_path = Search::find_valid_physical_path(here, 
            targets, 
            State::BFS_CAP, 
            deadline, 
            round_num, 
            moves_this_turn, 
            others
        );
    
        auto frontier_targets =
            Search::get_frontier_targets(here, round_num);

        auto frontier_path = Search::find_valid_path(
            here,
            frontier_targets,
            mode,
            fatal_zone,
            safe_zone,
            State::BFS_CAP,
            deadline,
            round_num,
            moves_this_turn,
            others
        );

        if (frontier_path.has_value()) {
            execute_path(*frontier_path, tiles);
            return;
        }

        if(physical_path.has_value()){
            execute_path(*physical_path, tiles);
            return;
        }

        auto frontier_physical_path =
            Search::find_valid_physical_path(
                here,
                frontier_targets,
                State::BFS_CAP,
                deadline,
                round_num,
                moves_this_turn,
                others
            );

        if (frontier_physical_path.has_value()) {
            execute_path(*frontier_physical_path, tiles);
            return;
        }

        auto emergency = Search::choose_emergency_move(here, round_num, others);

        if (emergency.has_value()) {
            unswbc::ct->make_move(
                *emergency
            );
            return;
        }

        for (auto d :unswbc::Direction::get_direction_list()) {  // something more sophisticated for this

            auto nxt =
                Search::open_step(here, d);

            if (nxt.has_value() && d != unswbc::ct->get_dir().get_opposite()) {
                unswbc::ct->make_move(d);
            }
        }
    }
} // decision closer    