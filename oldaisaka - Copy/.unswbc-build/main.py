import helper as unswbc
from helper import Direction, EdgeType
import random
import time
from collections import deque
import hashlib
import heapq 

ct: unswbc.Controller
game: unswbc.Game

# Test these global variables

SPLIT_MIN_LENGTH = 4            # aggressive dragons split once this long
PORTAL_CHANCE = 1             # aggressive dragons: odds of diving into an
                                 # available portal this turn instead of pathing normally

MAX_NEARBY_ALLIES_TO_SPLIT = 4    # skip splitting if more than this many other allies are close

SPLIT_ROOM_MULTIPLIER = 2   # a split needs room for two independent bodies, not
                            # one, so require more open space than plain
                            # movement (tier 2) does before committing to it
PEARL_SPAWN_HORIZON = 5   # ignore spawns further out than this - too likely to
                           # be irrelevant or contested by the time we'd arrive

BFS_CAP = 120              # was 300 - this is the dominant per-turn cost (see
                            # bfs_targets), and pearl_dist_bfs reruns it fresh
                            # for every candidate direction, so this roughly
                            # linearly scales the most expensive part of a turn.
                            # Pearls this far away rarely win a tie anyway.

TURN_TIME_BUDGET = 0.05   # seconds. Stop chasing BFS precision once this much
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

KNOWN_EDGES = {}
OCC = set()
W = H = 0
DIRS = Direction.get_direction_list()
NORTH, EAST, SOUTH, WEST = DIRS
EMPTY = EdgeType.EMPTY
KELP = EdgeType.KELP
OFF = {d: d.get_offset() for d in DIRS}
OPP = {d: d.get_opposite() for d in DIRS}       # (x, y) -> {Direction: EdgeType}, remembered forever once seen


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
random.seed(14)
# ---------- helpers ----------

def stable_roll(key):
    """Deterministic value in [0,1) derived from `key`. Same key always
    gives the same roll - unlike random.random(), this can't be desynced
    by seeding, call order, or how many other random() calls happened
    earlier this turn/process. Use this for any role-assignment decision
    that needs a reliable population-wide ratio."""
    h = hashlib.sha256(str(key).encode()).hexdigest()
    return int(h[:8], 16) / 0xFFFFFFFF

def terrain_reachable(start, cap=BFS_CAP):
    """Flood fill over known non-kelp edges (tuple keys, no Position allocs).
    Returns a set of (x, y) keys reached within `cap` tiles."""
    sk = (start.x, start.y)
    seen = {sk}
    queue = deque([sk])
    while queue and len(seen) < cap:
        k = queue.popleft()
        edges = KNOWN_EDGES.get(k)
        for d in DIRS:
            if edges is None:
                if not AGGRESSIVE:
                    continue
            elif edges.get(d, EMPTY) != EMPTY:
                continue
            dx, dy = OFF[d]
            nk = ((k[0] + dx) % W, (k[1] + dy) % H)
            if nk in seen:
                continue
            seen.add(nk)
            queue.append(nk)
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
    dx = abs(a.x - b.x)
    dx = min(dx, W - dx)
    dy = abs(a.y - b.y)
    dy = min(dy, H - dy)
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
    """Flood fill from start over free tiles. Returns tiles reachable, stopping
    at cap. Reaching a never-seen tile counts as 'plenty of room'."""
    sk = (start.x, start.y)
    seen = {(came_from.x, came_from.y), sk}
    queue = deque([sk])
    count = 0
    while queue:
        k = queue.popleft()
        count += 1
        if count >= cap:
            return cap
        edges = KNOWN_EDGES.get(k)
        for d in DIRS:
            if edges is None:
                if not AGGRESSIVE:
                    continue
            elif edges.get(d, EMPTY) != EMPTY:
                continue
            dx, dy = OFF[d]
            nk = ((k[0] + dx) % W, (k[1] + dy) % H)
            if nk in seen:
                continue
            if nk not in KNOWN_EDGES:
                return cap
            if nk in OCC:
                continue
            seen.add(nk)
            queue.append(nk)
    return count

def tile_ever_seen(pos):
    return (pos.x, pos.y) in KNOWN_EDGES

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
                total += 2 * weight
            elif d == 2:
                total += 1 * weight
            elif d == 3:
                total += 0.5 * weight
        elif d <= 1:
            total += 0.5 * weight
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
# can't route more than 3 tiles out. Terrain is worth remembering past that
# boundary, purely from this one dragon's own vision history - no sonar,
# nothing shared with any other dragon: kelp and walls never move once
# placed, so any edge we've ever seen can be trusted forever, long after it
# scrolls out of view.

def remember_vision():
    """Fold everything visible into memory and build this turn's occupancy set.
    Call once per turn before any decision."""
    global OCC
    OCC = set()
    for tile in ct.get_tiles():
        pos = tile.position
        key = (pos.x, pos.y)
        e = tile._edges
        KNOWN_EDGES[key] = {NORTH: e[0].edge_type, EAST: e[1].edge_type,
                            SOUTH: e[2].edge_type, WEST: e[3].edge_type}
        if tile.dragon_part is not None:
            OCC.add(key)


def known_edge_type(pos, d):
    edges = KNOWN_EDGES.get((pos.x, pos.y))
    if edges is None:
        return EMPTY if AGGRESSIVE else KELP
    return edges.get(d, EMPTY)


def known_occupied(pos):
    return (pos.x, pos.y) in OCC


# ---------- one shared pathfinding pass per turn ----------

def fatal_keys(others):
    """Tiles within 1 of a visible enemy head (what is_fatal rejects)."""
    my_team = ct.get_team()
    out = set()
    for part in others:
        if part.is_dragon_head and part.team != my_team:
            hx, hy = part.position.x, part.position.y
            out.add((hx, hy))
            for d in DIRS:
                dx, dy = OFF[d]
                out.add(((hx + dx) % W, (hy + dy) % H))
    return out


def pearl_field(targets, goal_keys, avoid, deadline=None, cap=BFS_CAP):
    """Search outward from ALL pearl targets at once (edges are symmetric, so
    distance-to-target is the same searched from either end). One pass gives
    every candidate move its distance, instead of one A* per direction.
    A pending pearl (spawns in pt turns) starts at pt, so the value is
    'turns until collectable' (walk + wait, slightly pessimistic vs max()).
    Stops as soon as every key in goal_keys is settled, or cap/deadline."""
    heap = []
    best = {}
    for pos, pt in targets:
        k = (pos.x, pos.y)
        if k not in best or pt < best[k]:
            best[k] = pt
            heap.append((pt, k))
    heapq.heapify(heap)
    remaining = set(goal_keys)
    done = {}
    n = 0
    while heap and remaining and len(done) < cap:
        g, k = heapq.heappop(heap)
        if k in done:
            continue
        done[k] = g
        remaining.discard(k)
        n += 1
        if deadline is not None and n % BFS_DEADLINE_CHECK_EVERY == 0 and time.perf_counter() >= deadline:
            break
        kx, ky = k
        for d in DIRS:
            dx, dy = OFF[d]
            nk = ((kx + dx) % W, (ky + dy) % H)
            if nk in done or nk in OCC:
                continue
            if avoid and nk in avoid:
                continue
            e = KNOWN_EDGES.get(nk)
            if e is None:
                if not AGGRESSIVE:
                    continue
            elif e.get(OPP[d], EMPTY) != EMPTY:
                continue
            ng = g + 1
            if ng < best.get(nk, 1 << 30):
                best[nk] = ng
                heapq.heappush(heap, (ng, nk))
    return done

# ---------- decision ----------

def cheap_fallback_direction(moves, others, targets):
    """Used only when we're already out of time this turn (see
    TURN_TIME_BUDGET): skip A* entirely, but still reach for a pearl first
    using the cheap Manhattan estimate (pearl_dist costs nothing beyond a
    distance calc per target, unlike the graph search), rather than
    defaulting purely to danger-avoidance. danger only breaks ties between
    moves that already look equally good pearl-wise, so this still won't
    walk into an obviously worse spot when two options tie - it just no
    longer treats caution as more important than a pearl right in front of
    it when time is short."""
    return min(moves, key=lambda d: (pearl_dist(moves[d], targets),
                                       danger(moves[d], others),
                                       random.random()))


def choose_direction(others, round_num, deadline=None):
    here = ct.get_position()
    targets = gather_pearl_targets(here)

    if AGGRESSIVE and not targets:
        portals = portal_directions(here, round_num)
        if portals and random.random() < PORTAL_CHANCE:
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

    # hard veto: drop anything that ends next to an enemy head. Safe dragons
    # still refuse this outright, since a head-on hit kills both and they
    # have nothing to gain from contesting the space. Aggressive dragons
    # accept the risk instead of ceding ground - many maps punish giving up
    # space this way, so they play the gamble rather than back off.
    if not AGGRESSIVE:
        safe_from_death = {d: p for d, p in moves.items() if not is_fatal(p, others)}
        if safe_from_death:
            moves = safe_from_death

    # tier 2: drop moves into pockets too small for us - applies to both
    # styles, since walking into a dead end is never useful, only risky
    if AGGRESSIVE:
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

    # already out of time - don't start A* scoring at all, just take a
    # legal, pearl-leaning move and get out
    if deadline is not None and time.perf_counter() >= deadline:
        return cheap_fallback_direction(moves, others, targets)

    # one shared search for all candidate moves
    if targets:
        avoid = None if AGGRESSIVE else fatal_keys(others)
        goal = [(p.x, p.y) for p in moves.values()]
        field = pearl_field(targets, goal, avoid, deadline=deadline)
    else:
        field = {}

    def pdist(p):
        if not targets:
            return 0
        v = field.get((p.x, p.y))
        return v if v is not None else pearl_dist(p, targets)

    if AGGRESSIVE:
        def key(d):
            p = moves[d]
            return (pdist(p), ally_crowding(p, others), d != facing, random.random())
    else:
        def key(d):
            p = moves[d]
            return (danger(p, others), pdist(p), d != facing, random.random())

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
    length = ct.get_length()
    if length % 2 == 0:
        if (length // 2)  % 2 != 0:
            child = 2
        else:
            child = length // 2
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
    remember_vision()

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
    global W, H
    W, H = game.get_map_size()
    rolled = False

    
    while unswbc.update(ct, game):
        
        if rolled == False:
            progress = (game.get_round_num() - 1) / 250                            # 0 at start, 1 around round 350
            AGGRO_CHANCE = max(1 - 0.6 * progress, 0.2)                    # 100% -> floors at 40%
            AGGRESSIVE = stable_roll(ct.get_id()) < AGGRO_CHANCE                     # heavy subject to change
            rolled = True

        if game.get_round_num() > 475: # Inside of turning into safe: Either suicide or kamikaze, this condition would depend on if enemies are nearby
            AGGRESSIVE = False

        if ct.get_length() > 6:    # Prevention from desperate split child from becoming aggressive
            AGGRESSIVE = False
        
        execute_turn()
        unswbc.end_turn()


if __name__ == "__main__":
    main()