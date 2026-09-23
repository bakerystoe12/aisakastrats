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

Pathfinding to pearls (and pending pearl spawns) now runs a BFS over the
visible 7x7 window instead of scoring moves by straight-line (Manhattan)
distance. Manhattan distance ignores kelp walls, dragons blocking the way,
and how the map wraps, so a move that "looks" closer in a straight line can
actually be the long way round, or blocked entirely. BFS walks the real
graph of passable edges and free tiles, so the distance used to rank moves
is the true shortest-path distance. `dist()` itself stays Manhattan on
purpose: it's used for proximity/threat checks (is_fatal, danger,
ally_crowding) that genuinely mean "how close would this land me", not
"what's the walking distance", so BFS would be the wrong tool there.

Each dragon also keeps its own memory of what it has personally seen over
its life (no sonar, nothing shared with other dragons):

  - terrain: kelp and walls never move once placed, so any edge we've ever
    seen stays trustworthy forever, letting BFS route past the edge of our
    current 7x7 vision instead of stopping dead at it.
  - teammates: aggressive dragons only chase pearls, so two siblings from
    the same split can easily beeline for the same spot. Remembering where
    we recently saw one of our own other dragons lets an aggressive
    dragon's BFS steer its route away from that ground for a few rounds.
    This is separate from, and complements, ally_crowding below: that one
    only reacts to allies currently in view as a live tiebreaker, while
    this affects which multi-step route BFS considers best in the first
    place, using sightings that may already be out of view.
  - the last portal we personally crossed: re-entering the exact same
    portal we just came through is not a blind gamble like any other
    portal - portal ids are unique per pair of edges, so the far side of
    that specific edge is guaranteed to be the tile we just departed, and
    our own neck is still sitting on or near that tile for roughly our
    current length in rounds afterward. That one portal is excluded from
    the gamble pool for that long; every other portal is still a genuine
    unknown and stays fair game.

This memory is scoped to one dragon's whole life; a freshly split child
starts with none of it, since it runs its own init() from scratch.
"""
import helper as unswbc
from helper import Direction, EdgeType
import random
import time
from collections import deque

ct: unswbc.Controller
game: unswbc.Game

# Test these global variables

AGGRESSIVE = True                 # overwritten once per dragon, right after init()

SPLIT_MIN_LENGTH = 4            # aggressive dragons split once this long
PORTAL_CHANCE = 0.8             # aggressive dragons: odds of diving into an
                                 # available portal this turn instead of pathing normally

MAX_NEARBY_ALLIES_TO_SPLIT = 4    # skip splitting if more than this many other allies are close

SPLIT_ROOM_MULTIPLIER = 2  # a split needs room for two independent bodies, not
                            # one, so require more open space than plain
                            # movement (tier 2) does before committing to it
PEARL_SPAWN_HORIZON = 3   # ignore spawns further out than this - too likely to
                           # be irrelevant or contested by the time we'd arrive

BFS_CAP = 120              # was 300 - this is the dominant per-turn cost (see
                            # bfs_targets), and pearl_dist_bfs reruns it fresh
                            # for every candidate direction, so this roughly
                            # linearly scales the most expensive part of a turn.
                            # Pearls this far away rarely win a tie anyway.

TURN_TIME_BUDGET = 0.018   # seconds. Stop chasing BFS precision once this much
                            # of the turn has elapsed and fall back to a cheap
                            # move instead, so we always answer well inside the
                            # engine's per-turn limit rather than risk a
                            # "gave no valid action" timeout death.
BFS_DEADLINE_CHECK_EVERY = 4   # nodes between deadline checks inside a BFS.
                                # time.perf_counter() itself is cheap (~50-100ns),
                                # so this stays a small fraction of normal per-node
                                # cost, but keeps the worst-case overrun small even
                                # if a single node's work is unexpectedly slow
                                # (e.g. host contention) rather than just uniformly
                                # slower - see test_deadline.py for why 32 was too
                                # coarse: it let a slow node blow well past budget
                                # before the next check even happened.

# ---------- portals: gamble pool, minus the one we know is a trap ----------
#
# Crossing a portal edge lands us somewhere add_dir can't compute, so in
# general we have no way to score what's on the other side - that risk is
# unavoidable and stays exclusive to the aggressive style. But the *specific*
# portal we ourselves just crossed is not actually a mystery: portal_id is
# unique per pair of edges (see _Edges.__missing__ in helper.py), so
# stepping through that exact id again is guaranteed - not just likely - to
# put us back on the tile we just departed. Our own neck occupies that tile,
# or is still close enough behind it, for roughly `length` rounds after we
# leave, so re-entering it there is a guaranteed self-collision, not a gamble.

LAST_PORTAL_ID = -1          # portal_id of the last portal we crossed, or -1 if none yet
LAST_PORTAL_ROUND = -10**9   # round we crossed it, far in the past so the cooldown starts expired

# ---------- helpers ----------

def terrain_reachable(start, cap=BFS_CAP):
    """Flood fill using only known edge terrain (kelp vs empty), ignoring
    dragons and danger entirely - kelp never moves, so this answers a
    narrower, more stable question than bfs_targets does: 'is there any
    possible route at all', not 'is there a route that's clear and safe
    right now'. Uses the same remembered-edge memory as bfs_targets, so it
    can see past the current 7x7 window for targets near the edge of view.
    Portal edges don't count as passable here either, same reasoning as
    everywhere else: we can't compute where they lead.
 
    Returns the set of (x, y) tiles reached within `cap` steps. Since every
    pearl target comes from gather_pearl_targets, which only looks at the
    current 7x7 window, they're all close to `start` - this basically never
    needs anywhere near the full cap in practice, it's just a safety valve
    against a pathological kelp maze."""
    seen = {(start.x, start.y)}
    queue = deque([start])
    while queue and len(seen) < cap:
        pos = queue.popleft()
        for d in Direction.get_direction_list():
            if known_edge_type(pos, d) != EdgeType.EMPTY:
                continue
            nxt = pos.add_dir(d)
            key = (nxt.x, nxt.y)
            if key in seen:
                continue
            seen.add(key)
            queue.append(nxt)
    return seen
 
 
def gather_pearl_targets(here):
    """Every tile worth heading toward for a pearl: ones that already have
    one (cost 0), plus ones that don't yet but will soon enough to be worth
    planning a path to. Skips pending tiles that currently have a dragon
    sitting on them - a pearl can't spawn under a dragon, so an occupied
    tile's countdown isn't trustworthy until whoever's there moves off.
 
    Also drops anything not actually reachable from `here` through known
    non-kelp terrain - a pearl sitting in a pocket permanently sealed off by
    kelp is worth zero, not "very far away", and letting it into the target
    list lets its Manhattan-distance fallback (see pearl_dist / pearl_dist_bfs)
    mislead a direction comparison toward a target that can never be
    collected. This check only concerns permanent terrain, not dragons or
    danger - a target temporarily blocked by another dragon standing in the
    way is still included here, since that dragon will move."""
    candidates = []
    for t in ct.get_tiles():
        if t.has_pearl():
            candidates.append((t.get_position(), 0))
            continue
        pt = t.get_pearl_time()
        if not (0 <= pt <= PEARL_SPAWN_HORIZON):
            continue
        if t.get_dragon() is not None:
            continue
        candidates.append((t.get_position(), pt))
 
    if not candidates:
        return []
 
    reachable = terrain_reachable(here)
    return [(pos, pt) for pos, pt in candidates if (pos.x, pos.y) in reachable]

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
    """Manhattan distance that accounts for the map wrapping around.
    Used for proximity/threat checks (is_fatal, danger, ally_crowding), and
    as pearl_dist's fallback when BFS can't confirm a real path - see
    bfs_targets and pearl_dist_bfs below for the actual pathfinding."""
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



def portal_directions(pos, round_num):
    """Directions from pos whose edge is a portal, excluding whichever one we
    ourselves crossed most recently while our tail could still be sitting on
    the tile it would send us back to. Returns (direction, portal_id) pairs,
    not bare directions, so the caller can remember which one it picked."""
    tile = ct.get_tile(pos)
    if tile is None:
        return []
    cooldown = ct.get_length()   # rough time for our tail to fully clear the departure tile
    options = []
    for d in Direction.get_direction_list():
        edge = tile.get_edge(d)
        if edge.get_edge_type() != EdgeType.PORTAL:
            continue
        pid = edge.get_portal_id()
        if pid == LAST_PORTAL_ID and round_num - LAST_PORTAL_ROUND < cooldown:
            continue   # this one goes straight back into our own tail right now
        options.append((d, pid))
    return options


def remember_portal_crossing(pid, round_num):
    global LAST_PORTAL_ID, LAST_PORTAL_ROUND
    LAST_PORTAL_ID = pid
    LAST_PORTAL_ROUND = round_num


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

# ---------- memory: what this dragon has personally seen over its life ----------
#
# Vision only ever shows the 7x7 window around our head, so BFS on its own
# can't route more than 3 tiles out. Two things are worth remembering past
# that boundary, purely from this one dragon's own vision history - no
# sonar, nothing shared with any other dragon:
#
#   - terrain: kelp and walls never move once placed, so any edge we've ever
#     seen can be trusted forever, long after it scrolls out of view.
#   - teammates: recording where we recently saw one of our own other
#     dragons lets an aggressive dragon's BFS steer around ground a sibling
#     is probably already covering, instead of both racing for the same spot.

TEAMMATE_MEMORY_FRESH_ROUNDS = 4   # how many rounds a teammate sighting is still worth avoiding

KNOWN_EDGES = {}       # (x, y) -> {Direction: EdgeType}, remembered forever once seen
TEAMMATE_MEMORY = {}   # (x, y) -> round we last saw one of our own other dragons standing there


def remember_vision(round_num):
    """Fold everything currently visible into memory. Call once per turn,
    before making any decisions, so this turn's own tiles are included."""
    my_id = ct.get_id()
    my_team = ct.get_team()
    for tile in ct.get_tiles():
        pos = tile.get_position()
        key = (pos.x, pos.y)
        KNOWN_EDGES[key] = {d: tile.get_edge(d).get_edge_type() for d in Direction.get_direction_list()}

        part = tile.get_dragon()
        if part is not None and part.get_team() == my_team and part.get_id() != my_id:
            TEAMMATE_MEMORY[key] = round_num


def known_edge_type(pos, d):
    """The edge type stepping d from pos: read live if pos is in view right
    now, else whatever we remembered from the last time we saw pos. None
    means we've never seen this edge at all, so - same as an unseen tile
    does for a single step - it stays untraversable."""
    tile = ct.get_tile(pos)
    if tile is not None:
        return tile.get_edge(d).get_edge_type()
    edges = KNOWN_EDGES.get((pos.x, pos.y))
    return None if edges is None else edges.get(d)


def known_occupied(pos):
    """Whether pos currently holds a dragon. Only live vision can answer
    this - a remembered tile's occupant could easily have moved on since we
    last looked, so anything out of view is assumed clear."""
    tile = ct.get_tile(pos)
    return tile is not None and tile.get_dragon() is not None


def teammate_recently_at(pos, round_num):
    """Whether one of our own other dragons was seen standing on pos recently
    enough that it might still be nearby. Deliberately a short window, since
    a dragon moves every round and this is meant as a soft steer-clear for
    BFS route planning, not a hard wall."""
    seen_round = TEAMMATE_MEMORY.get((pos.x, pos.y))
    return seen_round is not None and round_num - seen_round <= TEAMMATE_MEMORY_FRESH_ROUNDS


# ---------- BFS pathfinding ----------

def bfs_targets(here, others, round_num, avoid_teammates=False, cap=BFS_CAP, deadline=None):
    """Breadth-first search outward from `here`, using live vision where we
    have it and remembered terrain beyond it (known_edge_type / known_occupied),
    so the search isn't boxed in by the 7x7 window the way plain vision is.
    An edge must be known EMPTY, the tile beyond it must not be occupied, and
    it must not be an outright kill (is_fatal). Portal edges are skipped here
    just like kelp: crossing one doesn't land at add_dir's tile, so there's
    no tile for this search to record a distance for or continue from.

    When avoid_teammates is set, a tile where we recently saw one of our own
    other dragons is also treated as blocked for the purposes of this search,
    so an aggressive dragon doesn't plan a route straight through ground a
    sibling is probably already covering (see teammate_recently_at). This is
    only ever a soft, short-lived nudge to route *planning*: the actual next
    move is always re-checked against live vision separately (open_step,
    is_fatal), so this can never cause a move into a tile that's actually
    free right now.

    Stops after `cap` tiles so a long-lived dragon's growing memory can't
    make this search unbounded. If `deadline` (a time.perf_counter() value)
    is given and we run past it, stops early and returns whatever's been
    found so far - a partial result, not a wrong one: pearl_dist_bfs already
    falls back to a Manhattan estimate for any target this didn't reach.

    Returns two dicts:
      dist_to[pos]  -> shortest known number of steps from `here` to pos
      root_dir[pos] -> which of the 4 first-step directions out of `here`
                       starts that shortest path

    Because this is a real BFS over the passable-edge graph (not a straight
    line), dist_to gives the true shortest-path distance, accounting for
    kelp walls, dragons in the way, and map wraparound - unlike the Manhattan
    `dist()` helper, which only measures as the crow flies.
    """
    dist_to = {here: 0}
    root_dir = {}
    queue = deque([here])
    popped = 0
    while queue and len(dist_to) < cap:
        if deadline is not None:
            popped += 1
            if popped % BFS_DEADLINE_CHECK_EVERY == 0 and time.perf_counter() >= deadline:
                break
        pos = queue.popleft()
        for d in Direction.get_direction_list():
            if known_edge_type(pos, d) != EdgeType.EMPTY:
                continue
            nxt = pos.add_dir(d)
            if nxt in dist_to or known_occupied(nxt):
                continue
            if is_fatal(nxt, others):
                continue
            if avoid_teammates and teammate_recently_at(nxt, round_num):
                continue
            dist_to[nxt] = dist_to[pos] + 1
            root_dir[nxt] = root_dir.get(pos, d)
            queue.append(nxt)
    return dist_to, root_dir


def pearl_dist_bfs(pos, targets, others, round_num, avoid_teammates, deadline=None):
    """Real shortest-path turns-until-collectable for the best target, using
    a BFS run from `pos` (the tile this move would land on) itself, not a
    single shared search from the current tile.

    A single BFS from `here` can only record one shortest-path "root
    direction" per target, so every other direction would silently fall
    back to Manhattan distance even when it also has a real, confirmed
    route - just not the fastest one. That's not a harmless approximation:
    a shorter-*looking* but wrong Manhattan estimate for one direction can
    then beat the honest, larger BFS number for the direction that's
    actually correct, which is precisely the mistake BFS was meant to fix
    in the first place. Running BFS fresh from each candidate move avoids
    that - every direction gets an honest distance, not just one.

    Falls back to pearl_dist's own Manhattan-based estimate only for a
    target this search genuinely couldn't reach at all from `pos` (e.g. it
    sits outside `cap`, or through territory never explored)."""
    if not targets:
        return 0
    dist_to, _ = bfs_targets(pos, others, round_num, avoid_teammates=avoid_teammates, deadline=deadline)
    best = None
    for target_pos, pt in targets:
        if target_pos in dist_to:
            turns = max(dist_to[target_pos], pt)
        else:
            turns = max(dist(pos, target_pos), pt)
        if best is None or turns < best:
            best = turns
    return best

# ---------- decision ----------

def cheap_fallback_direction(moves, others):
    """Used only when we're already out of time this turn (see
    TURN_TIME_BUDGET): skip pathfinding entirely and just take the least
    dangerous of whatever moves tier 1/2 already narrowed us down to,
    breaking ties arbitrarily. No BFS, so this always finishes in well
    under a millisecond regardless of map size or memory."""
    return min(moves, key=lambda d: (danger(moves[d], others), random.random()))


def choose_direction(others, round_num, deadline=None):
    here = ct.get_position()
    targets = gather_pearl_targets(here)

    if AGGRESSIVE and not targets:
        # only gamble on a portal when there's nothing better to chase -
        # otherwise it was abandoning a visible pearl on a coin flip
        portals = portal_directions(here, round_num)
        if portals and random.random() < PORTAL_CHANCE:
            # nothing to score for whichever one we pick - we don't know
            # what's on the other side - so just commit, in keeping with
            # the aggressive style. portal_directions has already ruled out
            # the one portal we do have information about: the one we just
            # came through, which would walk us straight into our own tail.
            d, pid = random.choice(portals)
            remember_portal_crossing(pid, round_num)
            return d

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
    if AGGRESSIVE == True:
        needed = ct.get_length()
    else:
        needed = ct.get_length() + 1
    space = {d: room(p, here, needed) for d, p in moves.items()}
    roomy = {d: p for d, p in moves.items() if space[d] >= needed}
    if roomy:
        moves = roomy
    else:
        best = max(space.values())
        moves = {d: p for d, p in moves.items() if space[d] == best}

    facing = ct.get_dir()

    # already out of time (e.g. an unusually slow host, or a lot of visible
    # dragons stacking up danger/is_fatal cost) - don't start BFS scoring at
    # all, just take a legal, not-actively-dangerous move and get out
    if deadline is not None and time.perf_counter() >= deadline:
        return cheap_fallback_direction(moves, others)

    if AGGRESSIVE:
        # skip danger, but dead ends are already handled above: go straight
        # for the nearest pearl among whatever's left, using the real BFS
        # shortest-path distance instead of Manhattan distance (run fresh
        # from each candidate move - see pearl_dist_bfs for why), and
        # breaking ties toward whichever option gives our own teammates
        # more room. Also steers away from ground a sibling was recently
        # seen on (avoid_teammates), so splits spread out.
        def key(d):
            p = moves[d]
            return (pearl_dist_bfs(p, targets, others, round_num, AGGRESSIVE, deadline=deadline),
                     ally_crowding(p, others), d != facing, random.random())
    else:
        # tiers 3 and 4: least danger first, then closest to a pearl by real
        # BFS shortest-path distance
        def key(d):
            p = moves[d]
            return (danger(p, others), pearl_dist_bfs(p, targets, others, round_num, AGGRESSIVE, deadline=deadline),
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
    child = max(min_size, length - min_size)
    if ct.can_split(child):
        ct.do_split(child)
        return True
    return False


def last_resort(round_num):
    """Every direction is blocked. Try a portal escape first - excluding, as
    always, whichever one we just came through, since that one is a
    guaranteed self-collision, not a gamble, and wouldn't actually save us.
    Failing that, split off the tail rather than lose the whole body - the
    child becomes its own dragon immediately, so it survives the parent's
    fate even if the head doesn't. Returns a Direction to move, or None if a
    split already used up the turn."""
    here = ct.get_position()
    portals = portal_directions(here, round_num)
    if portals:
        d, pid = portals[0]
        remember_portal_crossing(pid, round_num)
        return d

    if desperate_split():
        return None   # split used the turn; nothing left to move

    return ct.get_dir()   # truly nothing left to try


def execute_turn() -> None:
    round_num = game.get_round_num()
    deadline = time.perf_counter() + TURN_TIME_BUDGET
    others = other_dragon_parts()
    remember_vision(round_num)

    if try_split(others):
        return

    d = choose_direction(others, round_num, deadline=deadline)
    if d is None:
        d = last_resort(round_num)
        if d is None:
            return   # last_resort already split; no move to send
    ct.make_move(d)


# ---------- boilerplate ----------

def main() -> None:
    global ct, game, AGGRESSIVE
    ct, game = unswbc.init()
    progress = game.get_round_num() / 250                            # 0 at start, 1 around round 350
    AGGRO_CHANCE = max(1 - 0.6 * progress, 0.2)                    # 100% -> floors at 40%
    AGGRESSIVE = random.random() < AGGRO_CHANCE                         # heavy subject to change

    while unswbc.update(ct, game):
        if game.get_round_num() > 475:
            AGGRESSIVE = False
        execute_turn()
        unswbc.end_turn()


if __name__ == "__main__":
    main()