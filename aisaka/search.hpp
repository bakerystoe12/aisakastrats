#pragma once

#include "helper.hpp"
#include <deque>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <optional>


// ============================================================================
// SEARCH
// ============================================================================
namespace Search {

    struct SearchNeighbor {
    unswbc::Position position;
    int cost;
    bool through_portal;
    };

    unswbc::Position portal_exit_position(uint64_t key) {
    int x = static_cast<int>(key >> 32);
    int y = static_cast<int>((key >> 1) & 0x7FFFFFFF);

    return unswbc::Position(x, y);
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

    int dist(unswbc::Position a, unswbc::Position b) {
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
                1,
                false
            });
            continue;
        }

        unswbc::Position destination;

        if (get_portal_destination(pos, d, destination)) {
            result.push_back({
                destination,
                1,
                true
            });
        }
    }

    return result;
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

    int room(
        unswbc::Position start,
        unswbc::Position came_from,
        int cap,
        double deadline = -1
    ) {
        std::unordered_set<
            unswbc::Position,
            unswbc::PositionHash
        > seen = {came_from, start};

        std::deque<unswbc::Position> queue = {start};

        int count = 0;
        int timer = State::BFS_DEADLINE_CHECK_EVERY;

        while (!queue.empty()) {
            unswbc::Position pos = queue.front();
            queue.pop_front();

            count++;

            if (count >= cap)
                return cap;

            if (deadline > 0) {
                if (--timer == 0) {
                    double now =
                        std::chrono::duration<double>(
                            std::chrono::steady_clock::now()
                                .time_since_epoch()
                        ).count();

                    if (now >= deadline)
                        return count;

                    timer = State::BFS_DEADLINE_CHECK_EVERY;
                }
            }

            for (auto neighbour :
                get_neighbors(
                    pos,
                    unswbc::game->get_round_num()
                )) {

                unswbc::Position nxt = neighbour.position;

                if (seen.count(nxt))
                    continue;

                if (!tile_ever_seen(nxt))
                    return cap;

                if (Memory::known_occupied(nxt))
                    continue;

                seen.insert(nxt);
                queue.push_back(nxt);
            }
        }

        return count;
    }

        double split_room(
        unswbc::Position start,
        unswbc::Position came_from,
        double needed,
        int bfs_cap = 100
    ) {
        std::unordered_set<
            unswbc::Position,
            unswbc::PositionHash
        > seen = {came_from, start};

        std::deque<unswbc::Position> queue = {start};

        double total = 0.0;
        int expanded = 0;

        while (!queue.empty()) {
            if (total >= needed || expanded >= bfs_cap)
                break;

            unswbc::Position pos = queue.front();
            queue.pop_front();

            expanded++;

            double weight = 1.0;

            auto* tile = unswbc::ct->get_tile(pos);

            if (tile) {
                int pt = tile->get_pearl_time();

                if (
                    tile->has_pearl() ||
                    (0 <= pt &&
                    pt <= 25)
                ) {
                    weight = State::PEARL_ROOM_WEIGHT;
                }
            }

            total += weight;

            for (auto neighbour :
                get_neighbors(
                    pos,
                    unswbc::game->get_round_num()
                )) {

                unswbc::Position nxt = neighbour.position;

                if (seen.count(nxt))
                    continue;

                if (!tile_ever_seen(nxt))
                    return needed;

                if (Memory::known_occupied(nxt))
                    continue;

                seen.insert(nxt);
                queue.push_back(nxt);
            }
        }

        return total;
    }

    struct AStarNode {
        int f, g, counter;
        unswbc::Position pos;
        bool operator>(const AStarNode& o) const {
            if (f != o.f) return f > o.f;
            if (g != o.g) return g > o.g;
            return counter > o.counter;
        }
    };

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

    int pearl_dist(unswbc::Position pos, const std::vector<std::pair<unswbc::Position, int>>& targets) {
        if (targets.empty()) return 0;
        int best = 100000;
        for (auto& t : targets) best = std::min(best, std::max(dist(pos, t.first), t.second));
        return best;
    }

    const int CLUSTER_RADIUS = 2;
    const int CLUSTER_MIN_SIZE = 6;

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

    std::unordered_map<
        unswbc::Position,
        int,
        unswbc::PositionHash
    > astar_targets(
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

    std::optional<unswbc::Position> nearest_unexplored(
        unswbc::Position start,
        int round_num
    ) {
        std::deque<unswbc::Position> queue;

        std::unordered_set<
            unswbc::Position,
            unswbc::PositionHash
        > seen;

        queue.push_back(start);
        seen.insert(start);

        int expanded = 0;

        while (
            !queue.empty() &&
            expanded < State::UNEXPLORED_BFS_CAP
        ) {
            unswbc::Position pos =
                queue.front();

            queue.pop_front();
            expanded++;

            for (auto neighbour :
                get_neighbors(
                    pos,
                    round_num
                )) {

                unswbc::Position nxt =
                    neighbour.position;

                // Already considered in this search.
                if (seen.count(nxt))
                    continue;

                // ----------------------------------------------------
                // PERMANENT EXPLORATION MEMORY
                //
                // Never intentionally travel back through a tile
                // the dragon has already visited.
                // ----------------------------------------------------

                if (State::EXPLORATION_VISITED.count(nxt))
                    continue;

                seen.insert(nxt);

                if (Memory::known_occupied(nxt))
                    continue;

                // ----------------------------------------------------
                // FRONTIER
                //
                // We have reached the edge of our known map without
                // physically exploring this position yet.
                // ----------------------------------------------------

                if (!tile_ever_seen(nxt)) {
                    return nxt;
                }

                // Known tile that we have never physically visited.
                // Continue exploring through it.
                queue.push_back(nxt);
            }
        }

        return std::nullopt;
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
}