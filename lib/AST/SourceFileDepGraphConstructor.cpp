//===--- FineGrainedDependenciesSourceFileDepGraphConstructor.cpp ---------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include <stdio.h>

// may not all be needed
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/FileSystem.h"
#include "swift/AST/FineGrainedDependencies.h"
#include "swift/AST/Module.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/SourceFileDepGraphConstructor.h"
#include "swift/AST/Types.h"
#include "swift/Basic/FileSystem.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/ReferenceDependencyKeys.h"
#include "swift/Demangling/Demangle.h"
#include "swift/Frontend/FrontendOptions.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/YAMLParser.h"

// This file holds the code to build a SourceFileDepGraph in the frontend.
// This graph captures relationships between definitions and uses, and
// it is written to a file which is read by the driver in order to decide which
// source files require recompilation.

using namespace swift;
using namespace fine_grained_dependencies;

//==============================================================================
// MARK: Helpers for key construction that must be in frontend
//==============================================================================

template <typename DeclT> static std::string getBaseName(const DeclT *decl) {
  return decl->getBaseName().userFacingName();
}

template <typename DeclT> static std::string getName(const DeclT *decl) {
  return DeclBaseName(decl->getName()).userFacingName();
}

static std::string mangleTypeAsContext(const NominalTypeDecl *NTD) {
  Mangle::ASTMangler Mangler;
  return !NTD ? "" : Mangler.mangleTypeAsContextUSR(NTD);
}

//==============================================================================
// MARK: Privacy queries
//==============================================================================

static bool declIsPrivate(const ValueDecl *VD) {
  return VD->getFormalAccess() <= AccessLevel::FilePrivate;
}

/// Return true if \param D cannot affect other files.
static bool declIsPrivate(const Decl *D) {
  if (auto *VD = dyn_cast<ValueDecl>(D))
    return declIsPrivate(VD);
  switch (D->getKind()) {
  case DeclKind::Import:
  case DeclKind::PatternBinding:
  case DeclKind::EnumCase:
  case DeclKind::TopLevelCode:
  case DeclKind::IfConfig:
  case DeclKind::PoundDiagnostic:
    return true;

  case DeclKind::Extension:
  case DeclKind::InfixOperator:
  case DeclKind::PrefixOperator:
  case DeclKind::PostfixOperator:
    return false;

  default:
    llvm_unreachable("everything else is a ValueDecl");
  }
}

/// Return true if \ref ED does not contain a member that can affect other
/// files.
static bool allMembersArePrivate(const ExtensionDecl *ED) {
  return std::all_of(ED->getMembers().begin(), ED->getMembers().end(),
                     [](const Decl *d) { return declIsPrivate(d); });
  //                     declIsPrivate);
}

/// \ref inheritedType, an inherited protocol, return true if this inheritance
/// cannot affect other files.
static bool extendedTypeIsPrivate(TypeLoc inheritedType) {
  auto type = inheritedType.getType();
  if (!type)
    return true;

  if (!type->isExistentialType()) {
    // Be conservative. We don't know how to deal with other extended types.
    return false;
  }

  auto layout = type->getExistentialLayout();
  assert(!layout.explicitSuperclass &&
         "Should not have a subclass existential "
         "in the inheritance clause of an extension");
  for (auto protoTy : layout.getProtocols()) {
    if (!declIsPrivate(protoTy->getDecl()))
      return false;
  }

  return true;
}

/// Return true if \ref ED does not inherit a protocol that can affect other
/// files. Was called "justMembers" in ReferenceDependencies.cpp
/// \ref ED might be null.
static bool allInheritedProtocolsArePrivate(const ExtensionDecl *ED) {
  return std::all_of(ED->getInherited().begin(), ED->getInherited().end(),
                     extendedTypeIsPrivate);
}

//==============================================================================
// MARK: SourceFileDeclFinder
//==============================================================================

namespace {
/// Takes all the Decls in a SourceFile, and collects them into buckets by
/// groups of DeclKinds. Also casts them to more specific types
/// TODO: Factor with SourceFileDeclFinder
struct SourceFileDeclFinder {

public:
  /// Existing system excludes private decls in some cases.
  /// In the future, we might not want to do this, so use bool to decide.
  const bool includePrivateDecls;

  // The extracted Decls:
  ConstPtrVec<ExtensionDecl> extensions;
  ConstPtrVec<OperatorDecl> operators;
  ConstPtrVec<PrecedenceGroupDecl> precedenceGroups;
  ConstPtrVec<NominalTypeDecl> topNominals;
  ConstPtrVec<ValueDecl> topValues;
  ConstPtrVec<NominalTypeDecl> allNominals;
  ConstPtrVec<NominalTypeDecl> potentialMemberHolders;
  ConstPtrVec<FuncDecl> memberOperatorDecls;
  ConstPtrPairVec<NominalTypeDecl, ValueDecl> valuesInExtensions;
  ConstPtrVec<ValueDecl> classMembers;

  /// Construct me and separates the Decls.
  // clang-format off
    SourceFileDeclFinder(const SourceFile *const SF, const bool includePrivateDecls)
    : includePrivateDecls(includePrivateDecls) {
      for (const Decl *const D : SF->getTopLevelDecls()) {
        select<ExtensionDecl, DeclKind::Extension>(D, extensions, false) ||
        select<OperatorDecl, DeclKind::InfixOperator, DeclKind::PrefixOperator,
        DeclKind::PostfixOperator>(D, operators, false) ||
        select<PrecedenceGroupDecl, DeclKind::PrecedenceGroup>(
                                                               D, precedenceGroups, false) ||
        select<NominalTypeDecl, DeclKind::Enum, DeclKind::Struct,
        DeclKind::Class, DeclKind::Protocol>(D, topNominals, true) ||
        select<ValueDecl, DeclKind::TypeAlias, DeclKind::Var, DeclKind::Func,
        DeclKind::Accessor>(D, topValues, true);
      }
    // clang-format on
    // The order is important because some of these use instance variables
    // computed by others.
    findNominalsFromExtensions();
    findNominalsInTopNominals();
    findValuesInExtensions();
    findClassMembers(SF);
  }

private:
  /// Extensions may contain nominals and operators.
  void findNominalsFromExtensions() {
    for (auto *ED : extensions) {
      const auto *const NTD = ED->getExtendedNominal();
      if (NTD)
        findNominalsAndOperatorsIn(NTD, ED);
    }
  }
  /// Top-level nominals may contain nominals and operators.
  void findNominalsInTopNominals() {
    for (const auto *const NTD : topNominals)
      findNominalsAndOperatorsIn(NTD);
  }
  /// Any nominal may contain nominals and operators.
  /// (indirectly recursive)
  void findNominalsAndOperatorsIn(const NominalTypeDecl *const NTD,
                                  const ExtensionDecl *ED = nullptr) {
    if (excludeIfPrivate(NTD))
      return;
    const bool exposedProtocolIsExtended =
        ED && !allInheritedProtocolsArePrivate(ED);
    if (ED && !includePrivateDecls && !exposedProtocolIsExtended &&
        std::all_of(ED->getMembers().begin(), ED->getMembers().end(),
                    [&](const Decl *D) { return declIsPrivate(D); })) {
      return;
    }
    if (includePrivateDecls || !ED || exposedProtocolIsExtended)
      allNominals.push_back(NTD);
    potentialMemberHolders.push_back(NTD);
    findNominalsAndOperatorsInMembers(ED ? ED->getMembers()
                                         : NTD->getMembers());
  }

  /// Search through the members to find nominals and operators.
  /// (indirectly recursive)
  /// TODO: clean this up, maybe recurse separately for each purpose.
  void findNominalsAndOperatorsInMembers(const DeclRange members) {
    for (const Decl *const D : members) {
      auto *VD = dyn_cast<ValueDecl>(D);
      if (!VD || excludeIfPrivate(VD))
        continue;
      if (VD->getFullName().isOperator())
        memberOperatorDecls.push_back(cast<FuncDecl>(D));
      else if (const auto *const NTD = dyn_cast<NominalTypeDecl>(D))
        findNominalsAndOperatorsIn(NTD);
    }
  }

  /// Extensions may contain ValueDecls.
  void findValuesInExtensions() {
    for (const auto *ED : extensions) {
      const auto *const NTD = ED->getExtendedNominal();
      if (!NTD || excludeIfPrivate(NTD))
        continue;
      if (!includePrivateDecls &&
          (!allInheritedProtocolsArePrivate(ED) || allMembersArePrivate(ED)))
        continue;
      for (const auto *member : ED->getMembers())
        if (const auto *VD = dyn_cast<ValueDecl>(member))
          if (VD->hasName() && (includePrivateDecls || !declIsPrivate(VD))) {
            const auto *const NTD = ED->getExtendedNominal();
            if (NTD)
              valuesInExtensions.push_back(std::make_pair(NTD, VD));
          }
    }
  }

  /// Class members are needed for dynamic lookup dependency nodes.
  void findClassMembers(const SourceFile *const SF) {
    struct Collector : public VisibleDeclConsumer {
      ConstPtrVec<ValueDecl> &classMembers;
      Collector(ConstPtrVec<ValueDecl> &classMembers)
          : classMembers(classMembers) {}
      void foundDecl(ValueDecl *VD, DeclVisibilityKind,
                     DynamicLookupInfo) override {
        classMembers.push_back(VD);
      }
    } collector{classMembers};
    SF->lookupClassMembers({}, collector);
  }

  /// Check \p D to see if it is one of the DeclKinds in the template
  /// arguments. If so, cast it to DesiredDeclType and add it to foundDecls.
  /// \returns true if successful.
  template <typename DesiredDeclType, DeclKind firstKind,
            DeclKind... restOfKinds>
  bool select(const Decl *const D, ConstPtrVec<DesiredDeclType> &foundDecls,
              const bool canExcludePrivateDecls) {
    if (D->getKind() == firstKind) {
      auto *dd = cast<DesiredDeclType>(D);
      const bool exclude = canExcludePrivateDecls && excludeIfPrivate(dd);
      if (!exclude)
        foundDecls.push_back(cast<DesiredDeclType>(D));
      return true;
    }
    return select<DesiredDeclType, restOfKinds...>(D, foundDecls,
                                                   canExcludePrivateDecls);
  }

  /// Terminate the template recursion.
  template <typename DesiredDeclType>
  bool select(const Decl *const D, ConstPtrVec<DesiredDeclType> &foundDecls,
              bool) {
    return false;
  }

  /// Return true if \param D should be excluded on privacy grounds.
  bool excludeIfPrivate(const Decl *const D) {
    return !includePrivateDecls && declIsPrivate(D);
  }
};
} // namespace

template <NodeKind kindArg, typename Entity>
DependencyKey DependencyKey::createForProvidedEntityInterface(Entity entity) {
  return DependencyKey(
      kindArg, DeclAspect::interface,
      DependencyKey::computeContextForProvidedEntity<kindArg>(entity),
      DependencyKey::computeNameForProvidedEntity<kindArg>(entity));
}

//==============================================================================
// MARK: computeContextForProvidedEntity
//==============================================================================

template <NodeKind kind, typename Entity>
std::string DependencyKey::computeContextForProvidedEntity(Entity) {
  // Context field is not used for most kinds
  return "";
}

// \ref nominal dependencies are created from a Decl and use the context field.
template <>
std::string DependencyKey::computeContextForProvidedEntity<
    NodeKind::nominal, NominalTypeDecl const *>(NominalTypeDecl const *D) {
  return mangleTypeAsContext(D);
}

/// \ref potentialMember dependencies are created from a Decl and use the
/// context field.
template <>
std::string
DependencyKey::computeContextForProvidedEntity<NodeKind::potentialMember,
                                               NominalTypeDecl const *>(
    const NominalTypeDecl *D) {
  return mangleTypeAsContext(D);
}

template <>
std::string DependencyKey::computeContextForProvidedEntity<
    NodeKind::member, const NominalTypeDecl *>(const NominalTypeDecl *holder) {
  return mangleTypeAsContext(holder);
}

/// \ref member dependencies are created from a pair and use the context field.
template <>
std::string DependencyKey::computeContextForProvidedEntity<
    NodeKind::member, std::pair<const NominalTypeDecl *, const ValueDecl *>>(
    std::pair<const NominalTypeDecl *, const ValueDecl *> holderAndMember) {
  return computeContextForProvidedEntity<NodeKind::member>(
      holderAndMember.first);
}

// Linux compiler requires the following:
template
std::string
DependencyKey::computeContextForProvidedEntity<NodeKind::sourceFileProvide,
                                               StringRef>(StringRef);

//==============================================================================
// MARK: computeNameForProvidedEntity
//==============================================================================

template <>
std::string
DependencyKey::computeNameForProvidedEntity<NodeKind::sourceFileProvide,
                                            StringRef>(StringRef swiftDeps) {
  assert(!swiftDeps.empty());
  return swiftDeps;
}

template <>
std::string
DependencyKey::computeNameForProvidedEntity<NodeKind::topLevel,
                                            PrecedenceGroupDecl const *>(
    const PrecedenceGroupDecl *D) {
  return ::getName(D);
}
template <>
std::string DependencyKey::computeNameForProvidedEntity<
    NodeKind::topLevel, FuncDecl const *>(const FuncDecl *D) {
  return ::getName(D);
}
template <>
std::string DependencyKey::computeNameForProvidedEntity<
    NodeKind::topLevel, OperatorDecl const *>(const OperatorDecl *D) {
  return ::getName(D);
}
template <>
std::string DependencyKey::computeNameForProvidedEntity<
    NodeKind::topLevel, NominalTypeDecl const *>(const NominalTypeDecl *D) {
  return ::getName(D);
}
template <>
std::string DependencyKey::computeNameForProvidedEntity<
    NodeKind::topLevel, ValueDecl const *>(const ValueDecl *D) {
  return getBaseName(D);
}
template <>
std::string DependencyKey::computeNameForProvidedEntity<
    NodeKind::dynamicLookup, ValueDecl const *>(const ValueDecl *D) {
  return getBaseName(D);
}
template <>
std::string DependencyKey::computeNameForProvidedEntity<
    NodeKind::nominal, NominalTypeDecl const *>(const NominalTypeDecl *D) {
  return "";
}
template <>
std::string
DependencyKey::computeNameForProvidedEntity<NodeKind::potentialMember,
                                            NominalTypeDecl const *>(
    const NominalTypeDecl *D) {
  return "";
}

template <>
std::string DependencyKey::computeNameForProvidedEntity<
    NodeKind::member, std::pair<const NominalTypeDecl *, const ValueDecl *>>(
    std::pair<const NominalTypeDecl *, const ValueDecl *> holderAndMember) {
  return getBaseName(holderAndMember.second);
}

//==============================================================================
// MARK: createDependedUponKey
//==============================================================================

template <>
DependencyKey
DependencyKey::createDependedUponKey<NodeKind::topLevel>(StringRef name) {
  return DependencyKey(NodeKind::topLevel, DeclAspect::interface, "", name);
}

template <>
DependencyKey
DependencyKey::createDependedUponKey<NodeKind::dynamicLookup>(StringRef name) {
  return DependencyKey(NodeKind::dynamicLookup, DeclAspect::interface, "",
                       name);
}

template <>
DependencyKey
DependencyKey::createDependedUponKey<NodeKind::externalDepend>(StringRef name) {
  return DependencyKey(NodeKind::externalDepend, DeclAspect::interface, "",
                       name);
}

template <>
DependencyKey
DependencyKey::createDependedUponKey<NodeKind::nominal>(StringRef mangledName) {
  return DependencyKey(NodeKind::nominal, DeclAspect::interface, mangledName,
                       "");
}

DependencyKey DependencyKey::createDependedUponKey(StringRef mangledHolderName,
                                                   StringRef memberBaseName) {
  const bool isMemberBlank = memberBaseName.empty();
  const auto kind =
      isMemberBlank ? NodeKind::potentialMember : NodeKind::member;
  return DependencyKey(kind, DeclAspect::interface, mangledHolderName,
                       isMemberBlank ? "" : memberBaseName);
}

//==============================================================================
// MARK: SourceFileDepGraphConstructor
//==============================================================================


//==============================================================================
// MARK: SourceFileDepGraphConstructor: Adding nodes to the graph
//==============================================================================

SourceFileDepGraph SourceFileDepGraphConstructor::addSourceFileNodesAndThen(
    StringRef name, StringRef fingerprint, function_ref<void()> doTheRest) {
  // Order matters here, each function adds state used by the next one.
  addSourceFileNodesToGraph(name, fingerprint);
  if (!hadCompilationError)
    doTheRest();
  assert(g.verify());
  return std::move(g);
}

/// Centralize the invariant that the fingerprint of the whole file is the
/// interface hash
std::string SourceFileDepGraphConstructor::getFingerprint(SourceFile *SF) {
  return getInterfaceHash(SF);
}

/// At present, only nominals, protocols, and extensions have (body)
/// fingerprints
Optional<std::string> SourceFileDepGraphConstructor::getFingerprintIfAny(
    std::pair<const NominalTypeDecl *, const ValueDecl *>) {
  return None;
}
Optional<std::string>
SourceFileDepGraphConstructor::getFingerprintIfAny(const Decl *d) {
  if (const auto *idc = dyn_cast<IterableDeclContext>(d)) {
    auto result = idc->getBodyFingerprint();
    assert((!result || !result->empty()) && "Fingerprint should never be empty");
    return result;
  }
  return None;
}

std::string SourceFileDepGraphConstructor::getInterfaceHash(SourceFile *SF) {
  llvm::SmallString<32> interfaceHash;
  SF->getInterfaceHash(interfaceHash);
  return interfaceHash.str().str();
}

void SourceFileDepGraphConstructor::addSourceFileNodesToGraph(
    StringRef swiftDeps, StringRef fingerprint) {
  g.findExistingNodePairOrCreateAndAddIfNew(
      DependencyKey::createKeyForWholeSourceFile(DeclAspect::interface,
                                                 swiftDeps),
      fingerprint);
}

void SourceFileDepGraphConstructor::enumerateDefinedDecls(
    SourceFile *SF, AddDefinedDecl addDefinedDeclFn) {
  // TODO: express the multiple provides and depends streams with variadic
  // templates

  // Many kinds of Decls become top-level depends.

  SourceFileDeclFinder declFinder(SF, includePrivateDeps);

  enumerateAllProviderNodesOfAGivenType<NodeKind::topLevel>(
      declFinder.precedenceGroups, addDefinedDeclFn);
  enumerateAllProviderNodesOfAGivenType<NodeKind::topLevel>(
      declFinder.memberOperatorDecls, addDefinedDeclFn);
  enumerateAllProviderNodesOfAGivenType<NodeKind::topLevel>(
      declFinder.operators, addDefinedDeclFn);
  enumerateAllProviderNodesOfAGivenType<NodeKind::topLevel>(
      declFinder.topNominals, addDefinedDeclFn);
  enumerateAllProviderNodesOfAGivenType<NodeKind::topLevel>(
      declFinder.topValues, addDefinedDeclFn);
  enumerateAllProviderNodesOfAGivenType<NodeKind::nominal>(
      declFinder.allNominals, addDefinedDeclFn);
  enumerateAllProviderNodesOfAGivenType<NodeKind::potentialMember>(
      declFinder.potentialMemberHolders, addDefinedDeclFn);
  enumerateAllProviderNodesOfAGivenType<NodeKind::member>(
      declFinder.valuesInExtensions, addDefinedDeclFn);
  enumerateAllProviderNodesOfAGivenType<NodeKind::dynamicLookup>(
      declFinder.classMembers, addDefinedDeclFn);
}

/// Given an array of Decls or pairs of them in \p declsOrPairs
/// create node pairs for context and name
template <NodeKind kind, typename ContentsT>
void SourceFileDepGraphConstructor::enumerateAllProviderNodesOfAGivenType(
    std::vector<ContentsT> &contentsVec, AddDefinedDecl addDefinedDeclFn) {
  for (const auto declOrPair : contentsVec) {
    Optional<std::string> fp = getFingerprintIfAny(declOrPair);
    addDefinedDeclFn(
        DependencyKey::createForProvidedEntityInterface<kind>(declOrPair),
        fp ? StringRef(fp.getValue()) : Optional<StringRef>());
  }
}

void SourceFileDepGraphConstructor::addDefinedDecl(
    const DependencyKey &interfaceKey, Optional<StringRef> fingerprint) {

  auto nodePair =
      g.findExistingNodePairOrCreateAndAddIfNew(interfaceKey, fingerprint);
  // Since the current type fingerprints only include tokens in the body,
  // when the interface hash changes, it is possible that the type in the
  // file has changed.
  g.addArc(g.getSourceFileNodePair().getInterface(), nodePair.getInterface());
}

//==============================================================================
// Entry point from the Frontend to this whole system
//==============================================================================

namespace {
/// Extracts uses out of a SourceFile
class UsedDeclEnumerator {
  SourceFile *SF;
  const DependencyTracker &depTracker;
  StringRef swiftDeps;

  /// Cache these for efficiency
  const DependencyKey sourceFileInterface;
  const DependencyKey sourceFileImplementation;

  function_ref<void(const DependencyKey &, const DependencyKey &)> createUseDef;

  const bool includeIntrafileDeps;

public:
  UsedDeclEnumerator(
      SourceFile *SF, const DependencyTracker &depTracker, StringRef swiftDeps,
      function_ref<void(const DependencyKey &, const DependencyKey &)>
          createUseDef,
      bool includeIntrafileDeps)
      : SF(SF), depTracker(depTracker), swiftDeps(swiftDeps),
        sourceFileInterface(DependencyKey::createKeyForWholeSourceFile(
            DeclAspect::interface, swiftDeps)),
        sourceFileImplementation(DependencyKey::createKeyForWholeSourceFile(
            DeclAspect::implementation, swiftDeps)),
        createUseDef(createUseDef), includeIntrafileDeps(includeIntrafileDeps) {
  }

public:
  void enumerateAllUses() {
    enumerateSimpleUses<NodeKind::topLevel>(
        SF->getReferencedNameTracker()->getTopLevelNames());
    enumerateSimpleUses<NodeKind::dynamicLookup>(
        SF->getReferencedNameTracker()->getDynamicLookupNames());
    enumerateExternalUses();
    enumerateCompoundUses();
  }

private:
  void enumerateUse(NodeKind kind, StringRef context, StringRef name,
                    bool isCascadingUse) {
    // Assume that what is depended-upon is the interface
    createUseDef(DependencyKey(kind, DeclAspect::interface, context, name),
                 isCascadingUse ? sourceFileInterface
                                : sourceFileImplementation);
  }
  template <NodeKind kind>
  void enumerateSimpleUses(llvm::DenseMap<DeclBaseName, bool> cascadesByName) {
    for (const auto &p : cascadesByName)
      enumerateUse(kind, "", p.getFirst().userFacingName(), p.getSecond());
  }

  void enumerateCompoundUses() {
    enumerateNominalUses(std::move(computeHoldersOfCascadingMembers()));
    enumerateMemberUses();
  }

  llvm::StringSet<> computeHoldersOfCascadingMembers() {
    llvm::StringSet<> holdersOfCascadingMembers;
    for (const auto &p : SF->getReferencedNameTracker()->getUsedMembers()) {
      {
        bool isPrivate = declIsPrivate(p.getFirst().first);
        if (isPrivate && !includeIntrafileDeps)
          continue;
      }
      StringRef context =
          DependencyKey::computeContextForProvidedEntity<NodeKind::nominal>(
              p.getFirst().first);
      bool isCascading = p.getSecond();
      if (isCascading)
        holdersOfCascadingMembers.insert(context);
    }
    return holdersOfCascadingMembers;
  }

  void
  enumerateNominalUses(const llvm::StringSet<> &&holdersOfCascadingMembers) {
    for (const auto &p : SF->getReferencedNameTracker()->getUsedMembers()) {
      {
        bool isPrivate = declIsPrivate(p.getFirst().first);
        if (isPrivate && !includeIntrafileDeps)
          continue;
      }
      const NominalTypeDecl *nominal = p.getFirst().first;

      StringRef context =
          DependencyKey::computeContextForProvidedEntity<NodeKind::nominal>(
              nominal);
      const bool isCascadingUse = holdersOfCascadingMembers.count(context) != 0;
      enumerateUse(NodeKind::nominal, context, "", isCascadingUse);
    }
  }

  void enumerateMemberUses() {
    for (const auto &p : SF->getReferencedNameTracker()->getUsedMembers()) {
      const NominalTypeDecl *nominal = p.getFirst().first;
      const auto rawName = p.getFirst().second;
      const bool isPotentialMember = rawName.empty();
      const bool isCascadingUse = p.getSecond();
      if (isPotentialMember) {
        StringRef context = DependencyKey::computeContextForProvidedEntity<
            NodeKind::potentialMember>(nominal);
        enumerateUse(NodeKind::potentialMember, context, "", isCascadingUse);
      } else {
        StringRef context =
            DependencyKey::computeContextForProvidedEntity<NodeKind::member>(
                nominal);
        StringRef name = rawName.userFacingName();
        enumerateUse(NodeKind::member, context, name, isCascadingUse);
      }
    }
  }

  void enumerateExternalUses() {
    // external dependencies always cascade
    for (StringRef s : depTracker.getDependencies())
      enumerateUse(NodeKind::externalDepend, "", s, true);
  }
};
} // end namespace

bool fine_grained_dependencies::emitReferenceDependencies(
    DiagnosticEngine &diags, SourceFile *const SF,
    const DependencyTracker &depTracker, StringRef outputPath,
    const bool alsoEmitDotFile) {

  // Before writing to the dependencies file path, preserve any previous file
  // that may have been there. No error handling -- this is just a nicety, it
  // doesn't matter if it fails.
  llvm::sys::fs::rename(outputPath, outputPath + "~");
  // Since, when fingerprints are enabled,
  // the parser diverts token hashing into per-body fingerprints
  // before it can know if a difference is in a private type,
  // in order to be able to test the changed  fingerprints
  // we force the inclusion of private declarations when fingerprints
  // are enabled.
  const bool includeIntrafileDeps =
      SF->getASTContext()
          .LangOpts.FineGrainedDependenciesIncludeIntrafileOnes ||
      SF->getASTContext().LangOpts.EnableTypeFingerprints;
  const bool hadCompilationError = SF->getASTContext().hadError();
  auto gc =
      SourceFileDepGraphConstructor(includeIntrafileDeps, hadCompilationError);

  auto forEachDefinedDecl =
      [&](SourceFileDepGraphConstructor::AddDefinedDecl addDefinedDeclFn) {
        gc.enumerateDefinedDecls(SF, addDefinedDeclFn);
      };

  auto forEachUsedDecl =
      [&](function_ref<void(const DependencyKey &, const DependencyKey &)>
              createDefUse) {
        UsedDeclEnumerator(SF, depTracker, outputPath, createDefUse,
                           includeIntrafileDeps)
            .enumerateAllUses();
      };

  SourceFileDepGraph g = gc.construct(outputPath, gc.getFingerprint(SF),
                                      forEachDefinedDecl, forEachUsedDecl);

  const bool hadError =
      withOutputFile(diags, outputPath, [&](llvm::raw_pwrite_stream &out) {
        out << g.yamlProlog(hadCompilationError);
        llvm::yaml::Output yamlWriter(out);
        yamlWriter << g;
        return false;
      });

  // If path is stdout, cannot read it back, so check for "-"
  assert(outputPath == "-" || g.verifyReadsWhatIsWritten(outputPath));

  if (alsoEmitDotFile)
    g.emitDotFile(outputPath, diags);

  return hadError;
}

  //==============================================================================
  // For the unit tests
  //==============================================================================

#warning dmu kmove out of this mark
void SourceFileDepGraphConstructor::addAllDefinedDecls(
    ForEachDefinedDecl forEachDefinedDecl) {
  forEachDefinedDecl(
      [&](const DependencyKey &interfaceKey, Optional<StringRef> fingerprint) {
        addDefinedDecl(interfaceKey, fingerprint);
      });
}

void SourceFileDepGraphConstructor::addAllUsedDecls(
    ForEachUsedDecl forEachUsedDecl) {
  forEachUsedDecl([&](const DependencyKey &defKey,
                      const DependencyKey &useKey) {
    auto *defNode = g.findExistingNodeOrCreateIfNew(defKey, None,
                                                    false /* = !isProvides */);
    auto nullableUse = g.findExistingNode(useKey);
    assert(nullableUse.isNonNull() && "Use must be an already-added provides");
    auto *useNode = nullableUse.get();
    assert(useNode->getIsProvides() && "Use (using node) must be a provides");
    g.addArc(defNode, useNode);
  });
}
