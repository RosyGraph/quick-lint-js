// Copyright (C) 2020  Matthew Glazar
// See end of file for extended copyright information.

#include <algorithm>
#include <optional>
#include <quick-lint-js/assert.h>
#include <quick-lint-js/char8.h>
#include <quick-lint-js/error.h>
#include <quick-lint-js/language.h>
#include <quick-lint-js/lex.h>
#include <quick-lint-js/lint.h>
#include <quick-lint-js/optional.h>
#include <vector>

// The linter class implements single-pass variable lookup. A single-pass
// algorithm is complicated in JavaScript for a few reasons:
//
// * Variables declared with 'var' or 'function' statements are hoisted. This
//   means that the declaration might be *textually-after* a use of that
//   variable:
//
//     console.log(x);  // OK; x holds undefined.
//     var x = 3;
//
// * Simiarly, variables declared with 'class', 'const', or 'let' statements are
//   pseudo-hoisted: it is an error to reference the variable textually-before
//   its declaration:
//
//     console.log(x);  // ERROR; x is uninitialized.
//     let x = 3;
//
// * Variables of any type can referenced textually-before their declaration if
//   the variable is declared in a containing function:
//
//     function f() {
//       console.log(x);  // OK, if f is called after x' declaration.
//     }
//     let x = 3;
//     f();
//
// * Pseudo-hoisted variables shadow other variables:
//
//     let x;
//     {
//       console.log(x);  // ERROR; x refers to the variable declared below, so
//                        // x is uninitialized.
//       let x;
//     }
//
// To satisfy these requirements, the linter class implements the following
// algorithm (simplified for digestion):
//
// * When we see a variable declaration (visit_variable_declaration):
//   * Remember the declaration for the current scope (declared_variables).
//   * If the variable was already used in the current scope (variables_used or
//     variables_used_in_descendant_scope):
//     * Report a use-before-declaration error if necessary.
//     * Check use legality [1].
//     * Forget the variable use in the current scope (variables_used or
//       variables_used_in_descendant_scope).
// * When we see a variable use (visit_variable_assignment or
//   visit_variable_use):
//   * If the variable is declared in the current scope:
//     * Check use legality [1].
//   * Otherwise (if the variable is not declared in the current scope):
//     * Remember the use for the current scope (variables_used).
// * When we reach the end of a scope:
//   * For each remembered variable use in the current scope (variables_used and
//     variables_used_in_descendant_scope):
//     * If the current scope is a function scope:
//       * Use the variable in the parent scope (as if calling
//         visit_variable_assignment or visit_variable_use), except permit
//         use-before-declaration (variables_used_in_descendant_scope).
//     * Otherwise (if the current scope is not a function scope):
//       * Use the variable in the parent scope (as if calling
//         visit_variable_assignment or visit_variable_use).
//     * Remember: the variable use is not in the current scope's remembered
//       variable declarations (declared_variables).
// * When we reach the end of the module (visit_end_of_module):
//   * For each remember variable use in the current scope (variables_used and
//     variables_used_in_descendant_scope):
//     * Report a use-of-undeclared-variable error.
//
// Note: Counter to your likely intuition, when we see a variable use, we do
// *not* look for declarations in all ancestor scopes. We only ever look for
// declarations in the current scope. Looking in ancestor scopes would work for
// a two-pass linter (find declaration pass; bind uses to declarations pass),
// but not for a one-pass linter like ours.
//
// [1] "Check use legality" includes checks unrelated to variable lookup, such
//     as reporting an error if a 'const'-declared variable is assigned to.

namespace quick_lint_js {
linter::declared_variable_set linter::make_global_variables() {
  declared_variable_set vars;

  const char8 *writable_global_variables[] = {
      // ECMA-262 18.1 Value Properties of the Global Object
      u8"globalThis",

      // ECMA-262 18.2 Function Properties of the Global Object
      u8"decodeURI",
      u8"decodeURIComponent",
      u8"encodeURI",
      u8"encodeURIComponent",
      u8"eval",
      u8"isFinite",
      u8"isNaN",
      u8"parseFloat",
      u8"parseInt",

      // ECMA-262 18.3 Constructor Properties of the Global Object
      u8"Array",
      u8"ArrayBuffer",
      u8"BigInt",
      u8"BigInt64Array",
      u8"BigUint64Array",
      u8"Boolean",
      u8"DataView",
      u8"Date",
      u8"Error",
      u8"EvalError",
      u8"Float32Array",
      u8"Float64Array",
      u8"Function",
      u8"Int16Array",
      u8"Int32Array",
      u8"Int8Array",
      u8"Map",
      u8"Number",
      u8"Object",
      u8"Promise",
      u8"Proxy",
      u8"RangeError",
      u8"ReferenceError",
      u8"RegExp",
      u8"Set",
      u8"SharedArrayBuffer",
      u8"String",
      u8"Symbol",
      u8"SyntaxError",
      u8"TypeError",
      u8"URIError",
      u8"Uint16Array",
      u8"Uint32Array",
      u8"Uint8Array",
      u8"Uint8ClampedArray",
      u8"WeakMap",
      u8"WeakSet",

      // ECMA-262 18.4 Other Properties of the Global Object
      u8"Atomics",
      u8"JSON",
      u8"Math",
      u8"Reflect",

      // Node.js
      u8"Buffer",
      u8"GLOBAL",
      u8"Intl",
      u8"TextDecoder",
      u8"TextEncoder",
      u8"URL",
      u8"URLSearchParams",
      u8"WebAssembly",
      u8"clearImmediate",
      u8"clearInterval",
      u8"clearTimeout",
      u8"console",
      u8"escape",
      u8"global",
      u8"process",
      u8"queueMicrotask",
      u8"root",
      u8"setImmediate",
      u8"setInterval",
      u8"setTimeout",
      u8"unescape",
  };
  for (const char8 *global_variable : writable_global_variables) {
    vars.add_predefined_variable_declaration(global_variable,
                                             variable_kind::_function);
  }

  const char8 *non_writable_global_variables[] = {
      // ECMA-262 18.1 Value Properties of the Global Object
      u8"Infinity",
      u8"NaN",
      u8"undefined",
  };
  for (const char8 *global_variable : non_writable_global_variables) {
    vars.add_predefined_variable_declaration(global_variable,
                                             variable_kind::_const);
  }

  return vars;
}

const linter::declared_variable_set *linter::get_global_variables() {
  static declared_variable_set vars = make_global_variables();
  return &vars;
}

linter::linter(error_reporter *error_reporter)
    : global_scope_(this->get_global_variables()),
      error_reporter_(error_reporter) {
  scope &module_scope = this->scopes_.module_scope();

  const char8 *writable_module_variables[] = {
      // Node.js
      u8"__dirname", u8"__filename", u8"exports", u8"module", u8"require",
  };

  for (const char8 *module_variable : writable_module_variables) {
    module_scope.declared_variables.add_predefined_variable_declaration(
        module_variable, variable_kind::_function);
  }
}

void linter::visit_enter_block_scope() { this->scopes_.push(); }

void linter::visit_enter_class_scope() { this->scopes_.push(); }

void linter::visit_enter_for_scope() { this->scopes_.push(); }

void linter::visit_enter_function_scope() { this->scopes_.push(); }

void linter::visit_enter_function_scope_body() {
  this->propagate_variable_uses_to_parent_scope(
      /*allow_variable_use_before_declaration=*/true,
      /*consume_arguments=*/true);
}

void linter::visit_enter_named_function_scope(identifier function_name) {
  scope &current_scope = this->scopes_.push();
  current_scope.function_expression_declaration = declared_variable::make_local(
      function_name, variable_kind::_function,
      declared_variable_scope::declared_in_current_scope);
}

void linter::visit_exit_block_scope() {
  QLJS_ASSERT(!this->scopes_.empty());
  this->propagate_variable_uses_to_parent_scope(
      /*allow_variable_use_before_declaration=*/false,
      /*consume_arguments=*/false);
  this->propagate_variable_declarations_to_parent_scope();
  this->scopes_.pop();
}

void linter::visit_exit_class_scope() {
  QLJS_ASSERT(!this->scopes_.empty());
  this->propagate_variable_uses_to_parent_scope(
      /*allow_variable_use_before_declaration=*/false,
      /*consume_arguments=*/false);

  // No variable declarations should be propagatable to the parent scope.
  for (const declared_variable &var :
       this->current_scope().declared_variables) {
    QLJS_ASSERT(var.kind() == variable_kind::_class);
  }

  this->scopes_.pop();
}

void linter::visit_exit_for_scope() {
  QLJS_ASSERT(!this->scopes_.empty());
  this->propagate_variable_uses_to_parent_scope(
      /*allow_variable_use_before_declaration=*/false,
      /*consume_arguments=*/false);
  this->propagate_variable_declarations_to_parent_scope();
  this->scopes_.pop();
}

void linter::visit_exit_function_scope() {
  QLJS_ASSERT(!this->scopes_.empty());
  this->propagate_variable_uses_to_parent_scope(
      /*allow_variable_use_before_declaration=*/true,
      /*consume_arguments=*/true);
  this->scopes_.pop();
}

void linter::visit_property_declaration(std::optional<identifier>) {}

void linter::visit_variable_declaration(identifier name, variable_kind kind) {
  this->declare_variable(
      /*scope=*/this->current_scope(),
      /*name=*/name,
      /*kind=*/kind,
      /*declared_scope=*/declared_variable_scope::declared_in_current_scope);
}

void linter::declare_variable(scope &scope, identifier name, variable_kind kind,
                              declared_variable_scope declared_scope) {
  if (declared_scope == declared_variable_scope::declared_in_descendant_scope) {
    QLJS_ASSERT(kind == variable_kind::_function ||
                kind == variable_kind::_var);
  }

  this->report_error_if_variable_declaration_conflicts_in_scope(
      scope, name, kind, declared_scope);

  const declared_variable *declared =
      scope.declared_variables.add_variable_declaration(name, kind,
                                                        declared_scope);

  auto erase_if = [](auto &variables, auto predicate) {
    variables.erase(
        std::remove_if(variables.begin(), variables.end(), predicate),
        variables.end());
  };
  erase_if(scope.variables_used, [&](const used_variable &used_var) {
    if (name.normalized_name() != used_var.name.normalized_name()) {
      return false;
    }
    if (kind == variable_kind::_function &&
        declared_scope ==
            declared_variable_scope::declared_in_descendant_scope &&
        used_var.kind == used_variable_kind::use) {
      this->error_reporter_->report(
          error_function_call_before_declaration_in_blocked_scope{used_var.name,
                                                                  name});
    }
    if (kind == variable_kind::_class || kind == variable_kind::_const ||
        kind == variable_kind::_let) {
      switch (used_var.kind) {
      case used_variable_kind::assignment:
        this->report_error_if_assignment_is_illegal(
            declared, used_var.name,
            /*is_assigned_before_declaration=*/true);
        break;
      case used_variable_kind::_typeof:
      case used_variable_kind::use:
        this->error_reporter_->report(
            error_variable_used_before_declaration{used_var.name, name});
        break;
      case used_variable_kind::_export:
        // Use before declaration is legal for variable exports.
        break;
      }
    }
    return true;
  });
  erase_if(scope.variables_used_in_descendant_scope,
           [&](const used_variable &used_var) {
             if (name.normalized_name() != used_var.name.normalized_name()) {
               return false;
             }
             switch (used_var.kind) {
             case used_variable_kind::assignment:
               this->report_error_if_assignment_is_illegal(
                   declared, used_var.name,
                   /*is_assigned_before_declaration=*/false);
               break;
             case used_variable_kind::_export:
               // TODO(strager): This shouldn't happen. export statements are
               // not allowed inside functions.
               break;
             case used_variable_kind::_typeof:
             case used_variable_kind::use:
               break;
             }
             return true;
           });
}

void linter::visit_variable_assignment(identifier name) {
  QLJS_ASSERT(!this->scopes_.empty());
  scope &current_scope = this->current_scope();
  const declared_variable *var = current_scope.declared_variables.find(name);
  if (var) {
    this->report_error_if_assignment_is_illegal(
        var, name, /*is_assigned_before_declaration=*/false);
  } else {
    current_scope.variables_used.emplace_back(name,
                                              used_variable_kind::assignment);
  }
}

void linter::visit_variable_export_use(identifier name) {
  this->visit_variable_use(name, used_variable_kind::_export);
}

void linter::visit_variable_typeof_use(identifier name) {
  this->visit_variable_use(name, used_variable_kind::_typeof);
}

void linter::visit_variable_use(identifier name) {
  this->visit_variable_use(name, used_variable_kind::use);
}

void linter::visit_variable_use(identifier name, used_variable_kind use_kind) {
  QLJS_ASSERT(!this->scopes_.empty());
  scope &current_scope = this->current_scope();
  bool variable_is_declared =
      current_scope.declared_variables.find(name) != nullptr;
  if (!variable_is_declared) {
    current_scope.variables_used.emplace_back(name, use_kind);
  }
}

void linter::visit_end_of_module() {
  // We expect only the module scope.
  QLJS_ASSERT(this->scopes_.size() == 1);

  linter::global_scope &global_scope = this->global_scope_;

  this->propagate_variable_uses_to_parent_scope(
      /*parent_scope=*/global_scope,
      /*allow_variable_use_before_declaration=*/false,
      /*consume_arguments=*/false);

  std::vector<identifier> typeof_variables;
  for (const used_variable &used_var : global_scope.variables_used) {
    if (used_var.kind == used_variable_kind::_typeof) {
      typeof_variables.emplace_back(used_var.name);
    }
  }
  for (const used_variable &used_var :
       global_scope.variables_used_in_descendant_scope) {
    if (used_var.kind == used_variable_kind::_typeof) {
      typeof_variables.emplace_back(used_var.name);
    }
  }
  auto is_variable_declared_by_typeof = [&](const used_variable &var) -> bool {
    return std::find_if(typeof_variables.begin(), typeof_variables.end(),
                        [&](const identifier &typeof_variable) {
                          return typeof_variable.normalized_name() ==
                                 var.name.normalized_name();
                        }) != typeof_variables.end();
  };
  auto is_variable_declared = [&](const used_variable &var) -> bool {
    return global_scope.declared_variables.find(var.name) ||
           is_variable_declared_by_typeof(var);
  };

  for (const used_variable &used_var : global_scope.variables_used) {
    if (!is_variable_declared(used_var)) {
      switch (used_var.kind) {
      case used_variable_kind::assignment:
        this->error_reporter_->report(
            error_assignment_to_undeclared_variable{used_var.name});
        break;
      case used_variable_kind::_export:
      case used_variable_kind::use:
        this->error_reporter_->report(
            error_use_of_undeclared_variable{used_var.name});
        break;
      case used_variable_kind::_typeof:
        // 'typeof foo' is often used to detect if the variable 'foo' is
        // declared. Do not report that the variable is undeclared.
        break;
      }
    }
  }
  for (const used_variable &used_var :
       global_scope.variables_used_in_descendant_scope) {
    if (!is_variable_declared(used_var)) {
      switch (used_var.kind) {
      case used_variable_kind::assignment:
        this->error_reporter_->report(
            error_assignment_to_undeclared_variable{used_var.name});
        break;
      // TODO(strager): Is 'default' correct here?
      default:
        this->error_reporter_->report(
            error_use_of_undeclared_variable{used_var.name});
        break;
      }
    }
  }
}

void linter::propagate_variable_uses_to_parent_scope(
    bool allow_variable_use_before_declaration, bool consume_arguments) {
  this->propagate_variable_uses_to_parent_scope(
      /*parent_scope=*/this->parent_scope(),
      /*allow_variable_use_before_declaration=*/
      allow_variable_use_before_declaration,
      /*consume_arguments=*/consume_arguments);
}

template <class Scope>
void linter::propagate_variable_uses_to_parent_scope(
    Scope &parent_scope, bool allow_variable_use_before_declaration,
    bool consume_arguments) {
  scope &current_scope = this->current_scope();

  auto is_current_scope_function_name = [&](const used_variable &var) {
    return current_scope.function_expression_declaration.has_value() &&
           current_scope.function_expression_declaration->name() ==
               var.name.normalized_name();
  };

  for (const used_variable &used_var : current_scope.variables_used) {
    QLJS_ASSERT(!current_scope.declared_variables.find(used_var.name));
    const declared_variable *var =
        parent_scope.declared_variables.find(used_var.name);
    if (var) {
      // This variable was declared in the parent scope. Don't propagate.
      if (used_var.kind == used_variable_kind::assignment) {
        this->report_error_if_assignment_is_illegal(
            var, used_var.name, /*is_assigned_before_declaration=*/false);
      }
    } else if (consume_arguments &&
               used_var.name.normalized_name() == u8"arguments") {
      // Treat this variable as declared in the current scope.
    } else if (is_current_scope_function_name(used_var)) {
      // Treat this variable as declared in the current scope.
    } else {
      (allow_variable_use_before_declaration
           ? parent_scope.variables_used_in_descendant_scope
           : parent_scope.variables_used)
          .emplace_back(used_var);
    }
  }
  current_scope.variables_used.clear();

  for (const used_variable &used_var :
       current_scope.variables_used_in_descendant_scope) {
    const declared_variable *var =
        parent_scope.declared_variables.find(used_var.name);
    if (var) {
      // This variable was declared in the parent scope. Don't propagate.
      if (used_var.kind == used_variable_kind::assignment) {
        this->report_error_if_assignment_is_illegal(
            var, used_var.name, /*is_assigned_before_declaration=*/false);
      }
    } else if (is_current_scope_function_name(used_var)) {
      // Treat this variable as declared in the current scope.
    } else {
      parent_scope.variables_used_in_descendant_scope.emplace_back(used_var);
    }
  }
  current_scope.variables_used_in_descendant_scope.clear();
}

void linter::propagate_variable_declarations_to_parent_scope() {
  scope &current_scope = this->current_scope();
  scope &parent_scope = this->parent_scope();

  for (const declared_variable &var : current_scope.declared_variables) {
    if (var.kind() == variable_kind::_function ||
        var.kind() == variable_kind::_var) {
      QLJS_ASSERT(!var.is_global_variable());
      this->declare_variable(
          /*scope=*/parent_scope,
          /*name=*/var.declaration(),
          /*kind=*/var.kind(),
          /*declared_scope=*/
          declared_variable_scope::declared_in_descendant_scope);
    }
  }
}

void linter::report_error_if_assignment_is_illegal(
    const linter::declared_variable *var, const identifier &assignment,
    bool is_assigned_before_declaration) const {
  switch (var->kind()) {
  case variable_kind::_const:
  case variable_kind::_import:
    if (var->is_global_variable()) {
      this->error_reporter_->report(
          error_assignment_to_const_global_variable{assignment});
    } else {
      if (is_assigned_before_declaration) {
        this->error_reporter_->report(
            error_assignment_to_const_variable_before_its_declaration{
                var->declaration(), assignment, var->kind()});
      } else {
        this->error_reporter_->report(error_assignment_to_const_variable{
            var->declaration(), assignment, var->kind()});
      }
    }
    break;
  case variable_kind::_catch:
  case variable_kind::_class:
  case variable_kind::_function:
  case variable_kind::_let:
  case variable_kind::_parameter:
  case variable_kind::_var:
    if (is_assigned_before_declaration) {
      this->error_reporter_->report(
          error_assignment_before_variable_declaration{
              .assignment = assignment, .declaration = var->declaration()});
    }
    break;
  }
}

void linter::report_error_if_variable_declaration_conflicts_in_scope(
    const linter::scope &scope, identifier name, variable_kind kind,
    linter::declared_variable_scope declaration_scope) const {
  const declared_variable *already_declared_variable =
      scope.declared_variables.find(name);
  if (already_declared_variable) {
    using vk = variable_kind;
    vk other_kind = already_declared_variable->kind();

    switch (other_kind) {
    case vk::_catch:
      QLJS_ASSERT(kind != vk::_import);
      QLJS_ASSERT(kind != vk::_parameter);
      break;
    case vk::_class:
    case vk::_const:
    case vk::_function:
    case vk::_let:
    case vk::_var:
      QLJS_ASSERT(kind != vk::_catch);
      QLJS_ASSERT(kind != vk::_parameter);
      break;
    case vk::_parameter:
      QLJS_ASSERT(kind != vk::_catch);
      QLJS_ASSERT(kind != vk::_import);
      break;
    case vk::_import:
      break;
    }

    bool redeclaration_ok =
        (other_kind == vk::_function && kind == vk::_parameter) ||
        (other_kind == vk::_function && kind == vk::_function) ||
        (other_kind == vk::_parameter && kind == vk::_function) ||
        (other_kind == vk::_var && kind == vk::_function) ||
        (other_kind == vk::_parameter && kind == vk::_parameter) ||
        (other_kind == vk::_catch && kind == vk::_var) ||
        (other_kind == vk::_function && kind == vk::_var) ||
        (other_kind == vk::_parameter && kind == vk::_var) ||
        (other_kind == vk::_var && kind == vk::_var) ||
        (other_kind == vk::_function &&
         already_declared_variable->declaration_scope() ==
             declared_variable_scope::declared_in_descendant_scope) ||
        (kind == vk::_function &&
         declaration_scope ==
             declared_variable_scope::declared_in_descendant_scope);
    if (!redeclaration_ok) {
      if (already_declared_variable->is_global_variable()) {
        this->error_reporter_->report(
            error_redeclaration_of_global_variable{name});
      } else {
        this->error_reporter_->report(error_redeclaration_of_variable{
            name, already_declared_variable->declaration()});
      }
    }
  }
}

const linter::declared_variable *
linter::declared_variable_set::add_variable_declaration(
    identifier name, variable_kind kind,
    declared_variable_scope declared_scope) {
  this->variables_.emplace_back(
      declared_variable::make_local(name, kind, declared_scope));
  return &this->variables_.back();
}

void linter::declared_variable_set::add_predefined_variable_declaration(
    const char8 *name, variable_kind kind) {
  this->variables_.emplace_back(declared_variable::make_global(name, kind));
}

const linter::declared_variable *linter::declared_variable_set::find(
    identifier name) const noexcept {
  string8_view name_view = name.normalized_name();
  for (const declared_variable &var : this->variables_) {
    if (var.name() == name_view) {
      return &var;
    }
  }
  return nullptr;
}

void linter::declared_variable_set::clear() noexcept {
  this->variables_.clear();
}

std::vector<linter::declared_variable>::const_iterator
linter::declared_variable_set::begin() const noexcept {
  return this->variables_.cbegin();
}

std::vector<linter::declared_variable>::const_iterator
linter::declared_variable_set::end() const noexcept {
  return this->variables_.cend();
}

void linter::scope::clear() {
  this->declared_variables.clear();
  this->variables_used.clear();
  this->variables_used_in_descendant_scope.clear();
  this->function_expression_declaration.reset();
}

linter::scopes::scopes() {
  this->push();  // module_scope
}

linter::scope &linter::scopes::module_scope() noexcept {
  return this->scopes_[0];
}

linter::scope &linter::scopes::current_scope() noexcept {
  QLJS_ASSERT(!this->empty());
  return this->scopes_[narrow_cast<std::size_t>(this->size()) - 1];
}

linter::scope &linter::scopes::parent_scope() noexcept {
  QLJS_ASSERT(this->size() >= 2);
  return this->scopes_[narrow_cast<std::size_t>(this->size()) - 2];
}

linter::scope &linter::scopes::push() {
  bool full = this->scope_count_ == narrow_cast<int>(this->scopes_.size());
  scope *s;
  if (full) {
    s = &this->scopes_.emplace_back();
  } else {
    s = &this->scopes_[narrow_cast<std::size_t>(this->scope_count_)];
    s->clear();
  }
  this->scope_count_ += 1;
  return *s;
}

void linter::scopes::pop() {
  QLJS_ASSERT(!this->empty());
  this->scope_count_ -= 1;
}

bool linter::scopes::empty() const noexcept { return this->scope_count_ == 0; }

int linter::scopes::size() const noexcept { return this->scope_count_; }
}

// quick-lint-js finds bugs in JavaScript programs.
// Copyright (C) 2020  Matthew Glazar
//
// This file is part of quick-lint-js.
//
// quick-lint-js is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// quick-lint-js is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with quick-lint-js.  If not, see <https://www.gnu.org/licenses/>.
