#pragma once
#include "erelang/parser.hpp"
namespace erelang {
struct OptimizeResult { int folds{0}; int passes{0}; int nodesReplaced{0}; };
OptimizeResult optimize_program(Program& program);
}
