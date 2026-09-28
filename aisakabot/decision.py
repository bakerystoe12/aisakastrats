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
    for d in DIRS:
        edge = tile.get_edge(d)
        if edge.get_edge_type() != EdgeType.PORTAL:
            continue
        pid = edge.get_portal_id()
        if pid == LAST_PORTAL_ID and round_num - LAST_PORTAL_ROUND < cooldown:
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
    """True if pos is within 1 tile (or 0, though that's already blocked) of
    an enemy head - see build_fatal_zone for how that zone is computed.
    Applies to every dragon, aggressive or safe (aggressive dragons just
    choose to accept the risk instead of avoiding it)."""
    return (pos.x, pos.y) in fatal_zone



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


def choose_direction(others, round_num, tiles, deadline=None):
    here = ct.get_position()
    targets = gather_pearl_targets(here, tiles)

    # If a dense cluster of pearls is sitting nearby, chase that instead of
    # whatever lone pearl happens to be a tile or two closer - a cluster is
    # worth more total length, so it should win the tie even against a
    # slightly nearer single spawn.
    cluster = find_pearl_cluster(targets)
    if cluster:
        targets = cluster

    # Derived once and reused across every candidate direction below, rather
    # than re-derived from `targets` on each of the up to 4 pearl_dist_astar
    # calls this turn.
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
    # still refuse this outright, since a head-on hit kills both and they
    # have nothing to gain from contesting the space. Aggressive dragons
    # accept the risk instead of ceding ground - many maps punish giving up
    # space this way, so they play the gamble rather than back off.
    #
    # fatal_zone is only built (and only needed) for the safe style: it's
    # used again below when scoring safe moves through A*, so it's computed
    # once here and threaded through rather than recomputed per candidate.
    fatal_zone = None
    if not AGGRESSIVE:
        fatal_zone = build_fatal_zone(others)
        safe_from_death = {d: p for d, p in moves.items() if not is_fatal(p, fatal_zone)}
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

    # already out of time - don't start A* scoring at all, just take a
    # legal, pearl-leaning move and get out
    if deadline is not None and time.perf_counter() >= deadline:
        return cheap_fallback_direction(moves, others, targets)
    
    if AGGRESSIVE:
        # skip danger, and skip the enemy-head veto too (avoid_fatal=False,
        # fatal_zone unused) so routing doesn't silently detour around
        # contested ground either - go straight for the nearest pearl among
        # whatever's left, using the real A* shortest-path distance,
        # breaking ties toward whichever option gives our own teammates
        # more room.
        def key(d):
            p = moves[d]
            return (pearl_dist_astar(p, targets, None, deadline=deadline, avoid_fatal=False,
                                       target_positions=target_positions),
                     ally_crowding(p, others), d != facing, random.random())
    else:
        # tiers 3 and 4: least danger first (danger already down-weights
        # enemy-head proximity gradually), then closest to a pearl by real
        # A* shortest-path distance, still avoiding fatal tiles in routing
        def key(d):
            p = moves[d]
            return (danger(p, others),
                     pearl_dist_astar(p, targets, fatal_zone, deadline=deadline, avoid_fatal=True,
                                        target_positions=target_positions),
                     d != facing, random.random())

    return min(moves, key=key)

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

    if ct.get_length() < max_visible_length:
        return ally_found

    if ct.get_length() < max_avg * 0.8:
        return ally_found
    return False

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