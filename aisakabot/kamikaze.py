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


def path_onto(here, target_pos, cap=49, deadline=None):
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

        for d in DIRS:
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

# ---------- kamikaze dodge: safe dragons flee a shorter enemy that could trade with us ----------
#
# Stepping onto another dragon's head kills BOTH sides, no matter which one is
# longer (see DragonPart.is_head) - so a shorter enemy loses less than we do by
# trying it, and has every incentive to hunt us down for exactly that trade
# (it's the mirror image of our own kamikaze_charge, above). Safe dragons have
# nothing to gain from that fight, so as soon as a visibly-shorter enemy head
# is close enough to plausibly land the hit, or is lined up and heading
# straight for us, we shed a small chunk of tail (less to lose if it still
# connects) and turn to move away from it - a simple override that takes
# priority over the normal pearl-seeking/danger-scoring move.

KAMIKAZE_DODGE_CLOSE_RANGE = 2     # a shorter enemy head this close could
                                     # plausibly reach and hit us within a turn or two
KAMIKAZE_DODGE_SPRINT_RANGE = 3    # further out, but still worth reacting to if
                                     # it's actually heading straight for us

def enemy_can_reach(enemy_pos, here, max_steps):
    """Whether a real path (through known non-kelp terrain, ignoring
    dragons/danger - same coarse-but-cheap approximation terrain_reachable
    already uses elsewhere) connects enemy_pos to here within max_steps.
    Without this, kamikaze_threat flags a head as threatening purely by
    raw Manhattan distance, even when a kelp wall makes the two of you
    completely unable to reach each other - which was making the dodge
    shed tail (a real, permanent cost) against threats that were never
    actually live."""
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
    """Safe dragons only: if a shorter enemy head is close enough or clearly
    closing in, shed a small chunk of tail every other turn a threat
    persists - less to lose if it still lands the trade. Movement itself is
    NOT handled here anymore: fleeing is folded into choose_direction's own
    scoring (see the threat term in its key()) so it inherits tier 1's fatal
    veto and tier 2's room-based dead-end avoidance for free, instead of
    away_from's un-room-checked movement, which is what walked us into a
    dead end - see the pink-portal incident."""
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

