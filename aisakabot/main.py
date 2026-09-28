import helper as unswbc
from helper import Direction, EdgeType
import random
import time
from collections import deque
import hashlib
import heapq 

ct: unswbc.Controller
game: unswbc.Game

def stable_roll(key):
    """Deterministic value in [0,1) derived from `key`. Same key always
    gives the same roll - unlike random.random(), this can't be desynced
    by seeding, call order, or how many other random() calls happened
    earlier this turn/process. Use this for any role-assignment decision
    that needs a reliable population-wide ratio."""
    h = hashlib.sha256(str(key).encode()).hexdigest()
    return int(h[:8], 16) / 0xFFFFFFFF

def other_dragon_parts(tiles):
    """Every visible dragon segment that isn't part of this dragon."""
    my_id = ct.get_id()
    parts = []
    for t in tiles:
        part = t.get_dragon()
        if part is not None and part.get_id() != my_id:
            parts.append(part)
    return parts

def execute_turn() -> None:                                                                                                          # EXECUTE
    round_num = game.get_round_num()
    deadline = time.perf_counter() + TURN_TIME_BUDGET
    reset_turn_caches()   # clear the per-turn edge/occupancy memoization
    tiles = ct.get_tiles()   # fetched once and shared, instead of re-copied
                              # by other_dragon_parts, remember_vision and
                              # gather_pearl_targets separately
    others = other_dragon_parts(tiles)
    update_own_body()
    remember_vision(tiles)
    read_sonar(round_num)

    if try_split(others):
        return

    if kamikaze_charge(others, deadline=deadline):
        return

    if kamikaze_dodge_split(others):
        return

    if round_num > 400 and not enemy_nearby(others) and round_num < 490: # activate feeding
        if seppuku_conditions(others):
            suicide()
            return

    d = choose_direction(others, round_num, tiles, deadline=deadline)

    if d is None:
        d = last_resort(round_num)
        if d is None:
            return   # last_resort already split; no move to send
    ct.make_move(d)

# ---------- boilerplate ----------

def main() -> None:
    global ct, game, AGGRESSIVE, SONAR3
    ct, game = unswbc.init()
    SONAR3 = hasattr(ct, "get_sonar_echoes")   # only the protocol 3 helper has this
    rolled = False
    w, h = game.get_map_size()
    global WIDTH, HEIGHT
    WIDTH = w
    HEIGHT = h
    
    while unswbc.update(ct, game):
        round_num = game.get_round_num()


        if rolled == False:
            progress = (round_num - 1) / 500                            # 0 at start, 1 around round 350
            AGGRO_CHANCE = max(1 - 0.5 * progress, 0.3)                        # 100% -> floors at 40%
            AGGRESSIVE = stable_roll(ct.get_id()) < AGGRO_CHANCE                     # heavy subject to change
            rolled = True
            if round_num < 250:
                AGGRESSIVE = True


        if ct.get_length() > 5 and round_num > 1:    # Prevention from desperate split child from becoming aggressive
            AGGRESSIVE = False

        execute_turn()  
        send_sonar_report()   # sonars fire after our action, so send after the move
        unswbc.end_turn()


if __name__ == "__main__":
    main()