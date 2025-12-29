#include "board.hpp"
#include "precalc.hpp"
#include <iostream>

int main() {
    Board board;
    board.print();
    print_bitboard(KNIGHT_MOVES[36]); // Print knight moves from e5 (square 36)
    return 0;
}
