//===---------------- SemaCodeComplete.cpp - Code Completion ----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file defines the code-completion semantic actions.
//
//===----------------------------------------------------------------------===//
#include "clang/Sema/SemaInternal.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Overload.h"
#include "clang/Sema/CodeCompleteConsumer.h"
#include "clang/Sema/ExternalSemaSource.h"
#include "clang/Sema/Scope.h"
#include "clang/Sema/ScopeInfo.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ExprObjC.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Twine.h"
#include <list>
#include <map>
#include <vector>

using namespace clang;
using namespace sema;

namespace {
  /// \brief A container of code-completion results.
  class ResultBuilder {
  public:
    /// \brief The type of a name-lookup filter, which can be provided to the
    /// name-lookup routines to specify which declarations should be included in
    /// the result set (when it returns true) and which declarations should be
    /// filtered out (returns false).
    typedef bool (ResultBuilder::*LookupFilter)(NamedDecl *) const;
    
    typedef CodeCompletionResult Result;
    
  private:
    /// \brief The actual results we have found.
    std::vector<Result> Results;
    
    /// \brief A record of all of the declarations we have found and placed
    /// into the result set, used to ensure that no declaration ever gets into
    /// the result set twice.
    llvm::SmallPtrSet<Decl*, 16> AllDeclsFound;
    
    typedef std::pair<NamedDecl *, unsigned> DeclIndexPair;

    /// \brief An entry in the shadow map, which is optimized to store
    /// a single (declaration, index) mapping (the common case) but
    /// can also store a list of (declaration, index) mappings.
    class ShadowMapEntry {
      typedef SmallVector<DeclIndexPair, 4> DeclIndexPairVector;

      /// \brief Contains either the solitary NamedDecl * or a vector
      /// of (declaration, index) pairs.
      llvm::PointerUnion<NamedDecl *, DeclIndexPairVector*> DeclOrVector;

      /// \brief When the entry contains a single declaration, this is
      /// the index associated with that entry.
      unsigned SingleDeclIndex;

    public:
      ShadowMapEntry() : DeclOrVector(), SingleDeclIndex(0) { }

      void Add(NamedDecl *ND, unsigned Index) {
        if (DeclOrVector.isNull()) {
          // 0 - > 1 elements: just set the single element information.
          DeclOrVector = ND;
          SingleDeclIndex = Index;
          return;
        }

        if (NamedDecl *PrevND = DeclOrVector.dyn_cast<NamedDecl *>()) {
          // 1 -> 2 elements: create the vector of results and push in the
          // existing declaration.
          DeclIndexPairVector *Vec = new DeclIndexPairVector;
          Vec->push_back(DeclIndexPair(PrevND, SingleDeclIndex));
          DeclOrVector = Vec;
        }

        // Add the new element to the end of the vector.
        DeclOrVector.get<DeclIndexPairVector*>()->push_back(
                                                    DeclIndexPair(ND, Index));
      }

      void Destroy() {
        if (DeclIndexPairVector *Vec
              = DeclOrVector.dyn_cast<DeclIndexPairVector *>()) {
          delete Vec;
          DeclOrVector = ((NamedDecl *)0);
        }
      }

      // Iteration.
      class iterator;
      iterator begin() const;
      iterator end() const;
    };

    /// \brief A mapping from declaration names to the declarations that have
    /// this name within a particular scope and their index within the list of
    /// results.
    typedef llvm::DenseMap<DeclarationName, ShadowMapEntry> ShadowMap;
    
    /// \brief The semantic analysis object for which results are being 
    /// produced.
    Sema &SemaRef;

    /// \brief The allocator used to allocate new code-completion strings.
    CodeCompletionAllocator &Allocator;
    
    /// \brief If non-NULL, a filter function used to remove any code-completion
    /// results that are not desirable.
    LookupFilter Filter;

    /// \brief Whether we should allow declarations as
    /// nested-name-specifiers that would otherwise be filtered out.
    bool AllowNestedNameSpecifiers;

    /// \brief If set, the type that we would prefer our resulting value
    /// declarations to have.
    ///
    /// Closely matching the preferred type gives a boost to a result's 
    /// priority.
    CanQualType PreferredType;
    
    /// \brief A list of shadow maps, which is used to model name hiding at
    /// different levels of, e.g., the inheritance hierarchy.
    std::list<ShadowMap> ShadowMaps;
    
    /// \brief If we're potentially referring to a C++ member function, the set
    /// of qualifiers applied to the object type.
    Qualifiers ObjectTypeQualifiers;
    
    /// \brief Whether the \p ObjectTypeQualifiers field is active.
    bool HasObjectTypeQualifiers;
    
    /// \brief The selector that we prefer.
    Selector PreferredSelector;
    
    /// \brief The completion context in which we are gathering results.
    CodeCompletionContext CompletionContext;
    
    /// \brief If we are in an instance method definition, the @implementation
    /// object.
    ObjCImplementationDecl *ObjCImplementation;
    
    void AdjustResultPriorityForDecl(Result &R);

    void MaybeAddConstructorResults(Result R);
    
  public:
    explicit ResultBuilder(Sema &SemaRef, CodeCompletionAllocator &Allocator,
                           const CodeCompletionContext &CompletionContext,
                           LookupFilter Filter = 0)
      : SemaRef(SemaRef), Allocator(Allocator), Filter(Filter), 
        AllowNestedNameSpecifiers(false), HasObjectTypeQualifiers(false), 
        CompletionContext(CompletionContext),
        ObjCImplementation(0) 
    { 
      // If this is an Objective-C instance method definition, dig out the 
      // corresponding implementation.
      switch (CompletionContext.getKind()) {
      case CodeCompletionContext::CCC_Expression:
      case CodeCompletionContext::CCC_ObjCMessageReceiver:
      case CodeCompletionContext::CCC_ParenthesizedExpression:
      case CodeCompletionContext::CCC_Statement:
      case CodeCompletionContext::CCC_Recovery:
        if (ObjCMethodDecl *Method = SemaRef.getCurMethodDecl())
          if (Method->isInstanceMethod())
            if (ObjCInterfaceDecl *Interface = Method->getClassInterface())
              ObjCImplementation = Interface->getImplementation();
        break;
          
      default:
        break;
      }
    }
    
    /// \brief Whether we should include code patterns in the completion
    /// results.
    bool includeCodePatterns() const {
      return SemaRef.CodeCompleter && 
             SemaRef.CodeCompleter->includeCodePatterns();
    }
    
    /// \brief Set the filter used for code-completion results.
    void setFilter(LookupFilter Filter) {
      this->Filter = Filter;
    }
    
    Result *data() { return Results.empty()? 0 : &Results.front(); }
    unsigned size() const { return Results.size(); }
    bool empty() const { return Results.empty(); }
    
    /// \brief Specify the preferred type.
    void setPreferredType(QualType T) { 
      PreferredType = SemaRef.Context.getCanonicalType(T); 
    }
    
    /// \brief Set the cv-qualifiers on the object type, for us in filtering
    /// calls to member functions.
    ///
    /// When there are qualifiers in this set, they will be used to filter
    /// out member functions that aren't available (because there will be a 
    /// cv-qualifier mismatch) or prefer functions with an exact qualifier
    /// match.
    void setObjectTypeQualifiers(Qualifiers Quals) {
      ObjectTypeQualifiers = Quals;
      HasObjectTypeQualifiers = true;
    }
    
    /// \brief Set the preferred selector.
    ///
    /// When an Objective-C method declaration result is added, and that
    /// method's selector matches this preferred selector, we give that method
    /// a slight priority boost.
    void setPreferredSelector(Selector Sel) {
      PreferredSelector = Sel;
    }
        
    /// \brief Retrieve the code-completion context for which results are
    /// being collected.
    const CodeCompletionContext &getCompletionContext() const { 
      return CompletionContext; 
    }
    
    /// \brief Specify whether nested-name-specifiers are allowed.
    void allowNestedNameSpecifiers(bool Allow = true) {
      AllowNestedNameSpecifiers = Allow;
    }

    /// \brief Return the semantic analysis object for which we are collecting
    /// code completion results.
    Sema &getSema() const { return SemaRef; }
    
    /// \brief Retrieve the allocator used to allocate code completion strings.
    CodeCompletionAllocator &getAllocator() const { return Allocator; }
    
    /// \brief Determine whether the given declaration is at all interesting
    /// as a code-completion result.
    ///
    /// \param ND the declaration that we are inspecting.
    ///
    /// \param AsNestedNameSpecifier will be set true if this declaration is
    /// only interesting when it is a nested-name-specifier.
    bool isInterestingDecl(NamedDecl *ND, bool &AsNestedNameSpecifier) const;
    
    /// \brief Check whether the result is hidden by the Hiding declaration.
    ///
    /// \returns true if the result is hidden and cannot be found, false if
    /// the hidden result could still be found. When false, \p R may be
    /// modified to describe how the result can be found (e.g., via extra
    /// qualification).
    bool CheckHiddenResult(Result &R, DeclContext *CurContext,
                           NamedDecl *Hiding);
    
    /// \brief Add a new result to this result set (if it isn't already in one
    /// of the shadow maps), or replace an existing result (for, e.g., a 
    /// redeclaration).
    ///
    /// \param CurContext the result to add (if it is unique).
    ///
    /// \param R the context in which this result will be named.
    void MaybeAddResult(Result R, DeclContext *CurContext = 0);
    
    /// \brief Add a new result to this result set, where we already know
    /// the hiding declation (if any).
    ///
    /// \param R the result to add (if it is unique).
    ///
    /// \param CurContext the context in which this result will be named.
    ///
    /// \param Hiding the declaration that hides the result.
    ///
    /// \param InBaseClass whether the result was found in a base
    /// class of the searched context.
    void AddResult(Result R, DeclContext *CurContext, NamedDecl *Hiding,
                   bool InBaseClass);
    
    /// \brief Add a new non-declaration result to this result set.
    void AddResult(Result R);

    /// \brief Enter into a new scope.
    void EnterNewScope();
    
    /// \brief Exit from the current scope.
    void ExitScope();
    
    /// \brief Ignore this declaration, if it is seen again.
    void Ignore(Decl *D) { AllDeclsFound.insert(D->getCanonicalDecl()); }

    /// \name Name lookup predicates
    ///
    /// These predicates can be passed to the name lookup functions to filter the
    /// results of name lookup. All of the predicates have the same type, so that
    /// 
    //@{
    bool IsOrdinaryName(NamedDecl *ND) const;
    bool IsOrdinaryNonTypeName(NamedDecl *ND) const;
    bool IsIntegralConstantValue(NamedDecl *ND) const;
    bool IsOrdinaryNonValueName(NamedDecl *ND) const;
    bool IsNestedNameSpecifier(NamedDecl *ND) const;
    bool IsEnum(NamedDecl *ND) const;
    bool IsClassOrStruct(NamedDecl *ND) const;
    bool IsUnion(NamedDecl *ND) const;
    bool IsNamespace(NamedDecl *ND) const;
    bool IsNamespaceOrAlias(NamedDecl *ND) const;
    bool IsType(NamedDecl *ND) const;
    bool IsMember(NamedDecl *ND) const;
    bool IsObjCIvar(NamedDecl *ND) const;
    bool IsObjCMessageReceiver(NamedDecl *ND) const;
    bool IsObjCCollection(NamedDecl *ND) const;
    bool IsImpossibleToSatisfy(NamedDecl *ND) const;
    //@}    
  };  
}

class ResultBuilder::ShadowMapEntry::iterator {
  llvm::PointerUnion<NamedDecl*, const DeclIndexPair*> DeclOrIterator;
  unsigned SingleDeclIndex;

public:
  typedef DeclIndexPair value_type;
  typedef value_type reference;
  typedef std::ptrdiff_t difference_type;
  typedef std::input_iterator_tag iterator_category;
        
  class pointer {
    DeclIndexPair Value;

  public:
    pointer(const DeclIndexPair &Value) : Value(Value) { }

    const DeclIndexPair *operator->() const {
      return &Value;
    }
  };
        
  iterator() : DeclOrIterator((NamedDecl *)0), SingleDeclIndex(0) { }

  iterator(NamedDecl *SingleDecl, unsigned Index)
    : DeclOrIterator(SingleDecl), SingleDeclIndex(Index) { }

  iterator(const DeclIndexPair *Iterator)
    : DeclOrIterator(Iterator), SingleDeclIndex(0) { }

  iterator &operator++() {
    if (DeclOrIterator.is<NamedDecl *>()) {
      DeclOrIterator = (NamedDecl *)0;
      SingleDeclIndex = 0;
      return *this;
    }

    const DeclIndexPair *I = DeclOrIterator.get<const DeclIndexPair*>();
    ++I;
    DeclOrIterator = I;
    return *this;
  }

  /*iterator operator++(int) {
    iterator tmp(*this);
    ++(*this);
    return tmp;
  }*/

  reference operator*() const {
    if (NamedDecl *ND = DeclOrIterator.dyn_cast<NamedDecl *>())
      return reference(ND, SingleDeclIndex);

    return *DeclOrIterator.get<const DeclIndexPair*>();
  }

  pointer operator->() const {
    return pointer(**this);
  }

  friend bool operator==(const iterator &X, const iterator &Y) {
    return X.DeclOrIterator.getOpaqueValue()
                                  == Y.DeclOrIterator.getOpaqueValue() &&
      X.SingleDeclIndex == Y.SingleDeclIndex;
  }

  friend bool operator!=(const iterator &X, const iterator &Y) {
    return !(X == Y);
  }
};

ResultBuilder::ShadowMapEntry::iterator 
ResultBuilder::ShadowMapEntry::begin() const {
  if (DeclOrVector.isNull())
    return iterator();

  if (NamedDecl *ND = DeclOrVector.dyn_cast<NamedDecl *>())
    return iterator(ND, SingleDeclIndex);

  return iterator(DeclOrVector.get<DeclIndexPairVector *>()->begin());
}

ResultBuilder::ShadowMapEntry::iterator 
ResultBuilder::ShadowMapEntry::end() const {
  if (DeclOrVector.is<NamedDecl *>() || DeclOrVector.isNull())
    return iterator();

  return iterator(DeclOrVector.get<DeclIndexPairVector *>()->end());
}

/// \brief Compute the qualification required to get from the current context
/// (\p CurContext) to the target context (\p TargetContext).
///
/// \param Context the AST context in which the qualification will be used.
///
/// \param CurContext the context where an entity is being named, which is
/// typically based on the current scope.
///
/// \param TargetContext the context in which the named entity actually 
/// resides.
///
/// \returns a nested name specifier that refers into the target context, or
/// NULL if no qualification is needed.
static NestedNameSpecifier *
getRequiredQualification(ASTContext &Context,
                         DeclContext *CurContext,
                         DeclContext *TargetContext) {
  SmallVector<DeclContext *, 4> TargetParents;
  
  for (DeclContext *CommonAncestor = TargetContext;
       CommonAncestor && !CommonAncestor->Encloses(CurContext);
       CommonAncestor = CommonAncestor->getLookupParent()) {
    if (CommonAncestor->isTransparentContext() ||
        CommonAncestor->isFunctionOrMethod())
      continue;
    
    TargetParents.push_back(CommonAncestor);
  }
  
  NestedNameSpecifier *Result = 0;
  while (!TargetParents.empty()) {
    DeclContext *Parent = TargetParents.back();
    TargetParents.pop_back();
    
    if (NamespaceDecl *Namespace = dyn_cast<NamespaceDecl>(Parent)) {
      if (!Namespace->getIdentifier())
        continue;

      Result = NestedNameSpecifier::Create(Context, Result, Namespace);
    }
    else if (TagDecl *TD = dyn_cast<TagDecl>(Parent))
      Result = NestedNameSpecifier::Create(Context, Result,
                                           false,
                                     Context.getTypeDeclType(TD).getTypePtr());
  }  
  return Result;
}

bool ResultBuilder::isInterestingDecl(NamedDecl *ND, 
                                      bool &AsNestedNameSpecifier) const {
  AsNestedNameSpecifier = false;

  ND = ND->getUnderlyingDecl();
  unsigned IDNS = ND->getIdentifierNamespace();

  // Skip unnamed entities.
  if (!ND->getDeclName())
    return false;
  
  // Friend declarations and declarations introduced due to friends are never
  // added as results.
  if (IDNS & (Decl::IDNS_OrdinaryFriend | Decl::IDNS_TagFriend))
    return false;
  
  // Class template (partial) specializations are never added as results.
  if (isa<ClassTemplateSpecializationDecl>(ND) ||
      isa<ClassTemplatePartialSpecializationDecl>(ND))
    return false;
  
  // Using declarations themselves are never added as results.
  if (isa<UsingDecl>(ND))
    return false;
  
  // Some declarations have reserved names that we don't want to ever show.
  if (const IdentifierInfo *Id = ND->getIdentifier()) {
    // __va_list_tag is a freak of nature. Find it and skip it.
    if (Id->isStr("__va_list_tag") || Id->isStr("__builtin_va_list"))
      return false;
    
    // Filter out names reserved for the implementation (C99 7.1.3, 
    // C++ [lib.global.names]) if they come from a system header.
    //
    // FIXME: Add predicate for this.
    if (Id->getLength() >= 2) {
      const char *Name = Id->getNameStart();
      if (Name[0] == '_' &&
          (Name[1] == '_' || (Name[1] >= 'A' && Name[1] <= 'Z')) &&
          (ND->getLocation().isInvalid() ||
           SemaRef.SourceMgr.isInSystemHeader(
                          SemaRef.SourceMgr.getSpellingLoc(ND->getLocation()))))
        return false;
    }
  }
   
  // Skip out-of-line declarations and definitions.
  // NOTE: Unless it's an Objective-C property, method, or ivar, where
  // the contexts can be messy.
  if (!ND->getDeclContext()->Equals(ND->getLexicalDeclContext()) &&
      !(isa<ObjCPropertyDecl>(ND) || isa<ObjCIvarDecl>(ND) ||
        isa<ObjCMethodDecl>(ND)))
    return false;

  if (Filter == &ResultBuilder::IsNestedNameSpecifier ||
      ((isa<NamespaceDecl>(ND) || isa<NamespaceAliasDecl>(ND)) &&
       Filter != &ResultBuilder::IsNamespace &&
       Filter != &ResultBuilder::IsNamespaceOrAlias &&
       Filter != 0))
    AsNestedNameSpecifier = true;

  // Filter out any unwanted results.
  if (Filter && !(this->*Filter)(ND)) {
    // Check whether it is interesting as a nested-name-specifier.
    if (AllowNestedNameSpecifiers && SemaRef.getLangOptions().CPlusPlus && 
        IsNestedNameSpecifier(ND) &&
        (Filter != &ResultBuilder::IsMember ||
         (isa<CXXRecordDecl>(ND) && 
          cast<CXXRecordDecl>(ND)->isInjectedClassName()))) {
      AsNestedNameSpecifier = true;
      return true;
    }

    return false;
  }  
  // ... then it must be interesting!
  return true;
}

bool ResultBuilder::CheckHiddenResult(Result &R, DeclContext *CurContext,
                                      NamedDecl *Hiding) {
  // In C, there is no way to refer to a hidden name.
  // FIXME: This isn't true; we can find a tag name hidden by an ordinary
  // name if we introduce the tag type.
  if (!SemaRef.getLangOptions().CPlusPlus)
    return true;
  
  DeclContext *HiddenCtx = R.Declaration->getDeclContext()->getRedeclContext();
  
  // There is no way to qualify a name declared in a function or method.
  if (HiddenCtx->isFunctionOrMethod())
    return true;
  
  if (HiddenCtx == Hiding->getDeclContext()->getRedeclContext())
    return true;
  
  // We can refer to the result with the appropriate qualification. Do it.
  R.Hidden = true;
  R.QualifierIsInformative = false;
  
  if (!R.Qualifier)
    R.Qualifier = getRequiredQualification(SemaRef.Context, 
                                           CurContext, 
                                           R.Declaration->getDeclContext());
  return false;
}

/// \brief A simplified classification of types used to determine whether two
/// types are "similar enough" when adjusting priorities.
SimplifiedTypeClass clang::getSimplifiedTypeClass(CanQualType T) {
  switch (T->getTypeClass()) {
  case Type::Builtin:
    switch (cast<BuiltinType>(T)->getKind()) {
      case BuiltinType::Void:
        return STC_Void;
        
      case BuiltinType::NullPtr:
        return STC_Pointer;
        
      case BuiltinType::Overload:
      case BuiltinType::Dependent:
        return STC_Other;
        
      case BuiltinType::ObjCId:
      case BuiltinType::ObjCClass:
      case BuiltinType::ObjCSel:
        return STC_ObjectiveC;
        
      default:
        return STC_Arithmetic;
    }
    return STC_Other;
    
  case Type::Complex:
    return STC_Arithmetic;
    
  case Type::Pointer:
    return STC_Pointer;
    
  case Type::BlockPointer:
    return STC_Block;
    
  case Type::LValueReference:
  case Type::RValueReference:
    return getSimplifiedTypeClass(T->getAs<ReferenceType>()->getPointeeType());
    
  case Type::ConstantArray:
  case Type::IncompleteArray:
  case Type::VariableArray:
  case Type::DependentSizedArray:
    return STC_Array;
    
  case Type::DependentSizedExtVector:
  case Type::Vector:
  case Type::ExtVector:
    return STC_Arithmetic;
    
  case Type::FunctionProto:
  case Type::FunctionNoProto:
    return STC_Function;
    
  case Type::Record:
    return STC_Record;
    
  case Type::Enum:
    return STC_Arithmetic;
    
  case Type::ObjCObject:
  case Type::ObjCInterface:
  case Type::ObjCObjectPointer:
    return STC_ObjectiveC;
    
  default:
    return STC_Other;
  }
}

/// \brief Get the type that a given expression will have if this declaration
/// is used as an expression in its "typical" code-completion form.
QualType clang::getDeclUsageType(ASTContext &C, NamedDecl *ND) {
  ND = cast<NamedDecl>(ND->getUnderlyingDecl());
  
  if (TypeDecl *Type = dyn_cast<TypeDecl>(ND))
    return C.getTypeDeclType(Type);
  if (ObjCInterfaceDecl *Iface = dyn_cast<ObjCInterfaceDecl>(ND))
    return C.getObjCInterfaceType(Iface);
  
  QualType T;
  if (FunctionDecl *Function = dyn_cast<FunctionDecl>(ND))
    T = Function->getCallResultType();
  else if (ObjCMethodDecl *Method = dyn_cast<ObjCMethodDecl>(ND))
    T = Method->getSendResultType();
  else if (FunctionTemplateDecl *FunTmpl = dyn_cast<FunctionTemplateDecl>(ND))
    T = FunTmpl->getTemplatedDecl()->getCallResultType();
  else if (EnumConstantDecl *Enumerator = dyn_cast<EnumConstantDecl>(ND))
    T = C.getTypeDeclType(cast<EnumDecl>(Enumerator->getDeclContext()));
  else if (ObjCPropertyDecl *Property = dyn_cast<ObjCPropertyDecl>(ND))
    T = Property->getType();
  else if (ValueDecl *Value = dyn_cast<ValueDecl>(ND))
    T = Value->getType();
  else
    return QualType();

  // Dig through references, function pointers, and block pointers to
  // get down to the likely type of an expression when the entity is
  // used.
  do {
    if (const ReferenceType *Ref = T->getAs<ReferenceType>()) {
      T = Ref->getPointeeType();
      continue;
    }

    if (const PointerType *Pointer = T->getAs<PointerType>()) {
      if (Pointer->getPointeeType()->isFunctionType()) {
        T = Pointer->getPointeeType();
        continue;
      }

      break;
    }

    if (const BlockPointerType *Block = T->getAs<BlockPointerType>()) {
      T = Block->getPointeeType();
      continue;
    }

    if (const FunctionType *Function = T->getAs<FunctionType>()) {
      T = Function->getResultType();
      continue;
    }

    break;
  } while (true);
    
  return T;
}

void ResultBuilder::AdjustResultPriorityForDecl(Result &R) {
  // If this is an Objective-C method declaration whose selector matches our
  // preferred selector, give it a priority boost.
  if (!PreferredSelector.isNull())
    if (ObjCMethodDecl *Method = dyn_cast<ObjCMethodDecl>(R.Declaration))
      if (PreferredSelector == Method->getSelector())
        R.Priority += CCD_SelectorMatch;
  
  // If we have a preferred type, adjust the priority for results with exactly-
  // matching or nearly-matching types.
  if (!PreferredType.isNull()) {
    QualType T = getDeclUsageType(SemaRef.Context, R.Declaration);
    if (!T.isNull()) {
      CanQualType TC = SemaRef.Context.getCanonicalType(T);
      // Check for exactly-matching types (modulo qualifiers).
      if (SemaRef.Context.hasSameUnqualifiedType(PreferredType, TC))
        R.Priority /= CCF_ExactTypeMatch;
      // Check for nearly-matching types, based on classification of each.
      else if ((getSimplifiedTypeClass(PreferredType)
                                               == getSimplifiedTypeClass(TC)) &&
               !(PreferredType->isEnumeralType() && TC->isEnumeralType()))
        R.Priority /= CCF_SimilarTypeMatch;  
    }
  }  
}

void ResultBuilder::MaybeAddConstructorResults(Result R) {
  if (!SemaRef.getLangOptions().CPlusPlus || !R.Declaration ||
      !CompletionContext.wantConstructorResults())
    return;
  
  ASTContext &Context = SemaRef.Context;
  NamedDecl *D = R.Declaration;
  CXXRecordDecl *Record = 0;
  if (ClassTemplateDecl *ClassTemplate = dyn_cast<ClassTemplateDecl>(D))
    Record = ClassTemplate->getTemplatedDecl();
  else if ((Record = dyn_cast<CXXRecordDecl>(D))) {
    // Skip specializations and partial specializations.
    if (isa<ClassTemplateSpecializationDecl>(Record))
      return;
  } else {
    // There are no constructors here.
    return;
  }
  
  Record = Record->getDefinition();
  if (!Record)
    return;

  
  QualType RecordTy = Context.getTypeDeclType(Record);
  DeclarationName ConstructorName
    = Context.DeclarationNames.getCXXConstructorName(
                                           Context.getCanonicalType(RecordTy));
  for (DeclContext::lookup_result Ctors = Record->lookup(ConstructorName);
       Ctors.first != Ctors.second; ++Ctors.first) {
    R.Declaration = *Ctors.first;
    R.CursorKind = getCursorKindForDecl(R.Declaration);
    Results.push_back(R);
  }
}

void ResultBuilder::MaybeAddResult(Result R, DeclContext *CurContext) {
  assert(!ShadowMaps.empty() && "Must enter into a results scope");
  
  if (R.Kind != Result::RK_Declaration) {
    // For non-declaration results, just add the result.
    Results.push_back(R);
    return;
  }

  // Look through using declarations.
  if (UsingShadowDecl *Using = dyn_cast<UsingShadowDecl>(R.Declaration)) {
    MaybeAddResult(Result(Using->getTargetDecl(), R.Qualifier), CurContext);
    return;
  }
  
  Decl *CanonDecl = R.Declaration->getCanonicalDecl();
  unsigned IDNS = CanonDecl->getIdentifierNamespace();

  bool AsNestedNameSpecifier = false;
  if (!isInterestingDecl(R.Declaration, AsNestedNameSpecifier))
    return;
      
  // C++ constructors are never found by name lookup.
  if (isa<CXXConstructorDecl>(R.Declaration))
    return;

  ShadowMap &SMap = ShadowMaps.back();
  ShadowMapEntry::iterator I, IEnd;
  ShadowMap::iterator NamePos = SMap.find(R.Declaration->getDeclName());
  if (NamePos != SMap.end()) {
    I = NamePos->second.begin();
    IEnd = NamePos->second.end();
  }

  for (; I != IEnd; ++I) {
    NamedDecl *ND = I->first;
    unsigned Index = I->second;
    if (ND->getCanonicalDecl() == CanonDecl) {
      // This is a redeclaration. Always pick the newer declaration.
      Results[Index].Declaration = R.Declaration;
      
      // We're done.
      return;
    }
  }
  
  // This is a new declaration in this scope. However, check whether this
  // declaration name is hidden by a similarly-named declaration in an outer
  // scope.
  std::list<ShadowMap>::iterator SM, SMEnd = ShadowMaps.end();
  --SMEnd;
  for (SM = ShadowMaps.begin(); SM != SMEnd; ++SM) {
    ShadowMapEntry::iterator I, IEnd;
    ShadowMap::iterator NamePos = SM->find(R.Declaration->getDeclName());
    if (NamePos != SM->end()) {
      I = NamePos->second.begin();
      IEnd = NamePos->second.end();
    }
    for (; I != IEnd; ++I) {
      // A tag declaration does not hide a non-tag declaration.
      if (I->first->hasTagIdentifierNamespace() &&
          (IDNS & (Decl::IDNS_Member | Decl::IDNS_Ordinary | 
                   Decl::IDNS_ObjCProtocol)))
        continue;
      
      // Protocols are in distinct namespaces from everything else.
      if (((I->first->getIdentifierNamespace() & Decl::IDNS_ObjCProtocol)
           || (IDNS & Decl::IDNS_ObjCProtocol)) &&
          I->first->getIdentifierNamespace() != IDNS)
        continue;
      
      // The newly-added result is hidden by an entry in the shadow map.
      if (CheckHiddenResult(R, CurContext, I->first))
        return;
      
      break;
    }
  }
  
  // Make sure that any given declaration only shows up in the result set once.
  if (!AllDeclsFound.insert(CanonDecl))
    return;

  // If the filter is for nested-name-specifiers, then this result starts a
  // nested-name-specifier.
  if (AsNestedNameSpecifier) {
    R.StartsNestedNameSpecifier = true;
    R.Priority = CCP_NestedNameSpecifier;
  } else 
      AdjustResultPriorityForDecl(R);
      
  // If this result is supposed to have an informative qualifier, add one.
  if (R.QualifierIsInformative && !R.Qualifier &&
      !R.StartsNestedNameSpecifier) {
    DeclContext *Ctx = R.Declaration->getDeclContext();
    if (NamespaceDecl *Namespace = dyn_cast<NamespaceDecl>(Ctx))
      R.Qualifier = NestedNameSpecifier::Create(SemaRef.Context, 0, Namespace);
    else if (TagDecl *Tag = dyn_cast<TagDecl>(Ctx))
      R.Qualifier = NestedNameSpecifier::Create(SemaRef.Context, 0, false, 
                             SemaRef.Context.getTypeDeclType(Tag).getTypePtr());
    else
      R.QualifierIsInformative = false;
  }
    
  // Insert this result into the set of results and into the current shadow
  // map.
  SMap[R.Declaration->getDeclName()].Add(R.Declaration, Results.size());
  Results.push_back(R);
  
  if (!AsNestedNameSpecifier)
    MaybeAddConstructorResults(R);
}

void ResultBuilder::AddResult(Result R, DeclContext *CurContext, 
                              NamedDecl *Hiding, bool InBaseClass = false) {
  if (R.Kind != Result::RK_Declaration) {
    // For non-declaration results, just add the result.
    Results.push_back(R);
    return;
  }

  // Look through using declarations.
  if (UsingShadowDecl *Using = dyn_cast<UsingShadowDecl>(R.Declaration)) {
    AddResult(Result(Using->getTargetDecl(), R.Qualifier), CurContext, Hiding);
    return;
  }
  
  bool AsNestedNameSpecifier = false;
  if (!isInterestingDecl(R.Declaration, AsNestedNameSpecifier))
    return;
  
  // C++ constructors are never found by name lookup.
  if (isa<CXXConstructorDecl>(R.Declaration))
    return;

  if (Hiding && CheckHiddenResult(R, CurContext, Hiding))
    return;
      
  // Make sure that any given declaration only shows up in the result set once.
  if (!AllDeclsFound.insert(R.Declaration->getCanonicalDecl()))
    return;
  
  // If the filter is for nested-name-specifiers, then this result starts a
  // nested-name-specifier.
  if (AsNestedNameSpecifier) {
    R.StartsNestedNameSpecifier = true;
    R.Priority = CCP_NestedNameSpecifier;
  }
  else if (Filter == &ResultBuilder::IsMember && !R.Qualifier && InBaseClass &&
           isa<CXXRecordDecl>(R.Declaration->getDeclContext()
                                                  ->getRedeclContext()))
    R.QualifierIsInformative = true;

  // If this result is supposed to have an informative qualifier, add one.
  if (R.QualifierIsInformative && !R.Qualifier &&
      !R.StartsNestedNameSpecifier) {
    DeclContext *Ctx = R.Declaration->getDeclContext();
    if (NamespaceDecl *Namespace = dyn_cast<NamespaceDecl>(Ctx))
      R.Qualifier = NestedNameSpecifier::Create(SemaRef.Context, 0, Namespace);
    else if (TagDecl *Tag = dyn_cast<TagDecl>(Ctx))
      R.Qualifier = NestedNameSpecifier::Create(SemaRef.Context, 0, false, 
                            SemaRef.Context.getTypeDeclType(Tag).getTypePtr());
    else
      R.QualifierIsInformative = false;
  }
  
  // Adjust the priority if this result comes from a base class.
  if (InBaseClass)
    R.Priority += CCD_InBaseClass;
  
  AdjustResultPriorityForDecl(R);
  
  if (HasObjectTypeQualifiers)
    if (CXXMethodDecl *Method = dyn_cast<CXXMethodDecl>(R.Declaration))
      if (Method->isInstance()) {
        Qualifiers MethodQuals
                        = Qualifiers::fromCVRMask(Method->getTypeQualifiers());
        if (ObjectTypeQualifiers == MethodQuals)
          R.Priority += CCD_ObjectQualifierMatch;
        else if (ObjectTypeQualifiers - MethodQuals) {
          // The method cannot be invoked, because doing so would drop 
          // qualifiers.
          return;
        }
      }
  
  // Insert this result into the set of results.
  Results.push_back(R);
  
  if (!AsNestedNameSpecifier)
    MaybeAddConstructorResults(R);
}

void ResultBuilder::AddResult(Result R) {
  assert(R.Kind != Result::RK_Declaration && 
          "Declaration results need more context");
  Results.push_back(R);
}

/// \brief Enter into a new scope.
void ResultBuilder::EnterNewScope() {
  ShadowMaps.push_back(ShadowMap());
}

/// \brief Exit from the current scope.
void ResultBuilder::ExitScope() {
  for (ShadowMap::iterator E = ShadowMaps.back().begin(),
                        EEnd = ShadowMaps.back().end();
       E != EEnd;
       ++E)
    E->second.Destroy();
         
  ShadowMaps.pop_back();
}

/// \brief Determines whether this given declaration will be found by
/// ordinary name lookup.
bool ResultBuilder::IsOrdinaryName(NamedDecl *ND) const {
  ND = cast<NamedDecl>(ND->getUnderlyingDecl());

  unsigned IDNS = Decl::IDNS_Ordinary;
  if (SemaRef.getLangOptions().CPlusPlus)
    IDNS |= Decl::IDNS_Tag | Decl::IDNS_Namespace | Decl::IDNS_Member;
  else if (SemaRef.getLangOptions().ObjC1) {
    if (isa<ObjCIvarDecl>(ND))
      return true;
  }
  
  return ND->getIdentifierNamespace() & IDNS;
}

/// \brief Determines whether this given declaration will be found by
/// ordinary name lookup but is not a type name.
bool ResultBuilder::IsOrdinaryNonTypeName(NamedDecl *ND) const {
  ND = cast<NamedDecl>(ND->getUnderlyingDecl());
  if (isa<TypeDecl>(ND) || isa<ObjCInterfaceDecl>(ND))
    return false;
  
  unsigned IDNS = Decl::IDNS_Ordinary;
  if (SemaRef.getLangOptions().CPlusPlus)
    IDNS |= Decl::IDNS_Tag | Decl::IDNS_Namespace | Decl::IDNS_Member;
  else if (SemaRef.getLangOptions().ObjC1) {
    if (isa<ObjCIvarDecl>(ND))
      return true;
  }
 
  return ND->getIdentifierNamespace() & IDNS;
}

bool ResultBuilder::IsIntegralConstantValue(NamedDecl *ND) const {
  if (!IsOrdinaryNonTypeName(ND))
    return 0;
  
  if (ValueDecl *VD = dyn_cast<ValueDecl>(ND->getUnderlyingDecl()))
    if (VD->getType()->isIntegralOrEnumerationType())
      return true;
        
  return false;
}

/// \brief Determines whether this given declaration will be found by
/// ordinary name lookup.
bool ResultBuilder::IsOrdinaryNonValueName(NamedDecl *ND) const {
  ND = cast<NamedDecl>(ND->getUnderlyingDecl());

  unsigned IDNS = Decl::IDNS_Ordinary;
  if (SemaRef.getLangOptions().CPlusPlus)
    IDNS |= Decl::IDNS_Tag | Decl::IDNS_Namespace;
  
  return (ND->getIdentifierNamespace() & IDNS) && 
    !isa<ValueDecl>(ND) && !isa<FunctionTemplateDecl>(ND) && 
    !isa<ObjCPropertyDecl>(ND);
}

/// \brief Determines whether the given declaration is suitable as the 
/// start of a C++ nested-name-specifier, e.g., a class or namespace.
bool ResultBuilder::IsNestedNameSpecifier(NamedDecl *ND) const {
  // Allow us to find class templates, too.
  if (ClassTemplateDecl *ClassTemplate = dyn_cast<ClassTemplateDecl>(ND))
    ND = ClassTemplate->getTemplatedDecl();
  
  return SemaRef.isAcceptableNestedNameSpecifier(ND);
}

/// \brief Determines whether the given declaration is an enumeration.
bool ResultBuilder::IsEnum(NamedDecl *ND) const {
  return isa<EnumDecl>(ND);
}

/// \brief Determines whether the given declaration is a class or struct.
bool ResultBuilder::IsClassOrStruct(NamedDecl *ND) const {
  // Allow us to find class templates, too.
  if (ClassTemplateDecl *ClassTemplate = dyn_cast<ClassTemplateDecl>(ND))
    ND = ClassTemplate->getTemplatedDecl();
  
  if (RecordDecl *RD = dyn_cast<RecordDecl>(ND))
    return RD->getTagKind() == TTK_Class ||
    RD->getTagKind() == TTK_Struct;
  
  return false;
}

/// \brief Determines whether the given declaration is a union.
bool ResultBuilder::IsUnion(NamedDecl *ND) const {
  // Allow us to find class templates, too.
  if (ClassTemplateDecl *ClassTemplate = dyn_cast<ClassTemplateDecl>(ND))
    ND = ClassTemplate->getTemplatedDecl();
  
  if (RecordDecl *RD = dyn_cast<RecordDecl>(ND))
    return RD->getTagKind() == TTK_Union;
  
  return false;
}

/// \brief Determines whether the given declaration is a namespace.
bool ResultBuilder::IsNamespace(NamedDecl *ND) const {
  return isa<NamespaceDecl>(ND);
}

/// \brief Determines whether the given declaration is a namespace or 
/// namespace alias.
bool ResultBuilder::IsNamespaceOrAlias(NamedDecl *ND) const {
  return isa<NamespaceDecl>(ND) || isa<NamespaceAliasDecl>(ND);
}

/// \brief Determines whether the given declaration is a type.
bool ResultBuilder::IsType(NamedDecl *ND) const {
  if (UsingShadowDecl *Using = dyn_cast<UsingShadowDecl>(ND))
    ND = Using->getTargetDecl();
  
  return isa<TypeDecl>(ND) || isa<ObjCInterfaceDecl>(ND);
}

/// \brief Determines which members of a class should be visible via
/// "." or "->".  Only value declarations, nested name specifiers, and
/// using declarations thereof should show up.
bool ResultBuilder::IsMember(NamedDecl *ND) const {
  if (UsingShadowDecl *Using = dyn_cast<UsingShadowDecl>(ND))
    ND = Using->getTargetDecl();

  return isa<ValueDecl>(ND) || isa<FunctionTemplateDecl>(ND) ||
    isa<ObjCPropertyDecl>(ND);
}

static bool isObjCReceiverType(ASTContext &C, QualType T) {
  T = C.getCanonicalType(T);
  switch (T->getTypeClass()) {
  case Type::ObjCObject: 
  case Type::ObjCInterface:
  case Type::ObjCObjectPointer:
    return true;
      
  case Type::Builtin:
    switch (cast<BuiltinType>(T)->getKind()) {
    case BuiltinType::ObjCId:
    case BuiltinType::ObjCClass:
    case BuiltinType::ObjCSel:
      return true;
      
    default:
      break;
    }
    return false;
      
  default:
    break;
  }
  
  if (!C.getLangOptions().CPlusPlus)
    return false;

  // FIXME: We could perform more analysis here to determine whether a 
  // particular class type has any conversions to Objective-C types. For now,
  // just accept all class types.
  return T->isDependentType() || T->isRecordType();
}

bool ResultBuilder::IsObjCMessageReceiver(NamedDecl *ND) const {
  QualType T = getDeclUsageType(SemaRef.Context, ND);
  if (T.isNull())
    return false;
  
  T = SemaRef.Context.getBaseElementType(T);
  return isObjCReceiverType(SemaRef.Context, T);
}

bool ResultBuilder::IsObjCCollection(NamedDecl *ND) const {
  if ((SemaRef.getLangOptions().CPlusPlus && !IsOrdinaryName(ND)) ||
      (!SemaRef.getLangOptions().CPlusPlus && !IsOrdinaryNonTypeName(ND)))
    return false;
  
  QualType T = getDeclUsageType(SemaRef.Context, ND);
  if (T.isNull())
    return false;
  
  T = SemaRef.Context.getBaseElementType(T);
  return T->isObjCObjectType() || T->isObjCObjectPointerType() ||
         T->isObjCIdType() || 
         (SemaRef.getLangOptions().CPlusPlus && T->isRecordType());
}

bool ResultBuilder::IsImpossibleToSatisfy(NamedDecl *ND) const {
  return false;
}

/// \rief Determines whether the given declaration is an Objective-C
/// instance variable.
bool ResultBuilder::IsObjCIvar(NamedDecl *ND) const {
  return isa<ObjCIvarDecl>(ND);
}

namespace {
  /// \brief Visible declaration consumer that adds a code-completion result
  /// for each visible declaration.
  class CodeCompletionDeclConsumer : public VisibleDeclConsumer {
    ResultBuilder &Results;
    DeclContext *CurContext;
    
  public:
    CodeCompletionDeclConsumer(ResultBuilder &Results, DeclContext *CurContext)
      : Results(Results), CurContext(CurContext) { }
    
    virtual void FoundDecl(NamedDecl *ND, NamedDecl *Hiding, DeclContext *Ctx,
                           bool InBaseClass) {
      bool Accessible = true;
      if (Ctx)
        Accessible = Results.getSema().IsSimplyAccessible(ND, Ctx);
      
      ResultBuilder::Result Result(ND, 0, false, Accessible);
      Results.AddResult(Result, CurContext, Hiding, InBaseClass);
    }
  };
}

/// \brief Add type specifiers for the current language as keyword results.
static void AddTypeSpecifierResults(const LangOptions &LangOpts,
                                    ResultBuilder &Results) {
  typedef CodeCompletionResult Result;
  Results.AddResult(Result("short", CCP_Type));
  Results.AddResult(Result("long", CCP_Type));
  Results.AddResult(Result("signed", CCP_Type));
  Results.AddResult(Result("unsigned", CCP_Type));
  Results.AddResult(Result("void", CCP_Type));
  Results.AddResult(Result("char", CCP_Type));
  Results.AddResult(Result("int", CCP_Type));
  Results.AddResult(Result("float", CCP_Type));
  Results.AddResult(Result("double", CCP_Type));
  Results.AddResult(Result("enum", CCP_Type));
  Results.AddResult(Result("struct", CCP_Type));
  Results.AddResult(Result("union", CCP_Type));
  Results.AddResult(Result("const", CCP_Type));
  Results.AddResult(Result("volatile", CCP_Type));

  if (LangOpts.C99) {
    // C99-specific
    Results.AddResult(Result("_Complex", CCP_Type));
    Results.AddResult(Result("_Imaginary", CCP_Type));
    Results.AddResult(Result("_Bool", CCP_Type));
    Results.AddResult(Result("restrict", CCP_Type));
  }
  
  CodeCompletionBuilder Builder(Results.getAllocator());
  if (LangOpts.CPlusPlus) {
    // C++-specific
    Results.AddResult(Result("bool", CCP_Type + 
                             (LangOpts.ObjC1? CCD_bool_in_ObjC : 0)));
    Results.AddResult(Result("class", CCP_Type));
    Results.AddResult(Result("wchar_t", CCP_Type));
    
    // typename qualified-id
    Builder.AddTypedTextChunk("typename");
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddPlaceholderChunk("qualifier");
    Builder.AddTextChunk("::");
    Builder.AddPlaceholderChunk("name");
    Results.AddResult(Result(Builder.TakeString()));
    
    if (LangOpts.CPlusPlus0x) {
      Results.AddResult(Result("auto", CCP_Type));
      Results.AddResult(Result("char16_t", CCP_Type));
      Results.AddResult(Result("char32_t", CCP_Type));
      
      Builder.AddTypedTextChunk("decltype");
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("expression");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Results.AddResult(Result(Builder.TakeString()));
    }
  }
  
  // GNU extensions
  if (LangOpts.GNUMode) {
    // FIXME: Enable when we actually support decimal floating point.
    //    Results.AddResult(Result("_Decimal32"));
    //    Results.AddResult(Result("_Decimal64"));
    //    Results.AddResult(Result("_Decimal128"));
    
    Builder.AddTypedTextChunk("typeof");
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddPlaceholderChunk("expression");
    Results.AddResult(Result(Builder.TakeString()));

    Builder.AddTypedTextChunk("typeof");
    Builder.AddChunk(CodeCompletionString::CK_LeftParen);
    Builder.AddPlaceholderChunk("type");
    Builder.AddChunk(CodeCompletionString::CK_RightParen);
    Results.AddResult(Result(Builder.TakeString()));
  }
}

static void AddStorageSpecifiers(Sema::ParserCompletionContext CCC,
                                 const LangOptions &LangOpts, 
                                 ResultBuilder &Results) {
  typedef CodeCompletionResult Result;
  // Note: we don't suggest either "auto" or "register", because both
  // are pointless as storage specifiers. Elsewhere, we suggest "auto"
  // in C++0x as a type specifier.
  Results.AddResult(Result("extern"));
  Results.AddResult(Result("static"));
}

static void AddFunctionSpecifiers(Sema::ParserCompletionContext CCC,
                                  const LangOptions &LangOpts, 
                                  ResultBuilder &Results) {
  typedef CodeCompletionResult Result;
  switch (CCC) {
  case Sema::PCC_Class:
  case Sema::PCC_MemberTemplate:
    if (LangOpts.CPlusPlus) {
      Results.AddResult(Result("explicit"));
      Results.AddResult(Result("friend"));
      Results.AddResult(Result("mutable"));
      Results.AddResult(Result("virtual"));
    }    
    // Fall through

  case Sema::PCC_ObjCInterface:
  case Sema::PCC_ObjCImplementation:
  case Sema::PCC_Namespace:
  case Sema::PCC_Template:
    if (LangOpts.CPlusPlus || LangOpts.C99)
      Results.AddResult(Result("inline"));
    break;

  case Sema::PCC_ObjCInstanceVariableList:
  case Sema::PCC_Expression:
  case Sema::PCC_Statement:
  case Sema::PCC_ForInit:
  case Sema::PCC_Condition:
  case Sema::PCC_RecoveryInFunction:
  case Sema::PCC_Type:
  case Sema::PCC_ParenthesizedExpression:
  case Sema::PCC_LocalDeclarationSpecifiers:
    break;
  }
}

static void AddObjCExpressionResults(ResultBuilder &Results, bool NeedAt);
static void AddObjCStatementResults(ResultBuilder &Results, bool NeedAt);
static void AddObjCVisibilityResults(const LangOptions &LangOpts,
                                     ResultBuilder &Results,
                                     bool NeedAt);  
static void AddObjCImplementationResults(const LangOptions &LangOpts,
                                         ResultBuilder &Results,
                                         bool NeedAt);
static void AddObjCInterfaceResults(const LangOptions &LangOpts,
                                    ResultBuilder &Results,
                                    bool NeedAt);
static void AddObjCTopLevelResults(ResultBuilder &Results, bool NeedAt);

static void AddTypedefResult(ResultBuilder &Results) {
  CodeCompletionBuilder Builder(Results.getAllocator());
  Builder.AddTypedTextChunk("typedef");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("type");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("name");
  Results.AddResult(CodeCompletionResult(Builder.TakeString()));        
}

static bool WantTypesInContext(Sema::ParserCompletionContext CCC,
                               const LangOptions &LangOpts) {
  switch (CCC) {
  case Sema::PCC_Namespace:
  case Sema::PCC_Class:
  case Sema::PCC_ObjCInstanceVariableList:
  case Sema::PCC_Template:
  case Sema::PCC_MemberTemplate:
  case Sema::PCC_Statement:
  case Sema::PCC_RecoveryInFunction:
  case Sema::PCC_Type:
  case Sema::PCC_ParenthesizedExpression:
  case Sema::PCC_LocalDeclarationSpecifiers:
    return true;
    
  case Sema::PCC_Expression:
  case Sema::PCC_Condition:
    return LangOpts.CPlusPlus;
      
  case Sema::PCC_ObjCInterface:
  case Sema::PCC_ObjCImplementation:
    return false;
    
  case Sema::PCC_ForInit:
    return LangOpts.CPlusPlus || LangOpts.ObjC1 || LangOpts.C99;
  }
  
  return false;
}

/// \brief Retrieve a printing policy suitable for code completion.
static PrintingPolicy getCompletionPrintingPolicy(Sema &S) {
  PrintingPolicy Policy = S.getPrintingPolicy();
  Policy.AnonymousTagLocations = false;
  Policy.SuppressStrongLifetime = true;
  Policy.SuppressUnwrittenScope = true;
  return Policy;
}

/// \brief Retrieve the string representation of the given type as a string
/// that has the appropriate lifetime for code completion.
///
/// This routine provides a fast path where we provide constant strings for
/// common type names.
static const char *GetCompletionTypeString(QualType T,
                                           ASTContext &Context,
                                           const PrintingPolicy &Policy,
                                           CodeCompletionAllocator &Allocator) {
  if (!T.getLocalQualifiers()) {
    // Built-in type names are constant strings.
    if (const BuiltinType *BT = dyn_cast<BuiltinType>(T))
      return BT->getName(Policy);
    
    // Anonymous tag types are constant strings.
    if (const TagType *TagT = dyn_cast<TagType>(T))
      if (TagDecl *Tag = TagT->getDecl())
        if (!Tag->getIdentifier() && !Tag->getTypedefNameForAnonDecl()) {
          switch (Tag->getTagKind()) {
          case TTK_Struct: return "struct <anonymous>";
          case TTK_Class:  return "class <anonymous>";            
          case TTK_Union:  return "union <anonymous>";
          case TTK_Enum:   return "enum <anonymous>";
          }
        }
  }
  
  // Slow path: format the type as a string.
  std::string Result;
  T.getAsStringInternal(Result, Policy);
  return Allocator.CopyString(Result);
}

/// \brief Add language constructs that show up for "ordinary" names.
static void AddOrdinaryNameResults(Sema::ParserCompletionContext CCC,
                                   Scope *S,
                                   Sema &SemaRef,
                                   ResultBuilder &Results) {
  CodeCompletionAllocator &Allocator = Results.getAllocator();
  CodeCompletionBuilder Builder(Allocator);
  PrintingPolicy Policy = getCompletionPrintingPolicy(SemaRef);
  
  typedef CodeCompletionResult Result;
  switch (CCC) {
  case Sema::PCC_Namespace:
    if (SemaRef.getLangOptions().CPlusPlus) {
      if (Results.includeCodePatterns()) {
        // namespace <identifier> { declarations }
        Builder.AddTypedTextChunk("namespace");
        Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
        Builder.AddPlaceholderChunk("identifier");
        Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
        Builder.AddPlaceholderChunk("declarations");
        Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
        Builder.AddChunk(CodeCompletionString::CK_RightBrace);
        Results.AddResult(Result(Builder.TakeString()));
      }
      
      // namespace identifier = identifier ;
      Builder.AddTypedTextChunk("namespace");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddPlaceholderChunk("name");
      Builder.AddChunk(CodeCompletionString::CK_Equal);
      Builder.AddPlaceholderChunk("namespace");
      Results.AddResult(Result(Builder.TakeString()));

      // Using directives
      Builder.AddTypedTextChunk("using");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddTextChunk("namespace");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddPlaceholderChunk("identifier");
      Results.AddResult(Result(Builder.TakeString()));

      // asm(string-literal)      
      Builder.AddTypedTextChunk("asm");
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("string-literal");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Results.AddResult(Result(Builder.TakeString()));

      if (Results.includeCodePatterns()) {
        // Explicit template instantiation
        Builder.AddTypedTextChunk("template");
        Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
        Builder.AddPlaceholderChunk("declaration");
        Results.AddResult(Result(Builder.TakeString()));
      }
    }
      
    if (SemaRef.getLangOptions().ObjC1)
      AddObjCTopLevelResults(Results, true);
      
    AddTypedefResult(Results);
    // Fall through

  case Sema::PCC_Class:
    if (SemaRef.getLangOptions().CPlusPlus) {
      // Using declaration
      Builder.AddTypedTextChunk("using");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddPlaceholderChunk("qualifier");
      Builder.AddTextChunk("::");
      Builder.AddPlaceholderChunk("name");
      Results.AddResult(Result(Builder.TakeString()));
      
      // using typename qualifier::name (only in a dependent context)
      if (SemaRef.CurContext->isDependentContext()) {
        Builder.AddTypedTextChunk("using");
        Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
        Builder.AddTextChunk("typename");
        Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
        Builder.AddPlaceholderChunk("qualifier");
        Builder.AddTextChunk("::");
        Builder.AddPlaceholderChunk("name");
        Results.AddResult(Result(Builder.TakeString()));
      }

      if (CCC == Sema::PCC_Class) {
        AddTypedefResult(Results);

        // public:
        Builder.AddTypedTextChunk("public");
        Builder.AddChunk(CodeCompletionString::CK_Colon);
        Results.AddResult(Result(Builder.TakeString()));

        // protected:
        Builder.AddTypedTextChunk("protected");
        Builder.AddChunk(CodeCompletionString::CK_Colon);
        Results.AddResult(Result(Builder.TakeString()));

        // private:
        Builder.AddTypedTextChunk("private");
        Builder.AddChunk(CodeCompletionString::CK_Colon);
        Results.AddResult(Result(Builder.TakeString()));
      }
    }
    // Fall through

  case Sema::PCC_Template:
  case Sema::PCC_MemberTemplate:
    if (SemaRef.getLangOptions().CPlusPlus && Results.includeCodePatterns()) {
      // template < parameters >
      Builder.AddTypedTextChunk("template");
      Builder.AddChunk(CodeCompletionString::CK_LeftAngle);
      Builder.AddPlaceholderChunk("parameters");
      Builder.AddChunk(CodeCompletionString::CK_RightAngle);
      Results.AddResult(Result(Builder.TakeString()));
    }

    AddStorageSpecifiers(CCC, SemaRef.getLangOptions(), Results);
    AddFunctionSpecifiers(CCC, SemaRef.getLangOptions(), Results);
    break;

  case Sema::PCC_ObjCInterface:
    AddObjCInterfaceResults(SemaRef.getLangOptions(), Results, true);
    AddStorageSpecifiers(CCC, SemaRef.getLangOptions(), Results);
    AddFunctionSpecifiers(CCC, SemaRef.getLangOptions(), Results);
    break;
      
  case Sema::PCC_ObjCImplementation:
    AddObjCImplementationResults(SemaRef.getLangOptions(), Results, true);
    AddStorageSpecifiers(CCC, SemaRef.getLangOptions(), Results);
    AddFunctionSpecifiers(CCC, SemaRef.getLangOptions(), Results);
    break;
      
  case Sema::PCC_ObjCInstanceVariableList:
    AddObjCVisibilityResults(SemaRef.getLangOptions(), Results, true);
    break;
      
  case Sema::PCC_RecoveryInFunction:
  case Sema::PCC_Statement: {
    AddTypedefResult(Results);

    if (SemaRef.getLangOptions().CPlusPlus && Results.includeCodePatterns() &&
        SemaRef.getLangOptions().CXXExceptions) {
      Builder.AddTypedTextChunk("try");
      Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
      Builder.AddPlaceholderChunk("statements");
      Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
      Builder.AddChunk(CodeCompletionString::CK_RightBrace);
      Builder.AddTextChunk("catch");
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("declaration");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
      Builder.AddPlaceholderChunk("statements");
      Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
      Builder.AddChunk(CodeCompletionString::CK_RightBrace);
      Results.AddResult(Result(Builder.TakeString()));
    }
    if (SemaRef.getLangOptions().ObjC1)
      AddObjCStatementResults(Results, true);
    
    if (Results.includeCodePatterns()) {
      // if (condition) { statements }
      Builder.AddTypedTextChunk("if");
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      if (SemaRef.getLangOptions().CPlusPlus)
        Builder.AddPlaceholderChunk("condition");
      else
        Builder.AddPlaceholderChunk("expression");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
      Builder.AddPlaceholderChunk("statements");
      Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
      Builder.AddChunk(CodeCompletionString::CK_RightBrace);
      Results.AddResult(Result(Builder.TakeString()));

      // switch (condition) { }
      Builder.AddTypedTextChunk("switch");
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      if (SemaRef.getLangOptions().CPlusPlus)
        Builder.AddPlaceholderChunk("condition");
      else
        Builder.AddPlaceholderChunk("expression");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
      Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
      Builder.AddChunk(CodeCompletionString::CK_RightBrace);
      Results.AddResult(Result(Builder.TakeString()));
    }
    
    // Switch-specific statements.
    if (!SemaRef.getCurFunction()->SwitchStack.empty()) {
      // case expression:
      Builder.AddTypedTextChunk("case");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddPlaceholderChunk("expression");
      Builder.AddChunk(CodeCompletionString::CK_Colon);
      Results.AddResult(Result(Builder.TakeString()));

      // default:
      Builder.AddTypedTextChunk("default");
      Builder.AddChunk(CodeCompletionString::CK_Colon);
      Results.AddResult(Result(Builder.TakeString()));
    }

    if (Results.includeCodePatterns()) {
      /// while (condition) { statements }
      Builder.AddTypedTextChunk("while");
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      if (SemaRef.getLangOptions().CPlusPlus)
        Builder.AddPlaceholderChunk("condition");
      else
        Builder.AddPlaceholderChunk("expression");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
      Builder.AddPlaceholderChunk("statements");
      Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
      Builder.AddChunk(CodeCompletionString::CK_RightBrace);
      Results.AddResult(Result(Builder.TakeString()));

      // do { statements } while ( expression );
      Builder.AddTypedTextChunk("do");
      Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
      Builder.AddPlaceholderChunk("statements");
      Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
      Builder.AddChunk(CodeCompletionString::CK_RightBrace);
      Builder.AddTextChunk("while");
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("expression");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Results.AddResult(Result(Builder.TakeString()));

      // for ( for-init-statement ; condition ; expression ) { statements }
      Builder.AddTypedTextChunk("for");
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      if (SemaRef.getLangOptions().CPlusPlus || SemaRef.getLangOptions().C99)
        Builder.AddPlaceholderChunk("init-statement");
      else
        Builder.AddPlaceholderChunk("init-expression");
      Builder.AddChunk(CodeCompletionString::CK_SemiColon);
      Builder.AddPlaceholderChunk("condition");
      Builder.AddChunk(CodeCompletionString::CK_SemiColon);
      Builder.AddPlaceholderChunk("inc-expression");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
      Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
      Builder.AddPlaceholderChunk("statements");
      Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
      Builder.AddChunk(CodeCompletionString::CK_RightBrace);
      Results.AddResult(Result(Builder.TakeString()));
    }
    
    if (S->getContinueParent()) {
      // continue ;
      Builder.AddTypedTextChunk("continue");
      Results.AddResult(Result(Builder.TakeString()));
    }

    if (S->getBreakParent()) {
      // break ;
      Builder.AddTypedTextChunk("break");
      Results.AddResult(Result(Builder.TakeString()));
    }

    // "return expression ;" or "return ;", depending on whether we
    // know the function is void or not.
    bool isVoid = false;
    if (FunctionDecl *Function = dyn_cast<FunctionDecl>(SemaRef.CurContext))
      isVoid = Function->getResultType()->isVoidType();
    else if (ObjCMethodDecl *Method
                                 = dyn_cast<ObjCMethodDecl>(SemaRef.CurContext))
      isVoid = Method->getResultType()->isVoidType();
    else if (SemaRef.getCurBlock() && 
             !SemaRef.getCurBlock()->ReturnType.isNull())
      isVoid = SemaRef.getCurBlock()->ReturnType->isVoidType();
    Builder.AddTypedTextChunk("return");
    if (!isVoid) {
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddPlaceholderChunk("expression");
    }
    Results.AddResult(Result(Builder.TakeString()));

    // goto identifier ;
    Builder.AddTypedTextChunk("goto");
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddPlaceholderChunk("label");
    Results.AddResult(Result(Builder.TakeString()));    

    // Using directives
    Builder.AddTypedTextChunk("using");
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddTextChunk("namespace");
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddPlaceholderChunk("identifier");
    Results.AddResult(Result(Builder.TakeString()));
  }

  // Fall through (for statement expressions).
  case Sema::PCC_ForInit:
  case Sema::PCC_Condition:
    AddStorageSpecifiers(CCC, SemaRef.getLangOptions(), Results);
    // Fall through: conditions and statements can have expressions.

  case Sema::PCC_ParenthesizedExpression:
    if (SemaRef.getLangOptions().ObjCAutoRefCount &&
        CCC == Sema::PCC_ParenthesizedExpression) {
      // (__bridge <type>)<expression>
      Builder.AddTypedTextChunk("__bridge");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddPlaceholderChunk("type");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddPlaceholderChunk("expression");
      Results.AddResult(Result(Builder.TakeString()));      

      // (__bridge_transfer <Objective-C type>)<expression>
      Builder.AddTypedTextChunk("__bridge_transfer");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddPlaceholderChunk("Objective-C type");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddPlaceholderChunk("expression");
      Results.AddResult(Result(Builder.TakeString()));      

      // (__bridge_retained <CF type>)<expression>
      Builder.AddTypedTextChunk("__bridge_retained");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddPlaceholderChunk("CF type");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddPlaceholderChunk("expression");
      Results.AddResult(Result(Builder.TakeString()));      
    }
    // Fall through

  case Sema::PCC_Expression: {
    if (SemaRef.getLangOptions().CPlusPlus) {
      // 'this', if we're in a non-static member function.
      QualType ThisTy = SemaRef.getCurrentThisType(false);
      if (!ThisTy.isNull()) {
        Builder.AddResultTypeChunk(GetCompletionTypeString(ThisTy, 
                                                           SemaRef.Context, 
                                                           Policy,
                                                           Allocator));
        Builder.AddTypedTextChunk("this");
        Results.AddResult(Result(Builder.TakeString()));      
      }
      
      // true
      Builder.AddResultTypeChunk("bool");
      Builder.AddTypedTextChunk("true");
      Results.AddResult(Result(Builder.TakeString()));
      
      // false
      Builder.AddResultTypeChunk("bool");
      Builder.AddTypedTextChunk("false");
      Results.AddResult(Result(Builder.TakeString()));

      if (SemaRef.getLangOptions().RTTI) {
        // dynamic_cast < type-id > ( expression )
        Builder.AddTypedTextChunk("dynamic_cast");
        Builder.AddChunk(CodeCompletionString::CK_LeftAngle);
        Builder.AddPlaceholderChunk("type");
        Builder.AddChunk(CodeCompletionString::CK_RightAngle);
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddPlaceholderChunk("expression");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
        Results.AddResult(Result(Builder.TakeString()));      
      }
      
      // static_cast < type-id > ( expression )
      Builder.AddTypedTextChunk("static_cast");
      Builder.AddChunk(CodeCompletionString::CK_LeftAngle);
      Builder.AddPlaceholderChunk("type");
      Builder.AddChunk(CodeCompletionString::CK_RightAngle);
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("expression");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Results.AddResult(Result(Builder.TakeString()));      

      // reinterpret_cast < type-id > ( expression )
      Builder.AddTypedTextChunk("reinterpret_cast");
      Builder.AddChunk(CodeCompletionString::CK_LeftAngle);
      Builder.AddPlaceholderChunk("type");
      Builder.AddChunk(CodeCompletionString::CK_RightAngle);
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("expression");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Results.AddResult(Result(Builder.TakeString()));      

      // const_cast < type-id > ( expression )
      Builder.AddTypedTextChunk("const_cast");
      Builder.AddChunk(CodeCompletionString::CK_LeftAngle);
      Builder.AddPlaceholderChunk("type");
      Builder.AddChunk(CodeCompletionString::CK_RightAngle);
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("expression");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Results.AddResult(Result(Builder.TakeString()));      

      if (SemaRef.getLangOptions().RTTI) {
        // typeid ( expression-or-type )
        Builder.AddResultTypeChunk("std::type_info");
        Builder.AddTypedTextChunk("typeid");
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddPlaceholderChunk("expression-or-type");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
        Results.AddResult(Result(Builder.TakeString()));      
      }
      
      // new T ( ... )
      Builder.AddTypedTextChunk("new");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddPlaceholderChunk("type");
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("expressions");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Results.AddResult(Result(Builder.TakeString()));      

      // new T [ ] ( ... )
      Builder.AddTypedTextChunk("new");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddPlaceholderChunk("type");
      Builder.AddChunk(CodeCompletionString::CK_LeftBracket);
      Builder.AddPlaceholderChunk("size");
      Builder.AddChunk(CodeCompletionString::CK_RightBracket);
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("expressions");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Results.AddResult(Result(Builder.TakeString()));      

      // delete expression
      Builder.AddResultTypeChunk("void");
      Builder.AddTypedTextChunk("delete");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddPlaceholderChunk("expression");
      Results.AddResult(Result(Builder.TakeString()));      

      // delete [] expression
      Builder.AddResultTypeChunk("void");
      Builder.AddTypedTextChunk("delete");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddChunk(CodeCompletionString::CK_LeftBracket);
      Builder.AddChunk(CodeCompletionString::CK_RightBracket);
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddPlaceholderChunk("expression");
      Results.AddResult(Result(Builder.TakeString()));

      if (SemaRef.getLangOptions().CXXExceptions) {
        // throw expression
        Builder.AddResultTypeChunk("void");
        Builder.AddTypedTextChunk("throw");
        Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
        Builder.AddPlaceholderChunk("expression");
        Results.AddResult(Result(Builder.TakeString()));
      }
     
      // FIXME: Rethrow?

      if (SemaRef.getLangOptions().CPlusPlus0x) {
        // nullptr
        Builder.AddResultTypeChunk("std::nullptr_t");
        Builder.AddTypedTextChunk("nullptr");
        Results.AddResult(Result(Builder.TakeString()));

        // alignof
        Builder.AddResultTypeChunk("size_t");
        Builder.AddTypedTextChunk("alignof");
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddPlaceholderChunk("type");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
        Results.AddResult(Result(Builder.TakeString()));

        // noexcept
        Builder.AddResultTypeChunk("bool");
        Builder.AddTypedTextChunk("noexcept");
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddPlaceholderChunk("expression");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
        Results.AddResult(Result(Builder.TakeString()));

        // sizeof... expression
        Builder.AddResultTypeChunk("size_t");
        Builder.AddTypedTextChunk("sizeof...");
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddPlaceholderChunk("parameter-pack");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
        Results.AddResult(Result(Builder.TakeString()));
      }
    }

    if (SemaRef.getLangOptions().ObjC1) {
      // Add "super", if we're in an Objective-C class with a superclass.
      if (ObjCMethodDecl *Method = SemaRef.getCurMethodDecl()) {
        // The interface can be NULL.
        if (ObjCInterfaceDecl *ID = Method->getClassInterface())
          if (ID->getSuperClass()) {
            std::string SuperType;
            SuperType = ID->getSuperClass()->getNameAsString();
            if (Method->isInstanceMethod())
              SuperType += " *";
            
            Builder.AddResultTypeChunk(Allocator.CopyString(SuperType));
            Builder.AddTypedTextChunk("super");
            Results.AddResult(Result(Builder.TakeString()));
          }
      }

      AddObjCExpressionResults(Results, true);
    }

    // sizeof expression
    Builder.AddResultTypeChunk("size_t");
    Builder.AddTypedTextChunk("sizeof");
    Builder.AddChunk(CodeCompletionString::CK_LeftParen);
    Builder.AddPlaceholderChunk("expression-or-type");
    Builder.AddChunk(CodeCompletionString::CK_RightParen);
    Results.AddResult(Result(Builder.TakeString()));
    break;
  }
      
  case Sema::PCC_Type:
  case Sema::PCC_LocalDeclarationSpecifiers:
    break;
  }

  if (WantTypesInContext(CCC, SemaRef.getLangOptions()))
    AddTypeSpecifierResults(SemaRef.getLangOptions(), Results);

  if (SemaRef.getLangOptions().CPlusPlus && CCC != Sema::PCC_Type)
    Results.AddResult(Result("operator"));
}

/// \brief If the given declaration has an associated type, add it as a result 
/// type chunk.
static void AddResultTypeChunk(ASTContext &Context,
                               const PrintingPolicy &Policy,
                               NamedDecl *ND,
                               CodeCompletionBuilder &Result) {
  if (!ND)
    return;

  // Skip constructors and conversion functions, which have their return types
  // built into their names.
  if (isa<CXXConstructorDecl>(ND) || isa<CXXConversionDecl>(ND))
    return;

  // Determine the type of the declaration (if it has a type).
  QualType T;  
  if (FunctionDecl *Function = dyn_cast<FunctionDecl>(ND))
    T = Function->getResultType();
  else if (ObjCMethodDecl *Method = dyn_cast<ObjCMethodDecl>(ND))
    T = Method->getResultType();
  else if (FunctionTemplateDecl *FunTmpl = dyn_cast<FunctionTemplateDecl>(ND))
    T = FunTmpl->getTemplatedDecl()->getResultType();
  else if (EnumConstantDecl *Enumerator = dyn_cast<EnumConstantDecl>(ND))
    T = Context.getTypeDeclType(cast<TypeDecl>(Enumerator->getDeclContext()));
  else if (isa<UnresolvedUsingValueDecl>(ND)) {
    /* Do nothing: ignore unresolved using declarations*/
  } else if (ValueDecl *Value = dyn_cast<ValueDecl>(ND)) {
    T = Value->getType();
  } else if (ObjCPropertyDecl *Property = dyn_cast<ObjCPropertyDecl>(ND))
    T = Property->getType();
  
  if (T.isNull() || Context.hasSameType(T, Context.DependentTy))
    return;
  
  Result.AddResultTypeChunk(GetCompletionTypeString(T, Context, Policy,
                                                    Result.getAllocator()));
}

static void MaybeAddSentinel(ASTContext &Context, NamedDecl *FunctionOrMethod,
                             CodeCompletionBuilder &Result) {
  if (SentinelAttr *Sentinel = FunctionOrMethod->getAttr<SentinelAttr>())
    if (Sentinel->getSentinel() == 0) {
      if (Context.getLangOptions().ObjC1 &&
          Context.Idents.get("nil").hasMacroDefinition())
        Result.AddTextChunk(", nil");
      else if (Context.Idents.get("NULL").hasMacroDefinition())
        Result.AddTextChunk(", NULL");
      else
        Result.AddTextChunk(", (void*)0");
    }
}

static std::string formatObjCParamQualifiers(unsigned ObjCQuals) {
  std::string Result;
  if (ObjCQuals & Decl::OBJC_TQ_In)
    Result += "in ";
  else if (ObjCQuals & Decl::OBJC_TQ_Inout)
    Result += "inout ";
  else if (ObjCQuals & Decl::OBJC_TQ_Out)
    Result += "out ";
  if (ObjCQuals & Decl::OBJC_TQ_Bycopy)
    Result += "bycopy ";
  else if (ObjCQuals & Decl::OBJC_TQ_Byref)
    Result += "byref ";
  if (ObjCQuals & Decl::OBJC_TQ_Oneway)
    Result += "oneway ";
  return Result;
}

static std::string FormatFunctionParameter(ASTContext &Context,
                                           const PrintingPolicy &Policy,
                                           ParmVarDecl *Param,
                                           bool SuppressName = false,
                                           bool SuppressBlock = false) {
  bool ObjCMethodParam = isa<ObjCMethodDecl>(Param->getDeclContext());
  if (Param->getType()->isDependentType() ||
      !Param->getType()->isBlockPointerType()) {
    // The argument for a dependent or non-block parameter is a placeholder 
    // containing that parameter's type.
    std::string Result;
    
    if (Param->getIdentifier() && !ObjCMethodParam && !SuppressName)
      Result = Param->getIdentifier()->getName();
    
    Param->getType().getAsStringInternal(Result, Policy);
    
    if (ObjCMethodParam) {
      Result = "(" + formatObjCParamQualifiers(Param->getObjCDeclQualifier())
             + Result + ")";
      if (Param->getIdentifier() && !SuppressName)
        Result += Param->getIdentifier()->getName();
    }
    return Result;
  }
  
  // The argument for a block pointer parameter is a block literal with
  // the appropriate type.
  FunctionTypeLoc *Block = 0;
  FunctionProtoTypeLoc *BlockProto = 0;
  TypeLoc TL;
  if (TypeSourceInfo *TSInfo = Param->getTypeSourceInfo()) {
    TL = TSInfo->getTypeLoc().getUnqualifiedLoc();
    while (true) {
      // Look through typedefs.
      if (!SuppressBlock) {
        if (TypedefTypeLoc *TypedefTL = dyn_cast<TypedefTypeLoc>(&TL)) {
          if (TypeSourceInfo *InnerTSInfo
              = TypedefTL->getTypedefNameDecl()->getTypeSourceInfo()) {
            TL = InnerTSInfo->getTypeLoc().getUnqualifiedLoc();
            continue;
          }
        }
        
        // Look through qualified types
        if (QualifiedTypeLoc *QualifiedTL = dyn_cast<QualifiedTypeLoc>(&TL)) {
          TL = QualifiedTL->getUnqualifiedLoc();
          continue;
        }
      }
      
      // Try to get the function prototype behind the block pointer type,
      // then we're done.
      if (BlockPointerTypeLoc *BlockPtr
          = dyn_cast<BlockPointerTypeLoc>(&TL)) {
        TL = BlockPtr->getPointeeLoc().IgnoreParens();
        Block = dyn_cast<FunctionTypeLoc>(&TL);
        BlockProto = dyn_cast<FunctionProtoTypeLoc>(&TL);
      }
      break;
    }
  }

  if (!Block) {
    // We were unable to find a FunctionProtoTypeLoc with parameter names
    // for the block; just use the parameter type as a placeholder.
    std::string Result;
    if (!ObjCMethodParam && Param->getIdentifier())
      Result = Param->getIdentifier()->getName();

    Param->getType().getUnqualifiedType().getAsStringInternal(Result, Policy);
    
    if (ObjCMethodParam) {
      Result = "(" + formatObjCParamQualifiers(Param->getObjCDeclQualifier())
             + Result + ")";
      if (Param->getIdentifier())
        Result += Param->getIdentifier()->getName();
    }
      
    return Result;
  }
    
  // We have the function prototype behind the block pointer type, as it was
  // written in the source.
  std::string Result;
  QualType ResultType = Block->getTypePtr()->getResultType();
  if (!ResultType->isVoidType() || SuppressBlock)
    ResultType.getAsStringInternal(Result, Policy);

  // Format the parameter list.
  std::string Params;
  if (!BlockProto || Block->getNumArgs() == 0) {
    if (BlockProto && BlockProto->getTypePtr()->isVariadic())
      Params = "(...)";
    else
      Params = "(void)";
  } else {
    Params += "(";
    for (unsigned I = 0, N = Block->getNumArgs(); I != N; ++I) {
      if (I)
        Params += ", ";
      Params += FormatFunctionParameter(Context, Policy, Block->getArg(I),
                                        /*SuppressName=*/false, 
                                        /*SuppressBlock=*/true);
      
      if (I == N - 1 && BlockProto->getTypePtr()->isVariadic())
        Params += ", ...";
    }
    Params += ")";
  }
  
  if (SuppressBlock) {
    // Format as a parameter.
    Result = Result + " (^";
    if (Param->getIdentifier())
      Result += Param->getIdentifier()->getName();
    Result += ")";
    Result += Params;
  } else {
    // Format as a block literal argument.
    Result = '^' + Result;
    Result += Params;
    
    if (Param->getIdentifier())
      Result += Param->getIdentifier()->getName();
  }
  
  return Result;
}

/// \brief Add function parameter chunks to the given code completion string.
static void AddFunctionParameterChunks(ASTContext &Context,
                                       const PrintingPolicy &Policy,
                                       FunctionDecl *Function,
                                       CodeCompletionBuilder &Result,
                                       unsigned Start = 0,
                                       bool InOptional = false) {
  typedef CodeCompletionString::Chunk Chunk;
  bool FirstParameter = true;
  
  for (unsigned P = Start, N = Function->getNumParams(); P != N; ++P) {
    ParmVarDecl *Param = Function->getParamDecl(P);
    
    if (Param->hasDefaultArg() && !InOptional) {
      // When we see an optional default argument, put that argument and
      // the remaining default arguments into a new, optional string.
      CodeCompletionBuilder Opt(Result.getAllocator());
      if (!FirstParameter)
        Opt.AddChunk(Chunk(CodeCompletionString::CK_Comma));        
      AddFunctionParameterChunks(Context, Policy, Function, Opt, P, true);
      Result.AddOptionalChunk(Opt.TakeString());
      break;
    }
    
    if (FirstParameter)
      FirstParameter = false;
    else
      Result.AddChunk(Chunk(CodeCompletionString::CK_Comma));
    
    InOptional = false;
    
    // Format the placeholder string.
    std::string PlaceholderStr = FormatFunctionParameter(Context, Policy, 
                                                         Param);
        
    if (Function->isVariadic() && P == N - 1)
      PlaceholderStr += ", ...";

    // Add the placeholder string.
    Result.AddPlaceholderChunk(
                             Result.getAllocator().CopyString(PlaceholderStr));
  }
  
  if (const FunctionProtoType *Proto 
        = Function->getType()->getAs<FunctionProtoType>())
    if (Proto->isVariadic()) {
      if (Proto->getNumArgs() == 0)
        Result.AddPlaceholderChunk("...");

      MaybeAddSentinel(Context, Function, Result);
    }
}

/// \brief Add template parameter chunks to the given code completion string.
static void AddTemplateParameterChunks(ASTContext &Context,
                                       const PrintingPolicy &Policy,
                                       TemplateDecl *Template,
                                       CodeCompletionBuilder &Result,
                                       unsigned MaxParameters = 0,
                                       unsigned Start = 0,
                                       bool InDefaultArg = false) {
  typedef CodeCompletionString::Chunk Chunk;
  bool FirstParameter = true;
  
  TemplateParameterList *Params = Template->getTemplateParameters();
  TemplateParameterList::iterator PEnd = Params->end();
  if (MaxParameters)
    PEnd = Params->begin() + MaxParameters;
  for (TemplateParameterList::iterator P = Params->begin() + Start; 
       P != PEnd; ++P) {
    bool HasDefaultArg = false;
    std::string PlaceholderStr;
    if (TemplateTypeParmDecl *TTP = dyn_cast<TemplateTypeParmDecl>(*P)) {
      if (TTP->wasDeclaredWithTypename())
        PlaceholderStr = "typename";
      else
        PlaceholderStr = "class";
      
      if (TTP->getIdentifier()) {
        PlaceholderStr += ' ';
        PlaceholderStr += TTP->getIdentifier()->getName();
      }
      
      HasDefaultArg = TTP->hasDefaultArgument();
    } else if (NonTypeTemplateParmDecl *NTTP 
                                    = dyn_cast<NonTypeTemplateParmDecl>(*P)) {
      if (NTTP->getIdentifier())
        PlaceholderStr = NTTP->getIdentifier()->getName();
      NTTP->getType().getAsStringInternal(PlaceholderStr, Policy);
      HasDefaultArg = NTTP->hasDefaultArgument();
    } else {
      assert(isa<TemplateTemplateParmDecl>(*P));
      TemplateTemplateParmDecl *TTP = cast<TemplateTemplateParmDecl>(*P);
      
      // Since putting the template argument list into the placeholder would
      // be very, very long, we just use an abbreviation.
      PlaceholderStr = "template<...> class";
      if (TTP->getIdentifier()) {
        PlaceholderStr += ' ';
        PlaceholderStr += TTP->getIdentifier()->getName();
      }
      
      HasDefaultArg = TTP->hasDefaultArgument();
    }
    
    if (HasDefaultArg && !InDefaultArg) {
      // When we see an optional default argument, put that argument and
      // the remaining default arguments into a new, optional string.
      CodeCompletionBuilder Opt(Result.getAllocator());
      if (!FirstParameter)
        Opt.AddChunk(Chunk(CodeCompletionString::CK_Comma));
      AddTemplateParameterChunks(Context, Policy, Template, Opt, MaxParameters,
                                 P - Params->begin(), true);
      Result.AddOptionalChunk(Opt.TakeString());
      break;
    }
    
    InDefaultArg = false;
    
    if (FirstParameter)
      FirstParameter = false;
    else
      Result.AddChunk(Chunk(CodeCompletionString::CK_Comma));
    
    // Add the placeholder string.
    Result.AddPlaceholderChunk(
                              Result.getAllocator().CopyString(PlaceholderStr));
  }    
}

/// \brief Add a qualifier to the given code-completion string, if the
/// provided nested-name-specifier is non-NULL.
static void 
AddQualifierToCompletionString(CodeCompletionBuilder &Result, 
                               NestedNameSpecifier *Qualifier, 
                               bool QualifierIsInformative,
                               ASTContext &Context,
                               const PrintingPolicy &Policy) {
  if (!Qualifier)
    return;
  
  std::string PrintedNNS;
  {
    llvm::raw_string_ostream OS(PrintedNNS);
    Qualifier->print(OS, Policy);
  }
  if (QualifierIsInformative)
    Result.AddInformativeChunk(Result.getAllocator().CopyString(PrintedNNS));
  else
    Result.AddTextChunk(Result.getAllocator().CopyString(PrintedNNS));
}

static void 
AddFunctionTypeQualsToCompletionString(CodeCompletionBuilder &Result,
                                       FunctionDecl *Function) {
  const FunctionProtoType *Proto
    = Function->getType()->getAs<FunctionProtoType>();
  if (!Proto || !Proto->getTypeQuals())
    return;

  // FIXME: Add ref-qualifier!
  
  // Handle single qualifiers without copying
  if (Proto->getTypeQuals() == Qualifiers::Const) {
    Result.AddInformativeChunk(" const");
    return;
  }

  if (Proto->getTypeQuals() == Qualifiers::Volatile) {
    Result.AddInformativeChunk(" volatile");
    return;
  }

  if (Proto->getTypeQuals() == Qualifiers::Restrict) {
    Result.AddInformativeChunk(" restrict");
    return;
  }

  // Handle multiple qualifiers.
  std::string QualsStr;
  if (Proto->getTypeQuals() & Qualifiers::Const)
    QualsStr += " const";
  if (Proto->getTypeQuals() & Qualifiers::Volatile)
    QualsStr += " volatile";
  if (Proto->getTypeQuals() & Qualifiers::Restrict)
    QualsStr += " restrict";
  Result.AddInformativeChunk(Result.getAllocator().CopyString(QualsStr));
}

/// \brief Add the name of the given declaration 
static void AddTypedNameChunk(ASTContext &Context, const PrintingPolicy &Policy,
                              NamedDecl *ND, CodeCompletionBuilder &Result) {
  typedef CodeCompletionString::Chunk Chunk;
  
  DeclarationName Name = ND->getDeclName();
  if (!Name)
    return;
  
  switch (Name.getNameKind()) {
    case DeclarationName::CXXOperatorName: {
      const char *OperatorName = 0;
      switch (Name.getCXXOverloadedOperator()) {
      case OO_None: 
      case OO_Conditional:
      case NUM_OVERLOADED_OPERATORS:
        OperatorName = "operator"; 
        break;
    
#define OVERLOADED_OPERATOR(Name,Spelling,Token,Unary,Binary,MemberOnly) \
      case OO_##Name: OperatorName = "operator" Spelling; break;
#define OVERLOADED_OPERATOR_MULTI(Name,Spelling,Unary,Binary,MemberOnly)
#include "clang/Basic/OperatorKinds.def"
          
      case OO_New:          OperatorName = "operator new"; break;
      case OO_Delete:       OperatorName = "operator delete"; break;
      case OO_Array_New:    OperatorName = "operator new[]"; break;
      case OO_Array_Delete: OperatorName = "operator delete[]"; break;
      case OO_Call:         OperatorName = "operator()"; break;
      case OO_Subscript:    OperatorName = "operator[]"; break;
      }
      Result.AddTypedTextChunk(OperatorName);
      break;
    }
      
  case DeclarationName::Identifier:
  case DeclarationName::CXXConversionFunctionName:
  case DeclarationName::CXXDestructorName:
  case DeclarationName::CXXLiteralOperatorName:
    Result.AddTypedTextChunk(
                      Result.getAllocator().CopyString(ND->getNameAsString()));
    break;
      
  case DeclarationName::CXXUsingDirective:
  case DeclarationName::ObjCZeroArgSelector:
  case DeclarationName::ObjCOneArgSelector:
  case DeclarationName::ObjCMultiArgSelector:
    break;
      
  case DeclarationName::CXXConstructorName: {
    CXXRecordDecl *Record = 0;
    QualType Ty = Name.getCXXNameType();
    if (const RecordType *RecordTy = Ty->getAs<RecordType>())
      Record = cast<CXXRecordDecl>(RecordTy->getDecl());
    else if (const InjectedClassNameType *InjectedTy
                                        = Ty->getAs<InjectedClassNameType>())
      Record = InjectedTy->getDecl();
    else {
      Result.AddTypedTextChunk(
                      Result.getAllocator().CopyString(ND->getNameAsString()));
      break;
    }
    
    Result.AddTypedTextChunk(
                  Result.getAllocator().CopyString(Record->getNameAsString()));
    if (ClassTemplateDecl *Template = Record->getDescribedClassTemplate()) {
      Result.AddChunk(Chunk(CodeCompletionString::CK_LeftAngle));
      AddTemplateParameterChunks(Context, Policy, Template, Result);
      Result.AddChunk(Chunk(CodeCompletionString::CK_RightAngle));
    }
    break;
  }
  }
}

/// \brief If possible, create a new code completion string for the given
/// result.
///
/// \returns Either a new, heap-allocated code completion string describing
/// how to use this result, or NULL to indicate that the string or name of the
/// result is all that is needed.
CodeCompletionString *
CodeCompletionResult::CreateCodeCompletionString(Sema &S,
                                           CodeCompletionAllocator &Allocator) {
  typedef CodeCompletionString::Chunk Chunk;
  CodeCompletionBuilder Result(Allocator, Priority, Availability);
  
  PrintingPolicy Policy = getCompletionPrintingPolicy(S);
  if (Kind == RK_Pattern) {
    Pattern->Priority = Priority;
    Pattern->Availability = Availability;
    return Pattern;
  }
  
  if (Kind == RK_Keyword) {
    Result.AddTypedTextChunk(Keyword);
    return Result.TakeString();
  }
  
  if (Kind == RK_Macro) {
    MacroInfo *MI = S.PP.getMacroInfo(Macro);
    assert(MI && "Not a macro?");

    Result.AddTypedTextChunk(
                            Result.getAllocator().CopyString(Macro->getName()));

    if (!MI->isFunctionLike())
      return Result.TakeString();
    
    // Format a function-like macro with placeholders for the arguments.
    Result.AddChunk(Chunk(CodeCompletionString::CK_LeftParen));
    bool CombineVariadicArgument = false;
    MacroInfo::arg_iterator A = MI->arg_begin(), AEnd = MI->arg_end();
    if (MI->isVariadic() && AEnd - A > 1) {
      AEnd -= 2;
      CombineVariadicArgument = true;
    }
    for (MacroInfo::arg_iterator A = MI->arg_begin(); A != AEnd; ++A) {
      if (A != MI->arg_begin())
        Result.AddChunk(Chunk(CodeCompletionString::CK_Comma));
      
      if (!MI->isVariadic() || A + 1 != AEnd) {
        // Non-variadic argument.
        Result.AddPlaceholderChunk(
                            Result.getAllocator().CopyString((*A)->getName()));
        continue;
      }
      
      // Variadic argument; cope with the difference between GNU and C99
      // variadic macros, providing a single placeholder for the rest of the
      // arguments.
      if ((*A)->isStr("__VA_ARGS__"))
        Result.AddPlaceholderChunk("...");
      else {
        std::string Arg = (*A)->getName();
        Arg += "...";
        Result.AddPlaceholderChunk(Result.getAllocator().CopyString(Arg));
      }
    }
     
    if (CombineVariadicArgument) {
      // Handle the next-to-last argument, combining it with the variadic
      // argument.
      std::string LastArg = (*A)->getName();
      ++A;
      if ((*A)->isStr("__VA_ARGS__"))
        LastArg += ", ...";
      else
        LastArg += ", " + (*A)->getName().str() + "...";
      Result.AddPlaceholderChunk(Result.getAllocator().CopyString(LastArg));
    }
    Result.AddChunk(Chunk(CodeCompletionString::CK_RightParen));
    return Result.TakeString();
  }
  
  assert(Kind == RK_Declaration && "Missed a result kind?");
  NamedDecl *ND = Declaration;
  
  if (StartsNestedNameSpecifier) {
    Result.AddTypedTextChunk(
                      Result.getAllocator().CopyString(ND->getNameAsString()));
    Result.AddTextChunk("::");
    return Result.TakeString();
  }

  for (Decl::attr_iterator i = ND->attr_begin(); i != ND->attr_end(); ++i) {
    if (AnnotateAttr *Attr = dyn_cast_or_null<AnnotateAttr>(*i)) {
      Result.AddAnnotation(Result.getAllocator().CopyString(Attr->getAnnotation()));
    }
  }
  
  AddResultTypeChunk(S.Context, Policy, ND, Result);
  
  if (FunctionDecl *Function = dyn_cast<FunctionDecl>(ND)) {
    AddQualifierToCompletionString(Result, Qualifier, QualifierIsInformative, 
                                   S.Context, Policy);
    AddTypedNameChunk(S.Context, Policy, ND, Result);
    Result.AddChunk(Chunk(CodeCompletionString::CK_LeftParen));
    AddFunctionParameterChunks(S.Context, Policy, Function, Result);
    Result.AddChunk(Chunk(CodeCompletionString::CK_RightParen));
    AddFunctionTypeQualsToCompletionString(Result, Function);
    return Result.TakeString();
  }
  
  if (FunctionTemplateDecl *FunTmpl = dyn_cast<FunctionTemplateDecl>(ND)) {
    AddQualifierToCompletionString(Result, Qualifier, QualifierIsInformative, 
                                   S.Context, Policy);
    FunctionDecl *Function = FunTmpl->getTemplatedDecl();
    AddTypedNameChunk(S.Context, Policy, Function, Result);

    // Figure out which template parameters are deduced (or have default
    // arguments).
    SmallVector<bool, 16> Deduced;
    S.MarkDeducedTemplateParameters(FunTmpl, Deduced);
    unsigned LastDeducibleArgument;
    for (LastDeducibleArgument = Deduced.size(); LastDeducibleArgument > 0;
         --LastDeducibleArgument) {
      if (!Deduced[LastDeducibleArgument - 1]) {
        // C++0x: Figure out if the template argument has a default. If so,
        // the user doesn't need to type this argument.
        // FIXME: We need to abstract template parameters better!
        bool HasDefaultArg = false;
        NamedDecl *Param = FunTmpl->getTemplateParameters()->getParam(
                                                    LastDeducibleArgument - 1);
        if (TemplateTypeParmDecl *TTP = dyn_cast<TemplateTypeParmDecl>(Param))
          HasDefaultArg = TTP->hasDefaultArgument();
        else if (NonTypeTemplateParmDecl *NTTP 
                 = dyn_cast<NonTypeTemplateParmDecl>(Param))
          HasDefaultArg = NTTP->hasDefaultArgument();
        else {
          assert(isa<TemplateTemplateParmDecl>(Param));
          HasDefaultArg 
            = cast<TemplateTemplateParmDecl>(Param)->hasDefaultArgument();
        }
        
        if (!HasDefaultArg)
          break;
      }
    }
    
    if (LastDeducibleArgument) {
      // Some of the function template arguments cannot be deduced from a
      // function call, so we introduce an explicit template argument list
      // containing all of the arguments up to the first deducible argument.
      Result.AddChunk(Chunk(CodeCompletionString::CK_LeftAngle));
      AddTemplateParameterChunks(S.Context, Policy, FunTmpl, Result, 
                                 LastDeducibleArgument);
      Result.AddChunk(Chunk(CodeCompletionString::CK_RightAngle));
    }
    
    // Add the function parameters
    Result.AddChunk(Chunk(CodeCompletionString::CK_LeftParen));
    AddFunctionParameterChunks(S.Context, Policy, Function, Result);
    Result.AddChunk(Chunk(CodeCompletionString::CK_RightParen));
    AddFunctionTypeQualsToCompletionString(Result, Function);
    return Result.TakeString();
  }
  
  if (TemplateDecl *Template = dyn_cast<TemplateDecl>(ND)) {
    AddQualifierToCompletionString(Result, Qualifier, QualifierIsInformative, 
                                   S.Context, Policy);
    Result.AddTypedTextChunk(
                Result.getAllocator().CopyString(Template->getNameAsString()));
    Result.AddChunk(Chunk(CodeCompletionString::CK_LeftAngle));
    AddTemplateParameterChunks(S.Context, Policy, Template, Result);
    Result.AddChunk(Chunk(CodeCompletionString::CK_RightAngle));
    return Result.TakeString();
  }
  
  if (ObjCMethodDecl *Method = dyn_cast<ObjCMethodDecl>(ND)) {
    Selector Sel = Method->getSelector();
    if (Sel.isUnarySelector()) {
      Result.AddTypedTextChunk(Result.getAllocator().CopyString(
                                  Sel.getNameForSlot(0)));
      return Result.TakeString();
    }

    std::string SelName = Sel.getNameForSlot(0).str();
    SelName += ':';
    if (StartParameter == 0)
      Result.AddTypedTextChunk(Result.getAllocator().CopyString(SelName));
    else {
      Result.AddInformativeChunk(Result.getAllocator().CopyString(SelName));
      
      // If there is only one parameter, and we're past it, add an empty
      // typed-text chunk since there is nothing to type.
      if (Method->param_size() == 1)
        Result.AddTypedTextChunk("");
    }
    unsigned Idx = 0;
    for (ObjCMethodDecl::param_iterator P = Method->param_begin(),
                                     PEnd = Method->param_end();
         P != PEnd; (void)++P, ++Idx) {
      if (Idx > 0) {
        std::string Keyword;
        if (Idx > StartParameter)
          Result.AddChunk(CodeCompletionString::CK_HorizontalSpace);
        if (IdentifierInfo *II = Sel.getIdentifierInfoForSlot(Idx))
          Keyword += II->getName();
        Keyword += ":";
        if (Idx < StartParameter || AllParametersAreInformative)
          Result.AddInformativeChunk(Result.getAllocator().CopyString(Keyword));
        else 
          Result.AddTypedTextChunk(Result.getAllocator().CopyString(Keyword));
      }
      
      // If we're before the starting parameter, skip the placeholder.
      if (Idx < StartParameter)
        continue;

      std::string Arg;
      
      if ((*P)->getType()->isBlockPointerType() && !DeclaringEntity)
        Arg = FormatFunctionParameter(S.Context, Policy, *P, true);
      else {
        (*P)->getType().getAsStringInternal(Arg, Policy);
        Arg = "(" + formatObjCParamQualifiers((*P)->getObjCDeclQualifier()) 
            + Arg + ")";
        if (IdentifierInfo *II = (*P)->getIdentifier())
          if (DeclaringEntity || AllParametersAreInformative)
            Arg += II->getName();
      }
      
      if (Method->isVariadic() && (P + 1) == PEnd)
        Arg += ", ...";
      
      if (DeclaringEntity)
        Result.AddTextChunk(Result.getAllocator().CopyString(Arg));
      else if (AllParametersAreInformative)
        Result.AddInformativeChunk(Result.getAllocator().CopyString(Arg));
      else
        Result.AddPlaceholderChunk(Result.getAllocator().CopyString(Arg));
    }

    if (Method->isVariadic()) {
      if (Method->param_size() == 0) {
        if (DeclaringEntity)
          Result.AddTextChunk(", ...");
        else if (AllParametersAreInformative)
          Result.AddInformativeChunk(", ...");
        else
          Result.AddPlaceholderChunk(", ...");
      }
      
      MaybeAddSentinel(S.Context, Method, Result);
    }
    
    return Result.TakeString();
  }

  if (Qualifier)
    AddQualifierToCompletionString(Result, Qualifier, QualifierIsInformative, 
                                   S.Context, Policy);

  Result.AddTypedTextChunk(
                       Result.getAllocator().CopyString(ND->getNameAsString()));
  return Result.TakeString();
}

CodeCompletionString *
CodeCompleteConsumer::OverloadCandidate::CreateSignatureString(
                                                          unsigned CurrentArg,
                                                               Sema &S,
                                     CodeCompletionAllocator &Allocator) const {
  typedef CodeCompletionString::Chunk Chunk;
  PrintingPolicy Policy = getCompletionPrintingPolicy(S);

  // FIXME: Set priority, availability appropriately.
  CodeCompletionBuilder Result(Allocator, 1, CXAvailability_Available);
  FunctionDecl *FDecl = getFunction();
  AddResultTypeChunk(S.Context, Policy, FDecl, Result);
  const FunctionProtoType *Proto 
    = dyn_cast<FunctionProtoType>(getFunctionType());
  if (!FDecl && !Proto) {
    // Function without a prototype. Just give the return type and a 
    // highlighted ellipsis.
    const FunctionType *FT = getFunctionType();
    Result.AddTextChunk(GetCompletionTypeString(FT->getResultType(),
                                                S.Context, Policy, 
                                                Result.getAllocator()));
    Result.AddChunk(Chunk(CodeCompletionString::CK_LeftParen));
    Result.AddChunk(Chunk(CodeCompletionString::CK_CurrentParameter, "..."));
    Result.AddChunk(Chunk(CodeCompletionString::CK_RightParen));
    return Result.TakeString();
  }
  
  if (FDecl)
    Result.AddTextChunk(
                    Result.getAllocator().CopyString(FDecl->getNameAsString()));
  else
    Result.AddTextChunk(
         Result.getAllocator().CopyString(
                                  Proto->getResultType().getAsString(Policy)));
  
  Result.AddChunk(Chunk(CodeCompletionString::CK_LeftParen));
  unsigned NumParams = FDecl? FDecl->getNumParams() : Proto->getNumArgs();
  for (unsigned I = 0; I != NumParams; ++I) {
    if (I)
      Result.AddChunk(Chunk(CodeCompletionString::CK_Comma));
    
    std::string ArgString;
    QualType ArgType;
    
    if (FDecl) {
      ArgString = FDecl->getParamDecl(I)->getNameAsString();
      ArgType = FDecl->getParamDecl(I)->getOriginalType();
    } else {
      ArgType = Proto->getArgType(I);
    }
    
    ArgType.getAsStringInternal(ArgString, Policy);
    
    if (I == CurrentArg)
      Result.AddChunk(Chunk(CodeCompletionString::CK_CurrentParameter, 
                             Result.getAllocator().CopyString(ArgString)));
    else
      Result.AddTextChunk(Result.getAllocator().CopyString(ArgString));
  }
  
  if (Proto && Proto->isVariadic()) {
    Result.AddChunk(Chunk(CodeCompletionString::CK_Comma));
    if (CurrentArg < NumParams)
      Result.AddTextChunk("...");
    else
      Result.AddChunk(Chunk(CodeCompletionString::CK_CurrentParameter, "..."));
  }
  Result.AddChunk(Chunk(CodeCompletionString::CK_RightParen));
  
  return Result.TakeString();
}

unsigned clang::getMacroUsagePriority(StringRef MacroName, 
                                      const LangOptions &LangOpts,
                                      bool PreferredTypeIsPointer) {
  unsigned Priority = CCP_Macro;
  
  // Treat the "nil", "Nil" and "NULL" macros as null pointer constants.
  if (MacroName.equals("nil") || MacroName.equals("NULL") || 
      MacroName.equals("Nil")) {
    Priority = CCP_Constant;
    if (PreferredTypeIsPointer)
      Priority = Priority / CCF_SimilarTypeMatch;
  } 
  // Treat "YES", "NO", "true", and "false" as constants.
  else if (MacroName.equals("YES") || MacroName.equals("NO") ||
           MacroName.equals("true") || MacroName.equals("false"))
    Priority = CCP_Constant;
  // Treat "bool" as a type.
  else if (MacroName.equals("bool"))
    Priority = CCP_Type + (LangOpts.ObjC1? CCD_bool_in_ObjC : 0);
    
  
  return Priority;
}

CXCursorKind clang::getCursorKindForDecl(Decl *D) {
  if (!D)
    return CXCursor_UnexposedDecl;
  
  switch (D->getKind()) {
    case Decl::Enum:               return CXCursor_EnumDecl;
    case Decl::EnumConstant:       return CXCursor_EnumConstantDecl;
    case Decl::Field:              return CXCursor_FieldDecl;
    case Decl::Function:  
      return CXCursor_FunctionDecl;
    case Decl::ObjCCategory:       return CXCursor_ObjCCategoryDecl;
    case Decl::ObjCCategoryImpl:   return CXCursor_ObjCCategoryImplDecl;
      // FIXME
      return CXCursor_UnexposedDecl;
    case Decl::ObjCImplementation: return CXCursor_ObjCImplementationDecl;

    case Decl::ObjCInterface:
      if (cast<ObjCInterfaceDecl>(D)->isThisDeclarationADefinition())
        return CXCursor_ObjCInterfaceDecl;
      
      // Forward declarations are not directly exposed.
      return CXCursor_UnexposedDecl;

    case Decl::ObjCIvar:           return CXCursor_ObjCIvarDecl; 
    case Decl::ObjCMethod:
      return cast<ObjCMethodDecl>(D)->isInstanceMethod()
      ? CXCursor_ObjCInstanceMethodDecl : CXCursor_ObjCClassMethodDecl;
    case Decl::CXXMethod:          return CXCursor_CXXMethod;
    case Decl::CXXConstructor:     return CXCursor_Constructor;
    case Decl::CXXDestructor:      return CXCursor_Destructor;
    case Decl::CXXConversion:      return CXCursor_ConversionFunction;
    case Decl::ObjCProperty:       return CXCursor_ObjCPropertyDecl;
    case Decl::ObjCProtocol:       
      if (cast<ObjCProtocolDecl>(D)->isThisDeclarationADefinition())
        return CXCursor_ObjCProtocolDecl;
      
      return CXCursor_UnexposedDecl;
      
    case Decl::ParmVar:            return CXCursor_ParmDecl;
    case Decl::Typedef:            return CXCursor_TypedefDecl;
    case Decl::TypeAlias:          return CXCursor_TypeAliasDecl;
    case Decl::Var:                return CXCursor_VarDecl;
    case Decl::Namespace:          return CXCursor_Namespace;
    case Decl::NamespaceAlias:     return CXCursor_NamespaceAlias;
    case Decl::TemplateTypeParm:   return CXCursor_TemplateTypeParameter;
    case Decl::NonTypeTemplateParm:return CXCursor_NonTypeTemplateParameter;
    case Decl::TemplateTemplateParm:return CXCursor_TemplateTemplateParameter;
    case Decl::FunctionTemplate:   return CXCursor_FunctionTemplate;
    case Decl::ClassTemplate:      return CXCursor_ClassTemplate;
    case Decl::AccessSpec:         return CXCursor_CXXAccessSpecifier;
    case Decl::ClassTemplatePartialSpecialization:
      return CXCursor_ClassTemplatePartialSpecialization;
    case Decl::UsingDirective:     return CXCursor_UsingDirective;
      
    case Decl::Using:
    case Decl::UnresolvedUsingValue:
    case Decl::UnresolvedUsingTypename: 
      return CXCursor_UsingDeclaration;
      
    case Decl::ObjCPropertyImpl:
      switch (cast<ObjCPropertyImplDecl>(D)->getPropertyImplementation()) {
      case ObjCPropertyImplDecl::Dynamic:
        return CXCursor_ObjCDynamicDecl;
          
      case ObjCPropertyImplDecl::Synthesize:
        return CXCursor_ObjCSynthesizeDecl;
      }
      break;
      
    default:
      if (TagDecl *TD = dyn_cast<TagDecl>(D)) {
        switch (TD->getTagKind()) {
          case TTK_Struct: return CXCursor_StructDecl;
          case TTK_Class:  return CXCursor_ClassDecl;
          case TTK_Union:  return CXCursor_UnionDecl;
          case TTK_Enum:   return CXCursor_EnumDecl;
        }
      }
  }
  
  return CXCursor_UnexposedDecl;
}

static void AddMacroResults(Preprocessor &PP, ResultBuilder &Results,
                            bool TargetTypeIsPointer = false) {
  typedef CodeCompletionResult Result;
  
  Results.EnterNewScope();
  
  for (Preprocessor::macro_iterator M = PP.macro_begin(), 
                                 MEnd = PP.macro_end();
       M != MEnd; ++M) {
    Results.AddResult(Result(M->first, 
                             getMacroUsagePriority(M->first->getName(),
                                                   PP.getLangOptions(),
                                                   TargetTypeIsPointer)));
  }
  
  Results.ExitScope();
  
}

static void AddPrettyFunctionResults(const LangOptions &LangOpts, 
                                     ResultBuilder &Results) {
  typedef CodeCompletionResult Result;
  
  Results.EnterNewScope();
  
  Results.AddResult(Result("__PRETTY_FUNCTION__", CCP_Constant));
  Results.AddResult(Result("__FUNCTION__", CCP_Constant));
  if (LangOpts.C99 || LangOpts.CPlusPlus0x)
    Results.AddResult(Result("__func__", CCP_Constant));
  Results.ExitScope();
}

static void HandleCodeCompleteResults(Sema *S,
                                      CodeCompleteConsumer *CodeCompleter,
                                      CodeCompletionContext Context,
                                      CodeCompletionResult *Results,
                                      unsigned NumResults) {
  if (CodeCompleter)
    CodeCompleter->ProcessCodeCompleteResults(*S, Context, Results, NumResults);
}

static enum CodeCompletionContext::Kind mapCodeCompletionContext(Sema &S, 
                                            Sema::ParserCompletionContext PCC) {
  switch (PCC) {
  case Sema::PCC_Namespace:
    return CodeCompletionContext::CCC_TopLevel;
      
  case Sema::PCC_Class:
    return CodeCompletionContext::CCC_ClassStructUnion;

  case Sema::PCC_ObjCInterface:
    return CodeCompletionContext::CCC_ObjCInterface;
      
  case Sema::PCC_ObjCImplementation:
    return CodeCompletionContext::CCC_ObjCImplementation;

  case Sema::PCC_ObjCInstanceVariableList:
    return CodeCompletionContext::CCC_ObjCIvarList;
      
  case Sema::PCC_Template:
  case Sema::PCC_MemberTemplate:
    if (S.CurContext->isFileContext())
      return CodeCompletionContext::CCC_TopLevel;
    else if (S.CurContext->isRecord())
      return CodeCompletionContext::CCC_ClassStructUnion;
    else 
      return CodeCompletionContext::CCC_Other;
      
  case Sema::PCC_RecoveryInFunction:
    return CodeCompletionContext::CCC_Recovery;

  case Sema::PCC_ForInit:
    if (S.getLangOptions().CPlusPlus || S.getLangOptions().C99 ||
        S.getLangOptions().ObjC1)
      return CodeCompletionContext::CCC_ParenthesizedExpression;
    else
      return CodeCompletionContext::CCC_Expression;

  case Sema::PCC_Expression:
  case Sema::PCC_Condition:
    return CodeCompletionContext::CCC_Expression;
      
  case Sema::PCC_Statement:
    return CodeCompletionContext::CCC_Statement;

  case Sema::PCC_Type:
    return CodeCompletionContext::CCC_Type;

  case Sema::PCC_ParenthesizedExpression:
    return CodeCompletionContext::CCC_ParenthesizedExpression;
      
  case Sema::PCC_LocalDeclarationSpecifiers:
    return CodeCompletionContext::CCC_Type;
  }
  
  return CodeCompletionContext::CCC_Other;
}

/// \brief If we're in a C++ virtual member function, add completion results
/// that invoke the functions we override, since it's common to invoke the 
/// overridden function as well as adding new functionality.
///
/// \param S The semantic analysis object for which we are generating results.
///
/// \param InContext This context in which the nested-name-specifier preceding
/// the code-completion point 
static void MaybeAddOverrideCalls(Sema &S, DeclContext *InContext,
                                  ResultBuilder &Results) {
  // Look through blocks.
  DeclContext *CurContext = S.CurContext;
  while (isa<BlockDecl>(CurContext))
    CurContext = CurContext->getParent();
  
  
  CXXMethodDecl *Method = dyn_cast<CXXMethodDecl>(CurContext);
  if (!Method || !Method->isVirtual())
    return;
  
  // We need to have names for all of the parameters, if we're going to 
  // generate a forwarding call.
  for (CXXMethodDecl::param_iterator P = Method->param_begin(),
                                  PEnd = Method->param_end();
       P != PEnd;
       ++P) {
    if (!(*P)->getDeclName())
      return;
  }

  PrintingPolicy Policy = getCompletionPrintingPolicy(S);
  for (CXXMethodDecl::method_iterator M = Method->begin_overridden_methods(),
                                   MEnd = Method->end_overridden_methods();
       M != MEnd; ++M) {
    CodeCompletionBuilder Builder(Results.getAllocator());
    CXXMethodDecl *Overridden = const_cast<CXXMethodDecl *>(*M);
    if (Overridden->getCanonicalDecl() == Method->getCanonicalDecl())
      continue;
        
    // If we need a nested-name-specifier, add one now.
    if (!InContext) {
      NestedNameSpecifier *NNS
        = getRequiredQualification(S.Context, CurContext,
                                   Overridden->getDeclContext());
      if (NNS) {
        std::string Str;
        llvm::raw_string_ostream OS(Str);
        NNS->print(OS, Policy);
        Builder.AddTextChunk(Results.getAllocator().CopyString(OS.str()));
      }
    } else if (!InContext->Equals(Overridden->getDeclContext()))
      continue;
    
    Builder.AddTypedTextChunk(Results.getAllocator().CopyString( 
                                         Overridden->getNameAsString()));
    Builder.AddChunk(CodeCompletionString::CK_LeftParen);
    bool FirstParam = true;
    for (CXXMethodDecl::param_iterator P = Method->param_begin(),
                                    PEnd = Method->param_end();
         P != PEnd; ++P) {
      if (FirstParam)
        FirstParam = false;
      else
        Builder.AddChunk(CodeCompletionString::CK_Comma);

      Builder.AddPlaceholderChunk(Results.getAllocator().CopyString( 
                                        (*P)->getIdentifier()->getName()));
    }
    Builder.AddChunk(CodeCompletionString::CK_RightParen);
    Results.AddResult(CodeCompletionResult(Builder.TakeString(),
                                           CCP_SuperCompletion,
                                           CXCursor_CXXMethod));
    Results.Ignore(Overridden);
  }
}

void Sema::CodeCompleteOrdinaryName(Scope *S, 
                                    ParserCompletionContext CompletionContext) {
  typedef CodeCompletionResult Result;
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        mapCodeCompletionContext(*this, CompletionContext));
  Results.EnterNewScope();
  
  // Determine how to filter results, e.g., so that the names of
  // values (functions, enumerators, function templates, etc.) are
  // only allowed where we can have an expression.
  switch (CompletionContext) {
  case PCC_Namespace:
  case PCC_Class:
  case PCC_ObjCInterface:
  case PCC_ObjCImplementation:
  case PCC_ObjCInstanceVariableList:
  case PCC_Template:
  case PCC_MemberTemplate:
  case PCC_Type:
  case PCC_LocalDeclarationSpecifiers:
    Results.setFilter(&ResultBuilder::IsOrdinaryNonValueName);
    break;

  case PCC_Statement:
  case PCC_ParenthesizedExpression:
  case PCC_Expression:
  case PCC_ForInit:
  case PCC_Condition:
    if (WantTypesInContext(CompletionContext, getLangOptions()))
      Results.setFilter(&ResultBuilder::IsOrdinaryName);
    else
      Results.setFilter(&ResultBuilder::IsOrdinaryNonTypeName);
      
    if (getLangOptions().CPlusPlus)
      MaybeAddOverrideCalls(*this, /*InContext=*/0, Results);
    break;
      
  case PCC_RecoveryInFunction:
    // Unfiltered
    break;
  }

  // If we are in a C++ non-static member function, check the qualifiers on
  // the member function to filter/prioritize the results list.
  if (CXXMethodDecl *CurMethod = dyn_cast<CXXMethodDecl>(CurContext))
    if (CurMethod->isInstance())
      Results.setObjectTypeQualifiers(
                      Qualifiers::fromCVRMask(CurMethod->getTypeQualifiers()));
  
  CodeCompletionDeclConsumer Consumer(Results, CurContext);
  LookupVisibleDecls(S, LookupOrdinaryName, Consumer,
                     CodeCompleter->includeGlobals());

  AddOrdinaryNameResults(CompletionContext, S, *this, Results);
  Results.ExitScope();

  switch (CompletionContext) {
  case PCC_ParenthesizedExpression:
  case PCC_Expression:
  case PCC_Statement:
  case PCC_RecoveryInFunction:
    if (S->getFnParent())
      AddPrettyFunctionResults(PP.getLangOptions(), Results);        
    break;
    
  case PCC_Namespace:
  case PCC_Class:
  case PCC_ObjCInterface:
  case PCC_ObjCImplementation:
  case PCC_ObjCInstanceVariableList:
  case PCC_Template:
  case PCC_MemberTemplate:
  case PCC_ForInit:
  case PCC_Condition:
  case PCC_Type:
  case PCC_LocalDeclarationSpecifiers:
    break;
  }
  
  if (CodeCompleter->includeMacros())
    AddMacroResults(PP, Results);
  
  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(),Results.size());
}

static void AddClassMessageCompletions(Sema &SemaRef, Scope *S, 
                                       ParsedType Receiver,
                                       IdentifierInfo **SelIdents,
                                       unsigned NumSelIdents,
                                       bool AtArgumentExpression,
                                       bool IsSuper,
                                       ResultBuilder &Results);

void Sema::CodeCompleteDeclSpec(Scope *S, DeclSpec &DS,
                                bool AllowNonIdentifiers,
                                bool AllowNestedNameSpecifiers) {
  typedef CodeCompletionResult Result;
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        AllowNestedNameSpecifiers
                          ? CodeCompletionContext::CCC_PotentiallyQualifiedName
                          : CodeCompletionContext::CCC_Name);
  Results.EnterNewScope();
  
  // Type qualifiers can come after names.
  Results.AddResult(Result("const"));
  Results.AddResult(Result("volatile"));
  if (getLangOptions().C99)
    Results.AddResult(Result("restrict"));

  if (getLangOptions().CPlusPlus) {
    if (AllowNonIdentifiers) {
      Results.AddResult(Result("operator")); 
    }
    
    // Add nested-name-specifiers.
    if (AllowNestedNameSpecifiers) {
      Results.allowNestedNameSpecifiers();
      Results.setFilter(&ResultBuilder::IsImpossibleToSatisfy);
      CodeCompletionDeclConsumer Consumer(Results, CurContext);
      LookupVisibleDecls(S, LookupNestedNameSpecifierName, Consumer,
                         CodeCompleter->includeGlobals());
      Results.setFilter(0);
    }
  }
  Results.ExitScope();

  // If we're in a context where we might have an expression (rather than a
  // declaration), and what we've seen so far is an Objective-C type that could
  // be a receiver of a class message, this may be a class message send with
  // the initial opening bracket '[' missing. Add appropriate completions.
  if (AllowNonIdentifiers && !AllowNestedNameSpecifiers &&
      DS.getTypeSpecType() == DeclSpec::TST_typename &&
      DS.getStorageClassSpecAsWritten() == DeclSpec::SCS_unspecified &&
      !DS.isThreadSpecified() && !DS.isExternInLinkageSpec() &&
      DS.getTypeSpecComplex() == DeclSpec::TSC_unspecified &&
      DS.getTypeSpecSign() == DeclSpec::TSS_unspecified &&
      DS.getTypeQualifiers() == 0 &&
      S && 
      (S->getFlags() & Scope::DeclScope) != 0 &&
      (S->getFlags() & (Scope::ClassScope | Scope::TemplateParamScope |
                        Scope::FunctionPrototypeScope | 
                        Scope::AtCatchScope)) == 0) {
    ParsedType T = DS.getRepAsType();
    if (!T.get().isNull() && T.get()->isObjCObjectOrInterfaceType())
      AddClassMessageCompletions(*this, S, T, 0, 0, false, false, Results); 
  }

  // Note that we intentionally suppress macro results here, since we do not
  // encourage using macros to produce the names of entities.

  HandleCodeCompleteResults(this, CodeCompleter, 
                            Results.getCompletionContext(),
                            Results.data(), Results.size());
}

struct Sema::CodeCompleteExpressionData {
  CodeCompleteExpressionData(QualType PreferredType = QualType()) 
    : PreferredType(PreferredType), IntegralConstantExpression(false),
      ObjCCollection(false) { }
  
  QualType PreferredType;
  bool IntegralConstantExpression;
  bool ObjCCollection;
  SmallVector<Decl *, 4> IgnoreDecls;
};

/// \brief Perform code-completion in an expression context when we know what
/// type we're looking for.
///
/// \param IntegralConstantExpression Only permit integral constant 
/// expressions.
void Sema::CodeCompleteExpression(Scope *S, 
                                  const CodeCompleteExpressionData &Data) {
  typedef CodeCompletionResult Result;
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_Expression);
  if (Data.ObjCCollection)
    Results.setFilter(&ResultBuilder::IsObjCCollection);
  else if (Data.IntegralConstantExpression)
    Results.setFilter(&ResultBuilder::IsIntegralConstantValue);
  else if (WantTypesInContext(PCC_Expression, getLangOptions()))
    Results.setFilter(&ResultBuilder::IsOrdinaryName);
  else
    Results.setFilter(&ResultBuilder::IsOrdinaryNonTypeName);

  if (!Data.PreferredType.isNull())
    Results.setPreferredType(Data.PreferredType.getNonReferenceType());
  
  // Ignore any declarations that we were told that we don't care about.
  for (unsigned I = 0, N = Data.IgnoreDecls.size(); I != N; ++I)
    Results.Ignore(Data.IgnoreDecls[I]);
  
  CodeCompletionDeclConsumer Consumer(Results, CurContext);
  LookupVisibleDecls(S, LookupOrdinaryName, Consumer,
                     CodeCompleter->includeGlobals());
  
  Results.EnterNewScope();
  AddOrdinaryNameResults(PCC_Expression, S, *this, Results);
  Results.ExitScope();
  
  bool PreferredTypeIsPointer = false;
  if (!Data.PreferredType.isNull())
    PreferredTypeIsPointer = Data.PreferredType->isAnyPointerType()
      || Data.PreferredType->isMemberPointerType() 
      || Data.PreferredType->isBlockPointerType();
  
  if (S->getFnParent() && 
      !Data.ObjCCollection && 
      !Data.IntegralConstantExpression)
    AddPrettyFunctionResults(PP.getLangOptions(), Results);        

  if (CodeCompleter->includeMacros())
    AddMacroResults(PP, Results, PreferredTypeIsPointer);
  HandleCodeCompleteResults(this, CodeCompleter, 
                CodeCompletionContext(CodeCompletionContext::CCC_Expression, 
                                      Data.PreferredType),
                            Results.data(),Results.size());
}

void Sema::CodeCompletePostfixExpression(Scope *S, ExprResult E) {
  if (E.isInvalid())
    CodeCompleteOrdinaryName(S, PCC_RecoveryInFunction);
  else if (getLangOptions().ObjC1)
    CodeCompleteObjCInstanceMessage(S, E.take(), 0, 0, false);
}

/// \brief The set of properties that have already been added, referenced by
/// property name.
typedef llvm::SmallPtrSet<IdentifierInfo*, 16> AddedPropertiesSet;

static void AddObjCProperties(ObjCContainerDecl *Container, 
                              bool AllowCategories,
                              bool AllowNullaryMethods,
                              DeclContext *CurContext,
                              AddedPropertiesSet &AddedProperties,
                              ResultBuilder &Results) {
  typedef CodeCompletionResult Result;

  // Add properties in this container.
  for (ObjCContainerDecl::prop_iterator P = Container->prop_begin(),
                                     PEnd = Container->prop_end();
       P != PEnd;
       ++P) {
    if (AddedProperties.insert(P->getIdentifier()))
      Results.MaybeAddResult(Result(*P, 0), CurContext);
  }
  
  // Add nullary methods
  if (AllowNullaryMethods) {
    ASTContext &Context = Container->getASTContext();
    PrintingPolicy Policy = getCompletionPrintingPolicy(Results.getSema());
    for (ObjCContainerDecl::method_iterator M = Container->meth_begin(),
                                         MEnd = Container->meth_end();
         M != MEnd; ++M) {
      if (M->getSelector().isUnarySelector())
        if (IdentifierInfo *Name = M->getSelector().getIdentifierInfoForSlot(0))
          if (AddedProperties.insert(Name)) {
            CodeCompletionBuilder Builder(Results.getAllocator());
            AddResultTypeChunk(Context, Policy, *M, Builder);
            Builder.AddTypedTextChunk(
                            Results.getAllocator().CopyString(Name->getName()));
            
            CXAvailabilityKind Availability = CXAvailability_Available;
            switch (M->getAvailability()) {
            case AR_Available:
            case AR_NotYetIntroduced:
              Availability = CXAvailability_Available;      
              break;
              
            case AR_Deprecated:
              Availability = CXAvailability_Deprecated;
              break;
              
            case AR_Unavailable:
              Availability = CXAvailability_NotAvailable;
              break;
            }

            Results.MaybeAddResult(Result(Builder.TakeString(),
                                  CCP_MemberDeclaration + CCD_MethodAsProperty,
                                          M->isInstanceMethod()
                                            ? CXCursor_ObjCInstanceMethodDecl
                                            : CXCursor_ObjCClassMethodDecl,
                                          Availability),
                                          CurContext);
          }
    }
  }
    
  
  // Add properties in referenced protocols.
  if (ObjCProtocolDecl *Protocol = dyn_cast<ObjCProtocolDecl>(Container)) {
    for (ObjCProtocolDecl::protocol_iterator P = Protocol->protocol_begin(),
                                          PEnd = Protocol->protocol_end();
         P != PEnd; ++P)
      AddObjCProperties(*P, AllowCategories, AllowNullaryMethods, CurContext, 
                        AddedProperties, Results);
  } else if (ObjCInterfaceDecl *IFace = dyn_cast<ObjCInterfaceDecl>(Container)){
    if (AllowCategories) {
      // Look through categories.
      for (ObjCCategoryDecl *Category = IFace->getCategoryList();
           Category; Category = Category->getNextClassCategory())
        AddObjCProperties(Category, AllowCategories, AllowNullaryMethods, 
                          CurContext, AddedProperties, Results);
    }
    
    // Look through protocols.
    for (ObjCInterfaceDecl::all_protocol_iterator
         I = IFace->all_referenced_protocol_begin(),
         E = IFace->all_referenced_protocol_end(); I != E; ++I)
      AddObjCProperties(*I, AllowCategories, AllowNullaryMethods, CurContext,
                        AddedProperties, Results);
    
    // Look in the superclass.
    if (IFace->getSuperClass())
      AddObjCProperties(IFace->getSuperClass(), AllowCategories, 
                        AllowNullaryMethods, CurContext, 
                        AddedProperties, Results);
  } else if (const ObjCCategoryDecl *Category
                                    = dyn_cast<ObjCCategoryDecl>(Container)) {
    // Look through protocols.
    for (ObjCCategoryDecl::protocol_iterator P = Category->protocol_begin(),
                                          PEnd = Category->protocol_end(); 
         P != PEnd; ++P)
      AddObjCProperties(*P, AllowCategories, AllowNullaryMethods, CurContext,
                        AddedProperties, Results);
  }
}

void Sema::CodeCompleteMemberReferenceExpr(Scope *S, Expr *BaseE,
                                           SourceLocation OpLoc,
                                           bool IsArrow) {
  if (!BaseE || !CodeCompleter)
    return;
  
  typedef CodeCompletionResult Result;
  
  Expr *Base = static_cast<Expr *>(BaseE);
  QualType BaseType = Base->getType();

  if (IsArrow) {
    if (const PointerType *Ptr = BaseType->getAs<PointerType>())
      BaseType = Ptr->getPointeeType();
    else if (BaseType->isObjCObjectPointerType())
      /*Do nothing*/ ;
    else
      return;
  }
  
  enum CodeCompletionContext::Kind contextKind;
  
  if (IsArrow) {
    contextKind = CodeCompletionContext::CCC_ArrowMemberAccess;
  }
  else {
    if (BaseType->isObjCObjectPointerType() ||
        BaseType->isObjCObjectOrInterfaceType()) {
      contextKind = CodeCompletionContext::CCC_ObjCPropertyAccess;
    }
    else {
      contextKind = CodeCompletionContext::CCC_DotMemberAccess;
    }
  }
  
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                  CodeCompletionContext(contextKind,
                                        BaseType),
                        &ResultBuilder::IsMember);
  Results.EnterNewScope();
  if (const RecordType *Record = BaseType->getAs<RecordType>()) {
    // Indicate that we are performing a member access, and the cv-qualifiers
    // for the base object type.
    Results.setObjectTypeQualifiers(BaseType.getQualifiers());
    
    // Access to a C/C++ class, struct, or union.
    Results.allowNestedNameSpecifiers();
    CodeCompletionDeclConsumer Consumer(Results, CurContext);
    LookupVisibleDecls(Record->getDecl(), LookupMemberName, Consumer,
                       CodeCompleter->includeGlobals());

    if (getLangOptions().CPlusPlus) {
      if (!Results.empty()) {
        // The "template" keyword can follow "->" or "." in the grammar.
        // However, we only want to suggest the template keyword if something
        // is dependent.
        bool IsDependent = BaseType->isDependentType();
        if (!IsDependent) {
          for (Scope *DepScope = S; DepScope; DepScope = DepScope->getParent())
            if (DeclContext *Ctx = (DeclContext *)DepScope->getEntity()) {
              IsDependent = Ctx->isDependentContext();
              break;
            }
        }

        if (IsDependent)
          Results.AddResult(Result("template"));
      }
    }
  } else if (!IsArrow && BaseType->getAsObjCInterfacePointerType()) {
    // Objective-C property reference.
    AddedPropertiesSet AddedProperties;
    
    // Add property results based on our interface.
    const ObjCObjectPointerType *ObjCPtr
      = BaseType->getAsObjCInterfacePointerType();
    assert(ObjCPtr && "Non-NULL pointer guaranteed above!");
    AddObjCProperties(ObjCPtr->getInterfaceDecl(), true, 
                      /*AllowNullaryMethods=*/true, CurContext, 
                      AddedProperties, Results);
    
    // Add properties from the protocols in a qualified interface.
    for (ObjCObjectPointerType::qual_iterator I = ObjCPtr->qual_begin(),
                                              E = ObjCPtr->qual_end();
         I != E; ++I)
      AddObjCProperties(*I, true, /*AllowNullaryMethods=*/true, CurContext, 
                        AddedProperties, Results);
  } else if ((IsArrow && BaseType->isObjCObjectPointerType()) ||
             (!IsArrow && BaseType->isObjCObjectType())) {
    // Objective-C instance variable access.
    ObjCInterfaceDecl *Class = 0;
    if (const ObjCObjectPointerType *ObjCPtr
                                    = BaseType->getAs<ObjCObjectPointerType>())
      Class = ObjCPtr->getInterfaceDecl();
    else
      Class = BaseType->getAs<ObjCObjectType>()->getInterface();
    
    // Add all ivars from this class and its superclasses.
    if (Class) {
      CodeCompletionDeclConsumer Consumer(Results, CurContext);
      Results.setFilter(&ResultBuilder::IsObjCIvar);
      LookupVisibleDecls(Class, LookupMemberName, Consumer,
                         CodeCompleter->includeGlobals());
    }
  }
  
  // FIXME: How do we cope with isa?
  
  Results.ExitScope();

  // Hand off the results found for code completion.
  HandleCodeCompleteResults(this, CodeCompleter, 
                            Results.getCompletionContext(),
                            Results.data(),Results.size());
}

void Sema::CodeCompleteTag(Scope *S, unsigned TagSpec) {
  if (!CodeCompleter)
    return;
  
  typedef CodeCompletionResult Result;
  ResultBuilder::LookupFilter Filter = 0;
  enum CodeCompletionContext::Kind ContextKind
    = CodeCompletionContext::CCC_Other;
  switch ((DeclSpec::TST)TagSpec) {
  case DeclSpec::TST_enum:
    Filter = &ResultBuilder::IsEnum;
    ContextKind = CodeCompletionContext::CCC_EnumTag;
    break;
    
  case DeclSpec::TST_union:
    Filter = &ResultBuilder::IsUnion;
    ContextKind = CodeCompletionContext::CCC_UnionTag;
    break;
    
  case DeclSpec::TST_struct:
  case DeclSpec::TST_class:
    Filter = &ResultBuilder::IsClassOrStruct;
    ContextKind = CodeCompletionContext::CCC_ClassOrStructTag;
    break;
    
  default:
    llvm_unreachable("Unknown type specifier kind in CodeCompleteTag");
  }
  
  ResultBuilder Results(*this, CodeCompleter->getAllocator(), ContextKind);
  CodeCompletionDeclConsumer Consumer(Results, CurContext);

  // First pass: look for tags.
  Results.setFilter(Filter);
  LookupVisibleDecls(S, LookupTagName, Consumer,
                     CodeCompleter->includeGlobals());

  if (CodeCompleter->includeGlobals()) {
    // Second pass: look for nested name specifiers.
    Results.setFilter(&ResultBuilder::IsNestedNameSpecifier);
    LookupVisibleDecls(S, LookupNestedNameSpecifierName, Consumer);
  }
  
  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(),Results.size());
}

void Sema::CodeCompleteTypeQualifiers(DeclSpec &DS) {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_TypeQualifiers);
  Results.EnterNewScope();
  if (!(DS.getTypeQualifiers() & DeclSpec::TQ_const))
    Results.AddResult("const");
  if (!(DS.getTypeQualifiers() & DeclSpec::TQ_volatile))
    Results.AddResult("volatile");
  if (getLangOptions().C99 &&
      !(DS.getTypeQualifiers() & DeclSpec::TQ_restrict))
    Results.AddResult("restrict");
  Results.ExitScope();
  HandleCodeCompleteResults(this, CodeCompleter, 
                            Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteCase(Scope *S) {
  if (getCurFunction()->SwitchStack.empty() || !CodeCompleter)
    return;

  SwitchStmt *Switch = getCurFunction()->SwitchStack.back();
  QualType type = Switch->getCond()->IgnoreImplicit()->getType();
  if (!type->isEnumeralType()) {
    CodeCompleteExpressionData Data(type);
    Data.IntegralConstantExpression = true;
    CodeCompleteExpression(S, Data);
    return;
  }
  
  // Code-complete the cases of a switch statement over an enumeration type
  // by providing the list of 
  EnumDecl *Enum = type->castAs<EnumType>()->getDecl();
  
  // Determine which enumerators we have already seen in the switch statement.
  // FIXME: Ideally, we would also be able to look *past* the code-completion
  // token, in case we are code-completing in the middle of the switch and not
  // at the end. However, we aren't able to do so at the moment.
  llvm::SmallPtrSet<EnumConstantDecl *, 8> EnumeratorsSeen;
  NestedNameSpecifier *Qualifier = 0;
  for (SwitchCase *SC = Switch->getSwitchCaseList(); SC; 
       SC = SC->getNextSwitchCase()) {
    CaseStmt *Case = dyn_cast<CaseStmt>(SC);
    if (!Case)
      continue;

    Expr *CaseVal = Case->getLHS()->IgnoreParenCasts();
    if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(CaseVal))
      if (EnumConstantDecl *Enumerator 
            = dyn_cast<EnumConstantDecl>(DRE->getDecl())) {
        // We look into the AST of the case statement to determine which 
        // enumerator was named. Alternatively, we could compute the value of 
        // the integral constant expression, then compare it against the
        // values of each enumerator. However, value-based approach would not 
        // work as well with C++ templates where enumerators declared within a 
        // template are type- and value-dependent.
        EnumeratorsSeen.insert(Enumerator);
        
        // If this is a qualified-id, keep track of the nested-name-specifier
        // so that we can reproduce it as part of code completion, e.g.,
        //
        //   switch (TagD.getKind()) {
        //     case TagDecl::TK_enum:
        //       break;
        //     case XXX
        //
        // At the XXX, our completions are TagDecl::TK_union,
        // TagDecl::TK_struct, and TagDecl::TK_class, rather than TK_union,
        // TK_struct, and TK_class.
        Qualifier = DRE->getQualifier();
      }
  }
  
  if (getLangOptions().CPlusPlus && !Qualifier && EnumeratorsSeen.empty()) {
    // If there are no prior enumerators in C++, check whether we have to 
    // qualify the names of the enumerators that we suggest, because they
    // may not be visible in this scope.
    Qualifier = getRequiredQualification(Context, CurContext,
                                         Enum->getDeclContext());
    
    // FIXME: Scoped enums need to start with "EnumDecl" as the context!
  }
  
  // Add any enumerators that have not yet been mentioned.
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_Expression);
  Results.EnterNewScope();
  for (EnumDecl::enumerator_iterator E = Enum->enumerator_begin(),
                                  EEnd = Enum->enumerator_end();
       E != EEnd; ++E) {
    if (EnumeratorsSeen.count(*E))
      continue;
    
    CodeCompletionResult R(*E, Qualifier);
    R.Priority = CCP_EnumInCase;
    Results.AddResult(R, CurContext, 0, false);
  }
  Results.ExitScope();

  //We need to make sure we're setting the right context, 
  //so only say we include macros if the code completer says we do
  enum CodeCompletionContext::Kind kind = CodeCompletionContext::CCC_Other;
  if (CodeCompleter->includeMacros()) {
    AddMacroResults(PP, Results);
    kind = CodeCompletionContext::CCC_OtherWithMacros;
  }
  
  
  HandleCodeCompleteResults(this, CodeCompleter, 
                            kind,
                            Results.data(),Results.size());
}

namespace {
  struct IsBetterOverloadCandidate {
    Sema &S;
    SourceLocation Loc;
    
  public:
    explicit IsBetterOverloadCandidate(Sema &S, SourceLocation Loc)
      : S(S), Loc(Loc) { }
    
    bool 
    operator()(const OverloadCandidate &X, const OverloadCandidate &Y) const {
      return isBetterOverloadCandidate(S, X, Y, Loc);
    }
  };
}

static bool anyNullArguments(Expr **Args, unsigned NumArgs) {
  if (NumArgs && !Args)
    return true;
  
  for (unsigned I = 0; I != NumArgs; ++I)
    if (!Args[I])
      return true;
  
  return false;
}

void Sema::CodeCompleteCall(Scope *S, Expr *FnIn,
                            Expr **ArgsIn, unsigned NumArgs) {
  if (!CodeCompleter)
    return;

  // When we're code-completing for a call, we fall back to ordinary
  // name code-completion whenever we can't produce specific
  // results. We may want to revisit this strategy in the future,
  // e.g., by merging the two kinds of results.

  Expr *Fn = (Expr *)FnIn;
  Expr **Args = (Expr **)ArgsIn;

  // Ignore type-dependent call expressions entirely.
  if (!Fn || Fn->isTypeDependent() || anyNullArguments(Args, NumArgs) ||
      Expr::hasAnyTypeDependentArguments(Args, NumArgs)) {
    CodeCompleteOrdinaryName(S, PCC_Expression);
    return;
  }

  // Build an overload candidate set based on the functions we find.
  SourceLocation Loc = Fn->getExprLoc();
  OverloadCandidateSet CandidateSet(Loc);

  // FIXME: What if we're calling something that isn't a function declaration?
  // FIXME: What if we're calling a pseudo-destructor?
  // FIXME: What if we're calling a member function?
  
  typedef CodeCompleteConsumer::OverloadCandidate ResultCandidate;
  SmallVector<ResultCandidate, 8> Results;

  Expr *NakedFn = Fn->IgnoreParenCasts();
  if (UnresolvedLookupExpr *ULE = dyn_cast<UnresolvedLookupExpr>(NakedFn))
    AddOverloadedCallCandidates(ULE, Args, NumArgs, CandidateSet,
                                /*PartialOverloading=*/ true);
  else if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(NakedFn)) {
    FunctionDecl *FDecl = dyn_cast<FunctionDecl>(DRE->getDecl());
    if (FDecl) {
      if (!getLangOptions().CPlusPlus || 
          !FDecl->getType()->getAs<FunctionProtoType>())
        Results.push_back(ResultCandidate(FDecl));
      else
        // FIXME: access?
        AddOverloadCandidate(FDecl, DeclAccessPair::make(FDecl, AS_none),
                             Args, NumArgs, CandidateSet,
                             false, /*PartialOverloading*/true);
    }
  }
  
  QualType ParamType;
  
  if (!CandidateSet.empty()) {
    // Sort the overload candidate set by placing the best overloads first.
    std::stable_sort(CandidateSet.begin(), CandidateSet.end(),
                     IsBetterOverloadCandidate(*this, Loc));
  
    // Add the remaining viable overload candidates as code-completion reslults.
    for (OverloadCandidateSet::iterator Cand = CandidateSet.begin(),
                                     CandEnd = CandidateSet.end();
         Cand != CandEnd; ++Cand) {
      if (Cand->Viable)
        Results.push_back(ResultCandidate(Cand->Function));
    }

    // From the viable candidates, try to determine the type of this parameter.
    for (unsigned I = 0, N = Results.size(); I != N; ++I) {
      if (const FunctionType *FType = Results[I].getFunctionType())
        if (const FunctionProtoType *Proto = dyn_cast<FunctionProtoType>(FType))
          if (NumArgs < Proto->getNumArgs()) {
            if (ParamType.isNull())
              ParamType = Proto->getArgType(NumArgs);
            else if (!Context.hasSameUnqualifiedType(
                                            ParamType.getNonReferenceType(),
                           Proto->getArgType(NumArgs).getNonReferenceType())) {
              ParamType = QualType();
              break;
            }
          }
    }
  } else {
    // Try to determine the parameter type from the type of the expression
    // being called.
    QualType FunctionType = Fn->getType();
    if (const PointerType *Ptr = FunctionType->getAs<PointerType>())
      FunctionType = Ptr->getPointeeType();
    else if (const BlockPointerType *BlockPtr
                                    = FunctionType->getAs<BlockPointerType>())
      FunctionType = BlockPtr->getPointeeType();
    else if (const MemberPointerType *MemPtr
                                    = FunctionType->getAs<MemberPointerType>())
      FunctionType = MemPtr->getPointeeType();
    
    if (const FunctionProtoType *Proto
                                  = FunctionType->getAs<FunctionProtoType>()) {
      if (NumArgs < Proto->getNumArgs())
        ParamType = Proto->getArgType(NumArgs);
    }
  }

  if (ParamType.isNull())
    CodeCompleteOrdinaryName(S, PCC_Expression);
  else
    CodeCompleteExpression(S, ParamType);
  
  if (!Results.empty())
    CodeCompleter->ProcessOverloadCandidates(*this, NumArgs, Results.data(), 
                                             Results.size());
}

void Sema::CodeCompleteInitializer(Scope *S, Decl *D) {
  ValueDecl *VD = dyn_cast_or_null<ValueDecl>(D);
  if (!VD) {
    CodeCompleteOrdinaryName(S, PCC_Expression);
    return;
  }
  
  CodeCompleteExpression(S, VD->getType());
}

void Sema::CodeCompleteReturn(Scope *S) {
  QualType ResultType;
  if (isa<BlockDecl>(CurContext)) {
    if (BlockScopeInfo *BSI = getCurBlock())
      ResultType = BSI->ReturnType;
  } else if (FunctionDecl *Function = dyn_cast<FunctionDecl>(CurContext))
    ResultType = Function->getResultType();
  else if (ObjCMethodDecl *Method = dyn_cast<ObjCMethodDecl>(CurContext))
    ResultType = Method->getResultType();
  
  if (ResultType.isNull())
    CodeCompleteOrdinaryName(S, PCC_Expression);
  else
    CodeCompleteExpression(S, ResultType);
}

void Sema::CodeCompleteAfterIf(Scope *S) {
  typedef CodeCompletionResult Result;
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        mapCodeCompletionContext(*this, PCC_Statement));
  Results.setFilter(&ResultBuilder::IsOrdinaryName);
  Results.EnterNewScope();
  
  CodeCompletionDeclConsumer Consumer(Results, CurContext);
  LookupVisibleDecls(S, LookupOrdinaryName, Consumer,
                     CodeCompleter->includeGlobals());
  
  AddOrdinaryNameResults(PCC_Statement, S, *this, Results);
  
  // "else" block
  CodeCompletionBuilder Builder(Results.getAllocator());
  Builder.AddTypedTextChunk("else");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
  Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
  Builder.AddPlaceholderChunk("statements");
  Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
  Builder.AddChunk(CodeCompletionString::CK_RightBrace);
  Results.AddResult(Builder.TakeString());

  // "else if" block
  Builder.AddTypedTextChunk("else");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddTextChunk("if");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddChunk(CodeCompletionString::CK_LeftParen);
  if (getLangOptions().CPlusPlus)
    Builder.AddPlaceholderChunk("condition");
  else
    Builder.AddPlaceholderChunk("expression");
  Builder.AddChunk(CodeCompletionString::CK_RightParen);
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
  Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
  Builder.AddPlaceholderChunk("statements");
  Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
  Builder.AddChunk(CodeCompletionString::CK_RightBrace);
  Results.AddResult(Builder.TakeString());

  Results.ExitScope();
  
  if (S->getFnParent())
    AddPrettyFunctionResults(PP.getLangOptions(), Results);        
  
  if (CodeCompleter->includeMacros())
    AddMacroResults(PP, Results);
  
  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(),Results.size());
}

void Sema::CodeCompleteAssignmentRHS(Scope *S, Expr *LHS) {
  if (LHS)
    CodeCompleteExpression(S, static_cast<Expr *>(LHS)->getType());
  else
    CodeCompleteOrdinaryName(S, PCC_Expression);
}

void Sema::CodeCompleteQualifiedId(Scope *S, CXXScopeSpec &SS,
                                   bool EnteringContext) {
  if (!SS.getScopeRep() || !CodeCompleter)
    return;
  
  DeclContext *Ctx = computeDeclContext(SS, EnteringContext);
  if (!Ctx)
    return;

  // Try to instantiate any non-dependent declaration contexts before
  // we look in them.
  if (!isDependentScopeSpecifier(SS) && RequireCompleteDeclContext(SS, Ctx))
    return;

  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_Name);
  Results.EnterNewScope();
  
  // The "template" keyword can follow "::" in the grammar, but only
  // put it into the grammar if the nested-name-specifier is dependent.
  NestedNameSpecifier *NNS = (NestedNameSpecifier *)SS.getScopeRep();
  if (!Results.empty() && NNS->isDependent())
    Results.AddResult("template");

  // Add calls to overridden virtual functions, if there are any.
  //
  // FIXME: This isn't wonderful, because we don't know whether we're actually
  // in a context that permits expressions. This is a general issue with
  // qualified-id completions.
  if (!EnteringContext)
    MaybeAddOverrideCalls(*this, Ctx, Results);
  Results.ExitScope();  
  
  CodeCompletionDeclConsumer Consumer(Results, CurContext);
  LookupVisibleDecls(Ctx, LookupOrdinaryName, Consumer);

  HandleCodeCompleteResults(this, CodeCompleter, 
                            Results.getCompletionContext(),
                            Results.data(),Results.size());
}

void Sema::CodeCompleteUsing(Scope *S) {
  if (!CodeCompleter)
    return;
  
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_PotentiallyQualifiedName,
                        &ResultBuilder::IsNestedNameSpecifier);
  Results.EnterNewScope();
  
  // If we aren't in class scope, we could see the "namespace" keyword.
  if (!S->isClassScope())
    Results.AddResult(CodeCompletionResult("namespace"));
  
  // After "using", we can see anything that would start a 
  // nested-name-specifier.
  CodeCompletionDeclConsumer Consumer(Results, CurContext);
  LookupVisibleDecls(S, LookupOrdinaryName, Consumer,
                     CodeCompleter->includeGlobals());
  Results.ExitScope();
  
  HandleCodeCompleteResults(this, CodeCompleter, 
                            CodeCompletionContext::CCC_PotentiallyQualifiedName,
                            Results.data(),Results.size());
}

void Sema::CodeCompleteUsingDirective(Scope *S) {
  if (!CodeCompleter)
    return;
  
  // After "using namespace", we expect to see a namespace name or namespace
  // alias.
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_Namespace,
                        &ResultBuilder::IsNamespaceOrAlias);
  Results.EnterNewScope();
  CodeCompletionDeclConsumer Consumer(Results, CurContext);
  LookupVisibleDecls(S, LookupOrdinaryName, Consumer,
                     CodeCompleter->includeGlobals());
  Results.ExitScope();
  HandleCodeCompleteResults(this, CodeCompleter, 
                            CodeCompletionContext::CCC_Namespace,
                            Results.data(),Results.size());
}

void Sema::CodeCompleteNamespaceDecl(Scope *S)  {
  if (!CodeCompleter)
    return;
  
  DeclContext *Ctx = (DeclContext *)S->getEntity();
  if (!S->getParent())
    Ctx = Context.getTranslationUnitDecl();
  
  bool SuppressedGlobalResults
    = Ctx && !CodeCompleter->includeGlobals() && isa<TranslationUnitDecl>(Ctx);
  
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        SuppressedGlobalResults
                          ? CodeCompletionContext::CCC_Namespace
                          : CodeCompletionContext::CCC_Other,
                        &ResultBuilder::IsNamespace);
  
  if (Ctx && Ctx->isFileContext() && !SuppressedGlobalResults) {
    // We only want to see those namespaces that have already been defined
    // within this scope, because its likely that the user is creating an
    // extended namespace declaration. Keep track of the most recent 
    // definition of each namespace.
    std::map<NamespaceDecl *, NamespaceDecl *> OrigToLatest;
    for (DeclContext::specific_decl_iterator<NamespaceDecl> 
         NS(Ctx->decls_begin()), NSEnd(Ctx->decls_end());
         NS != NSEnd; ++NS)
      OrigToLatest[NS->getOriginalNamespace()] = *NS;
    
    // Add the most recent definition (or extended definition) of each 
    // namespace to the list of results.
    Results.EnterNewScope();
    for (std::map<NamespaceDecl *, NamespaceDecl *>::iterator 
         NS = OrigToLatest.begin(), NSEnd = OrigToLatest.end();
         NS != NSEnd; ++NS)
      Results.AddResult(CodeCompletionResult(NS->second, 0),
                        CurContext, 0, false);
    Results.ExitScope();
  }
  
  HandleCodeCompleteResults(this, CodeCompleter, 
                            Results.getCompletionContext(),
                            Results.data(),Results.size());
}

void Sema::CodeCompleteNamespaceAliasDecl(Scope *S)  {
  if (!CodeCompleter)
    return;
  
  // After "namespace", we expect to see a namespace or alias.
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_Namespace,
                        &ResultBuilder::IsNamespaceOrAlias);
  CodeCompletionDeclConsumer Consumer(Results, CurContext);
  LookupVisibleDecls(S, LookupOrdinaryName, Consumer,
                     CodeCompleter->includeGlobals());
  HandleCodeCompleteResults(this, CodeCompleter, 
                            Results.getCompletionContext(),
                            Results.data(),Results.size());
}

void Sema::CodeCompleteOperatorName(Scope *S) {
  if (!CodeCompleter)
    return;

  typedef CodeCompletionResult Result;
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_Type,
                        &ResultBuilder::IsType);
  Results.EnterNewScope();
  
  // Add the names of overloadable operators.
#define OVERLOADED_OPERATOR(Name,Spelling,Token,Unary,Binary,MemberOnly)      \
  if (std::strcmp(Spelling, "?"))                                                  \
    Results.AddResult(Result(Spelling));
#include "clang/Basic/OperatorKinds.def"
  
  // Add any type names visible from the current scope
  Results.allowNestedNameSpecifiers();
  CodeCompletionDeclConsumer Consumer(Results, CurContext);
  LookupVisibleDecls(S, LookupOrdinaryName, Consumer,
                     CodeCompleter->includeGlobals());
  
  // Add any type specifiers
  AddTypeSpecifierResults(getLangOptions(), Results);
  Results.ExitScope();
  
  HandleCodeCompleteResults(this, CodeCompleter, 
                            CodeCompletionContext::CCC_Type,
                            Results.data(),Results.size());
}

void Sema::CodeCompleteConstructorInitializer(Decl *ConstructorD,
                                              CXXCtorInitializer** Initializers,
                                              unsigned NumInitializers) {
  PrintingPolicy Policy = getCompletionPrintingPolicy(*this);
  CXXConstructorDecl *Constructor
    = static_cast<CXXConstructorDecl *>(ConstructorD);
  if (!Constructor)
    return;
  
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_PotentiallyQualifiedName);
  Results.EnterNewScope();
  
  // Fill in any already-initialized fields or base classes.
  llvm::SmallPtrSet<FieldDecl *, 4> InitializedFields;
  llvm::SmallPtrSet<CanQualType, 4> InitializedBases;
  for (unsigned I = 0; I != NumInitializers; ++I) {
    if (Initializers[I]->isBaseInitializer())
      InitializedBases.insert(
        Context.getCanonicalType(QualType(Initializers[I]->getBaseClass(), 0)));
    else
      InitializedFields.insert(cast<FieldDecl>(
                               Initializers[I]->getAnyMember()));
  }
  
  // Add completions for base classes.
  CodeCompletionBuilder Builder(Results.getAllocator());
  bool SawLastInitializer = (NumInitializers == 0);
  CXXRecordDecl *ClassDecl = Constructor->getParent();
  for (CXXRecordDecl::base_class_iterator Base = ClassDecl->bases_begin(),
                                       BaseEnd = ClassDecl->bases_end();
       Base != BaseEnd; ++Base) {
    if (!InitializedBases.insert(Context.getCanonicalType(Base->getType()))) {
      SawLastInitializer
        = NumInitializers > 0 && 
          Initializers[NumInitializers - 1]->isBaseInitializer() &&
          Context.hasSameUnqualifiedType(Base->getType(),
               QualType(Initializers[NumInitializers - 1]->getBaseClass(), 0));
      continue;
    }
    
    Builder.AddTypedTextChunk(
               Results.getAllocator().CopyString(
                          Base->getType().getAsString(Policy)));
    Builder.AddChunk(CodeCompletionString::CK_LeftParen);
    Builder.AddPlaceholderChunk("args");
    Builder.AddChunk(CodeCompletionString::CK_RightParen);
    Results.AddResult(CodeCompletionResult(Builder.TakeString(), 
                                   SawLastInitializer? CCP_NextInitializer
                                                     : CCP_MemberDeclaration));
    SawLastInitializer = false;
  }
  
  // Add completions for virtual base classes.
  for (CXXRecordDecl::base_class_iterator Base = ClassDecl->vbases_begin(),
                                       BaseEnd = ClassDecl->vbases_end();
       Base != BaseEnd; ++Base) {
    if (!InitializedBases.insert(Context.getCanonicalType(Base->getType()))) {
      SawLastInitializer
        = NumInitializers > 0 && 
          Initializers[NumInitializers - 1]->isBaseInitializer() &&
          Context.hasSameUnqualifiedType(Base->getType(),
               QualType(Initializers[NumInitializers - 1]->getBaseClass(), 0));
      continue;
    }
    
    Builder.AddTypedTextChunk(
               Builder.getAllocator().CopyString(
                          Base->getType().getAsString(Policy)));
    Builder.AddChunk(CodeCompletionString::CK_LeftParen);
    Builder.AddPlaceholderChunk("args");
    Builder.AddChunk(CodeCompletionString::CK_RightParen);
    Results.AddResult(CodeCompletionResult(Builder.TakeString(), 
                                   SawLastInitializer? CCP_NextInitializer
                                                     : CCP_MemberDeclaration));
    SawLastInitializer = false;
  }
  
  // Add completions for members.
  for (CXXRecordDecl::field_iterator Field = ClassDecl->field_begin(),
                                  FieldEnd = ClassDecl->field_end();
       Field != FieldEnd; ++Field) {
    if (!InitializedFields.insert(cast<FieldDecl>(Field->getCanonicalDecl()))) {
      SawLastInitializer
        = NumInitializers > 0 && 
          Initializers[NumInitializers - 1]->isAnyMemberInitializer() &&
          Initializers[NumInitializers - 1]->getAnyMember() == *Field;
      continue;
    }
    
    if (!Field->getDeclName())
      continue;
    
    Builder.AddTypedTextChunk(Builder.getAllocator().CopyString(
                                         Field->getIdentifier()->getName()));
    Builder.AddChunk(CodeCompletionString::CK_LeftParen);
    Builder.AddPlaceholderChunk("args");
    Builder.AddChunk(CodeCompletionString::CK_RightParen);
    Results.AddResult(CodeCompletionResult(Builder.TakeString(), 
                                   SawLastInitializer? CCP_NextInitializer
                                                     : CCP_MemberDeclaration,
                                           CXCursor_MemberRef));
    SawLastInitializer = false;
  }
  Results.ExitScope();
  
  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

// Macro that expands to @Keyword or Keyword, depending on whether NeedAt is
// true or false.
#define OBJC_AT_KEYWORD_NAME(NeedAt,Keyword) NeedAt? "@" #Keyword : #Keyword
static void AddObjCImplementationResults(const LangOptions &LangOpts,
                                         ResultBuilder &Results,
                                         bool NeedAt) {
  typedef CodeCompletionResult Result;
  // Since we have an implementation, we can end it.
  Results.AddResult(Result(OBJC_AT_KEYWORD_NAME(NeedAt,end)));
  
  CodeCompletionBuilder Builder(Results.getAllocator());
  if (LangOpts.ObjC2) {
    // @dynamic
    Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt,dynamic));
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddPlaceholderChunk("property");
    Results.AddResult(Result(Builder.TakeString()));
    
    // @synthesize
    Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt,synthesize));
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddPlaceholderChunk("property");
    Results.AddResult(Result(Builder.TakeString()));
  }  
}

static void AddObjCInterfaceResults(const LangOptions &LangOpts,
                                    ResultBuilder &Results,
                                    bool NeedAt) {
  typedef CodeCompletionResult Result;
  
  // Since we have an interface or protocol, we can end it.
  Results.AddResult(Result(OBJC_AT_KEYWORD_NAME(NeedAt,end)));
  
  if (LangOpts.ObjC2) {
    // @property
    Results.AddResult(Result(OBJC_AT_KEYWORD_NAME(NeedAt,property)));
  
    // @required
    Results.AddResult(Result(OBJC_AT_KEYWORD_NAME(NeedAt,required)));
  
    // @optional
    Results.AddResult(Result(OBJC_AT_KEYWORD_NAME(NeedAt,optional)));
  }
}

static void AddObjCTopLevelResults(ResultBuilder &Results, bool NeedAt) {
  typedef CodeCompletionResult Result;
  CodeCompletionBuilder Builder(Results.getAllocator());
  
  // @class name ;
  Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt,class));
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("name");
  Results.AddResult(Result(Builder.TakeString()));
  
  if (Results.includeCodePatterns()) {
    // @interface name 
    // FIXME: Could introduce the whole pattern, including superclasses and 
    // such.
    Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt,interface));
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddPlaceholderChunk("class");
    Results.AddResult(Result(Builder.TakeString()));
  
    // @protocol name
    Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt,protocol));
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddPlaceholderChunk("protocol");
    Results.AddResult(Result(Builder.TakeString()));
    
    // @implementation name
    Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt,implementation));
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddPlaceholderChunk("class");
    Results.AddResult(Result(Builder.TakeString()));
  }
  
  // @compatibility_alias name
  Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt,compatibility_alias));
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("alias");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("class");
  Results.AddResult(Result(Builder.TakeString()));
}

void Sema::CodeCompleteObjCAtDirective(Scope *S) {
  typedef CodeCompletionResult Result;
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_Other);
  Results.EnterNewScope();
  if (isa<ObjCImplDecl>(CurContext))
    AddObjCImplementationResults(getLangOptions(), Results, false);
  else if (CurContext->isObjCContainer())
    AddObjCInterfaceResults(getLangOptions(), Results, false);
  else
    AddObjCTopLevelResults(Results, false);
  Results.ExitScope();
  HandleCodeCompleteResults(this, CodeCompleter, 
                            CodeCompletionContext::CCC_Other,
                            Results.data(),Results.size());
}

static void AddObjCExpressionResults(ResultBuilder &Results, bool NeedAt) {
  typedef CodeCompletionResult Result;
  CodeCompletionBuilder Builder(Results.getAllocator());

  // @encode ( type-name )
  const char *EncodeType = "char[]";
  if (Results.getSema().getLangOptions().CPlusPlus ||
      Results.getSema().getLangOptions().ConstStrings)
    EncodeType = " const char[]";
  Builder.AddResultTypeChunk(EncodeType);
  Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt,encode));
  Builder.AddChunk(CodeCompletionString::CK_LeftParen);
  Builder.AddPlaceholderChunk("type-name");
  Builder.AddChunk(CodeCompletionString::CK_RightParen);
  Results.AddResult(Result(Builder.TakeString()));
  
  // @protocol ( protocol-name )
  Builder.AddResultTypeChunk("Protocol *");
  Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt,protocol));
  Builder.AddChunk(CodeCompletionString::CK_LeftParen);
  Builder.AddPlaceholderChunk("protocol-name");
  Builder.AddChunk(CodeCompletionString::CK_RightParen);
  Results.AddResult(Result(Builder.TakeString()));

  // @selector ( selector )
  Builder.AddResultTypeChunk("SEL");
  Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt,selector));
  Builder.AddChunk(CodeCompletionString::CK_LeftParen);
  Builder.AddPlaceholderChunk("selector");
  Builder.AddChunk(CodeCompletionString::CK_RightParen);
  Results.AddResult(Result(Builder.TakeString()));
}

static void AddObjCStatementResults(ResultBuilder &Results, bool NeedAt) {
  typedef CodeCompletionResult Result;
  CodeCompletionBuilder Builder(Results.getAllocator());
  
  if (Results.includeCodePatterns()) {
    // @try { statements } @catch ( declaration ) { statements } @finally
    //   { statements }
    Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt,try));
    Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
    Builder.AddPlaceholderChunk("statements");
    Builder.AddChunk(CodeCompletionString::CK_RightBrace);
    Builder.AddTextChunk("@catch");
    Builder.AddChunk(CodeCompletionString::CK_LeftParen);
    Builder.AddPlaceholderChunk("parameter");
    Builder.AddChunk(CodeCompletionString::CK_RightParen);
    Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
    Builder.AddPlaceholderChunk("statements");
    Builder.AddChunk(CodeCompletionString::CK_RightBrace);
    Builder.AddTextChunk("@finally");
    Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
    Builder.AddPlaceholderChunk("statements");
    Builder.AddChunk(CodeCompletionString::CK_RightBrace);
    Results.AddResult(Result(Builder.TakeString()));
  }
  
  // @throw
  Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt,throw));
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("expression");
  Results.AddResult(Result(Builder.TakeString()));
  
  if (Results.includeCodePatterns()) {
    // @synchronized ( expression ) { statements }
    Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt,synchronized));
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddChunk(CodeCompletionString::CK_LeftParen);
    Builder.AddPlaceholderChunk("expression");
    Builder.AddChunk(CodeCompletionString::CK_RightParen);
    Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
    Builder.AddPlaceholderChunk("statements");
    Builder.AddChunk(CodeCompletionString::CK_RightBrace);
    Results.AddResult(Result(Builder.TakeString()));
  }
}

static void AddObjCVisibilityResults(const LangOptions &LangOpts,
                                     ResultBuilder &Results,
                                     bool NeedAt) {
  typedef CodeCompletionResult Result;
  Results.AddResult(Result(OBJC_AT_KEYWORD_NAME(NeedAt,private)));
  Results.AddResult(Result(OBJC_AT_KEYWORD_NAME(NeedAt,protected)));
  Results.AddResult(Result(OBJC_AT_KEYWORD_NAME(NeedAt,public)));
  if (LangOpts.ObjC2)
    Results.AddResult(Result(OBJC_AT_KEYWORD_NAME(NeedAt,package)));    
}

void Sema::CodeCompleteObjCAtVisibility(Scope *S) {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_Other);
  Results.EnterNewScope();
  AddObjCVisibilityResults(getLangOptions(), Results, false);
  Results.ExitScope();
  HandleCodeCompleteResults(this, CodeCompleter, 
                            CodeCompletionContext::CCC_Other,
                            Results.data(),Results.size());
}

void Sema::CodeCompleteObjCAtStatement(Scope *S) {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_Other);
  Results.EnterNewScope();
  AddObjCStatementResults(Results, false);
  AddObjCExpressionResults(Results, false);
  Results.ExitScope();
  HandleCodeCompleteResults(this, CodeCompleter, 
                            CodeCompletionContext::CCC_Other,
                            Results.data(),Results.size());
}

void Sema::CodeCompleteObjCAtExpression(Scope *S) {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_Other);
  Results.EnterNewScope();
  AddObjCExpressionResults(Results, false);
  Results.ExitScope();
  HandleCodeCompleteResults(this, CodeCompleter, 
                            CodeCompletionContext::CCC_Other,
                            Results.data(),Results.size());
}

/// \brief Determine whether the addition of the given flag to an Objective-C
/// property's attributes will cause a conflict.
static bool ObjCPropertyFlagConflicts(unsigned Attributes, unsigned NewFlag) {
  // Check if we've already added this flag.
  if (Attributes & NewFlag)
    return true;
  
  Attributes |= NewFlag;
  
  // Check for collisions with "readonly".
  if ((Attributes & ObjCDeclSpec::DQ_PR_readonly) &&
      (Attributes & (ObjCDeclSpec::DQ_PR_readwrite |
                     ObjCDeclSpec::DQ_PR_assign |
                     ObjCDeclSpec::DQ_PR_unsafe_unretained |
                     ObjCDeclSpec::DQ_PR_copy |
                     ObjCDeclSpec::DQ_PR_retain |
                     ObjCDeclSpec::DQ_PR_strong)))
    return true;
  
  // Check for more than one of { assign, copy, retain, strong }.
  unsigned AssignCopyRetMask = Attributes & (ObjCDeclSpec::DQ_PR_assign |
                                         ObjCDeclSpec::DQ_PR_unsafe_unretained |
                                             ObjCDeclSpec::DQ_PR_copy |
                                             ObjCDeclSpec::DQ_PR_retain|
                                             ObjCDeclSpec::DQ_PR_strong);
  if (AssignCopyRetMask &&
      AssignCopyRetMask != ObjCDeclSpec::DQ_PR_assign &&
      AssignCopyRetMask != ObjCDeclSpec::DQ_PR_unsafe_unretained &&
      AssignCopyRetMask != ObjCDeclSpec::DQ_PR_copy &&
      AssignCopyRetMask != ObjCDeclSpec::DQ_PR_retain &&
      AssignCopyRetMask != ObjCDeclSpec::DQ_PR_strong)
    return true;
  
  return false;
}

void Sema::CodeCompleteObjCPropertyFlags(Scope *S, ObjCDeclSpec &ODS) { 
  if (!CodeCompleter)
    return;
  
  unsigned Attributes = ODS.getPropertyAttributes();
  
  typedef CodeCompletionResult Result;
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_Other);
  Results.EnterNewScope();
  if (!ObjCPropertyFlagConflicts(Attributes, ObjCDeclSpec::DQ_PR_readonly))
    Results.AddResult(CodeCompletionResult("readonly"));
  if (!ObjCPropertyFlagConflicts(Attributes, ObjCDeclSpec::DQ_PR_assign))
    Results.AddResult(CodeCompletionResult("assign"));
  if (!ObjCPropertyFlagConflicts(Attributes,
                                 ObjCDeclSpec::DQ_PR_unsafe_unretained))
    Results.AddResult(CodeCompletionResult("unsafe_unretained"));
  if (!ObjCPropertyFlagConflicts(Attributes, ObjCDeclSpec::DQ_PR_readwrite))
    Results.AddResult(CodeCompletionResult("readwrite"));
  if (!ObjCPropertyFlagConflicts(Attributes, ObjCDeclSpec::DQ_PR_retain))
    Results.AddResult(CodeCompletionResult("retain"));
  if (!ObjCPropertyFlagConflicts(Attributes, ObjCDeclSpec::DQ_PR_strong))
    Results.AddResult(CodeCompletionResult("strong"));
  if (!ObjCPropertyFlagConflicts(Attributes, ObjCDeclSpec::DQ_PR_copy))
    Results.AddResult(CodeCompletionResult("copy"));
  if (!ObjCPropertyFlagConflicts(Attributes, ObjCDeclSpec::DQ_PR_nonatomic))
    Results.AddResult(CodeCompletionResult("nonatomic"));
  if (!ObjCPropertyFlagConflicts(Attributes, ObjCDeclSpec::DQ_PR_atomic))
    Results.AddResult(CodeCompletionResult("atomic"));
  if (!ObjCPropertyFlagConflicts(Attributes, ObjCDeclSpec::DQ_PR_setter)) {
    CodeCompletionBuilder Setter(Results.getAllocator());
    Setter.AddTypedTextChunk("setter");
    Setter.AddTextChunk(" = ");
    Setter.AddPlaceholderChunk("method");
    Results.AddResult(CodeCompletionResult(Setter.TakeString()));
  }
  if (!ObjCPropertyFlagConflicts(Attributes, ObjCDeclSpec::DQ_PR_getter)) {
    CodeCompletionBuilder Getter(Results.getAllocator());
    Getter.AddTypedTextChunk("getter");
    Getter.AddTextChunk(" = ");
    Getter.AddPlaceholderChunk("method");
    Results.AddResult(CodeCompletionResult(Getter.TakeString()));
  }
  Results.ExitScope();
  HandleCodeCompleteResults(this, CodeCompleter, 
                            CodeCompletionContext::CCC_Other,
                            Results.data(),Results.size());
}

/// \brief Descripts the kind of Objective-C method that we want to find
/// via code completion.
enum ObjCMethodKind {
  MK_Any, //< Any kind of method, provided it means other specified criteria.
  MK_ZeroArgSelector, //< Zero-argument (unary) selector.
  MK_OneArgSelector //< One-argument selector.
};

static bool isAcceptableObjCSelector(Selector Sel,
                                     ObjCMethodKind WantKind,
                                     IdentifierInfo **SelIdents,
                                     unsigned NumSelIdents,
                                     bool AllowSameLength = true) {
  if (NumSelIdents > Sel.getNumArgs())
    return false;
  
  switch (WantKind) {
    case MK_Any:             break;
    case MK_ZeroArgSelector: return Sel.isUnarySelector();
    case MK_OneArgSelector:  return Sel.getNumArgs() == 1;
  }
  
  if (!AllowSameLength && NumSelIdents && NumSelIdents == Sel.getNumArgs())
    return false;
  
  for (unsigned I = 0; I != NumSelIdents; ++I)
    if (SelIdents[I] != Sel.getIdentifierInfoForSlot(I))
      return false;
  
  return true;
}

static bool isAcceptableObjCMethod(ObjCMethodDecl *Method,
                                   ObjCMethodKind WantKind,
                                   IdentifierInfo **SelIdents,
                                   unsigned NumSelIdents,
                                   bool AllowSameLength = true) {
  return isAcceptableObjCSelector(Method->getSelector(), WantKind, SelIdents,
                                  NumSelIdents, AllowSameLength);
}

namespace {
  /// \brief A set of selectors, which is used to avoid introducing multiple 
  /// completions with the same selector into the result set.
  typedef llvm::SmallPtrSet<Selector, 16> VisitedSelectorSet;
}

/// \brief Add all of the Objective-C methods in the given Objective-C 
/// container to the set of results.
///
/// The container will be a class, protocol, category, or implementation of 
/// any of the above. This mether will recurse to include methods from 
/// the superclasses of classes along with their categories, protocols, and
/// implementations.
///
/// \param Container the container in which we'll look to find methods.
///
/// \param WantInstance whether to add instance methods (only); if false, this
/// routine will add factory methods (only).
///
/// \param CurContext the context in which we're performing the lookup that
/// finds methods.
///
/// \param AllowSameLength Whether we allow a method to be added to the list
/// when it has the same number of parameters as we have selector identifiers.
///
/// \param Results the structure into which we'll add results.
static void AddObjCMethods(ObjCContainerDecl *Container, 
                           bool WantInstanceMethods,
                           ObjCMethodKind WantKind,
                           IdentifierInfo **SelIdents,
                           unsigned NumSelIdents,
                           DeclContext *CurContext,
                           VisitedSelectorSet &Selectors,
                           bool AllowSameLength,
                           ResultBuilder &Results,
                           bool InOriginalClass = true) {
  typedef CodeCompletionResult Result;
  for (ObjCContainerDecl::method_iterator M = Container->meth_begin(),
                                       MEnd = Container->meth_end();
       M != MEnd; ++M) {
    if ((*M)->isInstanceMethod() == WantInstanceMethods) {
      // Check whether the selector identifiers we've been given are a 
      // subset of the identifiers for this particular method.
      if (!isAcceptableObjCMethod(*M, WantKind, SelIdents, NumSelIdents,
                                  AllowSameLength))
        continue;

      if (!Selectors.insert((*M)->getSelector()))
        continue;
      
      Result R = Result(*M, 0);
      R.StartParameter = NumSelIdents;
      R.AllParametersAreInformative = (WantKind != MK_Any);
      if (!InOriginalClass)
        R.Priority += CCD_InBaseClass;
      Results.MaybeAddResult(R, CurContext);
    }
  }
  
  // Visit the protocols of protocols.
  if (ObjCProtocolDecl *Protocol = dyn_cast<ObjCProtocolDecl>(Container)) {
    if (Protocol->hasDefinition()) {
      const ObjCList<ObjCProtocolDecl> &Protocols
        = Protocol->getReferencedProtocols();
      for (ObjCList<ObjCProtocolDecl>::iterator I = Protocols.begin(),
                                                E = Protocols.end(); 
           I != E; ++I)
        AddObjCMethods(*I, WantInstanceMethods, WantKind, SelIdents, 
                       NumSelIdents, CurContext, Selectors, AllowSameLength, 
                       Results, false);
    }
  }
  
  ObjCInterfaceDecl *IFace = dyn_cast<ObjCInterfaceDecl>(Container);
  if (!IFace || !IFace->hasDefinition())
    return;
  
  // Add methods in protocols.
  const ObjCList<ObjCProtocolDecl> &Protocols= IFace->getReferencedProtocols();
  for (ObjCList<ObjCProtocolDecl>::iterator I = Protocols.begin(),
                                            E = Protocols.end(); 
       I != E; ++I)
    AddObjCMethods(*I, WantInstanceMethods, WantKind, SelIdents, NumSelIdents, 
                   CurContext, Selectors, AllowSameLength, Results, false);
  
  // Add methods in categories.
  for (ObjCCategoryDecl *CatDecl = IFace->getCategoryList(); CatDecl;
       CatDecl = CatDecl->getNextClassCategory()) {
    AddObjCMethods(CatDecl, WantInstanceMethods, WantKind, SelIdents, 
                   NumSelIdents, CurContext, Selectors, AllowSameLength, 
                   Results, InOriginalClass);
    
    // Add a categories protocol methods.
    const ObjCList<ObjCProtocolDecl> &Protocols 
      = CatDecl->getReferencedProtocols();
    for (ObjCList<ObjCProtocolDecl>::iterator I = Protocols.begin(),
                                              E = Protocols.end();
         I != E; ++I)
      AddObjCMethods(*I, WantInstanceMethods, WantKind, SelIdents, 
                     NumSelIdents, CurContext, Selectors, AllowSameLength, 
                     Results, false);
    
    // Add methods in category implementations.
    if (ObjCCategoryImplDecl *Impl = CatDecl->getImplementation())
      AddObjCMethods(Impl, WantInstanceMethods, WantKind, SelIdents, 
                     NumSelIdents, CurContext, Selectors, AllowSameLength, 
                     Results, InOriginalClass);
  }
  
  // Add methods in superclass.
  if (IFace->getSuperClass())
    AddObjCMethods(IFace->getSuperClass(), WantInstanceMethods, WantKind, 
                   SelIdents, NumSelIdents, CurContext, Selectors, 
                   AllowSameLength, Results, false);

  // Add methods in our implementation, if any.
  if (ObjCImplementationDecl *Impl = IFace->getImplementation())
    AddObjCMethods(Impl, WantInstanceMethods, WantKind, SelIdents,
                   NumSelIdents, CurContext, Selectors, AllowSameLength, 
                   Results, InOriginalClass);
}


void Sema::CodeCompleteObjCPropertyGetter(Scope *S) {
  typedef CodeCompletionResult Result;

  // Try to find the interface where getters might live.
  ObjCInterfaceDecl *Class = dyn_cast_or_null<ObjCInterfaceDecl>(CurContext);
  if (!Class) {
    if (ObjCCategoryDecl *Category
          = dyn_cast_or_null<ObjCCategoryDecl>(CurContext))
      Class = Category->getClassInterface();

    if (!Class)
      return;
  }

  // Find all of the potential getters.
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_Other);
  Results.EnterNewScope();

  VisitedSelectorSet Selectors;
  AddObjCMethods(Class, true, MK_ZeroArgSelector, 0, 0, CurContext, Selectors,
                 /*AllowSameLength=*/true, Results);
  Results.ExitScope();
  HandleCodeCompleteResults(this, CodeCompleter,
                            CodeCompletionContext::CCC_Other,
                            Results.data(),Results.size());
}

void Sema::CodeCompleteObjCPropertySetter(Scope *S) {
  typedef CodeCompletionResult Result;

  // Try to find the interface where setters might live.
  ObjCInterfaceDecl *Class
    = dyn_cast_or_null<ObjCInterfaceDecl>(CurContext);
  if (!Class) {
    if (ObjCCategoryDecl *Category
          = dyn_cast_or_null<ObjCCategoryDecl>(CurContext))
      Class = Category->getClassInterface();

    if (!Class)
      return;
  }

  // Find all of the potential getters.
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_Other);
  Results.EnterNewScope();

  VisitedSelectorSet Selectors;
  AddObjCMethods(Class, true, MK_OneArgSelector, 0, 0, CurContext, 
                 Selectors, /*AllowSameLength=*/true, Results);

  Results.ExitScope();
  HandleCodeCompleteResults(this, CodeCompleter,
                            CodeCompletionContext::CCC_Other,
                            Results.data(),Results.size());
}

void Sema::CodeCompleteObjCPassingType(Scope *S, ObjCDeclSpec &DS,
                                       bool IsParameter) {
  typedef CodeCompletionResult Result;
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_Type);
  Results.EnterNewScope();
  
  // Add context-sensitive, Objective-C parameter-passing keywords.
  bool AddedInOut = false;
  if ((DS.getObjCDeclQualifier() & 
       (ObjCDeclSpec::DQ_In | ObjCDeclSpec::DQ_Inout)) == 0) {
    Results.AddResult("in");
    Results.AddResult("inout");
    AddedInOut = true;
  }
  if ((DS.getObjCDeclQualifier() & 
       (ObjCDeclSpec::DQ_Out | ObjCDeclSpec::DQ_Inout)) == 0) {
    Results.AddResult("out");
    if (!AddedInOut)
      Results.AddResult("inout");
  }
  if ((DS.getObjCDeclQualifier() & 
       (ObjCDeclSpec::DQ_Bycopy | ObjCDeclSpec::DQ_Byref |
        ObjCDeclSpec::DQ_Oneway)) == 0) {
     Results.AddResult("bycopy");
     Results.AddResult("byref");
     Results.AddResult("oneway");
  }
  
  // If we're completing the return type of an Objective-C method and the 
  // identifier IBAction refers to a macro, provide a completion item for
  // an action, e.g.,
  //   IBAction)<#selector#>:(id)sender
  if (DS.getObjCDeclQualifier() == 0 && !IsParameter &&
      Context.Idents.get("IBAction").hasMacroDefinition()) {
    typedef CodeCompletionString::Chunk Chunk;
    CodeCompletionBuilder Builder(Results.getAllocator(), CCP_CodePattern, 
                                  CXAvailability_Available);
    Builder.AddTypedTextChunk("IBAction");
    Builder.AddChunk(Chunk(CodeCompletionString::CK_RightParen));
    Builder.AddPlaceholderChunk("selector");
    Builder.AddChunk(Chunk(CodeCompletionString::CK_Colon));
    Builder.AddChunk(Chunk(CodeCompletionString::CK_LeftParen));
    Builder.AddTextChunk("id");
    Builder.AddChunk(Chunk(CodeCompletionString::CK_RightParen));
    Builder.AddTextChunk("sender");
    Results.AddResult(CodeCompletionResult(Builder.TakeString()));
  }
  
  // Add various builtin type names and specifiers.
  AddOrdinaryNameResults(PCC_Type, S, *this, Results);
  Results.ExitScope();
  
  // Add the various type names
  Results.setFilter(&ResultBuilder::IsOrdinaryNonValueName);
  CodeCompletionDeclConsumer Consumer(Results, CurContext);
  LookupVisibleDecls(S, LookupOrdinaryName, Consumer,
                     CodeCompleter->includeGlobals());
  
  if (CodeCompleter->includeMacros())
    AddMacroResults(PP, Results);

  HandleCodeCompleteResults(this, CodeCompleter,
                            CodeCompletionContext::CCC_Type,
                            Results.data(), Results.size());
}

/// \brief When we have an expression with type "id", we may assume
/// that it has some more-specific class type based on knowledge of
/// common uses of Objective-C. This routine returns that class type,
/// or NULL if no better result could be determined.
static ObjCInterfaceDecl *GetAssumedMessageSendExprType(Expr *E) {
  ObjCMessageExpr *Msg = dyn_cast_or_null<ObjCMessageExpr>(E);
  if (!Msg)
    return 0;

  Selector Sel = Msg->getSelector();
  if (Sel.isNull())
    return 0;

  IdentifierInfo *Id = Sel.getIdentifierInfoForSlot(0);
  if (!Id)
    return 0;

  ObjCMethodDecl *Method = Msg->getMethodDecl();
  if (!Method)
    return 0;

  // Determine the class that we're sending the message to.
  ObjCInterfaceDecl *IFace = 0;
  switch (Msg->getReceiverKind()) {
  case ObjCMessageExpr::Class:
    if (const ObjCObjectType *ObjType
                           = Msg->getClassReceiver()->getAs<ObjCObjectType>())
      IFace = ObjType->getInterface();
    break;

  case ObjCMessageExpr::Instance: {
    QualType T = Msg->getInstanceReceiver()->getType();
    if (const ObjCObjectPointerType *Ptr = T->getAs<ObjCObjectPointerType>())
      IFace = Ptr->getInterfaceDecl();
    break;
  }

  case ObjCMessageExpr::SuperInstance:
  case ObjCMessageExpr::SuperClass:
    break;
  }

  if (!IFace)
    return 0;

  ObjCInterfaceDecl *Super = IFace->getSuperClass();
  if (Method->isInstanceMethod())
    return llvm::StringSwitch<ObjCInterfaceDecl *>(Id->getName())
      .Case("retain", IFace)
      .Case("strong", IFace)
      .Case("autorelease", IFace)
      .Case("copy", IFace)
      .Case("copyWithZone", IFace)
      .Case("mutableCopy", IFace)
      .Case("mutableCopyWithZone", IFace)
      .Case("awakeFromCoder", IFace)
      .Case("replacementObjectFromCoder", IFace)
      .Case("class", IFace)
      .Case("classForCoder", IFace)
      .Case("superclass", Super)
      .Default(0);

  return llvm::StringSwitch<ObjCInterfaceDecl *>(Id->getName())
    .Case("new", IFace)
    .Case("alloc", IFace)
    .Case("allocWithZone", IFace)
    .Case("class", IFace)
    .Case("superclass", Super)
    .Default(0);
}

// Add a special completion for a message send to "super", which fills in the
// most likely case of forwarding all of our arguments to the superclass 
// function.
///
/// \param S The semantic analysis object.
///
/// \param S NeedSuperKeyword Whether we need to prefix this completion with
/// the "super" keyword. Otherwise, we just need to provide the arguments.
///
/// \param SelIdents The identifiers in the selector that have already been
/// provided as arguments for a send to "super".
///
/// \param NumSelIdents The number of identifiers in \p SelIdents.
///
/// \param Results The set of results to augment.
///
/// \returns the Objective-C method declaration that would be invoked by 
/// this "super" completion. If NULL, no completion was added.
static ObjCMethodDecl *AddSuperSendCompletion(Sema &S, bool NeedSuperKeyword,
                                              IdentifierInfo **SelIdents,
                                              unsigned NumSelIdents,
                                              ResultBuilder &Results) {
  ObjCMethodDecl *CurMethod = S.getCurMethodDecl();
  if (!CurMethod)
    return 0;
  
  ObjCInterfaceDecl *Class = CurMethod->getClassInterface();
  if (!Class)
    return 0;
  
  // Try to find a superclass method with the same selector.
  ObjCMethodDecl *SuperMethod = 0;
  while ((Class = Class->getSuperClass()) && !SuperMethod) {
    // Check in the class
    SuperMethod = Class->getMethod(CurMethod->getSelector(), 
                                   CurMethod->isInstanceMethod());

    // Check in categories or class extensions.
    if (!SuperMethod) {
      for (ObjCCategoryDecl *Category = Class->getCategoryList(); Category;
           Category = Category->getNextClassCategory())
        if ((SuperMethod = Category->getMethod(CurMethod->getSelector(), 
                                               CurMethod->isInstanceMethod())))
          break;
    }
  }

  if (!SuperMethod)
    return 0;
  
  // Check whether the superclass method has the same signature.
  if (CurMethod->param_size() != SuperMethod->param_size() ||
      CurMethod->isVariadic() != SuperMethod->isVariadic())
    return 0;
      
  for (ObjCMethodDecl::param_iterator CurP = CurMethod->param_begin(),
                                   CurPEnd = CurMethod->param_end(),
                                    SuperP = SuperMethod->param_begin();
       CurP != CurPEnd; ++CurP, ++SuperP) {
    // Make sure the parameter types are compatible.
    if (!S.Context.hasSameUnqualifiedType((*CurP)->getType(), 
                                          (*SuperP)->getType()))
      return 0;
    
    // Make sure we have a parameter name to forward!
    if (!(*CurP)->getIdentifier())
      return 0;
  }
  
  // We have a superclass method. Now, form the send-to-super completion.
  CodeCompletionBuilder Builder(Results.getAllocator());
  
  // Give this completion a return type.
  AddResultTypeChunk(S.Context, getCompletionPrintingPolicy(S), SuperMethod, 
                     Builder);

  // If we need the "super" keyword, add it (plus some spacing).
  if (NeedSuperKeyword) {
    Builder.AddTypedTextChunk("super");
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  }
  
  Selector Sel = CurMethod->getSelector();
  if (Sel.isUnarySelector()) {
    if (NeedSuperKeyword)
      Builder.AddTextChunk(Builder.getAllocator().CopyString(
                                  Sel.getNameForSlot(0)));
    else
      Builder.AddTypedTextChunk(Builder.getAllocator().CopyString(
                                   Sel.getNameForSlot(0)));
  } else {
    ObjCMethodDecl::param_iterator CurP = CurMethod->param_begin();
    for (unsigned I = 0, N = Sel.getNumArgs(); I != N; ++I, ++CurP) {
      if (I > NumSelIdents)
        Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      
      if (I < NumSelIdents)
        Builder.AddInformativeChunk(
                   Builder.getAllocator().CopyString(
                                                 Sel.getNameForSlot(I) + ":"));
      else if (NeedSuperKeyword || I > NumSelIdents) {
        Builder.AddTextChunk(
                 Builder.getAllocator().CopyString(
                                                  Sel.getNameForSlot(I) + ":"));
        Builder.AddPlaceholderChunk(Builder.getAllocator().CopyString(
                                         (*CurP)->getIdentifier()->getName()));
      } else {
        Builder.AddTypedTextChunk(
                  Builder.getAllocator().CopyString(
                                                  Sel.getNameForSlot(I) + ":"));
        Builder.AddPlaceholderChunk(Builder.getAllocator().CopyString(
                                         (*CurP)->getIdentifier()->getName())); 
      }
    }
  }
  
  Results.AddResult(CodeCompletionResult(Builder.TakeString(), CCP_SuperCompletion,
                                         SuperMethod->isInstanceMethod()
                                           ? CXCursor_ObjCInstanceMethodDecl
                                           : CXCursor_ObjCClassMethodDecl));
  return SuperMethod;
}
                                   
void Sema::CodeCompleteObjCMessageReceiver(Scope *S) {
  typedef CodeCompletionResult Result;
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_ObjCMessageReceiver,
                        &ResultBuilder::IsObjCMessageReceiver);
  
  CodeCompletionDeclConsumer Consumer(Results, CurContext);
  Results.EnterNewScope();
  LookupVisibleDecls(S, LookupOrdinaryName, Consumer,
                     CodeCompleter->includeGlobals());
  
  // If we are in an Objective-C method inside a class that has a superclass,
  // add "super" as an option.
  if (ObjCMethodDecl *Method = getCurMethodDecl())
    if (ObjCInterfaceDecl *Iface = Method->getClassInterface())
      if (Iface->getSuperClass()) {
        Results.AddResult(Result("super"));
        
        AddSuperSendCompletion(*this, /*NeedSuperKeyword=*/true, 0, 0, Results);
      }
  
  Results.ExitScope();
  
  if (CodeCompleter->includeMacros())
    AddMacroResults(PP, Results);
  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
  
}

void Sema::CodeCompleteObjCSuperMessage(Scope *S, SourceLocation SuperLoc,
                                        IdentifierInfo **SelIdents,
                                        unsigned NumSelIdents,
                                        bool AtArgumentExpression) {
  ObjCInterfaceDecl *CDecl = 0;
  if (ObjCMethodDecl *CurMethod = getCurMethodDecl()) {
    // Figure out which interface we're in.
    CDecl = CurMethod->getClassInterface();
    if (!CDecl)
      return;
    
    // Find the superclass of this class.
    CDecl = CDecl->getSuperClass();
    if (!CDecl)
      return;

    if (CurMethod->isInstanceMethod()) {
      // We are inside an instance method, which means that the message
      // send [super ...] is actually calling an instance method on the
      // current object.
      return CodeCompleteObjCInstanceMessage(S, 0,
                                             SelIdents, NumSelIdents,
                                             AtArgumentExpression,
                                             CDecl);
    }

    // Fall through to send to the superclass in CDecl.
  } else {
    // "super" may be the name of a type or variable. Figure out which
    // it is.
    IdentifierInfo *Super = &Context.Idents.get("super");
    NamedDecl *ND = LookupSingleName(S, Super, SuperLoc, 
                                     LookupOrdinaryName);
    if ((CDecl = dyn_cast_or_null<ObjCInterfaceDecl>(ND))) {
      // "super" names an interface. Use it.
    } else if (TypeDecl *TD = dyn_cast_or_null<TypeDecl>(ND)) {
      if (const ObjCObjectType *Iface
            = Context.getTypeDeclType(TD)->getAs<ObjCObjectType>())
        CDecl = Iface->getInterface();
    } else if (ND && isa<UnresolvedUsingTypenameDecl>(ND)) {
      // "super" names an unresolved type; we can't be more specific.
    } else {
      // Assume that "super" names some kind of value and parse that way.
      CXXScopeSpec SS;
      UnqualifiedId id;
      id.setIdentifier(Super, SuperLoc);
      ExprResult SuperExpr = ActOnIdExpression(S, SS, id, false, false);
      return CodeCompleteObjCInstanceMessage(S, (Expr *)SuperExpr.get(),
                                             SelIdents, NumSelIdents,
                                             AtArgumentExpression);
    }

    // Fall through
  }

  ParsedType Receiver;
  if (CDecl)
    Receiver = ParsedType::make(Context.getObjCInterfaceType(CDecl));
  return CodeCompleteObjCClassMessage(S, Receiver, SelIdents, 
                                      NumSelIdents, AtArgumentExpression,
                                      /*IsSuper=*/true);
}

/// \brief Given a set of code-completion results for the argument of a message
/// send, determine the preferred type (if any) for that argument expression.
static QualType getPreferredArgumentTypeForMessageSend(ResultBuilder &Results,
                                                       unsigned NumSelIdents) {
  typedef CodeCompletionResult Result;  
  ASTContext &Context = Results.getSema().Context;
  
  QualType PreferredType;
  unsigned BestPriority = CCP_Unlikely * 2;
  Result *ResultsData = Results.data();
  for (unsigned I = 0, N = Results.size(); I != N; ++I) {
    Result &R = ResultsData[I];
    if (R.Kind == Result::RK_Declaration && 
        isa<ObjCMethodDecl>(R.Declaration)) {
      if (R.Priority <= BestPriority) {
        ObjCMethodDecl *Method = cast<ObjCMethodDecl>(R.Declaration);
        if (NumSelIdents <= Method->param_size()) {
          QualType MyPreferredType = Method->param_begin()[NumSelIdents - 1]
                                       ->getType();
          if (R.Priority < BestPriority || PreferredType.isNull()) {
            BestPriority = R.Priority;
            PreferredType = MyPreferredType;
          } else if (!Context.hasSameUnqualifiedType(PreferredType,
                                                     MyPreferredType)) {
            PreferredType = QualType();
          }
        }
      }
    }
  }

  return PreferredType;
}

static void AddClassMessageCompletions(Sema &SemaRef, Scope *S, 
                                       ParsedType Receiver,
                                       IdentifierInfo **SelIdents,
                                       unsigned NumSelIdents,
                                       bool AtArgumentExpression,
                                       bool IsSuper,
                                       ResultBuilder &Results) {
  typedef CodeCompletionResult Result;
  ObjCInterfaceDecl *CDecl = 0;
  
  // If the given name refers to an interface type, retrieve the
  // corresponding declaration.
  if (Receiver) {
    QualType T = SemaRef.GetTypeFromParser(Receiver, 0);
    if (!T.isNull()) 
      if (const ObjCObjectType *Interface = T->getAs<ObjCObjectType>())
        CDecl = Interface->getInterface();
  }
  
  // Add all of the factory methods in this Objective-C class, its protocols,
  // superclasses, categories, implementation, etc.
  Results.EnterNewScope();
  
  // If this is a send-to-super, try to add the special "super" send 
  // completion.
  if (IsSuper) {
    if (ObjCMethodDecl *SuperMethod
        = AddSuperSendCompletion(SemaRef, false, SelIdents, NumSelIdents, 
                                 Results))
      Results.Ignore(SuperMethod);
  }
  
  // If we're inside an Objective-C method definition, prefer its selector to
  // others.
  if (ObjCMethodDecl *CurMethod = SemaRef.getCurMethodDecl())
    Results.setPreferredSelector(CurMethod->getSelector());
  
  VisitedSelectorSet Selectors;
  if (CDecl) 
    AddObjCMethods(CDecl, false, MK_Any, SelIdents, NumSelIdents, 
                   SemaRef.CurContext, Selectors, AtArgumentExpression,
                   Results);  
  else {
    // We're messaging "id" as a type; provide all class/factory methods.
    
    // If we have an external source, load the entire class method
    // pool from the AST file.
    if (SemaRef.ExternalSource) {
      for (uint32_t I = 0, 
                    N = SemaRef.ExternalSource->GetNumExternalSelectors();
           I != N; ++I) {
        Selector Sel = SemaRef.ExternalSource->GetExternalSelector(I);
        if (Sel.isNull() || SemaRef.MethodPool.count(Sel))
          continue;
        
        SemaRef.ReadMethodPool(Sel);
      }
    }
    
    for (Sema::GlobalMethodPool::iterator M = SemaRef.MethodPool.begin(),
                                       MEnd = SemaRef.MethodPool.end();
         M != MEnd; ++M) {
      for (ObjCMethodList *MethList = &M->second.second;
           MethList && MethList->Method; 
           MethList = MethList->Next) {
        if (!isAcceptableObjCMethod(MethList->Method, MK_Any, SelIdents, 
                                    NumSelIdents))
          continue;
        
        Result R(MethList->Method, 0);
        R.StartParameter = NumSelIdents;
        R.AllParametersAreInformative = false;
        Results.MaybeAddResult(R, SemaRef.CurContext);
      }
    }
  }
  
  Results.ExitScope();  
}

void Sema::CodeCompleteObjCClassMessage(Scope *S, ParsedType Receiver,
                                        IdentifierInfo **SelIdents,
                                        unsigned NumSelIdents,
                                        bool AtArgumentExpression,
                                        bool IsSuper) {
  
  QualType T = this->GetTypeFromParser(Receiver);
  
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
              CodeCompletionContext(CodeCompletionContext::CCC_ObjCClassMessage,
                                    T, SelIdents, NumSelIdents));
    
  AddClassMessageCompletions(*this, S, Receiver, SelIdents, NumSelIdents, 
                             AtArgumentExpression, IsSuper, Results);
  
  // If we're actually at the argument expression (rather than prior to the 
  // selector), we're actually performing code completion for an expression.
  // Determine whether we have a single, best method. If so, we can 
  // code-complete the expression using the corresponding parameter type as
  // our preferred type, improving completion results.
  if (AtArgumentExpression) {
    QualType PreferredType = getPreferredArgumentTypeForMessageSend(Results, 
                                                                  NumSelIdents);
    if (PreferredType.isNull())
      CodeCompleteOrdinaryName(S, PCC_Expression);
    else
      CodeCompleteExpression(S, PreferredType);
    return;
  }

  HandleCodeCompleteResults(this, CodeCompleter, 
                            Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteObjCInstanceMessage(Scope *S, Expr *Receiver,
                                           IdentifierInfo **SelIdents,
                                           unsigned NumSelIdents,
                                           bool AtArgumentExpression,
                                           ObjCInterfaceDecl *Super) {
  typedef CodeCompletionResult Result;
  
  Expr *RecExpr = static_cast<Expr *>(Receiver);
  
  // If necessary, apply function/array conversion to the receiver.
  // C99 6.7.5.3p[7,8].
  if (RecExpr) {
    ExprResult Conv = DefaultFunctionArrayLvalueConversion(RecExpr);
    if (Conv.isInvalid()) // conversion failed. bail.
      return;
    RecExpr = Conv.take();
  }
  QualType ReceiverType = RecExpr? RecExpr->getType() 
                          : Super? Context.getObjCObjectPointerType(
                                            Context.getObjCInterfaceType(Super))
                                 : Context.getObjCIdType();
  
  // If we're messaging an expression with type "id" or "Class", check
  // whether we know something special about the receiver that allows
  // us to assume a more-specific receiver type.
  if (ReceiverType->isObjCIdType() || ReceiverType->isObjCClassType())
    if (ObjCInterfaceDecl *IFace = GetAssumedMessageSendExprType(RecExpr)) {
      if (ReceiverType->isObjCClassType())
        return CodeCompleteObjCClassMessage(S, 
                       ParsedType::make(Context.getObjCInterfaceType(IFace)),
                                            SelIdents, NumSelIdents,
                                            AtArgumentExpression, Super);

      ReceiverType = Context.getObjCObjectPointerType(
                                          Context.getObjCInterfaceType(IFace));
    }

  // Build the set of methods we can see.
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
           CodeCompletionContext(CodeCompletionContext::CCC_ObjCInstanceMessage,
                                 ReceiverType, SelIdents, NumSelIdents));
  
  Results.EnterNewScope();

  // If this is a send-to-super, try to add the special "super" send 
  // completion.
  if (Super) {
    if (ObjCMethodDecl *SuperMethod
          = AddSuperSendCompletion(*this, false, SelIdents, NumSelIdents, 
                                   Results))
      Results.Ignore(SuperMethod);
  }
  
  // If we're inside an Objective-C method definition, prefer its selector to
  // others.
  if (ObjCMethodDecl *CurMethod = getCurMethodDecl())
    Results.setPreferredSelector(CurMethod->getSelector());
  
  // Keep track of the selectors we've already added.
  VisitedSelectorSet Selectors;
  
  // Handle messages to Class. This really isn't a message to an instance
  // method, so we treat it the same way we would treat a message send to a
  // class method.
  if (ReceiverType->isObjCClassType() || 
      ReceiverType->isObjCQualifiedClassType()) {
    if (ObjCMethodDecl *CurMethod = getCurMethodDecl()) {
      if (ObjCInterfaceDecl *ClassDecl = CurMethod->getClassInterface())
        AddObjCMethods(ClassDecl, false, MK_Any, SelIdents, NumSelIdents, 
                       CurContext, Selectors, AtArgumentExpression, Results);
    }
  } 
  // Handle messages to a qualified ID ("id<foo>").
  else if (const ObjCObjectPointerType *QualID
             = ReceiverType->getAsObjCQualifiedIdType()) {
    // Search protocols for instance methods.
    for (ObjCObjectPointerType::qual_iterator I = QualID->qual_begin(),
                                              E = QualID->qual_end(); 
         I != E; ++I)
      AddObjCMethods(*I, true, MK_Any, SelIdents, NumSelIdents, CurContext, 
                     Selectors, AtArgumentExpression, Results);
  }
  // Handle messages to a pointer to interface type.
  else if (const ObjCObjectPointerType *IFacePtr
                              = ReceiverType->getAsObjCInterfacePointerType()) {
    // Search the class, its superclasses, etc., for instance methods.
    AddObjCMethods(IFacePtr->getInterfaceDecl(), true, MK_Any, SelIdents,
                   NumSelIdents, CurContext, Selectors, AtArgumentExpression, 
                   Results);
    
    // Search protocols for instance methods.
    for (ObjCObjectPointerType::qual_iterator I = IFacePtr->qual_begin(),
         E = IFacePtr->qual_end(); 
         I != E; ++I)
      AddObjCMethods(*I, true, MK_Any, SelIdents, NumSelIdents, CurContext, 
                     Selectors, AtArgumentExpression, Results);
  }
  // Handle messages to "id".
  else if (ReceiverType->isObjCIdType()) {
    // We're messaging "id", so provide all instance methods we know
    // about as code-completion results.

    // If we have an external source, load the entire class method
    // pool from the AST file.
    if (ExternalSource) {
      for (uint32_t I = 0, N = ExternalSource->GetNumExternalSelectors();
           I != N; ++I) {
        Selector Sel = ExternalSource->GetExternalSelector(I);
        if (Sel.isNull() || MethodPool.count(Sel))
          continue;

        ReadMethodPool(Sel);
      }
    }

    for (GlobalMethodPool::iterator M = MethodPool.begin(),
                                    MEnd = MethodPool.end();
         M != MEnd; ++M) {
      for (ObjCMethodList *MethList = &M->second.first;
           MethList && MethList->Method; 
           MethList = MethList->Next) {
        if (!isAcceptableObjCMethod(MethList->Method, MK_Any, SelIdents, 
                                    NumSelIdents))
          continue;
        
        if (!Selectors.insert(MethList->Method->getSelector()))
          continue;
        
        Result R(MethList->Method, 0);
        R.StartParameter = NumSelIdents;
        R.AllParametersAreInformative = false;
        Results.MaybeAddResult(R, CurContext);
      }
    }
  }
  Results.ExitScope();
  
  
  // If we're actually at the argument expression (rather than prior to the 
  // selector), we're actually performing code completion for an expression.
  // Determine whether we have a single, best method. If so, we can 
  // code-complete the expression using the corresponding parameter type as
  // our preferred type, improving completion results.
  if (AtArgumentExpression) {
    QualType PreferredType = getPreferredArgumentTypeForMessageSend(Results, 
                                                                  NumSelIdents);
    if (PreferredType.isNull())
      CodeCompleteOrdinaryName(S, PCC_Expression);
    else
      CodeCompleteExpression(S, PreferredType);
    return;
  }
  
  HandleCodeCompleteResults(this, CodeCompleter, 
                            Results.getCompletionContext(),
                            Results.data(),Results.size());
}

void Sema::CodeCompleteObjCForCollection(Scope *S, 
                                         DeclGroupPtrTy IterationVar) {
  CodeCompleteExpressionData Data;
  Data.ObjCCollection = true;
  
  if (IterationVar.getAsOpaquePtr()) {
    DeclGroupRef DG = IterationVar.getAsVal<DeclGroupRef>();
    for (DeclGroupRef::iterator I = DG.begin(), End = DG.end(); I != End; ++I) {
      if (*I)
        Data.IgnoreDecls.push_back(*I);
    }
  }
  
  CodeCompleteExpression(S, Data);
}

void Sema::CodeCompleteObjCSelector(Scope *S, IdentifierInfo **SelIdents,
                                    unsigned NumSelIdents) {
  // If we have an external source, load the entire class method
  // pool from the AST file.
  if (ExternalSource) {
    for (uint32_t I = 0, N = ExternalSource->GetNumExternalSelectors();
         I != N; ++I) {
      Selector Sel = ExternalSource->GetExternalSelector(I);
      if (Sel.isNull() || MethodPool.count(Sel))
        continue;
      
      ReadMethodPool(Sel);
    }
  }
  
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_SelectorName);
  Results.EnterNewScope();
  for (GlobalMethodPool::iterator M = MethodPool.begin(),
                               MEnd = MethodPool.end();
       M != MEnd; ++M) {
    
    Selector Sel = M->first;
    if (!isAcceptableObjCSelector(Sel, MK_Any, SelIdents, NumSelIdents))
      continue;

    CodeCompletionBuilder Builder(Results.getAllocator());
    if (Sel.isUnarySelector()) {
      Builder.AddTypedTextChunk(Builder.getAllocator().CopyString(
                                                       Sel.getNameForSlot(0)));
      Results.AddResult(Builder.TakeString());
      continue;
    }
    
    std::string Accumulator;
    for (unsigned I = 0, N = Sel.getNumArgs(); I != N; ++I) {
      if (I == NumSelIdents) {
        if (!Accumulator.empty()) {
          Builder.AddInformativeChunk(Builder.getAllocator().CopyString(
                                                 Accumulator));
          Accumulator.clear();
        }
      }
      
      Accumulator += Sel.getNameForSlot(I);
      Accumulator += ':';
    }
    Builder.AddTypedTextChunk(Builder.getAllocator().CopyString( Accumulator));
    Results.AddResult(Builder.TakeString());
  }
  Results.ExitScope();
  
  HandleCodeCompleteResults(this, CodeCompleter, 
                            CodeCompletionContext::CCC_SelectorName,
                            Results.data(), Results.size());
}

/// \brief Add all of the protocol declarations that we find in the given
/// (translation unit) context.
static void AddProtocolResults(DeclContext *Ctx, DeclContext *CurContext,
                               bool OnlyForwardDeclarations,
                               ResultBuilder &Results) {
  typedef CodeCompletionResult Result;
  
  for (DeclContext::decl_iterator D = Ctx->decls_begin(), 
                               DEnd = Ctx->decls_end();
       D != DEnd; ++D) {
    // Record any protocols we find.
    if (ObjCProtocolDecl *Proto = dyn_cast<ObjCProtocolDecl>(*D))
      if (!OnlyForwardDeclarations || !Proto->hasDefinition())
        Results.AddResult(Result(Proto, 0), CurContext, 0, false);
  }
}

void Sema::CodeCompleteObjCProtocolReferences(IdentifierLocPair *Protocols,
                                              unsigned NumProtocols) {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_ObjCProtocolName);
  
  if (CodeCompleter && CodeCompleter->includeGlobals()) {
    Results.EnterNewScope();
    
    // Tell the result set to ignore all of the protocols we have
    // already seen.
    // FIXME: This doesn't work when caching code-completion results.
    for (unsigned I = 0; I != NumProtocols; ++I)
      if (ObjCProtocolDecl *Protocol = LookupProtocol(Protocols[I].first,
                                                      Protocols[I].second))
        Results.Ignore(Protocol);

    // Add all protocols.
    AddProtocolResults(Context.getTranslationUnitDecl(), CurContext, false,
                       Results);

    Results.ExitScope();
  }
  
  HandleCodeCompleteResults(this, CodeCompleter, 
                            CodeCompletionContext::CCC_ObjCProtocolName,
                            Results.data(),Results.size());
}

void Sema::CodeCompleteObjCProtocolDecl(Scope *) {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_ObjCProtocolName);
  
  if (CodeCompleter && CodeCompleter->includeGlobals()) {
    Results.EnterNewScope();
    
    // Add all protocols.
    AddProtocolResults(Context.getTranslationUnitDecl(), CurContext, true,
                       Results);

    Results.ExitScope();
  }
  
  HandleCodeCompleteResults(this, CodeCompleter, 
                            CodeCompletionContext::CCC_ObjCProtocolName,
                            Results.data(),Results.size());
}

/// \brief Add all of the Objective-C interface declarations that we find in
/// the given (translation unit) context.
static void AddInterfaceResults(DeclContext *Ctx, DeclContext *CurContext,
                                bool OnlyForwardDeclarations,
                                bool OnlyUnimplemented,
                                ResultBuilder &Results) {
  typedef CodeCompletionResult Result;
  
  for (DeclContext::decl_iterator D = Ctx->decls_begin(), 
                               DEnd = Ctx->decls_end();
       D != DEnd; ++D) {
    // Record any interfaces we find.
    if (ObjCInterfaceDecl *Class = dyn_cast<ObjCInterfaceDecl>(*D))
      if ((!OnlyForwardDeclarations || !Class->hasDefinition()) &&
          (!OnlyUnimplemented || !Class->getImplementation()))
        Results.AddResult(Result(Class, 0), CurContext, 0, false);
  }
}

void Sema::CodeCompleteObjCInterfaceDecl(Scope *S) { 
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_Other);
  Results.EnterNewScope();
  
  if (CodeCompleter->includeGlobals()) {
    // Add all classes.
    AddInterfaceResults(Context.getTranslationUnitDecl(), CurContext, false,
                        false, Results);
  }
  
  Results.ExitScope();

  HandleCodeCompleteResults(this, CodeCompleter,
                            CodeCompletionContext::CCC_ObjCInterfaceName,
                            Results.data(),Results.size());
}

void Sema::CodeCompleteObjCSuperclass(Scope *S, IdentifierInfo *ClassName,
                                      SourceLocation ClassNameLoc) { 
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_ObjCInterfaceName);
  Results.EnterNewScope();
  
  // Make sure that we ignore the class we're currently defining.
  NamedDecl *CurClass
    = LookupSingleName(TUScope, ClassName, ClassNameLoc, LookupOrdinaryName);
  if (CurClass && isa<ObjCInterfaceDecl>(CurClass))
    Results.Ignore(CurClass);

  if (CodeCompleter->includeGlobals()) {
    // Add all classes.
    AddInterfaceResults(Context.getTranslationUnitDecl(), CurContext, false,
                        false, Results);
  }
  
  Results.ExitScope();

  HandleCodeCompleteResults(this, CodeCompleter, 
                            CodeCompletionContext::CCC_ObjCInterfaceName,
                            Results.data(),Results.size());
}

void Sema::CodeCompleteObjCImplementationDecl(Scope *S) { 
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_Other);
  Results.EnterNewScope();

  if (CodeCompleter->includeGlobals()) {
    // Add all unimplemented classes.
    AddInterfaceResults(Context.getTranslationUnitDecl(), CurContext, false,
                        true, Results);
  }
  
  Results.ExitScope();

  HandleCodeCompleteResults(this, CodeCompleter, 
                            CodeCompletionContext::CCC_ObjCInterfaceName,
                            Results.data(),Results.size());
}

void Sema::CodeCompleteObjCInterfaceCategory(Scope *S, 
                                             IdentifierInfo *ClassName,
                                             SourceLocation ClassNameLoc) {
  typedef CodeCompletionResult Result;
  
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_ObjCCategoryName);
  
  // Ignore any categories we find that have already been implemented by this
  // interface.
  llvm::SmallPtrSet<IdentifierInfo *, 16> CategoryNames;
  NamedDecl *CurClass
    = LookupSingleName(TUScope, ClassName, ClassNameLoc, LookupOrdinaryName);
  if (ObjCInterfaceDecl *Class = dyn_cast_or_null<ObjCInterfaceDecl>(CurClass))
    for (ObjCCategoryDecl *Category = Class->getCategoryList(); Category;
         Category = Category->getNextClassCategory())
      CategoryNames.insert(Category->getIdentifier());
  
  // Add all of the categories we know about.
  Results.EnterNewScope();
  TranslationUnitDecl *TU = Context.getTranslationUnitDecl();
  for (DeclContext::decl_iterator D = TU->decls_begin(), 
                               DEnd = TU->decls_end();
       D != DEnd; ++D) 
    if (ObjCCategoryDecl *Category = dyn_cast<ObjCCategoryDecl>(*D))
      if (CategoryNames.insert(Category->getIdentifier()))
        Results.AddResult(Result(Category, 0), CurContext, 0, false);
  Results.ExitScope();
  
  HandleCodeCompleteResults(this, CodeCompleter, 
                            CodeCompletionContext::CCC_ObjCCategoryName,
                            Results.data(),Results.size());  
}

void Sema::CodeCompleteObjCImplementationCategory(Scope *S, 
                                                  IdentifierInfo *ClassName,
                                                  SourceLocation ClassNameLoc) {
  typedef CodeCompletionResult Result;
  
  // Find the corresponding interface. If we couldn't find the interface, the
  // program itself is ill-formed. However, we'll try to be helpful still by
  // providing the list of all of the categories we know about.
  NamedDecl *CurClass
    = LookupSingleName(TUScope, ClassName, ClassNameLoc, LookupOrdinaryName);
  ObjCInterfaceDecl *Class = dyn_cast_or_null<ObjCInterfaceDecl>(CurClass);
  if (!Class)
    return CodeCompleteObjCInterfaceCategory(S, ClassName, ClassNameLoc);
    
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_ObjCCategoryName);
  
  // Add all of the categories that have have corresponding interface 
  // declarations in this class and any of its superclasses, except for
  // already-implemented categories in the class itself.
  llvm::SmallPtrSet<IdentifierInfo *, 16> CategoryNames;
  Results.EnterNewScope();
  bool IgnoreImplemented = true;
  while (Class) {
    for (ObjCCategoryDecl *Category = Class->getCategoryList(); Category;
         Category = Category->getNextClassCategory())
      if ((!IgnoreImplemented || !Category->getImplementation()) &&
          CategoryNames.insert(Category->getIdentifier()))
        Results.AddResult(Result(Category, 0), CurContext, 0, false);
    
    Class = Class->getSuperClass();
    IgnoreImplemented = false;
  }
  Results.ExitScope();
  
  HandleCodeCompleteResults(this, CodeCompleter, 
                            CodeCompletionContext::CCC_ObjCCategoryName,
                            Results.data(),Results.size());  
}

void Sema::CodeCompleteObjCPropertyDefinition(Scope *S) {
  typedef CodeCompletionResult Result;
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_Other);

  // Figure out where this @synthesize lives.
  ObjCContainerDecl *Container
    = dyn_cast_or_null<ObjCContainerDecl>(CurContext);
  if (!Container || 
      (!isa<ObjCImplementationDecl>(Container) && 
       !isa<ObjCCategoryImplDecl>(Container)))
    return; 

  // Ignore any properties that have already been implemented.
  for (DeclContext::decl_iterator D = Container->decls_begin(), 
                               DEnd = Container->decls_end();
       D != DEnd; ++D)
    if (ObjCPropertyImplDecl *PropertyImpl = dyn_cast<ObjCPropertyImplDecl>(*D))
      Results.Ignore(PropertyImpl->getPropertyDecl());
  
  // Add any properties that we find.
  AddedPropertiesSet AddedProperties;
  Results.EnterNewScope();
  if (ObjCImplementationDecl *ClassImpl
        = dyn_cast<ObjCImplementationDecl>(Container))
    AddObjCProperties(ClassImpl->getClassInterface(), false, 
                      /*AllowNullaryMethods=*/false, CurContext, 
                      AddedProperties, Results);
  else
    AddObjCProperties(cast<ObjCCategoryImplDecl>(Container)->getCategoryDecl(),
                      false, /*AllowNullaryMethods=*/false, CurContext, 
                      AddedProperties, Results);
  Results.ExitScope();
  
  HandleCodeCompleteResults(this, CodeCompleter, 
                            CodeCompletionContext::CCC_Other,
                            Results.data(),Results.size());  
}

void Sema::CodeCompleteObjCPropertySynthesizeIvar(Scope *S, 
                                                  IdentifierInfo *PropertyName) {
  typedef CodeCompletionResult Result;
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_Other);

  // Figure out where this @synthesize lives.
  ObjCContainerDecl *Container
    = dyn_cast_or_null<ObjCContainerDecl>(CurContext);
  if (!Container || 
      (!isa<ObjCImplementationDecl>(Container) && 
       !isa<ObjCCategoryImplDecl>(Container)))
    return; 
  
  // Figure out which interface we're looking into.
  ObjCInterfaceDecl *Class = 0;
  if (ObjCImplementationDecl *ClassImpl
                                 = dyn_cast<ObjCImplementationDecl>(Container))  
    Class = ClassImpl->getClassInterface();
  else
    Class = cast<ObjCCategoryImplDecl>(Container)->getCategoryDecl()
                                                          ->getClassInterface();

  // Determine the type of the property we're synthesizing.
  QualType PropertyType = Context.getObjCIdType();
  if (Class) {
    if (ObjCPropertyDecl *Property
                              = Class->FindPropertyDeclaration(PropertyName)) {
      PropertyType 
        = Property->getType().getNonReferenceType().getUnqualifiedType();
      
      // Give preference to ivars 
      Results.setPreferredType(PropertyType);
    }
  }

  // Add all of the instance variables in this class and its superclasses.
  Results.EnterNewScope();
  bool SawSimilarlyNamedIvar = false;
  std::string NameWithPrefix;
  NameWithPrefix += '_';
  NameWithPrefix += PropertyName->getName();
  std::string NameWithSuffix = PropertyName->getName().str();
  NameWithSuffix += '_';
  for(; Class; Class = Class->getSuperClass()) {
    for (ObjCIvarDecl *Ivar = Class->all_declared_ivar_begin(); Ivar; 
         Ivar = Ivar->getNextIvar()) {
      Results.AddResult(Result(Ivar, 0), CurContext, 0, false);
      
      // Determine whether we've seen an ivar with a name similar to the 
      // property.
      if ((PropertyName == Ivar->getIdentifier() ||
           NameWithPrefix == Ivar->getName() ||
           NameWithSuffix == Ivar->getName())) {
        SawSimilarlyNamedIvar = true;
       
        // Reduce the priority of this result by one, to give it a slight
        // advantage over other results whose names don't match so closely.
        if (Results.size() && 
            Results.data()[Results.size() - 1].Kind 
                                      == CodeCompletionResult::RK_Declaration &&
            Results.data()[Results.size() - 1].Declaration == Ivar)
          Results.data()[Results.size() - 1].Priority--;
      }
    }
  }
  
  if (!SawSimilarlyNamedIvar) {
    // Create ivar result _propName, that the user can use to synthesize
    // an ivar of the appropriate type.    
    unsigned Priority = CCP_MemberDeclaration + 1;
    typedef CodeCompletionResult Result;
    CodeCompletionAllocator &Allocator = Results.getAllocator();
    CodeCompletionBuilder Builder(Allocator, Priority,CXAvailability_Available);

    PrintingPolicy Policy = getCompletionPrintingPolicy(*this);
    Builder.AddResultTypeChunk(GetCompletionTypeString(PropertyType, Context,
                                                       Policy, Allocator));
    Builder.AddTypedTextChunk(Allocator.CopyString(NameWithPrefix));
    Results.AddResult(Result(Builder.TakeString(), Priority, 
                             CXCursor_ObjCIvarDecl));
  }
  
  Results.ExitScope();
  
  HandleCodeCompleteResults(this, CodeCompleter, 
                            CodeCompletionContext::CCC_Other,
                            Results.data(),Results.size());
}

// Mapping from selectors to the methods that implement that selector, along
// with the "in original class" flag.
typedef llvm::DenseMap<Selector, std::pair<ObjCMethodDecl *, bool> > 
  KnownMethodsMap;

/// \brief Find all of the methods that reside in the given container
/// (and its superclasses, protocols, etc.) that meet the given
/// criteria. Insert those methods into the map of known methods,
/// indexed by selector so they can be easily found.
static void FindImplementableMethods(ASTContext &Context,
                                     ObjCContainerDecl *Container,
                                     bool WantInstanceMethods,
                                     QualType ReturnType,
                                     KnownMethodsMap &KnownMethods,
                                     bool InOriginalClass = true) {
  if (ObjCInterfaceDecl *IFace = dyn_cast<ObjCInterfaceDecl>(Container)) {
    // Recurse into protocols.
    if (!IFace->hasDefinition())
      return;
    
    const ObjCList<ObjCProtocolDecl> &Protocols
      = IFace->getReferencedProtocols();
    for (ObjCList<ObjCProtocolDecl>::iterator I = Protocols.begin(),
                                              E = Protocols.end(); 
         I != E; ++I)
      FindImplementableMethods(Context, *I, WantInstanceMethods, ReturnType,
                               KnownMethods, InOriginalClass);

    // Add methods from any class extensions and categories.
    for (const ObjCCategoryDecl *Cat = IFace->getCategoryList(); Cat;
         Cat = Cat->getNextClassCategory())
      FindImplementableMethods(Context, const_cast<ObjCCategoryDecl*>(Cat), 
                               WantInstanceMethods, ReturnType,
                               KnownMethods, false);      
    
    // Visit the superclass.
    if (IFace->getSuperClass())
      FindImplementableMethods(Context, IFace->getSuperClass(), 
                               WantInstanceMethods, ReturnType,
                               KnownMethods, false);
  }

  if (ObjCCategoryDecl *Category = dyn_cast<ObjCCategoryDecl>(Container)) {
    // Recurse into protocols.
    const ObjCList<ObjCProtocolDecl> &Protocols
      = Category->getReferencedProtocols();
    for (ObjCList<ObjCProtocolDecl>::iterator I = Protocols.begin(),
                                              E = Protocols.end(); 
         I != E; ++I)
      FindImplementableMethods(Context, *I, WantInstanceMethods, ReturnType,
                               KnownMethods, InOriginalClass);
    
    // If this category is the original class, jump to the interface.
    if (InOriginalClass && Category->getClassInterface())
      FindImplementableMethods(Context, Category->getClassInterface(), 
                               WantInstanceMethods, ReturnType, KnownMethods,
                               false);
  }

  if (ObjCProtocolDecl *Protocol = dyn_cast<ObjCProtocolDecl>(Container)) {
    if (Protocol->hasDefinition()) {
      // Recurse into protocols.
      const ObjCList<ObjCProtocolDecl> &Protocols
        = Protocol->getReferencedProtocols();
      for (ObjCList<ObjCProtocolDecl>::iterator I = Protocols.begin(),
             E = Protocols.end(); 
           I != E; ++I)
        FindImplementableMethods(Context, *I, WantInstanceMethods, ReturnType,
                                 KnownMethods, false);
    }
  }

  // Add methods in this container. This operation occurs last because
  // we want the methods from this container to override any methods
  // we've previously seen with the same selector.
  for (ObjCContainerDecl::method_iterator M = Container->meth_begin(),
                                       MEnd = Container->meth_end();
       M != MEnd; ++M) {
    if ((*M)->isInstanceMethod() == WantInstanceMethods) {
      if (!ReturnType.isNull() &&
          !Context.hasSameUnqualifiedType(ReturnType, (*M)->getResultType()))
        continue;

      KnownMethods[(*M)->getSelector()] = std::make_pair(*M, InOriginalClass);
    }
  }
}

/// \brief Add the parenthesized return or parameter type chunk to a code 
/// completion string.
static void AddObjCPassingTypeChunk(QualType Type,
                                    ASTContext &Context,
                                    const PrintingPolicy &Policy,
                                    CodeCompletionBuilder &Builder) {
  Builder.AddChunk(CodeCompletionString::CK_LeftParen);
  Builder.AddTextChunk(GetCompletionTypeString(Type, Context, Policy,
                                               Builder.getAllocator()));
  Builder.AddChunk(CodeCompletionString::CK_RightParen);
}

/// \brief Determine whether the given class is or inherits from a class by
/// the given name.
static bool InheritsFromClassNamed(ObjCInterfaceDecl *Class, 
                                   StringRef Name) {
  if (!Class)
    return false;
  
  if (Class->getIdentifier() && Class->getIdentifier()->getName() == Name)
    return true;
  
  return InheritsFromClassNamed(Class->getSuperClass(), Name);
}
                  
/// \brief Add code completions for Objective-C Key-Value Coding (KVC) and
/// Key-Value Observing (KVO).
static void AddObjCKeyValueCompletions(ObjCPropertyDecl *Property,
                                       bool IsInstanceMethod,
                                       QualType ReturnType,
                                       ASTContext &Context,
                                       VisitedSelectorSet &KnownSelectors,
                                       ResultBuilder &Results) {
  IdentifierInfo *PropName = Property->getIdentifier();
  if (!PropName || PropName->getLength() == 0)
    return;
  
  PrintingPolicy Policy = getCompletionPrintingPolicy(Results.getSema());

  // Builder that will create each code completion.
  typedef CodeCompletionResult Result;
  CodeCompletionAllocator &Allocator = Results.getAllocator();
  CodeCompletionBuilder Builder(Allocator);
  
  // The selector table.
  SelectorTable &Selectors = Context.Selectors;
  
  // The property name, copied into the code completion allocation region
  // on demand.
  struct KeyHolder {
    CodeCompletionAllocator &Allocator;
    StringRef Key;
    const char *CopiedKey;
    
    KeyHolder(CodeCompletionAllocator &Allocator, StringRef Key)
    : Allocator(Allocator), Key(Key), CopiedKey(0) { }
    
    operator const char *() {
      if (CopiedKey)
        return CopiedKey;
      
      return CopiedKey = Allocator.CopyString(Key);
    }
  } Key(Allocator, PropName->getName());
  
  // The uppercased name of the property name.
  std::string UpperKey = PropName->getName();
  if (!UpperKey.empty())
    UpperKey[0] = toupper(UpperKey[0]);      
  
  bool ReturnTypeMatchesProperty = ReturnType.isNull() ||
    Context.hasSameUnqualifiedType(ReturnType.getNonReferenceType(), 
                                   Property->getType());
  bool ReturnTypeMatchesVoid 
    = ReturnType.isNull() || ReturnType->isVoidType();
  
  // Add the normal accessor -(type)key.
  if (IsInstanceMethod &&
      KnownSelectors.insert(Selectors.getNullarySelector(PropName)) &&
      ReturnTypeMatchesProperty && !Property->getGetterMethodDecl()) {
    if (ReturnType.isNull())
      AddObjCPassingTypeChunk(Property->getType(), Context, Policy, Builder);
    
    Builder.AddTypedTextChunk(Key);
    Results.AddResult(Result(Builder.TakeString(), CCP_CodePattern, 
                             CXCursor_ObjCInstanceMethodDecl));
  }
  
  // If we have an integral or boolean property (or the user has provided
  // an integral or boolean return type), add the accessor -(type)isKey.
  if (IsInstanceMethod &&
      ((!ReturnType.isNull() && 
        (ReturnType->isIntegerType() || ReturnType->isBooleanType())) ||
       (ReturnType.isNull() && 
        (Property->getType()->isIntegerType() || 
         Property->getType()->isBooleanType())))) {
    std::string SelectorName = (Twine("is") + UpperKey).str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getNullarySelector(SelectorId))) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("BOOL");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }
      
      Builder.AddTypedTextChunk(
                                Allocator.CopyString(SelectorId->getName()));
      Results.AddResult(Result(Builder.TakeString(), CCP_CodePattern, 
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }
  
  // Add the normal mutator.
  if (IsInstanceMethod && ReturnTypeMatchesVoid && 
      !Property->getSetterMethodDecl()) {
    std::string SelectorName = (Twine("set") + UpperKey).str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getUnarySelector(SelectorId))) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("void");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }
      
      Builder.AddTypedTextChunk(
                                Allocator.CopyString(SelectorId->getName()));
      Builder.AddTypedTextChunk(":");
      AddObjCPassingTypeChunk(Property->getType(), Context, Policy, Builder);
      Builder.AddTextChunk(Key);
      Results.AddResult(Result(Builder.TakeString(), CCP_CodePattern, 
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }
  
  // Indexed and unordered accessors
  unsigned IndexedGetterPriority = CCP_CodePattern;
  unsigned IndexedSetterPriority = CCP_CodePattern;
  unsigned UnorderedGetterPriority = CCP_CodePattern;
  unsigned UnorderedSetterPriority = CCP_CodePattern;
  if (const ObjCObjectPointerType *ObjCPointer 
                    = Property->getType()->getAs<ObjCObjectPointerType>()) {
    if (ObjCInterfaceDecl *IFace = ObjCPointer->getInterfaceDecl()) {
      // If this interface type is not provably derived from a known
      // collection, penalize the corresponding completions.
      if (!InheritsFromClassNamed(IFace, "NSMutableArray")) {
        IndexedSetterPriority += CCD_ProbablyNotObjCCollection;            
        if (!InheritsFromClassNamed(IFace, "NSArray"))
          IndexedGetterPriority += CCD_ProbablyNotObjCCollection;
      }

      if (!InheritsFromClassNamed(IFace, "NSMutableSet")) {
        UnorderedSetterPriority += CCD_ProbablyNotObjCCollection;            
        if (!InheritsFromClassNamed(IFace, "NSSet"))
          UnorderedGetterPriority += CCD_ProbablyNotObjCCollection;
      }
    }
  } else {
    IndexedGetterPriority += CCD_ProbablyNotObjCCollection;
    IndexedSetterPriority += CCD_ProbablyNotObjCCollection;
    UnorderedGetterPriority += CCD_ProbablyNotObjCCollection;
    UnorderedSetterPriority += CCD_ProbablyNotObjCCollection;
  }
  
  // Add -(NSUInteger)countOf<key>
  if (IsInstanceMethod &&  
      (ReturnType.isNull() || ReturnType->isIntegerType())) {
    std::string SelectorName = (Twine("countOf") + UpperKey).str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getNullarySelector(SelectorId))) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("NSUInteger");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }
      
      Builder.AddTypedTextChunk(
                                Allocator.CopyString(SelectorId->getName()));
      Results.AddResult(Result(Builder.TakeString(), 
                               std::min(IndexedGetterPriority, 
                                        UnorderedGetterPriority),
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }
  
  // Indexed getters
  // Add -(id)objectInKeyAtIndex:(NSUInteger)index
  if (IsInstanceMethod &&
      (ReturnType.isNull() || ReturnType->isObjCObjectPointerType())) {
    std::string SelectorName
      = (Twine("objectIn") + UpperKey + "AtIndex").str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getUnarySelector(SelectorId))) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("id");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }
      
      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddTextChunk("NSUInteger");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("index");
      Results.AddResult(Result(Builder.TakeString(), IndexedGetterPriority, 
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }
  
  // Add -(NSArray *)keyAtIndexes:(NSIndexSet *)indexes
  if (IsInstanceMethod &&
      (ReturnType.isNull() || 
       (ReturnType->isObjCObjectPointerType() &&
        ReturnType->getAs<ObjCObjectPointerType>()->getInterfaceDecl() &&
        ReturnType->getAs<ObjCObjectPointerType>()->getInterfaceDecl()
                                                ->getName() == "NSArray"))) {
    std::string SelectorName
      = (Twine(Property->getName()) + "AtIndexes").str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getUnarySelector(SelectorId))) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("NSArray *");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }
       
      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddTextChunk("NSIndexSet *");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("indexes");
      Results.AddResult(Result(Builder.TakeString(), IndexedGetterPriority, 
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }
  
  // Add -(void)getKey:(type **)buffer range:(NSRange)inRange
  if (IsInstanceMethod && ReturnTypeMatchesVoid) {
    std::string SelectorName = (Twine("get") + UpperKey).str();
    IdentifierInfo *SelectorIds[2] = {
      &Context.Idents.get(SelectorName),
      &Context.Idents.get("range")
    };
    
    if (KnownSelectors.insert(Selectors.getSelector(2, SelectorIds))) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("void");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }
      
      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("object-type");
      Builder.AddTextChunk(" **");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("buffer");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddTypedTextChunk("range:");
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddTextChunk("NSRange");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("inRange");
      Results.AddResult(Result(Builder.TakeString(), IndexedGetterPriority, 
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }
  
  // Mutable indexed accessors
  
  // - (void)insertObject:(type *)object inKeyAtIndex:(NSUInteger)index
  if (IsInstanceMethod && ReturnTypeMatchesVoid) {
    std::string SelectorName = (Twine("in") + UpperKey + "AtIndex").str();
    IdentifierInfo *SelectorIds[2] = {
      &Context.Idents.get("insertObject"),
      &Context.Idents.get(SelectorName)
    };
    
    if (KnownSelectors.insert(Selectors.getSelector(2, SelectorIds))) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("void");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }
      
      Builder.AddTypedTextChunk("insertObject:");
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("object-type");
      Builder.AddTextChunk(" *");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("object");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("NSUInteger");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("index");
      Results.AddResult(Result(Builder.TakeString(), IndexedSetterPriority, 
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }
  
  // - (void)insertKey:(NSArray *)array atIndexes:(NSIndexSet *)indexes
  if (IsInstanceMethod && ReturnTypeMatchesVoid) {
    std::string SelectorName = (Twine("insert") + UpperKey).str();
    IdentifierInfo *SelectorIds[2] = {
      &Context.Idents.get(SelectorName),
      &Context.Idents.get("atIndexes")
    };
    
    if (KnownSelectors.insert(Selectors.getSelector(2, SelectorIds))) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("void");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }
      
      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddTextChunk("NSArray *");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("array");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddTypedTextChunk("atIndexes:");
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("NSIndexSet *");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("indexes");
      Results.AddResult(Result(Builder.TakeString(), IndexedSetterPriority, 
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }
  
  // -(void)removeObjectFromKeyAtIndex:(NSUInteger)index
  if (IsInstanceMethod && ReturnTypeMatchesVoid) {
    std::string SelectorName
      = (Twine("removeObjectFrom") + UpperKey + "AtIndex").str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);        
    if (KnownSelectors.insert(Selectors.getUnarySelector(SelectorId))) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("void");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }
      
      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddTextChunk("NSUInteger");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("index");
      Results.AddResult(Result(Builder.TakeString(), IndexedSetterPriority, 
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }
  
  // -(void)removeKeyAtIndexes:(NSIndexSet *)indexes
  if (IsInstanceMethod && ReturnTypeMatchesVoid) {
    std::string SelectorName
      = (Twine("remove") + UpperKey + "AtIndexes").str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);        
    if (KnownSelectors.insert(Selectors.getUnarySelector(SelectorId))) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("void");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }
      
      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddTextChunk("NSIndexSet *");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("indexes");
      Results.AddResult(Result(Builder.TakeString(), IndexedSetterPriority, 
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }
  
  // - (void)replaceObjectInKeyAtIndex:(NSUInteger)index withObject:(id)object
  if (IsInstanceMethod && ReturnTypeMatchesVoid) {
    std::string SelectorName
      = (Twine("replaceObjectIn") + UpperKey + "AtIndex").str();
    IdentifierInfo *SelectorIds[2] = {
      &Context.Idents.get(SelectorName),
      &Context.Idents.get("withObject")
    };
    
    if (KnownSelectors.insert(Selectors.getSelector(2, SelectorIds))) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("void");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }
      
      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("NSUInteger");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("index");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddTypedTextChunk("withObject:");
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddTextChunk("id");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("object");
      Results.AddResult(Result(Builder.TakeString(), IndexedSetterPriority, 
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }
  
  // - (void)replaceKeyAtIndexes:(NSIndexSet *)indexes withKey:(NSArray *)array
  if (IsInstanceMethod && ReturnTypeMatchesVoid) {
    std::string SelectorName1 
      = (Twine("replace") + UpperKey + "AtIndexes").str();
    std::string SelectorName2 = (Twine("with") + UpperKey).str();
    IdentifierInfo *SelectorIds[2] = {
      &Context.Idents.get(SelectorName1),
      &Context.Idents.get(SelectorName2)
    };
    
    if (KnownSelectors.insert(Selectors.getSelector(2, SelectorIds))) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("void");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }
      
      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName1 + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("NSIndexSet *");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("indexes");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName2 + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddTextChunk("NSArray *");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("array");
      Results.AddResult(Result(Builder.TakeString(), IndexedSetterPriority, 
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }  
  
  // Unordered getters
  // - (NSEnumerator *)enumeratorOfKey
  if (IsInstanceMethod && 
      (ReturnType.isNull() || 
       (ReturnType->isObjCObjectPointerType() &&
        ReturnType->getAs<ObjCObjectPointerType>()->getInterfaceDecl() &&
        ReturnType->getAs<ObjCObjectPointerType>()->getInterfaceDecl()
          ->getName() == "NSEnumerator"))) {
    std::string SelectorName = (Twine("enumeratorOf") + UpperKey).str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getNullarySelector(SelectorId))) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("NSEnumerator *");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }
       
      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName));
      Results.AddResult(Result(Builder.TakeString(), UnorderedGetterPriority, 
                              CXCursor_ObjCInstanceMethodDecl));
    }
  }

  // - (type *)memberOfKey:(type *)object
  if (IsInstanceMethod && 
      (ReturnType.isNull() || ReturnType->isObjCObjectPointerType())) {
    std::string SelectorName = (Twine("memberOf") + UpperKey).str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getUnarySelector(SelectorId))) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddPlaceholderChunk("object-type");
        Builder.AddTextChunk(" *");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }
      
      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      if (ReturnType.isNull()) {
        Builder.AddPlaceholderChunk("object-type");
        Builder.AddTextChunk(" *");
      } else {
        Builder.AddTextChunk(GetCompletionTypeString(ReturnType, Context, 
                                                     Policy,
                                                     Builder.getAllocator()));
      }
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("object");
      Results.AddResult(Result(Builder.TakeString(), UnorderedGetterPriority, 
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }
  
  // Mutable unordered accessors
  // - (void)addKeyObject:(type *)object
  if (IsInstanceMethod && ReturnTypeMatchesVoid) {
    std::string SelectorName
      = (Twine("add") + UpperKey + Twine("Object")).str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getUnarySelector(SelectorId))) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("void");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }
      
      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("object-type");
      Builder.AddTextChunk(" *");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("object");
      Results.AddResult(Result(Builder.TakeString(), UnorderedSetterPriority, 
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }  

  // - (void)addKey:(NSSet *)objects
  if (IsInstanceMethod && ReturnTypeMatchesVoid) {
    std::string SelectorName = (Twine("add") + UpperKey).str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getUnarySelector(SelectorId))) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("void");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }
      
      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddTextChunk("NSSet *");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("objects");
      Results.AddResult(Result(Builder.TakeString(), UnorderedSetterPriority, 
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }  
  
  // - (void)removeKeyObject:(type *)object
  if (IsInstanceMethod && ReturnTypeMatchesVoid) {
    std::string SelectorName
      = (Twine("remove") + UpperKey + Twine("Object")).str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getUnarySelector(SelectorId))) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("void");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }
      
      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("object-type");
      Builder.AddTextChunk(" *");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("object");
      Results.AddResult(Result(Builder.TakeString(), UnorderedSetterPriority, 
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }  
  
  // - (void)removeKey:(NSSet *)objects
  if (IsInstanceMethod && ReturnTypeMatchesVoid) {
    std::string SelectorName = (Twine("remove") + UpperKey).str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getUnarySelector(SelectorId))) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("void");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }
      
      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddTextChunk("NSSet *");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("objects");
      Results.AddResult(Result(Builder.TakeString(), UnorderedSetterPriority, 
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }    

  // - (void)intersectKey:(NSSet *)objects
  if (IsInstanceMethod && ReturnTypeMatchesVoid) {
    std::string SelectorName = (Twine("intersect") + UpperKey).str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getUnarySelector(SelectorId))) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("void");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }
      
      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddTextChunk("NSSet *");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("objects");
      Results.AddResult(Result(Builder.TakeString(), UnorderedSetterPriority, 
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }  
  
  // Key-Value Observing
  // + (NSSet *)keyPathsForValuesAffectingKey
  if (!IsInstanceMethod && 
      (ReturnType.isNull() || 
       (ReturnType->isObjCObjectPointerType() &&
        ReturnType->getAs<ObjCObjectPointerType>()->getInterfaceDecl() &&
        ReturnType->getAs<ObjCObjectPointerType>()->getInterfaceDecl()
                                                    ->getName() == "NSSet"))) {
    std::string SelectorName 
      = (Twine("keyPathsForValuesAffecting") + UpperKey).str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getNullarySelector(SelectorId))) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("NSSet *");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }
       
      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName));
      Results.AddResult(Result(Builder.TakeString(), CCP_CodePattern, 
                              CXCursor_ObjCClassMethodDecl));
    }
  }

  // + (BOOL)automaticallyNotifiesObserversForKey
  if (!IsInstanceMethod &&
      (ReturnType.isNull() ||
       ReturnType->isIntegerType() || 
       ReturnType->isBooleanType())) {
    std::string SelectorName 
      = (Twine("automaticallyNotifiesObserversOf") + UpperKey).str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getNullarySelector(SelectorId))) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("BOOL");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }
       
      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName));
      Results.AddResult(Result(Builder.TakeString(), CCP_CodePattern, 
                              CXCursor_ObjCClassMethodDecl));
    }
  }
}

void Sema::CodeCompleteObjCMethodDecl(Scope *S, 
                                      bool IsInstanceMethod,
                                      ParsedType ReturnTy) {
  // Determine the return type of the method we're declaring, if
  // provided.
  QualType ReturnType = GetTypeFromParser(ReturnTy);
  Decl *IDecl = 0;
  if (CurContext->isObjCContainer()) {
      ObjCContainerDecl *OCD = dyn_cast<ObjCContainerDecl>(CurContext);
      IDecl = cast<Decl>(OCD);
  }
  // Determine where we should start searching for methods.
  ObjCContainerDecl *SearchDecl = 0;
  bool IsInImplementation = false;
  if (Decl *D = IDecl) {
    if (ObjCImplementationDecl *Impl = dyn_cast<ObjCImplementationDecl>(D)) {
      SearchDecl = Impl->getClassInterface();
      IsInImplementation = true;
    } else if (ObjCCategoryImplDecl *CatImpl 
                                         = dyn_cast<ObjCCategoryImplDecl>(D)) {
      SearchDecl = CatImpl->getCategoryDecl();
      IsInImplementation = true;
    } else
      SearchDecl = dyn_cast<ObjCContainerDecl>(D);
  }

  if (!SearchDecl && S) {
    if (DeclContext *DC = static_cast<DeclContext *>(S->getEntity()))
      SearchDecl = dyn_cast<ObjCContainerDecl>(DC);
  }

  if (!SearchDecl) {
    HandleCodeCompleteResults(this, CodeCompleter, 
                              CodeCompletionContext::CCC_Other,
                              0, 0);
    return;
  }
    
  // Find all of the methods that we could declare/implement here.
  KnownMethodsMap KnownMethods;
  FindImplementableMethods(Context, SearchDecl, IsInstanceMethod, 
                           ReturnType, KnownMethods);
  
  // Add declarations or definitions for each of the known methods.
  typedef CodeCompletionResult Result;
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_Other);
  Results.EnterNewScope();
  PrintingPolicy Policy = getCompletionPrintingPolicy(*this);
  for (KnownMethodsMap::iterator M = KnownMethods.begin(), 
                              MEnd = KnownMethods.end();
       M != MEnd; ++M) {
    ObjCMethodDecl *Method = M->second.first;
    CodeCompletionBuilder Builder(Results.getAllocator());
    
    // If the result type was not already provided, add it to the
    // pattern as (type).
    if (ReturnType.isNull())
      AddObjCPassingTypeChunk(Method->getResultType(), Context, Policy, 
                              Builder);

    Selector Sel = Method->getSelector();

    // Add the first part of the selector to the pattern.
    Builder.AddTypedTextChunk(Builder.getAllocator().CopyString(
                                                       Sel.getNameForSlot(0)));

    // Add parameters to the pattern.
    unsigned I = 0;
    for (ObjCMethodDecl::param_iterator P = Method->param_begin(), 
                                     PEnd = Method->param_end();
         P != PEnd; (void)++P, ++I) {
      // Add the part of the selector name.
      if (I == 0)
        Builder.AddTypedTextChunk(":");
      else if (I < Sel.getNumArgs()) {
        Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
        Builder.AddTypedTextChunk(
                Builder.getAllocator().CopyString(Sel.getNameForSlot(I) + ":"));
      } else
        break;

      // Add the parameter type.
      AddObjCPassingTypeChunk((*P)->getOriginalType(), Context, Policy, 
                              Builder);
      
      if (IdentifierInfo *Id = (*P)->getIdentifier())
        Builder.AddTextChunk(Builder.getAllocator().CopyString( Id->getName())); 
    }

    if (Method->isVariadic()) {
      if (Method->param_size() > 0)
        Builder.AddChunk(CodeCompletionString::CK_Comma);
      Builder.AddTextChunk("...");
    }        

    if (IsInImplementation && Results.includeCodePatterns()) {
      // We will be defining the method here, so add a compound statement.
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
      Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
      if (!Method->getResultType()->isVoidType()) {
        // If the result type is not void, add a return clause.
        Builder.AddTextChunk("return");
        Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
        Builder.AddPlaceholderChunk("expression");
        Builder.AddChunk(CodeCompletionString::CK_SemiColon);
      } else
        Builder.AddPlaceholderChunk("statements");
        
      Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
      Builder.AddChunk(CodeCompletionString::CK_RightBrace);
    }

    unsigned Priority = CCP_CodePattern;
    if (!M->second.second)
      Priority += CCD_InBaseClass;
    
    Results.AddResult(Result(Builder.TakeString(), Priority, 
                             Method->isInstanceMethod()
                               ? CXCursor_ObjCInstanceMethodDecl
                               : CXCursor_ObjCClassMethodDecl));
  }

  // Add Key-Value-Coding and Key-Value-Observing accessor methods for all of 
  // the properties in this class and its categories.
  if (Context.getLangOptions().ObjC2) {
    SmallVector<ObjCContainerDecl *, 4> Containers;
    Containers.push_back(SearchDecl);
    
    VisitedSelectorSet KnownSelectors;
    for (KnownMethodsMap::iterator M = KnownMethods.begin(), 
                                MEnd = KnownMethods.end();
         M != MEnd; ++M)
      KnownSelectors.insert(M->first);

    
    ObjCInterfaceDecl *IFace = dyn_cast<ObjCInterfaceDecl>(SearchDecl);
    if (!IFace)
      if (ObjCCategoryDecl *Category = dyn_cast<ObjCCategoryDecl>(SearchDecl))
        IFace = Category->getClassInterface();
    
    if (IFace) {
      for (ObjCCategoryDecl *Category = IFace->getCategoryList(); Category;
           Category = Category->getNextClassCategory())
        Containers.push_back(Category);
    }
    
    for (unsigned I = 0, N = Containers.size(); I != N; ++I) {
      for (ObjCContainerDecl::prop_iterator P = Containers[I]->prop_begin(),
                                         PEnd = Containers[I]->prop_end(); 
           P != PEnd; ++P) {
        AddObjCKeyValueCompletions(*P, IsInstanceMethod, ReturnType, Context, 
                                   KnownSelectors, Results);
      }
    }
  }
  
  Results.ExitScope();
  
  HandleCodeCompleteResults(this, CodeCompleter, 
                            CodeCompletionContext::CCC_Other,
                            Results.data(),Results.size());
}

void Sema::CodeCompleteObjCMethodDeclSelector(Scope *S, 
                                              bool IsInstanceMethod,
                                              bool AtParameterName,
                                              ParsedType ReturnTy,
                                              IdentifierInfo **SelIdents,
                                              unsigned NumSelIdents) {
  // If we have an external source, load the entire class method
  // pool from the AST file.
  if (ExternalSource) {
    for (uint32_t I = 0, N = ExternalSource->GetNumExternalSelectors();
         I != N; ++I) {
      Selector Sel = ExternalSource->GetExternalSelector(I);
      if (Sel.isNull() || MethodPool.count(Sel))
        continue;

      ReadMethodPool(Sel);
    }
  }

  // Build the set of methods we can see.
  typedef CodeCompletionResult Result;
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_Other);
  
  if (ReturnTy)
    Results.setPreferredType(GetTypeFromParser(ReturnTy).getNonReferenceType());

  Results.EnterNewScope();  
  for (GlobalMethodPool::iterator M = MethodPool.begin(),
                                  MEnd = MethodPool.end();
       M != MEnd; ++M) {
    for (ObjCMethodList *MethList = IsInstanceMethod ? &M->second.first :
                                                       &M->second.second;
         MethList && MethList->Method; 
         MethList = MethList->Next) {
      if (!isAcceptableObjCMethod(MethList->Method, MK_Any, SelIdents, 
                                  NumSelIdents))
        continue;
      
      if (AtParameterName) {
        // Suggest parameter names we've seen before.
        if (NumSelIdents && NumSelIdents <= MethList->Method->param_size()) {
          ParmVarDecl *Param = MethList->Method->param_begin()[NumSelIdents-1];
          if (Param->getIdentifier()) {
            CodeCompletionBuilder Builder(Results.getAllocator());
            Builder.AddTypedTextChunk(Builder.getAllocator().CopyString(
                                           Param->getIdentifier()->getName()));
            Results.AddResult(Builder.TakeString());
          }
        }
        
        continue;
      }
      
      Result R(MethList->Method, 0);
      R.StartParameter = NumSelIdents;
      R.AllParametersAreInformative = false;
      R.DeclaringEntity = true;
      Results.MaybeAddResult(R, CurContext);
    }
  }
  
  Results.ExitScope();
  HandleCodeCompleteResults(this, CodeCompleter, 
                            CodeCompletionContext::CCC_Other,
                            Results.data(),Results.size());
}

void Sema::CodeCompletePreprocessorDirective(bool InConditional) {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_PreprocessorDirective);
  Results.EnterNewScope();
  
  // #if <condition>
  CodeCompletionBuilder Builder(Results.getAllocator());
  Builder.AddTypedTextChunk("if");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("condition");
  Results.AddResult(Builder.TakeString());
  
  // #ifdef <macro>
  Builder.AddTypedTextChunk("ifdef");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("macro");
  Results.AddResult(Builder.TakeString());
  
  // #ifndef <macro>
  Builder.AddTypedTextChunk("ifndef");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("macro");
  Results.AddResult(Builder.TakeString());

  if (InConditional) {
    // #elif <condition>
    Builder.AddTypedTextChunk("elif");
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddPlaceholderChunk("condition");
    Results.AddResult(Builder.TakeString());

    // #else
    Builder.AddTypedTextChunk("else");
    Results.AddResult(Builder.TakeString());

    // #endif
    Builder.AddTypedTextChunk("endif");
    Results.AddResult(Builder.TakeString());
  }
  
  // #include "header"
  Builder.AddTypedTextChunk("include");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddTextChunk("\"");
  Builder.AddPlaceholderChunk("header");
  Builder.AddTextChunk("\"");
  Results.AddResult(Builder.TakeString());

  // #include <header>
  Builder.AddTypedTextChunk("include");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddTextChunk("<");
  Builder.AddPlaceholderChunk("header");
  Builder.AddTextChunk(">");
  Results.AddResult(Builder.TakeString());
  
  // #define <macro>
  Builder.AddTypedTextChunk("define");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("macro");
  Results.AddResult(Builder.TakeString());
  
  // #define <macro>(<args>)
  Builder.AddTypedTextChunk("define");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("macro");
  Builder.AddChunk(CodeCompletionString::CK_LeftParen);
  Builder.AddPlaceholderChunk("args");
  Builder.AddChunk(CodeCompletionString::CK_RightParen);
  Results.AddResult(Builder.TakeString());
  
  // #undef <macro>
  Builder.AddTypedTextChunk("undef");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("macro");
  Results.AddResult(Builder.TakeString());

  // #line <number>
  Builder.AddTypedTextChunk("line");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("number");
  Results.AddResult(Builder.TakeString());
  
  // #line <number> "filename"
  Builder.AddTypedTextChunk("line");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("number");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddTextChunk("\"");
  Builder.AddPlaceholderChunk("filename");
  Builder.AddTextChunk("\"");
  Results.AddResult(Builder.TakeString());
  
  // #error <message>
  Builder.AddTypedTextChunk("error");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("message");
  Results.AddResult(Builder.TakeString());

  // #pragma <arguments>
  Builder.AddTypedTextChunk("pragma");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("arguments");
  Results.AddResult(Builder.TakeString());

  if (getLangOptions().ObjC1) {
    // #import "header"
    Builder.AddTypedTextChunk("import");
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddTextChunk("\"");
    Builder.AddPlaceholderChunk("header");
    Builder.AddTextChunk("\"");
    Results.AddResult(Builder.TakeString());
    
    // #import <header>
    Builder.AddTypedTextChunk("import");
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddTextChunk("<");
    Builder.AddPlaceholderChunk("header");
    Builder.AddTextChunk(">");
    Results.AddResult(Builder.TakeString());
  }
  
  // #include_next "header"
  Builder.AddTypedTextChunk("include_next");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddTextChunk("\"");
  Builder.AddPlaceholderChunk("header");
  Builder.AddTextChunk("\"");
  Results.AddResult(Builder.TakeString());
  
  // #include_next <header>
  Builder.AddTypedTextChunk("include_next");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddTextChunk("<");
  Builder.AddPlaceholderChunk("header");
  Builder.AddTextChunk(">");
  Results.AddResult(Builder.TakeString());

  // #warning <message>
  Builder.AddTypedTextChunk("warning");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("message");
  Results.AddResult(Builder.TakeString());

  // Note: #ident and #sccs are such crazy anachronisms that we don't provide
  // completions for them. And __include_macros is a Clang-internal extension
  // that we don't want to encourage anyone to use.

  // FIXME: we don't support #assert or #unassert, so don't suggest them.
  Results.ExitScope();
  
  HandleCodeCompleteResults(this, CodeCompleter, 
                            CodeCompletionContext::CCC_PreprocessorDirective,
                            Results.data(), Results.size());
}

void Sema::CodeCompleteInPreprocessorConditionalExclusion(Scope *S) {
  CodeCompleteOrdinaryName(S,
                           S->getFnParent()? Sema::PCC_RecoveryInFunction 
                                           : Sema::PCC_Namespace);
}

void Sema::CodeCompletePreprocessorMacroName(bool IsDefinition) {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        IsDefinition? CodeCompletionContext::CCC_MacroName
                                    : CodeCompletionContext::CCC_MacroNameUse);
  if (!IsDefinition && (!CodeCompleter || CodeCompleter->includeMacros())) {
    // Add just the names of macros, not their arguments.    
    CodeCompletionBuilder Builder(Results.getAllocator());
    Results.EnterNewScope();
    for (Preprocessor::macro_iterator M = PP.macro_begin(), 
                                   MEnd = PP.macro_end();
         M != MEnd; ++M) {
      Builder.AddTypedTextChunk(Builder.getAllocator().CopyString(
                                           M->first->getName()));
      Results.AddResult(Builder.TakeString());
    }
    Results.ExitScope();
  } else if (IsDefinition) {
    // FIXME: Can we detect when the user just wrote an include guard above?
  }
  
  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size()); 
}

void Sema::CodeCompletePreprocessorExpression() {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompletionContext::CCC_PreprocessorExpression);
  
  if (!CodeCompleter || CodeCompleter->includeMacros())
    AddMacroResults(PP, Results);
  
    // defined (<macro>)
  Results.EnterNewScope();
  CodeCompletionBuilder Builder(Results.getAllocator());
  Builder.AddTypedTextChunk("defined");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddChunk(CodeCompletionString::CK_LeftParen);
  Builder.AddPlaceholderChunk("macro");
  Builder.AddChunk(CodeCompletionString::CK_RightParen);
  Results.AddResult(Builder.TakeString());
  Results.ExitScope();
  
  HandleCodeCompleteResults(this, CodeCompleter, 
                            CodeCompletionContext::CCC_PreprocessorExpression,
                            Results.data(), Results.size()); 
}

void Sema::CodeCompletePreprocessorMacroArgument(Scope *S,
                                                 IdentifierInfo *Macro,
                                                 MacroInfo *MacroInfo,
                                                 unsigned Argument) {
  // FIXME: In the future, we could provide "overload" results, much like we
  // do for function calls.
  
  // Now just ignore this. There will be another code-completion callback
  // for the expanded tokens.
}

void Sema::CodeCompleteNaturalLanguage() {
  HandleCodeCompleteResults(this, CodeCompleter,
                            CodeCompletionContext::CCC_NaturalLanguage,
                            0, 0);
}

void Sema::GatherGlobalCodeCompletions(CodeCompletionAllocator &Allocator,
                 SmallVectorImpl<CodeCompletionResult> &Results) {
  ResultBuilder Builder(*this, Allocator, CodeCompletionContext::CCC_Recovery);
  if (!CodeCompleter || CodeCompleter->includeGlobals()) {
    CodeCompletionDeclConsumer Consumer(Builder, 
                                        Context.getTranslationUnitDecl());
    LookupVisibleDecls(Context.getTranslationUnitDecl(), LookupAnyName, 
                       Consumer);
  }
  
  if (!CodeCompleter || CodeCompleter->includeMacros())
    AddMacroResults(PP, Builder);
  
  Results.clear();
  Results.insert(Results.end(), 
                 Builder.data(), Builder.data() + Builder.size());
}
