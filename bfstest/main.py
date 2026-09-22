"""
Same pathfinding as safe_bot.py, except each dragon rolls its play style once,
right after init(), and keeps it for its whole life:

  ~90% AGGRESSIVE : skips the dead-end and danger tiers, beelines for pearls,
                     splits as soon as it's long enough, and will occasionally
                     dive through a portal on a hunch instead of pathing around.
  ~10% SAFE       : the original tiered logic (dead ends, then danger, then
                     pearls), never splits, and never risks a portal.

Every dragon, regardless of style, still refuses a move that ends 1 tile from
an enemy head, since that's an outright kill and never worth it. Portal dives
are the one exception to that veto: we genuinely can't see what's on the other
side of a portal, so there's nothing to check - it's a gamble kept exclusive
to the aggressive style on purpose.
"""
import helper as unswbc
from helper import Direction, EdgeType
import random
from collections import deque

ct: unswbc.Controller
game: unswbc.Game


AGGRESSIVE = True                 # overwritten once per dragon, right after init()

SPLIT_MIN_LENGTH = 4            # aggressive dragons split once this long
PORTAL_CHANCE = 0.5             # aggressive dragons: odds of diving into an
                                 # available portal this turn instead of pathing normally


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


def portal_directions(pos):
    """Directions from pos whose edge is a portal. There's no way to know
    what's waiting on the other side - add_dir doesn't resolve the far end
    of a portal - so this isn't a scored move like the rest, just a list of
    "would teleport" options for whoever's willing to risk it."""
    tile = ct.get_tile(pos)
    if tile is None:
        return []
    return [d for d in Direction.get_direction_list()
            if tile.get_edge(d).get_edge_type() == EdgeType.PORTAL]


# ---------- hard veto: never walk next to an enemy head ----------

def is_fatal(pos, others):
    """True if pos ends up 1 tile (or 0, though that's already blocked) from
    an enemy head. Applies to every dragon, aggressive or safe."""
    my_team = ct.get_team()
    for part in others:
        if part.is_head() and part.get_team() != my_team and dist(pos, part.get_position()) <= 1:
            return True
    return False


# ---------- tier 2: dead ends (safe dragons only) ----------

def room(start, came_from, cap):
    """Flood fill from start over free tiles in view. Returns how many tiles are
    reachable, stopping at cap. Reaching the edge of our view counts as
    'plenty of room' because we can't see what lies beyond."""
    seen = {came_from, start}
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
                continue
            nxt = pos.add_dir(d)
            if nxt in seen:
                continue
            ahead = ct.get_tile(nxt)
            if ahead is None:
                return cap
            if ahead.get_dragon() is not None:
                continue
            seen.add(nxt)
            queue.append(nxt)
    return count


# ---------- tier 3: dragons (safe dragons only) ----------

def danger(pos, others):
    """How risky it is to end a step on pos. Heads matter most, since a head-on
    hit kills both dragons and enemies may sprint several tiles."""
    my_team = ct.get_team()
    total = 0.0
    for part in others:
        d = dist(pos, part.get_position())
        weight = 1.0
        if part.is_head():
            if d <= 1:
                total += 5 * weight
            elif d == 2:
                total += 4 * weight
            elif d == 3:
                total += 1 * weight
        elif d <= 1:
            total += 1 * weight
    return total


# ---------- tier 4: pearls (everyone) ----------

def pearl_dist(pos, pearls):
    """Distance from pos to the nearest visible pearl (0 if none are visible)."""
    if not pearls:
        return 0
    return min(dist(pos, p) for p in pearls)

# ---------- BFS pathfinding ----------

def bfs_targets(here, others):
    """Breadth-first search outward from `here` over every tile currently in
    view, following exactly the same legality rules as a single step
    (open_step): edges must be EMPTY (kelp blocks, portals block since they'd
    need special-casing), the tile ahead must be visible, and it must not
    already hold a dragon segment. Tiles that would land us 1 tile from an
    enemy head are also excluded from the search, so pathfinding never routes
    through a tile that's an outright kill to stand on.

    Returns two dicts:
      dist_to[pos]  -> shortest number of steps from `here` to pos
      root_dir[pos] -> which of the 4 first-step directions out of `here`
                       starts the shortest path that reaches pos

    Because this is a real BFS over the passable-edge graph (not a straight
    line), dist_to gives the true shortest-path distance, accounting for
    kelp walls, dragons in the way, and map wraparound - unlike the Manhattan
    `dist()` helper, which only measures as the crow flies.
    """
    dist_to = {here: 0}
    root_dir = {}
    queue = deque([here])
    while queue:
        pos = queue.popleft()
        tile = ct.get_tile(pos)
        if tile is None:
            continue
        for d in Direction.get_direction_list():
            if tile.get_edge(d).get_edge_type() != EdgeType.EMPTY:
                continue
            nxt = pos.add_dir(d)
            if nxt in dist_to:
                continue
            ahead = ct.get_tile(nxt)
            if ahead is None or ahead.get_dragon() is not None:
                continue
            if is_fatal(nxt, others):
                continue
            dist_to[nxt] = dist_to[pos] + 1
            root_dir[nxt] = root_dir.get(pos, d)
            queue.append(nxt)
    return dist_to, root_dir


def pearl_dist_bfs(d, pos, pearls, dist_to, root_dir):
    """Real shortest-path distance remaining to the nearest pearl, if taking
    first step `d` actually lies on that pearl's shortest BFS path from here.
    Falls back to Manhattan distance from `pos` when no visible pearl has a
    known BFS path through this particular first step (e.g. every pearl is
    outside the searched window, or only reachable via a different step)."""
    best = None
    for pearl in pearls:
        if pearl in dist_to and root_dir.get(pearl) == d:
            remaining = dist_to[pearl] - 1  # the step to `pos` already counts as 1
            if best is None or remaining < best:
                best = remaining
    if best is not None:
        return best
    return pearl_dist(pos, pearls)


# ---------- decision ----------

def choose_direction(others):
    here = ct.get_position()

    if AGGRESSIVE:
        portals = portal_directions(here)
        if portals and random.random() < PORTAL_CHANCE:
            # nothing to score - we don't know what's on the other side -
            # so just commit, in keeping with the aggressive style
            return random.choice(portals)

    # tier 1: drop anything blocked
    moves = {}
    for d in Direction.get_direction_list():
        nxt = open_step(here, d)
        if nxt is not None:
            moves[d] = nxt
    if not moves:
        return None

    # hard veto: drop anything that ends next to an enemy head, for every dragon
    safe_from_death = {d: p for d, p in moves.items() if not is_fatal(p, others)}
    if safe_from_death:
        moves = safe_from_death
    # else: every option is fatal (fully boxed in), so keep the full list and
    # let scoring pick the least-worst one rather than returning nothing

    # tier 2: drop moves into pockets too small for us - applies to both
    # styles, since walking into a dead end is never useful, only risky
    needed = ct.get_length()
    space = {d: room(p, here, needed) for d, p in moves.items()}
    roomy = {d: p for d, p in moves.items() if space[d] >= needed}
    if roomy:
        moves = roomy
    else:
        best = max(space.values())
        moves = {d: p for d, p in moves.items() if space[d] == best}

    pearls = [t.get_position() for t in ct.get_tiles() if t.has_pearl()]
    facing = ct.get_dir()

    # BFS once per turn from the current tile; every move's pearl score below
    # is read off this same search rather than re-scanning per direction.
    dist_to, root_dir = bfs_targets(here, others)

    if AGGRESSIVE:
        # skip dead ends and danger: go straight for the nearest pearl, using
        # the real BFS shortest-path distance instead of Manhattan distance
        def key(d):
            p = moves[d]
            return (pearl_dist_bfs(d, p, pearls, dist_to, root_dir), d != facing, random.random())
    else:
        # tiers 3 and 4: least danger first, then closest to a pearl by real
        # BFS shortest-path distance
        def key(d):
            p = moves[d]
            return (danger(p, others), pearl_dist_bfs(d, p, pearls, dist_to, root_dir),
                     d != facing, random.random())

    return min(moves, key=key)


def try_split():
    """Aggressive dragons split as soon as they're long enough. Safe dragons
    never split, to keep their body intact for evasive pathfinding."""
    if not AGGRESSIVE or ct.get_length() < SPLIT_MIN_LENGTH:
        return False
    if ct.get_length() % 2 == 0:
        child = ct.get_length() // 2
    else:
        child = 2
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

    if try_split():
        return

    d = choose_direction(others)
    if d is None:
        d = last_resort()
    ct.make_move(d)


# ---------- boilerplate ----------

def main() -> None:
    global ct, game, AGGRESSIVE
    ct, game = unswbc.init()
    progress = game.get_round_num() / 350   # 0 at start, 1 at round 500
    AGGRO_CHANCE = max(0.95 - 0.6 * progress, 0.4)                              # 100% -> 50%
    AGGRESSIVE = random.random() < AGGRO_CHANCE

    while unswbc.update(ct, game):
        execute_turn()
        unswbc.end_turn()


if __name__ == "__main__":
    main()