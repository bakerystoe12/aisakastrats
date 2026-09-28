import state



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

SONAR3 = False          # set in main(): True only if helper.py has the protocol 3 sonar API
KELP_EDGES = set()      # every kelp edge we know of (seen ourselves or heard from a teammate)
KELP_LIST = []          # same edges, in a list so we can send them round-robin
KELP_SEND_CURSOR = 0    # where in KELP_LIST the next message starts
TEAMMATES = {}          # dragon id -> {"aggressive": bool, "length": int, "round": int}


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
    if key not in KELP_EDGES:
        KELP_EDGES.add(key)
        KELP_LIST.append(key)


def next_kelp_chunk():
    """The next KELP_PER_MSG kelp edges to send, cycling through everything we
    know. Most sonars never reach a teammate, so we keep re-sending in a loop."""
    global KELP_SEND_CURSOR
    chunk = []
    for _ in range(min(KELP_PER_MSG, len(KELP_LIST))):
        chunk.append(KELP_LIST[KELP_SEND_CURSOR % len(KELP_LIST)])
        KELP_SEND_CURSOR += 1
    return chunk


def encode_report(kelp_chunk):
    slots = [pack(list(zip(k, EDGE_LAYOUT))) for k in kelp_chunk]
    slots += [0] * (KELP_PER_MSG - len(slots))
    return pack(list(zip(
        [SONAR_MAGIC, int(AGGRESSIVE), min(ct.get_length(), 127),
         ct.get_id() & ID_MASK, len(kelp_chunk), slots[0], slots[1]],
        REPORT_LAYOUT)))


def send_sonar_report():
    """Call once per turn, after our move. Sends one report out of each side
    (a sonar stops at the first thing it hits, so more directions = more chances
    of reaching a teammate), each carrying a different slice of our kelp map."""
    if not SONAR3:
        return
    for d in DIRS:
        ct.send_sonar(d, encode_report(next_kelp_chunk()))


def read_sonar(round_num):
    """Parse everything that reached us since last turn. Call at the start of
    the turn so any new kelp is already in the map before we plan a move."""
    width, height = game.get_map_size()
    for msg in ct.get_sonar_messages():
        magic, aggressive, length, sender, count, *slots = unpack(msg, REPORT_LAYOUT)
        if magic != SONAR_MAGIC:
            continue                        # not one of ours
        if sender == ct.get_id() & ID_MASK:
            continue                        # our own sonar bounced back to us
        TEAMMATES[sender] = {"aggressive": bool(aggressive), "length": length, "round": round_num}
        for slot in slots[:count]:
            x, y, horizontal = unpack(slot, EDGE_LAYOUT)
            if x < width and y < height:    # ignore coordinates that can't be real
                add_kelp((x, y, horizontal))