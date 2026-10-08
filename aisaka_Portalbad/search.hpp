#pragma once

#include "helper.hpp"
#include <deque>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <optional>
#include <algorithm>
#include <chrono>
#include <queue>


namespace Search {
    /* ==============================================================
        Search constants
    ===============================================================*/

    const int CLUSTER_RADIUS = 2;
    const int CLUSTER_MIN_SIZE = 6;
    int DANGER_WEIGHT = 0.1;

    /* ===============================================================
        Constructors
    ==================================================================*/

    enum class SearchMode {
        SAFE,
        AGGRESSIVE,
        QUEEN
    };  

    struct PathValidationResult {
        bool valid = true;
        std::optional<unswbc::Position> bad_node;
    };

    struct DeadEndResult {
        bool is_dead_end = false;
        int pearl_count = 0;
    };

    struct SearchNeighbor {
    unswbc::Position position;
    unswbc::Position from;
    unswbc::Direction direction;
    int cost;
    bool through_portal;
    int portal_id = -1;
    };

    struct AStarNode {
        int f, g, counter;
        unswbc::Position pos;
        bool operator>(const AStarNode& o) const {
            if (f != o.f) return f > o.f;
            if (g != o.g) return g > o.g;
            return counter > o.counter;
        }
    };

    struct PathResult {
        unswbc::Position target;
        std::vector<unswbc::Direction> directions;
        std::vector<unswbc::Position> positions;
        int cost = 0;
    };

    /*=============================================================
        Portals handling
    ==============================================================*/
    unswbc::Position portal_exit_position(
        uint64_t key,
        unswbc::Direction entry_direction
    ) {
        int x = static_cast<int>(key >> 32);
        int y = static_cast<int>((key >> 1) & 0x7FFFFFFF);

        unswbc::Position portal_pos(x, y);

        if (entry_direction == unswbc::Direction::EAST ||
            entry_direction == unswbc::Direction::SOUTH) {
            return portal_pos;
        }

        return portal_pos.add_dir(entry_direction);
    }


    bool get_portal_destination(
        unswbc::Position pos,
        unswbc::Direction d,
        unswbc::Position& destination
    ) {
        uint64_t key = Memory::edge_key(pos, d);

        auto edge_it = State::PORTAL_EDGES.find(key);

        if (edge_it == State::PORTAL_EDGES.end())
            return false;

        int portal_id = edge_it->second;

        auto portal_it = State::PORTALS.find(portal_id);

        if (portal_it == State::PORTALS.end())
            return false;

        const auto& endpoints = portal_it->second;

        // We need both ends of the portal pair.
        if (endpoints.size() != 2)
            return false;

        uint64_t other_key;

        if (endpoints[0] == key)
            other_key = endpoints[1];
        else if (endpoints[1] == key)
            other_key = endpoints[0];
        else
            return false;

        destination = portal_exit_position(
            other_key,
            d
        );

        return true;
    }
    
    std::vector<std::pair<unswbc::Direction, int>> portal_directions(unswbc::Position pos, int round_num) {
        std::vector<std::pair<unswbc::Direction, int>> options;
        auto* tile = unswbc::ct->get_tile(pos);
        if (!tile) return options;
        int cooldown = 0;
        for (auto d : unswbc::Direction::get_direction_list()) {
            auto edge = tile->get_edge(d);
            if (edge.get_edge_type() != unswbc::EdgeType::PORTAL) continue;
            int pid = edge.get_portal_id();
            if (pid == State::LAST_PORTAL_ID && round_num - State::LAST_PORTAL_ROUND < cooldown) continue;
            options.push_back({d, pid});
        }
        return options;
    }

    /* ========================================================================================
        Helper functions: dist, tile_ever_seen, open_step, get_neighbors, get_known neighbors
    =========================================================================================*/

    int dist(unswbc::Position a, unswbc::Position b) { // manhattan distance
        int w = State::WIDTH, h = State::HEIGHT;
        int dx = std::abs(a.x - b.x); dx = std::min(dx, w - dx);
        int dy = std::abs(a.y - b.y); dy = std::min(dy, h - dy);
        return dx + dy;
    }

    bool tile_ever_seen(unswbc::Position pos) {
        if (unswbc::ct->get_tile(pos)) return true;
        return State::KNOWN_EDGES.count(pos) > 0;
    }

    std::optional<unswbc::Position> open_step(
        unswbc::Position pos,
        unswbc::Direction d
    ) {
        auto* tile = unswbc::ct->get_tile(pos);

        if (!tile)
            return std::nullopt;

        auto edge_type = tile->get_edge(d).get_edge_type();

        // Kelp / blocked edge
        if (edge_type == unswbc::EdgeType::KELP)
            return std::nullopt;

        // Portal
        if (edge_type == unswbc::EdgeType::PORTAL) {
            unswbc::Position destination;

            if (!get_portal_destination(pos, d, destination))
                return std::nullopt;

            auto* exit_tile = unswbc::ct->get_tile(destination);

            if (!exit_tile || exit_tile->get_dragon())
                return std::nullopt;

            return destination;
        }

        // Normal empty edge
        if (edge_type != unswbc::EdgeType::EMPTY)
            return std::nullopt;

        unswbc::Position nxt = pos.add_dir(d);

        auto* ahead = unswbc::ct->get_tile(nxt);

        if (!ahead || ahead->get_dragon())
            return std::nullopt;

        return nxt;
    }

    std::vector<SearchNeighbor> get_neighbors(
        unswbc::Position pos,
        int round_num
    ) {
        std::vector<SearchNeighbor> result;

        for (auto d : unswbc::Direction::get_direction_list()) {

            auto edge_type = Memory::known_edge_type(pos, d);

            if (edge_type == unswbc::EdgeType::KELP)
                continue;

            if (edge_type == unswbc::EdgeType::EMPTY) {
                result.push_back({
                    pos.add_dir(d),
                    pos,
                    d,
                    1,
                    false,
                    -1
                });
                continue;
            }

            unswbc::Position destination;

            if (get_portal_destination(pos, d, destination)) {

                auto edge_key = Memory::edge_key(pos, d);
                int portal_id = State::PORTAL_EDGES.at(edge_key);

                result.push_back({
                    destination,
                    pos,
                    d,
                    1,
                    true,
                    portal_id
                });
            }
        }

        return result;
    }

    int danger_score(
        const unswbc::Position& pos,
        const std::vector<unswbc::DragonPart*>& others
    ) {
        int best_distance = INT_MAX;

        const auto my_team =
            unswbc::ct->get_team();

        for (const auto* part : others) {
            if (!part)
                continue;

            if (!part->is_head() ||
                part->get_team() == my_team) {
                continue;
            }

            best_distance = std::min(
                best_distance,
                dist(
                    pos,
                    part->get_position()
                )
            );
        }

        if (best_distance == INT_MAX)
            return 0;

        if (best_distance > State::SAFE_DANGER_RADIUS)
            return 0;

        return State::SAFE_DANGER_RADIUS
            - best_distance
            + 1;
}

    std::vector<SearchNeighbor> get_known_neighbors (  // used for search, cant risk on other stuff
        unswbc::Position pos,
        int round_num
    ) {
        std::vector<SearchNeighbor> result;

        for (auto d : unswbc::Direction::get_direction_list()) {

            if (!Memory::edge_is_known(pos, d))
                continue;

            auto edge_type = Memory::known_edge_type(pos, d);

            if (edge_type == unswbc::EdgeType::KELP)
                continue;

            if (edge_type == unswbc::EdgeType::EMPTY) {
                result.push_back({
                    pos.add_dir(d),
                    pos,
                    d,
                    1,
                    false,
                    -1
                });
                continue;
            }

            unswbc::Position destination;

            if (get_portal_destination(pos, d, destination)) {

                auto edge_key = Memory::edge_key(pos, d);
                int portal_id = State::PORTAL_EDGES.at(edge_key);

                result.push_back({
                    destination,
                    pos,
                    d,
                    1,
                    true,
                    portal_id
                });
            }
        }

        return result;
    }

    /* ======================================================================
     Fatal Zone + Safe zone
    =======================================================================*/

    std::unordered_set<
        unswbc::Position,
        unswbc::PositionHash
    > build_safe_zone(const std::vector<unswbc::DragonPart*>& others) {
        std::unordered_set<
            unswbc::Position,
            unswbc::PositionHash
        > zone;

        unswbc::Team my_team = unswbc::ct->get_team();

        for (auto part : others) {
            if (!part->is_head() || part->get_team() == my_team)
                continue;

            unswbc::Position enemy = part->get_position();

            zone.insert(enemy);

            for (int dx = -State::SAFE_DANGER_RADIUS;
                dx <= State::SAFE_DANGER_RADIUS;
                ++dx) {

                for (int dy = -State::SAFE_DANGER_RADIUS;
                    dy <= State::SAFE_DANGER_RADIUS;
                    ++dy) {

                    if (std::abs(dx) + std::abs(dy)
                        > State::SAFE_DANGER_RADIUS)
                        continue;

                    unswbc::Position p(
                        (enemy.x + dx + State::WIDTH) % State::WIDTH,
                        (enemy.y + dy + State::HEIGHT) % State::HEIGHT
                    );

                    zone.insert(p);
                }
            }
        }

        return zone;
    }

    std::unordered_set<
        unswbc::Position,
        unswbc::PositionHash
    > build_queen_safe_zone(const std::vector<unswbc::DragonPart*>& others) {
        std::unordered_set<
            unswbc::Position,
            unswbc::PositionHash
        > zone;

        unswbc::Team my_team = unswbc::ct->get_team();

        for (auto part : others) {
            if (!part->is_head() || part->get_team() == my_team)
                continue;

            unswbc::Position enemy = part->get_position();
            zone.insert(enemy);

            for (int dx = -State::QUEEN_SAFE_DANGER_RADIUS;
                dx <= State::QUEEN_SAFE_DANGER_RADIUS;
                ++dx) {

                for (int dy = -State::QUEEN_SAFE_DANGER_RADIUS;
                    dy <= State::QUEEN_SAFE_DANGER_RADIUS;
                    ++dy) {

                    if (std::abs(dx) + std::abs(dy)
                        > State::QUEEN_SAFE_DANGER_RADIUS)
                        continue;

                    unswbc::Position p(
                        (enemy.x + dx + State::WIDTH) % State::WIDTH,
                        (enemy.y + dy + State::HEIGHT) % State::HEIGHT
                    );

                    zone.insert(p);
                }
            }
        }

        return zone;
    }

    std::unordered_set<unswbc::Position, unswbc::PositionHash> build_fatal_zone(const std::vector<unswbc::DragonPart*>& others) {
        std::unordered_set<unswbc::Position, unswbc::PositionHash> zone;
        unswbc::Team my_team = unswbc::ct->get_team();
        for (auto* part : others) {
            if (!part->is_head() || part->get_team() == my_team) continue;
            unswbc::Position p = part->get_position();
            zone.insert(p);
            for (auto d : unswbc::Direction::get_direction_list()) zone.insert(p.add_dir(d));
        }
        return zone;
    }

    bool is_fatal(unswbc::Position pos, const std::unordered_set<unswbc::Position, unswbc::PositionHash>& fatal_zone) {
        return fatal_zone.count(pos) > 0;
    }

    /* =======================================================================
     gather targets: terrain_reachable, gather_pearl_targets
    ======================================================================= */

    std::unordered_set<unswbc::Position, unswbc::PositionHash> 
        terrain_reachable(
            unswbc::Position start,
            int cap,
            const std::unordered_set<unswbc::Position, unswbc::PositionHash>& targets
        ) {
    
        std::unordered_set<unswbc::Position, unswbc::PositionHash> seen = {start};

        std::unordered_set<unswbc::Position, unswbc::PositionHash> remaining = targets;

        remaining.erase(start);

        if (remaining.empty() && !targets.empty())
            return seen;

        int counter = 0;

        std::priority_queue<
            AStarNode,
            std::vector<AStarNode>,
            std::greater<AStarNode>
        > heap;

        heap.push({0, 0, counter, start});

        std::unordered_set<unswbc::Position, unswbc::PositionHash> visited;

        int expanded = 0;

        while (!heap.empty() && expanded < cap) {

            auto node = heap.top();
            heap.pop();

            if (visited.count(node.pos))
                continue;

            visited.insert(node.pos);
            expanded++;

            for (auto neighbour : get_known_neighbors(
                node.pos,
                unswbc::game->get_round_num()
            )) {

                unswbc::Position nxt = neighbour.position;

                if (seen.count(nxt))
                    continue;

                seen.insert(nxt);

                if (!targets.empty()) {
                    remaining.erase(nxt);

                    if (remaining.empty())
                        return seen;
                }

                if (!tile_ever_seen(nxt))
                    continue;

                counter++;

                int new_g = node.g + neighbour.cost;

                heap.push({
                    new_g,
                    new_g,
                    counter,
                    nxt
                });
            }
        }

        return seen;
    }

    std::vector<std::pair<unswbc::Position, int>> gather_pearl_targets(unswbc::Position here, const std::vector<unswbc::Tile>& tiles) {
        std::vector<std::pair<unswbc::Position, int>> candidates;
        for (const auto& t : tiles) {
            if (t.has_pearl()) {
                candidates.push_back({t.get_position(), 0});
                continue;
            }
            int pt = t.get_pearl_time();
            if (0 <= pt && pt <= State::PEARL_SPAWN_HORIZON) {
                if (!t.get_dragon()) candidates.push_back({t.get_position(), pt});
            }
        }
        if (candidates.empty()) return {};

        std::unordered_set<unswbc::Position, unswbc::PositionHash> c_pos;
        for (auto& c : candidates) c_pos.insert(c.first);
        auto reachable = terrain_reachable(here, State::BFS_CAP, c_pos);

        std::vector<std::pair<unswbc::Position, int>> targets;
        for (auto& c : candidates) {
            if (reachable.count(c.first)) {
                State::PEARL_CAPABLE.insert(c.first);
                targets.push_back(c);
            }
        }
        return targets;
    }

    /* ========================================================
        PEARL CLUSTER DETECTION
    =========================================================*/

    std::vector<std::pair<unswbc::Position, int>> find_pearl_cluster(const std::vector<std::pair<unswbc::Position, int>>& targets) {
        if (targets.size() < static_cast<size_t>(CLUSTER_MIN_SIZE)) return {};
        std::vector<unswbc::Position> positions;
        for (auto& t : targets) positions.push_back(t.first);

        std::vector<std::pair<unswbc::Position, int>> best_group;
        for (auto& p : positions) {
            std::vector<std::pair<unswbc::Position, int>> group;
            for (size_t i = 0; i < targets.size(); i++) {
                if (dist(p, positions[i]) <= CLUSTER_RADIUS) {
                    group.push_back(targets[i]);
                }
            }
            if (group.size() >= static_cast<size_t>(CLUSTER_MIN_SIZE)) {
                if (best_group.empty() || group.size() > best_group.size()) {
                    best_group = group;
                }
            }
        }
        return best_group.empty() ? targets : best_group;
    }
    /*===============================================================================
        Pathfinding helpers
    ===============================================================================*/
    bool valid_transition(
        const SearchNeighbor& neighbour,
        SearchMode mode,
        const std::unordered_set<
            unswbc::Position,
            unswbc::PositionHash
        >& fatal_zone,
        const std::unordered_set<
            unswbc::Position,
            unswbc::PositionHash
        >& safe_zone,
        const std::unordered_set<
            unswbc::Position,
            unswbc::PositionHash
        >& blocked,
        int round_num
    ) {
        unswbc::Position nxt = neighbour.position;

        // 1. Already blocked during this turn
        if (blocked.count(nxt))
            return false;

        // 2. Known occupied tile
        if (Memory::known_occupied(nxt))
            return false;

        // 3. Hard danger: the Queen never enters fatal danger.
        if ((mode == SearchMode::SAFE || mode == SearchMode::QUEEN) &&
            is_fatal(nxt, fatal_zone))
            return false;

        // 4. Extra danger radius: SAFE and QUEEN avoid their conservative
        // danger zones.
        if ((mode == SearchMode::SAFE || mode == SearchMode::QUEEN) &&
            is_fatal(nxt, safe_zone))
            return false;

        // 5. Queen portal rule: portals are never part of normal Queen
        // pathfinding. They are only considered by the final emergency
        // move fallback when no ordinary move is available.
        if (mode == SearchMode::QUEEN && neighbour.through_portal)
            return false;

        // 6. Portal cooldown
        if (neighbour.through_portal) {
            int cooldown = 0;

            if (neighbour.portal_id == State::LAST_PORTAL_ID &&
                round_num - State::LAST_PORTAL_ROUND < cooldown) {
                return false;
            }
        }

        return true;
    }


    /* ===========================================================
        PATHFINDING: 
    ============================================================*/

    int pearl_dist(unswbc::Position pos, const std::vector<std::pair<unswbc::Position, int>>& targets) { // timeout fallback, uses manhanttan dist to pearls for best move
        if (targets.empty()) return 0;
        int best = 100000;
        for (auto& t : targets) best = std::min(best, std::max(dist(pos, t.first), t.second));
        return best;
    } 

    /* ================================================================
        TARGET PATHFINDING
    ================================================================ */
    std::optional<PathResult> dijkstra_targets(
        unswbc::Position here,
        const std::vector<std::pair<unswbc::Position, int>>& targets,
        SearchMode mode,
        const std::unordered_set<unswbc::Position, unswbc::PositionHash>& fatal_zone,
        const std::unordered_set<unswbc::Position, unswbc::PositionHash>& safe_zone,
        const std::unordered_set<unswbc::Position, unswbc::PositionHash>& blocked,
        int cap,
        double* deadline,
        int round_num
    ) {
        if (targets.empty())
            return std::nullopt;

        struct Parent {
            unswbc::Position previous;
            unswbc::Direction direction;

            Parent(
                unswbc::Position previous,
                unswbc::Direction direction
            )
                : previous(previous),
                direction(direction)
            {}
        };

        std::unordered_map<unswbc::Position, int, unswbc::PositionHash> target_penalty;

        for (const auto& [position, penalty] : targets) {
            auto it = target_penalty.find(position);

            if (it == target_penalty.end()) {
                target_penalty[position] = penalty;
            } else {
                it->second = std::min(it->second, penalty);
            }
        }

        std::priority_queue<
            AStarNode,
            std::vector<AStarNode>,
            std::greater<AStarNode>
        > open;

        std::unordered_map<unswbc::Position, int, unswbc::PositionHash> distance;

        std::unordered_map<
            unswbc::Position,
            Parent,
            unswbc::PositionHash
        > came_from;

        std::unordered_set<unswbc::Position, unswbc::PositionHash> visited;

        int counter = 0;

        distance[here] = 0;
        open.push({0, 0, counter++, here});

        int best_cost = INT_MAX;
        std::vector<unswbc::Position> best_targets;

        while (!open.empty()) {

            // Deadline check
            if (deadline != nullptr) {
                double now = std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch()
                ).count();

                if (now >= *deadline) {
                    break;
                }
            }

            AStarNode current = open.top();
            open.pop();

            if (visited.count(current.pos))
                continue;

            visited.insert(current.pos);

            // Best-cost early termination.
            // Dijkstra pops nodes in non-decreasing g order.
            // effective_cost = max(g, penalty) >= g.
            if (best_cost != INT_MAX && current.g > best_cost) {
                break;
            }

            // Target reached
            auto target_it = target_penalty.find(current.pos);

            if (target_it != target_penalty.end()) {
                int effective_cost = std::max(
                    current.g,
                    target_it->second
                );

                if (effective_cost < best_cost) {
                    best_cost = effective_cost;
                    best_targets.clear();
                    best_targets.push_back(current.pos);
                } else if (effective_cost == best_cost) {
                    best_targets.push_back(current.pos);
                }
            }

            // Cap check
            if (static_cast<int>(visited.size()) >= cap) {
                break;
            }

            // Expand neighbours
            auto neighbours = get_known_neighbors(
                current.pos,
                round_num
            );

            for (const auto& neighbour : neighbours) {

                if (!valid_transition(
                        neighbour,
                        mode,
                        fatal_zone,
                        safe_zone,
                        blocked,
                        round_num
                    )) {
                    continue;
                }

                const auto next = neighbour.position;

                if (visited.count(next))
                    continue;

                const int new_distance =
                    current.g + neighbour.cost;

                auto distance_it = distance.find(next);

                if (distance_it != distance.end() &&
                    new_distance >= distance_it->second) {
                    continue;
                }

                distance[next] = new_distance;

                came_from.insert_or_assign(
                    next,
                    Parent{
                        current.pos,
                        neighbour.direction
                    }
                );

                open.push({
                    new_distance,
                    new_distance,
                    counter++,
                    next
                });
            }
        }

        if (best_targets.empty()) {
            return std::nullopt;
        }

        const auto selected_target = best_targets.front();

        PathResult result;

        result.target = selected_target;
        result.cost = best_cost;

        // Reconstruct path
        auto current = selected_target;

        while (current != here) {

            auto parent_it = came_from.find(current);

            if (parent_it == came_from.end()) {
                return std::nullopt;
            }

            const auto& parent = parent_it->second;

            result.directions.push_back(parent.direction);
            result.positions.push_back(current);

            current = parent.previous;
        }

        std::reverse(
            result.directions.begin(),
            result.directions.end()
        );

        std::reverse(
            result.positions.begin(),
            result.positions.end()
        );

        return result;
    }

    std::optional<PathResult> find_physical_path(
        unswbc::Position here,
        const std::vector<std::pair<unswbc::Position, int>>& targets,
        const std::vector<unswbc::DragonPart*>& others,
        const std::unordered_set<
            unswbc::Position,
            unswbc::PositionHash
        >& blocked,
        int cap,
        double* deadline,
        int round_num
    ) {
        if (targets.empty())
            return std::nullopt;

        /*
        * Search state.
        *
        * sprint_steps tells us how many movement tiles have already
        * been traversed during the current sprint.
        *
        * It is capped at moves_this_turn because danger only applies
        * to the tiles actually traversed during this turn.
        */
        struct PhysicalState {
            unswbc::Position pos;
            int sprint_steps;

            bool operator==(const PhysicalState& other) const {
                return pos == other.pos &&
                    sprint_steps == other.sprint_steps;
            }
        };

        struct PhysicalStateHash {
            std::size_t operator()(
                const PhysicalState& state
            ) const {
                return unswbc::PositionHash()(state.pos)
                    ^ (
                        static_cast<std::size_t>(
                            state.sprint_steps
                        ) << 16
                    );
            }
        };

        /*
        * Parent information for reconstructing the path.
        */
        struct Parent {
            PhysicalState previous;
            unswbc::Direction direction;

            Parent(
                PhysicalState previous,
                unswbc::Direction direction
            )
                : previous(previous),
                direction(direction)
            {}
        };

        /*
        * Priority queue node.
        *
        * score =
        *     path length
        *   + weighted sprint danger
        */
        struct PhysicalNode {
            int score;
            int path_length;
            int sprint_steps;
            int counter;
            unswbc::Position pos;

            bool operator>(
                const PhysicalNode& other
            ) const {
                if (score != other.score)
                    return score > other.score;

                if (path_length != other.path_length)
                    return path_length > other.path_length;

                return counter > other.counter;
            }
        };

        /*
        * Preserve the existing target penalty behaviour.
        */
        std::unordered_map<
            unswbc::Position,
            int,
            unswbc::PositionHash
        > target_penalty;

        for (const auto& [position, penalty] : targets) {

            auto it =
                target_penalty.find(position);

            if (it == target_penalty.end()) {

                target_penalty[position] =
                    penalty;

            } else {

                it->second =
                    std::min(
                        it->second,
                        penalty
                    );
            }
        }

        /*
        * Number of tiles this dragon actually traverses
        * during the current turn.
        */
        const int moves_this_turn =
            (unswbc::ct->get_length() + 3) / 4;

        std::priority_queue<
            PhysicalNode,
            std::vector<PhysicalNode>,
            std::greater<PhysicalNode>
        > open;

        /*
        * Best score for each
        * (position, sprint progress) state.
        */
        std::unordered_map<
            PhysicalState,
            int,
            PhysicalStateHash
        > distance;

        /*
        * Parent map for path reconstruction.
        */
        std::unordered_map<
            PhysicalState,
            Parent,
            PhysicalStateHash
        > came_from;

        /*
        * States that have already been fully expanded.
        */
        std::unordered_set<
            PhysicalState,
            PhysicalStateHash
        > visited;

        int counter = 0;

        PhysicalState start{
            here,
            0
        };

        distance[start] = 0;

        open.push({
            0,      // score
            0,      // path_length
            0,      // sprint_steps
            counter++,
            here
        });

        int best_cost = INT_MAX;

        std::optional<PhysicalState>
            best_target_state;

        while (!open.empty()) {

            /*
            * Time limit.
            */
            if (deadline != nullptr) {

                double now =
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now()
                            .time_since_epoch()
                    ).count();

                if (now >= *deadline)
                    break;
            }

            PhysicalNode current =
                open.top();

            open.pop();

            PhysicalState current_state{
                current.pos,
                current.sprint_steps
            };

            /*
            * We can reach the same state multiple times through
            * different queue entries. Only process it once.
            */
            if (visited.count(current_state))
                continue;

            visited.insert(current_state);

            /*
            * Since all future costs are non-negative, once the
            * current score exceeds our best target score, there
            * cannot be a better result later in the queue.
            */
            if (best_cost != INT_MAX &&
                current.score > best_cost) {
                break;
            }

            /*
            * Check whether this position is a target.
            */
            auto target_it =
                target_penalty.find(
                    current.pos
                );

            if (target_it != target_penalty.end()) {

                /*
                * current.score consists of:
                *
                *     path length
                *   + sprint danger
                *
                * Extract the danger component so that the existing
                * max(path_length, target_penalty) behaviour is kept.
                */
                const int sprint_danger_cost =
                    current.score
                    - current.path_length;

                const int base_cost =
                    std::max(
                        current.path_length,
                        target_it->second
                    );

                /*
                * Target danger is evaluated separately.
                */
                const int target_danger =
                    danger_score(
                        current.pos,
                        others
                    );

                const int effective_cost =
                    base_cost
                    + sprint_danger_cost
                    + DANGER_WEIGHT * target_danger;

                if (effective_cost < best_cost) {

                    best_cost =
                        effective_cost;

                    best_target_state =
                        current_state;
                }
            }

            /*
            * Search cap.
            */
            if (
                static_cast<int>(
                    visited.size()
                ) >= cap
            ) {
                break;
            }

            auto neighbours =
                get_known_neighbors(
                    current.pos,
                    round_num
                );

            for (const auto& neighbour : neighbours) {

                const auto next =
                    neighbour.position;

                /*
                * Physical search still cannot move through
                * known occupied tiles.
                */
                if (
                    Memory::known_occupied(
                        next
                    )
                ) {
                    continue;
                }

                /*
                * Don't retry previously rejected nodes.
                */
                if (blocked.count(next))
                    continue;

                /*
                * Calculate sprint progress.
                */
                const int new_sprint_steps =
                    std::min(
                        current.sprint_steps
                            + neighbour.cost,
                        moves_this_turn
                    );

                PhysicalState next_state{
                    next,
                    new_sprint_steps
                };

                if (visited.count(next_state))
                    continue;

                /*
                * Normal movement cost.
                */
                const int new_path_length =
                    current.path_length
                    + neighbour.cost;

                int new_score =
                    current.score
                    + neighbour.cost;

                /*
                * Only apply danger while traversing tiles
                * during the current sprint.
                *
                * Once sprint_steps reaches moves_this_turn,
                * subsequent path tiles don't receive danger cost.
                */
                if (
                    current.sprint_steps
                    < moves_this_turn
                ) {

                    new_score +=
                        DANGER_WEIGHT
                        * danger_score(
                            next,
                            others
                        );
                }

                /*
                * Only keep the best-scoring route to this
                * particular (position, sprint progress) state.
                */
                auto distance_it =
                    distance.find(
                        next_state
                    );

                if (
                    distance_it != distance.end() &&
                    new_score >= distance_it->second
                ) {
                    continue;
                }

                distance[next_state] =
                    new_score;

                came_from.insert_or_assign(
                    next_state,
                    Parent{
                        current_state,
                        neighbour.direction
                    }
                );

                open.push({
                    new_score,
                    new_path_length,
                    new_sprint_steps,
                    counter++,
                    next
                });
            }
        }

        /*
        * No target was reached.
        */
        if (!best_target_state.has_value())
            return std::nullopt;

        /*
        * Reconstruct path from the target STATE.
        *
        * We cannot reconstruct using only Position because
        * sprint progress is part of the search state.
        */
        PathResult result;

        result.target =
            best_target_state->pos;

        result.cost =
            best_cost;

        PhysicalState current =
            *best_target_state;

        while (current.pos != here) {

            auto parent_it =
                came_from.find(
                    current
                );

            if (
                parent_it ==
                came_from.end()
            ) {
                return std::nullopt;
            }

            const auto& parent =
                parent_it->second;

            result.directions.push_back(
                parent.direction
            );

            result.positions.push_back(
                current.pos
            );

            current =
                parent.previous;
        }

        std::reverse(
            result.directions.begin(),
            result.directions.end()
        );

        std::reverse(
            result.positions.begin(),
            result.positions.end()
        );

        return result;
    }


    /* ============================================================
       PATHFINDING IF WE SEE NO PEARLS: nearest_unexplored
    =============================================================*/

    std::optional<unswbc::Position> nearest_unexplored(
        unswbc::Position start,
        int round_num
    ) {
        struct Node {
            unswbc::Position pos;
            int distance;
        };

        /*
        * First pass:
        * Search locally for tiles that we can already see but have
        * never physically visited.
        *
        * Vision radius is 3, so this is intentionally small.
        * This helps the bot leave spawn quickly when the exit is
        * already visible.
        */
        constexpr int LOCAL_SEARCH_DEPTH = 3;

        std::deque<Node> local_queue;

        std::unordered_set<
            unswbc::Position,
            unswbc::PositionHash
        > local_seen;

        local_queue.push_back({start, 0});
        local_seen.insert(start);

        std::optional<unswbc::Position> best_local_target;
        int best_local_distance = INT_MAX;
        int best_local_frontier = -1;

        while (!local_queue.empty()) {
            Node current = local_queue.front();
            local_queue.pop_front();

            if (current.distance >= LOCAL_SEARCH_DEPTH)
                continue;

            for (const auto& neighbour :
                get_neighbors(current.pos, round_num)) {

                unswbc::Position nxt = neighbour.position;

                if (local_seen.count(nxt))
                    continue;

                local_seen.insert(nxt);

                if (Memory::known_occupied(nxt))
                    continue;

                int next_distance =
                    current.distance + neighbour.cost;

                /*
                * We have never physically visited this tile.
                *
                * Score how much unexplored territory surrounds it.
                * This helps prefer an actual exit/frontier over some
                * random visible tile sitting in the middle of spawn.
                */
                if (!State::EXPLORATION_VISITED.count(nxt)) {

                    int frontier = 0;

                    for (auto direction :
                        unswbc::Direction::get_direction_list()) {

                        if (Memory::known_edge_type(nxt, direction) ==
                            unswbc::EdgeType::KELP) {
                            continue;
                        }

                        unswbc::Position beyond =
                            nxt.add_dir(direction);

                        if (Memory::known_occupied(beyond))
                            continue;

                        // Strong signal: this tile borders genuinely
                        // unseen territory.
                        if (!tile_ever_seen(beyond)) {
                            frontier += 10;
                        }
                        // Weaker signal: this tile borders territory
                        // we still haven't physically visited.
                        else if (!State::EXPLORATION_VISITED.count(beyond)) {
                            frontier += 1;
                        }
                    }

                    /*
                    * Prefer:
                    * 1. Stronger frontier
                    * 2. Shorter distance
                    */
                    if (frontier > best_local_frontier ||
                        (frontier == best_local_frontier &&
                        next_distance < best_local_distance)) {

                        best_local_target = nxt;
                        best_local_frontier = frontier;
                        best_local_distance = next_distance;
                    }
                }

                /*
                * We can continue searching through known territory.
                * This means the local search can find a visible exit
                * even if there are already-visited tiles between us
                * and it.
                */
                if (tile_ever_seen(nxt)) {
                    local_queue.push_back({
                        nxt,
                        next_distance
                    });
                }
            }
        }

        /*
        * If we found a nearby visible-but-unvisited frontier,
        * use it immediately.
        */
        if (best_local_target.has_value())
            return best_local_target;

        /*
        * Second pass:
        * No useful visible frontier nearby, so search more broadly
        * for genuinely unseen territory.
        */
        std::deque<Node> queue;

        std::unordered_set<
            unswbc::Position,
            unswbc::PositionHash
        > seen;

        queue.push_back({start, 0});
        seen.insert(start);

        int expanded = 0;

        while (!queue.empty() &&
            expanded < State::UNEXPLORED_BFS_CAP) {

            Node current = queue.front();
            queue.pop_front();

            ++expanded;

            for (const auto& neighbour :
                get_neighbors(current.pos, round_num)) {

                unswbc::Position nxt = neighbour.position;

                if (seen.count(nxt))
                    continue;

                seen.insert(nxt);

                if (Memory::known_occupied(nxt))
                    continue;

                /*
                * Never select territory we have already physically
                * explored.
                */
                if (State::EXPLORATION_VISITED.count(nxt))
                    continue;

                /*
                * Completely unseen tile.
                */
                if (!tile_ever_seen(nxt))
                    return nxt;

                /*
                * We know this tile, so it is safe to search through it
                * to reach other unexplored territory.
                */
                queue.push_back({
                    nxt,
                    current.distance + neighbour.cost
                });
            }
        }

        return std::nullopt;
    }

    std::vector<std::pair<unswbc::Position, int>> get_frontier_targets(
        unswbc::Position start,
        int round_num
    ) {
        struct Node {
            unswbc::Position pos;
            int distance;
        };

        std::vector<std::pair<unswbc::Position, int>> candidates;

        std::unordered_set<
            unswbc::Position,
            unswbc::PositionHash
        > added;

        /*
        * First pass:
        * Search locally for visible-but-unvisited frontier tiles.
        */
        constexpr int LOCAL_SEARCH_DEPTH = 3;

        std::deque<Node> local_queue;

        std::unordered_set<
            unswbc::Position,
            unswbc::PositionHash
        > local_seen;

        local_queue.push_back({start, 0});
        local_seen.insert(start);

        struct FrontierCandidate {
            unswbc::Position pos;
            int distance;
            int frontier_score;
        };

        std::vector<FrontierCandidate> local_candidates;

        while (!local_queue.empty()) {

            Node current = local_queue.front();
            local_queue.pop_front();

            if (current.distance >= LOCAL_SEARCH_DEPTH)
                continue;

            for (const auto& neighbour :
                get_neighbors(current.pos, round_num)) {

                unswbc::Position nxt = neighbour.position;

                if (local_seen.count(nxt))
                    continue;

                local_seen.insert(nxt);

                if (Memory::known_occupied(nxt))
                    continue;

                int next_distance =
                    current.distance + neighbour.cost;

                if (!State::EXPLORATION_VISITED.count(nxt)) {

                    int frontier_score = 0;

                    for (auto direction :
                        unswbc::Direction::get_direction_list()) {

                        if (Memory::known_edge_type(nxt, direction) ==
                            unswbc::EdgeType::KELP) {
                            continue;
                        }

                        unswbc::Position beyond =
                            nxt.add_dir(direction);

                        if (Memory::known_occupied(beyond))
                            continue;

                        if (!tile_ever_seen(beyond)) {
                            frontier_score += 10;
                        }
                        else if (!State::EXPLORATION_VISITED.count(beyond)) {
                            frontier_score += 1;
                        }
                    }

                    if (frontier_score > 0) {
                        local_candidates.push_back({
                            nxt,
                            next_distance,
                            frontier_score
                        });
                    }
                }

                if (tile_ever_seen(nxt)) {
                    local_queue.push_back({
                        nxt,
                        next_distance
                    });
                }
            }
        }

        /*
        * Sort local candidates:
        *
        * 1. Stronger frontier
        * 2. Shorter distance
        */
        std::sort(
            local_candidates.begin(),
            local_candidates.end(),
            [](const FrontierCandidate& a,
            const FrontierCandidate& b) {

                if (a.frontier_score != b.frontier_score)
                    return a.frontier_score > b.frontier_score;

                return a.distance < b.distance;
            }
        );

        /*
        * Add local candidates first.
        */
        for (const auto& candidate : local_candidates) {

            if (static_cast<int>(candidates.size()) >=
                State::MAX_FRONTIER_TARGETS) {
                break;
            }

            if (added.insert(candidate.pos).second) {
                candidates.push_back({
                    candidate.pos,
                    0
                });
            }
        }

        /*
        * Second pass:
        * Search more broadly for genuinely unseen territory.
        */
        if (static_cast<int>(candidates.size()) <
            State::MAX_FRONTIER_TARGETS) {

            std::deque<Node> queue;

            std::unordered_set<
                unswbc::Position,
                unswbc::PositionHash
            > seen;

            queue.push_back({start, 0});
            seen.insert(start);

            int expanded = 0;

            while (!queue.empty() &&
                expanded < State::UNEXPLORED_BFS_CAP &&
                static_cast<int>(candidates.size()) <
                    State::MAX_FRONTIER_TARGETS) {

                Node current = queue.front();
                queue.pop_front();

                ++expanded;

                for (const auto& neighbour :
                    get_neighbors(current.pos, round_num)) {

                    unswbc::Position nxt =
                        neighbour.position;

                    if (seen.count(nxt))
                        continue;

                    seen.insert(nxt);

                    if (Memory::known_occupied(nxt))
                        continue;

                    if (State::EXPLORATION_VISITED.count(nxt))
                        continue;

                    /*
                    * Completely unseen territory.
                    */
                    if (!tile_ever_seen(nxt)) {

                        if (added.insert(nxt).second) {
                            candidates.push_back({
                                nxt,
                                0
                            });
                        }

                        if (static_cast<int>(candidates.size()) >=
                            State::MAX_FRONTIER_TARGETS) {
                            break;
                        }

                        continue;
                    }

                    /*
                    * Continue searching through known territory.
                    */
                    queue.push_back({
                        nxt,
                        current.distance + neighbour.cost
                    });
                }
            }
        }

        return candidates;
    }

    // ============================================================================
    // PATH VALIDATION
    // ============================================================================

    // ---------------------------------------------------------------------------
    // Get physical neighbours.
    //
    // This deliberately ignores strategic validation rules such as:
    // - SAFE / AGGRESSIVE mode
    // - enemy danger zones
    // - target preference
    // - dead ends
    // - ally blocking
    //
    // It answers only:
    // "Where can I physically move from this position?"
    //
    // KELP       -> cannot move
    // EMPTY      -> move to adjacent tile
    // PORTAL     -> move through portal to its destination
    // occupied   -> cannot move there
    // ---------------------------------------------------------------------------

    std::vector<unswbc::Position> get_physical_neighbors(
        const unswbc::Position& pos,
        int round_num
    ) {
        std::vector<unswbc::Position> result;

        for (const auto& direction :
            unswbc::Direction::get_direction_list()) {

            unswbc::EdgeType edge =
                Memory::known_edge_type(pos, direction);

            // Cannot move through kelp.
            if (edge == unswbc::EdgeType::KELP)
                continue;

            // ---------------------------------------------------------------
            // Normal movement
            // ---------------------------------------------------------------
            if (edge == unswbc::EdgeType::EMPTY) {

                unswbc::Position next =
                    pos.add_dir(direction);

                // Known occupied tile cannot be entered.
                if (Memory::known_occupied(next))
                    continue;

                result.push_back(next);
            }

            // ---------------------------------------------------------------
            // Portal movement
            // ---------------------------------------------------------------
            else if (edge == unswbc::EdgeType::PORTAL) {

                unswbc::Position destination;

                if (!get_portal_destination(
                        pos,
                        direction,
                        destination)) {
                    continue;
                }

                // Check the actual portal destination,
                // not the tile immediately beside the portal.
                if (Memory::known_occupied(destination))
                    continue;

                result.push_back(destination);
            }
        }

        return result;
    }


    // ---------------------------------------------------------------------------
    // Count pearls inside a dead-end pocket.
    //
    // The important distinction here is that we do NOT only count pearls
    // directly along the planned path.
    //
    // Once a dead-end pocket is identified, we flood-fill the physically
    // reachable pocket and count all known pearls inside it.
    //
    // This is used to allow entering a dead end if the pocket contains
    // enough pearls to make the commitment worthwhile.
    // ---------------------------------------------------------------------------

    int count_pocket_pearls(
        const unswbc::Position& entry,
        const unswbc::Position& previous,
        int round_num
    ) {
        std::deque<unswbc::Position> queue;

        std::unordered_set<
            unswbc::Position,
            unswbc::PositionHash
        > seen;

        queue.push_back(entry);
        seen.insert(entry);

        int pearl_count = 0;

        while (!queue.empty()) {

            unswbc::Position current =
                queue.front();

            queue.pop_front();

            // ---------------------------------------------------------------
            // Count known pearl at this position.
            // ---------------------------------------------------------------
            auto pearl_it =
                State::PEARL_MEMORY.find(current);

            if (pearl_it != State::PEARL_MEMORY.end()) {

                const auto& pearl =
                    pearl_it->second;

                // pearl_time == -1 means this position does not have
                // a pearl that will spawn.
                if (pearl.has_pearl &&
                    pearl.pearl_time != -1) {

                    ++pearl_count;
                }
            }

            // ---------------------------------------------------------------
            // Expand through physical space.
            // ---------------------------------------------------------------
            std::vector<unswbc::Position> neighbours =
                get_physical_neighbors(
                    current,
                    round_num
                );

            for (const auto& next : neighbours) {

                // Do not immediately walk back out through the
                // entrance of the pocket.
                if (next == previous)
                    continue;

                if (seen.contains(next))
                    continue;

                seen.insert(next);
                queue.push_back(next);
            }
        }

        return pearl_count;
    }


    // ---------------------------------------------------------------------------
    // Analyse whether entering `entry` commits us to a dead end.
    //
    // We only look ahead a short distance.
    //
    // Example:
    //
    //      previous
    //          |
    //          v
    //       entry
    //          |
    //          v
    //          A
    //          |
    //          v
    //          B
    //          |
    //          v
    //        DEAD END
    //
    // If every step is forced for DEAD_END_LOOKAHEAD steps and eventually
    // there are no exits, we treat it as a dead-end commitment.
    //
    // If the pocket contains enough pearls, the dead end can still be worth
    // entering.
    // ---------------------------------------------------------------------------

    DeadEndResult analyze_dead_end(
        const unswbc::Position& entry,
        const unswbc::Position& previous,
        int round_num
    ) {
        unswbc::Position current = entry;
        unswbc::Position came_from = previous;

        std::unordered_set<
            unswbc::Position,
            unswbc::PositionHash
        > seen;

        seen.insert(current);

        for (int depth = 0;
            depth < State::DEAD_END_LOOKAHEAD;
            ++depth) {

            std::vector<unswbc::Position> neighbours =
                get_physical_neighbors(
                    current,
                    round_num
                );

            // ---------------------------------------------------------------
            // We are only interested in exits other than the position
            // we just came from.
            // ---------------------------------------------------------------
            neighbours.erase(
                std::remove(
                    neighbours.begin(),
                    neighbours.end(),
                    came_from
                ),
                neighbours.end()
            );

            // ---------------------------------------------------------------
            // No forward exit -> genuine dead end.
            // ---------------------------------------------------------------
            if (neighbours.empty()) {

                int pearl_count =
                    count_pocket_pearls(
                        entry,
                        previous,
                        round_num
                    );

                return {
                    true,
                    pearl_count
                };
            }

            // ---------------------------------------------------------------
            // More than one possible forward exit means this isn't a
            // forced corridor anymore.
            // ---------------------------------------------------------------
            if (neighbours.size() > 1) {
                return {
                    false,
                    0
                };
            }

            // ---------------------------------------------------------------
            // Exactly one forward option.
            // Continue following the forced corridor.
            // ---------------------------------------------------------------
            unswbc::Position next =
                neighbours.front();

            // A cycle means we are not dealing with a dead end.
            if (seen.contains(next)) {
                return {
                    false,
                    0
                };
            }

            seen.insert(next);

            came_from = current;
            current = next;
        }

        // We did not encounter a dead end within the lookahead.
        return {
            false,
            0
        };
    }


    // ---------------------------------------------------------------------------
    // Check whether entering `node` would block an ally.
    //
    // `others` comes directly from main.cpp:
    //
    //     auto others = other_dragon_parts(tiles);
    //
    // This function filters that list to teammates only.
    //
    // We only reject the move when:
    //
    //     1. the dragon is an ally
    //     2. node is one of its physical exits
    //     3. node is its ONLY physical exit
    //
    // This deliberately does NOT try to simulate every dragon's future
    // movement. It is simply a local "don't take your teammate's only exit"
    // check.
    // ---------------------------------------------------------------------------

    bool blocks_ally(
        const unswbc::Position& node,
        const std::vector<unswbc::DragonPart*>& others,
        int round_num
    ) {
        for (const auto* dragon : others) {

            if (!dragon)
                continue;

            // Ignore enemies.
            if (dragon->get_team() != unswbc::ct->get_team())
                continue;

            unswbc::Position ally_position =
                dragon->get_position();

            std::vector<unswbc::Position> exits =
                get_physical_neighbors(
                    ally_position,
                    round_num
                );

            // Is our candidate node one of the ally's exits?
            bool uses_node =
                std::find(
                    exits.begin(),
                    exits.end(),
                    node
                ) != exits.end();

            if (!uses_node)
                continue;

            // If this is their only known physical exit,
            // entering it would block them.
            if (exits.size() == 1)
                return true;
        }

        return false;
    }


    std::vector<std::pair<unswbc::Position, int>> get_queen_safe_targets(
        unswbc::Position here,
        const std::vector<unswbc::DragonPart*>& others,
        int round_num
    ) {
        std::vector<std::pair<unswbc::Position, int>> candidates;

        auto fatal_zone = build_fatal_zone(others);
        auto safe_zone = build_queen_safe_zone(others);

        std::deque<unswbc::Position> queue;
        std::unordered_map<unswbc::Position, int, unswbc::PositionHash> distance;
        std::unordered_set<unswbc::Position, unswbc::PositionHash> seen;

        queue.push_back(here);
        distance[here] = 0;
        seen.insert(here);

        while (!queue.empty() &&
               static_cast<int>(seen.size()) < State::QUEEN_SAFE_TARGET_CAP) {
            auto current = queue.front();
            queue.pop_front();

            int current_distance = distance[current];

            if (current != here &&
                !fatal_zone.count(current) &&
                !safe_zone.count(current) &&
                !Memory::known_occupied(current)) {

                if (dist(here, current) >
                    State::QUEEN_MAX_DISTANCE_FROM_HEAD) {
                    continue;
                }

                auto exits = get_physical_neighbors(current, round_num);

                if (!exits.empty() &&
                    !blocks_ally(current, others, round_num)) {

                    int score =
                        static_cast<int>(exits.size()) * 10;

                    if (unswbc::ct->get_tile(current))
                        score += 8;

                    // Prefer positions that are already known and mobile.
                    // Dijkstra interprets lower target penalties as better.
                    int penalty = std::max(1, 40 - score + current_distance);
                    candidates.push_back({current, penalty});
                }
            }

            for (const auto& neighbour : get_known_neighbors(current, round_num)) {
                auto next = neighbour.position;

                if (seen.count(next))
                    continue;

                if (fatal_zone.count(next) || safe_zone.count(next))
                    continue;

                if (Memory::known_occupied(next))
                    continue;

                seen.insert(next);
                distance[next] = current_distance + neighbour.cost;
                queue.push_back(next);
            }
        }

        return candidates;
    }

    // ---------------------------------------------------------------------------
    // Validate only the portion of the path that the dragon will actually
    // execute THIS TURN.
    //
    // The caller should calculate:
    //
    //     moves_this_turn = ceil(length / 4)
    //
    // and pass that value here.
    //
    // For example:
    //
    //     length = 3  -> 1 move
    //     length = 4  -> 1 move
    //     length = 5  -> 2 moves
    //     length = 8  -> 2 moves
    //     length = 20 -> 5 moves
    //
    // We do NOT validate the entire future path because the world state can
    // change before those future moves are actually executed.
    //
    // If something is wrong, return the FIRST bad node.
    //
    // The caller is then responsible for:
    //
    //     block bad node
    //     rerun Dijkstra
    //     validate again
    // ---------------------------------------------------------------------------

    PathValidationResult validate_path(
        const PathResult& path,
        int moves_this_turn,
        int round_num,
        const std::vector<unswbc::DragonPart*>& others,
        SearchMode mode
    ) {
        // Nothing to validate.
        if (path.positions.empty())
            return {};

        int moves_to_validate =
            std::min(
                moves_this_turn,
                static_cast<int>(
                    path.positions.size()
                )
            );

        // PathResult::positions contains destination positions only.
        //
        // Therefore the first `previous` position is our current head.
        unswbc::Position previous =
            unswbc::ct->get_position();

        for (int i = 0;
            i < moves_to_validate;
            ++i) {

            const unswbc::Position& node =
                path.positions[i];

            // ---------------------------------------------------------------
            // 1. Dead-end validation
            // ---------------------------------------------------------------
            DeadEndResult dead_end =
                analyze_dead_end(
                    node,
                    previous,
                    round_num
                );

            // The Queen must NEVER enter an analysed dead end, even if
            // that dead end contains Pearls. Normal dragons keep the
            // existing Pearl-based exception.
            if (mode == SearchMode::QUEEN) {
                if (dead_end.is_dead_end) {
                    return {
                        false,
                        node
                    };
                }
            } else {
                int dead_end_threshold =
                    State::DEAD_END_PEARL_THRESHOLD;

                if (dead_end.is_dead_end &&
                    dead_end.pearl_count <= dead_end_threshold) {

                    return {
                        false,
                        node
                    };
                }
            }

            // ---------------------------------------------------------------
            // 2. Ally blocking validation
            // ---------------------------------------------------------------
            if (blocks_ally(
                    node,
                    others,
                    round_num)) {

                return {
                    false,
                    node
                };
            }

            // This node passed validation.
            previous = node;
        }

        return {};
    }


    std::optional<PathResult> find_valid_path(
        unswbc::Position here,
        const std::vector<std::pair<unswbc::Position, int>>& targets,
        SearchMode mode,
        const std::unordered_set<
            unswbc::Position,
            unswbc::PositionHash
        >& fatal_zone,
        const std::unordered_set<
            unswbc::Position,
            unswbc::PositionHash
        >& safe_zone,
        int cap,
        double* deadline,
        int round_num,
        int moves_this_turn,
        const std::vector<unswbc::DragonPart*>& others
    ) {
        // Nodes rejected by path validation during this search.
        //
        // This is temporary. It only exists for the current attempt
        // to find a valid path.
        std::unordered_set<
            unswbc::Position,
            unswbc::PositionHash
        > blocked;

        while (true) {

            // ---------------------------------------------------------------
            // Check the deadline before starting another Dijkstra search.
            // ---------------------------------------------------------------
            if (deadline != nullptr) {

                double now =
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now()
                            .time_since_epoch()
                    ).count();

                if (now >= *deadline)
                    return std::nullopt;
            }

            // ---------------------------------------------------------------
            // Find the best path while avoiding:
            //
            //   - normal invalid transitions
            //   - fatal/safe zones
            //   - nodes rejected by previous validation attempts
            // ---------------------------------------------------------------
            auto path = dijkstra_targets(
                here,
                targets,
                mode,
                fatal_zone,
                safe_zone,
                blocked,
                cap,
                deadline,
                round_num
            );

            // No preferred path exists.
            if (!path.has_value())
                return std::nullopt;

            // ---------------------------------------------------------------
            // Validate only the part of the path that will actually be
            // executed during this turn.
            // ---------------------------------------------------------------
            PathValidationResult validation =
                validate_path(
                    *path,
                    moves_this_turn,
                    round_num,
                    others,
                    mode
                );
            
            // ---------------------------------------------------------------
            // The path is valid.
            // ---------------------------------------------------------------
            if (validation.valid)
                return path;
                
            // ---------------------------------------------------------------
            // Invalid path but no offending node.
            //
            // This should not normally happen because validate_path()
            // returns the first bad node whenever valid == false.
            // ---------------------------------------------------------------
            if (!validation.bad_node.has_value())
                return std::nullopt;

            // ---------------------------------------------------------------
            // Reject the offending node and rerun Dijkstra.
            // ---------------------------------------------------------------
            blocked.insert(
                *validation.bad_node
            );
        }

        return std::nullopt;
    }

    std::optional<PathResult> find_valid_physical_path(
        unswbc::Position here,
        const std::vector<std::pair<unswbc::Position, int>>& targets,
        int cap,
        double* deadline,
        int round_num,
        int moves_this_turn,
        const std::vector<unswbc::DragonPart*>& others
    ) {
        std::unordered_set<
            unswbc::Position,
            unswbc::PositionHash
        > blocked;

        while (true) {

            if (deadline != nullptr) {
                double now =
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now()
                            .time_since_epoch()
                    ).count();

                if (now >= *deadline)
                    return std::nullopt;
            }

            auto path = find_physical_path(
                here,
                targets,
                others,
                blocked,
                cap,
                deadline,
                round_num
            );

            if (!path.has_value())
                return std::nullopt;

            PathValidationResult validation =
                validate_path(
                    *path,
                    moves_this_turn,
                    round_num,
                    others,
                    State::IS_QUEEN
                        ? SearchMode::QUEEN
                        : (State::AGGRESSIVE
                            ? SearchMode::AGGRESSIVE
                            : SearchMode::SAFE)
                );

            if (validation.valid)
                return path;

            if (!validation.bad_node.has_value())
                return std::nullopt;

            blocked.insert(
                *validation.bad_node
            );
        }

        return std::nullopt;
    }
    
    std::optional<unswbc::Direction> choose_emergency_move(
        unswbc::Position here,
        int round_num,
        const std::vector<unswbc::DragonPart*>& others,
        SearchMode mode = SearchMode::SAFE
    ) {
        std::vector<std::pair<
            unswbc::Direction,
            unswbc::Position
        >> candidates;

        auto fatal_zone = build_fatal_zone(others);
        auto queen_safe_zone = build_queen_safe_zone(others);

        // Queen survival fallback: if every strict Queen move is a detected
        // dead end, keep a second list of otherwise legal moves. This list
        // is only used when there is no strict option, so dead-end avoidance
        // remains the normal rule without allowing the Queen to die merely
        // because the validator rejected every direction.
        std::vector<std::pair<
            unswbc::Direction,
            unswbc::Position
        >> queen_survival_candidates;

        // A portal is a true last-resort escape for the Queen. Keep portal
        // candidates separate so an ordinary legal move always wins first.
        std::vector<std::pair<
            unswbc::Direction,
            unswbc::Position
        >> queen_portal_candidates;

        for (const auto& direction :
            unswbc::Direction::get_direction_list()) {

            // The Queen must never deliberately reverse into her own body.
            if (mode == SearchMode::QUEEN &&
                direction == unswbc::ct->get_dir().get_opposite())
                continue;

            unswbc::EdgeType edge =
                Memory::known_edge_type(here, direction);

            bool is_portal =
                edge == unswbc::EdgeType::PORTAL;

            if (edge == unswbc::EdgeType::KELP)
                continue;

            unswbc::Position next;

            // Normal movement.
            if (edge == unswbc::EdgeType::EMPTY) {
                next = here.add_dir(direction);
            }

            // Portal movement.
            else if (edge == unswbc::EdgeType::PORTAL) {
                if (!get_portal_destination(
                        here,
                        direction,
                        next)) {
                    continue;
                }

                // Respect portal cooldown.
                auto edge_key =
                    Memory::edge_key(here, direction);

                auto portal_it =
                    State::PORTAL_EDGES.find(edge_key);

                if (portal_it == State::PORTAL_EDGES.end())
                    continue;

                int portal_id = portal_it->second;

                int cooldown =
                    0;

                if (portal_id == State::LAST_PORTAL_ID &&
                    round_num - State::LAST_PORTAL_ROUND <
                        cooldown) {
                    continue;
                }
            }

            else {
                continue;
            }

            // Never move into known occupied space.
            if (Memory::known_occupied(next))
                continue;

            // Even the Queen's final survival fallback must not deliberately
            // enter an immediately fatal enemy tile or the Queen's existing
            // conservative danger zone.
            if (mode == SearchMode::QUEEN &&
                (is_fatal(next, fatal_zone) ||
                 is_fatal(next, queen_safe_zone)))
                continue;

            // The Queen must NEVER enter an analysed dead end, regardless
            // of how many Pearls are inside it. Normal dragons retain the
            // existing Pearl-based exception.
            DeadEndResult dead_end =
                analyze_dead_end(
                    next,
                    here,
                    round_num
                );

            bool blocks = blocks_ally(
                next,
                others,
                round_num
            );

            if (mode == SearchMode::QUEEN) {
                if (dead_end.is_dead_end) {
                    // Do not use a detected dead end while any strict move
                    // exists. Keep it only as the final survival fallback.
                    if (!blocks) {
                        queen_survival_candidates.push_back({
                            direction,
                            next
                        });
                    }
                    continue;
                }
            } else if (dead_end.is_dead_end &&
                       dead_end.pearl_count <=
                           State::DEAD_END_PEARL_THRESHOLD) {
                continue;
            }

            // Don't immediately block an ally.
            if (blocks)
                continue;

            if (mode == SearchMode::QUEEN && is_portal) {
                queen_portal_candidates.push_back({
                    direction,
                    next
                });
            } else {
                candidates.push_back({
                    direction,
                    next
                });
            }
        }

        // A Queen should normally never enter a detected dead end. However,
        // refusing to act can be fatal if the game requires an action every
        // turn. If no strict move exists, use the least-bad legal move that
        // still avoids known occupied space, fatal space, and immediate ally
        // blocking.
        if (candidates.empty() &&
            mode == SearchMode::QUEEN &&
            !queen_survival_candidates.empty()) {
            candidates = std::move(queen_survival_candidates);
        }

        // Only after every ordinary move (including the Queen's emergency
        // survival candidates) has failed may a portal be considered.
        if (candidates.empty() &&
            mode == SearchMode::QUEEN &&
            !queen_portal_candidates.empty()) {
            candidates = std::move(queen_portal_candidates);
        }

        if (candidates.empty())
            return std::nullopt;

        // Prefer the move that leaves the most physical mobility.
        int best_score = -1;
        std::optional<unswbc::Direction> best_move;

        for (const auto& [direction, next] : candidates) {

            auto exits =
                get_physical_neighbors(
                    next,
                    round_num
                );

            int score =
                static_cast<int>(exits.size()) * 10;

            // Prefer unexplored territory.
            if (!State::EXPLORATION_VISITED.count(next))
                score += 5;

            if (score > best_score) {
                best_score = score;
                best_move = direction;
            }
        }

        return best_move;
    }
} // search namespace closer