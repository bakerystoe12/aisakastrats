import heapq
import state
import memory 
import time
from collections import deque
from helper import EdgeType, Direction

# ---------------------- Helper ---------------------------------------

def dist(a, b):
    """Manhattan distance that accounts for the map wrapping around.
    Used for proximity/threat checks (danger, ally_crowding), and
    as pearl_dist's fallback when BFS can't confirm a real path - see
    bfs_targets and pearl_dist_bfs below for the actual pathfinding."""
    w, h = state.WIDTH, state.HEIGHT
    dx = abs(a.x - b.x)
    dx = min(dx, w - dx)
    dy = abs(a.y - b.y)
    dy = min(dy, h - dy)
    return dx + dy


def tile_ever_seen(pos):
    if state.ct.get_tile(pos) is not None:
        return True
    return (pos.x, pos.y) in state.KNOWN_EDGES


def open_step(pos, d):
    """Landing position if stepping d from pos is legal, else None.
    Blocked by kelp, by a dragon on the tile ahead, or by an unknown tile.
    Portals count as blocked because add_dir gives the wrong tile across one."""
    tile = state.ct.get_tile(pos)
    if tile is None:
        return None
    if tile.get_edge(d).get_edge_type() != EdgeType.EMPTY:
        return None
    nxt = pos.add_dir(d)
    ahead = state.ct.get_tile(nxt)
    if ahead is None or ahead.get_dragon() is not None:
        return None
    return nxt

def portal_directions(pos, round_num):
    """Directions from pos whose edge is a portal, excluding whichever one we
    ourselves crossed most recently while our tail could still be sitting on
    the tile it would send us back to. Returns (direction, portal_id) pairs,
    not bare directions, so the caller can remember which one it picked."""
    tile = state.ct.get_tile(pos)
    if tile is None:
        return []
    cooldown = state.ct.get_length()   # rough time for our tail to fully clear the departure tile
    options = []
    for d in state.DIRS:
        edge = tile.get_edge(d)
        if edge.get_edge_type() != EdgeType.PORTAL:
            continue
        pid = edge.get_portal_id()
        if pid == state.LAST_PORTAL_ID and round_num - state.LAST_PORTAL_ROUND < cooldown:
            continue   # this one goes straight back into our own tail right now
        options.append((d, pid))
    return options

def build_fatal_zone(others):
    """Every tile that would be fatal to land on: the position of a visible
    enemy head, plus its four neighbours (wraparound-aware, via add_dir, so
    it agrees exactly with what dist(pos, head) <= 1 would say). is_fatal
    used to redo an O(#others) distance scan - with map-wrap divisions and
    all - on every single tile it was asked about, and it gets asked about
    constantly: once per candidate move, and once per node A* expands
    whenever avoid_fatal is on. There are only ever a handful of enemy
    heads, so building this small set once per turn and then doing O(1)
    membership checks removes that repeated scan entirely without changing
    which tiles count as fatal."""
    my_team = state.ct.get_team()
    zone = set()
    for part in others:
        if not part.is_head() or part.get_team() == my_team:
            continue
        p = part.get_position()
        zone.add((p.x, p.y))
        for d in state.DIRS:
            n = p.add_dir(d)
            zone.add((n.x, n.y))
    return zone


def is_fatal(pos, fatal_zone):
    """True if pos is within 1 tile (or 0, though that's already blocked) of
    an enemy head - see build_fatal_zone for how that zone is computed.
    Applies to every dragon, aggressive or safe (aggressive dragons just
    choose to accept the risk instead of avoiding it)."""
    return (pos.x, pos.y) in fatal_zone

# ------------------- Room, split_room and terrain_reachable ------------------------------

def room(start, came_from, cap=81, deadline=None):
    """Flood fill from start over free tiles in view. Returns how many tiles are
    reachable, stopping at cap, or early if deadline is hit (returns the
    count reached so far, which tier 2 then treats as an underestimate -
    safe, since an underestimate can only make a spot look tighter than it
    is, never falsely roomy)."""
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
        if deadline is not None and popped % state.BFS_DEADLINE_CHECK_EVERY == 0 and time.perf_counter() >= deadline:
            return count
        for d in state.DIRS:
            if memory.known_edge_type(pos, d) != EdgeType.EMPTY:
                continue
            nxt = pos.add_dir(d)
            if nxt in seen:
                continue
            if not tile_ever_seen(nxt):
                return cap
            if memory.known_occupied(nxt):
                continue
            seen.add(nxt)
            queue.append(nxt)
    return count

def split_room(start, came_from, needed, bfs_cap = 50):
    """Flood fill from start over free tiles in view, same traversal rules as
    room() (kelp/dragons block, unseen tiles beyond the fringe count as
    'plenty of room'), but a tile that currently has a pearl or will spawn
    one within PEARL_SPAWN_HORIZON only contributes PEARL_ROOM_WEIGHT
    instead of a full 1 - kept separate from room() since ordinary movement
    (tier 2) doesn't need this discount, only the split-safety check does.

    Two different limits, not one: `needed` is the room threshold we're
    testing against (weighted total, can stop the instant it's reached),
    while `bfs_cap` bounds how many actual tiles get expanded regardless of
    weight - so a search made of mostly-discounted pearl tiles can't run
    far longer than a plain search just because each tile only adds
    PEARL_ROOM_WEIGHT toward the total instead of 1.

    Only affects tiles currently in live vision; pearl state isn't
    remembered past the 7x7 window the way kelp is (see
    gather_pearl_targets), so anything beyond it still counts as plain
    open space."""
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
        tile = state.ct.get_tile(pos)
        if tile is not None:
            pt = tile.get_pearl_time()
            if tile.has_pearl() or (0 <= pt <= 25):
                weight = state.PEARL_ROOM_WEIGHT
        total += weight

        for d in Direction.get_direction_list():
            if memory.known_edge_type(pos, d) != EdgeType.EMPTY:
                continue
            nxt = pos.add_dir(d)
            if nxt in seen:
                continue
            if not tile_ever_seen(nxt):
                return needed   # unseen fringe = plenty of room, same as room()
            if memory.known_occupied(nxt):
                continue
            seen.add(nxt)
            queue.append(nxt)

    return total

def terrain_reachable(start, cap=81, targets=None):
    start_key = (start.x, start.y)
    seen = {start_key}
    remaining = None
    if targets is not None:
        remaining = set(targets)
        remaining.discard(start_key)
        if not remaining:
            return seen

    def key_dist(a, b):
        w, h = state.WIDTH, state.HEIGHT
        dx = abs(a[0] - b[0]); dx = min(dx, w - dx)
        dy = abs(a[1] - b[1]); dy = min(dy, h - dy)
        return dx + dy

    def heuristic(pos_key):
        return min(key_dist(pos_key, t) for t in remaining) if remaining else 0

    counter = 0
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

        for d in state.DIRS:
            if memory.known_edge_type(pos, d) != EdgeType.EMPTY:
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
    way is still included here, since that dragon will move.

    `tiles` is this turn's already-fetched ct.get_tiles() result, passed in
    so we don't ask the vision layer to re-copy its 49-tile list a second
    time this turn."""
    candidates = []
    for t in tiles:
        if t.has_pearl():
            candidates.append((t.get_position(), 0))
            continue
        pt = t.get_pearl_time()
        if not (0 <= pt <= state.PEARL_SPAWN_HORIZON):
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
        state.PEARL_CAPABLE.add(key)
        targets.append((pos, pt))

    return targets

def pearl_dist(pos, targets):
    """Turns until pos could plausibly collect the best target: whichever is
    later, the walk there or the spawn actually happening. Reduces to plain
    distance for pearls that already exist, since their time is 0."""
    if not targets:
        return 0
    return min(max(dist(pos, p), t) for p, t in targets)


# ---------- pearl clusters: prefer a dense patch over scattered singles ----------

CLUSTER_RADIUS = 2      # pearls within this many tiles (Manhattan) of each
                         # other count as part of the same cluster
CLUSTER_MIN_SIZE = 6    # need at least this many pearls packed that tightly
                         # before we call it "a cluster" - anything looser is
                         # just handled as ordinary individual pearl-seeking


def find_pearl_cluster(targets):
    """Look for a tight group of at least CLUSTER_MIN_SIZE pearls among
    `targets` (all within CLUSTER_RADIUS of one shared pearl). If one exists,
    return just that group so the caller paths toward the dense patch instead
    of splitting attention across every stray pearl on the board. Returns
    None when nothing that dense is around, so normal pearl-seeking (every
    target, closest first) applies as before.

    Deliberately simple: targets is at most a few dozen entries (one 7x7
    vision window's worth), so a plain O(n^2) distance check per turn is
    cheap - no need for real spatial indexing here."""
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

# ---------- A* pathfinding ----------

def astar_targets(here, fatal_zone, target_positions, cap=state.BFS_CAP, deadline=None, avoid_fatal=True):
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

    `fatal_zone` is the precomputed set from build_fatal_zone (see there for
    why this is a set instead of the raw `others` list) - pass None when
    avoid_fatal is False, since it's then never touched.

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
            if popped % state.BFS_DEADLINE_CHECK_EVERY == 0 and time.perf_counter() >= deadline:
                break

        for d in state.DIRS:
            if memory.known_edge_type(pos, d) != EdgeType.EMPTY:
                continue
            nxt = pos.add_dir(d)
            if memory.known_occupied(nxt):
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
    """Real shortest-path turns-until-collectable for the best target, using
    A* run fresh from `pos` (the tile this move would land on), same
    per-direction reasoning as the old pearl_dist_bfs: a single search from
    `here` can't give every direction its own honest distance, so each
    candidate move gets its own run. Falls back to Manhattan-based pearl_dist
    only for a target A* genuinely couldn't reach (outside cap, or through
    unexplored territory).

    `target_positions` lets the caller pass in the bare positions once
    instead of us re-deriving the same list from `targets` on every one of
    the up to 4 calls choose_direction makes per turn - pass None to have it
    derived here as before."""
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





