//===--- ASTContext.cpp - ASTContext Implementation -----------------------===//
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
//  This file implements the ASTContext class.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/AST.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/ExprHandle.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
using namespace swift;

struct ASTContext::Implementation {
  Implementation();
  ~Implementation();

  llvm::BumpPtrAllocator Allocator; // used in later initializations
  llvm::StringMap<char, llvm::BumpPtrAllocator&> IdentifierTable;
  llvm::FoldingSet<TupleType> TupleTypes;
  llvm::DenseMap<Type, MetaTypeType*> MetaTypeTypes;
  llvm::DenseMap<Module*, ModuleType*> ModuleTypes;
  llvm::DenseMap<std::pair<Type,std::pair<Type,char>>,
                 FunctionType*> FunctionTypes;
  llvm::DenseMap<std::pair<Type, uint64_t>, ArrayType*> ArrayTypes;
  llvm::DenseMap<Type, ArraySliceType*> ArraySliceTypes;
  llvm::DenseMap<unsigned, BuiltinIntegerType*> IntegerTypes;
  llvm::DenseMap<Type, ParenType*> ParenTypes;
  llvm::DenseMap<std::pair<Type, LValueType::Qual::opaque_type>, LValueType*>
    LValueTypes;
  llvm::DenseMap<std::pair<Type, Type>, SubstitutedType *> SubstitutedTypes;

  llvm::FoldingSet<OneOfType> OneOfTypes;
  llvm::FoldingSet<StructType> StructTypes;
  llvm::FoldingSet<ClassType> ClassTypes;
  llvm::FoldingSet<ProtocolCompositionType> ProtocolCompositionTypes;
  llvm::FoldingSet<UnboundGenericType> UnboundGenericTypes;
  llvm::FoldingSet<BoundGenericType> BoundGenericTypes;

  llvm::DenseMap<BoundGenericType *, ArrayRef<Substitution>>
    BoundGenericSubstitutions;
};

ASTContext::Implementation::Implementation()
 : IdentifierTable(Allocator) {}
ASTContext::Implementation::~Implementation() {}

ASTContext::ASTContext(LangOptions &langOpts, llvm::SourceMgr &sourcemgr,
                       DiagnosticEngine &Diags)
  : Impl(*new Implementation()),
    LangOpts(langOpts),
    SourceMgr(sourcemgr),
    Diags(Diags),
    TheBuiltinModule(new (*this) BuiltinModule(getIdentifier("Builtin"),*this)),
    TheErrorType(new (*this) ErrorType(*this)),
    TheEmptyTupleType(TupleType::get(ArrayRef<TupleTypeElt>(), *this)),
    TheObjectPointerType(new (*this) BuiltinObjectPointerType(*this)),
    TheObjCPointerType(new (*this) BuiltinObjCPointerType(*this)),
    TheRawPointerType(new (*this) BuiltinRawPointerType(*this)),
    TheUnstructuredUnresolvedType(new (*this) UnstructuredUnresolvedType(*this)),
    TheIEEE32Type(new (*this) BuiltinFloatType(BuiltinFloatType::IEEE32,*this)),
    TheIEEE64Type(new (*this) BuiltinFloatType(BuiltinFloatType::IEEE64,*this)),
    TheIEEE16Type(new (*this) BuiltinFloatType(BuiltinFloatType::IEEE16,*this)),
    TheIEEE80Type(new (*this) BuiltinFloatType(BuiltinFloatType::IEEE80,*this)),
    TheIEEE128Type(new (*this) BuiltinFloatType(BuiltinFloatType::IEEE128,
                                                *this)),
    ThePPC128Type(new (*this) BuiltinFloatType(BuiltinFloatType::PPC128,*this)){
}

ASTContext::~ASTContext() {
  delete &Impl;

  for (auto &entry : ConformsTo)
    delete const_cast<ProtocolConformance*>(entry.second);
}

void *ASTContext::Allocate(unsigned long Bytes, unsigned Alignment) {
  return Impl.Allocator.Allocate(Bytes, Alignment);
}

/// getIdentifier - Return the uniqued and AST-Context-owned version of the
/// specified string.
Identifier ASTContext::getIdentifier(StringRef Str) {
  // Make sure null pointers stay null.
  if (Str.empty()) return Identifier(0);
  
  return Identifier(Impl.IdentifierTable.GetOrCreateValue(Str).getKeyData());
}

bool ASTContext::hadError() const {
  return Diags.hadAnyError();
}

Optional<ArrayRef<Substitution>>
ASTContext::getSubstitutions(BoundGenericType* Bound) {
  assert(Bound->isCanonical() && "Requesting non-canonical substitutions");
  auto Known = Impl.BoundGenericSubstitutions.find(Bound);
  if (Known == Impl.BoundGenericSubstitutions.end())
    return Nothing;

  return Known->second;
}

void ASTContext::setSubstitutions(BoundGenericType* Bound,
                                  ArrayRef<Substitution> Subs) {
  assert(Bound->isCanonical() && "Requesting non-canonical substitutions");
  assert(Impl.BoundGenericSubstitutions.count(Bound) == 0 &&
         "Already have substitutions?");
  Impl.BoundGenericSubstitutions[Bound] = Subs;
}

//===----------------------------------------------------------------------===//
// Type manipulation routines.
//===----------------------------------------------------------------------===//

// Simple accessors.
Type ErrorType::get(ASTContext &C) { return C.TheErrorType; }
Type UnstructuredUnresolvedType::get(ASTContext &C) { 
  return C.TheUnstructuredUnresolvedType; 
}


BuiltinIntegerType *BuiltinIntegerType::get(unsigned BitWidth, ASTContext &C) {
  BuiltinIntegerType *&Result = C.Impl.IntegerTypes[BitWidth];
  if (Result == 0)
    Result = new (C) BuiltinIntegerType(BitWidth, C);
  return Result;
}

ParenType *ParenType::get(ASTContext &C, Type underlying) {
  ParenType *&Result = C.Impl.ParenTypes[underlying];
  bool hasTypeVariable = underlying->hasTypeVariable();
  if (Result == 0)
    Result = new (C) ParenType(underlying, hasTypeVariable);
  return Result;
}

Type TupleType::getEmpty(ASTContext &C) { return C.TheEmptyTupleType; }

void TupleType::Profile(llvm::FoldingSetNodeID &ID,
                        ArrayRef<TupleTypeElt> Fields) {
  ID.AddInteger(Fields.size());
  for (const TupleTypeElt &Elt : Fields) {
    ID.AddPointer(Elt.getType().getPointer());
    ID.AddPointer(Elt.getName().get());
    ID.AddPointer(Elt.getInit());
    ID.AddPointer(Elt.getVarargBaseTy().getPointer());
  }
}

/// getTupleType - Return the uniqued tuple type with the specified elements.
Type TupleType::get(ArrayRef<TupleTypeElt> Fields, ASTContext &C) {
  if (Fields.size() == 1 && !Fields[0].isVararg() && !Fields[0].hasName())
    return ParenType::get(C, Fields[0].getType());

  bool HasAnyDefaultValues = false;
  bool HasTypeVariable = false;

  for (const TupleTypeElt &Elt : Fields) {
    if (Elt.hasInit()) {
      HasAnyDefaultValues = true;
      if (HasTypeVariable)
        break;
    }
    if (Elt.getType() && Elt.getType()->hasTypeVariable()) {
      HasTypeVariable = true;
      if (HasAnyDefaultValues)
        break;
    }
  }

  void *InsertPos = 0;
  if (!HasAnyDefaultValues) {
    // Check to see if we've already seen this tuple before.
    llvm::FoldingSetNodeID ID;
    TupleType::Profile(ID, Fields);

    if (TupleType *TT = C.Impl.TupleTypes.FindNodeOrInsertPos(ID, InsertPos))
      return TT;
  }

  // Make a copy of the fields list into ASTContext owned memory.
  TupleTypeElt *FieldsCopy =
    C.AllocateCopy<TupleTypeElt>(Fields.begin(), Fields.end());
  
  bool IsCanonical = true;   // All canonical elts means this is canonical.
  for (const TupleTypeElt &Elt : Fields) {
    if (Elt.getType().isNull() || !Elt.getType()->isCanonical()) {
      IsCanonical = false;
      break;
    }
  }

  Fields = ArrayRef<TupleTypeElt>(FieldsCopy, Fields.size());
  
  TupleType *New = new (C) TupleType(Fields, IsCanonical ? &C : 0,
                                     HasTypeVariable);
  if (!HasAnyDefaultValues)
    C.Impl.TupleTypes.InsertNode(New, InsertPos);

  return New;
}

void UnboundGenericType::Profile(llvm::FoldingSetNodeID &ID,
                                 NominalTypeDecl *TheDecl, Type Parent) {
  ID.AddPointer(TheDecl);
  ID.AddPointer(Parent.getPointer());
}

UnboundGenericType* UnboundGenericType::get(NominalTypeDecl *TheDecl,
                                            Type Parent,
                                            ASTContext &C) {
  llvm::FoldingSetNodeID ID;
  UnboundGenericType::Profile(ID, TheDecl, Parent);
  void *InsertPos = 0;
  if (auto unbound = C.Impl.UnboundGenericTypes.FindNodeOrInsertPos(ID,
                                                                    InsertPos))
    return unbound;

  bool hasTypeVariable = Parent && Parent->hasTypeVariable();
  auto result = new (C) UnboundGenericType(TheDecl, Parent, C, hasTypeVariable);
  C.Impl.UnboundGenericTypes.InsertNode(result, InsertPos);
  return result;
}

void BoundGenericType::Profile(llvm::FoldingSetNodeID &ID,
                               NominalTypeDecl *TheDecl, Type Parent,
                               ArrayRef<Type> GenericArgs) {
  ID.AddPointer(TheDecl);
  ID.AddPointer(Parent.getPointer());
  ID.AddInteger(GenericArgs.size());
  for (Type Arg : GenericArgs)
    ID.AddPointer(Arg.getPointer());
}

BoundGenericType::BoundGenericType(TypeKind theKind,
                                   NominalTypeDecl *theDecl,
                                   Type parent,
                                   ArrayRef<Type> genericArgs,
                                   ASTContext *context,
                                   bool hasTypeVariable)
  : TypeBase(theKind, context, /*Unresolved=*/false,
             hasTypeVariable),
    TheDecl(theDecl), Parent(parent), GenericArgs(genericArgs)
{
  // Determine whether this type is unresolved.
  if (parent && parent->isUnresolvedType())
    setUnresolved();
  else for (Type arg : genericArgs) {
    if (arg->isUnresolvedType()) {
      setUnresolved();
      break;
    }
  }
}

BoundGenericType *BoundGenericType::get(NominalTypeDecl *TheDecl,
                                        Type Parent,
                                        ArrayRef<Type> GenericArgs) {
  ASTContext &C = TheDecl->getDeclContext()->getASTContext();
  llvm::FoldingSetNodeID ID;
  BoundGenericType::Profile(ID, TheDecl, Parent, GenericArgs);

  void *InsertPos = 0;
  if (BoundGenericType *BGT =
          C.Impl.BoundGenericTypes.FindNodeOrInsertPos(ID, InsertPos))
    return BGT;

  bool HasTypeVariable = Parent && Parent->hasTypeVariable();
  ArrayRef<Type> ArgsCopy = C.AllocateCopy(GenericArgs);
  bool IsCanonical = !Parent || Parent->isCanonical();
  if (IsCanonical || !HasTypeVariable) {
    for (Type Arg : GenericArgs) {
      if (!Arg->isCanonical()) {
        IsCanonical = false;
        if (HasTypeVariable)
          break;
      }

      if (Arg->hasTypeVariable()) {
        HasTypeVariable = true;
        if (!IsCanonical)
          break;
      }
    }
  }

  BoundGenericType *newType;
  if (auto theClass = dyn_cast<ClassDecl>(TheDecl)) {
    newType = new (C) BoundGenericClassType(theClass, Parent, ArgsCopy,
                                            IsCanonical ? &C : 0,
                                            HasTypeVariable);
  } else if (auto theStruct = dyn_cast<StructDecl>(TheDecl)) {
    newType = new (C) BoundGenericStructType(theStruct, Parent, ArgsCopy,
                                             IsCanonical ? &C : 0,
                                             HasTypeVariable);
  } else {
    auto theOneOf = cast<OneOfDecl>(TheDecl);
    newType = new (C) BoundGenericOneOfType(theOneOf, Parent, ArgsCopy,
                                            IsCanonical ? &C : 0,
                                            HasTypeVariable);
  }
  C.Impl.BoundGenericTypes.InsertNode(newType, InsertPos);

  return newType;
}

NominalType *NominalType::get(NominalTypeDecl *D, Type Parent, ASTContext &C) {
  switch (D->getKind()) {
  case DeclKind::OneOf:
    return OneOfType::get(cast<OneOfDecl>(D), Parent, C);
  case DeclKind::Struct:
    return StructType::get(cast<StructDecl>(D), Parent, C);
  case DeclKind::Class:
    return ClassType::get(cast<ClassDecl>(D), Parent, C);
  case DeclKind::Protocol:
    return D->getDeclaredType()->castTo<ProtocolType>();

  default:
    llvm_unreachable("Not a nominal declaration!");
  }
}

OneOfType::OneOfType(OneOfDecl *TheDecl, Type Parent, ASTContext &C,
                     bool HasTypeVariable)
  : NominalType(TypeKind::OneOf, &C, TheDecl, Parent, HasTypeVariable) { }

OneOfType *OneOfType::get(OneOfDecl *D, Type Parent, ASTContext &C) {
  llvm::FoldingSetNodeID id;
  OneOfType::Profile(id, D,Parent);

  void *insertPos = 0;
  if (auto oneOfTy = C.Impl.OneOfTypes.FindNodeOrInsertPos(id, insertPos))
    return oneOfTy;

  bool hasTypeVariable = Parent && Parent->hasTypeVariable();
  auto oneOfTy = new (C) OneOfType(D, Parent, C, hasTypeVariable);
  C.Impl.OneOfTypes.InsertNode(oneOfTy, insertPos);
  return oneOfTy;
}

void OneOfType::Profile(llvm::FoldingSetNodeID &ID, OneOfDecl *D, Type Parent) {
  ID.AddPointer(D);
  ID.AddPointer(Parent.getPointer());
}

StructType::StructType(StructDecl *TheDecl, Type Parent, ASTContext &C,
                       bool HasTypeVariable)
  : NominalType(TypeKind::Struct, &C, TheDecl, Parent, HasTypeVariable) { }

StructType *StructType::get(StructDecl *D, Type Parent, ASTContext &C) {
  llvm::FoldingSetNodeID id;
  StructType::Profile(id, D, Parent);

  void *insertPos = 0;
  if (auto structTy = C.Impl.StructTypes.FindNodeOrInsertPos(id, insertPos))
    return structTy;

  bool hasTypeVariable = Parent && Parent->hasTypeVariable();
  auto structTy = new (C) StructType(D, Parent, C, hasTypeVariable);
  C.Impl.StructTypes.InsertNode(structTy, insertPos);
  return structTy;
}

void StructType::Profile(llvm::FoldingSetNodeID &ID, StructDecl *D, Type Parent) {
  ID.AddPointer(D);
  ID.AddPointer(Parent.getPointer());
}

ClassType::ClassType(ClassDecl *TheDecl, Type Parent, ASTContext &C,
                     bool HasTypeVariable)
  : NominalType(TypeKind::Class, &C, TheDecl, Parent, HasTypeVariable) { }

ClassType *ClassType::get(ClassDecl *D, Type Parent, ASTContext &C) {
  llvm::FoldingSetNodeID id;
  ClassType::Profile(id, D, Parent);

  void *insertPos = 0;
  if (auto classTy = C.Impl.ClassTypes.FindNodeOrInsertPos(id, insertPos))
    return classTy;

  bool hasTypeVariable = Parent && Parent->hasTypeVariable();
  auto classTy = new (C) ClassType(D, Parent, C, hasTypeVariable);
  C.Impl.ClassTypes.InsertNode(classTy, insertPos);
  return classTy;
}

void ClassType::Profile(llvm::FoldingSetNodeID &ID, ClassDecl *D, Type Parent) {
  ID.AddPointer(D);
  ID.AddPointer(Parent.getPointer());
}

IdentifierType *IdentifierType::getNew(ASTContext &C,
                                       MutableArrayRef<Component> Components) {
  Components = C.AllocateCopy(Components);
  return new (C) IdentifierType(Components);
}

ProtocolCompositionType *
ProtocolCompositionType::build(ASTContext &C, ArrayRef<Type> Protocols) {
  // Check to see if we've already seen this protocol composition before.
  void *InsertPos = 0;
  llvm::FoldingSetNodeID ID;
  ProtocolCompositionType::Profile(ID, Protocols);
  if (ProtocolCompositionType *Result
        = C.Impl.ProtocolCompositionTypes.FindNodeOrInsertPos(ID, InsertPos))
    return Result;

  bool isCanonical = true;
  for (Type t : Protocols) {
    if (!t->isCanonical())
      isCanonical = false;
  }

  // Create a new protocol composition type.
  ProtocolCompositionType *New =
      new (C) ProtocolCompositionType(isCanonical ? &C : nullptr,
                                      C.AllocateCopy(Protocols));
  C.Impl.ProtocolCompositionTypes.InsertNode(New, InsertPos);
  return New;
}


MetaTypeType *MetaTypeType::get(Type T, ASTContext &C) {
  MetaTypeType *&Entry = C.Impl.MetaTypeTypes[T];
  if (Entry) return Entry;

  bool hasTypeVariable = T->hasTypeVariable();
  return Entry = new (C) MetaTypeType(T, T->isCanonical() ? &C : 0,
                                      hasTypeVariable);
}

MetaTypeType::MetaTypeType(Type T, ASTContext *C, bool HasTypeVariable)
  : TypeBase(TypeKind::MetaType, C, T->isUnresolvedType(), HasTypeVariable),
    InstanceType(T) {
}

ModuleType *ModuleType::get(Module *M) {
  ASTContext &C = M->getASTContext();
  
  ModuleType *&Entry = C.Impl.ModuleTypes[M];
  if (Entry) return Entry;
  
  return Entry = new (C) ModuleType(M, C);
}

/// FunctionType::get - Return a uniqued function type with the specified
/// input and result.
FunctionType *FunctionType::get(Type Input, Type Result, bool isAutoClosure,
                                ASTContext &C) {
  FunctionType *&Entry =
    C.Impl.FunctionTypes[std::make_pair(Input,
                                        std::make_pair(Result, 
                                                       (char)isAutoClosure))];
  if (Entry) return Entry;

  bool hasTypeVariable = Input->hasTypeVariable() || Result->hasTypeVariable();
  return Entry = new (C) FunctionType(Input, Result, isAutoClosure,
                                      hasTypeVariable);
}

// If the input and result types are canonical, then so is the result.
FunctionType::FunctionType(Type input, Type output, bool isAutoClosure,
                           bool hasTypeVariable)
  : AnyFunctionType(TypeKind::Function,
             (input->isCanonical() && output->isCanonical()) ?
               &input->getASTContext() : 0,
             input, output,
             (input->isUnresolvedType() || output->isUnresolvedType()),
             hasTypeVariable),
    AutoClosure(isAutoClosure) { }


/// FunctionType::get - Return a uniqued function type with the specified
/// input and result.
PolymorphicFunctionType *PolymorphicFunctionType::get(Type input, Type output,
                                                      GenericParamList *params,
                                                      ASTContext &C) {
  // FIXME: one day we should do canonicalization properly.
  return new (C) PolymorphicFunctionType(input, output, params, C);
}

PolymorphicFunctionType::PolymorphicFunctionType(Type input, Type output,
                                                 GenericParamList *params,
                                                 ASTContext &C)
  : AnyFunctionType(TypeKind::PolymorphicFunction,
                    (input->isCanonical() && output->isCanonical()) ?&C : 0,
                    input, output,
                    (input->isUnresolvedType() || output->isUnresolvedType()),
                    /*HasTypeVariable=*/false),
    Params(params)
{
  assert(!input->hasTypeVariable() && !output->hasTypeVariable());
}

/// Return a uniqued array type with the specified base type and the
/// specified size.
ArrayType *ArrayType::get(Type BaseType, uint64_t Size, ASTContext &C) {
  assert(Size != 0);

  ArrayType *&Entry = C.Impl.ArrayTypes[std::make_pair(BaseType, Size)];
  if (Entry) return Entry;

  bool hasTypeVariable = BaseType->hasTypeVariable();
  return Entry = new (C) ArrayType(BaseType, Size, hasTypeVariable);
}

ArrayType::ArrayType(Type base, uint64_t size, bool hasTypeVariable)
  : TypeBase(TypeKind::Array, 
             base->isCanonical() ? &base->getASTContext() : 0,
             base->isUnresolvedType(), hasTypeVariable),
    Base(base), Size(size) {}


/// Return a uniqued array slice type with the specified base type.
ArraySliceType *ArraySliceType::get(Type base, ASTContext &C) {
  ArraySliceType *&entry = C.Impl.ArraySliceTypes[base];
  if (entry) return entry;

  bool hasTypeVariable = base->hasTypeVariable();
  return entry = new (C) ArraySliceType(base, hasTypeVariable);
}

ProtocolType::ProtocolType(ProtocolDecl *TheDecl, ASTContext &Ctx)
  : NominalType(TypeKind::Protocol, &Ctx, TheDecl, /*Parent=*/Type(),
                /*HasTypeVariable=*/false) { }

LValueType *LValueType::get(Type objectTy, Qual quals, ASTContext &C) {
  auto key = std::make_pair(objectTy, quals.getOpaqueData());
  auto it = C.Impl.LValueTypes.find(key);
  if (it != C.Impl.LValueTypes.end()) return it->second;

  bool hasTypeVariable = objectTy->hasTypeVariable();
  ASTContext *canonicalContext = (objectTy->isCanonical() ? &C : nullptr);
  LValueType *type = new (C) LValueType(objectTy, quals, canonicalContext,
                                        hasTypeVariable);
  C.Impl.LValueTypes.insert(std::make_pair(key, type));
  return type;
}

/// Return a uniqued substituted type.
SubstitutedType *SubstitutedType::get(Type Original, Type Replacement,
                                      ASTContext &C) {
  SubstitutedType *&Known = C.Impl.SubstitutedTypes[{Original, Replacement}];
  if (!Known) {
    bool hasTypeVariable = Replacement->hasTypeVariable();
    Known = new (C) SubstitutedType(Original, Replacement, hasTypeVariable);
  }
  return Known;
}

void *ExprHandle::operator new(size_t Bytes, ASTContext &C,
                            unsigned Alignment) {
  return C.Allocate(Bytes, Alignment);
}

ExprHandle *ExprHandle::get(ASTContext &Context, Expr *E) {
  return new (Context) ExprHandle(E);
}

void TypeLoc::setInvalidType(ASTContext &C) {
  T = ErrorType::get(C);
}
