#include "board.hpp"
#include <algorithm>
#include <iostream>
#include <sstream>

Board::Board(std::string_view fen) {
    // Initialize all bitboards to zero
    std::ranges::fill(pieces[0], Bitboard(0));
    std::ranges::fill(pieces[1], Bitboard(0));
    std::ranges::fill(occupied, Bitboard(0));
    std::ranges::fill(pieces_on_square, std::nullopt);
    all_occupied = Bitboard(0);
    en_passant = Bitboard(0);
    castling = Bitboard(0);

    std::istringstream ss(fen.data());
    std::string position, active_color, castling_str, en_passant_str;
    ss >> position >> active_color >> castling_str >> en_passant_str;

    // Parse piece positions (FEN starts at rank 8, file a)
    int rank = 7, file = 0;
    for (char c : position) {
        if (c == '/') {
            rank--;
            file = 0;
        } else if (c >= '1' && c <= '8') {
            file += c - '0';
        } else if (auto piece = char_to_piece(c)) {
            auto [color, piece_type] = *piece;
            int sq = square_from_coords(file, rank);
            Bitboard bb = square_bb(sq);

            pieces[static_cast<int>(color)][static_cast<int>(piece_type)] |= bb;
            occupied[static_cast<int>(color)] |= bb;
            all_occupied |= bb;
            pieces_on_square[sq] = piece_type;

            file++;
        }
    }

    // Parse active color
    turn = (active_color == "b") ? Color::Black : Color::White;

    // Parse castling rights (stored as corner square bitboard)
    for (char c : castling_str) {
        switch (c) {
            case 'K': castling |= square_bb(7);  break;  // h1
            case 'Q': castling |= square_bb(0);  break;  // a1
            case 'k': castling |= square_bb(63); break;  // h8
            case 'q': castling |= square_bb(56); break;  // a8
            default: break;
        }
    }

    // Parse en passant square
    if (en_passant_str != "-" && en_passant_str.size() >= 2) {
        int ep_file = en_passant_str[0] - 'a';
        int ep_rank = en_passant_str[1] - '1';
        if (ep_file >= 0 && ep_file < 8 && ep_rank >= 0 && ep_rank < 8) {
            en_passant = square_bb(square_from_coords(ep_file, ep_rank));
        }
    }
}

void Board::print() const {
    constexpr std::string_view piece_chars = "PNBRQKpnbrqk";

    auto piece_at = [&](int sq) -> char {
        if (!(all_occupied & square_bb(sq))) return '.';

        for (int color = 0; color < 2; ++color) {
            if (!(occupied[color] & square_bb(sq))) continue;
            for (int piece = 0; piece < 6; ++piece) {
                if (pieces[color][piece] & square_bb(sq)) {
                    return piece_chars[color * 6 + piece];
                }
            }
        }
        return '?';
    };

    std::cout << "\n  +---+---+---+---+---+---+---+---+\n";
    for (int rank = 7; rank >= 0; --rank) {
        std::cout << rank + 1 << " |";
        for (int file = 0; file < 8; ++file) {
            std::cout << ' ' << piece_at(square_from_coords(file, rank)) << " |";
        }
        std::cout << "\n  +---+---+---+---+---+---+---+---+\n";
    }
    std::cout << "    a   b   c   d   e   f   g   h\n\n";

    std::cout << "Turn: " << (turn == Color::White ? "White" : "Black") << '\n';

    std::cout << "Castling: ";
    if (castling == 0) std::cout << "-";
    else {
        if (castling & square_bb(7))  std::cout << 'K';
        if (castling & square_bb(0))  std::cout << 'Q';
        if (castling & square_bb(63)) std::cout << 'k';
        if (castling & square_bb(56)) std::cout << 'q';
    }
    std::cout << '\n';

    std::cout << "En passant: ";
    if (en_passant == 0) std::cout << "-";
    else {
        int sq = lsb_index(en_passant);
        std::cout << char('a' + sq % 8) << char('1' + sq / 8);
    }
    std::cout << "\n\n";
}
