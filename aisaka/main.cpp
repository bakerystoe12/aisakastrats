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

// ============================================================================
// STATE
// ============================================================================
namespace State {
    int WIDTH = 0;
    int HEIGHT = 0;
    bool SONAR3 = false;
    bool AGGRESSIVE = true;

    const int BFS_CAP = 300;
    const double TURN_TIME_BUDGET = 0.075;
    const int BFS_DEADLINE_CHECK_EVERY = 4;

    const int MAX_NEARBY_ALLIES_TO_SPLIT = 3;
    const double PEARL_ROOM_WEIGHT = 0.0;
    const double SPLIT_ROOM_MULTIPLIER = 1.5;
    const int PEARL_SPAWN_HORIZON = 5;
    const int SPLIT_MIN_LENGTH = 4;
    const double PORTAL_CHANCE = 1.0;

    std::deque<unswbc::Position> OWN_BODY;
    std::unordered_set<unswbc::Position, unswbc::PositionHash> OWN_BODY_SET;
    std::unordered_set<unswbc::Position, unswbc::PositionHash> PEARL_CAPABLE;

    std::unordered_set<uint64_t> KELP_EDGES;
    std::vector<uint64_t> KELP_LIST;
    int KELP_SEND_CURSOR = 0;

    struct Teammate { bool aggressive; int length; int round; };
    std::unordered_map<int, Teammate> TEAMMATES;

    std::unordered_map<unswbc::Position, std::array<unswbc::EdgeType, 4>, unswbc::PositionHash> KNOWN_EDGES;

    int LAST_PORTAL_ID = -1;
    int LAST_PORTAL_ROUND = -1000000000;

    struct PearlData { int pearl_time; bool has_pearl; int round; };
    std::unordered_map<unswbc::Position, PearlData, unswbc::PositionHash> PEARL_MEMORY;

    std::unordered_map<uint64_t, int> PORTAL_EDGES;
    std::unordered_map<int, std::vector<uint64_t>> PORTALS;

    struct CacheKey {
        unswbc::Position pos; int d;
        bool operator==(const CacheKey& o) const { return pos == o.pos && d == o.d; }
    };
    struct CacheKeyHash {
        std::size_t operator()(const CacheKey& k) const {
            return unswbc::PositionHash()(k.pos) ^ (k.d << 16);
        }
    };

    std::unordered_map<CacheKey, unswbc::EdgeType, CacheKeyHash> _EDGE_CACHE;
    std::unordered_map<unswbc::Position, bool, unswbc::PositionHash> _OCCUPIED_CACHE;

    void reset_turn_caches() {
        _EDGE_CACHE.clear();
        _OCCUPIED_CACHE.clear();
    }
}

// ============================================================================
// MEMORY
// ============================================================================
namespace Memory {
    const uint64_t SONAR_MAGIC = 0xA15A;
    const int KELP_PER_MSG = 2;
    const uint64_t ID_MASK = (1ULL << 12) - 1;

    uint64_t pack(const std::vector<std::pair<uint64_t, int>>& fields) {
        uint64_t n = 0;
        for (const auto& [value, bits] : fields) {
            n = (n << bits) | (value & ((1ULL << bits) - 1));
        }
        return n;
    }

    std::vector<uint64_t> unpack(uint64_t n, const std::vector<int>& widths) {
        std::vector<uint64_t> out;
        for (auto it = widths.rbegin(); it != widths.rend(); ++it) {
            out.push_back(n & ((1ULL << *it) - 1));
            n >>= *it;
        }
        std::reverse(out.begin(), out.end());
        return out;
    }

    uint64_t edge_key(unswbc::Position pos, unswbc::Direction d) {
        if (d.value == unswbc::Direction::SOUTH || d.value == unswbc::Direction::EAST) {
            pos = pos.add_dir(d);
            d = d.get_opposite();
        }
        uint64_t horizontal = (d.value == unswbc::Direction::NORTH) ? 0 : 1;
        return (static_cast<uint64_t>(pos.x) << 32) | (static_cast<uint64_t>(pos.y) << 1) | horizontal;
    }

    void add_kelp(uint64_t key) {
        if (State::KELP_EDGES.find(key) == State::KELP_EDGES.end()) {
            State::KELP_EDGES.insert(key);
            State::KELP_LIST.push_back(key);
        }
    }

    void remember_portal_crossing(int pid, int round_num) {
        State::LAST_PORTAL_ID = pid;
        State::LAST_PORTAL_ROUND = round_num;
    }

    void remember_portal(uint64_t ekey, int portal_id) {
        if (State::PORTAL_EDGES.find(ekey) != State::PORTAL_EDGES.end()) return;
        State::PORTAL_EDGES[ekey] = portal_id;
        State::PORTALS[portal_id].push_back(ekey);
    }

    void remember_pearl(unswbc::Position pos, const unswbc::Tile& tile, int round_num) {
        int pearl_time = tile.get_pearl_time();
        auto it = State::PEARL_MEMORY.find(pos);
        if (pearl_time == -1 && it != State::PEARL_MEMORY.end() && it->second.pearl_time == -1) return;
        State::PEARL_MEMORY[pos] = {pearl_time, tile.has_pearl(), round_num};
    }

    void remember_vision(const std::vector<unswbc::Tile>& tiles) {
        int now = unswbc::game->get_round_num();
        for (const auto& tile : tiles) {
            unswbc::Position pos = tile.get_position();
            if (State::KNOWN_EDGES.find(pos) == State::KNOWN_EDGES.end()) {
                std::array<unswbc::EdgeType, 4> edges;
                for (auto d : unswbc::Direction::get_direction_list()) {
                    auto edge_type = tile.get_edge(d).get_edge_type();
                    edges[direction_to_index(d)] = edge_type;
                    if (edge_type == unswbc::EdgeType::KELP) {
                        add_kelp(edge_key(pos, d));
                    } else if (edge_type == unswbc::EdgeType::PORTAL) {
                        remember_portal(edge_key(pos, d), tile.get_edge(d).get_portal_id());
                    }
                }
                State::KNOWN_EDGES[pos] = edges;
            }
            remember_pearl(pos, tile, now);
        }
    }

    unswbc::EdgeType known_edge_type(unswbc::Position pos, unswbc::Direction d) {
        State::CacheKey key{pos, direction_to_index(d)};
        auto it = State::_EDGE_CACHE.find(key);
        if (it != State::_EDGE_CACHE.end()) return it->second;

        unswbc::EdgeType result = unswbc::EdgeType::EMPTY;
        auto* tile = unswbc::ct->get_tile(pos);
        if (tile) {
            result = tile->get_edge(d).get_edge_type();
        } else {
            auto known = State::KNOWN_EDGES.find(pos);
            if (known == State::KNOWN_EDGES.end()) {
                if (State::KELP_EDGES.count(edge_key(pos, d))) {
                    result = unswbc::EdgeType::KELP;
                } else if (!State::AGGRESSIVE) {
                    result = unswbc::EdgeType::KELP;
                }
            } else {
                result = known->second[direction_to_index(d)];
            }
        }
        State::_EDGE_CACHE[key] = result;
        return result;
    }

    bool known_occupied(unswbc::Position pos) {
        auto it = State::_OCCUPIED_CACHE.find(pos);
        if (it != State::_OCCUPIED_CACHE.end()) return it->second;

        bool result = false;
        auto* tile = unswbc::ct->get_tile(pos);
        if (tile) {
            result = (tile->get_dragon() != nullptr);
        } else {
            result = State::OWN_BODY_SET.count(pos);
        }
        State::_OCCUPIED_CACHE[pos] = result;
        return result;
    }

    void update_own_body() {
        unswbc::Position here = unswbc::ct->get_position();
        if (State::OWN_BODY.empty() || !(State::OWN_BODY.front() == here)) {
            State::OWN_BODY.push_front(here);
        }
        int length = unswbc::ct->get_length();
        while (State::OWN_BODY.size() > static_cast<size_t>(length)) {
            State::OWN_BODY.pop_back();
        }
        State::OWN_BODY_SET.clear();
        for (const auto& p : State::OWN_BODY) State::OWN_BODY_SET.insert(p);
    }

    uint64_t encode_report(const std::vector<uint64_t>& kelp_chunk) {
        std::vector<std::pair<uint64_t, int>> fields = {
            {SONAR_MAGIC, 16}, {static_cast<uint64_t>(State::AGGRESSIVE), 1},
            {static_cast<uint64_t>(std::min(unswbc::ct->get_length(), 127)), 7},
            {static_cast<uint64_t>(unswbc::ct->get_id()) & ID_MASK, 12},
            {static_cast<uint64_t>(kelp_chunk.size()), 2}
        };
        for (size_t i = 0; i < 2; i++) {
            if (i < kelp_chunk.size()) {
                uint64_t k = kelp_chunk[i];
                uint64_t x = k >> 32;
                uint64_t y = (k >> 1) & ((1ULL << 31) - 1);
                uint64_t h = k & 1;
                uint64_t slot = pack({{x, 6}, {y, 6}, {h, 1}});
                fields.push_back({slot, 13});
            } else {
                fields.push_back({0, 13});
            }
        }
        return pack(fields);
    }

    void send_sonar_report() {
        if (!State::SONAR3) return;
        std::vector<uint64_t> chunk;
        for (int i = 0; i < std::min(KELP_PER_MSG, (int)State::KELP_LIST.size()); i++) {
            chunk.push_back(State::KELP_LIST[State::KELP_SEND_CURSOR % State::KELP_LIST.size()]);
            State::KELP_SEND_CURSOR++;
        }
        for (auto d : unswbc::Direction::get_direction_list()) {
            unswbc::ct->send_sonar(d, encode_report(chunk));
        }
    }

    void read_sonar(int round_num) {
        for (uint64_t msg : unswbc::ct->get_sonar_messages()) {
            auto out = unpack(msg, {16, 1, 7, 12, 2, 13, 13});
            if (out[0] != SONAR_MAGIC) continue;
            if (out[3] == (static_cast<uint64_t>(unswbc::ct->get_id()) & ID_MASK)) continue;
            
            State::TEAMMATES[out[3]] = {out[1] != 0, static_cast<int>(out[2]), round_num};
            for (uint64_t i = 0; i < out[4]; i++) {
                auto edge_data = unpack(out[5 + i], {6, 6, 1});
                if (edge_data[0] < State::WIDTH && edge_data[1] < State::HEIGHT) {
                    add_kelp((edge_data[0] << 32) | (edge_data[1] << 1) | edge_data[2]);
                }
            }
        }
    }
}

// ============================================================================
// SEARCH
// ============================================================================
namespace Search {
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

    int room(unswbc::Position start, unswbc::Position came_from, int cap, double deadline = -1) {
        std::unordered_set<unswbc::Position, unswbc::PositionHash> seen = {came_from, start};
        std::deque<unswbc::Position> queue = {start};
        int count = 0;
        int timer = State::BFS_DEADLINE_CHECK_EVERY;

        while (!queue.empty()) {
            unswbc::Position pos = queue.front(); queue.pop_front();
            count++;
            if (count >= cap) return cap;

            if (deadline > 0) {
                if (--timer == 0) {
                    double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                    if (now >= deadline) return count;
                    timer = State::BFS_DEADLINE_CHECK_EVERY;
                }
            }

            for (auto d : unswbc::Direction::get_direction_list()) {
                if (Memory::known_edge_type(pos, d) != unswbc::EdgeType::EMPTY) continue;
                unswbc::Position nxt = pos.add_dir(d);
                if (seen.count(nxt)) continue;
                if (!tile_ever_seen(nxt)) return cap;
                if (Memory::known_occupied(nxt)) continue;
                seen.insert(nxt);
                queue.push_back(nxt);
            }
        }
        return count;
    }

    double split_room(unswbc::Position start, unswbc::Position came_from, double needed, int bfs_cap = 30) {
        std::unordered_set<unswbc::Position, unswbc::PositionHash> seen = {came_from, start};
        std::deque<unswbc::Position> queue = {start};
        double total = 0.0;
        int expanded = 0;

        while (!queue.empty()) {
            if (total >= needed || expanded >= bfs_cap) break;
            unswbc::Position pos = queue.front(); queue.pop_front();
            expanded++;

            double weight = 1.0;
            auto* tile = unswbc::ct->get_tile(pos);
            if (tile) {
                int pt = tile->get_pearl_time();
                if (tile->has_pearl() || (0 <= pt && pt <= 25)) weight = State::PEARL_ROOM_WEIGHT;
            }
            total += weight;

            for (auto d : unswbc::Direction::get_direction_list()) {
                if (Memory::known_edge_type(pos, d) != unswbc::EdgeType::EMPTY) continue;
                unswbc::Position nxt = pos.add_dir(d);
                if (seen.count(nxt)) continue;
                if (!tile_ever_seen(nxt)) return needed;
                if (Memory::known_occupied(nxt)) continue;
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

    std::unordered_set<unswbc::Position, unswbc::PositionHash> terrain_reachable(unswbc::Position start, int cap, const std::unordered_set<unswbc::Position, unswbc::PositionHash>& targets) {
        std::unordered_set<unswbc::Position, unswbc::PositionHash> seen = {start};
        std::unordered_set<unswbc::Position, unswbc::PositionHash> remaining = targets;
        remaining.erase(start);
        if (remaining.empty() && !targets.empty()) return seen;

        auto heuristic = [&](unswbc::Position p) {
            int mn = 100000;
            for (auto t : remaining) mn = std::min(mn, dist(p, t));
            return remaining.empty() ? 0 : mn;
        };

        int counter = 0;
        std::priority_queue<AStarNode, std::vector<AStarNode>, std::greater<AStarNode>> heap;
        heap.push({heuristic(start), 0, counter, start});
        std::unordered_set<unswbc::Position, unswbc::PositionHash> visited;
        int expanded = 0;

        while (!heap.empty() && expanded < cap) {
            auto node = heap.top(); heap.pop();
            if (visited.count(node.pos)) continue;
            visited.insert(node.pos);
            expanded++;

            for (auto d : unswbc::Direction::get_direction_list()) {
                if (Memory::known_edge_type(node.pos, d) != unswbc::EdgeType::EMPTY) continue;
                unswbc::Position nxt = node.pos.add_dir(d);
                if (seen.count(nxt)) continue;
                seen.insert(nxt);
                if (targets.size() > 0) {
                    remaining.erase(nxt);
                    if (remaining.empty()) return seen;
                }
                if (!tile_ever_seen(nxt)) continue;
                counter++;
                heap.push({node.g + 1 + heuristic(nxt), node.g + 1, counter, nxt});
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
        auto reachable = terrain_reachable(here, 81, c_pos);

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

    std::unordered_map<unswbc::Position, int, unswbc::PositionHash> astar_targets(
        unswbc::Position here, const std::unordered_set<unswbc::Position, unswbc::PositionHash>& fatal_zone,
        const std::vector<unswbc::Position>& target_positions, int cap, double deadline, bool avoid_fatal) 
    {
        std::unordered_map<unswbc::Position, int, unswbc::PositionHash> dist_to;
        dist_to[here] = 0;
        if (target_positions.empty()) return dist_to;

        std::unordered_set<unswbc::Position, unswbc::PositionHash> remaining(target_positions.begin(), target_positions.end());
        auto h = [&](unswbc::Position p) {
            int mn = 100000;
            for (auto t : remaining) mn = std::min(mn, dist(p, t));
            return remaining.empty() ? 0 : mn;
        };

        int counter = 0;
        std::priority_queue<AStarNode, std::vector<AStarNode>, std::greater<AStarNode>> heap;
        heap.push({h(here), 0, counter, here});
        std::unordered_set<unswbc::Position, unswbc::PositionHash> visited;
        int timer = State::BFS_DEADLINE_CHECK_EVERY;

        while (!heap.empty() && !remaining.empty() && dist_to.size() < static_cast<size_t>(cap)) {
            auto node = heap.top(); heap.pop();
            if (visited.count(node.pos)) continue;
            visited.insert(node.pos);
            remaining.erase(node.pos);

            if (deadline > 0) {
                if (--timer == 0) {
                    double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                    if (now >= deadline) break;
                    timer = State::BFS_DEADLINE_CHECK_EVERY;
                }
            }

            for (auto d : unswbc::Direction::get_direction_list()) {
                if (Memory::known_edge_type(node.pos, d) != unswbc::EdgeType::EMPTY) continue;
                unswbc::Position nxt = node.pos.add_dir(d);
                if (Memory::known_occupied(nxt)) continue;
                if (avoid_fatal && is_fatal(nxt, fatal_zone)) continue;
                int new_g = node.g + 1;
                if (dist_to.find(nxt) == dist_to.end() || new_g < dist_to[nxt]) {
                    dist_to[nxt] = new_g;
                    counter++;
                    heap.push({new_g + h(nxt), new_g, counter, nxt});
                }
            }
        }
        return dist_to;
    }

    int pearl_dist_astar(unswbc::Position pos, const std::vector<std::pair<unswbc::Position, int>>& targets,
                         const std::unordered_set<unswbc::Position, unswbc::PositionHash>& fatal_zone,
                         double deadline, bool avoid_fatal, const std::vector<unswbc::Position>& target_positions) 
    {
        if (targets.empty()) return 0;
        auto dist_to = astar_targets(pos, fatal_zone, target_positions, State::BFS_CAP, deadline, avoid_fatal);
        int best = 100000;
        for (auto& t : targets) {
            int turns = dist_to.count(t.first) ? std::max(dist_to[t.first], t.second) : std::max(dist(pos, t.first), t.second);
            best = std::min(best, turns);
        }
        return best;
    }
}

// ============================================================================
// KAMIKAZE
// ============================================================================
namespace Kamikaze {
    const double KAMIKAZE_MIN_LENGTH_RATIO = 1.1;
    const int KAMIKAZE_DODGE_CLOSE_RANGE = 2;
    const int KAMIKAZE_DODGE_SPRINT_RANGE = 3;

    unswbc::DragonPart* nearest_enemy_head(const std::vector<unswbc::DragonPart*>& others) {
        unswbc::Position here = unswbc::ct->get_position();
        unswbc::Team my_team = unswbc::ct->get_team();
        unswbc::DragonPart* best = nullptr;
        int best_dist = 100000;
        for (auto* p : others) {
            if (p->is_head() && p->get_team() != my_team) {
                int d = Search::dist(here, p->get_position());
                if (d < best_dist) { best_dist = d; best = p; }
            }
        }
        return best;
    }

    int visible_enemy_length(int target_id, const std::vector<unswbc::DragonPart*>& others) {
        int count = 0;
        for (auto* p : others) if (p->get_id() == target_id) count++;
        return count;
    }

    bool trade_is_favorable(unswbc::DragonPart* target, const std::vector<unswbc::DragonPart*>& others) {
        int our_length = unswbc::ct->get_length();
        int their_visible = visible_enemy_length(target->get_id(), others);
        return their_visible >= our_length * KAMIKAZE_MIN_LENGTH_RATIO;
    }

    std::vector<unswbc::Direction> path_onto(unswbc::Position here, unswbc::Position target_pos, int cap, double deadline) {
        if (here == target_pos) return {};
        
        struct PathNode {
            unswbc::Position pos;
            unswbc::Direction dir;
            bool valid = false;

            PathNode() : pos(0, 0), dir(unswbc::Direction::NORTH), valid(false) {}
            PathNode(unswbc::Position p, unswbc::Direction d, bool v) : pos(p), dir(d), valid(v) {}
        };
        std::unordered_map<unswbc::Position, PathNode, unswbc::PositionHash> came_from;
        std::unordered_map<unswbc::Position, int, unswbc::PositionHash> dist_to;
        dist_to[here] = 0;

        auto h = [&](unswbc::Position p) { return Search::dist(p, target_pos); };

        int counter = 0;
        std::priority_queue<Search::AStarNode, std::vector<Search::AStarNode>, std::greater<Search::AStarNode>> heap;
        heap.push({h(here), 0, counter, here});
        std::unordered_set<unswbc::Position, unswbc::PositionHash> visited;
        int timer = State::BFS_DEADLINE_CHECK_EVERY;

        while (!heap.empty()) {
            auto node = heap.top(); heap.pop();
            if (visited.count(node.pos)) continue;
            visited.insert(node.pos);

            if (node.pos == target_pos) {
                std::vector<unswbc::Direction> path;
                unswbc::Position cur = node.pos;
                while (!(cur == here)) {
                    auto it = came_from.find(cur);
                    if (it == came_from.end()) break;
                    path.push_back(it->second.dir);
                    cur = it->second.pos;
                }
                std::reverse(path.begin(), path.end());
                return path;
            }

            if (deadline > 0) {
                if (--timer == 0) {
                    double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                    if (now >= deadline) return {};
                    timer = State::BFS_DEADLINE_CHECK_EVERY;
                }
            }
            if (dist_to.size() >= static_cast<size_t>(cap)) return {};

            for (auto d : unswbc::Direction::get_direction_list()) {
                if (Memory::known_edge_type(node.pos, d) != unswbc::EdgeType::EMPTY) continue;
                unswbc::Position nxt = node.pos.add_dir(d);
                if (!(nxt == target_pos) && Memory::known_occupied(nxt)) continue;
                int new_g = node.g + 1;
                if (dist_to.find(nxt) == dist_to.end() || new_g < dist_to[nxt]) {
                    dist_to[nxt] = new_g;
                    came_from[nxt] = {node.pos, d, true};
                    counter++;
                    heap.push({new_g + h(nxt), new_g, counter, nxt});
                }
            }
        }
        return {};
    }

    bool kamikaze_charge(const std::vector<unswbc::DragonPart*>& others, double deadline) {
        if (!State::AGGRESSIVE) return false;
        auto* target = nearest_enemy_head(others);
        if (!target || !trade_is_favorable(target, others)) return false;

        auto path = path_onto(unswbc::ct->get_position(), target->get_position(), 49, deadline);
        if (path.empty() || path.size() >= static_cast<size_t>(unswbc::ct->get_length())) return false;

        unswbc::ct->make_moves(path);
        return true;
    }

    bool enemy_can_reach(unswbc::Position enemy_pos, unswbc::Position here, int max_steps) {
        auto reachable = Search::terrain_reachable(enemy_pos, 16, {here});
        return reachable.count(here) > 0;
    }

    unswbc::DragonPart* kamikaze_threat(const std::vector<unswbc::DragonPart*>& others) {
        unswbc::Position here = unswbc::ct->get_position();
        unswbc::Team my_team = unswbc::ct->get_team();
        int our_length = unswbc::ct->get_length();
        unswbc::DragonPart* best = nullptr;
        int best_dist = 100000;

        for (auto* p : others) {
            if (!p->is_head() || p->get_team() == my_team) continue;
            if (visible_enemy_length(p->get_id(), others) >= our_length) continue;
            
            int d = Search::dist(here, p->get_position());
            bool threatening = false;
            if (d <= KAMIKAZE_DODGE_CLOSE_RANGE) threatening = true;
            else if (d <= KAMIKAZE_DODGE_SPRINT_RANGE) {
                unswbc::Position nxt = p->get_position().add_dir(p->get_dir());
                threatening = Search::dist(here, nxt) < d;
            }
            if (threatening && !enemy_can_reach(p->get_position(), here, KAMIKAZE_DODGE_SPRINT_RANGE + 1)) threatening = false;
            if (threatening && d < best_dist) { best_dist = d; best = p; }
        }
        return best;
    }

    bool kamikaze_dodge_split(const std::vector<unswbc::DragonPart*>& others) {
        if (State::AGGRESSIVE) return false;
        if (!kamikaze_threat(others)) return false;
        int child = std::max(2, unswbc::ct->get_length() - 2);
        if (unswbc::ct->can_split(child)) {
            unswbc::ct->do_split(child);
            return true;
        }
        return false;
    }
}

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

    std::optional<unswbc::Direction> choose_direction(const std::vector<unswbc::DragonPart*>& others, int round_num, const std::vector<unswbc::Tile>& tiles, double deadline) {
        unswbc::Position here = unswbc::ct->get_position();
        auto targets = Search::gather_pearl_targets(here, tiles);
        
        auto cluster = Search::find_pearl_cluster(targets);
        if (!cluster.empty()) {
            targets = cluster;
        }

        std::vector<unswbc::Position> target_positions;
        for (auto& t : targets) target_positions.push_back(t.first);

        if (State::AGGRESSIVE && targets.empty()) {
            auto portals = Search::portal_directions(here, round_num);
            if (!portals.empty()) {
                int pick = (round_num * 73 + here.x * 593 + here.y * 5119) % portals.size(); 
                Memory::remember_portal_crossing(portals[pick].second, round_num);
                return portals[pick].first;
            }
        }

        std::unordered_map<unswbc::Direction, unswbc::Position, DirectionHash> moves;
        for (auto d : unswbc::Direction::get_direction_list()) {
            auto nxt = Search::open_step(here, d);
            if (nxt) moves[d] = *nxt;
        }
        if (moves.empty()) return std::nullopt;

        std::unordered_set<unswbc::Position, unswbc::PositionHash> fatal_zone;
        if (!State::AGGRESSIVE) {
            fatal_zone = Search::build_fatal_zone(others);
            std::unordered_map<unswbc::Direction, unswbc::Position, DirectionHash> safe_moves;
            for (auto& kv : moves) if (!Search::is_fatal(kv.second, fatal_zone)) safe_moves.insert(kv);
            if (!safe_moves.empty()) moves = safe_moves;
        }

        int needed = State::AGGRESSIVE ? unswbc::ct->get_length() : unswbc::ct->get_length() + 1;
        std::unordered_map<unswbc::Direction, unswbc::Position, DirectionHash> roomy;
        int best_space = -1;
        for (auto& kv : moves) {
            int sp = Search::room(kv.second, here, needed, deadline);
            best_space = std::max(best_space, sp);
            if (sp >= needed) roomy.insert(kv);
        }
        if (!roomy.empty()) moves = roomy;
        else {
            std::unordered_map<unswbc::Direction, unswbc::Position, DirectionHash> best_moves;
            for (auto& kv : moves) if (Search::room(kv.second, here, needed, deadline) == best_space) best_moves.insert(kv);
            moves = best_moves;
        }

        unswbc::Direction facing = unswbc::ct->get_dir();
        double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now >= deadline) {
            unswbc::Direction best_d = moves.begin()->first;
            auto score = [&](unswbc::Direction d) {
                return std::make_tuple(Search::pearl_dist(moves[d], targets), danger(moves[d], others), direction_to_index(d));
            };
            auto best_s = score(best_d);
            for (auto& kv : moves) {
                auto s = score(kv.first);
                if (s < best_s) { best_s = s; best_d = kv.first; }
            }
            return best_d;
        }

        unswbc::Direction best_d = moves.begin()->first;
        if (State::AGGRESSIVE) {
            auto score = [&](unswbc::Direction d) {
                return std::make_tuple(
                    Search::pearl_dist_astar(moves[d], targets, fatal_zone, deadline, false, target_positions),
                    ally_crowding(moves[d], others), d != facing, direction_to_index(d));
            };
            auto best_s = score(best_d);
            for (auto& kv : moves) {
                auto s = score(kv.first);
                if (s < best_s) { best_s = s; best_d = kv.first; }
            }
        } else {
            auto score = [&](unswbc::Direction d) {
                return std::make_tuple(
                    danger(moves[d], others),
                    Search::pearl_dist_astar(moves[d], targets, fatal_zone, deadline, true, target_positions),
                    d != facing, direction_to_index(d));
            };
            auto best_s = score(best_d);
            for (auto& kv : moves) {
                auto s = score(kv.first);
                if (s < best_s) { best_s = s; best_d = kv.first; }
            }
        }
        return best_d;
    }
}

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
    
    Memory::update_own_body();
    Memory::remember_vision(tiles);
    Memory::read_sonar(round_num);

    if (Decision::try_split(others)) return;
    if (Kamikaze::kamikaze_charge(others, deadline)) return;
    if (Kamikaze::kamikaze_dodge_split(others)) return;

    if (round_num > 400 && !Decision::enemy_nearby(others) && round_num < 490) {
        if (Decision::seppuku_conditions(others)) {
            Decision::suicide_bot();
            return;
        }
    }

    auto opt_d = Decision::choose_direction(others, round_num, tiles, deadline);
    if (!opt_d.has_value()) {
        auto fallback = Decision::last_resort(round_num);
        if (fallback.has_value()) {
            unswbc::ct->make_move(fallback.value());
        }
        return;
    }

    unswbc::ct->make_move(opt_d.value());
}

int main() {
    auto init_tuple = unswbc::init();
    State::WIDTH = unswbc::game->get_map_size().first;
    State::HEIGHT = unswbc::game->get_map_size().second;
    State::SONAR3 = true; 
    bool rolled = false;

    while (unswbc::update(*unswbc::ct, *unswbc::game)) {
        int round_num = unswbc::game->get_round_num();

        if (!rolled) {
            double progress = (round_num - 1) / 500.0;
            double aggro_chance = std::max(1.0 - 0.5 * progress, 0.3);
            State::AGGRESSIVE = stable_roll(unswbc::ct->get_id(), aggro_chance);
            rolled = true;
            if (round_num < 250) State::AGGRESSIVE = true;
        }

        if (unswbc::ct->get_length() > 5 && round_num > 1) {
            State::AGGRESSIVE = false;
        }

        execute_turn();
        Memory::send_sonar_report();
        unswbc::end_turn();
    }
    return 0;
}