"""
Pathfinding priorities, highest first. Each tier only chooses between the moves
the tier above it left over.

  1. Never step into kelp or onto a dragon.
  2. Avoid dead ends (pockets too small for our length).
  3. Avoid other dragons (especially heads).
  4. Get pearls.
"""
import helper as unswbc
from helper import Direction, EdgeType
import random
from collections import deque

ct: unswbc.Controller
game: unswbc.Game

# random on average we have 90% that aggro split and get pearls, and 10% play safe, dont split

SPLIT_ENABLED = True
SPLIT_MIN_LENGTH = 20      # only split when at least this long (and no enemy head in view)


# ---------- helpers ----------

def dist(a, b):
    """Manhattan distance that accounts for the map wrapping around."""
    w, h = game.get_map_size()
    dx = abs(a.x - b.x)
    dx = min(dx, w - dx)
    dy = abs(a.y - b.y)
    dy = min(dy, h - dy)
    return dx + dy


def other_dragon_parts():
    """Every visible dragon segment that isn't part of this dragon."""
    my_id = ct.get_id()
    parts = []
    for t in ct.get_tiles():
        part = t.get_dragon()
        if part is not None and part.get_id() != my_id:
            parts.append(part)
    return parts


# ---------- tier 1: obstacles ----------

def open_step(pos, d):
    """Landing position if stepping d from pos is legal, else None.
    Blocked by kelp, by a dragon on the tile ahead, or by an unknown tile.
    Portals count as blocked because add_dir gives the wrong tile across one."""
    tile = ct.get_tile(pos)
    if tile is None:
        return None
    if tile.get_edge(d).get_edge_type() != EdgeType.EMPTY:
        return None
    nxt = pos.add_dir(d)
    ahead = ct.get_tile(nxt)
    if ahead is None or ahead.get_dragon() is not None:
        return None
    return nxt


# ---------- tier 2: dead ends ----------

def room(start, came_from, cap):
    """Flood fill from start over free tiles in view. Returns how many tiles are
    reachable, stopping at cap. Reaching the edge of our view counts as
    'plenty of room' because we can't see what lies beyond."""
    seen = {came_from, start}       # came_from is our head, which becomes body
    queue = deque([start])
    count = 0
    while queue:
        pos = queue.popleft()
        count += 1
        if count >= cap:
            return cap
        tile = ct.get_tile(pos)
        for d in Direction.get_direction_list():
            if tile.get_edge(d).get_edge_type() != EdgeType.EMPTY:
                continue            # kelp; portals ignored to stay conservative
            nxt = pos.add_dir(d)
            if nxt in seen:
                continue
            ahead = ct.get_tile(nxt)
            if ahead is None:
                return cap          # open tile beyond our view: not a dead end
            if ahead.get_dragon() is not None:
                continue
            seen.add(nxt)
            queue.append(nxt)
    return count


# ---------- tier 3: dragons ----------

def danger(pos, others):
    """How risky it is to end a step on pos. Heads matter most, since a head-on
    hit kills both dragons and enemies may sprint several tiles."""
    my_team = ct.get_team()
    total = 0.0
    for part in others:
        d = dist(pos, part.get_position())
        weight = 1.0 if part.get_team() != my_team else 0.5
        if part.is_head():
            if d <= 1:
                total += 10 * weight
            elif d == 2:
                total += 4 * weight
            elif d == 3:
                total += 1 * weight
        elif d <= 1:
            total += 1 * weight
    return total


# ---------- tier 4: pearls ----------

def pearl_dist(pos, pearls):
    """Distance from pos to the nearest visible pearl (0 if none are visible)."""
    if not pearls:
        return 0
    return min(dist(pos, p) for p in pearls)


# ---------- decision ----------

def choose_direction(others):
    here = ct.get_position()

    # tier 1: drop anything blocked
    moves = {}
    for d in Direction.get_direction_list():
        nxt = open_step(here, d)
        if nxt is not None:
            moves[d] = nxt
    if not moves:
        return None

    # tier 2: drop moves into pockets too small for us
    needed = ct.get_length()
    space = {d: room(p, here, needed) for d, p in moves.items()}
    roomy = {d: p for d, p in moves.items() if space[d] >= needed}
    if roomy:
        moves = roomy
    else:
        # every option is a dead end, so take the one with the most room
        best = max(space.values())
        moves = {d: p for d, p in moves.items() if space[d] == best}

    # tiers 3 and 4: least danger first, then closest to a pearl
    pearls = [t.get_position() for t in ct.get_tiles() if t.has_pearl()]
    facing = ct.get_dir()

    def key(d):
        p = moves[d]
        return (danger(p, others),
                pearl_dist(p, pearls),
                d != facing,            # nothing else to go on: keep straight
                random.random())        # random tie-break

    return min(moves, key=key)


def try_split(others):
    """Split when long enough and no enemy head is in view."""
    if not SPLIT_ENABLED or ct.get_length() < SPLIT_MIN_LENGTH:
        return False
    my_team = ct.get_team()
    if any(p.is_head() and p.get_team() != my_team for p in others):
        return False
    child = ct.get_length() // 2
    if ct.can_split(child):
        ct.do_split(child)
        return True
    return False


def last_resort():
    """Every direction is blocked: try a portal, else just keep facing forward."""
    here_tile = ct.get_tile(ct.get_position())
    for d in Direction.get_direction_list():
        if here_tile.get_edge(d).get_edge_type() == EdgeType.PORTAL:
            return d
    return ct.get_dir()


def execute_turn() -> None:
    others = other_dragon_parts()

    if try_split(others):
        return

    d = choose_direction(others)
    if d is None:
        d = last_resort()
    ct.make_move(d)


# ---------- boilerplate ----------

def main() -> None:
    global ct, game
    ct, game = unswbc.init()

    while unswbc.update(ct, game):
        execute_turn()
        unswbc.end_turn()


if __name__ == "__main__":
    main()