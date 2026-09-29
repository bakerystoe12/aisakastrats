import state
from helper import Direction, EdgeType


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
    if key not in state.KELP_EDGES:
        state.KELP_EDGES.add(key)
        state.KELP_LIST.append(key)


def next_kelp_chunk():
    """The next KELP_PER_MSG kelp edges to send, cycling through everything we
    know. Most sonars never reach a teammate, so we keep re-sending in a loop."""
    chunk = []
    for _ in range(min(KELP_PER_MSG, len(state.KELP_LIST))):
        chunk.append(state.KELP_LIST[state.KELP_SEND_CURSOR % len(state.KELP_LIST)])
        state.KELP_SEND_CURSOR += 1
    return chunk


def encode_report(kelp_chunk):
    slots = [pack(list(zip(k, EDGE_LAYOUT))) for k in kelp_chunk]
    slots += [0] * (KELP_PER_MSG - len(slots))
    return pack(list(zip(
        [SONAR_MAGIC, int(state.AGGRESSIVE), min(state.ct.get_length(), 127),
         state.ct.get_id() & ID_MASK, len(kelp_chunk), slots[0], slots[1]],
        REPORT_LAYOUT)))


def send_sonar_report():
    """Call once per turn, after our move. Sends one report out of each side
    (a sonar stops at the first thing it hits, so more directions = more chances
    of reaching a teammate), each carrying a different slice of our kelp map."""
    if not state.SONAR3:
        return
    for d in state.DIRS:
        state.ct.send_sonar(d, encode_report(next_kelp_chunk()))


def read_sonar(round_num):
    """Parse everything that reached us since last turn. Call at the start of
    the turn so any new kelp is already in the map before we plan a move."""
    width, height = state.WIDTH, state.HEIGHT
    for msg in state.ct.get_sonar_messages():
        magic, aggressive, length, sender, count, *slots = unpack(msg, REPORT_LAYOUT)
        if magic != SONAR_MAGIC:
            continue                        # not one of ours
        if sender == state.ct.get_id() & ID_MASK:
            continue                        # our own sonar bounced back to us
        state.TEAMMATES[sender] = {"aggressive": bool(aggressive), "length": length, "round": round_num}
        for slot in slots[:count]:
            x, y, horizontal = unpack(slot, EDGE_LAYOUT)
            if x < width and y < height:    # ignore coordinates that can't be real
                add_kelp((x, y, horizontal))


# ---------- memory: what this dragon has personally seen over its life ----------
#
# Vision only ever shows the 7x7 window around our head, so BFS on its own
# can't route more than 3 tiles out. Terrain is worth remembering past that
# boundary, purely from this one dragon's own vision history - no sonar,
# nothing shared with any other dragon: kelp and walls never move once
# placed, so any edge we've ever seen can be trusted forever, long after it
# scrolls out of view.

def remember_vision(tiles):
    """Fold everything currently visible into memory. Call once per turn,
    before making any decisions, so this turn's own tiles are included.
 
    Terrain (edges, kelp, portals) is static once seen, so a tile already
    present in KNOWN_EDGES has nothing left to learn on the terrain side -
    re-fetching and re-storing all 4 edges, and re-registering kelp/portals
    that are already known, was pure waste every single turn for all 49
    visible tiles regardless of whether any of it could possibly have
    changed. After the first turn, only genuinely new tiles at the edge of
    vision (as the window shifts with our head) need that work at all.
 
    Pearl state is the opposite: it's the one thing on a tile that's
    NOT static (spawn countdown ticks, has_pearl toggles), so
    remember_pearl runs unconditionally on every visible tile every turn,
    outside the KNOWN_EDGES skip - freezing it at whatever it happened to
    be the first time we ever saw that tile would silently break every
    pearl-timing decision downstream."""
    now = state.game.get_round_num()
    for tile in tiles:
        pos = tile.get_position()
        key = (pos.x, pos.y)
 
        if key not in state.KNOWN_EDGES:
            edges = {}
            for d in state.DIRS:
                edge = tile.get_edge(d)
                edge_type = edge.get_edge_type()
                edges[d] = edge_type
                if edge_type == EdgeType.KELP:
                    add_kelp(edge_key(pos, d))   # sonar: remember it so we can tell teammates
                elif edge_type == EdgeType.PORTAL:
                    remember_portal(edge_key(pos, d), edge.get_portal_id())
            state.KNOWN_EDGES[key] = edges
 
        remember_pearl(key, tile, now)


# ---------- pearl memory ----------

def remember_pearl(key, tile, round_num):
    """Record what this tile told us about pearls. A tile that reports
    pearl_time == -1 can never spawn a pearl; that is static, so once it is
    stored as -1 there is nothing more to learn from it and we skip it."""
    pearl_time = tile.get_pearl_time()
    old = state.PEARL_MEMORY.get(key)
    if pearl_time == -1 and old is not None and old["pearl_time"] == -1:
        return
    state.PEARL_MEMORY[key] = {
        "pearl_time": pearl_time,        # -1 = can never spawn a pearl
        "has_pearl": tile.has_pearl(),
        "round": round_num,
    }


# ---------- portal memory ----------

def remember_portal(ekey, portal_id):
    """Store one portal edge. edge_key gives an edge a single name no matter
    which of its two tiles saw it, so the same edge is only ever added once."""
    if ekey in state.PORTAL_EDGES:
        return
    state.PORTAL_EDGES[ekey] = portal_id
    state.PORTALS.setdefault(portal_id, []).append(ekey)


def known_edge_type(pos, d):
    """The edge type stepping d from pos: read live if pos is in view right
    now, else whatever we remembered from the last time we saw pos. None
    means we've never seen this edge at all, so - same as an unseen tile
    does for a single step - it stays untraversable.

    Memoized per turn (see _EDGE_CACHE) since the same (pos, d) gets asked
    about repeatedly across the several overlapping searches run each turn."""
    key = (pos.x, pos.y, d)
    result = state._EDGE_CACHE.get(key)
    if result is not None:
        return result

    tile = state.ct.get_tile(pos)
    if tile is not None:
        result = tile.get_edge(d).get_edge_type()
    else:
        edges = state.KNOWN_EDGES.get((pos.x, pos.y))
        if edges is None:
            if edge_key(pos, d) in state.KELP_EDGES:
                result = EdgeType.KELP        # never seen it ourselves, but a teammate's sonar told us
            elif state.AGGRESSIVE:
                result = EdgeType.EMPTY
            else:
                result = EdgeType.KELP        # unseen & safe -> untraversable, not a crash
        else:
            result = edges.get(d, EdgeType.EMPTY)

    state._EDGE_CACHE[key] = result
    return result


def known_occupied(pos):
    """Whether pos currently holds a dragon. Live vision is ground truth for
    any dragon, ours or an enemy's, when pos is in view. Out of view, an
    enemy's last-seen position is only a guess (it may well have moved), so
    it's treated as clear - but our own body is the one exception: see
    update_own_body, which reconstructs it exactly with no need for vision,
    so it stays correct even off-screen."""
    key = (pos.x, pos.y)
    result = state._OCCUPIED_CACHE.get(key)
    if result is not None:
        return result

    tile = state.ct.get_tile(pos)
    if tile is not None:
        result = tile.get_dragon() is not None
    else:
        result = key in state.OWN_BODY_SET

    state._OCCUPIED_CACHE[key] = result
    return result

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
    here = (state.ct.get_position().x,state.ct.get_position().y)
    if not state.OWN_BODY or state.OWN_BODY[0] != here:
        state.OWN_BODY.appendleft(here)
    length = state.ct.get_length()
    while len(state.OWN_BODY) > length:
        state.OWN_BODY.pop()
    state.OWN_BODY_SET.clear()
    state.OWN_BODY_SET.update(state.OWN_BODY)


def remember_portal_crossing(pid, round_num):
    state.LAST_PORTAL_ID = pid
    state.LAST_PORTAL_ROUND = round_num