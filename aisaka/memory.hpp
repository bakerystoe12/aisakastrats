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

    // One portal endpoint per sonar message.
    const int PORTAL_PER_MSG = 1;

    const uint64_t ID_MASK = (1ULL << 12) - 1;

    // Portal IDs are stored in 14 bits in the sonar packet.
    const uint64_t PORTAL_ID_MASK = (1ULL << 14) - 1;

    // ------------------------------------------------------------------------
    // Generic bit packing
    // ------------------------------------------------------------------------

    uint64_t pack(
        const std::vector<std::pair<uint64_t, int>>& fields
    ) {
        uint64_t n = 0;

        for (const auto& [value, bits] : fields) {
            n = (n << bits)
              | (value & ((1ULL << bits) - 1));
        }

        return n;
    }

    std::vector<uint64_t> unpack(
        uint64_t n,
        const std::vector<int>& widths
    ) {
        std::vector<uint64_t> out;

        for (auto it = widths.rbegin();
             it != widths.rend();
             ++it) {

            out.push_back(
                n & ((1ULL << *it) - 1)
            );

            n >>= *it;
        }

        std::reverse(out.begin(), out.end());

        return out;
    }

    // ------------------------------------------------------------------------
    // Edge keys
    // ------------------------------------------------------------------------

    uint64_t edge_key(
        unswbc::Position pos,
        unswbc::Direction d
    ) {
        if (
            d.value == unswbc::Direction::SOUTH ||
            d.value == unswbc::Direction::EAST
        ) {
            pos = pos.add_dir(d);
            d = d.get_opposite();
        }

        uint64_t horizontal =
            (d.value == unswbc::Direction::NORTH)
                ? 0
                : 1;

        return
            (static_cast<uint64_t>(pos.x) << 32)
            | (static_cast<uint64_t>(pos.y) << 1)
            | horizontal;
    }

    // ------------------------------------------------------------------------
    // KELP
    //
    // Still remembered locally because the bot needs it for pathfinding.
    // It is simply no longer transmitted over sonar.
    // ------------------------------------------------------------------------

    void add_kelp(uint64_t key) {
        State::KELP_EDGES.insert(key);
    }

    // ------------------------------------------------------------------------
    // PORTALS
    // ------------------------------------------------------------------------

    void remember_portal_crossing(
        int pid,
        int round_num
    ) {
        State::LAST_PORTAL_ID = pid;
        State::LAST_PORTAL_ROUND = round_num;
    }

    void remember_portal(
        uint64_t ekey,
        int portal_id
    ) {
        if (
            State::PORTAL_EDGES.find(ekey)
            != State::PORTAL_EDGES.end()
        ) {
            return;
        }

        State::PORTAL_EDGES[ekey] = portal_id;
        State::PORTALS[portal_id].push_back(ekey);

        // Keep a list so portal information can be sent
        // repeatedly over multiple rounds.
        State::PORTAL_LIST.push_back(ekey);
    }

    // ------------------------------------------------------------------------
    // PEARLS
    // ------------------------------------------------------------------------

    void remember_pearl(
        unswbc::Position pos,
        const unswbc::Tile& tile,
        int round_num
    ) {
        int pearl_time =
            tile.get_pearl_time();

        auto it =
            State::PEARL_MEMORY.find(pos);

        if (
            pearl_time == -1 &&
            it != State::PEARL_MEMORY.end() &&
            it->second.pearl_time == -1
        ) {
            return;
        }

        State::PEARL_MEMORY[pos] = {
            pearl_time,
            tile.has_pearl(),
            round_num
        };
    }

    // ------------------------------------------------------------------------
    // VISION
    // ------------------------------------------------------------------------

    void remember_vision(
        const std::vector<unswbc::Tile>& tiles
    ) {
        int now =
            unswbc::game->get_round_num();

        for (const auto& tile : tiles) {

            unswbc::Position pos =
                tile.get_position();

            if (
                State::KNOWN_EDGES.find(pos)
                == State::KNOWN_EDGES.end()
            ) {

                std::array<
                    unswbc::EdgeType,
                    4
                > edges;

                for (
                    auto d :
                    unswbc::Direction::get_direction_list()
                ) {

                    auto edge_type =
                        tile.get_edge(d)
                            .get_edge_type();

                    edges[direction_to_index(d)] =
                        edge_type;

                    if (
                        edge_type
                        == unswbc::EdgeType::KELP
                    ) {
                        add_kelp(
                            edge_key(pos, d)
                        );
                    }
                    else if (
                        edge_type
                        == unswbc::EdgeType::PORTAL
                    ) {
                        remember_portal(
                            edge_key(pos, d),
                            tile.get_edge(d)
                                .get_portal_id()
                        );
                    }
                }

                State::KNOWN_EDGES[pos] = edges;
            }

            remember_pearl(
                pos,
                tile,
                now
            );
        }
    }

    // ------------------------------------------------------------------------
    // EDGE KNOWLEDGE
    // -----------------------------------------------------------------------
    bool edge_is_known(unswbc::Position pos, unswbc::Direction d) {
        if (unswbc::ct->get_tile(pos))
            return true;

        if (State::KNOWN_EDGES.count(pos)) {
            return true;
        }

        uint64_t key = edge_key(pos, d);

        if (State::KELP_EDGES.count(key))
            return true;

        if (State::PORTAL_EDGES.count(key))
            return true;

        return false;
    }

    unswbc::EdgeType known_edge_type(
        unswbc::Position pos,
        unswbc::Direction d
    ) {
        State::CacheKey key{
            pos,
            direction_to_index(d)
        };

        auto it =
            State::_EDGE_CACHE.find(key);

        if (
            it != State::_EDGE_CACHE.end()
        ) {
            return it->second;
        }

        unswbc::EdgeType result =
            unswbc::EdgeType::EMPTY;

        auto* tile =
            unswbc::ct->get_tile(pos);

        if (tile) {

            result =
                tile->get_edge(d)
                    .get_edge_type();

        }
        else {

            auto known =
                State::KNOWN_EDGES.find(pos);

            if (
                known
                == State::KNOWN_EDGES.end()
            ) {

                if (
                    State::KELP_EDGES.count(
                        edge_key(pos, d)
                    )
                ) {
                    result =
                        unswbc::EdgeType::KELP;
                }
                else {
                    result =
                        unswbc::EdgeType::EMPTY;
                }

            }
            else {

                result =
                    known->second[
                        direction_to_index(d)
                    ];
            }
        }

        State::_EDGE_CACHE[key] =
            result;

        return result;
    }

    // ------------------------------------------------------------------------
    // OCCUPANCY
    // ------------------------------------------------------------------------

    bool known_occupied(
        unswbc::Position pos
    ) {
        auto it =
            State::_OCCUPIED_CACHE.find(pos);

        if (it != State::_OCCUPIED_CACHE.end()) {
            return it->second;
        }

        bool result =
            State::OWN_BODY_SET.count(pos) > 0;

        if (!result) {
            auto* tile =
                unswbc::ct->get_tile(pos);

            if (tile) {
                result =
                    (tile->get_dragon() != nullptr);
            }
        }

        State::_OCCUPIED_CACHE[pos] = result;

        return result;
    }

    // ------------------------------------------------------------------------
    // OWN BODY
    // ------------------------------------------------------------------------

    void update_own_body() {

        unswbc::Position here =
            unswbc::ct->get_position();

        if (
            State::OWN_BODY.empty() ||
            !(State::OWN_BODY.front() == here)
        ) {
            State::OWN_BODY.push_front(here);
        }

        int length =
            unswbc::ct->get_length();

        while (
            State::OWN_BODY.size()
            > static_cast<size_t>(length)
        ) {
            State::OWN_BODY.pop_back();
        }

        State::OWN_BODY_SET.clear();

        for (
            const auto& p :
            State::OWN_BODY
        ) {
            State::OWN_BODY_SET.insert(p);
        }
    }

    void predict_own_body(
        const std::vector<unswbc::Position>& positions,
        int length
    ) {
        int predicted_length = length;

        for (const auto& pos : positions) {

            State::OWN_BODY.push_front(pos);

            auto pearl_it = State::PEARL_MEMORY.find(pos);

            if (
                pearl_it != State::PEARL_MEMORY.end() &&
                pearl_it->second.has_pearl &&
                pearl_it->second.pearl_time != -1
            ) {
                ++predicted_length;

                pearl_it->second.has_pearl = false;
            }

            while (
                State::OWN_BODY.size()
                > static_cast<size_t>(predicted_length)
            ) {
                State::OWN_BODY.pop_back();
            }
        }

        State::OWN_BODY_SET.clear();

        for (const auto& pos : State::OWN_BODY) {
            State::OWN_BODY_SET.insert(pos);
        }
    }

    // ------------------------------------------------------------------------
    // PORTAL SONAR ENCODING
    //
    // 16  magic
    //  1  aggressive
    //  7  length
    // 12  sender ID
    //  1  has_portal
    // 14  portal ID
    //  6  x
    //  6  y
    //  1  horizontal
    //
    // Total = 64 bits
    // ------------------------------------------------------------------------

    uint64_t encode_report(
        std::optional<uint64_t> portal_key
    ) {
        uint64_t has_portal =
            portal_key.has_value()
                ? 1
                : 0;

        uint64_t portal_id = 0;
        uint64_t x = 0;
        uint64_t y = 0;
        uint64_t horizontal = 0;

        if (portal_key.has_value()) {

            uint64_t key =
                portal_key.value();

            auto portal_it =
                State::PORTAL_EDGES.find(key);

            if (
                portal_it !=
                State::PORTAL_EDGES.end()
            ) {
                portal_id =
                    static_cast<uint64_t>(
                        portal_it->second
                    );

                x = key >> 32;
                y = (key >> 1)
                    & 0x7FFFFFFF;
                horizontal =
                    key & 1;
            }
        }

        std::vector<
            std::pair<uint64_t, int>
        > fields = {

            {SONAR_MAGIC, 16},

            {
                static_cast<uint64_t>(
                    State::AGGRESSIVE
                ),
                1
            },

            {
                static_cast<uint64_t>(
                    std::min(
                        unswbc::ct->get_length(),
                        127
                    )
                ),
                7
            },

            {
                static_cast<uint64_t>(
                    unswbc::ct->get_id()
                ) & ID_MASK,
                12
            },

            {has_portal, 1},

            {
                portal_id & PORTAL_ID_MASK,
                14
            },

            {x, 6},
            {y, 6},
            {horizontal, 1}
        };

        return pack(fields);
    }

    // ------------------------------------------------------------------------
    // SEND SONAR
    // ------------------------------------------------------------------------

    void send_sonar_report() {

        std::optional<uint64_t>
            portal_key;

        if (
            !State::PORTAL_LIST.empty()
        ) {
            portal_key =
                State::PORTAL_LIST[
                    State::PORTAL_SEND_CURSOR
                    % State::PORTAL_LIST.size()
                ];

            State::PORTAL_SEND_CURSOR++;
        }

        uint64_t message =
            encode_report(portal_key);

        for (
            auto d :
            unswbc::Direction::get_direction_list()
        ) {
            unswbc::ct->send_sonar(
                d,
                message
            );
        }
    }

    // ------------------------------------------------------------------------
    // READ SONAR
    // ------------------------------------------------------------------------

    void read_sonar(
        int round_num
    ) {
        for (
            uint64_t msg :
            unswbc::ct->get_sonar_messages()
        ) {

            auto out =
                unpack(
                    msg,
                    {
                        16, // magic
                        1,  // aggressive
                        7,  // length
                        12, // sender ID
                        1,  // has portal
                        14, // portal ID
                        6,  // x
                        6,  // y
                        1   // horizontal
                    }
                );

            if (
                out[0] != SONAR_MAGIC
            ) {
                continue;
            }

            // Ignore our own reports.
            if (
                out[3]
                == (
                    static_cast<uint64_t>(
                        unswbc::ct->get_id()
                    ) & ID_MASK
                )
            ) {
                continue;
            }

            // Still share teammate information.
            State::TEAMMATES[out[3]] = {
                out[1] != 0,
                static_cast<int>(out[2]),
                round_num
            };

            // No portal in this message.
            if (out[4] == 0)
                continue;

            uint64_t portal_id =
                out[5];

            uint64_t x =
                out[6];

            uint64_t y =
                out[7];

            uint64_t horizontal =
                out[8];

            // Coordinates must be on the actual map.
            if (
                x >= static_cast<uint64_t>(
                    State::WIDTH
                ) ||
                y >= static_cast<uint64_t>(
                    State::HEIGHT
                )
            ) {
                continue;
            }

            if (horizontal > 1)
                continue;

            uint64_t key =
                (x << 32)
                | (y << 1)
                | horizontal;

            remember_portal(
                key,
                static_cast<int>(portal_id)
            );
        }
    }
}