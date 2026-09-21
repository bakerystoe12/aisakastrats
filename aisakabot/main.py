import helper as unswbc
from helper import Direction, EdgeType
import random

ct: unswbc.Controller
game: unswbc.Game

# 2 roles, greedy/attack

def dist(a,b): # calculates distance from two positions, includes map wrapping
    w, h = game.get_map_size()
    dx = abs(a.x - b.x)
    dx = min(dx, w - dx)
    dy = abs(a.y - b.y)
    dy = min(dy, h - dy)
    return dx + dy

def attack():
    return

def greedy():
    # Each Dragon has 5 possible moves at any turn: Normal Movement, Sprint, Split
    # Prioritise going in different directions
    # Go to nearest best cluster of pearls + avoid going into dragons/obstacles
    
    return

def execute_turn() -> None:
    # main logic, basically for a bot, we either greedy, kamikaze
    greedy()
    return

def main() -> None:
    global ct, game
    ct, game = unswbc.init()

    while unswbc.update(ct, game):
        execute_turn()
        unswbc.end_turn()

if __name__ == "__main__":
    main()
