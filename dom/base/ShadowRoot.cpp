/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Preferences.h"
#include "mozilla/dom/ShadowRoot.h"
#include "mozilla/dom/ShadowRootBinding.h"
#include "mozilla/dom/DocumentFragment.h"
#include "ChildIterator.h"
#include "nsContentUtils.h"
#include "nsDOMClassInfoID.h"
#include "nsIDOMHTMLElement.h"
#include "nsIStyleSheetLinkingElement.h"
#include "mozilla/dom/Element.h"
#include "mozilla/dom/HTMLContentElement.h"
#include "nsXBLPrototypeBinding.h"
#include "mozilla/StyleSheet.h"
#include "mozilla/StyleSheetInlines.h"

using namespace mozilla;
using namespace mozilla::dom;

NS_IMPL_CYCLE_COLLECTION_CLASS(ShadowRoot)

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(ShadowRoot,
                                                  DocumentFragment)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mStyleSheetList)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mAssociatedBinding)
  for (auto iter = tmp->mIdentifierMap.ConstIter(); !iter.Done();
       iter.Next()) {
    iter.Get()->Traverse(&cb);
  }
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(ShadowRoot)
  if (tmp->GetHost()) {
    tmp->GetHost()->RemoveMutationObserver(tmp);
  }
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mStyleSheetList)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mAssociatedBinding)
  tmp->mIdentifierMap.Clear();
NS_IMPL_CYCLE_COLLECTION_UNLINK_END_INHERITED(DocumentFragment)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(ShadowRoot)
  NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIContent)
  NS_INTERFACE_MAP_ENTRY(nsIMutationObserver)
NS_INTERFACE_MAP_END_INHERITING(DocumentFragment)

NS_IMPL_ADDREF_INHERITED(ShadowRoot, DocumentFragment)
NS_IMPL_RELEASE_INHERITED(ShadowRoot, DocumentFragment)

ShadowRoot::ShadowRoot(Element* aElement,
                       already_AddRefed<mozilla::dom::NodeInfo>&& aNodeInfo,
                       nsXBLPrototypeBinding* aProtoBinding)
  : DocumentFragment(aNodeInfo)
  , mProtoBinding(aProtoBinding)
  , mInsertionPointChanged(false)
  , mIsComposedDocParticipant(false)
{
  SetHost(aElement);

  // Nodes in a shadow tree should never store a value
  // in the subtree root pointer, nodes in the shadow tree
  // track the subtree root using GetContainingShadow().
  ClearSubtreeRootPointer();

  SetFlags(NODE_IS_IN_SHADOW_TREE);

  ExtendedDOMSlots()->mBindingParent = aElement;
  ExtendedDOMSlots()->mContainingShadow = this;

  // Add the ShadowRoot as a mutation observer on the host to watch
  // for mutations because the insertion points in this ShadowRoot
  // may need to be updated when the host children are modified.
  GetHost()->AddMutationObserver(this);
}

ShadowRoot::~ShadowRoot()
{
  if (GetHost()) {
    // mPoolHost may have been unlinked or a new ShadowRoot may have been
    // creating, making this one obsolete.
    GetHost()->RemoveMutationObserver(this);
  }

  UnsetFlags(NODE_IS_IN_SHADOW_TREE);

  // nsINode destructor expects mSubtreeRoot == this.
  SetSubtreeRootPointer(this);
}

JSObject*
ShadowRoot::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return mozilla::dom::ShadowRootBinding::Wrap(aCx, this, aGivenProto);
}

ShadowRoot*
ShadowRoot::FromNode(nsINode* aNode)
{
  if (aNode->IsInShadowTree() && !aNode->GetParentNode()) {
    MOZ_ASSERT(aNode->NodeType() == nsIDOMNode::DOCUMENT_FRAGMENT_NODE,
               "ShadowRoot is a document fragment.");
    return static_cast<ShadowRoot*>(aNode);
  }

  return nullptr;
}

void
ShadowRoot::StyleSheetChanged()
{
  mProtoBinding->FlushSkinSheets();

  nsIPresShell* shell = OwnerDoc()->GetShell();
  if (shell) {
    OwnerDoc()->BeginUpdate(UPDATE_STYLE);
    shell->RecordShadowStyleChange(this);
    OwnerDoc()->EndUpdate(UPDATE_STYLE);
  }
}

void
ShadowRoot::InsertSheet(StyleSheet* aSheet,
                        nsIContent* aLinkingContent)
{
  nsCOMPtr<nsIStyleSheetLinkingElement>
    linkingElement = do_QueryInterface(aLinkingContent);
  MOZ_ASSERT(linkingElement, "The only styles in a ShadowRoot should come "
                             "from <style>.");

  linkingElement->SetStyleSheet(aSheet); // This sets the ownerNode on the sheet

  // Find the correct position to insert into the style sheet list (must
  // be in tree order).
  for (size_t i = 0; i <= mProtoBinding->SheetCount(); i++) {
    if (i == mProtoBinding->SheetCount()) {
      mProtoBinding->AppendStyleSheet(aSheet);
      break;
    }

    nsINode* sheetOwningNode = mProtoBinding->StyleSheetAt(i)->GetOwnerNode();
    if (nsContentUtils::PositionIsBefore(aLinkingContent, sheetOwningNode)) {
      mProtoBinding->InsertStyleSheetAt(i, aSheet);
      break;
    }
  }

  if (aSheet->IsApplicable()) {
    StyleSheetChanged();
  }
}

void
ShadowRoot::RemoveSheet(StyleSheet* aSheet)
{
  mProtoBinding->RemoveStyleSheet(aSheet);

  if (aSheet->IsApplicable()) {
    StyleSheetChanged();
  }
}

Element*
ShadowRoot::GetElementById(const nsAString& aElementId)
{
  nsIdentifierMapEntry *entry = mIdentifierMap.GetEntry(aElementId);
  return entry ? entry->GetIdElement() : nullptr;
}

already_AddRefed<nsContentList>
ShadowRoot::GetElementsByTagName(const nsAString& aTagName)
{
  return NS_GetContentList(this, kNameSpaceID_Unknown, aTagName);
}

already_AddRefed<nsContentList>
ShadowRoot::GetElementsByTagNameNS(const nsAString& aNamespaceURI,
                                   const nsAString& aLocalName)
{
  int32_t nameSpaceId = kNameSpaceID_Wildcard;

  if (!aNamespaceURI.EqualsLiteral("*")) {
    nsresult rv =
      nsContentUtils::NameSpaceManager()->RegisterNameSpace(aNamespaceURI,
                                                            nameSpaceId);
    NS_ENSURE_SUCCESS(rv, nullptr);
  }

  NS_ASSERTION(nameSpaceId != kNameSpaceID_Unknown, "Unexpected namespace ID!");

  return NS_GetContentList(this, nameSpaceId, aLocalName);
}

void
ShadowRoot::AddToIdTable(Element* aElement, nsIAtom* aId)
{
  nsIdentifierMapEntry* entry = mIdentifierMap.PutEntry(aId);
  if (entry) {
    entry->AddIdElement(aElement);
  }
}

void
ShadowRoot::RemoveFromIdTable(Element* aElement, nsIAtom* aId)
{
  nsIdentifierMapEntry* entry = mIdentifierMap.GetEntry(aId);
  if (entry) {
    entry->RemoveIdElement(aElement);
    if (entry->IsEmpty()) {
      mIdentifierMap.RemoveEntry(entry);
    }
  }
}

already_AddRefed<nsContentList>
ShadowRoot::GetElementsByClassName(const nsAString& aClasses)
{
  return nsContentUtils::GetElementsByClassName(this, aClasses);
}

void
ShadowRoot::AddInsertionPoint(HTMLContentElement* aInsertionPoint)
{
  TreeOrderComparator comparator;
  mInsertionPoints.InsertElementSorted(aInsertionPoint, comparator);
}

void
ShadowRoot::RemoveInsertionPoint(HTMLContentElement* aInsertionPoint)
{
  mInsertionPoints.RemoveElement(aInsertionPoint);
}

void
ShadowRoot::RemoveDestInsertionPoint(nsIContent* aInsertionPoint,
                                     nsTArray<nsIContent*>& aDestInsertionPoints)
{
  // Remove the insertion point from the destination insertion points.
  // Also remove all succeeding insertion points because it is no longer
  // possible for the content to be distributed into deeper node trees.
  int32_t index = aDestInsertionPoints.IndexOf(aInsertionPoint);

  // It's possible that we already removed the insertion point while processing
  // other insertion point removals.
  if (index >= 0) {
    aDestInsertionPoints.SetLength(index);
  }
}

void
ShadowRoot::DistributeSingleNode(nsIContent* aContent)
{
  // Find the insertion point to which the content belongs.
  HTMLContentElement* insertionPoint = nullptr;
  for (uint32_t i = 0; i < mInsertionPoints.Length(); i++) {
    if (mInsertionPoints[i]->Match(aContent)) {
      if (mInsertionPoints[i]->MatchedNodes().Contains(aContent)) {
        // Node is already matched into the insertion point. We are done.
        return;
      }

      // Matching may cause the insertion point to drop fallback content.
      if (mInsertionPoints[i]->MatchedNodes().IsEmpty() &&
          static_cast<nsINode*>(mInsertionPoints[i])->GetFirstChild()) {
        // This match will cause the insertion point to drop all fallback
        // content and used matched nodes instead. Give up on the optimization
        // and just distribute all nodes.
        DistributeAllNodes();
        return;
      }
      insertionPoint = mInsertionPoints[i];
      break;
    }
  }

  // Find the index into the insertion point.
  if (insertionPoint) {
    nsCOMArray<nsIContent>& matchedNodes = insertionPoint->MatchedNodes();
    // Find the appropriate position in the matched node list for the
    // newly distributed content.
    bool isIndexFound = false;
    ExplicitChildIterator childIterator(GetHost());
    for (uint32_t i = 0; i < matchedNodes.Length(); i++) {
      // Seek through the host's explicit children until the inserted content
      // is found or when the current matched node is reached.
      if (childIterator.Seek(aContent, matchedNodes[i])) {
        // aContent was found before the current matched node.
        insertionPoint->InsertMatchedNode(i, aContent);
        isIndexFound = true;
        break;
      }
    }

    if (!isIndexFound) {
      // We have still not found an index in the insertion point,
      // thus it must be at the end.
      MOZ_ASSERT(childIterator.Seek(aContent, nullptr),
                 "Trying to match a node that is not a candidate to be matched");
      insertionPoint->AppendMatchedNode(aContent);
    }

    // Handle the case where the parent of the insertion point has a ShadowRoot.
    // The node distributed into the insertion point must be reprojected to the
    // insertion points of the parent's ShadowRoot.
    ShadowRoot* parentShadow = insertionPoint->GetParent()->GetShadowRoot();
    if (parentShadow) {
      parentShadow->DistributeSingleNode(aContent);
    }
  }
}

void
ShadowRoot::RemoveDistributedNode(nsIContent* aContent)
{
  // Find insertion point containing the content and remove the node.
  for (uint32_t i = 0; i < mInsertionPoints.Length(); i++) {
    if (mInsertionPoints[i]->MatchedNodes().Contains(aContent)) {
      // Removing the matched node may cause the insertion point to use
      // fallback content.
      if (mInsertionPoints[i]->MatchedNodes().Length() == 1 &&
          static_cast<nsINode*>(mInsertionPoints[i])->GetFirstChild()) {
        // Removing the matched node will cause fallback content to be
        // used instead. Give up optimization and distribute all nodes.
        DistributeAllNodes();
        return;
      }

      mInsertionPoints[i]->RemoveMatchedNode(aContent);

      // Handle the case where the parent of the insertion point has a ShadowRoot.
      // The removed node needs to be removed from the insertion points of the
      // parent's ShadowRoot.
      ShadowRoot* parentShadow = mInsertionPoints[i]->GetParent()->GetShadowRoot();
      if (parentShadow) {
        parentShadow->RemoveDistributedNode(aContent);
      }

      break;
    }
  }
}

void
ShadowRoot::DistributeAllNodes()
{
  // Create node pool.
  nsTArray<nsIContent*> nodePool;
  ExplicitChildIterator childIterator(GetHost());
  for (nsIContent* content = childIterator.GetNextChild(); content;
       content = childIterator.GetNextChild()) {
    nodePool.AppendElement(content);
  }

  nsTArray<ShadowRoot*> shadowsToUpdate;

  for (uint32_t i = 0; i < mInsertionPoints.Length(); i++) {
    mInsertionPoints[i]->ClearMatchedNodes();
    // Assign matching nodes from node pool.
    for (uint32_t j = 0; j < nodePool.Length(); j++) {
      if (mInsertionPoints[i]->Match(nodePool[j])) {
        mInsertionPoints[i]->AppendMatchedNode(nodePool[j]);
        nodePool.RemoveElementAt(j--);
      }
    }

    // Keep track of instances where the content insertion point is distributed
    // (parent of insertion point has a ShadowRoot).
    nsIContent* insertionParent = mInsertionPoints[i]->GetParent();
    MOZ_ASSERT(insertionParent, "The only way for an insertion point to be in the"
                                "mInsertionPoints array is to be a descendant of a"
                                "ShadowRoot, in which case, it should have a parent");

    // If the parent of the insertion point has a ShadowRoot, the nodes distributed
    // to the insertion point must be reprojected to the insertion points of the
    // parent's ShadowRoot.
    ShadowRoot* parentShadow = insertionParent->GetShadowRoot();
    if (parentShadow && !shadowsToUpdate.Contains(parentShadow)) {
      shadowsToUpdate.AppendElement(parentShadow);
    }
  }

  for (uint32_t i = 0; i < shadowsToUpdate.Length(); i++) {
    shadowsToUpdate[i]->DistributeAllNodes();
  }
}

void
ShadowRoot::GetInnerHTML(nsAString& aInnerHTML)
{
  GetMarkup(false, aInnerHTML);
}

void
ShadowRoot::SetInnerHTML(const nsAString& aInnerHTML, ErrorResult& aError)
{
  SetInnerHTMLInternal(aInnerHTML, aError);
}

Element*
ShadowRoot::Host()
{
  nsIContent* host = GetHost();
  MOZ_ASSERT(host && host->IsElement(),
             "ShadowRoot host should always be an element, "
             "how else did we create this ShadowRoot?");
  return host->AsElement();
}

bool
ShadowRoot::ApplyAuthorStyles()
{
  return mProtoBinding->InheritsStyle();
}

void
ShadowRoot::SetApplyAuthorStyles(bool aApplyAuthorStyles)
{
  mProtoBinding->SetInheritsStyle(aApplyAuthorStyles);

  nsIPresShell* shell = OwnerDoc()->GetShell();
  if (shell) {
    OwnerDoc()->BeginUpdate(UPDATE_STYLE);
    shell->RecordShadowStyleChange(this);
    OwnerDoc()->EndUpdate(UPDATE_STYLE);
  }
}

StyleSheetList*
ShadowRoot::StyleSheets()
{
  if (!mStyleSheetList) {
    mStyleSheetList = new ShadowRootStyleSheetList(this);
  }

  return mStyleSheetList;
}

/**
 * Returns whether the web components pool population algorithm
 * on the host would contain |aContent|. This function ignores
 * insertion points in the pool, thus should only be used to
 * test nodes that have not yet been distributed.
 */
bool
ShadowRoot::IsPooledNode(nsIContent* aContent, nsIContent* aContainer,
                         nsIContent* aHost)
{
  if (nsContentUtils::IsContentInsertionPoint(aContent)) {
    // Insertion points never end up in the pool.
    return false;
  }

  if (aContainer == aHost &&
      nsContentUtils::IsInSameAnonymousTree(aContainer, aContent)) {
    // Children of the host will end up in the pool. We check to ensure
    // that the content is in the same anonymous tree as the container
    // because anonymous content may report its container as the host
    // but it may not be in the host's child list.
    return true;
  }

  if (aContainer) {
    // Fallback content will end up in pool if its parent is a child of the host.
    HTMLContentElement* content = HTMLContentElement::FromContent(aContainer);
    return content && content->IsInsertionPoint() &&
           content->MatchedNodes().IsEmpty() &&
           aContainer->GetParentNode() == aHost;
  }

  return false;
}

void
ShadowRoot::AttributeChanged(nsIDocument* aDocument,
                             Element* aElement,
                             int32_t aNameSpaceID,
                             nsIAtom* aAttribute,
                             int32_t aModType,
                             const nsAttrValue* aOldValue)
{
  if (!IsPooledNode(aElement, aElement->GetParent(), GetHost())) {
    return;
  }

  // Attributes may change insertion point matching, find its new distribution.
  RemoveDistributedNode(aElement);
  DistributeSingleNode(aElement);
}

void
ShadowRoot::ContentAppended(nsIDocument* aDocument,
                            nsIContent* aContainer,
                            nsIContent* aFirstNewContent,
                            int32_t aNewIndexInContainer)
{
  if (mInsertionPointChanged) {
    DistributeAllNodes();
    mInsertionPointChanged = false;
    return;
  }

  // Watch for new nodes added to the pool because the node
  // may need to be added to an insertion point.
  nsIContent* currentChild = aFirstNewContent;
  while (currentChild) {
    // Add insertion point to destination insertion points of fallback content.
    if (nsContentUtils::IsContentInsertionPoint(aContainer)) {
      HTMLContentElement* content = HTMLContentElement::FromContent(aContainer);
      if (content->MatchedNodes().IsEmpty()) {
        currentChild->DestInsertionPoints().AppendElement(aContainer);
      }
    }

    if (IsPooledNode(currentChild, aContainer, GetHost())) {
      DistributeSingleNode(currentChild);
    }

    currentChild = currentChild->GetNextSibling();
  }
}

void
ShadowRoot::ContentInserted(nsIDocument* aDocument,
                            nsIContent* aContainer,
                            nsIContent* aChild,
                            int32_t aIndexInContainer)
{
  if (mInsertionPointChanged) {
    DistributeAllNodes();
    mInsertionPointChanged = false;
    return;
  }

  // Watch for new nodes added to the pool because the node
  // may need to be added to an insertion point.
  if (IsPooledNode(aChild, aContainer, GetHost())) {
    // Add insertion point to destination insertion points of fallback content.
    if (nsContentUtils::IsContentInsertionPoint(aContainer)) {
      HTMLContentElement* content = HTMLContentElement::FromContent(aContainer);
      if (content->MatchedNodes().IsEmpty()) {
        aChild->DestInsertionPoints().AppendElement(aContainer);
      }
    }

    DistributeSingleNode(aChild);
  }
}

void
ShadowRoot::ContentRemoved(nsIDocument* aDocument,
                           nsIContent* aContainer,
                           nsIContent* aChild,
                           int32_t aIndexInContainer,
                           nsIContent* aPreviousSibling)
{
  if (mInsertionPointChanged) {
    DistributeAllNodes();
    mInsertionPointChanged = false;
    return;
  }

  // Clear destination insertion points for removed
  // fallback content.
  if (nsContentUtils::IsContentInsertionPoint(aContainer)) {
    HTMLContentElement* content = HTMLContentElement::FromContent(aContainer);
    if (content->MatchedNodes().IsEmpty()) {
      aChild->DestInsertionPoints().Clear();
    }
  }

  // Watch for node that is removed from the pool because
  // it may need to be removed from an insertion point.
  if (IsPooledNode(aChild, aContainer, GetHost())) {
    RemoveDistributedNode(aChild);
  }
}

nsresult
ShadowRoot::Clone(mozilla::dom::NodeInfo *aNodeInfo, nsINode **aResult,
                  bool aPreallocateChildren) const
{
  *aResult = nullptr;
  return NS_ERROR_DOM_DATA_CLONE_ERR;
}

NS_IMPL_CYCLE_COLLECTION_INHERITED(ShadowRootStyleSheetList, StyleSheetList,
                                   mShadowRoot)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(ShadowRootStyleSheetList)
NS_INTERFACE_MAP_END_INHERITING(StyleSheetList)

NS_IMPL_ADDREF_INHERITED(ShadowRootStyleSheetList, StyleSheetList)
NS_IMPL_RELEASE_INHERITED(ShadowRootStyleSheetList, StyleSheetList)

ShadowRootStyleSheetList::ShadowRootStyleSheetList(ShadowRoot* aShadowRoot)
  : mShadowRoot(aShadowRoot)
{
}

ShadowRootStyleSheetList::~ShadowRootStyleSheetList()
{
}

StyleSheet*
ShadowRootStyleSheetList::IndexedGetter(uint32_t aIndex, bool& aFound)
{
  aFound = aIndex < mShadowRoot->mProtoBinding->SheetCount();
  if (!aFound) {
    return nullptr;
  }
  return mShadowRoot->mProtoBinding->StyleSheetAt(aIndex);
}

uint32_t
ShadowRootStyleSheetList::Length()
{
  return mShadowRoot->mProtoBinding->SheetCount();
}

