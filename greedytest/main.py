import helper as unswbc
from helper import Direction, EdgeType
import random

ct: unswbc.Controller
game: unswbc.Game

SPLIT_MIN_LENGTH = 6   # don't split until the dragon is at least this long


# ---------- helpers ----------

def dist(a, b):
    """Manhattan distance that accounts for the map wrapping around."""
    w, h = game.get_map_size()
    dx = abs(a.x - b.x)
    dx = min(dx, w - dx)
    dy = abs(a.y - b.y)
    dy = min(dy, h - dy)
    return dx + dy


def can_step(pos, d):
    """True if stepping d from pos is not blocked by kelp or a dragon.
    Portals are treated as walls because add_dir gives the wrong tile across one."""
    tile = ct.get_tile(pos)
    if tile is None:
        return False
    if tile.get_edge(d).get_edge_type() != EdgeType.EMPTY:
        return False
    ahead = ct.get_tile(pos.add_dir(d))
    if ahead is None:
        return False
    return ahead.get_dragon() is None


def near_enemy_head(pos):
    """True if an enemy head is next to pos (it could step into us and kill both)."""
    me = ct.get_team()
    for d in Direction.get_direction_list():
        tile = ct.get_tile(pos.add_dir(d))
        if tile is None:
            continue
        part = tile.get_dragon()
        if part is not None and part.is_head() and part.get_team() != me:
            return True
    return False


def pick_target():
    """Nearest visible pearl, else the tile that will try to spawn one soonest, else None."""
    here = ct.get_position()
    tiles = ct.get_tiles()

    pearls = [t.get_position() for t in tiles if t.has_pearl()]
    if pearls:
        return min(pearls, key=lambda p: dist(here, p))

    # no pearl in view: head for a tile that will spawn one soon
    soon = [t for t in tiles
            if t.get_pearl_time() >= 0 and dist(here, t.get_position()) > 0]
    if soon:
        best = min(soon, key=lambda t: max(t.get_pearl_time(),
                                           dist(here, t.get_position())))
        return best.get_position()

    return None


# ---------- strategy ----------

def greedy() -> None:
    here = ct.get_position()

    # 1. split if we can (splitting is our action for the turn)
    if ct.get_length() >= SPLIT_MIN_LENGTH:
        child = ct.get_length() // 2
        if ct.can_split(child):
            ct.do_split(child)
            return

    # 2. order the four directions by how good they are
    dirs = Direction.get_direction_list()
    random.shuffle(dirs)                      # random tie-breaks (sort is stable)

    target = pick_target()
    if target is not None:
        ct.draw_indicator_dot(target, 255, 0, 0)
        dirs.sort(key=lambda d: dist(here.add_dir(d), target))
    else:
        facing = ct.get_dir()
        dirs.sort(key=lambda d: d != facing)  # nothing to chase: keep going straight

    # 3. take the best direction that is safe, preferring ones away from enemy heads
    safe = [d for d in dirs if can_step(here, d)]
    calm = [d for d in safe if not near_enemy_head(here.add_dir(d))]
    options = calm or safe

    if options:
        ct.make_move(options[0])
    else:
        ct.make_move(ct.get_dir())            # boxed in, nothing safe left


# ---------- boilerplate ----------

def execute_turn() -> None:
    greedy()


def main() -> None:
    global ct, game
    ct, game = unswbc.init()

    while unswbc.update(ct, game):
        execute_turn()
        unswbc.end_turn()


if __name__ == "__main__":
    main()