#pragma once

#include "helper.hpp"
#include <algorithm>
#include <cmath>
#include <climits>
#include <deque>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <optional>
#include <queue>
#include <chrono>


namespace Search {
    /* ==============================================================
        Search constants
    ===============================================================*/

    const int CLUSTER_RADIUS = 2;
    const int CLUSTER_MIN_SIZE = 6;

    /* ===============================================================
        Constructors
    ==================================================================*/

    struct SearchNeighbor {
        unswbc::Position position;
        int cost;
        bool through_portal;
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


    /* ========================================================================
        Helper functions: dist, tile_ever_seen, open_step, get_neighbors
    =========================================================================*/

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

    // Forward declaration because get_neighbors uses the portal lookup below.
    bool get_portal_destination(unswbc::Position pos, unswbc::Direction d, unswbc::Position& destination);

    std::vector<SearchNeighbor> get_neighbors(unswbc::Position pos, int round_num) {

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

    /* =============================================================
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
    /* ======================================================================
     Fatal Zone
    =======================================================================*/

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

    /* ===========================================================
        PEARL PATHFINDING: 
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

    /* ================================================================
       DEAD-END DETECTION

       This replaces the old Search::room() idea.

       room() only answered "how many currently reachable tiles are there?"
       That is a poor fit for a dragon because our own body is temporary
       occupancy: the tail normally moves out of those tiles over time.

       This detector instead asks:
         "After making this one move, can the body still reach enough
          usable space without relying on currently unseen territory?"

       Ally safety is evaluated here as well: a candidate move is rejected
       when it removes an ally's last exit or shrinks the ally's reachable
       space below its body-length requirement.

       Search still does not choose targets or score enemy heads.
    ================================================================ */

    constexpr int DEAD_END_MIN_SPACE = 4;
    constexpr double DEAD_END_SPACE_FACTOR = 1.0;
    constexpr int DEAD_END_INF = INT_MAX / 4;

    struct DeadEndInfo {
        int area = 0;
        bool frontier = false;
        bool dead_end = false;
        std::vector<int> blocked_allies;
    };

    struct BodyChain {
        std::vector<unswbc::Position> tiles; // head -> visible tail
        bool complete = false;               // true if the real tail is visible
    };

    int dead_end_needed_space(int length) {
        return std::max(
            DEAD_END_MIN_SPACE,
            static_cast<int>(std::ceil(DEAD_END_SPACE_FACTOR * length))
        );
    }

    BodyChain visible_own_chain() {
        BodyChain chain;
        const int my_id = unswbc::ct->get_id();

        unswbc::Position head;
        bool found_head = false;

        for (const auto& tile : unswbc::ct->get_tiles()) {
            const auto* part = tile.get_dragon();
            if (part && part->get_id() == my_id && part->is_head()) {
                head = tile.get_position();
                found_head = true;
                break;
            }
        }

        if (!found_head)
            return chain;

        chain.tiles.push_back(head);
        unswbc::Position cur = head;

        while (true) {
            std::optional<unswbc::Position> next;

            for (auto d : unswbc::Direction::get_direction_list()) {
                const unswbc::Position candidate = cur.add_dir(d);
                const auto* tile = unswbc::ct->get_tile(candidate);
                const auto* part = tile ? tile->get_dragon() : nullptr;

                if (!part || part->is_head() || part->get_id() != my_id)
                    continue;

                if (std::find(chain.tiles.begin(), chain.tiles.end(), candidate)
                    != chain.tiles.end())
                    continue;

                // Body segments point towards the head/preceding segment.
                if (candidate.add_dir(part->get_dir()) == cur) {
                    next = candidate;
                    break;
                }
            }

            if (!next)
                break;

            chain.tiles.push_back(*next);
            cur = *next;
        }

        chain.complete = std::all_of(
            unswbc::Direction::get_direction_list().begin(),
            unswbc::Direction::get_direction_list().end(),
            [&](unswbc::Direction d) {
                return unswbc::ct->get_tile(cur.add_dir(d)) != nullptr;
            }
        );

        return chain;
    }

    bool dead_end_static_occupied(
        unswbc::Position pos,
        const std::unordered_set<unswbc::Position, unswbc::PositionHash>& own_visible
    ) {
        if (own_visible.count(pos))
            return false;

        const auto* tile = unswbc::ct->get_tile(pos);
        if (tile && tile->get_dragon())
            return tile->get_dragon()->get_id() != unswbc::ct->get_id();

        // Outside vision, reuse Memory's existing occupancy cache. This also
        // avoids introducing a second global body-position cache here.
        return Memory::known_occupied(pos);
    }

    std::pair<int, bool> time_aware_space(
        unswbc::Position start,
        int pearl_shift,
        const BodyChain& chain,
        int t0,
        const std::unordered_set<unswbc::Position, unswbc::PositionHash>& static_occupied
    ) {
        using PosSet = std::unordered_set<unswbc::Position, unswbc::PositionHash>;
        using ReleaseMap = std::unordered_map<unswbc::Position, int, unswbc::PositionHash>;

        ReleaseMap release;
        const int visible_len = static_cast<int>(chain.tiles.size());

        for (int k = 0; k < visible_len; ++k) {
            // If the real tail is outside vision, conservatively keep the
            // visible body blocked. We cannot prove when those tiles free.
            release[chain.tiles[k]] = chain.complete
                ? (visible_len - k + pearl_shift)
                : DEAD_END_INF;
        }

        PosSet seen{start};
        std::deque<std::pair<unswbc::Position, int>> queue;
        queue.emplace_back(start, 0);

        int area = 0;
        bool frontier = false;

        auto blocked_at = [&](unswbc::Position pos, int depth) {
            const auto release_it = release.find(pos);
            if (release_it != release.end())
                return (depth - 1 + t0) < release_it->second;
            return static_occupied.count(pos) != 0;
        };

        while (!queue.empty()) {
            const auto [cur, depth] = queue.front();
            queue.pop_front();

            const auto* tile = unswbc::ct->get_tile(cur);
            if (!tile)
                continue;

            for (auto d : unswbc::Direction::get_direction_list()) {
                const auto edge = tile->get_edge(d).get_edge_type();

                if (edge == unswbc::EdgeType::KELP)
                    continue;

                if (edge == unswbc::EdgeType::PORTAL) {
                    // A portal is an escape from the locally visible region.
                    frontier = true;
                    continue;
                }

                const unswbc::Position next = cur.add_dir(d);
                if (!unswbc::ct->get_tile(next)) {
                    frontier = true;
                    continue;
                }

                if (seen.count(next))
                    continue;

                const int next_depth = depth + 1;
                if (blocked_at(next, next_depth))
                    continue;

                seen.insert(next);
                ++area;
                queue.emplace_back(next, next_depth);
            }
        }

        return {area, frontier};
    }

    DeadEndInfo dead_end_space(
        unswbc::Position start,
        int pearl_shift,
        const BodyChain& chain
    ) {
        using PosSet = std::unordered_set<unswbc::Position, unswbc::PositionHash>;

        // Keep the existing occupancy semantics for our own dead-end test:
        // our body is handled by `chain`/release times, while other dragons
        // and remembered occupied tiles remain static obstacles.
        PosSet own_visible(chain.tiles.begin(), chain.tiles.end());
        PosSet static_occupied;

        for (const auto& tile : unswbc::ct->get_tiles()) {
            if (!tile.get_dragon())
                continue;
            if (!own_visible.count(tile.get_position()))
                static_occupied.insert(tile.get_position());
        }

        const auto [area, frontier] =
            time_aware_space(start, pearl_shift, chain, 1, static_occupied);

        const int need = dead_end_needed_space(
            unswbc::ct->get_length() + pearl_shift
        );

        return {area, frontier, !frontier && area < need};
    }

    // Build ordered visible body chains for every visible dragon once per
    // query. This lets ally safety reuse the same time-aware flood fill as
    // our own dead-end detection instead of maintaining a second model.
    std::unordered_map<int, BodyChain> visible_dragon_chains() {
        std::unordered_map<int, BodyChain> chains;

        for (const auto& tile_ref : unswbc::ct->get_tiles()) {
            const auto* head = tile_ref.get_dragon();
            if (!head || !head->is_head())
                continue;

            const int dragon_id = head->get_id();
            BodyChain chain;
            unswbc::Position cur = tile_ref.get_position();
            chain.tiles.push_back(cur);

            while (true) {
                std::optional<unswbc::Position> next;

                for (auto d : unswbc::Direction::get_direction_list()) {
                    const unswbc::Position candidate = cur.add_dir(d);
                    const auto* tile = unswbc::ct->get_tile(candidate);
                    const auto* part = tile ? tile->get_dragon() : nullptr;

                    if (!part || part->is_head() || part->get_id() != dragon_id)
                        continue;
                    if (std::find(chain.tiles.begin(), chain.tiles.end(), candidate)
                        != chain.tiles.end())
                        continue;

                    if (candidate.add_dir(part->get_dir()) == cur) {
                        next = candidate;
                        break;
                    }
                }

                if (!next)
                    break;

                chain.tiles.push_back(*next);
                cur = *next;
            }

            chain.complete = std::all_of(
                unswbc::Direction::get_direction_list().begin(),
                unswbc::Direction::get_direction_list().end(),
                [&](unswbc::Direction d) {
                    return unswbc::ct->get_tile(cur.add_dir(d)) != nullptr;
                }
            );

            chains.insert_or_assign(dragon_id, std::move(chain));
        }

        return chains;
    }

    int dragon_exits(
        unswbc::Position head,
        const std::unordered_set<unswbc::Position, unswbc::PositionHash>& occupied
    ) {
        int exits = 0;
        const auto* tile = unswbc::ct->get_tile(head);
        if (!tile)
            return 0;

        for (auto d : unswbc::Direction::get_direction_list()) {
            const auto edge = tile->get_edge(d).get_edge_type();

            if (edge == unswbc::EdgeType::PORTAL) {
                ++exits;
                continue;
            }

            if (edge == unswbc::EdgeType::KELP)
                continue;

            const unswbc::Position next = head.add_dir(d);
            if (!unswbc::ct->get_tile(next) || !occupied.count(next))
                ++exits;
        }

        return exits;
    }

    std::vector<int> blocked_allies(
        unswbc::Position dest,
        bool pearl
    ) {
        using PosSet = std::unordered_set<unswbc::Position, unswbc::PositionHash>;

        const int my_id = unswbc::ct->get_id();
        const auto chains = visible_dragon_chains();
        const auto mine_it = chains.find(my_id);
        if (mine_it == chains.end())
            return {};

        PosSet before;
        for (const auto& tile : unswbc::ct->get_tiles()) {
            if (tile.get_dragon())
                before.insert(tile.get_position());
        }

        PosSet after = before;
        if (mine_it->second.complete && !pearl && !mine_it->second.tiles.empty())
            after.erase(mine_it->second.tiles.back());
        after.insert(dest);

        std::vector<int> trapped;

        for (const auto& [ally_id, ally_chain] : chains) {
            if (ally_id == my_id || ally_chain.tiles.empty())
                continue;

            const auto* ally_head = unswbc::ct->get_tile(ally_chain.tiles.front());
            const auto* ally_part = ally_head ? ally_head->get_dragon() : nullptr;
            if (!ally_part || ally_part->get_team() != unswbc::ct->get_team())
                continue;

            const auto head = ally_chain.tiles.front();

            // Fast check: if we remove the ally's last available exit, the
            // move is immediately considered an ally trap.
            if (dragon_exits(head, before) > 0 && dragon_exits(head, after) == 0) {
                trapped.push_back(ally_id);
                continue;
            }

            // Stronger check: an ally may retain one exit but still be sealed
            // into a pocket too small for its body. Compare reachable space
            // before and after our candidate move using the same time-aware
            // body-release model as our own dead-end detector.
            PosSet ally_body(ally_chain.tiles.begin(), ally_chain.tiles.end());

            PosSet static_before = before;
            for (const auto& p : ally_body)
                static_before.erase(p);

            PosSet static_after = after;
            for (const auto& p : ally_body)
                static_after.erase(p);

            const auto [space_before, frontier_before] =
                time_aware_space(head, 0, ally_chain, 0, static_before);
            const auto [space_after, frontier_after] =
                time_aware_space(head, 0, ally_chain, 0, static_after);

            const int need = dead_end_needed_space(
                static_cast<int>(ally_chain.tiles.size())
            );

            const bool safe_before = frontier_before || space_before >= need;
            const bool safe_after = frontier_after || space_after >= need;

            if (safe_before && !safe_after)
                trapped.push_back(ally_id);
        }

        return trapped;
    }

    std::optional<DeadEndInfo> evaluate_dead_end(unswbc::Direction d) {
        const unswbc::Position here = unswbc::ct->get_position();
        const auto* tile = unswbc::ct->get_tile(here);
        if (!tile)
            return std::nullopt;

        const auto edge = tile->get_edge(d).get_edge_type();
        if (edge == unswbc::EdgeType::KELP)
            return std::nullopt;

        if (edge == unswbc::EdgeType::PORTAL)
            return DeadEndInfo{0, true, false};

        const unswbc::Position dest = here.add_dir(d);
        const auto* destination = unswbc::ct->get_tile(dest);

        if (!destination)
            return DeadEndInfo{0, true, false};

        if (destination->get_dragon())
            return std::nullopt;

        const BodyChain chain = visible_own_chain();
        const int pearl_shift = destination->has_pearl() ? 1 : 0;
        auto info = dead_end_space(dest, pearl_shift, chain);
        info.blocked_allies = blocked_allies(dest, pearl_shift != 0);
        return info;
    }

    bool is_dead_end(unswbc::Direction d) {
        const auto info = evaluate_dead_end(d);
        return info.has_value() && info->dead_end;
    }

    /*
       Replacement for the old room() filtering step.
       Returns legal first moves that are not proven dead ends and do not
       trap a visible ally. It deliberately does not choose among them:
       target/path scoring remains Decision's job.
    */

    std::vector<unswbc::Direction> non_dead_end_moves() {
        std::vector<unswbc::Direction> result;

        for (auto d : unswbc::Direction::get_direction_list()) {
            const auto info = evaluate_dead_end(d);
            if (info.has_value() && !info->dead_end && info->blocked_allies.empty())
                result.push_back(d);
        }

        return result;
    }

    bool move_has_escape(unswbc::Direction d) {
        const auto info = evaluate_dead_end(d);
        return info.has_value() && !info->dead_end && info->blocked_allies.empty();
    }

} // search namespace closer