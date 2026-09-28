import state
import sonar


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
    before making any decisions, so this turn's own tiles are included."""
    for tile in tiles:
        pos = tile.get_position()
        key = (pos.x, pos.y)
        edges = {d: tile.get_edge(d).get_edge_type() for d in DIRS}
        KNOWN_EDGES[key] = edges
        for d, edge_type in edges.items():
            if edge_type == EdgeType.KELP:
                add_kelp(edge_key(pos, d))   # sonar: remember it so we can tell teammates


def known_edge_type(pos, d):
    """The edge type stepping d from pos: read live if pos is in view right
    now, else whatever we remembered from the last time we saw pos. None
    means we've never seen this edge at all, so - same as an unseen tile
    does for a single step - it stays untraversable.

    Memoized per turn (see _EDGE_CACHE) since the same (pos, d) gets asked
    about repeatedly across the several overlapping searches run each turn."""
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
                result = EdgeType.KELP        # never seen it ourselves, but a teammate's sonar told us
            elif AGGRESSIVE:
                result = EdgeType.EMPTY
            else:
                result = EdgeType.KELP        # unseen & safe -> untraversable, not a crash
        else:
            result = edges.get(d, EdgeType.EMPTY)

    _EDGE_CACHE[key] = result
    return result


def known_occupied(pos):
    """Whether pos currently holds a dragon. Live vision is ground truth for
    any dragon, ours or an enemy's, when pos is in view. Out of view, an
    enemy's last-seen position is only a guess (it may well have moved), so
    it's treated as clear - but our own body is the one exception: see
    update_own_body, which reconstructs it exactly with no need for vision,
    so it stays correct even off-screen."""
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


def remember_portal_crossing(pid, round_num):
    global LAST_PORTAL_ID, LAST_PORTAL_ROUND
    LAST_PORTAL_ID = pid
    LAST_PORTAL_ROUND = round_num