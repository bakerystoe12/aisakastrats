import state
import search
import helper as unswbc
import memory
import random
import time

# helpers

def suicide(): # literally just makes the bot kills itself
    opposite = state.ct.get_dir().get_opposite()
    state.ct.make_move(opposite)
    return 

def enemy_nearby(others):
    my_team = state.ct.get_team()
    for drag in others:
        if drag.get_team() != my_team:
            return True
    return False

def danger(pos, others):
    """How risky it is to end a step on pos. Heads matter most, since a head-on
    hit kills both dragons and enemies may sprint several tiles."""
    my_team = state.ct.get_team()
    total = 0.0
    for part in others:
        d = search.dist(pos, part.get_position())
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
    my_team = state.ct.get_team()
    total = 0.0
    for part in others:
        if part.get_team() != my_team:
            continue
        d = search.dist(pos, part.get_position())
        if d <= 1:
            total += 2
        elif d == 2:
            total += 1
    return total

# ------------------------------------- conditions --------------------------------------

def has_room_to_split():
    here = state.ct.get_position()
    needed = state.ct.get_length() * state.SPLIT_ROOM_MULTIPLIER
    return search.split_room(here, here, needed) >= needed

def nearby_ally_count(others, radius=3):
    """How many distinct ally dragons (besides us) have any visible segment
    within `radius` tiles of our head. Counts dragons, not segments, so one
    long ally snake doesn't get double-weighted against a short one."""
    here = state.ct.get_position()
    my_team = state.ct.get_team()
    seen_ids = set()
    for part in others:
        if part.get_team() != my_team:
            continue
        if part.get_id() in seen_ids:
            continue
        if search.dist(here, part.get_position()) <= radius:
            seen_ids.add(part.get_id())
    return len(seen_ids)


def desperate_split():
    """Split off as much of the tail as legally possible, ignoring the normal
    AGGRESSIVE/SPLIT_MIN_LENGTH gate - this only runs when we're fully boxed
    in with no legal move, so saving even part of the dragon as a separate
    unit beats losing all of it to whatever's about to happen to the head."""
    length = state.ct.get_length()
    min_size = unswbc.Constants.MIN_SIZE
    if length < 2 * min_size:
        return False   # too short to split anything off at all
    child = max(min_size, length - min_size)
    if state.ct.can_split(child):
        state.ct.do_split(child)
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
    here = state.ct.get_position()
    portals = search.portal_directions(here, round_num)
    if portals:
        d, pid = portals[0]
        memory.remember_portal_crossing(pid, round_num)
        return d

    if desperate_split():
        return None   # split used the turn; nothing left to move
    
    for d in state.DIRS:
        nxt = here.add_dir(d)
        tile = state.ct.get_tile(nxt)
    
        if tile is not None:
            dragon = tile.get_dragon()
        
            if dragon is not None:
                if dragon.get_team() == state.ct.get_team() and dragon.is_head():
                    suicide()
                    return None
    return state.ct.get_dir()   # truly nothing left to try

def try_split(others):
    """Aggressive dragons split as soon as they're long enough. Safe dragons
    never split, to keep their body intact for evasive pathfinding."""
    if not state.AGGRESSIVE:
        return False
    if state.ct.get_length() < state.SPLIT_MIN_LENGTH:
        return False
    if nearby_ally_count(others) > state.MAX_NEARBY_ALLIES_TO_SPLIT:
        return False
    if not has_room_to_split():
        return False
    length = state.ct.get_length()
    if length % 2 == 0:
        if (length // 2)  % 2 != 0:
            child = 2
        else:
            child = length // 2
    else:
        child = 2
    if state.ct.can_split(child):
        state.ct.do_split(child)
        return True
    return False

#  ---------------------- fallback ---------------------------------------

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
    return min(moves, key=lambda d: (search.pearl_dist(moves[d], targets),
                                       danger(moves[d], others),
                                       random.random()))

# ------------------------------------------  Main decision making branch ------------------------------------

def choose_direction(others, round_num, tiles, deadline=None):
    here = state.ct.get_position()
    targets = search.gather_pearl_targets(here, tiles)

    # If a dense cluster of pearls is sitting nearby, chase that instead of
    # whatever lone pearl happens to be a tile or two closer - a cluster is
    # worth more total length, so it should win the tie even against a
    # slightly nearer single spawn.
    cluster = search.find_pearl_cluster(targets)
    if cluster:
        targets = cluster

    # Derived once and reused across every candidate direction below, rather
    # than re-derived from `targets` on each of the up to 4 pearl_dist_astar
    # calls this turn.
    target_positions = [p for p, _ in targets] if targets else None

    if state.AGGRESSIVE and not targets:
        portals = search.portal_directions(here, round_num)
        if portals and random.random() < state.PORTAL_CHANCE:
            d, pid = random.choice(portals)
            memory.remember_portal_crossing(pid, round_num)
            return d


    # tier 1: drop anything blocked
    moves = {}
    for d in state.DIRS:
        nxt = search.open_step(here, d)
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
    if not state.AGGRESSIVE:
        fatal_zone = search.build_fatal_zone(others)
        safe_from_death = {d: p for d, p in moves.items() if not search.is_fatal(p, fatal_zone)}
        if safe_from_death:
            moves = safe_from_death

    # tier 2: drop moves into pockets too small for us - applies to both
    # styles, since walking into a dead end is never useful, only risky
    
    if state.AGGRESSIVE:
        needed = state.ct.get_length()
    else:
        needed = state.ct.get_length() + 1
    space = {d: search.room(p, here, needed, deadline=deadline) for d, p in moves.items()}
    roomy = {d: p for d, p in moves.items() if space[d] >= needed}
    if roomy:
        moves = roomy
    else:
        best = max(space.values())
        moves = {d: p for d, p in moves.items() if space[d] == best}

    facing = state.ct.get_dir()

    # already out of time - don't start A* scoring at all, just take a
    # legal, pearl-leaning move and get out
    if deadline is not None and time.perf_counter() >= deadline:
        return cheap_fallback_direction(moves, others, targets)
    
    if state.AGGRESSIVE:
        # skip danger, and skip the enemy-head veto too (avoid_fatal=False,
        # fatal_zone unused) so routing doesn't silently detour around
        # contested ground either - go straight for the nearest pearl among
        # whatever's left, using the real A* shortest-path distance,
        # breaking ties toward whichever option gives our own teammates
        # more room.
        def key(d):
            p = moves[d]
            return (search.pearl_dist_astar(p, targets, None, deadline=deadline, avoid_fatal=False,
                                       target_positions=target_positions),
                     ally_crowding(p, others), d != facing, random.random())
    else:
        # tiers 3 and 4: least danger first (danger already down-weights
        # enemy-head proximity gradually), then closest to a pearl by real
        # A* shortest-path distance, still avoiding fatal tiles in routing
        def key(d):
            p = moves[d]
            return (danger(p, others),
                     search.pearl_dist_astar(p, targets, fatal_zone, deadline=deadline, avoid_fatal=True,
                                        target_positions=target_positions),
                     d != facing, random.random())

    return min(moves, key=key)

def seppuku_conditions(others):
    max_length = 0
    max_visible_length = 0
    if state.TEAMMATES:
        max_length = max(t["length"] for t in state.TEAMMATES.values())
    ally_found = False
    my_team = state.ct.get_team()
    for drag in others:
        if drag.get_team() == my_team and drag.is_head():
            ally_found = True

            masked_id = drag.get_id() & memory.ID_MASK

            if masked_id in state.TEAMMATES:
                ally_length = state.TEAMMATES[masked_id]["length"]
                if ally_length > max_visible_length:
                    max_visible_length = ally_length
    max_avg = (max_length + max_visible_length + max_visible_length) / 3

    if state.ct.get_length() < max_visible_length:
        return ally_found

    if state.ct.get_length() < max_avg * 0.8:
        return ally_found
    return False