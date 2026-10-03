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

    /* ===============================================================
        Constructors
    ==================================================================*/

    enum class SearchMode {
        SAFE,
        AGGRESSIVE
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

    unswbc::Position portal_exit_position(uint64_t key) {
        int x = static_cast<int>(key >> 32);
        int y = static_cast<int>((key >> 1) & 0x7FFFFFFF);
        return unswbc::Position(x, y);
    }


    bool get_portal_destination(unswbc::Position pos, unswbc::Direction d, unswbc::Position& destination) {

        uint64_t key = Memory::edge_key(pos, d);

        auto edge_it = State::PORTAL_EDGES.find(key);

        if (edge_it == State::PORTAL_EDGES.end())
            return false;

        int portal_id = edge_it->second;

        auto portal_it = State::PORTALS.find(portal_id);

        if (portal_it == State::PORTALS.end())
            return false;

        const auto& endpoints = portal_it->second;

        // We only know where the portal goes if both
        // ends have been discovered.
        if (endpoints.size() != 2)
            return false;

        uint64_t other_key;

        if (endpoints[0] == key)
            other_key = endpoints[1];
        else
            other_key = endpoints[0];

        destination = portal_exit_position(other_key);

        return true;
        }
    
    std::vector<std::pair<unswbc::Direction, int>> portal_directions(unswbc::Position pos, int round_num) {
        std::vector<std::pair<unswbc::Direction, int>> options;
        auto* tile = unswbc::ct->get_tile(pos);
        if (!tile) return options;
        int cooldown = unswbc::ct->get_length();
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

    std::optional<unswbc::Position> open_step(unswbc::Position pos, unswbc::Direction d) {
        auto* tile = unswbc::ct->get_tile(pos);
        if (!tile || tile->get_edge(d).get_edge_type() != unswbc::EdgeType::EMPTY) return std::nullopt;
        unswbc::Position nxt = pos.add_dir(d);
        auto* ahead = unswbc::ct->get_tile(nxt);
        if (!ahead || ahead->get_dragon()) return std::nullopt;
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

            for (auto neighbour : get_neighbors(
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

        // 3. Hard danger: avoid in both modes
        if (is_fatal(nxt, fatal_zone))
            return false;

        // 4. Extra danger radius: avoid only when SAFE
        if (mode == SearchMode::SAFE &&
            is_fatal(nxt, safe_zone))
            return false;

        // 5. Portal cooldown
        if (neighbour.through_portal) {
            int cooldown = unswbc::ct->get_length();

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

    // A* Targets, technically uses dijkstra now
    std::unordered_map<
        unswbc::Position,
        int,
        unswbc::PositionHash
    > 
    astar_targets(
        unswbc::Position here,
        const std::unordered_set<
            unswbc::Position,
            unswbc::PositionHash
        >& fatal_zone,
        const std::vector<unswbc::Position>& target_positions,
        int cap,
        double deadline,
        bool avoid_fatal
    ) {
        std::unordered_map<
            unswbc::Position,
            int,
            unswbc::PositionHash
        > dist_to;

        dist_to[here] = 0;

        if (target_positions.empty())
            return dist_to;

        std::unordered_set<
            unswbc::Position,
            unswbc::PositionHash
        > remaining(
            target_positions.begin(),
            target_positions.end()
        );

        int counter = 0;

        std::priority_queue<
            AStarNode,
            std::vector<AStarNode>,
            std::greater<AStarNode>
        > heap;

        heap.push({0, 0, counter, here});

        std::unordered_set<
            unswbc::Position,
            unswbc::PositionHash
        > visited;

        int timer = State::BFS_DEADLINE_CHECK_EVERY;

        while (
            !heap.empty() &&
            !remaining.empty() &&
            dist_to.size() < static_cast<size_t>(cap)
        ) {
            auto node = heap.top();
            heap.pop();

            if (visited.count(node.pos))
                continue;

            visited.insert(node.pos);

            // We have reached one of our targets.
            remaining.erase(node.pos);

            if (deadline > 0) {
                if (--timer == 0) {
                    double now =
                        std::chrono::duration<double>(
                            std::chrono::steady_clock::now()
                                .time_since_epoch()
                        ).count();

                    if (now >= deadline)
                        break;

                    timer = State::BFS_DEADLINE_CHECK_EVERY;
                }
            }

            for (auto neighbour :
                get_neighbors(
                    node.pos,
                    unswbc::game->get_round_num()
                )) {

                unswbc::Position nxt = neighbour.position;

                if (Memory::known_occupied(nxt))
                    continue;

                if (
                    avoid_fatal &&
                    is_fatal(nxt, fatal_zone)
                )
                    continue;

                int new_g = node.g + neighbour.cost;

                if (
                    dist_to.find(nxt) == dist_to.end() ||
                    new_g < dist_to[nxt]
                ) {
                    dist_to[nxt] = new_g;

                    counter++;

                    heap.push({
                        new_g,
                        new_g,
                        counter,
                        nxt
                    });
                }
            }
        }

        return dist_to;
    }

    int pearl_dist_astar(
        unswbc::Position pos,
        const std::vector<std::pair<unswbc::Position, int>>& targets,
        const std::unordered_set<unswbc::Position, unswbc::PositionHash>& fatal_zone,
        double deadline,
        bool avoid_fatal,
        const std::vector<unswbc::Position>& target_positions
    ) {
        if (targets.empty())
            return 0;

        auto dist_to = astar_targets(
            pos,
            fatal_zone,
            target_positions,
            State::BFS_CAP,
            deadline,
            avoid_fatal
        );

        int best = 100000;

        for (auto& t : targets) {

            // If the target was reached by the portal-aware search,
            // use the actual path distance.
            if (dist_to.count(t.first)) {
                int turns = std::max(
                    dist_to[t.first],
                    t.second
                );

                best = std::min(best, turns);
            }
        }

        return best;
    }

    /* ================================================================
        TARGET PATHFINDING
    ================================================================ */
    std::optional<PathResult> dijkstra_targets(
        unswbc::Position here,
        const std::vector<
            std::pair<unswbc::Position, int>
        >& targets,

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
                :previous(previous),
                direction(direction)
            {}
        };

        // ------------------------------------------------------------
        // Target lookup
        // ------------------------------------------------------------

        std::unordered_map<
            unswbc::Position,
            int,
            unswbc::PositionHash
        > target_penalty;

        for (const auto& [position, penalty] : targets) {

            auto it = target_penalty.find(position);

            // If duplicate positions somehow occur, keep
            // the smaller penalty.
            if (it == target_penalty.end()) {
                target_penalty[position] = penalty;
            } else {
                it->second = std::min(it->second, penalty);
            }
        }

        // ------------------------------------------------------------
        // Dijkstra state
        // ------------------------------------------------------------

        std::priority_queue<
            AStarNode,
            std::vector<AStarNode>,
            std::greater<AStarNode>
        > open;

        std::unordered_map<
            unswbc::Position,
            int,
            unswbc::PositionHash
        > distance;

        std::unordered_map<
            unswbc::Position,
            Parent,
            unswbc::PositionHash
        > came_from;

        std::unordered_set<
            unswbc::Position,
            unswbc::PositionHash
        > visited;

        int counter = 0;

        distance[here] = 0;

        open.push({
            0,      // f == g for Dijkstra
            0,
            counter++,
            here
        });

        // ------------------------------------------------------------
        // Best target found so far
        // ------------------------------------------------------------

        int best_cost = INT_MAX;

        std::vector<unswbc::Position> best_targets;

        // ------------------------------------------------------------
        // Search
        // ------------------------------------------------------------

        while (!open.empty()) {

            if (deadline != nullptr) {
                double now =
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now().time_since_epoch()
                    ).count();

                if (now >= *deadline)
                    break;
            }
            AStarNode current = open.top();
            open.pop();

            // Already finalised
            if (visited.count(current.pos))
                continue;

            visited.insert(current.pos);

            // --------------------------------------------------------
            // Target reached
            // --------------------------------------------------------

            auto target_it =
                target_penalty.find(current.pos);

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

            // --------------------------------------------------------
            // Search cap
            // --------------------------------------------------------

            if (static_cast<int>(visited.size()) >= cap)
                break;

            // --------------------------------------------------------
            // Expand neighbours
            // --------------------------------------------------------

            auto neighbours =
                get_known_neighbors(current.pos, round_num);

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

                auto distance_it =
                    distance.find(next);

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
                    new_distance,   // f
                    new_distance,   // g
                    counter++,
                    next
                });
            }
        }

        // ------------------------------------------------------------
        // No reachable target
        // ------------------------------------------------------------

        if (best_targets.empty())
            return std::nullopt;

        // ------------------------------------------------------------
        // For now, deterministic tie-breaking
        // ------------------------------------------------------------

        const auto selected_target =
            best_targets.front();

        // ------------------------------------------------------------
        // Reconstruct path
        // ------------------------------------------------------------

        PathResult result;

        result.target = selected_target;
        result.cost = best_cost;

        auto current = selected_target;

        while (current != here) {

            auto parent_it =
                came_from.find(current);

            if (parent_it == came_from.end()) {
                return std::nullopt;
            }

            const auto& parent = parent_it->second;

            result.directions.push_back(
                parent.direction
            );

            result.positions.push_back(
                current
            );

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

    /*
     TO DO: DEAD END DETECTION + CONSIDER BLOCKING IN ALLIES
    */
} // search namespace closer