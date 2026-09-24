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

KNOWN_EDGES = {}       # (x, y) -> {Direction: EdgeType}, remembered forever once seen


# ---------- portals: gamble pool, minus the one we know is a trap ----------
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
OWN_BODY = deque()      # our own body, head first, tail last - exact, not remembered/stale
OWN_BODY_SET = set()    # same positions, rebuilt each turn, for O(1) lookups


def update_own_body():
    """Rebuild exactly where our own body is right now, using only things we
    fully control: our own head position (ground truth every turn) and our
    own length (also ground truth). This never goes stale the way an
    enemy's position or remembered kelp does - we're the only one who moves
    our own head, so replaying "record today's head, keep only the most
    recent `length` positions" exactly reconstructs the whole body, vision
    or no vision.

    Handles growth and shrinkage the same simple way: eating a pearl grows
    length, so nothing gets popped and the old tail just stays put; a split
    shrinks length, so the trailing (tail-ward) entries get popped - which
    is exactly the chunk do_split hands off to the new child dragon.

    A split-only turn (no move that turn) is the one case that needs a
    guard: if the head hasn't actually moved, don't prepend a duplicate -
    just truncate what's already known, so the neck segment we already had
    right is preserved instead of getting overwritten by a fake copy of the
    head.
    """
    here = (ct.get_position().x, ct.get_position().y)
    if not OWN_BODY or OWN_BODY[0] != here:
        OWN_BODY.appendleft(here)
    length = ct.get_length()
    while len(OWN_BODY) > length:
        OWN_BODY.pop()
    OWN_BODY_SET.clear()
    OWN_BODY_SET.update(OWN_BODY)

def stable_roll(key):
    """Deterministic value in [0,1) derived from `key`. Same key always
    gives the same roll - unlike random.random(), this can't be desynced
    by seeding, call order, or how many other random() calls happened
    earlier this turn/process. Use this for any role-assignment decision
    that needs a reliable population-wide ratio."""
    h = hashlib.sha256(str(key).encode()).hexdigest()
    return int(h[:8], 16) / 0xFFFFFFFF

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

def nearby_enemy_count(others, radius=3):  # counts nearby enemy dragons
    here = ct.get_position()
    my_team = ct.get_team()
    seen_ids = set()
    for part in others:
        if part.get_team() == my_team:
            continue
        if part.get_id() in seen_ids:
            continue
        if dist(here, part.get_position()) <= radius:
            seen_ids.add(part.get_id())
    return len(seen_ids)

def ratio(others):
    ally = nearby_ally_count(others)
    enemy = nearby_enemy_count(others)
    if ally != 0:
        return enemy / ally
    elif enemy > 2:
        return 3
    else:
        return 0

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
# Vision only ever shows the 7x7 window around our head, so BFS on its own
# can't route more than 3 tiles out. Terrain is worth remembering past that
# boundary, purely from this one dragon's own vision history - no sonar,
# nothing shared with any other dragon: kelp and walls never move once
# placed, so any edge we've ever seen can be trusted forever, long after it
# scrolls out of view.

def remember_vision():
    """Fold everything currently visible into memory. Call once per turn,
    before making any decisions, so this turn's own tiles are included."""
    for tile in ct.get_tiles():
        pos = tile.get_position()
        key = (pos.x, pos.y)
        edges = {d: tile.get_edge(d).get_edge_type() for d in Direction.get_direction_list()}
        KNOWN_EDGES[key] = edges
        for d, edge_type in edges.items():
            if edge_type == EdgeType.KELP:
                add_kelp(edge_key(pos, d))   # sonar: remember it so we can tell teammates


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
        if edge_key(pos, d) in KELP_EDGES:
            return EdgeType.KELP        # never seen it ourselves, but a teammate's sonar told us
        if AGGRESSIVE:
            return EdgeType.EMPTY
        return EdgeType.KELP        # unseen & safe -> untraversable, not a crash
    return edges.get(d, EdgeType.EMPTY)


def known_occupied(pos):
    """Whether pos currently holds a dragon. Live vision is ground truth for
    any dragon, ours or an enemy's, when pos is in view. Out of view, an
    enemy's last-seen position is only a guess (it may well have moved), so
    it's treated as clear - but our own body is the one exception: see
    update_own_body, which reconstructs it exactly with no need for vision,
    so it stays correct even off-screen."""
    tile = ct.get_tile(pos)
    if tile is not None:
        return tile.get_dragon() is not None
    return (pos.x, pos.y) in OWN_BODY_SET


# ---------- sonar: share our mode, length, id and kelp map with teammates ----------
#
# Every sonar is one 64-bit integer, packed like this (top bits first):
#
#   16 bits  magic tag   - sonar carries no sender/team info, so enemies can hear
#                          us and we can hear them; the tag lets us ignore junk
#    1 bit   mode        - 1 = aggressive, 0 = safe
#    7 bits  length      - capped at 127
#   12 bits  dragon id   - low 12 bits of the id
#    2 bits  kelp count  - how many of the 2 kelp slots below are real (0-2)
#   13 bits  kelp slot 1 \_ each = x (6 bits), y (6 bits), which edge (1 bit)
#   13 bits  kelp slot 2 /   maps are at most 64x64, so 6 bits per coordinate
#
# A kelp "edge" sits between two tiles, so both tiles could describe it. To keep
# one name per edge we always store it as the NORTH edge (0) or WEST edge (1) of
# a tile - see edge_key().

SONAR_MAGIC = 0xA15A
REPORT_LAYOUT = [16, 1, 7, 12, 2, 13, 13]
EDGE_LAYOUT = [6, 6, 1]
KELP_PER_MSG = 2
ID_MASK = (1 << 12) - 1

SONAR3 = False          # set in main(): True only if helper.py has the protocol 3 sonar API
KELP_EDGES = set()      # every kelp edge we know of (seen ourselves or heard from a teammate)
KELP_LIST = []          # same edges, in a list so we can send them round-robin
KELP_SEND_CURSOR = 0    # where in KELP_LIST the next message starts
TEAMMATES = {}          # dragon id -> {"aggressive": bool, "length": int, "round": int}


def pack(fields):
    """[(value, bits), ...] -> one integer. First field ends up in the top bits."""
    n = 0
    for value, bits in fields:
        n = (n << bits) | (value & ((1 << bits) - 1))
    return n


def unpack(n, widths):
    """Reverse of pack: split n into fields of the given bit widths."""
    out = []
    for bits in reversed(widths):
        out.append(n & ((1 << bits) - 1))
        n >>= bits
    out.reverse()
    return out


def edge_key(pos, d):
    """One shared name for the edge you cross stepping d from pos: (x, y, 0) is
    the north edge of tile (x, y), (x, y, 1) is its west edge. A south/east edge
    is just the north/west edge of the neighbouring tile."""
    if d == Direction.SOUTH or d == Direction.EAST:
        pos = pos.add_dir(d)
        d = d.get_opposite()
    return (pos.x, pos.y, 0 if d == Direction.NORTH else 1)


def add_kelp(key):
    if key not in KELP_EDGES:
        KELP_EDGES.add(key)
        KELP_LIST.append(key)


def next_kelp_chunk():
    """The next KELP_PER_MSG kelp edges to send, cycling through everything we
    know. Most sonars never reach a teammate, so we keep re-sending in a loop."""
    global KELP_SEND_CURSOR
    chunk = []
    for _ in range(min(KELP_PER_MSG, len(KELP_LIST))):
        chunk.append(KELP_LIST[KELP_SEND_CURSOR % len(KELP_LIST)])
        KELP_SEND_CURSOR += 1
    return chunk


def encode_report(kelp_chunk):
    slots = [pack(list(zip(k, EDGE_LAYOUT))) for k in kelp_chunk]
    slots += [0] * (KELP_PER_MSG - len(slots))
    return pack(list(zip(
        [SONAR_MAGIC, int(AGGRESSIVE), min(ct.get_length(), 127),
         ct.get_id() & ID_MASK, len(kelp_chunk), slots[0], slots[1]],
        REPORT_LAYOUT)))


def send_sonar_report():
    """Call once per turn, after our move. Sends one report out of each side
    (a sonar stops at the first thing it hits, so more directions = more chances
    of reaching a teammate), each carrying a different slice of our kelp map."""
    if not SONAR3:
        return
    for d in Direction.get_direction_list():
        ct.send_sonar(d, encode_report(next_kelp_chunk()))


def read_sonar(round_num):
    """Parse everything that reached us since last turn. Call at the start of
    the turn so any new kelp is already in the map before we plan a move."""
    width, height = game.get_map_size()
    for msg in ct.get_sonar_messages():
        magic, aggressive, length, sender, count, *slots = unpack(msg, REPORT_LAYOUT)
        if magic != SONAR_MAGIC:
            continue                        # not one of ours
        if sender == ct.get_id() & ID_MASK:
            continue                        # our own sonar bounced back to us
        TEAMMATES[sender] = {"aggressive": bool(aggressive), "length": length, "round": round_num}
        for slot in slots[:count]:
            x, y, horizontal = unpack(slot, EDGE_LAYOUT)
            if x < width and y < height:    # ignore coordinates that can't be real
                add_kelp((x, y, horizontal))


# ---------- A* pathfinding ----------

def astar_targets(here, others, target_positions, cap=BFS_CAP, deadline=None, avoid_fatal=True):
    """A* from `here` over the known passable-edge graph (same rules as the
    old BFS: edge must be known EMPTY, landing tile not occupied, not
    is_fatal, portals skipped since crossing one doesn't land at add_dir's
    tile) - but guided by Manhattan distance to the nearest still-unsettled
    target instead of expanding blindly in every direction.

    dist() is a valid (admissible) heuristic here: every real step covers
    at most 1 unit of Manhattan distance, even accounting for map wraparound
    (dist() already handles that), so it never overestimates the true
    remaining cost and A* still finds genuine shortest paths - it just
    reaches them by exploring far less of the map than a flood-fill does.

    This is the actual fix for the timeout: the old BFS kept expanding
    until it hit `cap` tiles even after every pearl we cared about was
    already found. This stops the instant every target in
    `target_positions` has been settled with its optimal distance (or cap/
    deadline is hit), so a nearby pearl no longer costs a full map-sized
    search to find.

    Returns dist_to: {pos: shortest known step count from `here`}."""
    dist_to = {here: 0}
    if not target_positions:
        return dist_to

    remaining = set(target_positions)

    def h(pos):
        return min(dist(pos, t) for t in remaining) if remaining else 0

    counter = 0
    heap = [(h(here), 0, counter, here)]
    visited = set()
    popped = 0

    while heap and remaining and len(dist_to) < cap:
        f, g, _, pos = heapq.heappop(heap)
        if pos in visited:
            continue
        visited.add(pos)
        remaining.discard(pos)

        if deadline is not None:
            popped += 1
            if popped % BFS_DEADLINE_CHECK_EVERY == 0 and time.perf_counter() >= deadline:
                break

        for d in Direction.get_direction_list():
            if known_edge_type(pos, d) != EdgeType.EMPTY:
                continue
            nxt = pos.add_dir(d)
            if known_occupied(nxt):
                continue
            if avoid_fatal and is_fatal(nxt, others):
                continue
            new_g = g + 1
            if nxt not in dist_to or new_g < dist_to[nxt]:
                dist_to[nxt] = new_g
                counter += 1
                heapq.heappush(heap, (new_g + h(nxt), new_g, counter, nxt))

    return dist_to


def pearl_dist_astar(pos, targets, others, deadline=None, avoid_fatal=True):
    """Real shortest-path turns-until-collectable for the best target, using
    A* run fresh from `pos` (the tile this move would land on), same
    per-direction reasoning as the old pearl_dist_bfs: a single search from
    `here` can't give every direction its own honest distance, so each
    candidate move gets its own run. Falls back to Manhattan-based pearl_dist
    only for a target A* genuinely couldn't reach (outside cap, or through
    unexplored territory)."""
    if not targets:
        return 0
    target_positions = [p for p, _ in targets]
    dist_to = astar_targets(pos, others, target_positions, deadline=deadline, avoid_fatal=avoid_fatal)
    best = None
    for target_pos, pt in targets:
        if target_pos in dist_to:
            turns = max(dist_to[target_pos], pt)
        else:
            turns = max(dist(pos, target_pos), pt)
        if best is None or turns < best:
            best = turns
    return best

# ---------- kamikaze: aggressive-only kill sprint, gated on trade value ----------
#
# stepping onto another dragon's head kills both of you (see DragonPart.is_head's
# own docstring) - a body segment doesn't have that guarantee, so a kamikaze run
# only ever targets a visible enemy HEAD, never a body tile.
#
# The gate is the trade itself, not the round number: giving up our own
# dragon is only worth it if the enemy loses at least as much as we do.
#
# Caveat worth checking against a real replay before trusting this: get_id()'s
# docstring says your id IS your place in the turn order within a round, which
# means dragons likely don't move simultaneously. If we act before the target
# this round, the head position we see right now may not be where they still
# are once their own turn resolves - they could just move off it. If we act
# after them, their reported position is already final for the round. This
# function paths onto the CURRENTLY reported head position either way; if kills
# aren't landing in testing, that turn-order gap is the first thing to check.

KAMIKAZE_MIN_LENGTH_RATIO = 1.1   # only trade if the enemy's VISIBLE length is at
                                    # least this multiple of ours. 1.0 = break-even
                                    # or better; raise it (e.g. 1.5) to only take
                                    # trades that are clearly in our favor.


def nearest_enemy_head(others):
    """Closest visible enemy head, or None. Never a body segment - see the
    section note above for why."""
    here = ct.get_position()
    my_team = ct.get_team()
    heads = [p for p in others if p.is_head() and p.get_team() != my_team]
    if not heads:
        return None
    return min(heads, key=lambda p: dist(here, p.get_position()))


def visible_enemy_length(target, others):
    """How many segments of target's dragon we can currently see - a LOWER
    bound on their true length, since any part of their body sitting
    outside our 7x7 window just doesn't get counted. That undercount is the
    safe direction for this decision: if even what we can see already looks
    break-even or better for us, the real trade can only be as good or
    better than it looks, never worse."""
    return sum(1 for p in others if p.get_id() == target.get_id())


def trade_is_favorable(target, others):
    """Whether killing target is worth losing our own dragon for. Compares
    our exact length (ground truth) against the enemy's visible segment
    count. We only commit when what we can already see makes the trade
    look break-even or better - we have no way to know a trade that looks
    bad is secretly good, only that one that already looks good might be
    even better once their off-screen segments are counted too."""
    our_length = ct.get_length()
    their_visible = visible_enemy_length(target, others)
    return their_visible >= our_length * KAMIKAZE_MIN_LENGTH_RATIO


def path_onto(here, target_pos, cap=BFS_CAP, deadline=None):
    """A* from here to target_pos specifically - not astar_targets above,
    because that function refuses to ever step onto ANY occupied tile, and
    the whole point here is that target_pos IS occupied (the enemy head).
    Every other tile along the way is still blocked exactly as normal -
    kelp, any other dragon's body, map edge - only target_pos itself gets
    the exception, and only as the literal last step, so this can't be used
    to plow through some unrelated third dragon to reach the target.

    Returns the ordered list of Directions to take, or None if target_pos
    isn't reachable within cap/deadline."""
    if here == target_pos:
        return []

    came_from = {}
    dist_to = {here: 0}

    def h(pos):
        return dist(pos, target_pos)

    counter = 0
    heap = [(h(here), 0, counter, here)]
    visited = set()
    popped = 0

    while heap:
        f, g, _, pos = heapq.heappop(heap)
        if pos in visited:
            continue
        visited.add(pos)

        if pos == target_pos:
            path = []
            cur = pos
            while cur != here:
                cur, d = came_from[cur]
                path.append(d)
            path.reverse()
            return path

        if deadline is not None:
            popped += 1
            if popped % BFS_DEADLINE_CHECK_EVERY == 0 and time.perf_counter() >= deadline:
                return None
        if len(dist_to) >= cap:
            return None

        for d in Direction.get_direction_list():
            if known_edge_type(pos, d) != EdgeType.EMPTY:
                continue
            nxt = pos.add_dir(d)
            if nxt != target_pos and known_occupied(nxt):
                continue   # anyone else's body still blocks a detour through it
            new_g = g + 1
            if nxt not in dist_to or new_g < dist_to[nxt]:
                dist_to[nxt] = new_g
                came_from[nxt] = (pos, d)
                counter += 1
                heapq.heappush(heap, (new_g + h(nxt), new_g, counter, nxt))

    return None


def kamikaze_charge(others, deadline=None):
    """Aggressive-only: if a visible enemy head can be reached and hit in a
    single sprint this turn, AND the trade looks favorable, take it -
    deliberately spending ourselves to remove an enemy that's worth at
    least as much as we are. Returns True if it sent a sprint this turn,
    else False (nothing to do, nothing reachable in one turn, or the trade
    doesn't look worth it)."""
    if not AGGRESSIVE:
        return False

    target = nearest_enemy_head(others)
    if target is None:
        return False

    if not trade_is_favorable(target, others):
        return False

    here = ct.get_position()
    path = path_onto(here, target.get_position(), deadline=deadline)
    if not path:
        return False

    # a sprint of n steps costs n - 1 segments, so length must exceed n
    if len(path) >= ct.get_length():
        return False

    ct.make_moves(path)
    return True


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

    if AGGRESSIVE:
        # skip danger, and skip the enemy-head veto too (avoid_fatal=False)
        # so routing doesn't silently detour around contested ground either -
        # go straight for the nearest pearl among whatever's left, using the
        # real A* shortest-path distance, breaking ties toward whichever
        # option gives our own teammates more room.
        def key(d):
            p = moves[d]
            return (pearl_dist_astar(p, targets, others, deadline=deadline, avoid_fatal=False),
                     ally_crowding(p, others), d != facing, random.random())
    else:
        # tiers 3 and 4: least danger first (danger already down-weights
        # enemy-head proximity gradually), then closest to a pearl by real
        # A* shortest-path distance, still avoiding fatal tiles in routing
        def key(d):
            p = moves[d]
            return (danger(p, others),
                     pearl_dist_astar(p, targets, others, deadline=deadline, avoid_fatal=True),
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

    enemy_ratio = ratio(others)
    
    if enemy_ratio > 2:
        if ct.can_split(2):
            ct.do_split(2)
            return

    update_own_body()
    remember_vision()
    read_sonar(round_num)

    if try_split(others):
        return

    if kamikaze_charge(others, deadline=deadline):
        return

    if round_num > 425 and not enemy_nearby(others): # activate feeding
        if seppuku_conditions(others):
            suicide()
            return

    d = choose_direction(others, round_num, deadline=deadline)

    if d is None:
        d = last_resort(round_num)
        if d is None:
            return   # last_resort already split; no move to send
    ct.make_move(d)


def suicide(): # literally just makes the bot kills itself
    opposite = ct.get_dir().get_opposite()
    ct.make_move(opposite)
    return 

def enemy_nearby(others):
    my_team = ct.get_team()
    for drag in others:
        if drag.get_team() != my_team:
            return True
    return False

def seppuku_conditions(others):
    max_length = 0
    max_visible_length = 0
    if TEAMMATES:
        max_length = max(t["length"] for t in TEAMMATES.values())
    ally_found = False
    my_team = ct.get_team()
    for drag in others:
        if drag.get_team() == my_team and drag.is_head():
            ally_found = True

            masked_id = drag.get_id() & ID_MASK

            if masked_id in TEAMMATES:
                ally_length = TEAMMATES[masked_id]["length"]
                if ally_length > max_visible_length:
                    max_visible_length = ally_length
    max_avg = (max_length + max_visible_length + max_visible_length) / 3

    if ct.get_length() < max_avg * 0.8:
        return ally_found
    return False


# ---------- boilerplate ----------

def main() -> None:
    global ct, game, AGGRESSIVE, SONAR3
    ct, game = unswbc.init()
    SONAR3 = hasattr(ct, "get_sonar_echoes")   # only the protocol 3 helper has this
    rolled = False

    
    while unswbc.update(ct, game):
        
        if rolled == False:
            progress = (game.get_round_num() - 1) / 350                            # 0 at start, 1 around round 350
            AGGRO_CHANCE = max(1 - 0.6 * progress, 0.2)                    # 100% -> floors at 40%
            AGGRESSIVE = stable_roll(ct.get_id()) < AGGRO_CHANCE                     # heavy subject to change
            rolled = True

        if ct.get_length() > 5:    # Prevention from desperate split child from becoming aggressive
            AGGRESSIVE = False
        
        execute_turn()
        send_sonar_report()   # sonars fire after our action, so send after the move
        unswbc.end_turn()


if __name__ == "__main__":
    main()