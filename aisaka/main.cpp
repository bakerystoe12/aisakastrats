#include "helper.hpp"

#include <algorithm>
#include <random>

int main()
{
    auto [ct, game] = unswbc::init();

    // Seed so we get the same random generator every time.
    std::mt19937 shuffler(0);

    while (unswbc::update(ct, game))
    {
        // Step onto the first open neighbouring tile.
        auto const here = ct.get_position();
        auto const* hereTile = ct.get_tile(here);
        bool moved = false;

        auto directions = unswbc::Direction::get_direction_list();
        std::shuffle(directions.begin(), directions.end(), shuffler);

        for (auto const direction : directions)
        {
            if (hereTile && !hereTile->get_edge(direction).is_passable())
            {
                continue;
            }

            auto const* ahead = ct.get_tile(here.add_dir(direction));
            if (ahead && ahead->get_dragon())
            {
                continue;
            }

            ct.output_log("Moving in", direction);
            ct.make_move(direction);
            moved = true;
            break;
        }

        if (!moved)
        {
            ct.make_move(unswbc::Direction::NORTH);
        }
        unswbc::end_turn();
    }
}
