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
PORTAL_CHANCE = 1               # aggressive dragons: odds of diving into an
                                # available portal this turn instead of pathing normally

MAX_NEARBY_ALLIES_TO_SPLIT = 3  # skip splitting if more than this many other allies are close

PEARL_ROOM_WEIGHT = 0           # How much a pearl counts as space in split_room()

SPLIT_ROOM_MULTIPLIER = 1.5     # a split needs room for two independent bodies, not
                                # one, so require more open space than plain
                                # movement (tier 2) does before committing to it
PEARL_SPAWN_HORIZON = 5         # ignore spawns further out than this - too likely to
                                # be irrelevant or contested by the time we'd arrive

BFS_CAP = 120                   # per-search node cap; pearl_dist_astar reruns A* for
                                # every candidate direction, so this scales the most
                                # expensive part of a turn.

TURN_TIME_BUDGET = 0.06         # seconds. Stop chasing search precision once this much
                                # of the turn has elapsed and fall back to a cheap move.
BFS_DEADLINE_CHECK_EVERY = 4    # nodes between deadline checks inside a search.

KNOWN_EDGES = {}       # (x, y) -> {Direction: EdgeType}, remembered forever once seen

# Per-turn memoization for known_edge_type/known_occupied. Nothing about vision
# or occupancy changes mid-turn, so both are cached for one turn and cleared at
# the top of the next - see reset_turn_caches(), called from execute_turn.
_EDGE_CACHE = {}
_OCCUPIED_CACHE = {}


def reset_turn_caches():
    _EDGE_CACHE.clear()
    _OCCUPIED_CACHE.clear()


# Direction.get_direction_list() allocates a new list on every call (see
# helper.py). None of our call sites mutate it, so grab it once and reuse.
DIRS = Direction.get_direction_list()


# ---------- portals: gamble pool, minus the one we know is a trap ----------
# Crossing a portal edge lands us somewhere add_dir can't compute, so we can't
# score what's on the other side - that risk stays exclusive to the aggressive
# style. But the specific portal we just crossed is not a mystery: re-entering
# that exact id puts us back on the tile we just departed, which our own neck
# still occupies for roughly `length` rounds - a guaranteed self-collision.

LAST_PORTAL_ID = -1          # portal_id of the last portal we crossed, or -1 if none yet
LAST_PORTAL_ROUND = -10**9   # round we crossed it, far in the past so the cooldown starts expired

# ---------- helpers ----------
OWN_BODY = deque()      # our own body, head first, tail last - exact, not remembered/stale
OWN_BODY_SET = set()    # same positions, rebuilt each turn, for O(1) lookups

PEARL_CAPABLE = set()   # (x, y) tiles we've ever seen that CAN spawn a pearl
                        # (pearl_time != -1) - remembered forever.

# ---------- region-size pearl memory (local only, never sent over sonar) ----------
#
# We bucket the map into REGION_SIZE x REGION_SIZE squares and keep two
# per-region numbers:
#   REGION_PEARL_SCORE - how many distinct spawn-capable tiles we've seen there
#                        (static: a property of the map layout, never depletes)
#   REGION_VISITS      - how many turns our own head has spent there
#                        (dynamic: grows while we linger, so a region becomes
#                        less attractive the longer we stay in it)
# The wander tiebreak (region_bias) is dominated by VISITS so it pushes us
# toward unvisited regions instead of holding us in the spawn region, with
# PEARL_SCORE only a mild bonus.
REGION_SIZE = 4
REGION_PEARL_SCORE = {}       # (rx, ry) -> distinct spawn-capable tiles seen there
REGION_VISITS = {}            # (rx, ry) -> turns our head has spent in that region
REGION_PEARL_WEIGHT = 0.05    # how much the static pearl score offsets visits
_COUNTED_PEARL_TILES = set()  # spawn-capable tiles already counted
BARREN_TILES = set()   # tiles whose pearl_time is -1 (never spawn a pearl) - remembered forever
DEAD_ENDS = set()      # tiles with at most one open (non-kelp, non-portal) edge - cul-de-sacs


def update_own_body():
    """Rebuild exactly where our own body is right now from our own head
    position and length (both ground truth). Eating grows length (nothing
    popped); a split shrinks it (tail entries popped). A split-only turn
    doesn't move the head, so we don't prepend a duplicate."""
    here = (ct.get_position().x, ct.get_position().y)
    if not OWN_BODY or OWN_BODY[0] != here:
        OWN_BODY.appendleft(here)
    length = ct.get_length()
    while len(OWN_BODY) > length:
        OWN_BODY.pop()
    OWN_BODY_SET.clear()
    OWN_BODY_SET.update(OWN_BODY)


def stable_roll(key):
    """Deterministic value in [0,1) derived from `key`."""
    h = hashlib.sha256(str(key).encode()).hexdigest()
    return int(h[:8], 16) / 0xFFFFFFFF


def terrain_reachable(start, cap=81, targets=None):
    """A* reachability over known terrain (dragons ignored). Returns the set of
    (x, y) tiles discovered; stops early once every target is found. Unseen
    tiles are marked reachable but not expanded past."""
    start_key = (start.x, start.y)
    seen = {start_key}
    remaining = None
    if targets is not None:
        remaining = set(targets)
        remaining.discard(start_key)
        if not remaining:
            return seen

    def key_dist(a, b):
        w, h = WIDTH, HEIGHT
        dx = abs(a[0] - b[0]); dx = min(dx, w - dx)
        dy = abs(a[1] - b[1]); dy = min(dy, h - dy)
        return dx + dy

    def heuristic(pos_key):
        return min(key_dist(pos_key, t) for t in remaining) if remaining else 0

    counter = 0   # load-bearing heap tiebreaker: Position isn't orderable
    heap = [(heuristic(start_key), 0, counter, start)]   # (f, g, tiebreak, pos)
    visited = set()
    expanded = 0

    while heap and expanded < cap:
        _, g, _, pos = heapq.heappop(heap)
        pos_key = (pos.x, pos.y)
        if pos_key in visited:
            continue
        visited.add(pos_key)
        expanded += 1

        for d in DIRS:
            if known_edge_type(pos, d) != EdgeType.EMPTY:
                continue
            nxt = pos.add_dir(d)
            key = (nxt.x, nxt.y)
            if key in seen:
                continue
            seen.add(key)
            if remaining is not None:
                remaining.discard(key)
                if not remaining:
                    return seen
            if not tile_ever_seen(nxt):
                continue   # fringe: reachable, but don't expand past it
            new_g = g + 1
            counter += 1
            heapq.heappush(heap, (new_g + heuristic(key), new_g, counter, nxt))

    return seen


def gather_pearl_targets(here, tiles):
    """Every tile worth heading toward for a pearl: ones that already have
    one (cost 0), plus ones that will spawn within PEARL_SPAWN_HORIZON.
    Skips pending tiles with a dragon on them, and drops anything not
    reachable from `here` through known non-kelp terrain."""
    candidates = []
    for t in tiles:
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

    candidate_positions = {(pos.x, pos.y) for pos, _ in candidates}
    reachable = terrain_reachable(here, targets=candidate_positions)

    targets = []
    for pos, pt in candidates:
        key = (pos.x, pos.y)
        if key not in reachable:
            continue
        PEARL_CAPABLE.add(key)
        targets.append((pos, pt))

    return targets


def pearl_dist(pos, targets):
    """Turns until pos could plausibly collect the best target: whichever is
    later, the walk there or the spawn actually happening."""
    if not targets:
        return 0
    return min(max(dist(pos, p), t) for p, t in targets)


# ---------- pearl clusters: prefer a dense patch over scattered singles ----------

CLUSTER_RADIUS = 2      # pearls within this many tiles (Manhattan) of each
                        # other count as part of the same cluster
CLUSTER_MIN_SIZE = 6    # need at least this many pearls packed that tightly


def find_pearl_cluster(targets):
    """Return the largest tight group of at least CLUSTER_MIN_SIZE pearls
    among `targets`, or None if nothing that dense is around."""
    if len(targets) < CLUSTER_MIN_SIZE:
        return None

    positions = [p for p, _ in targets]
    best_group = None
    for p in positions:
        group = [t for t, q in zip(targets, positions) if dist(p, q) <= CLUSTER_RADIUS]
        if len(group) >= CLUSTER_MIN_SIZE:
            if best_group is None or len(group) > len(best_group):
                best_group = group
    return best_group


def split_room(start, came_from, needed, bfs_cap=50):
    """Flood fill from start over free tiles in view, same traversal rules as
    room(), but tiles with a pearl (or one spawning soon) only contribute
    PEARL_ROOM_WEIGHT instead of 1. `needed` is the weighted threshold;
    `bfs_cap` bounds actual tiles expanded."""
    seen = {came_from, start}
    queue = deque([start])
    total = 0.0
    expanded = 0

    while queue:
        if total >= needed or expanded >= bfs_cap:
            break
        pos = queue.popleft()
        expanded += 1

        weight = 1.0
        tile = ct.get_tile(pos)
        if tile is not None:
            pt = tile.get_pearl_time()
            if tile.has_pearl() or (0 <= pt <= 25):
                weight = PEARL_ROOM_WEIGHT
        total += weight

        for d in Direction.get_direction_list():
            if known_edge_type(pos, d) != EdgeType.EMPTY:
                continue
            nxt = pos.add_dir(d)
            if nxt in seen:
                continue
            if not tile_ever_seen(nxt):
                return needed   # unseen fringe = plenty of room, same as room()
            if known_occupied(nxt):
                continue
            seen.add(nxt)
            queue.append(nxt)

    return total


def has_room_to_split():
    here = ct.get_position()
    needed = ct.get_length() * SPLIT_ROOM_MULTIPLIER
    return split_room(here, here, needed) >= needed


def nearby_ally_count(others, radius=3):
    """How many distinct ally dragons (besides us) have any visible segment
    within `radius` tiles of our head."""
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
    w, h = WIDTH, HEIGHT
    dx = abs(a.x - b.x)
    dx = min(dx, w - dx)
    dy = abs(a.y - b.y)
    dy = min(dy, h - dy)
    return dx + dy


def other_dragon_parts(tiles):
    """Every visible dragon segment that isn't part of this dragon."""
    my_id = ct.get_id()
    parts = []
    for t in tiles:
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
    """(direction, portal_id) pairs for portal edges from pos, excluding the
    one we crossed most recently while our tail could still be on the tile
    it would send us back to."""
    tile = ct.get_tile(pos)
    if tile is None:
        return []
    cooldown = ct.get_length()
    options = []
    for d in DIRS:
        edge = tile.get_edge(d)
        if edge.get_edge_type() != EdgeType.PORTAL:
            continue
        pid = edge.get_portal_id()
        if pid == LAST_PORTAL_ID and round_num - LAST_PORTAL_ROUND < cooldown:
            continue
        options.append((d, pid))
    return options


def remember_portal_crossing(pid, round_num):
    global LAST_PORTAL_ID, LAST_PORTAL_ROUND
    LAST_PORTAL_ID = pid
    LAST_PORTAL_ROUND = round_num


# ---------- hard veto: never walk next to an enemy head ----------

def build_fatal_zone(others):
    """Every tile fatal to land on: a visible enemy head plus its four
    neighbours (wraparound-aware), built once per turn for O(1) lookups."""
    my_team = ct.get_team()
    zone = set()
    for part in others:
        if not part.is_head() or part.get_team() == my_team:
            continue
        p = part.get_position()
        zone.add((p.x, p.y))
        for d in DIRS:
            n = p.add_dir(d)
            zone.add((n.x, n.y))
    return zone


def is_fatal(pos, fatal_zone):
    return (pos.x, pos.y) in fatal_zone


# ---------- tier 2: dead ends ----------

def room(start, came_from, cap, deadline=None):
    """Flood fill from start over free tiles in view. Returns how many tiles
    are reachable, stopping at cap, or early if deadline is hit (an
    underestimate, which is the safe direction)."""
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
        if deadline is not None and popped % BFS_DEADLINE_CHECK_EVERY == 0 and time.perf_counter() >= deadline:
            return count
        for d in DIRS:
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
    """How risky it is to end a step on pos. Heads matter most."""
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
    """How boxed-in this move would leave us relative to our own team.
    A lightweight tiebreaker, never a hard restriction."""
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

def region_key(pos):
    """Which REGION_SIZE x REGION_SIZE bucket a position falls in."""
    return (pos.x // REGION_SIZE, pos.y // REGION_SIZE)


def remember_region_visit():
    """Count one turn spent by our head in its current region. Called once
    per turn from execute_turn. This is what makes a region lose its pull the
    longer we linger, instead of holding us in the spawn region forever."""
    r = region_key(ct.get_position())
    REGION_VISITS[r] = REGION_VISITS.get(r, 0) + 1


def region_bias(pos):
    """Wander tiebreak for landing on pos - LOWER is better. Visits dominate
    (unvisited regions win, and a region fades the longer we stay), with the
    static spawn-capable count as a mild bonus only."""
    r = region_key(pos)
    return REGION_VISITS.get(r, 0) - REGION_PEARL_WEIGHT * REGION_PEARL_SCORE.get(r, 0)


def avoid_penalty(pos):
    """1 if pos is a remembered dead-end or permanently barren tile, else 0.
    Looks up the (x, y) tuple, since Position never equals a tuple."""
    key = (pos.x, pos.y)
    return 1 if key in DEAD_ENDS or key in BARREN_TILES else 0


def remember_vision(tiles):
    """Fold everything currently visible into memory. Call once per turn,
    before making any decisions, so this turn's own tiles are included."""
    for tile in tiles:
        pos = tile.get_position()
        key = (pos.x, pos.y)
        edges = {d: tile.get_edge(d).get_edge_type() for d in DIRS}
        KNOWN_EDGES[key] = edges
        for d, edge_type in edges.items():
            if edge_type == EdgeType.KELP:
                add_kelp(edge_key(pos, d))   # sonar: remember it so we can tell teammates

        # Pearl bookkeeping. A tile currently holding a pearl is spawn-capable
        # even if it reports pearl_time == -1 while the pearl sits there, so
        # has_pearl() is checked first - otherwise a real spawn tile would be
        # marked barren forever and never counted.
        pt = tile.get_pearl_time()
        if tile.has_pearl() or 0 <= pt < 25:
            BARREN_TILES.discard(key)
            if key not in _COUNTED_PEARL_TILES:
                r = region_key(pos)
                REGION_PEARL_SCORE[r] = REGION_PEARL_SCORE.get(r, 0) + 1
                _COUNTED_PEARL_TILES.add(key)
        elif pt == -1:
            BARREN_TILES.add(key)

        open_exits = sum(1 for t in edges.values() if t == EdgeType.EMPTY)
        if open_exits <= 1:
            DEAD_ENDS.add(key)
        else:
            DEAD_ENDS.discard(key)


def known_edge_type(pos, d):
    """The edge type stepping d from pos: read live if pos is in view, else
    remembered. Never-seen edges are EMPTY for aggressive dragons and KELP for
    safe ones (unless a teammate's sonar already reported kelp there).
    Memoized per turn."""
    key = (pos.x, pos.y, d)
    result = _EDGE_CACHE.get(key)
    if result is not None:
        return result

    tile = ct.get_tile(pos)
    if tile is not None:
        result = tile.get_edge(d).get_edge_type()
    else:
        edges = KNOWN_EDGES.get((pos.x, pos.y))
        if edges is None:
            if edge_key(pos, d) in KELP_EDGES:
                result = EdgeType.KELP
            elif AGGRESSIVE:
                result = EdgeType.EMPTY
            else:
                result = EdgeType.KELP
        else:
            result = edges.get(d, EdgeType.EMPTY)

    _EDGE_CACHE[key] = result
    return result


def known_occupied(pos):
    """Whether pos currently holds a dragon. Live vision is ground truth in
    view; out of view only our own body (reconstructed exactly) counts."""
    key = (pos.x, pos.y)
    result = _OCCUPIED_CACHE.get(key)
    if result is not None:
        return result

    tile = ct.get_tile(pos)
    if tile is not None:
        result = tile.get_dragon() is not None
    else:
        result = key in OWN_BODY_SET

    _OCCUPIED_CACHE[key] = result
    return result


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
# A kelp "edge" is always stored as the NORTH edge (0) or WEST edge (1) of a
# tile - see edge_key().

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
    the north edge of tile (x, y), (x, y, 1) is its west edge."""
    if d == Direction.SOUTH or d == Direction.EAST:
        pos = pos.add_dir(d)
        d = d.get_opposite()
    return (pos.x, pos.y, 0 if d == Direction.NORTH else 1)


def add_kelp(key):
    if key not in KELP_EDGES:
        KELP_EDGES.add(key)
        KELP_LIST.append(key)


def next_kelp_chunk():
    """The next KELP_PER_MSG kelp edges to send, cycling through everything we know."""
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
    """Call once per turn, after our move. Sends one report out of each side,
    each carrying a different slice of our kelp map."""
    if not SONAR3:
        return
    for d in DIRS:
        ct.send_sonar(d, encode_report(next_kelp_chunk()))


def read_sonar(round_num):
    """Parse everything that reached us since last turn."""
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

def astar_targets(here, fatal_zone, target_positions, cap=BFS_CAP, deadline=None, avoid_fatal=True):
    """A* from `here` over the known passable-edge graph (edge must be known
    EMPTY, landing tile not occupied, not fatal), guided by Manhattan
    distance to the nearest unsettled target. Returns dist_to:
    {pos: shortest known step count from `here`}."""
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

        for d in DIRS:
            if known_edge_type(pos, d) != EdgeType.EMPTY:
                continue
            nxt = pos.add_dir(d)
            if known_occupied(nxt):
                continue
            if avoid_fatal and is_fatal(nxt, fatal_zone):
                continue
            new_g = g + 1
            if nxt not in dist_to or new_g < dist_to[nxt]:
                dist_to[nxt] = new_g
                counter += 1
                heapq.heappush(heap, (new_g + h(nxt), new_g, counter, nxt))

    return dist_to


def pearl_dist_astar(pos, targets, fatal_zone, deadline=None, avoid_fatal=True, target_positions=None):
    """Real shortest-path turns-until-collectable for the best target, run
    fresh from `pos`. Falls back to Manhattan only for a target A* couldn't
    reach."""
    if not targets:
        return 0
    if target_positions is None:
        target_positions = [p for p, _ in targets]
    dist_to = astar_targets(pos, fatal_zone, target_positions, deadline=deadline, avoid_fatal=avoid_fatal)
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
# Stepping onto another dragon's head kills both of you, so a kamikaze run only
# ever targets a visible enemy HEAD. We only trade if the enemy loses at least
# as much as we do. Caveat: dragons likely don't move simultaneously, so the
# head position we see may not be final - check this first if kills don't land.

KAMIKAZE_MIN_LENGTH_RATIO = 1.1   # only trade if the enemy's VISIBLE length is at
                                  # least this multiple of ours.


def nearest_enemy_head(others):
    """Closest visible enemy head, or None."""
    here = ct.get_position()
    my_team = ct.get_team()
    heads = [p for p in others if p.is_head() and p.get_team() != my_team]
    if not heads:
        return None
    return min(heads, key=lambda p: dist(here, p.get_position()))


def visible_enemy_length(target, others):
    """Visible segment count of target's dragon - a lower bound on its length."""
    return sum(1 for p in others if p.get_id() == target.get_id())


def trade_is_favorable(target, others):
    our_length = ct.get_length()
    their_visible = visible_enemy_length(target, others)
    return their_visible >= our_length * KAMIKAZE_MIN_LENGTH_RATIO


def path_onto(here, target_pos, cap=49, deadline=None):
    """A* from here to target_pos, allowing the (occupied) target as the
    final step only. Returns the ordered list of Directions, or None."""
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

        for d in DIRS:
            if known_edge_type(pos, d) != EdgeType.EMPTY:
                continue
            nxt = pos.add_dir(d)
            if nxt != target_pos and known_occupied(nxt):
                continue
            new_g = g + 1
            if nxt not in dist_to or new_g < dist_to[nxt]:
                dist_to[nxt] = new_g
                came_from[nxt] = (pos, d)
                counter += 1
                heapq.heappush(heap, (new_g + h(nxt), new_g, counter, nxt))

    return None


def kamikaze_charge(others, deadline=None):
    """Aggressive-only: if a visible enemy head can be reached in one sprint
    and the trade looks favorable, take it. Returns True if it sprinted."""
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


# ---------- kamikaze dodge: safe dragons flee a shorter enemy that could trade with us ----------

KAMIKAZE_DODGE_CLOSE_RANGE = 2     # a shorter enemy head this close could reach and hit us
KAMIKAZE_DODGE_SPRINT_RANGE = 3    # further out, but worth reacting to if heading straight for us


def enemy_can_reach(enemy_pos, here, max_steps):
    """Whether a terrain-only route (dragons ignored) connects enemy_pos to here."""
    reachable = terrain_reachable(enemy_pos, cap=16, targets={(here.x, here.y)})
    return (here.x, here.y) in reachable


def kamikaze_threat(others):
    here = ct.get_position()
    my_team = ct.get_team()
    our_length = ct.get_length()
    best = None
    best_dist = None
    for part in others:
        if not part.is_head() or part.get_team() == my_team:
            continue
        if visible_enemy_length(part, others) >= our_length:
            continue
        d = dist(here, part.get_position())
        if d <= KAMIKAZE_DODGE_CLOSE_RANGE:
            threatening = True
        elif d <= KAMIKAZE_DODGE_SPRINT_RANGE:
            nxt = part.get_position().add_dir(part.get_dir())
            threatening = dist(here, nxt) < d
        else:
            threatening = False

        if threatening and not enemy_can_reach(part.get_position(), here, KAMIKAZE_DODGE_SPRINT_RANGE + 1):
            threatening = False   # raw distance looked close, but no actual route exists

        if threatening and (best_dist is None or d < best_dist):
            best, best_dist = part, d
    return best


def kamikaze_dodge_split(others):
    """Safe dragons only: if a shorter enemy head is close or closing in,
    shed a chunk of tail - less to lose if it still lands the trade.
    Fleeing itself is handled by choose_direction's scoring."""
    if AGGRESSIVE:
        return False
    threat = kamikaze_threat(others)
    if threat is None:
        return False

    child = max(2, ct.get_length() - 2)
    if ct.can_split(child):
        ct.do_split(child)
        return True
    return False


# ---------- decision ----------

def cheap_fallback_direction(moves, others, targets):
    """Used only when we're already out of time this turn: skip A*, but still
    reach for a pearl first using the cheap Manhattan estimate. avoid_penalty
    and region_bias sit AFTER danger, matching the safe-style key() below."""
    return min(moves, key=lambda d: (pearl_dist(moves[d], targets),
                                     danger(moves[d], others),
                                     avoid_penalty(moves[d]),
                                     region_bias(moves[d]),
                                     random.random()))


def choose_direction(others, round_num, tiles, deadline=None):
    here = ct.get_position()
    targets = gather_pearl_targets(here, tiles)

    # If a dense cluster of pearls is nearby, chase that instead of whatever
    # lone pearl happens to be a tile or two closer.
    cluster = find_pearl_cluster(targets)
    if cluster:
        targets = cluster

    target_positions = [p for p, _ in targets] if targets else None

    if AGGRESSIVE and not targets:
        portals = portal_directions(here, round_num)
        if portals and random.random() < PORTAL_CHANCE:
            d, pid = random.choice(portals)
            remember_portal_crossing(pid, round_num)
            return d

    # tier 1: drop anything blocked
    moves = {}
    for d in DIRS:
        nxt = open_step(here, d)
        if nxt is not None:
            moves[d] = nxt
    if not moves:
        return None

    # hard veto: drop anything that ends next to an enemy head. Safe dragons
    # refuse this outright; aggressive dragons accept the risk.
    fatal_zone = None
    if not AGGRESSIVE:
        fatal_zone = build_fatal_zone(others)
        safe_from_death = {d: p for d, p in moves.items() if not is_fatal(p, fatal_zone)}
        if safe_from_death:
            moves = safe_from_death

    # tier 2: drop moves into pockets too small for us
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

    # already out of time - take a legal, pearl-leaning move and get out
    if deadline is not None and time.perf_counter() >= deadline:
        return cheap_fallback_direction(moves, others, targets)

    if AGGRESSIVE:
        # skip danger and the enemy-head veto; go straight for the nearest
        # pearl. avoid_penalty / ally_crowding / region_bias only matter once
        # the pearl distance is tied - a nudge, not a hard rule.
        def key(d):
            p = moves[d]
            return (pearl_dist_astar(p, targets, None, deadline=deadline, avoid_fatal=False,
                                     target_positions=target_positions), ally_crowding(p, others),
                    avoid_penalty(p),
                    region_bias(p),
                    d != facing, random.random())
    else:
        # least danger first, then closest pearl by real A* distance, then the
        # soft wander nudges (dead-ends/barren, then least-visited region).
        def key(d):
            p = moves[d]
            return (danger(p, others),
                    pearl_dist_astar(p, targets, fatal_zone, deadline=deadline, avoid_fatal=True,
                                     target_positions=target_positions),
                    avoid_penalty(p),
                    region_bias(p),
                    d != facing, random.random())

    return min(moves, key=key)


def try_split(others):
    """Aggressive dragons split as soon as they're long enough. Safe dragons
    never split (except the dodge and desperate splits)."""
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
        if (length // 2) % 2 != 0:
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
    """Split off as much of the tail as legally possible when fully boxed in."""
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
    """Every direction is blocked. Try a portal escape, then a desperate
    split, then feeding a teammate's head. Returns a Direction to move, or
    None if the turn was already used."""
    here = ct.get_position()
    portals = portal_directions(here, round_num)
    if portals:
        d, pid = portals[0]
        remember_portal_crossing(pid, round_num)
        return d

    if desperate_split():
        return None   # split used the turn; nothing left to move

    for d in DIRS:
        nxt = here.add_dir(d)
        tile = ct.get_tile(nxt)

        if tile is not None:
            dragon = tile.get_dragon()

            if dragon is not None:
                if dragon.get_team() == ct.get_team() and dragon.is_head():
                    suicide()
                    return None
    return ct.get_dir()   # truly nothing left to try


def execute_turn() -> None:
    round_num = game.get_round_num()
    deadline = time.perf_counter() + TURN_TIME_BUDGET
    reset_turn_caches()
    tiles = ct.get_tiles()   # fetched once and shared
    others = other_dragon_parts(tiles)
    update_own_body()
    remember_region_visit()   # must run every turn, before any early return below
    remember_vision(tiles)
    read_sonar(round_num)

    if try_split(others):
        return

    if kamikaze_charge(others, deadline=deadline):
        return

    if kamikaze_dodge_split(others):
        return

    if round_num > 400 and not enemy_nearby(others) and round_num < 490:  # activate feeding
        if seppuku_conditions(others):
            suicide()
            return

    d = choose_direction(others, round_num, tiles, deadline=deadline)

    if d is None:
        d = last_resort(round_num)
        if d is None:
            return   # last_resort already split; no move to send
    ct.make_move(d)


def suicide():  # literally just makes the bot kill itself
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

    if ct.get_length() < max_visible_length:
        return ally_found

    if ct.get_length() < max_avg * 0.8:
        return ally_found
    return False


# ---------- boilerplate ----------

def main() -> None:
    global ct, game, AGGRESSIVE, SONAR3
    ct, game = unswbc.init()
    SONAR3 = hasattr(ct, "get_sonar_echoes")   # only the protocol 3 helper has this
    rolled = False
    w, h = game.get_map_size()
    global WIDTH, HEIGHT
    WIDTH = w
    HEIGHT = h

    while unswbc.update(ct, game):
        round_num = game.get_round_num()

        if rolled == False:
            progress = (round_num - 1) / 500
            AGGRO_CHANCE = max(1 - 0.5 * progress, 0.3)
            AGGRESSIVE = stable_roll(ct.get_id()) < AGGRO_CHANCE
            rolled = True
            if round_num < 250:
                AGGRESSIVE = True

        if ct.get_length() > 5 and round_num > 1:    # keep desperate-split children from becoming aggressive
            AGGRESSIVE = False

        execute_turn()
        send_sonar_report()   # sonars fire after our action, so send after the move
        unswbc.end_turn()


if __name__ == "__main__":
    main()