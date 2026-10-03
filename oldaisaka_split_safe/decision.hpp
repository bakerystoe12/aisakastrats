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


    // ------------------------------------------------------------------------
    // SPLIT FATE PREDICTION
    //
    // A split is not considered safe merely because there is enough room
    // around the current head. The tail becomes a new dragon immediately and
    // that child gets its own turn later in the same round. We therefore
    // predict the first move available to BOTH resulting heads.
    // ------------------------------------------------------------------------
    struct SplitFate {
        bool safe = false;
        int parent_options = 0;
        int child_options = 0;
        int parent_best = 0;
        int child_best = 0;
        int ally_conflicts = 0;
    };

    bool split_edge_open(unswbc::Position pos, unswbc::Direction d) {
        return Memory::known_edge_type(pos, d) == unswbc::EdgeType::EMPTY;
    }

    bool split_position_blocked(
        unswbc::Position pos,
        const std::unordered_set<unswbc::Position, unswbc::PositionHash>& blocked) {
        return blocked.count(pos) != 0;
    }

    int split_safe_followups(
        unswbc::Position candidate,
        unswbc::Position old_head,
        const std::unordered_set<unswbc::Position, unswbc::PositionHash>& blocked,
        const std::vector<unswbc::DragonPart*>& others) {

        int safe = 0;
        unswbc::Team my_team = unswbc::ct->get_team();

        for (auto d : unswbc::Direction::get_direction_list()) {
            if (!split_edge_open(candidate, d)) continue;

            auto next = candidate.add_dir(d);

            // The old head has become body after this move, so it remains
            // blocked for the following step.
            if (next == old_head) continue;
            if (split_position_blocked(next, blocked)) continue;

            bool enemy_head = false;
            bool ally_head = false;
            for (auto* p : others) {
                if (p->get_position() != next) continue;
                if (p->get_team() == my_team) ally_head = p->is_head();
                else enemy_head = p->is_head();
            }

            if (enemy_head) continue;
            if (ally_head) continue;

            ++safe;
        }
        return safe;
    }

    SplitFate predict_split_fate(
        int child_size,
        const std::vector<unswbc::DragonPart*>& others,
        bool emergency = false) {

        SplitFate fate;
        const int length = unswbc::ct->get_length();
        const int parent_size = length - child_size;

        if (child_size < unswbc::Constants::MIN_SIZE ||
            parent_size < unswbc::Constants::MIN_SIZE ||
            !unswbc::ct->can_split(child_size) ||
            State::OWN_BODY.size() < static_cast<size_t>(length)) {
            return fate;
        }

        // After a split every existing segment still belongs to one of the
        // two dragons. Block the complete current body so the prediction is
        // deliberately conservative.
        std::unordered_set<unswbc::Position, unswbc::PositionHash> blocked;
        for (const auto& p : State::OWN_BODY) blocked.insert(p);

        // The child head is the old tail because the split-off section is
        // reversed before becoming the new dragon.
        unswbc::Position parent_head = State::OWN_BODY.front();
        unswbc::Position child_head = State::OWN_BODY.back();

        // Other visible dragons are also treated as occupied.
        for (auto* p : others) blocked.insert(p->get_position());

        // Heads themselves are not permanently occupied once they move, but
        // we keep the OTHER split head blocked to prevent a one-turn
        // head-to-head/self-blocking prediction.
        std::unordered_set<unswbc::Position, unswbc::PositionHash> parent_blocked = blocked;
        std::unordered_set<unswbc::Position, unswbc::PositionHash> child_blocked = blocked;
        parent_blocked.insert(child_head);
        child_blocked.insert(parent_head);

        auto evaluate_head =
            [&](unswbc::Position head,
                const std::unordered_set<unswbc::Position, unswbc::PositionHash>& local_blocked,
                int& option_count,
                int& best_followups) {

                int best = 0;
                for (auto d : unswbc::Direction::get_direction_list()) {
                    if (!split_edge_open(head, d)) continue;

                    auto next = head.add_dir(d);
                    if (split_position_blocked(next, local_blocked)) continue;

                    bool enemy_near = false;
                    bool ally_conflict = false;

                    for (auto* p : others) {
                        int dist = Search::dist(next, p->get_position());

                        if (p->get_team() == unswbc::ct->get_team()) {
                            if (p->is_head() && dist <= 1) {
                                ally_conflict = true;
                            }
                        } else if (p->is_head() && dist <= 1) {
                            enemy_near = true;
                        }
                    }

                    // A non-emergency split should not create a head which
                    // immediately enters an enemy-head danger square.
                    if (!emergency && enemy_near) continue;

                    int followups =
                        split_safe_followups(next, head, local_blocked, others);

                    // A move with no continuation is a one-turn dead end.
                    if (!emergency && followups == 0) continue;

                    if (ally_conflict) {
                        ++fate.ally_conflicts;
                    }

                    ++option_count;
                    best = std::max(best, followups);
                }

                best_followups = best;
            };

        evaluate_head(parent_head, parent_blocked,
                      fate.parent_options, fate.parent_best);
        evaluate_head(child_head, child_blocked,
                      fate.child_options, fate.child_best);

        // Normal split: both resulting dragons need at least one viable
        // first move AND at least one continuation. Prefer two choices so a
        // dragon is not forced down a single corridor.
        const int min_options = emergency ? 1 : 1;
        const int min_followups = emergency ? 0 : 1;

        fate.safe =
            fate.parent_options >= min_options &&
            fate.child_options >= min_options &&
            fate.parent_best >= min_followups &&
            fate.child_best >= min_followups;

        // If the split creates several immediate ally conflicts, it is much
        // more likely that our own dragons will box each other in. Only an
        // emergency split is allowed to ignore this.
        if (!emergency && fate.ally_conflicts >= 2) {
            fate.safe = false;
        }

        return fate;
    }

    bool split_is_safe(
        int child_size,
        const std::vector<unswbc::DragonPart*>& others,
        bool emergency = false) {
        return predict_split_fate(child_size, others, emergency).safe;
    }

    bool desperate_split(const std::vector<unswbc::DragonPart*>& others) {
        int length = unswbc::ct->get_length();
        if (length < 2 * unswbc::Constants::MIN_SIZE) return false;
        int child = std::max(unswbc::Constants::MIN_SIZE, length - unswbc::Constants::MIN_SIZE);
        if (unswbc::ct->can_split(child) && split_is_safe(child, others, true)) {
            unswbc::ct->do_split(child);
            return true;
        }
        return false;
    }

    std::optional<unswbc::Direction> last_resort(int round_num, const std::vector<unswbc::DragonPart*>& others) {
        unswbc::Position here = unswbc::ct->get_position();
        auto portals = Search::portal_directions(here, round_num);
        if (!portals.empty()) {
            Memory::remember_portal_crossing(portals[0].second, round_num);
            return portals[0].first;
        }
        if (desperate_split(others)) return std::nullopt;

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

        if (unswbc::ct->can_split(child) &&
            split_is_safe(child, others)) {
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