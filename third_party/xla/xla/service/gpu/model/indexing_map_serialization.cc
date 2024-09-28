/* Copyright 2024 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/service/gpu/model/indexing_map_serialization.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/types/span.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include "mlir/AsmParser/AsmParser.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Support/LLVM.h"
#include "xla/service/gpu/model/indexing_map.h"

namespace xla {
namespace gpu {
namespace {

using llvm::SmallVector;
using llvm::SmallVectorImpl;
using llvm::StringRef;
using mlir::AffineBinaryOpExpr;
using mlir::AffineConstantExpr;
using mlir::AffineDimExpr;
using mlir::AffineExpr;
using mlir::AffineExprKind;
using mlir::AffineMap;
using mlir::AffineMapAttr;
using mlir::AffineSymbolExpr;
using mlir::ArrayRef;
using mlir::MLIRContext;

enum class Delimeter { kParen, kBracket };

struct Token {
  enum class Kind {
    // Variable name, e.g. "d0", "s1".
    kVarName,
    // Integer literal.
    kIntLiteral,
    kBoolLiteral,
    // Keywords
    kKeywordDomain,
    kKeywordIn,
    kKeywordIsSimplified,
    // Arithmetic operation, e.g. "+", "-", "*", "floorDiv", "mod".
    kPlus,
    kMinus,
    kTimes,
    kFloorDiv,
    kMod,
    // Punctuation.
    kArrow,
    kLParen,
    kRParen,
    kLBracket,
    kRBracket,
    kComma,
    kColon,
    // Status.
    kError,
    kEOF
  };
  StringRef spelling;
  Token::Kind kind;
};

Token::Kind GetSingleCharTokenType(char c) {
  switch (c) {
    case '(':
      return Token::Kind::kLParen;
    case ')':
      return Token::Kind::kRParen;
    case '[':
      return Token::Kind::kLBracket;
    case ']':
      return Token::Kind::kRBracket;
    case ',':
      return Token::Kind::kComma;
    case ':':
      return Token::Kind::kColon;
    case '+':
      return Token::Kind::kPlus;
    case '-':
      return Token::Kind::kMinus;
    case '*':
      return Token::Kind::kTimes;
    default:
      return Token::Kind::kError;
  }
}

bool IsPartOfAffineExpr(Token token) {
  return token.kind == Token::Kind::kVarName ||
         token.kind == Token::Kind::kIntLiteral ||
         token.kind == Token::Kind::kPlus ||
         token.kind == Token::Kind::kMinus ||
         token.kind == Token::Kind::kTimes ||
         token.kind == Token::Kind::kFloorDiv ||
         token.kind == Token::Kind::kMod;
}

class Parser {
 public:
  explicit Parser(llvm::StringRef input) : input_(input), it_(input.begin()) {
    // Set the parser to the first token.
    current_token_ = GetNextTokenImpl();
  }

  const Token& GetCurrentToken() const { return current_token_; };
  void Advance() {
    if (current_token_.kind == Token::Kind::kError ||
        current_token_.kind == Token::Kind::kEOF) {
      return;
    }
    current_token_ = GetNextTokenImpl();
  }
  Token GetNextToken() {
    Advance();
    return current_token_;
  }

  bool ConsumeToken(Token::Kind kind);
  bool ParseVarName(std::string* var_name);
  bool ParseInt(int64_t* value);
  bool ParseBool(bool* boolean);
  bool ParseInterval(Interval* interval);
  bool ParseAffineExprString(std::string* affine_expr_str);
  bool ParseCommaSeparatedVarList(
      Delimeter delimeter,
      llvm::function_ref<bool(Parser& parser)> parse_element_fn);

 private:
  void ConsumeWhitespace() {
    while (it_ != input_.end() && std::isspace(*it_)) ++it_;
  }

  // Parses the next token from the input and sets the iterator to the position
  // right after it.
  Token GetNextTokenImpl();

  llvm::StringRef input_;
  llvm::StringRef::iterator it_;
  Token current_token_;
};

bool Parser::ParseVarName(std::string* var_name) {
  if (current_token_.kind != Token::Kind::kVarName) {
    llvm::errs() << "Expected var name, got: " << current_token_.spelling
                 << "\n";
    return false;
  }
  *var_name = current_token_.spelling.str();
  Advance();
  return true;
}

bool Parser::ParseInt(int64_t* value) {
  int val;
  if (current_token_.kind != Token::Kind::kIntLiteral ||
      current_token_.spelling.getAsInteger(/*radix=*/0, val)) {
    llvm::errs() << "Expected int literal, got: " << current_token_.spelling
                 << "\n";
    return false;
  }
  *value = static_cast<int64_t>(val);
  Advance();
  return true;
}

bool Parser::ParseBool(bool* boolean) {
  if (current_token_.kind != Token::Kind::kBoolLiteral) {
    llvm::errs() << "Expected bool literal, got: " << current_token_.spelling
                 << "\n";
    return false;
  }
  *boolean = current_token_.spelling.compare("true") == 0;
  Advance();
  return true;
}

bool Parser::ParseInterval(Interval* interval) {
  if (!ConsumeToken(Token::Kind::kLBracket) || !ParseInt(&interval->lower) ||
      !ConsumeToken(Token::Kind::kComma) || !ParseInt(&interval->upper) ||
      !ConsumeToken(Token::Kind::kRBracket)) {
    return false;
  }
  return interval;
}

bool Parser::ParseAffineExprString(std::string* affine_expr_str) {
  unsigned num_unmatched_parens = 0;
  while (true) {
    if (IsPartOfAffineExpr(current_token_)) {
      affine_expr_str->append(current_token_.spelling);
      affine_expr_str->push_back(' ');
      Advance();
      continue;
    }
    if (ConsumeToken(Token::Kind::kLParen)) {
      affine_expr_str->push_back('(');
      ++num_unmatched_parens;
      continue;
    }
    if (current_token_.kind == Token::Kind::kRParen &&
        num_unmatched_parens > 0) {
      affine_expr_str->push_back(')');
      --num_unmatched_parens;
      Advance();
      continue;
    }
    break;
  }
  return current_token_.kind != Token::Kind::kError;
}

bool Parser::ParseCommaSeparatedVarList(
    Delimeter delimeter,
    llvm::function_ref<bool(Parser& parser)> parse_element_fn) {
  auto left_delimiter = delimeter == Delimeter::kParen ? Token::Kind::kLParen
                                                       : Token::Kind::kLBracket;
  auto right_delimiter = delimeter == Delimeter::kParen
                             ? Token::Kind::kRParen
                             : Token::Kind::kRBracket;
  if (!ConsumeToken(left_delimiter)) {
    return false;
  }
  if (ConsumeToken(right_delimiter)) {
    return true;
  }
  std::string element;
  while (parse_element_fn(*this)) {
    if (ConsumeToken(Token::Kind::kComma)) continue;
    return ConsumeToken(right_delimiter);
  }
  return false;
}

bool Parser::ConsumeToken(Token::Kind kind) {
  Token token = GetCurrentToken();
  if (token.kind != kind) {
    return false;
  }
  GetNextToken();
  return true;
}

Token Parser::GetNextTokenImpl() {
  ConsumeWhitespace();
  if (it_ == input_.end()) {
    return Token{"", Token::Kind::kEOF};
  }
  auto start = it_;
  if (std::isalpha(*it_)) {
    // Variable name.
    while (it_ != input_.end() &&
           (std::isalpha(*it_) || std::isdigit(*it_) || *it_ == '_')) {
      ++it_;
    }
    StringRef spelling = input_.substr(start - input_.data(), it_ - start);
    if (spelling == "true" || spelling == "false") {
      return Token{spelling, Token::Kind::kBoolLiteral};
    }
    if (spelling == "domain") {
      return Token{spelling, Token::Kind::kKeywordDomain};
    }
    if (spelling == "in") {
      return Token{spelling, Token::Kind::kKeywordIn};
    }
    if (spelling == "mod") {
      return Token{spelling, Token::Kind::kMod};
    }
    if (spelling == "floorDiv") {
      return Token{spelling, Token::Kind::kFloorDiv};
    }
    return Token{spelling, Token::Kind::kVarName};
  }
  if (std::isdigit(*it_)) {
    auto start = it_;
    while (it_ != input_.end() && std::isdigit(*it_)) {
      ++it_;
    }

    StringRef spelling = input_.substr(start - input_.data(), it_ - start);
    return Token{spelling, Token::Kind::kIntLiteral};
  }
  if (*it_ == '-') {
    ++it_;
    if (it_ != input_.end()) {
      if (*it_ == '>') {
        ++it_;
        return Token{"->", Token::Kind::kArrow};
      } else if (std::isdigit(*it_)) {
        auto start = it_ - 1;
        while (it_ != input_.end() && std::isdigit(*it_)) {
          ++it_;
        }
        StringRef spelling = input_.substr(start - input_.data(), it_ - start);
        return Token{spelling, Token::Kind::kIntLiteral};
      } else {
        return Token{"-", Token::Kind::kMinus};
      }
    }
  }
  StringRef spelling = input_.substr(start - input_.data(), 1);
  return Token{spelling, GetSingleCharTokenType(*(it_++))};
}

// Parses a comma separated list of variable names. It is used to parse the
// lists of dimension and symbol variables.
bool ParseVarNames(Parser& parser, Delimeter delimeter,
                   SmallVectorImpl<std::string>& var_names) {
  auto parse_var_name_fn = [&](Parser& parser) {
    std::string var_name;
    if (!parser.ParseVarName(&var_name)) {
      return false;
    }
    var_names.push_back(var_name);
    return true;
  };
  return parser.ParseCommaSeparatedVarList(delimeter, parse_var_name_fn);
}

// Parses a comma separated list of affine expressions. It is used to parse
// the list of affine map results.
bool ParseAffineMapResults(Parser& parser,
                           SmallVectorImpl<std::string>& affine_expr_strs) {
  auto parse_var_name_fn = [&](Parser& parser) {
    std::string affine_expr_str;
    if (!parser.ParseAffineExprString(&affine_expr_str)) {
      return false;
    }
    affine_expr_strs.push_back(affine_expr_str);
    return true;
  };
  return parser.ParseCommaSeparatedVarList(Delimeter::kParen,
                                           parse_var_name_fn);
}

// Assembles an affine map from the given dimension and symbol names and the
// affine expressions for the results.
bool ParseAffineExprsWithMLIR(ArrayRef<std::string> dim_var_names,
                              ArrayRef<std::string> symbol_var_names,
                              ArrayRef<std::string> affine_expr_strings,
                              MLIRContext* context,
                              SmallVectorImpl<AffineExpr>& affine_exprs) {
  std::stringstream ss;
  ss << "affine_map<(" << absl::StrJoin(dim_var_names, ", ") << ") ";
  if (!symbol_var_names.empty()) {
    ss << '[' << absl::StrJoin(symbol_var_names, ", ") << "] ";
  }
  ss << " -> (" << absl::StrJoin(affine_expr_strings, ", ") << ")>";
  auto affine_map_attr = mlir::parseAttribute(ss.str(), context);
  if (!affine_map_attr) {
    llvm::errs() << "Failed to parse affine map: " << ss.str() << "\n";
    return false;
  }
  AffineMap affine_map = mlir::cast<AffineMapAttr>(affine_map_attr).getValue();
  affine_exprs = llvm::to_vector(affine_map.getResults());
  return true;
}

std::string GetSymbolName(int64_t symbol_id,
                          absl::Span<const std::string> symbol_names = {}) {
  if (symbol_names.empty()) {
    return absl::StrCat("s", symbol_id);
  }
  return symbol_names.at(symbol_id);
}

std::string GetDimensionName(int64_t dim_id,
                             absl::Span<const std::string> dim_names = {}) {
  if (dim_names.empty()) {
    return absl::StrCat("d", dim_id);
  }
  return dim_names.at(dim_id);
}

void PrintAffineExprImpl(const AffineExpr affine_expr,
                         absl::Span<const std::string> dim_names,
                         absl::Span<const std::string> symbol_names,
                         bool add_parentheses, llvm::raw_ostream& os) {
  const char* binopSpelling = nullptr;
  switch (affine_expr.getKind()) {
    case AffineExprKind::SymbolId: {
      unsigned symbol_id =
          mlir::cast<AffineSymbolExpr>(affine_expr).getPosition();
      os << GetSymbolName(symbol_id, symbol_names);
      return;
    }
    case AffineExprKind::DimId: {
      unsigned dim_id = mlir::cast<AffineDimExpr>(affine_expr).getPosition();
      os << GetDimensionName(dim_id, dim_names);
      return;
    }
    case AffineExprKind::Constant:
      os << mlir::cast<AffineConstantExpr>(affine_expr).getValue();
      return;
    case AffineExprKind::Add:
      binopSpelling = " + ";
      break;
    case AffineExprKind::Mul:
      binopSpelling = " * ";
      break;
    case AffineExprKind::FloorDiv:
      binopSpelling = " floordiv ";
      break;
    case AffineExprKind::CeilDiv:
      binopSpelling = " ceildiv ";
      break;
    case AffineExprKind::Mod:
      binopSpelling = " mod ";
      break;
  }

  auto binOp = mlir::cast<AffineBinaryOpExpr>(affine_expr);
  AffineExpr lhsExpr = binOp.getLHS();
  AffineExpr rhsExpr = binOp.getRHS();

  // Handle tightly binding binary operators.
  if (binOp.getKind() != AffineExprKind::Add) {
    if (add_parentheses) {
      os << '(';
    }

    // Pretty print multiplication with -1.
    auto rhsConst = mlir::dyn_cast<AffineConstantExpr>(rhsExpr);
    if (rhsConst && binOp.getKind() == AffineExprKind::Mul &&
        rhsConst.getValue() == -1) {
      os << "-";
      PrintAffineExprImpl(lhsExpr, dim_names, symbol_names,
                          /*add_parentheses=*/true, os);
      if (add_parentheses) {
        os << ')';
      }
      return;
    }
    PrintAffineExprImpl(lhsExpr, dim_names, symbol_names,
                        /*add_parentheses=*/true, os);

    os << binopSpelling;
    PrintAffineExprImpl(rhsExpr, dim_names, symbol_names,
                        /*add_parentheses=*/true, os);

    if (add_parentheses) {
      os << ')';
    }
    return;
  }

  // Print out special "pretty" forms for add.
  if (add_parentheses) {
    os << '(';
  }

  // Pretty print addition to a product that has a negative operand as a
  // subtraction.
  if (auto rhs = mlir::dyn_cast<AffineBinaryOpExpr>(rhsExpr)) {
    if (rhs.getKind() == AffineExprKind::Mul) {
      AffineExpr rrhsExpr = rhs.getRHS();
      if (auto rrhs = mlir::dyn_cast<AffineConstantExpr>(rrhsExpr)) {
        if (rrhs.getValue() == -1) {
          PrintAffineExprImpl(lhsExpr, dim_names, symbol_names,
                              /*add_parentheses=*/false, os);
          os << " - ";
          if (rhs.getLHS().getKind() == AffineExprKind::Add) {
            PrintAffineExprImpl(rhs.getLHS(), dim_names, symbol_names,
                                /*add_parentheses=*/true, os);
          } else {
            PrintAffineExprImpl(rhs.getLHS(), dim_names, symbol_names,
                                /*add_parentheses=*/false, os);
          }
          if (add_parentheses) {
            os << ')';
          }
          return;
        }

        if (rrhs.getValue() < -1) {
          PrintAffineExprImpl(lhsExpr, dim_names, symbol_names,
                              /*add_parentheses=*/false, os);
          os << " - ";
          PrintAffineExprImpl(rhs.getLHS(), dim_names, symbol_names,
                              /*add_parentheses=*/true, os);
          os << " * " << -rrhs.getValue();
          if (add_parentheses) {
            os << ')';
          }
          return;
        }
      }
    }
  }

  // Pretty print addition to a negative number as a subtraction.
  if (auto rhsConst = mlir::dyn_cast<AffineConstantExpr>(rhsExpr)) {
    if (rhsConst.getValue() < 0) {
      PrintAffineExprImpl(lhsExpr, dim_names, symbol_names,
                          /*add_parentheses=*/false, os);
      os << " - " << -rhsConst.getValue();
      if (add_parentheses) {
        os << ')';
      }
      return;
    }
  }

  PrintAffineExprImpl(lhsExpr, dim_names, symbol_names,
                      /*add_parentheses=*/false, os);

  os << " + ";
  PrintAffineExprImpl(rhsExpr, dim_names, symbol_names,
                      /*add_parentheses=*/false, os);

  if (add_parentheses) {
    os << ')';
  }
}

}  // namespace

std::optional<IndexingMap> ParseIndexingMap(llvm::StringRef input,
                                            MLIRContext* context) {
  Parser parser(input);

  // Parse variable names.
  SmallVector<std::string, 8> dim_var_names;
  SmallVector<std::string, 4> symbol_var_names;
  if (!ParseVarNames(parser, Delimeter::kParen, dim_var_names) ||
      (parser.GetCurrentToken().kind == Token::Kind::kLBracket &&
       !ParseVarNames(parser, Delimeter::kBracket, symbol_var_names))) {
    llvm::errs() << "Failed to parse variable names\n";
    return std::nullopt;
  }

  // Parse affine map results.
  SmallVector<std::string, 3> affine_expr_strs;
  if (!parser.ConsumeToken(Token::Kind::kArrow) ||
      !ParseAffineMapResults(parser, affine_expr_strs)) {
    llvm::errs() << "Failed to parse affine map results\n";
    return std::nullopt;
  }
  int num_affine_map_results = affine_expr_strs.size();

  // Special case: no domain is printed for the empty map.
  if (dim_var_names.empty() && symbol_var_names.empty()) {
    if (num_affine_map_results != 0 ||
        parser.GetCurrentToken().kind != Token::Kind::kEOF) {
      llvm::errs() << "Expected an empty indexing map\n";
      return std::nullopt;
    }
    return IndexingMap{AffineMap::get(context), /*dimensions=*/{},
                       /*range_vars=*/{}, /*rt_vars=*/{}};
  }

  if (!parser.ConsumeToken(Token::Kind::kComma) ||
      !parser.ConsumeToken(Token::Kind::kKeywordDomain) ||
      !parser.ConsumeToken(Token::Kind::kColon)) {
    llvm::errs() << "Failed to parse domain keyword\n";
    return std::nullopt;
  }
  // Parse dimension variables.
  std::vector<DimVar> dim_vars;
  for (auto& dim_name : dim_var_names) {
    std::string var_name;
    Interval interval;
    if (!parser.ParseVarName(&var_name) ||
        !parser.ConsumeToken(Token::Kind::kKeywordIn) ||
        !parser.ParseInterval(&interval) ||
        (parser.GetCurrentToken().kind != Token::Kind::kEOF &&
         !parser.ConsumeToken(Token::Kind::kComma))) {
      llvm::errs() << "Failed to parse DimVar\n";
      return std::nullopt;
    }
    if (var_name != dim_name) {
      llvm::errs() << "Dimension name mismatch\n";
      return std::nullopt;
    }
    dim_vars.push_back(DimVar{interval});
  }
  // Parse range variables.
  std::vector<RangeVar> range_vars;
  for (auto& symbol_var : symbol_var_names) {
    std::string var_name;
    Interval interval;
    if (!parser.ParseVarName(&var_name) ||
        !parser.ConsumeToken(Token::Kind::kKeywordIn) ||
        !parser.ParseInterval(&interval) ||
        (parser.GetCurrentToken().kind != Token::Kind::kEOF &&
         !parser.ConsumeToken(Token::Kind::kComma))) {
      llvm::errs() << "Failed to parse RangeVar\n";
      return std::nullopt;
    }
    if (var_name != symbol_var) {
      llvm::errs() << "Symbol name mismatch\n";
      return std::nullopt;
    }
    range_vars.push_back(RangeVar{interval});
  }
  // Parse constraints.
  SmallVector<Interval> constraint_bounds;
  while (!parser.ConsumeToken(Token::Kind::kEOF)) {
    std::string affine_expr_str;
    Interval interval;
    if (!parser.ParseAffineExprString(&affine_expr_str) ||
        !parser.ConsumeToken(Token::Kind::kKeywordIn) ||
        !parser.ParseInterval(&interval) ||
        (parser.GetCurrentToken().kind != Token::Kind::kEOF &&
         !parser.ConsumeToken(Token::Kind::kComma))) {
      llvm::errs() << "Failed to parse constraint\n";
      return std::nullopt;
    }
    affine_expr_strs.push_back(affine_expr_str);
    constraint_bounds.push_back(interval);
  }
  // Parse affine expressions.
  SmallVector<AffineExpr> affine_exprs;
  if (!ParseAffineExprsWithMLIR(dim_var_names, symbol_var_names,
                                affine_expr_strs, context, affine_exprs)) {
    return std::nullopt;
  }
  ArrayRef<AffineExpr> affine_map_results =
      ArrayRef(affine_exprs).take_front(num_affine_map_results);
  ArrayRef<AffineExpr> constraint_exprs =
      ArrayRef(affine_exprs).drop_front(num_affine_map_results);

  // Populate constraints.
  SmallVector<std::pair<AffineExpr, Interval>> constraints;
  constraints.reserve(constraint_exprs.size());
  for (const auto& [expr, bounds] :
       llvm::zip(constraint_exprs, constraint_bounds)) {
    constraints.push_back(std::make_pair(expr, bounds));
  }
  auto map = AffineMap::get(dim_vars.size(), range_vars.size(),
                            affine_map_results, context);
  return IndexingMap{map, std::move(dim_vars), std::move(range_vars),
                     /*rt_vars=*/{}, constraints};
}

std::string ToString(AffineExpr affine_expr) {
  return ToString(affine_expr, /*dim_names=*/{}, /*symbol_names=*/{});
}

std::ostream& operator<<(std::ostream& out, AffineExpr affine_expr) {
  out << ToString(affine_expr);
  return out;
}

std::string ToString(AffineExpr affine_expr,
                     absl::Span<const std::string> dim_names,
                     absl::Span<const std::string> symbol_names) {
  std::string s;
  llvm::raw_string_ostream ss(s);
  PrintAffineExprImpl(affine_expr, dim_names, symbol_names,
                      /*add_parentheses=*/false, ss);
  return s;
}

std::string ToString(AffineMap affine_map) {
  int dim_count = affine_map.getNumDims();
  SmallVector<std::string, 3> dim_names;
  dim_names.reserve(affine_map.getNumDims());
  for (int64_t dim_id = 0; dim_id < dim_count; ++dim_id) {
    dim_names.push_back(GetDimensionName(dim_id));
  }
  int symbol_count = affine_map.getNumSymbols();
  SmallVector<std::string, 3> symbol_names;
  symbol_names.reserve(affine_map.getNumSymbols());
  for (int64_t symbol_id = 0; symbol_id < symbol_count; ++symbol_id) {
    symbol_names.push_back(GetSymbolName(symbol_id));
  }
  return ToString(affine_map, dim_names, symbol_names);
}

std::ostream& operator<<(std::ostream& out, AffineMap affine_map) {
  out << ToString(affine_map);
  return out;
}

std::string ToString(AffineMap affine_map,
                     absl::Span<const std::string> dim_names,
                     absl::Span<const std::string> symbol_names) {
  CHECK_EQ(dim_names.size(), affine_map.getNumDims());
  CHECK_EQ(symbol_names.size(), affine_map.getNumSymbols());

  std::string s;
  llvm::raw_string_ostream ss(s);

  // Dimension identifiers.
  ss << '(' << absl::StrJoin(dim_names, ", ") << ')';
  // Symbolic identifiers.
  if (affine_map.getNumSymbols() != 0) {
    ss << '[' << absl::StrJoin(symbol_names, ", ") << ']';
  }
  // Result affine expressions.
  ss << " -> (";
  llvm::interleaveComma(affine_map.getResults(), ss, [&](AffineExpr expr) {
    PrintAffineExprImpl(expr, dim_names, symbol_names,
                        /*add_parentheses=*/false, ss);
  });
  ss << ')';
  return s;
}

std::string ToString(const IndexingMap& indexing_map) {
  const auto& affine_map = indexing_map.GetAffineMap();
  int dim_count = affine_map.getNumDims();
  SmallVector<std::string, 3> dim_names;
  dim_names.reserve(affine_map.getNumDims());
  for (int64_t dim_id = 0; dim_id < dim_count; ++dim_id) {
    dim_names.push_back(GetDimensionName(dim_id));
  }
  int symbol_count = affine_map.getNumSymbols();
  SmallVector<std::string, 3> symbol_names;
  symbol_names.reserve(affine_map.getNumSymbols());
  for (int64_t symbol_id = 0; symbol_id < symbol_count; ++symbol_id) {
    symbol_names.push_back(GetSymbolName(symbol_id));
  }
  return ToString(indexing_map, dim_names, symbol_names);
}

std::ostream& operator<<(std::ostream& out, const IndexingMap& indexing_map) {
  out << ToString(indexing_map);
  return out;
}

std::string ToString(const IndexingMap& indexing_map,
                     absl::Span<const std::string> dim_names,
                     absl::Span<const std::string> symbol_names) {
  std::stringstream ss;
  if (indexing_map.IsKnownEmpty()) {
    ss << "KNOWN EMPTY\n";
    return ss.str();
  }
  const auto& dim_vars = indexing_map.GetDimVars();
  const auto& range_vars = indexing_map.GetRangeVars();
  const auto& rt_vars = indexing_map.GetRTVars();
  ss << ToString(indexing_map.GetAffineMap(), dim_names, symbol_names);
  if (dim_vars.empty() && range_vars.empty() && rt_vars.empty()) {
    return ss.str();
  }
  ss << ", domain: ";
  int64_t remaining_vars_to_print =
      dim_vars.size() + range_vars.size() + rt_vars.size();
  for (const auto& [index, dim_var] : llvm::enumerate(dim_vars)) {
    ss << dim_names[index] << " in " << dim_var.bounds;
    if (--remaining_vars_to_print > 0) {
      ss << ", ";
    }
  }
  for (const auto& [index, range_var] : llvm::enumerate(range_vars)) {
    ss << symbol_names[index] << " in " << range_var.range;
    if (--remaining_vars_to_print > 0) {
      ss << ", ";
    }
  }
  int64_t num_range_vars = range_vars.size();
  for (const auto& [index, rt_var] : llvm::enumerate(rt_vars)) {
    ss << GetSymbolName(num_range_vars + index, symbol_names) << " in "
       << rt_var.feasible_values << ",  hlo: "
       << (rt_var.hlo == nullptr ? "NULL" : rt_var.hlo->ToString()) << ",  "
       << ToString(rt_var.map);
    if (--remaining_vars_to_print > 0) {
      ss << ", ";
    }
  }
  std::vector<std::string> expr_range_strings;
  const auto& constraints = indexing_map.GetConstraints();
  expr_range_strings.reserve(constraints.size());
  for (const auto& [expr, range] : constraints) {
    expr_range_strings.push_back(absl::StrCat(
        ToString(expr, dim_names, symbol_names), " in ", range.ToString()));
  }
  std::sort(expr_range_strings.begin(), expr_range_strings.end());
  if (!expr_range_strings.empty()) {
    ss << ", " << absl::StrJoin(expr_range_strings, ", ");
  }
  return ss.str();
}

}  // namespace gpu
}  // namespace xla
