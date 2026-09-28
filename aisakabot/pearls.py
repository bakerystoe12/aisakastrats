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