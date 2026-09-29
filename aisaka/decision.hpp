#pragma once

#include "helper.hpp"
#include <deque>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <cstdint>



// ============================================================================
// DECISION
// ============================================================================
namespace Decision {
    void suicide_bot() {
        unswbc::ct->make_move(unswbc::ct->get_dir().get_opposite());
    }

    bool enemy_nearby(const std::vector<unswbc::DragonPart*>& others) {
        unswbc::Team my_team = unswbc::ct->get_team();
        for (auto* p : others) if (p->get_team() != my_team) return true;
        return false;
    }

    double danger(unswbc::Position pos, const std::vector<unswbc::DragonPart*>& others) {
        double total = 0.0;
        for (auto* p : others) {
            int d = Search::dist(pos, p->get_position());
            if (p->is_head()) {
                if (d <= 1) total += 2.0;
                else if (d == 2) total += 1.0;
                else if (d == 3) total += 0.5;
            } else if (d <= 1) total += 0.5;
        }
        return total;
    }

    double ally_crowding(unswbc::Position pos, const std::vector<unswbc::DragonPart*>& others) {
        unswbc::Team my_team = unswbc::ct->get_team();
        double total = 0.0;
        for (auto* p : others) {
            if (p->get_team() != my_team) continue;
            int d = Search::dist(pos, p->get_position());
            if (d <= 1) total += 2.0;
            else if (d == 2) total += 1.0;
        }
        return total;
    }

    bool has_room_to_split() {
        unswbc::Position here = unswbc::ct->get_position();
        double needed = unswbc::ct->get_length() * State::SPLIT_ROOM_MULTIPLIER;
        return Search::split_room(here, here, needed) >= needed;
    }

    int nearby_ally_count(const std::vector<unswbc::DragonPart*>& others, int radius = 3) {
        unswbc::Position here = unswbc::ct->get_position();
        unswbc::Team my_team = unswbc::ct->get_team();
        std::unordered_set<int> seen;
        for (auto* p : others) {
            if (p->get_team() != my_team) continue;
            if (seen.count(p->get_id())) continue;
            if (Search::dist(here, p->get_position()) <= radius) seen.insert(p->get_id());
        }
        return seen.size();
    }

    bool desperate_split() {
        int length = unswbc::ct->get_length();
        if (length < 2 * unswbc::Constants::MIN_SIZE) return false;
        int child = std::max(unswbc::Constants::MIN_SIZE, length - unswbc::Constants::MIN_SIZE);
        if (unswbc::ct->can_split(child)) {
            unswbc::ct->do_split(child);
            return true;
        }
        return false;
    }

    std::optional<unswbc::Direction> last_resort(int round_num) {
        unswbc::Position here = unswbc::ct->get_position();
        auto portals = Search::portal_directions(here, round_num);
        if (!portals.empty()) {
            Memory::remember_portal_crossing(portals[0].second, round_num);
            return portals[0].first;
        }
        if (desperate_split()) return std::nullopt;

        for (auto d : unswbc::Direction::get_direction_list()) {
            unswbc::Position nxt = here.add_dir(d);
            auto* tile = unswbc::ct->get_tile(nxt);
            if (tile) {
                auto* dragon = tile->get_dragon();
                if (dragon != nullptr && dragon->get_team() == unswbc::ct->get_team() && dragon->is_head()) {
                    suicide_bot();
                    return std::nullopt;
                }
            }
        }
        return unswbc::ct->get_dir();
    }

    bool try_split(const std::vector<unswbc::DragonPart*>& others) {
        if (!State::AGGRESSIVE || unswbc::ct->get_length() < State::SPLIT_MIN_LENGTH) return false;
        if (nearby_ally_count(others) > State::MAX_NEARBY_ALLIES_TO_SPLIT || !has_room_to_split()) return false;
        
        int length = unswbc::ct->get_length();
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

        if (unswbc::ct->can_split(child)) {
            unswbc::ct->do_split(child);
            return true;
        }
        return false;
    }

    bool seppuku_conditions(const std::vector<unswbc::DragonPart*>& others) {
        int max_length = 0, max_visible = 0;
        for (const auto& kv : State::TEAMMATES) max_length = std::max(max_length, kv.second.length);
        
        bool ally_found = false;
        unswbc::Team my_team = unswbc::ct->get_team();
        for (auto* p : others) {
            if (p->get_team() == my_team && p->is_head()) {
                ally_found = true;
                int masked = p->get_id() & Memory::ID_MASK;
                if (State::TEAMMATES.count(masked)) {
                    max_visible = std::max(max_visible, State::TEAMMATES[masked].length);
                }
            }
        }
        double max_avg = (max_length + max_visible + max_visible) / 3.0;
        if (unswbc::ct->get_length() < max_visible) return ally_found;
        if (unswbc::ct->get_length() < max_avg * 0.8) return ally_found;
        return false;
    }

    std::optional<unswbc::Direction> choose_direction(
        const std::vector<unswbc::DragonPart*>& others,
        int round_num,
        const std::vector<unswbc::Tile>& tiles,
        double* deadline
    ) {

        unswbc::Position here = unswbc::ct->get_position();

        auto targets = Search::gather_pearl_targets(here, tiles);

        auto cluster = Search::find_pearl_cluster(targets);

        if (!cluster.empty()) {
            targets = cluster;
        }

        std::vector<unswbc::Position> target_positions;

        for (auto& t : targets) {
            target_positions.push_back(t.first);
        }

        std::unordered_map<
            unswbc::Direction,
            unswbc::Position,
            DirectionHash
        > moves;

        for (auto d : unswbc::Direction::get_direction_list()) {
            auto nxt = Search::open_step(here, d);

            if (nxt) {
                moves[d] = *nxt;
            }
        }

        auto portal_options =
            Search::portal_directions(here, round_num);

        std::unordered_set<
            unswbc::Direction,
            DirectionHash
        > unknown_portals;

        for (auto [d, pid] : portal_options) {
            unswbc::Position destination;

            if (Search::get_portal_destination(
                    here,
                    d,
                    destination
                )) {
                moves[d] = destination;
            } else {
                unknown_portals.insert(d);
            }
        }

        if (moves.empty() && unknown_portals.empty()) {
            return std::nullopt;
        }

        std::unordered_set<
            unswbc::Position,
            unswbc::PositionHash
        > fatal_zone;

        if (!State::AGGRESSIVE) {
            fatal_zone = Search::build_fatal_zone(others);

            std::unordered_map<
                unswbc::Direction,
                unswbc::Position,
                DirectionHash
            > safe_moves;

            for (auto& kv : moves) {
                if (!Search::is_fatal(
                        kv.second,
                        fatal_zone
                    )) {
                    safe_moves.insert(kv);
                }
            }

            if (!safe_moves.empty()) {
                moves = safe_moves;
            }
        }

        int needed =
            State::AGGRESSIVE
                ? unswbc::ct->get_length()
                : unswbc::ct->get_length() + 1;

        std::unordered_map<
            unswbc::Direction,
            unswbc::Position,
            DirectionHash
        > roomy;

        int best_space = -1;

        for (auto& kv : moves) {
            int sp = Search::room(
                kv.second,
                here,
                needed,
                *deadline
            );

            best_space = std::max(best_space, sp);

            if (sp >= needed) {
                roomy.insert(kv);
            }
        }

        if (!roomy.empty()) {
            moves = roomy;
        } else {
            std::unordered_map<
                unswbc::Direction,
                unswbc::Position,
                DirectionHash
            > best_moves;

            for (auto& kv : moves) {
                if (Search::room(
                        kv.second,
                        here,
                        needed,
                        *deadline
                    ) == best_space) {
                    best_moves.insert(kv);
                }
            }

            moves = best_moves;
        }

        // ------------------------------------------------------------
        // Pearl-memory exploration score
        //
        // Only relevant when there are no reachable pearl targets.
        // Higher score = more remembered tiles that can potentially
        // spawn pearls in that direction.
        // ------------------------------------------------------------

        std::unordered_map<
            unswbc::Direction,
            int,
            DirectionHash
        > pearl_memory_scores;
        
        std::unordered_map<
            unswbc::Direction,
            int,
            DirectionHash
        > unexplored_scores;

        if (targets.empty()) {
            for (auto& kv : moves) {
                pearl_memory_scores[kv.first] =
                    Search::pearl_spawn_score(
                        kv.second,
                        round_num,
                        deadline
                    );

                unexplored_scores[kv.first] =
                    Search::unexplored_score(
                        kv.second
                    );
            }
        }

        unswbc::Direction facing = unswbc::ct->get_dir();

        double now =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count();

        // ------------------------------------------------------------
        // Time-pressure fallback
        // ------------------------------------------------------------

        if (now >= *deadline) {
            unswbc::Direction best_d =
                moves.begin()->first;

            auto score = [&](unswbc::Direction d) {
                int memory_score =
                    pearl_memory_scores.count(d)
                        ? pearl_memory_scores[d]
                        : 0;

                if (targets.empty()) {
                    return std::make_tuple(
                        -memory_score,
                        Search::pearl_dist(
                            moves[d],
                            targets
                        ),
                        danger(
                            moves[d],
                            others
                        ),
                        direction_to_index(d)
                    );
                } else {
                    return std::make_tuple(
                        Search::pearl_dist(
                            moves[d],
                            targets
                        ),
                        static_cast<int>(
                            d != unswbc::ct->get_dir()
                        ),
                        danger(
                            moves[d],
                            others
                        ),
                        direction_to_index(d)
                    );
                }
            };

            auto best_s = score(best_d);

            for (auto& kv : moves) {
                auto s = score(kv.first);

                if (s < best_s) {
                    best_s = s;
                    best_d = kv.first;
                }
            }

            return best_d;
        }

        // ------------------------------------------------------------
        // Normal decision-making
        // ------------------------------------------------------------

        unswbc::Direction best_d =
            moves.begin()->first;

        if (State::AGGRESSIVE) {

            auto score = [&](unswbc::Direction d) {

                if (targets.empty()) {
                    int memory_score =
                        pearl_memory_scores.count(d)
                            ? pearl_memory_scores[d]
                            : 0;
                        int unexplored_score =
                        unexplored_scores.count(d)
                        ? unexplored_scores[d]
                        : 0;

                    
                    return std::make_tuple(
                        -memory_score,
                        -unexplored_score,
                        d != facing,
                        direction_to_index(d)
                    );

                } else {
                    return std::make_tuple(
                        Search::pearl_dist_astar(
                            moves[d],
                            targets,
                            fatal_zone,
                            *deadline,
                            false,
                            target_positions
                        ), 0, // to make tuples the same
                        d != facing,
                        direction_to_index(d)
                    );
                }
            };

            auto best_s = score(best_d);

            for (auto& kv : moves) {
                auto s = score(kv.first);

                if (s < best_s) {
                    best_s = s;
                    best_d = kv.first;
                }
            }

        } else {

            auto score = [&](unswbc::Direction d) {

                if (targets.empty()) {
                    int memory_score =
                        pearl_memory_scores.count(d)
                            ? pearl_memory_scores[d]
                            : 0;
                        int unexplored_score =
                            unexplored_scores.count(d)
                            ? unexplored_scores[d]
                            : 0;
                        
                    return std::make_tuple(
                        danger(
                            moves[d],
                            others
                        ),
                        -memory_score,
                        -unexplored_score,
                        d != facing,
                        direction_to_index(d)
                    );

                } else {
                    return std::make_tuple(
                        danger(
                            moves[d],
                            others
                        ),
                        0,  // to make the tuples the same
                        Search::pearl_dist_astar(
                            moves[d],
                            targets,
                            fatal_zone,
                            *deadline,
                            true,
                            target_positions
                        ),
                        d != facing,
                        direction_to_index(d)
                    );
                }
            };

            auto best_s = score(best_d);

            for (auto& kv : moves) {
                auto s = score(kv.first);

                if (s < best_s) {
                    best_s = s;
                    best_d = kv.first;
                }
            }
        }

        if (targets.empty() && !unknown_portals.empty()) {
            unswbc::Direction portal_choice =
                *unknown_portals.begin();

            double best_portal_danger = 1e9;
            int best_portal_facing = 2;
            int best_portal_index = 100;

            for (auto [portal_dir, portal_id] : portal_options) {
                if (!unknown_portals.count(portal_dir)) {
                    continue;
                }

                unswbc::Position entrance =
                    here.add_dir(portal_dir);

                // Defensive bots should still avoid an obviously
                // dangerous portal entrance if another is available.
                double portal_danger =
                    danger(entrance, others);

                int facing_penalty =
                    (portal_dir != facing);

                int dir_index =
                    direction_to_index(portal_dir);

                if (
                    portal_danger < best_portal_danger ||
                    (
                        portal_danger == best_portal_danger &&
                        facing_penalty < best_portal_facing
                    ) ||
                    (
                        portal_danger == best_portal_danger &&
                        facing_penalty == best_portal_facing &&
                        dir_index < best_portal_index
                    )
                ) {
                    portal_choice = portal_dir;
                    best_portal_danger = portal_danger;
                    best_portal_facing = facing_penalty;
                    best_portal_index = dir_index;
                }
            }

            for (auto [portal_dir, portal_id] : portal_options) {
                if (portal_dir == portal_choice) {
                    Memory::remember_portal_crossing(
                        portal_id,
                        round_num
                    );

                    return portal_dir;
                }
            }
        }

        return best_d;
    }
}