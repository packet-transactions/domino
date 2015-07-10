#include <iostream>
#include "clang/Lex/Lexer.h"
#include "clang_utility_functions.h"
#include "if_conversion_handler.h"

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::driver;
using namespace clang::tooling;

void IfConversionHandler::run(const MatchFinder::MatchResult & t_result) {
  const auto * if_stmt = t_result.Nodes.getNodeAs<IfStmt>("ifStmt");
  assert(if_stmt != nullptr);
  assert(if_stmt->getThen() != nullptr);
  if (if_stmt->getConditionVariableDeclStmt()) {
    throw std::logic_error("We don't yet handle declarations within the test portion of an if\n");
  }

  // Handle only unnested ifs. Every run processes only unnested ifs.
  // We keep iterating until clang-apply-replacements hits a fixed point.
  if (check_for_nesting(if_stmt)) {
    return;
  }

  // Create temporary variable to hold the if condition
  const auto condition_type_name = if_stmt->getCond()->getType().getAsString();
  const auto cond_variable = "tmp__" + std::to_string(uniform_dist_(prng_)); // TODO: This is sleazy, fix this sometime
  const auto cond_var_assignment = cond_variable + " = " + clang_stmt_printer(if_stmt->getCond()) + ";";

  // Convert statements within then block to ternary operators.
  assert(if_stmt->getThen());
  if (not isa<CompoundStmt>(if_stmt->getThen())) {
    // For now, error out if there's an if statement without braces (i.e. not CompoundStmt)
    throw std::logic_error("We don't yet handle if statments without braces\n");
  }

  // process if and else branch
  assert(isa<CompoundStmt>(if_stmt->getThen()));
  process_if_branch(dyn_cast<CompoundStmt>(if_stmt->getThen()), *t_result.SourceManager, cond_variable);
  if (if_stmt->getElse() != nullptr) {
    assert(isa<CompoundStmt>(if_stmt->getElse()));
    process_if_branch(dyn_cast<CompoundStmt>(if_stmt->getElse()), *t_result.SourceManager, "! " + cond_variable);
  }

  // Replace if statement with condition variable declaration
  CharSourceRange src_range;
  src_range.setBegin(if_stmt->getLocStart());
  src_range.setEnd(if_stmt->getThen()->getLocStart());
  replace_.insert(Replacement(*t_result.SourceManager, src_range, cond_var_assignment));

  // Remove if left and right brace
  remove_token(if_stmt->getThen()->getLocStart(), t_result);
  remove_token(if_stmt->getThen()->getLocEnd(), t_result);

  // Remove the else keyword
  if (if_stmt->getElse() != nullptr) {
    const auto begin_else = Lexer::getLocForEndOfToken(if_stmt->getThen()->getLocEnd(), 0, *t_result.SourceManager, t_result.Context->getLangOpts());
    src_range.setBegin(begin_else);
    src_range.setEnd(if_stmt->getElse()->getLocStart());
    replace_.insert(Replacement(*t_result.SourceManager, src_range, ""));

    // Remove else left and right brace
    remove_token(if_stmt->getElse()->getLocStart(), t_result);
    remove_token(if_stmt->getElse()->getLocEnd(), t_result);
  }

  // accumulate a declaration for the condition variable
  decl_strings_.emplace(condition_type_name + " " + cond_variable + ";");
}

void IfConversionHandler::remove_token(const SourceLocation & loc, const MatchFinder::MatchResult & t_result) {
  replace_.insert(Replacement(*t_result.SourceManager, get_src_range_for_loc(loc, t_result), ""));
}

CharSourceRange IfConversionHandler::get_src_range_for_loc(const SourceLocation & loc, const MatchFinder::MatchResult & t_result) const {
  CharSourceRange src_range;
  src_range.setBegin(Lexer::GetBeginningOfToken(loc, *t_result.SourceManager, t_result.Context->getLangOpts()));
  src_range.setEnd(Lexer::getLocForEndOfToken(loc, 0, *t_result.SourceManager, t_result.Context->getLangOpts()));
  return src_range;
}

bool IfConversionHandler::check_for_nesting(const IfStmt* if_stmt) const {
  assert(isa<CompoundStmt>(if_stmt->getThen()));
  for (const auto & child : if_stmt->getThen()->children()) {
    assert(child);
    if (isa<IfStmt>(child)) return true;
  }

  if (if_stmt->getElse()) {
    assert(isa<CompoundStmt>(if_stmt->getElse()));
    for (const auto & child : if_stmt->getElse()->children()) {
      assert(child);
      if (isa<IfStmt>(child)) return true;
    }
  }

  return false;
}

void IfConversionHandler::process_if_branch(const CompoundStmt * compound_stmt, SourceManager & source_manager, const std::string & cond_variable) {
  for (const auto & child : compound_stmt->children()) {
    assert(child);
    // When we if-convert a branch, we assume everything inside is already
    // if-converted and isn't an IfStmt or a CompoundStmt on its own.
    assert(not isa<CompoundStmt>(child));
    if (isa<DeclStmt>(child)) {
      throw std::logic_error("We don't yet handle variable declarations within if statements\n");
    }

    // The "atomic" statements can only be binary operators
    // Everything at this level is represented as a BinaryOperator in clang,
    // Even something like a = x ? 5 : 4; is a BinaryOperator with two operands
    // a and (x ? 5 : 4).
    assert(isa<BinaryOperator>(child));

    // Replace an atomic statement with a ternary version of itself
    replace_atomic_stmt(dyn_cast<BinaryOperator>(child), source_manager, cond_variable);
  }
}

void IfConversionHandler::replace_atomic_stmt(const BinaryOperator * stmt, SourceManager & source_manager, const std::string & cond_variable) {
  assert(stmt);
  assert(stmt->isAssignmentOp());
  assert(not stmt->isCompoundAssignmentOp());

  // Create predicated version of BinaryOperator
  const std::string lhs = clang_stmt_printer(dyn_cast<BinaryOperator>(stmt)->getLHS());
  const std::string rhs = "(" + cond_variable + " ? (" + clang_stmt_printer(dyn_cast<BinaryOperator>(stmt)->getRHS()) + ") :  " + lhs + ")";
  replace_.insert(Replacement(source_manager, stmt, lhs + " = " + rhs));
}