/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/compiler/analysis/function_scope.h"

#include "hphp/compiler/analysis/analysis_result.h"
#include "hphp/compiler/analysis/class_scope.h"
#include "hphp/compiler/analysis/code_error.h"
#include "hphp/compiler/analysis/file_scope.h"
#include "hphp/compiler/analysis/variable_table.h"
#include "hphp/compiler/expression/array_pair_expression.h"
#include "hphp/compiler/expression/closure_expression.h"
#include "hphp/compiler/expression/constant_expression.h"
#include "hphp/compiler/expression/expression_list.h"
#include "hphp/compiler/expression/function_call.h"
#include "hphp/compiler/expression/modifier_expression.h"
#include "hphp/compiler/expression/parameter_expression.h"
#include "hphp/compiler/expression/scalar_expression.h"
#include "hphp/compiler/expression/unary_op_expression.h"
#include "hphp/compiler/option.h"
#include "hphp/compiler/parser/parser.h"
#include "hphp/compiler/statement/exp_statement.h"
#include "hphp/compiler/statement/method_statement.h"
#include "hphp/compiler/statement/statement_list.h"

#include "hphp/parser/hphp.tab.hpp"

#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/variable-serializer.h"
#include "hphp/runtime/base/zend-string.h"

#include "hphp/util/atomic.h"
#include "hphp/util/logger.h"

#include <folly/Conv.h>
#include <utility>
#include <vector>

using namespace HPHP;

///////////////////////////////////////////////////////////////////////////////

FunctionScope::FunctionScope(AnalysisResultConstRawPtr ar, bool method,
                             const std::string &originalName, StatementPtr stmt,
                             bool reference, int minParam, int numDeclParam,
                             ModifierExpressionPtr modifiers,
                             int attribute, const std::string &docComment,
                             FileScopePtr file,
                             const std::vector<UserAttributePtr> &attrs,
                             bool inPseudoMain /* = false */)
    : BlockScope(originalName, docComment, stmt, BlockScope::FunctionScope),
      m_minParam(minParam), m_numDeclParams(numDeclParam),
      m_attribute(attribute), m_modifiers(modifiers), m_hasVoid(false),
      m_method(method), m_refReturn(reference), m_virtual(false),
      m_hasOverride(false),
      m_pseudoMain(inPseudoMain),
      m_system(false),
      m_containsThis(false), m_containsBareThis(0),
      m_generator(false),
      m_async(false),
      m_noLSB(false), m_nextLSB(false), m_localRedeclaring(false),
      m_fromTrait(false) {
  init(ar);

  for (unsigned i = 0; i < attrs.size(); ++i) {
    if (m_userAttributes.find(attrs[i]->getName()) != m_userAttributes.end()) {
      attrs[i]->parseTimeFatal(file,
                               Compiler::DeclaredAttributeTwice,
                               "Redeclared attribute %s",
                               attrs[i]->getName().c_str());
    }
    m_userAttributes[attrs[i]->getName()] = attrs[i]->getExp();
  }

  // Try to find if the function have __Native("VariadicByRef")
  auto params = getUserAttributeParams("__native");
  for (auto &param : params) {
    if (param->getString().compare("VariadicByRef") == 0) {
      setVariableArgument(1);
      break;
    }
  }

  if (isNative()) {
    m_coerceMode |= hasUserAttr("__ParamCoerceModeFalse")
      ? AttrParamCoerceModeFalse
      : AttrParamCoerceModeNull;
  }
}

FunctionScope::FunctionScope(FunctionScopePtr orig,
                             AnalysisResultConstRawPtr ar,
                             const std::string &originalName,
                             StatementPtr stmt,
                             ModifierExpressionPtr modifiers,
                             bool user)
    : BlockScope(originalName, orig->m_docComment, stmt,
                 BlockScope::FunctionScope),
      m_minParam(orig->m_minParam), m_numDeclParams(orig->m_numDeclParams),
      m_attribute(orig->m_attribute), m_modifiers(modifiers),
      m_userAttributes(orig->m_userAttributes), m_hasVoid(orig->m_hasVoid),
      m_method(orig->m_method), m_refReturn(orig->m_refReturn),
      m_virtual(orig->m_virtual), m_hasOverride(orig->m_hasOverride),
      m_pseudoMain(orig->m_pseudoMain),
      m_system(!user),
      m_containsThis(orig->m_containsThis),
      m_containsBareThis(orig->m_containsBareThis),
      m_generator(orig->m_generator),
      m_async(orig->m_async),
      m_noLSB(orig->m_noLSB),
      m_nextLSB(orig->m_nextLSB), m_localRedeclaring(orig->m_localRedeclaring),
      m_fromTrait(orig->m_fromTrait) {
  init(ar);
  setParamCounts(ar, m_minParam, m_numDeclParams);
}

void FunctionScope::init(AnalysisResultConstRawPtr /*ar*/) {
  m_dynamicInvoke = false;

  // FileScope's flags are from parser, but VariableTable has more flags
  // coming from type inference phase. So we are tranferring these flags
  // just for better modularization between FileScope and VariableTable.
  if (m_attribute & FileScope::ContainsDynamicVariable) {
    m_variables->setAttribute(VariableTable::ContainsDynamicVariable);
  }
  if (m_attribute & FileScope::ContainsLDynamicVariable) {
    m_variables->setAttribute(VariableTable::ContainsLDynamicVariable);
  }
  if (m_attribute & FileScope::ContainsUnset) {
    m_variables->setAttribute(VariableTable::ContainsUnset);
  }

  if (!m_method && RuntimeOption::DynamicInvokeFunctions.count(m_scopeName)) {
    setDynamicInvoke();
  }
  if (m_modifiers) {
    m_virtual = m_modifiers->isAbstract();
  }

  if (m_stmt) {
    auto stmt = dynamic_pointer_cast<MethodStatement>(m_stmt);
    StatementListPtr stmts = stmt->getStmts();
    if (stmts) {
      for (int i = 0; i < stmts->getCount(); i++) {
        StatementPtr stmt = (*stmts)[i];
        stmt->setFileLevel();
        if (stmt->is(Statement::KindOfExpStatement)) {
          auto expStmt = dynamic_pointer_cast<ExpStatement>(stmt);
          ExpressionPtr exp = expStmt->getExpression();
          exp->setTopLevel();
        }
      }
    }
  }
}

FunctionScope::FunctionScope(bool method, const std::string &name,
                             bool reference)
    : BlockScope(name, "", StatementPtr(), BlockScope::FunctionScope),
      m_minParam(0), m_numDeclParams(0), m_attribute(0),
      m_modifiers(ModifierExpressionPtr()), m_hasVoid(false),
      m_method(method), m_refReturn(reference), m_virtual(false),
      m_hasOverride(false),
      m_pseudoMain(false),
      m_system(true),
      m_containsThis(false), m_containsBareThis(0),
      m_generator(false),
      m_async(false),
      m_noLSB(false), m_nextLSB(false), m_localRedeclaring(false),
      m_fromTrait(false) {
  m_dynamicInvoke = false;
  if (!method && RuntimeOption::DynamicInvokeFunctions.count(name)) {
    setDynamicInvoke();
  }
}

void FunctionScope::setDynamicInvoke() {
  m_dynamicInvoke = true;
}

void FunctionScope::setParamCounts(AnalysisResultConstRawPtr, int minParam,
                                   int numDeclParam) {
  if (minParam >= 0) {
    m_minParam = minParam;
    m_numDeclParams = numDeclParam;
  } else {
    assert(numDeclParam == minParam);
  }
  assert(m_minParam >= 0 && m_numDeclParams >= m_minParam);
  assert(IMPLIES(hasVariadicParam(), m_numDeclParams > 0));
  if (m_numDeclParams > 0) {
    m_paramNames.resize(m_numDeclParams);
    m_refs.resize(m_numDeclParams);

    if (m_stmt) {
      auto stmt = dynamic_pointer_cast<MethodStatement>(m_stmt);
      ExpressionListPtr params = stmt->getParams();

      for (int i = 0; i < m_numDeclParams; i++) {
        if (stmt->isRef(i)) m_refs[i] = true;

        auto param = dynamic_pointer_cast<ParameterExpression>((*params)[i]);
        m_paramNames[i] = param->getName();
      }
      assert(m_paramNames.size() == m_numDeclParams);
    }
  }
}

bool FunctionScope::hasUserAttr(const char *attr) const {
  return m_userAttributes.find(attr) != m_userAttributes.end();
}

bool FunctionScope::isParamCoerceMode() const {
  return m_coerceMode & (AttrParamCoerceModeNull | AttrParamCoerceModeFalse);
}

bool FunctionScope::isPublic() const {
  return m_modifiers && m_modifiers->isPublic();
}

bool FunctionScope::isProtected() const {
  return m_modifiers && m_modifiers->isProtected();
}

bool FunctionScope::isPrivate() const {
  return m_modifiers && m_modifiers->isPrivate();
}

bool FunctionScope::isStatic() const {
  return m_modifiers && m_modifiers->isStatic();
}

bool FunctionScope::isAbstract() const {
  return m_modifiers && m_modifiers->isAbstract();
}

bool FunctionScope::isNative() const {
  return hasUserAttr("__native");
}

bool FunctionScope::isFinal() const {
  return m_modifiers && m_modifiers->isFinal();
}

bool FunctionScope::hasVariadicParam() const {
  return (m_attribute & FileScope::VariadicArgumentParam);
}

bool FunctionScope::allowsVariableArguments() const {
  return hasVariadicParam() || usesVariableArgumentFunc();
}

bool FunctionScope::usesVariableArgumentFunc() const {
  return m_attribute & FileScope::VariableArgument;
}

bool FunctionScope::hasRefVariadicParam() const {
  return m_attribute & FileScope::RefVariadicArgumentParam;
}

bool FunctionScope::isReferenceVariableArgument() const {
  bool res = m_attribute & FileScope::ReferenceVariableArgument;
  // If this method returns true, then usesVariableArgumentFunc() must also
  // return true.
  assert(!res || usesVariableArgumentFunc());
  return res;
}

bool FunctionScope::needsFinallyLocals() const {
  bool res = (m_attribute & FileScope::NeedsFinallyLocals);
  return res;
}

bool FunctionScope::mayContainThis() {
  return inPseudoMain() || getContainingClass() ||
    (isClosure() && !m_modifiers->isStatic());
}

bool FunctionScope::isClosure() const {
  return ParserBase::IsClosureName(getScopeName());
}

bool FunctionScope::isLambdaClosure() const {
  if (!isClosure()) return false;
  auto const stmt = static_pointer_cast<MethodStatement>(getStmt());
  return stmt->getContainingClosure()->type() == ClosureType::Short;
}

void FunctionScope::setVariableArgument(int reference) {
  m_attribute |= FileScope::VariableArgument;
  if (reference) {
    m_attribute |= FileScope::ReferenceVariableArgument;
  }
}

bool FunctionScope::isFoldable() const {
  // Systemlib (PHP&HNI) builtins
  auto f = Unit::lookupFunc(String(getScopeName()).get());
  return f && f->isFoldable();
}

void FunctionScope::setContainsThis(bool f /* = true */) {
  m_containsThis = f;
}

void FunctionScope::setContainsBareThis(bool f, bool ref /* = false */) {
  if (f) {
    m_containsBareThis |= ref ? 2 : 1;
  } else {
    m_containsBareThis = 0;
  }
}

bool FunctionScope::isNamed(const char* n) const {
  return !strcasecmp(getScopeName().c_str(), n);
}

bool FunctionScope::isMagic() const {
  return m_scopeName.size() >= 2 &&
    m_scopeName[0] == '_' && m_scopeName[1] == '_';
}

bool FunctionScope::needsLocalThis() const {
  return containsBareThis() &&
    (inPseudoMain() ||
     containsRefThis() ||
     isStatic() ||
     getVariables()->getAttribute(
       VariableTable::ContainsDynamicVariable));
}

static std::string s_empty;
const std::string &FunctionScope::getOriginalName() const {
  if (m_pseudoMain) return s_empty;
  return m_scopeName;
}

std::string FunctionScope::getOriginalFullName() const {
  if (m_stmt) {
    auto stmt = dynamic_pointer_cast<MethodStatement>(m_stmt);
    return stmt->getOriginalFullName();
  }
  return m_scopeName;
}

///////////////////////////////////////////////////////////////////////////////

bool FunctionScope::isRefParam(int index) const {
  assert(index >= 0 && index < (int)m_refs.size());
  return m_refs[index];
}

void FunctionScope::setRefParam(int index) {
  assert(index >= 0 && index < (int)m_refs.size());
  m_refs[index] = true;
}

const std::string &FunctionScope::getParamName(int index) const {
  assert(index >= 0 && index < (int)m_paramNames.size());
  return m_paramNames[index];
}

std::vector<ScalarExpressionPtr> FunctionScope::getUserAttributeParams(
    const std::string& key) {

  std::vector<ScalarExpressionPtr> ret;
  auto native = m_userAttributes.find(key);
  if (native == m_userAttributes.end()) {
    return ret;
  }

  auto arrayExp = static_pointer_cast<UnaryOpExpression>(native->second);
  if (!arrayExp->getExpression()) {
    return ret;
  }

  auto memberExp = static_pointer_cast<ExpressionList>(
                     arrayExp->getExpression());

  for (int i = 0; i < memberExp->getCount(); i++) {
    auto pairExp = dynamic_pointer_cast<ArrayPairExpression>((*memberExp)[i]);
    if (!pairExp) {
      continue;
    }

    auto value = dynamic_pointer_cast<ScalarExpression>(pairExp->getValue());
    if (value) {
      ret.push_back(value);
    }
  }

  return ret;
}

std::string FunctionScope::getDocName() const {
  return getScopeName();
}

std::string FunctionScope::getDocFullName() const {
  FunctionScope *self = const_cast<FunctionScope*>(this);
  auto const& docName = getDocName();
  if (ClassScopeRawPtr cls = self->getContainingClass()) {
    return cls->getDocName() + std::string("::") + docName;
  }
  return docName;
}

///////////////////////////////////////////////////////////////////////////////

void FunctionScope::outputPHP(CodeGenerator &cg, AnalysisResultPtr ar) {
  BlockScope::outputPHP(cg, ar);
}

void FunctionScope::serialize(JSON::CodeError::OutputStream &out) const {
  JSON::CodeError::MapStream ms(out);
  int vis = 0;
  if (isPublic()) vis = ClassScope::Public;
  else if (isProtected()) vis = ClassScope::Protected;
  else if (isPrivate()) vis = ClassScope::Protected;

  int mod = 0;
  if (isAbstract()) mod = ClassScope::Abstract;
  else if (isFinal()) mod = ClassScope::Final;

  ms.add("minArgs", m_minParam)
    .add("maxArgs", m_numDeclParams)
    .add("varArgs", allowsVariableArguments())
    .add("static", isStatic())
    .add("modifier", mod)
    .add("visibility", vis)
    .add("argIsRef", m_refs)
    .done();
}

void FunctionScope::serialize(JSON::DocTarget::OutputStream &out) const {
  JSON::DocTarget::MapStream ms(out);

  ms.add("name", getDocName());
  ms.add("line", getStmt() ? getStmt()->line0() : 0);
  ms.add("docs", m_docComment);

  int mods = 0;
  if (isPublic())    mods |= AttrPublic;
  if (isProtected()) mods |= AttrProtected;
  if (isPrivate())   mods |= AttrPrivate;
  if (isStatic())    mods |= AttrStatic;
  if (isFinal())     mods |= AttrFinal;
  if (isAbstract())  mods |= AttrAbstract;
  ms.add("modifiers", mods);

  ms.add("refreturn", isRefReturn());

  std::vector<SymParamWrapper> paramSymbols;
  auto const limit = getDeclParamCount();
  for (int i = 0; i < limit; i++) {
    auto const& name = getParamName(i);
    const Symbol *sym = getVariables()->getSymbol(name);
    assert(sym && sym->isParameter());
    paramSymbols.push_back(SymParamWrapper(sym));
  }
  ms.add("parameters", paramSymbols);

  ms.done();
}

void FunctionScope::recordParams() {
  auto const variables = getVariables();
  auto limit = getDeclParamCount();
  for (int i = 0; i < limit; i++) {
    variables->addParam(getParamName(i),
                        AnalysisResultPtr(), ConstructPtr());
  }
}
