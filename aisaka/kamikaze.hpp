#pragma once

#include "helper.hpp"
#include <deque>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <cstdint>

// ============================================================================
// KAMIKAZE
// ============================================================================
namespace Kamikaze {
    double KAMIKAZE_MIN_LENGTH_RATIO = 1.1;
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

        if (target->get_id() == 0 || target->get_id() == 1){   // if queen we kill
            return true;
        }

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
                    came_from[nxt] = PathNode(node.pos, d, true);
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

        int length = unswbc::ct->get_length();

        int max_sprint = length + ((length + 3) / 4) - 1;

        auto path = path_onto(unswbc::ct->get_position(), target->get_position(), 49, deadline);

        if (path.empty() || path.size() >= static_cast<size_t>(max_sprint)) return false;

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