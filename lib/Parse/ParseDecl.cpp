//===--- ParseDecl.cpp - Swift Language Parser for Declarations -----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Declaration Parsing and AST Building
//
//===----------------------------------------------------------------------===//

#include "swift/Parse/Lexer.h"
#include "Parser.h"
#include "swift/AST/Diagnostics.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PathV2.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Twine.h"
using namespace swift;


/// parseTranslationUnit - Main entrypoint for the parser.
///   translation-unit:
///     stmt-brace-item*
TranslationUnit *Parser::parseTranslationUnit() {
  // Prime the lexer.
  consumeToken();
  SourceLoc FileStartLoc = Tok.getLoc();

  StringRef ModuleName
    = llvm::sys::path::stem(Buffer->getBufferIdentifier());
  TranslationUnit *TU =
    new (Context) TranslationUnit(Context.getIdentifier(ModuleName),
                                  Context);
  CurDeclContext = TU;
  
  // Parse the body of the file.
  SmallVector<ExprStmtOrDecl, 128> Items;
  parseBraceItemList(Items, true);

  // Process the end of the translation unit.
  SourceLoc FileEnd = Tok.getLoc();
  
  // First thing, we transform the body into a brace expression.
  TU->Body = BraceStmt::create(Context, FileStartLoc, Items, FileEnd);
    
  // Do a prepass over the declarations to make sure they have basic sanity and
  // to find the list of top-level value declarations.
  for (auto Elt : TU->Body->getElements()) {
    if (!Elt.is<Decl*>()) continue;
    
    Decl *D = Elt.get<Decl*>();
    
    // If any top-level value decl has an unresolved type, then it is erroneous.
    // It is not valid to have something like "var x = 4" at the top level, all
    // types must be explicit here.
    ValueDecl *VD = dyn_cast<ValueDecl>(D);
    if (VD == 0) continue;
    
    // FIXME: This can be better handled in the various ActOnDecl methods when
    // they get passed in a parent context decl.
  }
  
  // Verify that any forward declared types were ultimately defined.
  // TODO: Move this to name binding!
  SmallVector<TypeAliasDecl*, 8> UnresolvedTypeList;
  for (TypeAliasDecl *Decl : ScopeInfo.getUnresolvedTypeList()) {
    if (!Decl->hasUnderlyingType())
      UnresolvedTypeList.push_back(Decl);
  }
  
  TU->UnresolvedTypesForParser = Context.AllocateCopy(UnresolvedTypeList);
  TU->UnresolvedScopedTypesForParser =
    Context.AllocateCopy(ScopeInfo.getUnresolvedScopedTypeList());
  return TU;
}

static bool isInfixAttr(Token &Tok, Associativity &Assoc) {
  if (Tok.getText() == "infix_left") {
    Assoc = Associativity::Left;
    return true;
  } else if (Tok.getText() == "infix_right") {
    Assoc = Associativity::Right;
    return true;
  } else if (Tok.getText() == "infix") {
    Assoc = Associativity::None;
    return true;
  } else {
    return false;
  }
}

/// parseAttribute
///   attribute:
///     'infix' '=' numeric_constant
///     'infix_left' '=' numeric_constant
///     'infix_right' '=' numeric_constant
///     'unary'
bool Parser::parseAttribute(DeclAttributes &Attributes) {
  // infix attributes.
  Associativity Assoc;
  if (Tok.is(tok::identifier) && isInfixAttr(Tok, Assoc)) {
    if (Attributes.isInfix())
      diagnose(Tok, diag::duplicate_attribute, Tok.getText());
    consumeToken(tok::identifier);

    // The default precedence is 100.
    Attributes.Infix = InfixData(100, Assoc);
    
    if (consumeIf(tok::equal)) {
      SourceLoc PrecLoc = Tok.getLoc();
      StringRef Text = Tok.getText();
      if (!parseToken(tok::numeric_constant, diag::expected_precedence_value)){
        long long Value;
        if (Text.getAsInteger(10, Value) || Value > 255 || Value < 0)
          diagnose(PrecLoc, diag::invalid_precedence, Text);
        else
          Attributes.Infix = InfixData(Value, Assoc);
      } else {
        // FIXME: I'd far rather that we describe this in terms of some
        // list structure in the caller. This feels too ad hoc.
        skipUntil(tok::r_square, tok::comma);
      }
    }

    return false;
  }

  if (Tok.is(tok::identifier))   
    diagnose(Tok, diag::unknown_attribute, Tok.getText());
  else
    diagnose(Tok, diag::expected_attribute_name);
  skipUntil(tok::r_square);
  return true;
}

/// parsePresentAttributeList
///   attribute-list:
///     attribute-list-present?
///
///   attribute-list-present:
///     '[' ']'
///     '[' attribute (',' attribute)* ']'
void Parser::parseAttributeListPresent(DeclAttributes &Attributes) {
  Attributes.LSquareLoc = consumeToken(tok::l_square);
  
  // If this is an empty attribute list, consume it and return.
  if (Tok.is(tok::r_square)) {
    Attributes.RSquareLoc = consumeToken(tok::r_square);
    return;
  }
  
  bool HadError = parseAttribute(Attributes);
  while (Tok.is(tok::comma)) {
    consumeToken(tok::comma);
    HadError |= parseAttribute(Attributes);
  }

  Attributes.RSquareLoc = Tok.getLoc();
  if (consumeIf(tok::r_square))
    return;
  
  // Otherwise, there was an error parsing the attribute list.  If we already
  // reported an error, skip to a ], otherwise report the error.
  if (!HadError)
    parseMatchingToken(tok::r_square, Attributes.RSquareLoc,
                       diag::expected_in_attribute_list, 
                       Attributes.LSquareLoc, diag::opening_bracket);
  skipUntil(tok::r_square);
  consumeIf(tok::r_square);
}

/// parseDecl - Parse a single syntactic declaration and return a list of decl
/// ASTs.  This can return multiple results for var decls that bind to multiple
/// values, structs that define a struct decl and a constructor, etc.
///
/// This method returns true on a parser error that requires recovery.
///
///   decl:
///     decl-typealias
///     decl-extension
///     decl-var
///     decl-func
///     decl-func-scoped
///     decl-oneof
///     decl-struct
///     decl-import  [[Only if AllowImportDecl = true]]
///
bool Parser::parseDecl(SmallVectorImpl<Decl*> &Entries, unsigned Flags) {
  unsigned EntryStart = Entries.size();
  bool HadParseError = false;
  switch (Tok.getKind()) {
  default:
    diagnose(Tok, diag::expected_decl);
    HadParseError = true;
    break;
  case tok::kw_import:
    Entries.push_back(parseDeclImport());
    break;
  case tok::kw_extension:
    Entries.push_back(parseDeclExtension());
    break;
  case tok::kw_var:
    HadParseError = parseDeclVar(Entries);
    break;
  case tok::kw_typealias:
    Entries.push_back(parseDeclTypeAlias());
    break;
  case tok::kw_oneof:
    Entries.push_back(parseDeclOneOf());
    break;
  case tok::kw_struct:
    HadParseError = parseDeclStruct(Entries);
    break;
  case tok::kw_protocol:
    Entries.push_back(parseDeclProtocol());
    break;
  case tok::kw_func:
    Entries.push_back(parseDeclFunc());
    break;
  }
  
  // If we got back a null pointer, then a parse error happened.
  if (Entries.back() == 0) {
    Entries.pop_back();
    HadParseError = true;
  }

  // Validate the new entries.
  for (unsigned i = EntryStart, e = Entries.size(); i != e; ++i) {
    Decl *D = Entries[i];

    // FIXME: Mark decls erroneous.
    if (isa<ImportDecl>(D) && !(Flags & PD_AllowImport))
      diagnose(D->getLocStart(), diag::import_inner_scope);
    if (isa<VarDecl>(D) && (Flags & PD_DisallowVar)) {
      diagnose(D->getLocStart(), diag::disallowed_var_decl);
    } else if (NamedDecl *ND = dyn_cast<NamedDecl>(D)) {
      if (ND->isOperator() && (Flags & PD_DisallowOperators))
        diagnose(ND->getLocStart(), diag::operator_in_decl);
    }
  }
  
  return HadParseError;
}


/// parseDeclImport - Parse an 'import' declaration, returning null (and doing
/// no token skipping) on error.
///
///   decl-import:
///      'import' attribute-list? identifier ('.' identifier)*
///
Decl *Parser::parseDeclImport() {
  SourceLoc ImportLoc = consumeToken(tok::kw_import);
  
  DeclAttributes Attributes;
  parseAttributeList(Attributes);
  
  SmallVector<std::pair<Identifier, SourceLoc>, 8> ImportPath(1);
  ImportPath.back().second = Tok.getLoc();
  if (parseIdentifier(ImportPath.back().first,diag::decl_expected_module_name))
    return 0;
  
  while (consumeIf(tok::period)) {
    ImportPath.push_back(std::make_pair(Identifier(), Tok.getLoc()));
    if (parseIdentifier(ImportPath.back().first,
                        diag::expected_identifier_in_decl, "import"))
      return 0;
  }
  
  if (!Attributes.empty())
    diagnose(Attributes.LSquareLoc, diag::import_attributes);
  
  return ImportDecl::create(Context, CurDeclContext, ImportLoc, ImportPath);
}


/// parseDeclExtension - Parse an 'extension' declaration.
///   extension:
///    'extension' type-identifier '{' decl* '}'
///
Decl *Parser::parseDeclExtension() {
  SourceLoc ExtensionLoc = consumeToken(tok::kw_extension);

  Type Ty;
  SourceLoc LBLoc, RBLoc;
  if (parseTypeIdentifier(Ty) ||
      parseToken(tok::l_brace, LBLoc, diag::expected_lbrace_oneof_type))
    return 0;
  
  // Parse the body as a series of decls.
  // FIXME: Need to diagnose invalid members at Sema time!
  SmallVector<Decl*, 8> MemberDecls;
  while (Tok.isNot(tok::r_brace) && Tok.isNot(tok::eof)) {
    if (parseDecl(MemberDecls, PD_Default))
      skipUntilDeclRBrace();
  }

  parseMatchingToken(tok::r_brace, RBLoc, diag::expected_rbrace_extension,
                     LBLoc, diag::opening_brace);

  return new (Context) ExtensionDecl(ExtensionLoc, Ty,
                                     Context.AllocateCopy(MemberDecls),
                                     CurDeclContext);
}

/// parseVarName
///   var-name:
///     identifier
///     '(' ')'
///     '(' name (',' name)* ')'
bool Parser::parseVarName(DeclVarName &Name) {
  // Single name case.
  if (Tok.is(tok::identifier) || Tok.is(tok::oper)) {
    SourceLoc IdLoc = Tok.getLoc();
    Identifier Id = Context.getIdentifier(Tok.getText());
    consumeToken();
    Name = DeclVarName(Id, IdLoc);
    return false;
  }
  
  if (Tok.isNot(tok::l_paren) && Tok.isNot(tok::l_paren_space)) {
    diagnose(Tok, diag::expected_lparen_var_name);
    return true;
  }
  
  SourceLoc LPLoc = consumeToken();
  
  SmallVector<DeclVarName*, 8> ChildNames;
  
  if (Tok.isNot(tok::r_paren)) {
    do {
      DeclVarName *Elt = new (Context) DeclVarName();
      if (parseVarName(*Elt)) return true;
      ChildNames.push_back(Elt);
    } while (consumeIf(tok::comma));
  }

  SourceLoc RPLoc;
  parseMatchingToken(tok::r_paren, RPLoc, diag::expected_rparen_var_name,
                     LPLoc, diag::opening_paren);

  Name = DeclVarName(LPLoc, Context.AllocateCopy(ChildNames), RPLoc);
  return false;
}


/// parseDeclTypeAlias
///   decl-typealias:
///     'typealias' identifier ':' type
TypeAliasDecl *Parser::parseDeclTypeAlias() {
  SourceLoc TypeAliasLoc = consumeToken(tok::kw_typealias);
  
  Identifier Id;
  Type Ty;
  if (parseIdentifier(Id, diag::expected_identifier_in_decl, "typealias") ||
      parseToken(tok::colon, diag::expected_colon_in_typealias) ||
      parseType(Ty, diag::expected_type_in_typealias))
    return 0;

  return ScopeInfo.addTypeAliasToScope(TypeAliasLoc, Id, Ty);
}


/// AddElementNamesForVarDecl - This recursive function walks a name specifier
/// adding ElementRefDecls for the named subcomponents and checking that types
/// match up correctly.
void Parser::actOnVarDeclName(const DeclVarName *Name,
                              SmallVectorImpl<unsigned> &AccessPath,
                              VarDecl *VD, SmallVectorImpl<Decl*> &Decls) {
  if (Name->isSimple()) {
    // If this is a leaf name, create a ElementRefDecl with the specified
    // access path.
    Type Ty = ElementRefDecl::getTypeForPath(VD->getType(), AccessPath);
    
    // If the type of the path is obviously invalid, diagnose it now and refuse
    // to create the decl.  The most common result here is DependentType, which
    // allows type checking to resolve this later.
    if (Ty.isNull()) {
      diagnose(Name->getLocation(), diag::invalid_index_in_var_name_path,
               Name->getIdentifier(), VD->getType());
      return;
    }
    
    // Create the decl for this name and add it to the current scope.
    ElementRefDecl *ERD =
      new (Context) ElementRefDecl(VD, Name->getLocation(),
                                   Name->getIdentifier(),
                                   Context.AllocateCopy(AccessPath), Ty,
                                   CurDeclContext);
    Decls.push_back(ERD);
    ScopeInfo.addToScope(ERD);
    return;
  }
  
  AccessPath.push_back(0);
  unsigned Index = 0;
  for (auto Element : Name->getElements()) {
    AccessPath.back() = Index++;
    actOnVarDeclName(Element, AccessPath, VD, Decls);
  }
  AccessPath.pop_back();
}

/// parseDeclVar - Parse a 'var' declaration, returning null (and doing no
/// token skipping) on error.
///
///   decl-var:
///      'var' attribute-list? var-name value-specifier
bool Parser::parseDeclVar(SmallVectorImpl<Decl*> &Decls) {
  SourceLoc VarLoc = consumeToken(tok::kw_var);
  
  DeclAttributes Attributes;
  parseAttributeList(Attributes);

  DeclVarName VarName;
  if (parseVarName(VarName)) return true;
  
  Type Ty;
  NullablePtr<Expr> Init;
  if (parseValueSpecifier(Ty, Init, /*single*/ false))
    return true;

  if (Ty.isNull())
    Ty = DependentType::get(Context);

  // Note that we enter the declaration into the current scope.  Since var's are
  // not allowed to be recursive, they are entered after its initializer is
  // parsed.  This does mean that stuff like this is different than C:
  //    var x = 1; { var x = x+1; assert(x == 2); }
  if (VarName.isSimple()) {
    VarDecl *VD = new (Context) VarDecl(VarLoc,  VarName.getIdentifier(), Ty,
                                        Init.getPtrOrNull(), Attributes,
                                        CurDeclContext);
    ScopeInfo.addToScope(VD);
    Decls.push_back(VD);
    return false;
  }
  
  // Copy the name into the ASTContext heap.
  DeclVarName *TmpName = new (Context) DeclVarName(VarName);
  VarDecl *VD = new (Context) VarDecl(VarLoc, TmpName, Ty, Init.getPtrOrNull(),
                                      Attributes, CurDeclContext);
  Decls.push_back(VD);
  
  // If there is a more interesting name presented here, then we need to walk
  // through it and synthesize the decls that reference the var elements as
  // appropriate.
  SmallVector<unsigned, 8> AccessPath;
  actOnVarDeclName(VD->getNestedName(), AccessPath, VD, Decls);
  return false;
}

/// parseDeclVarSimple - This just parses a reduced case of decl-var.
///
///   decl-var-simple:
///      'var' attribute-list? any-identifier value-specifier
///
VarDecl *Parser::parseDeclVarSimple() {
  SourceLoc CurLoc = Tok.getLoc();
  SmallVector<Decl*, 2> Decls;
  if (parseDeclVar(Decls)) return 0;
  
  if (Decls.size() == 1)
    if (VarDecl *VD = dyn_cast_or_null<VarDecl>(Decls[0]))
      return VD;
  
  // FIXME: "here" requires a lot more context.
  diagnose(CurLoc, diag::non_simple_var);
  return 0;
}


/// parseDeclFunc - Parse a 'func' declaration, returning null on error.  The
/// caller handles this case and does recovery as appropriate.  If AllowScoped
/// is true, we parse both productions.
///
///   decl-func:
///     'func' attribute-list? identifier type stmt-brace?
///   decl-func-scoped:
///     'func' attribute-list? type-identifier '::' identifier type stmt-brace?
///
FuncDecl *Parser::parseDeclFunc(Type ReceiverTy) {
  SourceLoc FuncLoc = consumeToken(tok::kw_func);

  DeclAttributes Attributes;
  // FIXME: Implicitly add immutable attribute.
  parseAttributeList(Attributes);

  Identifier Name;
  SourceLoc TypeNameLoc = Tok.getLoc();
  if (parseIdentifier(Name, diag::expected_identifier_in_decl, "func"))
    return 0;

  // If this is method syntax, the first name is the receiver type.  Parse the
  // actual function name.
  if (ReceiverTy.isNull() && consumeIf(tok::coloncolon)) {
    // Look up the type name.
    ReceiverTy = ScopeInfo.lookupOrInsertTypeName(Name, TypeNameLoc);
    if (parseIdentifier(Name, diag::expected_identifier_in_decl, "func"))
      return 0;
  }
  
  // We force first type of a func declaration to be a tuple for consistency.
  if (Tok.isNot(tok::l_paren) && Tok.isNot(tok::l_paren_space)) {
    diagnose(Tok, diag::func_decl_without_paren);
    return 0;
  }
    
  Type FuncTy;
  if (parseType(FuncTy))
    return 0;
  
  // If the parsed type is not spelled as a function type (i.e., has no '->' in
  // it), then it is implicitly a function that returns ().
  if (!isa<FunctionType>(FuncTy.getPointer()))
    FuncTy = FunctionType::get(FuncTy, TupleType::getEmpty(Context), Context);
  
  // If a receiver type was specified, install the first type as the receiver,
  // as a tuple with element named 'this'.  This turns "int->int" on FooTy into
  // "(this : FooTy)->(int->int)".
  if (!ReceiverTy.isNull()) {
    TupleTypeElt ReceiverElt(ReceiverTy, Context.getIdentifier("this"));
    FuncTy = FunctionType::get(TupleType::get(ReceiverElt, Context),
                               FuncTy, Context);
  }
  
  // Enter the arguments for the function into a new function-body scope.  We
  // need this even if there is no function body to detect argument name
  // duplication.
  FuncExpr *FE = 0;
  {
    Scope FnBodyScope(this);
    
    FE = actOnFuncExprStart(FuncLoc, FuncTy);

    // Establish the new context.
    ContextChange CC(*this, FE);
    
    // Then parse the expression.
    NullablePtr<Stmt> Body;
    
    // Check to see if we have a "{" which is a brace expr.
    if (Tok.is(tok::l_brace)) {
      ParseResult<BraceStmt> Body = parseStmtBrace(diag::invalid_diagnostic);
      if (Body.isSuccess())
        FE->setBody(Body.get());
      else  // FIXME: Should do some sort of error recovery here.
        FE = 0;
      
    } else {
      // Note, we just discard FE here.  It is bump pointer allocated, so this
      // is fine (if suboptimal).
      FE = 0;
    }
  }
  
  // Create the decl for the func and add it to the parent scope.
  FuncDecl *FD = new (Context) FuncDecl(FuncLoc, Name, FuncTy, FE, Attributes,
                                        CurDeclContext);
  ScopeInfo.addToScope(FD);
  return FD;
}

/// parseDeclOneOf - Parse a 'oneof' declaration, returning null (and doing no
/// token skipping) on error.
///
///   decl-oneof:
///      'oneof' attribute-list identifier oneof-body
///      
Decl *Parser::parseDeclOneOf() {
  SourceLoc OneOfLoc = consumeToken(tok::kw_oneof);

  DeclAttributes Attributes;
  parseAttributeList(Attributes);
  
  SourceLoc NameLoc = Tok.getLoc();
  Identifier OneOfName;
  Type OneOfType;
  if (parseIdentifier(OneOfName, diag::expected_identifier_in_decl, "oneof"))
    return 0;
  
  TypeAliasDecl *TAD = ScopeInfo.addTypeAliasToScope(NameLoc, OneOfName,Type());
  if (parseDeclOneOfBody(OneOfLoc, Attributes, OneOfType, TAD))
    return 0;
  return TAD;
}


///   oneof-body:
///      '{' oneof-element (',' oneof-element)* decl* '}'
///   oneof-element:
///      identifier
///      identifier ':' type
///
/// If TypeName is specified, it is the type that the constructors should be
/// built with, so that they preserve the name of the oneof decl that contains
/// this.
bool Parser::parseDeclOneOfBody(SourceLoc OneOfLoc, const DeclAttributes &Attrs,
                                Type &Result, TypeAliasDecl *TypeName) {
  SourceLoc LBLoc, RBLoc;
  if (parseToken(tok::l_brace, LBLoc, diag::expected_lbrace_oneof_type))
    return true;
  
  SmallVector<OneOfElementInfo, 8> ElementInfos;
  
  // Parse the comma separated list of oneof elements.
  while (Tok.is(tok::identifier)) {
    OneOfElementInfo ElementInfo;
    ElementInfo.Name = Tok.getText();
    ElementInfo.NameLoc = Tok.getLoc();
    ElementInfo.EltType = 0;
    
    consumeToken(tok::identifier);
    
    // See if we have a type specifier for this oneof element.  If so, parse it.
    if (consumeIf(tok::colon) &&
        parseType(ElementInfo.EltType, diag::expected_type_oneof_element)) {
      skipUntil(tok::r_brace);
      return true;
    }
    
    ElementInfos.push_back(ElementInfo);
    
    // Require comma separation.
    if (!consumeIf(tok::comma))
      break;
  }
  
  // Parse the body as a series of decls.
  SmallVector<Decl*, 8> MemberDecls;
  while (Tok.isNot(tok::r_brace) && Tok.isNot(tok::eof)) {
    if (parseDecl(MemberDecls, PD_DisallowVar|PD_DisallowOperators))
      skipUntilDeclRBrace();
  }
  
  parseMatchingToken(tok::r_brace, RBLoc, diag::expected_rbrace_oneof_type,
                     LBLoc, diag::opening_brace);
  
  Result = actOnOneOfType(OneOfLoc, Attrs, ElementInfos, MemberDecls, TypeName);
  return false;
}

OneOfType *Parser::actOnOneOfType(SourceLoc OneOfLoc,
                                  const DeclAttributes &Attrs,
                                  ArrayRef<OneOfElementInfo> Elts,
                                  ArrayRef<Decl*> MemberDecls,
                                  TypeAliasDecl *PrettyTypeName) {
  // No attributes are valid on oneof types at this time.
  if (!Attrs.empty())
    diagnose(Attrs.LSquareLoc, diag::oneof_attributes);
  
  llvm::SmallPtrSet<const char *, 16> SeenSoFar;
  SmallVector<OneOfElementDecl *, 16> EltDecls;
  
  // If we have a PrettyTypeName to use, use it.  Otherwise, just assign the
  // constructors a temporary dummy type.
  Type TmpTy = TupleType::getEmpty(Context);
  if (PrettyTypeName)
    TmpTy = PrettyTypeName->getAliasType();
  
  for (const OneOfElementInfo &Elt : Elts) {
    Identifier NameI = Context.getIdentifier(Elt.Name);
    
    // If this was multiply defined, reject it.
    if (!SeenSoFar.insert(NameI.get())) {
      diagnose(Elt.NameLoc, diag::duplicate_oneof_element, Elt.Name);
      
      // FIXME: Do we care enough to make this efficient?
      for (unsigned I = 0, N = EltDecls.size(); I != N; ++I) {
        if (EltDecls[I]->getName() == NameI) {
          diagnose(EltDecls[I]->getLocStart(), diag::previous_definition,
                   NameI);
          break;
        }
      }
      
      // Don't copy this element into NewElements.
      continue;
    }
    
    Type EltTy = TmpTy;
    if (Type ArgTy = Elt.EltType)
      if (PrettyTypeName)
        EltTy = FunctionType::get(ArgTy, EltTy, Context);
    
    // Create a decl for each element, giving each a temporary type.
    EltDecls.push_back(new (Context) OneOfElementDecl(Elt.NameLoc, NameI,
                                                      EltTy, Elt.EltType,
                                                      CurDeclContext));
  }
  
  OneOfType *Result = OneOfType::getNew(OneOfLoc, EltDecls, CurDeclContext);
  for (OneOfElementDecl *D : EltDecls)
    D->setDeclContext(Result);
  
  // Install all of the members into the OneOf's DeclContext.
  for (Decl *D : MemberDecls)
    D->setDeclContext(Result);
  
  if (PrettyTypeName) {
    // If we have a pretty name for this, complete it to its actual type.
    PrettyTypeName->setUnderlyingType(Result);
  } else {
    // Now that the oneof type is created, we can go back and give proper types
    // to each element decl.
    for (OneOfElementDecl *Elt : EltDecls) {
      Type EltTy = Result;
      // If the OneOf Element takes a type argument, then it is actually a
      // function that takes the type argument and returns the OneOfType.
      if (Type ArgTy = Elt->getArgumentType())
        EltTy = FunctionType::get(ArgTy, EltTy, Context);
      Elt->setType(EltTy);
    }
  }
  
  return Result;
}


/// parseDeclStruct - Parse a 'struct' declaration, returning null (and doing no
/// token skipping) on error.  A 'struct' is just syntactic sugar for a oneof
/// with a single element.
///
///   decl-struct:
///      'struct' attribute-list identifier { type-tuple-body? decl* }
///
bool Parser::parseDeclStruct(SmallVectorImpl<Decl*> &Decls) {
  SourceLoc StructLoc = consumeToken(tok::kw_struct);
  
  DeclAttributes Attributes;
  parseAttributeList(Attributes);
  
  Identifier StructName;
  SourceLoc LBLoc, RBLoc;
  if (parseIdentifier(StructName, diag::expected_identifier_in_decl, "struct")||
      parseToken(tok::l_brace, LBLoc, diag::expected_lbrace_struct))
    return true;

  // Get the TypeAlias for the name that we'll eventually have.  This ensures
  // that the constructors generated have the pretty name for the type instead
  // of the raw oneof.
  TypeAliasDecl *TAD = ScopeInfo.addTypeAliasToScope(StructLoc, StructName,
                                                     Type());
  Type StructTy = TAD->getAliasType();
  
  // Parse elements of the body as a tuple body.
  Type BodyTy;
  if (parseTypeTupleBody(LBLoc, BodyTy))
    return true;
  assert(isa<TupleType>(BodyTy.getPointer()));
  
  // Reject any unnamed members.
  for (auto Elt : BodyTy->castTo<TupleType>()->Fields)
    if (Elt.Name.empty()) {
      // FIXME: Mark erroneous, terrible location info.  Probably should just
      // have custom parsing logic instead of reusing type-tuple-body.
      diagnose(LBLoc, diag::struct_unnamed_member);
    }

  
  // Parse the body as a series of decls.
  SmallVector<Decl*, 8> MemberDecls;
  while (Tok.isNot(tok::r_brace) && Tok.isNot(tok::eof)) {
    if (parseDecl(MemberDecls, PD_DisallowVar|PD_DisallowOperators))
      skipUntilDeclRBrace();
  }
  
  if (parseMatchingToken(tok::r_brace, RBLoc, diag::expected_rbrace_struct,
                         LBLoc, diag::opening_brace))
    return true;
          
  Decls.push_back(TAD);
  
  // The 'struct' is syntactically fine, invoke the semantic actions for the
  // syntactically expanded oneof type.  Struct declarations are just sugar for
  // other existing constructs.
  Parser::OneOfElementInfo ElementInfo;
  ElementInfo.Name = StructName.str();
  ElementInfo.NameLoc = StructLoc;
  ElementInfo.EltType = BodyTy;
  OneOfType *OneOfTy = actOnOneOfType(StructLoc, Attributes, ElementInfo, 
                                      MemberDecls, TAD);
  assert(OneOfTy->isTransparentType() && "Somehow isn't a struct?");
  
  // In addition to defining the oneof declaration, structs also inject their
  // constructor into the global scope.
  assert(OneOfTy->Elements.size() == 1 && "Struct has exactly one element");
  ScopeInfo.addToScope(OneOfTy->getElement(0));
  Decls.push_back(OneOfTy->getElement(0));
  return false;
}


/// parseDeclProtocol - Parse a 'protocol' declaration, returning null (and
/// doing no token skipping) on error.
///
///   decl-protocol:
///      'protocol' attribute-list identifier protocol-body
///      
Decl *Parser::parseDeclProtocol() {
  SourceLoc ProtocolLoc = consumeToken(tok::kw_protocol);
  
  DeclAttributes Attributes;
  parseAttributeList(Attributes);
  
  SourceLoc NameLoc = Tok.getLoc();
  Identifier ProtocolName;
  if (parseIdentifier(ProtocolName,
                      diag::expected_identifier_in_decl, "protocol"))
    return 0;
  
  TypeAliasDecl *TAD = ScopeInfo.addTypeAliasToScope(NameLoc, ProtocolName,
                                                     Type());
  Type ProtocolType;
  if (parseProtocolBody(ProtocolLoc, Attributes, ProtocolType, TAD))
    return 0;
  return TAD;
}

///   protocol-body:
///      '{' protocol-element* '}'
///   protocol-element:
///      decl-func
///      decl-var-simple
///      // 'typealias' identifier
///
bool Parser::parseProtocolBody(SourceLoc ProtocolLoc, 
                               const DeclAttributes &Attributes,
                               Type &Result, TypeAliasDecl *TypeName) {
  // Parse the body.
  if (parseToken(tok::l_brace, diag::expected_lbrace_protocol_type))
    return true;
  
  Type ThisType = TypeName->getAliasType();
  
  // Parse the list of protocol elements.
  SmallVector<ValueDecl*, 8> Elements;
  do {
    switch (Tok.getKind()) {
    default:
      diagnose(Tok, diag::expected_protocol_member);
      return true;
    case tok::r_brace:  // End of protocol body.
      break;
      
      // FIXME: use standard parseDecl loop.
        
    case tok::kw_func:
      Elements.push_back(parseDeclFunc(ThisType));
      if (Elements.back() == 0) return true;
      break;
    case tok::kw_var:
      Elements.push_back(parseDeclVarSimple());
      if (Elements.back() == 0) return true;
      break;
    }
  } while (Tok.isNot(tok::r_brace));
  
  consumeToken(tok::r_brace);
  
  
  // Act on what we've parsed.
  if (!Attributes.empty())
    diagnose(Attributes.LSquareLoc, diag::protocol_attributes);
  
  ProtocolType *NewProto = ProtocolType::getNew(ProtocolLoc, Elements,
                                                CurDeclContext);
  
  // Install all of the members of protocol into the protocol's DeclContext.
  for (Decl *D : Elements)
    D->setDeclContext(NewProto);
  
  // Complete the pretty name for this type.
  TypeName->setUnderlyingType(NewProto);
  
  Result = NewProto;
  return false;
}

