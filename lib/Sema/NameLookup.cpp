//===--- NameLookup.cpp - Swift Name Lookup Routines ----------------------===//
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
// This file implements interfaces for performing name lookup.
//
//===----------------------------------------------------------------------===//


#include "NameLookup.h"
#include "swift/AST/AST.h"
#include <algorithm>
using namespace swift;

MemberLookup::MemberLookup(Type BaseTy, Identifier Name, Module &M) {
  doIt(BaseTy, Name, M);
}

/// doIt - Lookup a member 'Name' in 'BaseTy' within the context
/// of a given module 'M'.  This operation corresponds to a standard "dot" 
/// lookup operation like "a.b" where 'this' is the type of 'a'.  This
/// operation is only valid after name binding.
void MemberLookup::doIt(Type BaseTy, Identifier Name, Module &M) {
  typedef MemberLookupResult Result;
  
  // Just look through l-valueness.  It doesn't affect name lookup.
  if (LValueType *LV = BaseTy->getAs<LValueType>())
    BaseTy = LV->getObjectType();

  // Type check metatype references, as in "some_type.some_member".  These are
  // special and can't have extensions.
  if (MetaTypeType *MTT = BaseTy->getAs<MetaTypeType>()) {
    // The metatype represents an arbitrary named type: dig through to the
    // declared type to see what we're dealing with.  If the type was erroneous
    // then silently squish this erroneous subexpression.
    Type Ty = MTT->getTypeDecl()->getDeclaredType();

    // Handle references to the constructors of a oneof.
    if (OneOfType *OOTy = Ty->getAs<OneOfType>()) {
      OneOfElementDecl *Elt = OOTy->getDecl()->getElement(Name);
      if (Elt) {
        Results.push_back(Result::getIgnoreBase(Elt));

        // Fall through to find any members with the same name.
      }
    }
        
    // Otherwise, just perform normal dot lookup on the type with the specified
    // member name to see if we find extensions or anything else.  For example,
    // If type SomeTy.SomeMember can look up static functions, and can even look
    // up non-static functions as well (thus getting the address of the member).
    doIt(Ty, Name, M);

    // If we find anything that requires 'this', reset it back because we don't
    // have a this.
    bool AnyInvalid = false;
    for (Result &R : Results) {
      switch (R.Kind) {
      case Result::PassBase:
        // No 'this' to pass.
        R.Kind = Result::IgnoreBase;
        break;
          
      case Result::IgnoreBase:
        break;
          
      case Result::StructElement:
      case Result::TupleElement:
        AnyInvalid = true;
        break;
      }
    }

    if (AnyInvalid) {
      // If we found any results that can't have their base ignored, drop
      // them.
      // FIXME: This is a terrible way to implement this, but we crash
      // in createResultAST if we don't do something here.
      Results.erase(std::remove_if(Results.begin(), Results.end(),
                      ^(Result &R) { return R.Kind != Result::IgnoreBase; }),
                    Results.end());
    }
    return;
  }
  
  // Lookup module references, as on some_module.some_member.  These are
  // special and can't have extensions.
  if (ModuleType *MT = BaseTy->getAs<ModuleType>()) {
    SmallVector<ValueDecl*, 8> Decls;
    MT->getModule()->lookupValue(Module::AccessPathTy(), Name,
                                 NLKind::QualifiedLookup, Decls);
    for (ValueDecl *VD : Decls)
      Results.push_back(Result::getIgnoreBase(VD));
    return;
  }

  // If the base is a protocol, see if this is a reference to a declared
  // protocol member.
  if (ProtocolType *PT = BaseTy->getAs<ProtocolType>()) {
    for (ValueDecl *VD : PT->getDecl()->getElements()) {
      if (VD->getName() != Name) continue;
      
      // If this is a 'static' function, then just ignore the base expression.
      if (FuncDecl *FD = dyn_cast<FuncDecl>(VD))
        if (FD->isStatic()) {
          Results.push_back(Result::getIgnoreBase(FD));
          return;
        }
      
      Results.push_back(Result::getPassBase(VD));
      return;
    }
  }
  
  // Check to see if this is a reference to a tuple field.
  if (TupleType *TT = BaseTy->getAs<TupleType>())
    doTuple(TT, Name, false);

  // If this is a member access to a oneof with a single element constructor
  // (e.g. a struct), allow direct access to the type underlying the single
  // element.
  if (OneOfType *OneOf = BaseTy->getAs<OneOfType>()) {
    if (OneOf->getDecl()->isTransparentType()) {
      Type SubType = OneOf->getDecl()->getTransparentType();
      if (TupleType *TT = SubType->getAs<TupleType>())
        doTuple(TT, Name, true);
    }
  }
  

  // Look in any extensions that add methods to the base type.
  SmallVector<ValueDecl*, 8> ExtensionMethods;
  M.lookupGlobalExtensionMethods(BaseTy, Name, ExtensionMethods);

  for (ValueDecl *VD : ExtensionMethods) {
    if (TypeDecl *TAD = dyn_cast<TypeDecl>(VD)) {
      Results.push_back(Result::getIgnoreBase(TAD));
      continue;
    }
    if (FuncDecl *FD = dyn_cast<FuncDecl>(VD))
      if (FD->isStatic()) {
        Results.push_back(Result::getIgnoreBase(FD));
        continue;
      }
    Results.push_back(Result::getPassBase(VD));
  }
}

void MemberLookup::doTuple(TupleType *TT, Identifier Name, bool IsStruct) {
  // If the field name exists, we win.  Otherwise, if the field name is a
  // dollarident like $4, process it as a field index.
  int FieldNo = TT->getNamedElementId(Name);
  if (FieldNo != -1) {
    Results.push_back(MemberLookupResult::getTupleElement(FieldNo, IsStruct));
    return;
  }
  
  StringRef NameStr = Name.str();
  if (NameStr.startswith("$")) {
    unsigned Value = 0;
    if (!NameStr.substr(1).getAsInteger(10, Value) &&
        Value < TT->getFields().size())
      Results.push_back(MemberLookupResult::getTupleElement(Value, IsStruct));
  }
}

static Type makeSimilarLValue(Type objectType, Type lvalueType,
                              ASTContext &Context) {
  LValueType::Qual qs = cast<LValueType>(lvalueType)->getQualifiers();
  
  // Don't propagate explicitness.
  qs |= LValueType::Qual::Implicit;
  
  return LValueType::get(objectType, qs, Context);
}

static Expr *lookThroughOneofs(Expr *E, ASTContext &Context) {
  
  Type BaseType = E->getType();
  bool IsLValue = E->getType()->is<LValueType>();
  if (IsLValue)
    BaseType = cast<LValueType>(BaseType)->getObjectType();
  
  OneOfType *Oneof = BaseType->castTo<OneOfType>();
  assert(Oneof->getDecl()->isTransparentType());
  
  Type ResultType = Oneof->getDecl()->getTransparentType();
  if (IsLValue)
    ResultType = makeSimilarLValue(ResultType, E->getType(), Context);
  return new (Context) LookThroughOneofExpr(E, ResultType);
}

static Expr *buildTupleElementExpr(Expr *Base, SourceLoc DotLoc,
                                   SourceLoc NameLoc, unsigned FieldIndex,
                                   ASTContext &Context) {
  Type BaseTy = Base->getType();
  bool IsLValue = false;
  if (LValueType *LV = BaseTy->getAs<LValueType>()) {
    IsLValue = true;
    BaseTy = LV->getObjectType();
  }
  
  Type FieldType = BaseTy->castTo<TupleType>()->getElementType(FieldIndex);
  if (IsLValue)
    FieldType = makeSimilarLValue(FieldType, Base->getType(), Context);
  
  if (DotLoc.isValid())
    return new (Context) SyntacticTupleElementExpr(Base, DotLoc, FieldIndex,
                                                   NameLoc, FieldType);
  
  return new (Context) ImplicitThisTupleElementExpr(Base, FieldIndex, NameLoc,
                                                    FieldType);
}


/// createResultAST - Build an AST to represent this lookup, with the
/// specified base expression.
Expr *MemberLookup::createResultAST(Expr *Base, SourceLoc DotLoc, 
                                    SourceLoc NameLoc, ASTContext &Context) {
  assert(isSuccess() && "Can't create a result if we didn't find anything");
         
  // Handle the case when we found exactly one result.
  if (Results.size() == 1) {
    MemberLookupResult R = Results[0];

    switch (R.Kind) {
    case MemberLookupResult::StructElement:
      Base = lookThroughOneofs(Base, Context);
      // FALL THROUGH.
    case MemberLookupResult::TupleElement:
      return buildTupleElementExpr(Base, DotLoc, NameLoc, R.TupleFieldNo,
                                   Context);
    case MemberLookupResult::PassBase: {
      if (isa<FuncDecl>(R.D)) {
        Expr *Fn = new (Context) DeclRefExpr(R.D, NameLoc,
                                             R.D->getTypeOfReference());
        return new (Context) DotSyntaxCallExpr(Fn, DotLoc, Base);
      }

      VarDecl *Var = cast<VarDecl>(R.D);
      return new (Context) MemberRefExpr(Base, DotLoc, Var, NameLoc);
    }
    case MemberLookupResult::IgnoreBase:
      Expr *RHS = new (Context) DeclRefExpr(R.D, NameLoc,
                                            R.D->getTypeOfReference());
      return new (Context) DotSyntaxBaseIgnoredExpr(Base, DotLoc, RHS);
    }
  }
  
  // If we have an ambiguous result, build an overload set.
  SmallVector<ValueDecl*, 8> ResultSet;
    
  // This is collecting a mix of static and normal functions. We won't know
  // until after overload resolution whether we actually need 'this'.
  for (MemberLookupResult X : Results) {
    assert(X.Kind != MemberLookupResult::TupleElement &&
           X.Kind != MemberLookupResult::StructElement);
    ResultSet.push_back(X.D);
  }
  
  return OverloadedMemberRefExpr::createWithCopy(Base, DotLoc, ResultSet,
                                                 NameLoc);
}
