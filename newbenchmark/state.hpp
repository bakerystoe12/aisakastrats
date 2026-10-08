#pragma once

#include "helper.hpp"
#include <deque>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <cstdint>

// ============================================================================
// STATE
// ============================================================================
namespace State {
    inline int WIDTH = 0;
    inline int HEIGHT = 0;
    inline bool AGGRESSIVE = true;
    inline bool IS_QUEEN = false;

    inline const int BFS_CAP = 1500;
    inline const int UNEXPLORED_BFS_CAP = 500;
    inline const double TURN_TIME_BUDGET = 0.75;
    inline const int BFS_DEADLINE_CHECK_EVERY = 5;
    inline int SAFE_DANGER_RADIUS = 4;

    bool ENTERED_DEAD_END = false;
    inline constexpr int MAX_FRONTIER_TARGETS = 5;

    inline std::unordered_set<
        unswbc::Position,
        unswbc::PositionHash
    > EXPLORATION_VISITED;

    inline const int MAX_NEARBY_ALLIES_TO_SPLIT = 4;
    inline const int PEARL_SPAWN_HORIZON = 10;
    inline int SPLIT_MIN_LENGTH = 4;

    inline constexpr int DEAD_END_LOOKAHEAD = 64;
    inline int DEAD_END_PEARL_THRESHOLD = 2;

    inline std::deque<unswbc::Position> OWN_BODY;
    inline std::unordered_set<unswbc::Position, unswbc::PositionHash> OWN_BODY_SET;
    inline std::unordered_set<unswbc::Position, unswbc::PositionHash> PEARL_CAPABLE;

    inline std::unordered_set<uint64_t> KELP_EDGES;

    inline std::vector<uint64_t> PORTAL_LIST;
    inline int PORTAL_SEND_CURSOR = 0;


    struct Teammate { bool aggressive; int length; int round; };
    inline std::unordered_map<int, Teammate> TEAMMATES;

    inline std::unordered_map<unswbc::Position, std::array<unswbc::EdgeType, 4>, unswbc::PositionHash> KNOWN_EDGES;

    inline int LAST_PORTAL_ID = -1;
    inline int LAST_PORTAL_ROUND = -1000000000;

    struct PearlData { int pearl_time; bool has_pearl; int round; };
    inline std::unordered_map<unswbc::Position, PearlData, unswbc::PositionHash> PEARL_MEMORY;

    inline std::unordered_map<uint64_t, int> PORTAL_EDGES;
    inline std::unordered_map<int, std::vector<uint64_t>> PORTALS;

    struct CacheKey {
        unswbc::Position pos; int d;
        bool operator==(const CacheKey& o) const { return pos == o.pos && d == o.d; }
    };
    struct CacheKeyHash {
        std::size_t operator()(const CacheKey& k) const {
            return unswbc::PositionHash()(k.pos) ^ (k.d << 16);
        }
    };

    inline std::unordered_map<CacheKey, unswbc::EdgeType, CacheKeyHash> _EDGE_CACHE;
    inline std::unordered_map<unswbc::Position, bool, unswbc::PositionHash> _OCCUPIED_CACHE;

    inline void reset_turn_caches() {
        _EDGE_CACHE.clear();
        _OCCUPIED_CACHE.clear();
    }
}