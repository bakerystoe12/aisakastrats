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
#include <cmath>
#include <limits>


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

    struct BodyChain {
        std::vector<unswbc::Position> tiles;
        bool complete = false;
    };

    struct PathValidationResult {
        bool valid = true;
        bool timed_out = false;
        // For Queen dead-end rejection this is the first node that commits
        // the route to the sealed branch, not necessarily the dead-end tip.
        std::optional<unswbc::Position> bad_node;
    };

    constexpr int DEAD_END_MIN_SPACE = 4;
    constexpr double DEAD_END_SPACE_FACTOR = 1.0;
    constexpr int DEAD_END_INF = std::numeric_limits<int>::max() / 4;

    inline int dead_end_needed_space(int length) {
        return std::max(
            DEAD_END_MIN_SPACE,
            static_cast<int>(std::ceil(DEAD_END_SPACE_FACTOR * length))
        );
    }

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
        bool known_only = false
    ) {
        std::vector<SearchNeighbor> result;

        for (auto d : unswbc::Direction::get_direction_list()) {

            if (known_only && !Memory::edge_is_known(pos, d))
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
                node.pos
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
                get_neighbors(current.pos, true);

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
       PATH VALIDATION

       Step 5 already produced a legal route using the normal pathfinder.
       These helpers do not search for another target. They only walk the
       already-produced route and reject the first node that creates an
       obvious ally trap or dead end.
    =============================================================*/

    BodyChain visible_chain_for_id(
        int dragon_id,
        const std::vector<unswbc::Tile>& tiles
    ) {
        std::unordered_map<
            unswbc::Position,
            const unswbc::DragonPart*,
            unswbc::PositionHash
        > parts;

        std::optional<unswbc::Position> head;

        for (const auto& tile : tiles) {
            auto* part = tile.get_dragon();
            if (!part || part->get_id() != dragon_id)
                continue;

            parts[tile.get_position()] = part;
            if (part->is_head())
                head = tile.get_position();
        }

        BodyChain chain;
        if (!head.has_value())
            return chain;

        chain.tiles.push_back(*head);
        unswbc::Position current = *head;

        while (true) {
            std::optional<unswbc::Position> next;

            for (auto d : unswbc::Direction::get_direction_list()) {
                unswbc::Position candidate = current.add_dir(d);
                auto it = parts.find(candidate);
                if (it == parts.end())
                    continue;
                if (it->second->is_head())
                    continue;
                if (it->second->get_id() != dragon_id)
                    continue;
                if (candidate.add_dir(it->second->get_dir()) != current)
                    continue;
                if (std::find(
                        chain.tiles.begin(),
                        chain.tiles.end(),
                        candidate
                    ) != chain.tiles.end()) {
                    continue;
                }

                next = candidate;
                break;
            }

            if (!next.has_value())
                break;

            chain.tiles.push_back(*next);
            current = *next;
        }

        // The true tail is visible only when its four neighbours are visible.
        bool tail_visible = true;
        for (auto d : unswbc::Direction::get_direction_list()) {
            if (!unswbc::ct->get_tile(current.add_dir(d))) {
                tail_visible = false;
                break;
            }
        }
        chain.complete = tail_visible;
        return chain;
    }

    std::vector<BodyChain> visible_dragon_chains(
        const std::vector<unswbc::Tile>& tiles
    ) {
        std::unordered_set<int> ids;
        for (const auto& tile : tiles) {
            auto* part = tile.get_dragon();
            if (part && part->is_head())
                ids.insert(part->get_id());
        }

        std::vector<BodyChain> chains;
        for (int id : ids) {
            auto chain = visible_chain_for_id(id, tiles);
            if (!chain.tiles.empty())
                chains.push_back(std::move(chain));
        }
        return chains;
    }

    BodyChain simulate_own_chain(
        const BodyChain& original,
        const std::vector<unswbc::Position>& path,
        size_t step_index
    ) {
        BodyChain simulated;
        if (original.tiles.empty())
            return simulated;

        // A normal move advances the tail by one segment; eating a pearl
        // keeps the tail in place and increases the body length by one.
        int steps = static_cast<int>(step_index) + 1;
        int pearls = 0;
        for (size_t i = 0; i <= step_index && i < path.size(); ++i) {
            auto* tile = unswbc::ct->get_tile(path[i]);
            if (tile && tile->has_pearl())
                ++pearls;
        }

        int tail_advances = std::max(0, steps - pearls);
        int old_segments = std::max(
            0,
            static_cast<int>(original.tiles.size()) - tail_advances
        );

        // Build the future body in head->tail order: future head, then the
        // path backwards, then the old body prefix that remains attached.
        for (size_t i = step_index + 1; i > 0; --i)
            simulated.tiles.push_back(path[i - 1]);

        for (int i = 0; i < old_segments; ++i)
            simulated.tiles.push_back(original.tiles[i]);

        // If the visible tail was not known, do not claim that old body
        // segments are guaranteed to release. Pearl growth also makes the
        // simple one-step tail model conservative.
        simulated.complete = original.complete;

        return simulated;
    }

    std::unordered_set<unswbc::Position, unswbc::PositionHash>
    occupied_for_chains(
        const std::vector<BodyChain>& chains
    ) {
        std::unordered_set<unswbc::Position, unswbc::PositionHash> occupied;
        for (const auto& chain : chains)
            for (const auto& p : chain.tiles)
                occupied.insert(p);
        return occupied;
    }

    std::pair<int, bool> time_aware_space(
        unswbc::Position start,
        const BodyChain& chain,
        int t0,
        int pearl_shift,
        const std::unordered_set<unswbc::Position, unswbc::PositionHash>& static_occupied
    ) {
        std::unordered_map<
            unswbc::Position,
            int,
            unswbc::PositionHash
        > release;

        const int len = static_cast<int>(chain.tiles.size());
        for (int k = 0; k < len; ++k) {
            release[chain.tiles[k]] = chain.complete
                ? (len - k + pearl_shift)
                : DEAD_END_INF;
        }

        auto blocked_at = [&](unswbc::Position pos, int depth) {
            auto it = release.find(pos);
            if (it != release.end())
                return (depth - 1 + t0) < it->second;
            return static_occupied.count(pos) > 0;
        };

        std::deque<std::pair<unswbc::Position, int>> queue;
        std::unordered_set<unswbc::Position, unswbc::PositionHash> seen;
        queue.push_back({start, 0});
        seen.insert(start);

        int area = 0;
        bool frontier = false;

        while (!queue.empty()) {
            auto [current, depth] = queue.front();
            queue.pop_front();

            for (auto d : unswbc::Direction::get_direction_list()) {
                auto edge_type = Memory::known_edge_type(current, d);

                if (edge_type == unswbc::EdgeType::KELP)
                    continue;

                if (edge_type == unswbc::EdgeType::PORTAL) {
                    // A portal is an escape from the locally proven region.
                    frontier = true;
                    continue;
                }

                unswbc::Position next = current.add_dir(d);

                // We cannot prove anything beyond an unknown edge.
                if (!Memory::edge_is_known(current, d)) {
                    frontier = true;
                    continue;
                }

                if (seen.count(next) || blocked_at(next, depth + 1))
                    continue;

                seen.insert(next);
                ++area;
                queue.push_back({next, depth + 1});
            }
        }

        return {area, frontier};
    }

    // A structural dead-end terminal is a tile with at least three blocked
    // sides. Kelp, any visible dragon body, and the Queen's simulated body
    // all count as blocked. Unknown edges remain unknown and do not count.
    bool obvious_dead_end_at(
        unswbc::Position start,
        const std::unordered_set<unswbc::Position, unswbc::PositionHash>& blocked_body
    ) {
        int blocked_sides = 0;

        for (auto d : unswbc::Direction::get_direction_list()) {
            const auto edge_type = Memory::known_edge_type(start, d);

            if (edge_type == unswbc::EdgeType::KELP) {
                ++blocked_sides;
                continue;
            }

            // A known portal is not a wall.
            if (edge_type == unswbc::EdgeType::PORTAL)
                continue;

            // Unknown terrain is deliberately not treated as blocked.
            if (!Memory::edge_is_known(start, d))
                continue;

            const auto next = start.add_dir(d);
            if (blocked_body.count(next)) {
                ++blocked_sides;
                continue;
            }

        }

        return blocked_sides >= 3;
    }

    // Cheap pre-filter: a path node with fewer than two immediately blocked
    // sides is very unlikely to be the entrance to a sealed pocket. The final
    // path node is always allowed through so a dead-end just beyond the target
    // is still checked.
    bool has_two_blocked_sides(
        unswbc::Position start,
        const std::unordered_set<unswbc::Position, unswbc::PositionHash>& blocked_body
    ) {
        int blocked_sides = 0;

        for (auto d : unswbc::Direction::get_direction_list()) {
            const auto edge_type = Memory::known_edge_type(start, d);

            if (edge_type == unswbc::EdgeType::KELP) {
                ++blocked_sides;
            } else if (edge_type == unswbc::EdgeType::PORTAL) {
                continue;
            } else if (!Memory::edge_is_known(start, d)) {
                continue;
            } else if (blocked_body.count(start.add_dir(d))) {
                ++blocked_sides;
            }

            if (blocked_sides >= 2)
                return true;
        }

        return false;
    }

    struct DeadEndBranchResult {
        bool detected = false;
        bool has_known_exit = false;
        bool has_unknown_frontier = false;
        std::optional<unswbc::Position> commitment_node;
    };

    // Analyze one proposed route branch. The search is deliberately bounded:
    // once a real exit, unknown frontier, or the three-blocked-side terminal
    // is enough to decide the result, there is no reason to explore the whole
    // known map. This keeps Queen validation cheap enough to run every turn.
    DeadEndBranchResult analyze_dead_end_branch(
        const std::vector<unswbc::Position>& path,
        size_t commitment_index,
        unswbc::Position branch_entry,
        const std::unordered_set<unswbc::Position, unswbc::PositionHash>& blocked_body
    ) {
        DeadEndBranchResult result;

        if (commitment_index >= path.size())
            return result;

        // Avoid an expensive traversal for wide-open path nodes. The final
        // node is still checked because the terminal can lie immediately
        // beyond the requested target.
        if (commitment_index + 1 < path.size() &&
            !has_two_blocked_sides(path[commitment_index], blocked_body))
            return result;

        constexpr int DEAD_END_MAX_VISITS = 32;

        const unswbc::Position entry = branch_entry;
        std::deque<unswbc::Position> queue;
        std::unordered_set<unswbc::Position, unswbc::PositionHash> region;
        std::unordered_set<unswbc::Position, unswbc::PositionHash> path_nodes;

        // Only earlier path nodes matter when deciding whether the branch has
        // another known route back out. Build this once instead of scanning
        // the whole path for every flood-fill neighbour.
        for (size_t j = 0; j < commitment_index; ++j)
            path_nodes.insert(path[j]);

        queue.push_back(path[commitment_index]);
        region.insert(path[commitment_index]);

        int visits = 0;
        while (!queue.empty() && visits < DEAD_END_MAX_VISITS) {
            const auto current = queue.front();
            queue.pop_front();
            ++visits;

            if (obvious_dead_end_at(current, blocked_body)) {
                result.detected = true;
                break;
            }

            for (auto d : unswbc::Direction::get_direction_list()) {
                const auto edge_type = Memory::known_edge_type(current, d);

                if (edge_type == unswbc::EdgeType::KELP)
                    continue;

                // Known portals are real exits for the Queen.
                if (edge_type == unswbc::EdgeType::PORTAL) {
                    result.has_known_exit = true;
                    break;
                }

                // We cannot prove a sealed branch through an unseen edge.
                if (!Memory::edge_is_known(current, d)) {
                    result.has_unknown_frontier = true;
                    break;
                }

                const auto next = current.add_dir(d);

                // The normal route back toward the Queen is deliberately cut.
                if (next == entry)
                    continue;

                // A second connection to an earlier path node is a known exit.
                if (path_nodes.count(next)) {
                    result.has_known_exit = true;
                    break;
                }

                // Dragon bodies are walls for the structural flood fill.
                // The Queen's own simulated body is also a wall.
                if (blocked_body.count(next))
                    continue;


                if (region.count(next))
                    continue;

                region.insert(next);
                queue.push_back(next);
            }

            if (result.has_known_exit || result.has_unknown_frontier)
                break;
        }

        // Hitting the visit budget means we did not prove anything. Be
        // conservative: do not reject the route merely because analysis was
        // expensive.
        if (visits >= DEAD_END_MAX_VISITS &&
            !result.detected &&
            !result.has_known_exit &&
            !result.has_unknown_frontier) {
            return result;
        }

        if (result.detected &&
            !result.has_known_exit &&
            !result.has_unknown_frontier) {
            result.commitment_node = path[commitment_index];
        }

        return result;
    }

    std::vector<int> blocked_allies_for_chain(
        const std::vector<BodyChain>& chains,
        int my_id,
        const BodyChain& mine_before,
        const BodyChain& mine_after
    ) {
        std::unordered_set<unswbc::Position, unswbc::PositionHash> before;
        for (const auto& chain : chains)
            for (const auto& p : chain.tiles)
                before.insert(p);

        // The chain list contains the real/current own body. Replace it with
        // the simulated body for this point on the proposed path.
        for (const auto& chain : chains) {
            if (chain.tiles.empty())
                continue;
            auto* head_tile = unswbc::ct->get_tile(chain.tiles.front());
            if (head_tile && head_tile->get_dragon() &&
                head_tile->get_dragon()->get_id() == my_id) {
                for (const auto& p : chain.tiles)
                    before.erase(p);
                break;
            }
        }
        for (const auto& p : mine_before.tiles)
            before.insert(p);

        std::unordered_set<unswbc::Position, unswbc::PositionHash> after = before;
        for (const auto& p : mine_before.tiles)
            after.erase(p);
        for (const auto& p : mine_after.tiles)
            after.insert(p);

        std::vector<int> trapped;
        unswbc::Team my_team = unswbc::ct->get_team();

        auto chain_id = [&](const BodyChain& chain) -> int {
            if (chain.tiles.empty()) return -1;
            auto* tile = unswbc::ct->get_tile(chain.tiles.front());
            if (!tile || !tile->get_dragon()) return -1;
            return tile->get_dragon()->get_id();
        };

        auto exits = [&](unswbc::Position head,
                         const std::unordered_set<unswbc::Position, unswbc::PositionHash>& occupied) {
            int count = 0;
            for (auto d : unswbc::Direction::get_direction_list()) {
                auto edge_type = Memory::known_edge_type(head, d);
                if (edge_type == unswbc::EdgeType::KELP)
                    continue;
                if (edge_type == unswbc::EdgeType::PORTAL ||
                    !Memory::edge_is_known(head, d)) {
                    ++count;
                    continue;
                }
                if (!occupied.count(head.add_dir(d)))
                    ++count;
            }
            return count;
        };

        for (const auto& ally : chains) {
            int aid = chain_id(ally);
            if (aid < 0 || aid == my_id || ally.tiles.empty())
                continue;

            auto* head_tile = unswbc::ct->get_tile(ally.tiles.front());
            if (!head_tile || !head_tile->get_dragon() ||
                head_tile->get_dragon()->get_team() != my_team)
                continue;

            const auto head = ally.tiles.front();

            if (exits(head, before) > 0 && exits(head, after) == 0) {
                trapped.push_back(aid);
                continue;
            }

            std::unordered_set<unswbc::Position, unswbc::PositionHash> before_static = before;
            std::unordered_set<unswbc::Position, unswbc::PositionHash> after_static = after;
            for (const auto& p : ally.tiles) {
                before_static.erase(p);
                after_static.erase(p);
            }

            int need = dead_end_needed_space(static_cast<int>(ally.tiles.size()));
            auto [before_area, before_frontier] = time_aware_space(
                head, ally, 0, 0, before_static
            );
            auto [after_area, after_frontier] = time_aware_space(
                head, ally, 0, 0, after_static
            );

            bool safe_before = before_frontier || before_area >= need;
            bool safe_after = after_frontier || after_area >= need;

            if (safe_before && !safe_after)
                trapped.push_back(aid);
        }

        return trapped;
    }

    PathValidationResult validate_path(
        const PathResult& path,
        const std::vector<unswbc::Tile>& tiles,
        double* deadline
    ) {
        PathValidationResult result;

        if (path.positions.empty())
            return result;

        const int my_id = unswbc::ct->get_id();
        BodyChain original = visible_chain_for_id(my_id, tiles);
        if (original.tiles.empty()) {
            // We cannot construct the body model; do not manufacture a
            // rejection from missing information.
            return result;
        }

        auto chains = visible_dragon_chains(tiles);
        auto occupied_all = occupied_for_chains(chains);

        for (size_t i = 0; i < path.positions.size(); ++i) {
            if (deadline) {
                double now = std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch()
                ).count();
                if (now >= *deadline) {
                    result.timed_out = true;
                    return result;
                }
            }

            const auto destination = path.positions[i];
            auto* destination_tile = unswbc::ct->get_tile(destination);

            // Another visible dragon occupies the proposed node.
            if (destination_tile && destination_tile->get_dragon() &&
                destination_tile->get_dragon()->get_id() != my_id) {
                result.valid = false;
                result.bad_node = destination;
                return result;
            }

            BodyChain simulated = simulate_own_chain(
                original,
                path.positions,
                i
            );

            if (simulated.tiles.empty() ||
                std::find(
                    simulated.tiles.begin(),
                    simulated.tiles.end(),
                    destination
                ) == simulated.tiles.end()) {
                result.valid = false;
                result.bad_node = destination;
                return result;
            }

            BodyChain before_chain = original;
            if (i > 0) {
                before_chain = simulate_own_chain(
                    original,
                    path.positions,
                    i - 1
                );
            }

            auto occupied_at_step = occupied_all;
            for (const auto& p : original.tiles)
                occupied_at_step.erase(p);
            for (const auto& p : simulated.tiles)
                occupied_at_step.insert(p);

            // Only the Queen proactively rejects a structurally proven
            // dead-end branch. Normal dragons keep their existing freedom to
            // enter valuable pockets and fall back to last_resort() later.
            // IDs 0 and 1 are the Queen identities already used elsewhere in
            // this bot (main.cpp / kamikaze.hpp).
            const bool is_queen = (my_id == 0 || my_id == 1);

            if (is_queen) {
                const unswbc::Position branch_entry =
                    (i == 0)
                        ? original.tiles.front()
                        : path.positions[i - 1];

                auto dead_end = analyze_dead_end_branch(
                    path.positions,
                    i,
                    branch_entry,
                    occupied_at_step
                );

                if (dead_end.detected &&
                    !dead_end.has_known_exit &&
                    !dead_end.has_unknown_frontier &&
                    dead_end.commitment_node.has_value()) {
                    result.valid = false;
                    result.bad_node = dead_end.commitment_node;
                    return result;
                }
            }

            auto blocked_allies = blocked_allies_for_chain(
                chains,
                my_id,
                before_chain,
                simulated
            );

            if (!blocked_allies.empty()) {
                result.valid = false;
                result.bad_node = destination;
                return result;
            }
        }

        return result;
    }

    /* ============================================================
       PATHFINDING IF WE SEE NO PEARLS: nearest_unexplored
    =============================================================*/

    std::optional<unswbc::Position> nearest_unexplored(
        unswbc::Position start
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
                get_neighbors(current.pos)) {

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
                get_neighbors(current.pos)) {

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