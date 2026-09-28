
def dist(a, b):
    """Manhattan distance that accounts for the map wrapping around.
    Used for proximity/threat checks (danger, ally_crowding), and
    as pearl_dist's fallback when BFS can't confirm a real path - see
    bfs_targets and pearl_dist_bfs below for the actual pathfinding."""
    w, h = WIDTH, HEIGHT
    dx = abs(a.x - b.x)
    dx = min(dx, w - dx)
    dy = abs(a.y - b.y)
    dy = min(dy, h - dy)
    return dx + dy


def room(start, came_from, cap, deadline=None):
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
        w, h = WIDTH, HEIGHT
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


# ---------- A* pathfinding ----------

def astar_targets(here, fatal_zone, target_positions, cap=BFS_CAP, deadline=None, avoid_fatal=True):
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