"""
Three behaviour modes, picked fresh each turn by choose_mode():

  SAFE   - the original priority list. Never step into kelp/a dragon,
           avoid dead ends, avoid other dragons, then get pearls.
  GREEDY - same hard safety rules (tiers 1 & 2 never change), but pearls
           become the main sort key instead of danger. Used once we're
           big enough and nothing dangerous is nearby.
  ATTACK - same hard safety rules, but we actively close distance on a
           specific enemy head we think we can beat in a trade. Used
           once we're long, have a spare unit if it goes wrong, and a
           weaker enemy is close enough to chase.

Mode selection inputs:
  1. Our length and how long *this* dragon has been alive.
  2. Distance + estimated size of the nearest enemy head(s) in view.
  3. Whether our team has another unit alive (ct.get_unit_count() > 1),
     since a mutual kill is only "worth it" if we're not wiping the team.
  4. Whether any pearls are currently visible at all.

Any close, similarly-sized-or-bigger enemy head overrides everything and
forces SAFE - growth and kills are worthless if we die this turn.
"""
import helper as unswbc
from helper import Direction, EdgeType
import random
from enum import Enum
from collections import deque

ct: unswbc.Controller
game: unswbc.Game

SPLIT_ENABLED = True
SPLIT_MIN_LENGTH = 8      # only split when at least this long (and no enemy head in view)

# --- mode thresholds ---
GREEDY_MIN_LENGTH = 5     # below this we're too fragile to go pearl-hunting aggressively
ATTACK_MIN_LENGTH = 10    # below this we don't chase fights on purpose
SAFE_SURVIVAL_TURNS = 15  # stay cautious for our first N turns regardless of length
LETHAL_HEAD_DIST = 2      # any enemy head this close (and not clearly weaker) forces SAFE
ATTACK_RANGE = 4          # only start a chase if the target is within this many tiles

DEBUG_INDICATOR = False   # set True to label the dragon with its current mode in the replay

turns_survived = 0        # turns this specific dragon (not team) has been alive


class Mode(Enum):
    SAFE = "SAFE"
    GREEDY = "GREEDY"
    ATTACK = "ATTACK"


# ---------- helpers ----------

def dist(a, b):
    """Manhattan distance that accounts for the map wrapping around."""
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


def visible_enemy_sizes(others):
    """Rough lower-bound length per enemy dragon id: however many of its
    segments happen to be inside our 7x7 window right now. Undercounts
    whenever part of the dragon is off-screen, but is the only size signal
    we have, and is safest to use as a "definitely at least this long" hint."""
    counts = {}
    for p in others:
        counts[p.get_id()] = counts.get(p.get_id(), 0) + 1
    return counts


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


def legal_moves():
    """Every direction that survives tier 1, as {direction: landing position}."""
    here = ct.get_position()
    moves = {}
    for d in Direction.get_direction_list():
        nxt = open_step(here, d)
        if nxt is not None:
            moves[d] = nxt
    return moves


# ---------- tier 2: dead ends ----------

def room(start, came_from, cap):
    """Flood fill from start over free tiles in view. Returns how many tiles are
    reachable, stopping at cap. Reaching the edge of our view counts as
    'plenty of room' because we can't see what lies beyond."""
    seen = {came_from, start}       # came_from is our head, which becomes body
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
                continue            # kelp; portals ignored to stay conservative
            nxt = pos.add_dir(d)
            if nxt in seen:
                continue
            ahead = ct.get_tile(nxt)
            if ahead is None:
                return cap          # open tile beyond our view: not a dead end
            if ahead.get_dragon() is not None:
                continue
            seen.add(nxt)
            queue.append(nxt)
    return count


def drop_dead_ends(moves):
    """Tier 2, shared by every mode: never willingly walk into a pocket too
    small for our length. If every option is a dead end, take the roomiest."""
    here = ct.get_position()
    needed = ct.get_length()
    space = {d: room(p, here, needed) for d, p in moves.items()}
    roomy = {d: p for d, p in moves.items() if space[d] >= needed}
    if roomy:
        return roomy
    best = max(space.values())
    return {d: p for d, p in moves.items() if space[d] == best}


# ---------- tier 3: dragons ----------

def danger(pos, others):
    """How risky it is to end a step on pos. Heads matter most, since a head-on
    hit kills both dragons and enemies may sprint several tiles."""
    my_team = ct.get_team()
    total = 0.0
    for part in others:
        d = dist(pos, part.get_position())
        weight = 1.0 if part.get_team() != my_team else 0.5
        if part.is_head():
            if d <= 1:
                total += 10 * weight
            elif d == 2:
                total += 4 * weight
            elif d == 3:
                total += 1 * weight
        elif d <= 1:
            total += 1 * weight
    return total


# ---------- tier 4: pearls ----------

def pearl_dist(pos, pearls):
    """Distance from pos to the nearest visible pearl (0 if none are visible)."""
    if not pearls:
        return 0
    return min(dist(pos, p) for p in pearls)


# ---------- targeting (for ATTACK mode) ----------

def choose_target(others):
    """Nearest enemy head we think we can beat: strictly shorter (by our
    visible-segment estimate) than us. None if no such head is around."""
    my_pos = ct.get_position()
    my_len = ct.get_length()
    my_team = ct.get_team()
    sizes = visible_enemy_sizes(others)

    candidates = [
        p for p in others
        if p.is_head() and p.get_team() != my_team
        and sizes.get(p.get_id(), 0) + 1 < my_len   # +1 for the head itself
    ]
    if not candidates:
        return None
    return min(candidates, key=lambda p: dist(my_pos, p.get_position()))


# ---------- mode selection ----------

def choose_mode(others, pearls):
    my_pos = ct.get_position()
    my_len = ct.get_length()
    my_team = ct.get_team()
    sizes = visible_enemy_sizes(others)

    enemy_heads = [p for p in others if p.is_head() and p.get_team() != my_team]

    # overrides everything: a close head that's not clearly smaller than us
    for head in enemy_heads:
        if dist(my_pos, head.get_position()) <= LETHAL_HEAD_DIST \
                and sizes.get(head.get_id(), 0) + 1 >= my_len:
            return Mode.SAFE

    # still growing up: play it safe regardless of what's around
    if my_len < GREEDY_MIN_LENGTH or turns_survived < SAFE_SURVIVAL_TURNS:
        return Mode.SAFE

    # big enough, and our team can afford to lose this dragon in a trade:
    # look for a fight we'd win
    if my_len >= ATTACK_MIN_LENGTH and ct.get_unit_count() > 1:
        for head in enemy_heads:
            enemy_size = sizes.get(head.get_id(), 0) + 1
            if dist(my_pos, head.get_position()) <= ATTACK_RANGE and enemy_size < my_len:
                return Mode.ATTACK

    # nothing dangerous and nothing worth fighting: go get pearls
    if pearls:
        return Mode.GREEDY

    return Mode.SAFE


# ---------- direction choosers, one per mode ----------

def choose_direction_safe(others, pearls):
    moves = legal_moves()
    if not moves:
        return None
    moves = drop_dead_ends(moves)
    facing = ct.get_dir()

    def key(d):
        p = moves[d]
        return (danger(p, others),
                pearl_dist(p, pearls),
                d != facing,            # nothing else to go on: keep straight
                random.random())        # random tie-break

    return min(moves, key=key)


def choose_direction_greedy(others, pearls):
    """Same hard safety tiers as SAFE, but pearls lead the ranking and danger
    is only a tie-breaker - so we'll cut it closer to a body segment (never
    a dead end, never a real head threat) to reach a pearl faster."""
    moves = legal_moves()
    if not moves:
        return None
    moves = drop_dead_ends(moves)
    facing = ct.get_dir()

    def key(d):
        p = moves[d]
        return (pearl_dist(p, pearls),
                danger(p, others),
                d != facing,
                random.random())

    return min(moves, key=key)


def choose_direction_attack(others, pearls):
    """Same hard safety tiers as SAFE. Picks a target once for the turn, then
    ranks moves by distance closed on it; danger from every *other* dragon
    still applies so we don't get sandwiched while chasing."""
    moves = legal_moves()
    if not moves:
        return None
    moves = drop_dead_ends(moves)

    target = choose_target(others)
    facing = ct.get_dir()
    others_minus_target = others if target is None else [
        o for o in others if o.get_id() != target.get_id()
    ]

    def key(d):
        p = moves[d]
        chase = dist(p, target.get_position()) if target is not None else pearl_dist(p, pearls)
        return (danger(p, others_minus_target),
                chase,
                d != facing,
                random.random())

    return min(moves, key=key)


# ---------- splitting ----------

def try_split(others, mode):
    """Split when long enough, no enemy head is in view, and we're not mid-chase
    (ATTACK wants to stay at full length for the fight)."""
    if not SPLIT_ENABLED or mode == Mode.ATTACK:
        return False
    if ct.get_length() < SPLIT_MIN_LENGTH:
        return False
    my_team = ct.get_team()
    if any(p.is_head() and p.get_team() != my_team for p in others):
        return False
    child = ct.get_length() // 2
    if ct.can_split(child):
        ct.do_split(child)
        return True
    return False


def last_resort():
    """Every direction is blocked: try a portal, else just keep facing forward."""
    here_tile = ct.get_tile(ct.get_position())
    for d in Direction.get_direction_list():
        if here_tile.get_edge(d).get_edge_type() == EdgeType.PORTAL:
            return d
    return ct.get_dir()


def execute_turn() -> None:
    global turns_survived
    turns_survived += 1

    others = other_dragon_parts()
    pearls = [t.get_position() for t in ct.get_tiles() if t.has_pearl()]

    mode = choose_mode(others, pearls)
    if DEBUG_INDICATOR:
        ct.set_indicator_string(mode.value)

    if try_split(others, mode):
        return

    if mode == Mode.GREEDY:
        d = choose_direction_greedy(others, pearls)
    elif mode == Mode.ATTACK:
        d = choose_direction_attack(others, pearls)
    else:
        d = choose_direction_safe(others, pearls)

    if d is None:
        d = last_resort()
    ct.make_move(d)


# ---------- boilerplate ----------

def main() -> None:
    global ct, game
    ct, game = unswbc.init()

    while unswbc.update(ct, game):
        execute_turn()
        unswbc.end_turn()


if __name__ == "__main__":
    main()