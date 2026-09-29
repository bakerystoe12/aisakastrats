#pragma once

#include "helper.hpp"
#include <deque>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <optional>



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
                } else{
                    result = unswbc::EdgeType::EMPTY;
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