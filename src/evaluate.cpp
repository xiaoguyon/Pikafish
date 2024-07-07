/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2024 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "evaluate.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <memory>
#include <sstream>
#include <tuple>

#include "nnue/network.h"
#include "nnue/nnue_misc.h"
#include "position.h"
#include "types.h"
#include "uci.h"
#include "nnue/nnue_accumulator.h"

namespace Stockfish {
int evaluate_52_0 = 372, evaluate_53_0 = 11013, evaluate_55_0 = 31, evaluate_56_0 = 576, evaluate_56_1 = 107, evaluate_56_2 = 535, evaluate_62_0 = 279;
TUNE(evaluate_52_0, evaluate_53_0, evaluate_55_0, evaluate_56_0, evaluate_56_1, evaluate_56_2, evaluate_62_0);

// Evaluate is the evaluator for the outer world. It returns a static evaluation
// of the position from the point of view of the side to move.
Value Eval::evaluate(const Eval::NNUE::Network& network,
                     const Position&            pos,
                     NNUE::AccumulatorCaches&   caches,
                     int                        optimism) {

    assert(!pos.checkers());

    auto [psqt, positional] = network.evaluate(pos, &caches.cache);
    Value nnue              = psqt + positional;
    int   nnueComplexity    = std::abs(psqt - positional);

    // Blend optimism and eval with nnue complexity
    optimism += optimism * nnueComplexity / (evaluate_52_0);
    nnue -= nnue * nnueComplexity / (evaluate_53_0);

    int mm = pos.major_material() / (evaluate_55_0);
    int v  = (nnue * ((evaluate_56_0) + mm) + optimism * ((evaluate_56_1) + mm)) / (evaluate_56_2);

    // Evaluation grain (to get more alpha-beta cuts) with randomization (for robustness)
    v = (v / 16) * 16 - 1 + (pos.key() & 0x2);

    // Damp down the evaluation linearly when shuffling
    v -= (v * pos.rule60_count()) / (evaluate_62_0);

    // Guarantee evaluation does not hit the mate range
    v = std::clamp(v, VALUE_MATED_IN_MAX_PLY + 1, VALUE_MATE_IN_MAX_PLY - 1);

    return v;
}

// Like evaluate(), but instead of returning a value, it returns
// a string (suitable for outputting to stdout) that contains the detailed
// descriptions and values of each evaluation term. Useful for debugging.
// Trace scores are from white's point of view
std::string Eval::trace(Position& pos, const Eval::NNUE::Network& network) {

    if (pos.checkers())
        return "Final evaluation: none (in check)";

    auto caches = std::make_unique<Eval::NNUE::AccumulatorCaches>(network);

    std::stringstream ss;
    ss << std::showpoint << std::noshowpos << std::fixed << std::setprecision(2);

    ss << '\n' << NNUE::trace(pos, network, *caches) << '\n';

    ss << std::showpoint << std::showpos << std::fixed << std::setprecision(2) << std::setw(15);

    auto [psqt, positional] = network.evaluate(pos, &caches->cache);
    Value v                 = psqt + positional;
    v                       = pos.side_to_move() == WHITE ? v : -v;
    ss << "NNUE evaluation        " << 0.01 * UCIEngine::to_cp(v, pos) << " (white side)\n";

    v = evaluate(network, pos, *caches, VALUE_ZERO);
    v = pos.side_to_move() == WHITE ? v : -v;
    ss << "Final evaluation       " << 0.01 * UCIEngine::to_cp(v, pos) << " (white side)";
    ss << " [with scaled NNUE, ...]";
    ss << "\n";

    return ss.str();
}

}  // namespace Stockfish
