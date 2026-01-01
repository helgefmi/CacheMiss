#include "board.hpp"
#include "move.hpp"
#include "precalc.hpp"
#include <iostream>

int main() {
    Board board;
    for (auto move : generate_moves<Color::White>(board)) {
        std::cout << move.to_string() << '\n';
    }
    return 0;
}
