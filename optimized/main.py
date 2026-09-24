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

BFS_CAP = 120               # per-flood-fill node ceiling. This bounds node
                            # count; TURN_TIME_BUDGET (below) now bounds wall
                            # time for every flood fill in the turn, not just
                            # the pearl search - see the deadline plumbing in
                            # room()/terrain_reachable()/multi_source_dist.

TURN_TIME_BUDGET = 0.05   # seconds. Stop chasing search precision once this much
                            # of the turn has elapsed and fall back to a cheap
                            # move instead, so we always answer well inside the
                            # engine's per-turn limit rather than risk a
                            # "gave no valid action" timeout death.
                            #
                            # Previously only the pearl A* search respected this
                            # deadline; has_room_to_split(), terrain_reachable()
                            # and the tier-2 room() calls ran with no time check
                            # at all, bounded only by BFS_CAP. On open maps those
                            # flood fills reliably hit the cap rather than
                            # terminating early, and measured sandbox runs showed
                            # turns spiking to ~97% of the 100M point budget as a
                            # result. All of them now take `deadline` too, so no
                            # single turn's total flood-fill work is unbounded in
                            # time even though each individual call still has its
                            # own node cap.
BFS_DEADLINE_CHECK_EVERY = 4   # nodes between deadline checks inside a flood
                                # fill / search. time.perf_counter() itself is
                                # cheap (~50-100ns), so this stays a small
                                # fraction of normal per-node cost, but keeps
                                # the worst-case overrun small even if a single
                                # node's work is unexpectedly slow (e.g. host
                                # contention) rather than just uniformly slower
                                # - see test_deadline.py for why 32 was too
                                # coarse: it let a slow node blow well past
                                # budget before the next check even happened.

KNOWN_EDGES = {}       # (x, y) -> {Direction: EdgeType}, remembered forever once seen


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

def _deadline_hit(popped, deadline):
    """Shared cadence check used by every flood fill / search below: only
    call perf_counter() every BFS_DEADLINE_CHECK_EVERY nodes, same reasoning
    as the original astar deadline check. Centralised here so room(),
    terrain_reachable() and multi_source_dist() all use one definition
    instead of three copies drifting apart."""
    if deadline is None:
        return False
    return popped % BFS_DEADLINE_CHECK_EVERY == 0 and time.perf_counter() >= deadline


def terrain_reachable(start, cap=BFS_CAP, deadline=None):
    """Flood fill using only known edge terrain (kelp vs empty), ignoring
    dragons and danger entirely - kelp never moves, so this answers a
    narrower, more stable question than a live search does: 'is there any
    possible route at all', not 'is there a route that's clear and safe
    right now'. Uses the same remembered-edge memory as everything else, so
    it can see past the current 7x7 window for targets near the edge of view.
    Portal edges don't count as passable here either, same reasoning as
    everywhere else: we can't compute where they lead.

    Returns the set of (x, y) tiles reached within `cap` steps, or fewer if
    `deadline` is hit first - in that case we simply return whatever's been
    confirmed reachable so far. That's always safe here: this result is only
    used to *filter* pearl candidates down to reachable ones, so stopping
    early just means a few borderline-distant pearls get conservatively
    dropped, never that an unreachable one gets waved through.

    With enough remembered map, this can run considerably larger than the
    visible 7x7 window - a long-lived dragon (or any AGGRESSIVE dragon, which
    treats totally unseen edges as passable) can have this legitimately
    approach `cap` on a fairly open map, not just in a pathological kelp
    maze, which is why it now shares the same time discipline as everything
    else instead of being bounded by node count alone."""
    seen = {(start.x, start.y)}
    queue = deque([start])
    popped = 0
    while queue and len(seen) < cap:
        pos = queue.popleft()
        popped += 1
        if _deadline_hit(popped, deadline):
            break
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
 
 
def gather_pearl_targets(here, deadline=None):
    """Every tile worth heading toward for a pearl: ones that already have
    one (cost 0), plus ones that don't yet but will soon enough to be worth
    planning a path to. Skips pending tiles that currently have a dragon
    sitting on them - a pearl can't spawn under a dragon, so an occupied
    tile's countdown isn't trustworthy until whoever's there moves off.
 
    Also drops anything not actually reachable from `here` through known
    non-kelp terrain - a pearl sitting in a pocket permanently sealed off by
    kelp is worth zero, not "very far away", and letting it into the target
    list lets its Manhattan-distance fallback mislead a direction comparison
    toward a target that can never be collected. This check only concerns
    permanent terrain, not dragons or danger - a target temporarily blocked
    by another dragon standing in the way is still included here, since that
    dragon will move."""
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
 
    reachable = terrain_reachable(here, deadline=deadline)
    return [(pos, pt) for pos, pt in candidates if (pos.x, pos.y) in reachable]

def pearl_dist(pos, targets):
    """Turns until pos could plausibly collect the best target: whichever is
    later, the walk there or the spawn actually happening. Reduces to plain
    distance for pearls that already exist, since their time is 0. Cheap
    Manhattan estimate - used as the emergency fallback once we're already
    out of time, and as the per-pending-target floor inside
    pearl_dist_shared below."""
    if not targets:
        return 0
    return min(max(dist(pos, p), t) for p, t in targets)

def has_room_to_split(deadline=None):
    """Flood-fills the open space around us (kelp and other dragons both
    block it, same as room() does for movement) and refuses to split if
    it's too walled-in to comfortably hold both the parent and the new
    child afterward - a tight, kelp-choked pocket is exactly where two
    independent bodies end up ramming each other."""
    here = ct.get_position()
    needed = ct.get_length() * SPLIT_ROOM_MULTIPLIER
    return room(here, here, needed, deadline=deadline) >= needed

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
    Used for proximity/threat checks (is_fatal, danger, ally_crowding), as
    the search heuristic, and as the fallback wherever a real path can't be
    confirmed in time."""
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

def room(start, came_from, cap, deadline=None):
    """Flood fill from start over free tiles in view. Returns how many tiles are
    reachable, stopping at cap. Reaching the edge of our view counts as
    'plenty of room' because we can't see what lies beyond - and hitting
    `deadline` gets the same treatment, for the same reason unknown-territory
    already did: we can't disprove there's room out there, so we don't
    penalise a move just because we ran out of time to keep counting."""
    seen = {came_from, start}
    queue = deque([start])
    count = 0
    popped = 0
    while queue:
        pos = queue.popleft()
        count += 1
        if count >= cap:
            return cap
        popped += 1
        if _deadline_hit(popped, deadline):
            return cap
        for d in Direction.get_direction_list():
            if known_edge_type(pos, d) != EdgeType.EMPTY:
                continue
            nxt = pos.add_dir(d)
            if nxt in seen:
                continue
            if not tile_ever_seen(nxt):
                return cap
            if known_occupied(nxt):
                continue
            seen.add(nxt)
            queue.append(nxt)
    return count

def tile_ever_seen(pos):
    if ct.get_tile(pos) is not None:
        return True
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
# Vision only ever shows the 7x7 window around our head, so a single search
# on its own can't route more than 3 tiles out. Terrain is worth remembering
# past that boundary, purely from this one dragon's own vision history - no
# sonar, nothing shared with any other dragon: kelp and walls never move
# once placed, so any edge we've ever seen can be trusted forever, long
# after it scrolls out of view.

def remember_vision():
    """Fold everything currently visible into memory. Call once per turn,
    before making any decisions, so this turn's own tiles are included."""
    for tile in ct.get_tiles():
        pos = tile.get_position()
        key = (pos.x, pos.y)
        KNOWN_EDGES[key] = {d: tile.get_edge(d).get_edge_type() for d in Direction.get_direction_list()}


def known_edge_type(pos, d):
    """The edge type stepping d from pos: read live if pos is in view right
    now, else whatever we remembered from the last time we saw pos. None
    means we've never seen this edge at all, so - same as an unseen tile
    does for a single step - it stays untraversable."""
    tile = ct.get_tile(pos)
    if tile is not None:
        return tile.get_edge(d).get_edge_type()
    edges = KNOWN_EDGES.get((pos.x, pos.y))
    if edges is None:
        if AGGRESSIVE:
            return EdgeType.EMPTY
        return EdgeType.KELP        # unseen & safe -> untraversable, not a crash
    return edges.get(d, EdgeType.EMPTY)


def known_occupied(pos):
    """Whether pos currently holds a dragon. Only live vision can answer
    this - a remembered tile's occupant could easily have moved on since we
    last looked, so anything out of view is assumed clear."""
    tile = ct.get_tile(pos)
    return tile is not None and tile.get_dragon() is not None


# ---------- shared pathfinding ----------
#
# choose_direction previously scored each of up to 4 candidate directions by
# running a full A* search separately for each one (pearl_dist_astar), even
# though every one of those searches starts within 1 step of `here` and
# targets the exact same pearls - almost entirely overlapping work, up to
# ~4x redundant per turn. Since the movement graph's traversability doesn't
# depend on which way an edge is crossed, and every already-spawned pearl
# (pt == 0) shares the same zero starting cost, this can be inverted into a
# single multi-source Dijkstra seeded from every such pearl at once: the
# resulting distance field then answers "shortest path to nearest pearl"
# for every candidate direction with one O(1) lookup each, computed once.
#
# This only covers pt == 0 targets. Pending targets (pt > 0) have a floor of
# max(dist, pt), not dist itself, so they can't share one additive
# zero-seeded search the way already-spawned pearls can - a target 6 steps
# away with pt=2 isn't "distance 6 from a seed at cost 2", it's capped below
# by pt regardless of path length, which breaks the seed-then-relax
# invariant a shared search relies on. PEARL_SPAWN_HORIZON keeps that set
# small, so those still go through the cheap Manhattan estimate per
# direction, same as the old astar fallback path did for unreached targets.

def multi_source_dist(sources, others, cap=BFS_CAP, deadline=None, avoid_fatal=True):
    """Dijkstra run once from every position in `sources` simultaneously.
    Returns dist_field: {pos: shortest known step count from pos to the
    nearest source}, over the same known-passable-edge / not-occupied /
    (optionally) not-fatal graph the old per-direction search used.

    Stops early, same as before, once `cap` positions are settled or
    `deadline` is hit - whichever positions haven't been reached yet simply
    aren't in dist_field, and the caller falls back to Manhattan distance
    for those (see pearl_dist_shared)."""
    dist_field = {}
    heap = []
    counter = 0
    for pos in sources:
        if pos not in dist_field:
            dist_field[pos] = 0
            heapq.heappush(heap, (0, counter, pos))
            counter += 1

    popped = 0
    while heap and len(dist_field) < cap:
        d, _, pos = heapq.heappop(heap)
        if d > dist_field.get(pos, d):
            continue   # stale heap entry, a shorter path to pos was already found
        popped += 1
        if _deadline_hit(popped, deadline):
            break
        for dirn in Direction.get_direction_list():
            if known_edge_type(pos, dirn) != EdgeType.EMPTY:
                continue
            nxt = pos.add_dir(dirn)
            if known_occupied(nxt):
                continue
            if avoid_fatal and is_fatal(nxt, others):
                continue
            nd = d + 1
            if nxt not in dist_field or nd < dist_field[nxt]:
                dist_field[nxt] = nd
                counter += 1
                heapq.heappush(heap, (nd, counter, nxt))
    return dist_field


def pearl_dist_shared(pos, pending_targets, dist_field):
    """Best turns-until-collectable for pos, combining the shared
    already-spawned-pearl distance field (one O(1) lookup) with pending
    (pt > 0) targets via the cheap Manhattan floor, same formula the old
    per-direction fallback used. Falls back to 0 only if there's truly
    nothing to reach for, matching the old pearl_dist/pearl_dist_astar
    behaviour with no targets."""
    best = dist_field.get(pos)
    for target_pos, pt in pending_targets:
        candidate = max(dist(pos, target_pos), pt)
        if best is None or candidate < best:
            best = candidate
    return best if best is not None else 0


# ---------- decision ----------

def cheap_fallback_direction(moves, others, targets):
    """Used only when we're already out of time this turn (see
    TURN_TIME_BUDGET): skip the graph search entirely, but still reach for a
    pearl first using the cheap Manhattan estimate (pearl_dist costs nothing
    beyond a distance calc per target, unlike the graph search), rather than
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
    targets = gather_pearl_targets(here, deadline=deadline)

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
    space = {d: room(p, here, needed, deadline=deadline) for d, p in moves.items()}
    roomy = {d: p for d, p in moves.items() if space[d] >= needed}
    if roomy:
        moves = roomy
    else:
        best = max(space.values())
        moves = {d: p for d, p in moves.items() if space[d] == best}

    facing = ct.get_dir()

    # already out of time - don't start graph search scoring at all, just
    # take a legal, pearl-leaning move and get out
    if deadline is not None and time.perf_counter() >= deadline:
        return cheap_fallback_direction(moves, others, targets)

    spawned_sources = [p for p, pt in targets if pt == 0]
    pending_targets = [(p, pt) for p, pt in targets if pt > 0]

    if AGGRESSIVE:
        # skip danger, and skip the enemy-head veto too (avoid_fatal=False)
        # so routing doesn't silently detour around contested ground either -
        # one shared search for the nearest pearl among whatever's left,
        # breaking ties toward whichever option gives our own teammates more
        # room.
        dist_field = multi_source_dist(spawned_sources, others, deadline=deadline, avoid_fatal=False)
        def key(d):
            p = moves[d]
            return (pearl_dist_shared(p, pending_targets, dist_field),
                     ally_crowding(p, others), d != facing, random.random())
    else:
        # tiers 3 and 4: least danger first (danger already down-weights
        # enemy-head proximity gradually), then closest to a pearl via the
        # shared distance field, still avoiding fatal tiles in routing
        dist_field = multi_source_dist(spawned_sources, others, deadline=deadline, avoid_fatal=True)
        def key(d):
            p = moves[d]
            return (danger(p, others),
                     pearl_dist_shared(p, pending_targets, dist_field),
                     d != facing, random.random())

    return min(moves, key=key)

def try_split(others, deadline=None):
    """Aggressive dragons split as soon as they're long enough. Safe dragons
    never split, to keep their body intact for evasive pathfinding."""
    if not AGGRESSIVE:
        return False
    if ct.get_length() < SPLIT_MIN_LENGTH:
        return False
    if nearby_ally_count(others) > MAX_NEARBY_ALLIES_TO_SPLIT:
        return False
    if not has_room_to_split(deadline=deadline):
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

    if try_split(others, deadline=deadline):
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