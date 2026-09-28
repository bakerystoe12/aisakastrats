from collections import deque
from helper import Direction

# -------------------- GAME -------------------------

ct = None

game = None

HEIGHT = None

WIDTH = None

SONAR3 = False

# --------------------------- SEARCH CAPS-----------------------  
BFS_CAP = 120              # was 300 - this is the dominant per-turn cost (see
                            # bfs_targets), and pearl_dist_bfs reruns it fresh
                            # for every candidate direction, so this roughly     
                            # linearly scales the most expensive part of a turn.
                            # Pearls this far away rarely win a tie anyway.

TURN_TIME_BUDGET = 0.06   # seconds. Stop chasing BFS precision once this much
                            # of the turn has elapsed and fall back to a cheap
                            # move instead, so we always answer well inside the
                            # engine's per-turn limit rather than risk a
                            # "gave no valid action" timeout death.
BFS_DEADLINE_CHECK_EVERY = 4   # nodes between deadline checks inside a BFS.
                                # time.perf_counter() itself is cheap (~50-100ns),
                                # so this stays a small fraction of normal per-node
                                # cost, but keeps the worst-case overrun small even
                                # if a single node's work is unexpectedly slow
                                # (e.g. host contention) rather than just uniformly
                                # slower - see test_deadline.py for why 32 was too
                                # coarse: it let a slow node blow well past budget
                                # before the next check even happened.


# ---------------------------- MEMORY --------------------------------------
OWN_BODY = deque()      # our own body, head first, tail last - exact, not remembered/stale
OWN_BODY_SET = set()    # same positions, rebuilt each turn, for O(1) lookups

PEARL_CAPABLE = set()   # (x, y) tiles we've ever seen that CAN spawn a pearl
                         # (pearl_time != -1) - remembered forever, same
                         # reasoning as KNOWN_EDGES: capability is static,
                         # only the countdown itself is transient.

KELP_EDGES = set()      # every kelp edge we know of (seen ourselves or heard from a teammate)

KELP_LIST = []          # same edges, in a list so we can send them round-robin

KELP_SEND_CURSOR = 0    # where in KELP_LIST the next message starts

TEAMMATES = {}          # dragon id -> {"aggressive": bool, "length": int, "round": int}

KNOWN_EDGES = {}       # (x, y) -> {Direction: EdgeType}, remembered forever once seen

LAST_PORTAL_ID = -1          # portal_id of the last portal we crossed, or -1 if none yet

LAST_PORTAL_ROUND = -10**9   # round we crossed it, far in the past so the cooldown starts expired
# Per-turn memoization for known_edge_type/known_occupied. Every flood fill
# and A* run (room, terrain_reachable, astar_targets, path_onto) re-asks
# these same questions about the same handful of tiles near our head over
# and over - most obviously across the up to 4 direction-specific searches
# choose_direction fires each turn, which all start from neighbouring tiles
# and overlap heavily. Nothing about vision or occupancy changes mid-turn,
# so it's safe to cache both for the duration of one turn and clear them at
# the top of the next - see reset_turn_caches(), called once from
# execute_turn before anything else runs.

# ---------------------------------------- SPLIT LOGIC -----------------------------------------
MAX_NEARBY_ALLIES_TO_SPLIT = 3    # skip splitting if more than this many other allies are close

PEARL_ROOM_WEIGHT = 0                      # How much a pearl counts as space in split_room()

SPLIT_ROOM_MULTIPLIER = 1.5   # a split needs room for two independent bodies, not
                            # one, so require more open space than plain

                            # movement (tier 2) does before committing to it
PEARL_SPAWN_HORIZON = 5   # ignore spawns further out than this - too likely to
                           # be irrelevant or contested by the time we'd arrive

SPLIT_MIN_LENGTH = 4            # aggressive dragons split once this long

PORTAL_CHANCE = 1             # aggressive dragons: odds of diving into an
                                 # available portal this turn instead of pathing normally




# ---------------------------- TURN VISION ---------------------------------------

DIRS = Direction.get_direction_list()


_EDGE_CACHE = {}
_OCCUPIED_CACHE = {}

def reset_turn_caches():
    _EDGE_CACHE.clear()
    _OCCUPIED_CACHE.clear()


# -------------------------------------- MODE ---------------------------------------------

AGGRESSIVE = True