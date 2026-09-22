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

# Test these global variables

AGGRESSIVE = True                 # overwritten once per dragon, right after init()

SPLIT_MIN_LENGTH = 4            # aggressive dragons split once this long
PORTAL_CHANCE = 0.4             # aggressive dragons: odds of diving into an
                                 # available portal this turn instead of pathing normally

MAX_NEARBY_ALLIES_TO_SPLIT = 4    # skip splitting if more than this many other allies are close

SPLIT_ROOM_MULTIPLIER = 2  # a split needs room for two independent bodies, not
                            # one, so require more open space than plain
                            # movement (tier 2) does before committing to it
PEARL_SPAWN_HORIZON = 3   # ignore spawns further out than this - too likely to
                           # be irrelevant or contested by the time we'd arrive
# ---------- helpers ----------

def gather_pearl_targets():
    """Every tile worth heading toward for a pearl: ones that already have
    one (cost 0), plus ones that don't yet but will soon enough to be worth
    planning a path to. Skips pending tiles that currently have a dragon
    sitting on them - a pearl can't spawn under a dragon, so an occupied
    tile's countdown isn't trustworthy until whoever's there moves off."""
    targets = []
    for t in ct.get_tiles():
        if t.has_pearl():
            targets.append((t.get_position(), 0))
            continue
        pt = t.get_pearl_time()
        if not (0 <= pt <= PEARL_SPAWN_HORIZON):
            continue
        if t.get_dragon() is not None:
            continue
        targets.append((t.get_position(), pt))
    return targets

def pearl_dist(pos, targets):
    """Turns until pos could plausibly collect the best target: whichever is
    later, the walk there or the spawn actually happening. Reduces to plain
    distance for pearls that already exist, since their time is 0."""
    if not targets:
        return 0
    return min(max(dist(pos, p), t) for p, t in targets)

def has_room_to_split():
    """Flood-fills the open space around us (kelp and other dragons both
    block it, same as room() does for movement) and refuses to split if
    it's too walled-in to comfortably hold both the parent and the new
    child afterward - a tight, kelp-choked pocket is exactly where two
    independent bodies end up ramming each other."""
    here = ct.get_position()
    needed = ct.get_length() * SPLIT_ROOM_MULTIPLIER
    return room(here, here, needed) >= needed

def nearby_ally_count(others, radius=3):
    """How many distinct ally dragons (besides us) have any visible segment
    within `radius` tiles of our head. Counts dragons, not segments, so one
    long ally snake doesn't get double-weighted against a short one."""
    here = ct.get_position()
    my_team = ct.get_team()
    seen_ids = set()
    for part in others:
        if part.get_team() != my_team:
            continue
        if part.get_id() in seen_ids:
            continue
        if dist(here, part.get_position()) <= radius:
            seen_ids.add(part.get_id())
    return len(seen_ids)


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

def ally_crowding(pos, others):
    """How boxed-in this move would leave us relative to our own team. This is
    a lightweight tiebreaker, not a hard restriction - it never overrides
    pearl-seeking, it only nudges between two moves that are otherwise
    equally good, so aggressive dragons stop squeezing each other into
    corridors instead of just tier-1-blocking each other at the last tile."""
    my_team = ct.get_team()
    total = 0.0
    for part in others:
        if part.get_team() != my_team:
            continue
        d = dist(pos, part.get_position())
        if d <= 1:
            total += 2
        elif d == 2:
            total += 1
    return total

# ---------- decision ----------

def choose_direction(others):
    here = ct.get_position()
    targets = gather_pearl_targets()

    if AGGRESSIVE and not targets:
        # only gamble on a portal when there's nothing better to chase -
        # otherwise it was abandoning a visible pearl on a coin flip
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

    facing = ct.get_dir()

    if AGGRESSIVE:
        # skip danger, but dead ends are already handled above: go straight
        # for the nearest pearl among whatever's left, breaking ties toward
        # whichever option gives our own teammates more room
        def key(d):
            p = moves[d]
            return (pearl_dist(p, targets), ally_crowding(p, others),
                     d != facing, random.random())
    else:
        # tiers 3 and 4: least danger first, then closest to a pearl
        def key(d):
            p = moves[d]
            return (danger(p, others), pearl_dist(p, targets),
                     d != facing, random.random())

    return min(moves, key=key)


def try_split(others):
    """Aggressive dragons split as soon as they're long enough. Safe dragons
    never split, to keep their body intact for evasive pathfinding."""
    if not AGGRESSIVE:
        return False
    if ct.get_length() < SPLIT_MIN_LENGTH:
        return False
    if nearby_ally_count(others) > MAX_NEARBY_ALLIES_TO_SPLIT:
        return False
    if not has_room_to_split():
        return False
    if ct.get_length() % 2 == 0:
        child = ct.get_length() // 2
    else:
        child = 2
    if ct.can_split(child):
        ct.do_split(child)
        return True
    return False


def desperate_split():
    """Split off as much of the tail as legally possible, ignoring the normal
    AGGRESSIVE/SPLIT_MIN_LENGTH gate - this only runs when we're fully boxed
    in with no legal move, so saving even part of the dragon as a separate
    unit beats losing all of it to whatever's about to happen to the head."""
    length = ct.get_length()
    min_size = unswbc.Constants.MIN_SIZE
    if length < 2 * min_size:
        return False   # too short to split anything off at all
    child = max(min_size, min(length // 2, length - min_size))
    if ct.can_split(child):
        ct.do_split(child)
        return True
    return False


def last_resort():
    """Every direction is blocked. Try a portal escape first. Failing that,
    split off the tail rather than lose the whole body - the child becomes
    its own dragon immediately, so it survives the parent's fate even if the
    head doesn't. Returns a Direction to move, or None if a split already
    used up the turn."""
    here_tile = ct.get_tile(ct.get_position())
    for d in Direction.get_direction_list():
        if here_tile.get_edge(d).get_edge_type() == EdgeType.PORTAL:
            return d

    if desperate_split():
        return None   # split used the turn; nothing left to move

    return ct.get_dir()   # truly nothing left to try


def execute_turn() -> None:
    others = other_dragon_parts()

    if try_split(others):
        return

    d = choose_direction(others)
    if d is None:
        d = last_resort()
        if d is None:
            return   # last_resort already split; no move to send
    ct.make_move(d)


# ---------- boilerplate ----------

def main() -> None:
    global ct, game, AGGRESSIVE
    ct, game = unswbc.init()
    progress = game.get_round_num() / 250                            # 0 at start, 1 around round 350
    AGGRO_CHANCE = max(0.95 - 0.6 * progress, 0.2)                    # ~95% -> floors at 40%
    AGGRESSIVE = random.random() < AGGRO_CHANCE                         # heavy subject to change

    while unswbc.update(ct, game):
        if game.get_round_num() > 475:
            AGGRESSIVE = False
        execute_turn()
        unswbc.end_turn()


if __name__ == "__main__":
    main()