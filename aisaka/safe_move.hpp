// safe_move.hpp - ally-aware move filtering for unswbc dragons (C++ port).
//
// Two checks run on every candidate step:
//
//   1. Dead-end prevention: a time-aware flood fill from the new head tile.
//      Our own tail tiles count as free once the tail would have moved off
//      them. If the fill reaches the edge of our 7x7 vision (or a portal) we
//      cannot prove a dead end, so the move is treated as open.
//
//   2. Ally-block simulation: for every visible ally head, count its exits
//      and its reachable space before and after our move. If our move leaves
//      an ally with no exits, or seals it into a pocket smaller than its
//      body, the move is vetoed (heavily penalised).
//
// Entry point:
//     std::optional<Direction> choose_move(Controller& ct,
//                                          std::vector<Direction> const& preferred = {},
//                                          bool debug = false);
//   preferred: directions, best first, from your own pathfinder.
//              Safety always beats it.
//
// Usage:
//     auto [ct, game] = unswbc::init();
//     while (unswbc::update(ct, game)) {
//         std::vector<Direction> preferred = my_pathfinder(ct);
//         auto move = safe_move::choose_move(ct, preferred);
//         ct.make_move(move.value_or(Direction(Direction::NORTH)));
//         unswbc::end_turn();
//     }
#pragma once

#include "helper.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace safe_move {

using unswbc::Controller;
using unswbc::Direction;
using unswbc::DragonPart;
using unswbc::EdgeType;
using unswbc::Position;
using unswbc::PositionHash;
using unswbc::Team;
using unswbc::Tile;

using PosSet = std::unordered_set<Position, PositionHash>;
template <typename T> using PosMap = std::unordered_map<Position, T, PositionHash>;

inline const std::array<Direction, 4> DIRS = Direction::get_direction_list();

constexpr int INF = 1'000'000'000;
constexpr int BIG_AREA = 49;       // size of the whole vision window
constexpr double SAFE_FACTOR = 1.0; // needed space = SAFE_FACTOR * body length
constexpr int MIN_AREA = 4;

// scoring weights
constexpr int W_PREF = 10;          // per rank step in `preferred`
constexpr int W_ROOM = 5;           // prefer open space when nothing else decides
constexpr int P_DEAD_END = 1000;
constexpr int P_BLOCK_ALLY = 1000;  // per ally blocked
constexpr int P_HEAD_RISK = 50;     // per other dragon head adjacent to the destination

enum class Status { Open, Blocked, Portal, Unseen };

struct Neighbor {
    Status status;
    Position next; // only meaningful when status == Open
};

// Ordered body positions (head -> tail) plus whether the real tail is visible.
struct Chain {
    std::vector<Position> tiles;
    bool complete = false;
};

// Everything we learn about one candidate step.
struct Info {
    Direction dir;
    int area;
    bool frontier;
    bool dead_end;
    std::vector<int> blocks; // ids of allies this step would trap
    int risk;
    int needed;
};

// How much free space a dragon of this length needs to be considered safe.
inline int needed(int length) {
    return std::max(MIN_AREA, static_cast<int>(std::ceil(SAFE_FACTOR * length)));
}

class View {
  public:
    explicit View(Controller const& controller) : ct_(controller) {
        head_ = ct_.get_position();
        my_id_ = ct_.get_id();
        my_team_ = ct_.get_team();
        scan();
        build_chains();
    }

    // ---------------------------------------------------------- geometry
    // Classifies stepping from `from` in direction `d`. Ignores dragons;
    // only looks at edges (kelp / portal) and whether the next tile is visible.
    Neighbor neighbor(Position from, Direction d) const {
        auto const type = tile_of_.at(from)->get_edge(d).get_edge_type();
        if (type == EdgeType::KELP)
            return {Status::Blocked, {}};
        if (type == EdgeType::PORTAL)
            return {Status::Portal, {}}; // far side unknown, treat as an escape
        Position const next = from.add_dir(d);
        if (tile_of_.contains(next))
            return {Status::Open, next};
        return {Status::Unseen, {}};
    }

    // Number of directions a head could step without dying, given the set of
    // occupied tiles `occ`.
    int exits(Position head, PosSet const& occ) const {
        int n = 0;
        for (Direction d : DIRS) {
            auto const nb = neighbor(head, d);
            if (nb.status == Status::Portal || nb.status == Status::Unseen ||
                (nb.status == Status::Open && !occ.contains(nb.next)))
                ++n;
        }
        return n;
    }

    // -------------------------------------------------------- flood fill
    // Time-aware flood fill for dragon `did` whose head will sit on `start`.
    //   stat  : tiles blocked forever (other dragons, new heads...)
    //   t0    : moves already made when the first BFS step happens
    //           (1 for us, 0 for an ally that has not moved yet)
    //   shift : 1 if our move eats a pearl (tail does not advance)
    // The dragon's own body segment k (0 = head) frees up once enough moves
    // have passed; if the true tail isn't visible the body is blocked forever.
    // Returns {reachable tile count, touched the frontier/portal}.
    std::pair<int, bool> space(int did, Position start, int t0, int shift, PosSet const& stat) const {
        PosMap<int> release;
        if (auto const it = chains_.find(did); it != chains_.end()) {
            auto const& chain = it->second;
            int const len = static_cast<int>(chain.tiles.size());
            for (int k = 0; k < len; ++k)
                release[chain.tiles[k]] = chain.complete ? (len - k + shift) : INF;
        }

        auto const blocked_at = [&](Position key, int depth) -> bool {
            if (auto const r = release.find(key); r != release.end())
                return (depth - 1 + t0) < r->second;
            return stat.contains(key);
        };

        PosSet seen{start};
        std::deque<std::pair<Position, int>> q;
        q.emplace_back(start, 0);
        int area = 0;
        bool frontier = false;
        while (!q.empty()) {
            auto const [cur, m] = q.front();
            q.pop_front();
            for (Direction d : DIRS) {
                auto const nb = neighbor(cur, d);
                if (nb.status == Status::Portal || nb.status == Status::Unseen) {
                    frontier = true;
                    continue;
                }
                if (nb.status == Status::Blocked || seen.contains(nb.next) || blocked_at(nb.next, m + 1))
                    continue;
                seen.insert(nb.next);
                ++area;
                q.emplace_back(nb.next, m + 1);
            }
        }
        return {area, frontier};
    }

    // ----------------------------------------------------- ally simulation
    // Ids of visible allies that moving our head onto `dest` would trap.
    std::vector<int> blocked_allies(Position dest, bool pearl) const {
        auto const& mine = chains_.at(my_id_);
        PosSet before;
        for (auto const& [k, _] : part_of_)
            before.insert(k);
        PosSet after = before;
        if (mine.complete && !pearl)
            after.erase(mine.tiles.back()); // our tail leaves its tile
        after.insert(dest);                 // our new head occupies dest

        std::vector<int> trapped;
        for (auto const& [aid, achain] : chains_) {
            if (aid == my_id_ || !(team_of_.at(aid) == my_team_))
                continue;
            Position const ahead = achain.tiles.front();

            if (exits(ahead, before) > 0 && exits(ahead, after) == 0) {
                trapped.push_back(aid);
                continue;
            }
            int const need = needed(static_cast<int>(achain.tiles.size()));
            auto const [a0, f0] = space(aid, ahead, 0, 0, before);
            auto const [a1, f1] = space(aid, ahead, 0, 0, after);
            bool const fine_before = f0 || a0 >= need;
            bool const fine_after = f1 || a1 >= need;
            if (fine_before && !fine_after)
                trapped.push_back(aid);
        }
        return trapped;
    }

    // ---------------------------------------------------------- evaluation
    // nullopt if the step kills us outright, otherwise the facts about it.
    std::optional<Info> evaluate(Direction d) const {
        auto const nb = neighbor(head_, d);
        if (nb.status == Status::Blocked)
            return std::nullopt;
        if (nb.status == Status::Open && part_of_.contains(nb.next))
            return std::nullopt; // any dragon part, including our own tail
        if (nb.status == Status::Portal || nb.status == Status::Unseen)
            return Info{d, BIG_AREA, true, false, {}, 0, MIN_AREA};

        Position const dest = nb.next;
        auto const& mine = chains_.at(my_id_);
        bool const pearl = tile_of_.at(dest)->has_pearl();
        int const shift = pearl ? 1 : 0;

        PosSet all;
        for (auto const& [k, _] : part_of_)
            all.insert(k);
        auto const [area, frontier] = space(my_id_, dest, 1, shift, all);
        int const need = needed(static_cast<int>(mine.tiles.size()) + shift);
        bool const dead_end = !frontier && area < need;

        int risk = 0;
        for (Direction d2 : DIRS) {
            auto const nb2 = neighbor(dest, d2);
            if (nb2.status != Status::Open)
                continue;
            auto const p = part_of_.find(nb2.next);
            if (p != part_of_.end() && p->second.is_head() && p->second.get_id() != my_id_)
                ++risk;
        }

        return Info{d, area, frontier, dead_end, blocked_allies(dest, pearl), risk, need};
    }

  private:
    // Reads every visible tile (and the dragon part on it) from the controller.
    void scan() {
        for (Tile const& tile : ct_.get_tiles()) {
            Position const p = tile.get_position();
            tile_of_[p] = &tile;
            if (DragonPart const* part = tile.get_dragon()) {
                part_of_.insert_or_assign(p, *part);
                team_of_.insert_or_assign(part->get_id(), part->get_team());
            }
        }
    }

    // For each visible head, walk its body from head to tail by following the
    // segments whose facing points back at the previous one.
    void build_chains() {
        for (auto const& [k, part] : part_of_) {
            if (!part.is_head())
                continue;
            int const did = part.get_id();
            Chain chain;
            chain.tiles.push_back(k);
            Position cur = k;
            while (true) {
                std::optional<Position> next;
                for (Direction d : DIRS) {
                    Position const nk = cur.add_dir(d);
                    auto const p = part_of_.find(nk);
                    if (p == part_of_.end() || p->second.is_head() || p->second.get_id() != did)
                        continue;
                    if (std::find(chain.tiles.begin(), chain.tiles.end(), nk) != chain.tiles.end())
                        continue;
                    // body parts point towards the head
                    if (nk.add_dir(p->second.get_dir()) == cur) {
                        next = nk;
                        break;
                    }
                }
                if (!next)
                    break;
                chain.tiles.push_back(*next);
                cur = *next;
            }
            // The true tail is visible only if all four of its neighbours are.
            chain.complete = std::all_of(DIRS.begin(), DIRS.end(), [&](Direction d) {
                return tile_of_.contains(cur.add_dir(d));
            });
            chains_.insert_or_assign(did, std::move(chain));
        }
    }

    Controller const& ct_;
    Position head_;
    int my_id_;
    Team my_team_ = Team(Team::A);

    PosMap<Tile const*> tile_of_;
    PosMap<DragonPart> part_of_;
    std::unordered_map<int, Team> team_of_;
    std::unordered_map<int, Chain> chains_;
};

// Picks the safest step, using `preferred` (best first) as a tie-breaker.
// Returns nullopt if every step is fatal.
inline std::optional<Direction> choose_move(Controller& ct, std::vector<Direction> const& preferred = {},
                                            bool debug = false) {
    View const view(ct);

    std::optional<Direction> best;
    double best_score = -static_cast<double>(INF);

    for (Direction d : DIRS) {
        auto const info = view.evaluate(d);
        if (!info)
            continue;

        int bonus = 0;
        for (std::size_t i = 0; i < preferred.size(); ++i) {
            if (preferred[i] == d) {
                bonus = static_cast<int>(preferred.size() - i);
                break;
            }
        }

        double const cap = 3.0 * info->needed;
        double const room = std::min<double>(info->area, cap) / cap;
        double const score = W_PREF * bonus + W_ROOM * room - P_DEAD_END * (info->dead_end ? 1 : 0) -
                             P_BLOCK_ALLY * static_cast<int>(info->blocks.size()) - P_HEAD_RISK * info->risk;

        if (debug) {
            std::ostringstream out;
            out << d << ": area=" << info->area << " frontier=" << info->frontier
                << " dead_end=" << info->dead_end << " blocks=" << info->blocks.size()
                << " risk=" << info->risk << " score=" << score;
            ct.output_log(out.str());
        }
        if (score > best_score) {
            best = d;
            best_score = score;
        }
    }
    return best;
}

} // namespace safe_move
