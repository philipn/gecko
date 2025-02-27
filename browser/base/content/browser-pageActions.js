/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

var BrowserPageActions = {
  /**
   * The main page action button in the urlbar (DOM node)
   */
  get mainButtonNode() {
    delete this.mainButtonNode;
    return this.mainButtonNode = document.getElementById("pageActionButton");
  },

  /**
   * The main page action panel DOM node (DOM node)
   */
  get panelNode() {
    delete this.panelNode;
    return this.panelNode = document.getElementById("pageActionPanel");
  },

  /**
   * The photonmultiview node in the main page action panel (DOM node)
   */
  get multiViewNode() {
    delete this.multiViewNode;
    return this.multiViewNode = document.getElementById("pageActionPanelMultiView");
  },

  /**
   * The main panelview node in the main page action panel (DOM node)
   */
  get mainViewNode() {
    delete this.mainViewNode;
    return this.mainViewNode = document.getElementById("pageActionPanelMainView");
  },

  /**
   * The vbox body node in the main panelview node (DOM node)
   */
  get mainViewBodyNode() {
    delete this.mainViewBodyNode;
    return this.mainViewBodyNode = this.mainViewNode.querySelector(".panel-subview-body");
  },

  /**
   * Inits.  Call to init.
   */
  init() {
    this.placeAllActions();
  },

  /**
   * Places all registered actions.
   */
  placeAllActions() {
    // Place actions in the panel.  Notify of onBeforePlacedInWindow too.
    for (let action of PageActions.actions) {
      action.onBeforePlacedInWindow(window);
      this.placeActionInPanel(action);
    }

    // Place actions in the urlbar.  Do this in reverse order.  The reason is
    // subtle.  If there were no urlbar nodes already in markup (like the
    // bookmark star button), then doing this in forward order would be fine.
    // Forward order means that the insert-before relationship is always broken:
    // there's never a next-sibling node before which to insert a new node, so
    // node.insertBefore() is always passed null, and nodes are always appended.
    // That will break the position of nodes that should be inserted before
    // nodes that are in markup, which in turn can break other nodes.
    let actionsInUrlbar = PageActions.actionsInUrlbar;
    for (let i = actionsInUrlbar.length - 1; i >= 0; i--) {
      let action = actionsInUrlbar[i];
      this.placeActionInUrlbar(action);
    }
  },

  /**
   * Adds or removes as necessary DOM nodes for the given action.
   *
   * @param  action (PageActions.Action, required)
   *         The action to place.
   */
  placeAction(action) {
    action.onBeforePlacedInWindow(window);
    this.placeActionInPanel(action);
    this.placeActionInUrlbar(action);
  },

  /**
   * Adds or removes as necessary DOM nodes for the action in the panel.
   *
   * @param  action (PageActions.Action, required)
   *         The action to place.
   */
  placeActionInPanel(action) {
    let insertBeforeID = PageActions.nextActionIDInPanel(action);
    let id = this._panelButtonNodeIDForActionID(action.id);
    let node = document.getElementById(id);
    if (!node) {
      let panelViewNode;
      [node, panelViewNode] = this._makePanelButtonNodeForAction(action);
      node.id = id;
      let insertBeforeNode = null;
      if (insertBeforeID) {
        let insertBeforeNodeID =
          this._panelButtonNodeIDForActionID(insertBeforeID);
        insertBeforeNode = document.getElementById(insertBeforeNodeID);
      }
      this.mainViewBodyNode.insertBefore(node, insertBeforeNode);
      action.onPlacedInPanel(node);
      if (panelViewNode) {
        action.subview.onPlaced(panelViewNode);
      }
    }
  },

  _makePanelButtonNodeForAction(action) {
    if (action.__isSeparator) {
      let node = document.createElement("toolbarseparator");
      return [node, null];
    }

    let buttonNode = document.createElement("toolbarbutton");
    buttonNode.classList.add(
      "subviewbutton",
      "subviewbutton-iconic",
      "pageAction-panel-button"
    );
    buttonNode.setAttribute("label", action.title);
    if (action.iconURL) {
      buttonNode.style.listStyleImage = `url('${action.iconURL}')`;
    }
    if (action.nodeAttributes) {
      for (let name in action.nodeAttributes) {
        buttonNode.setAttribute(name, action.nodeAttributes[name]);
      }
    }
    let panelViewNode = null;
    if (action.subview) {
      buttonNode.classList.add("subviewbutton-nav");
      panelViewNode = this._makePanelViewNodeForAction(action, false);
      this.multiViewNode._panelViews = null;
      this.multiViewNode.appendChild(panelViewNode);
    }
    buttonNode.addEventListener("command", event => {
      this.doCommandForAction(action, event, buttonNode);
    });
    return [buttonNode, panelViewNode];
  },

  _makePanelViewNodeForAction(action, forUrlbar) {
    let panelViewNode = document.createElement("panelview");
    panelViewNode.id = this._panelViewNodeIDForActionID(action.id, forUrlbar);
    panelViewNode.classList.add("PanelUI-subView");
    let bodyNode = document.createElement("vbox");
    bodyNode.id = panelViewNode.id + "-body";
    bodyNode.classList.add("panel-subview-body");
    panelViewNode.appendChild(bodyNode);
    for (let button of action.subview.buttons) {
      let buttonNode = document.createElement("toolbarbutton");
      buttonNode.id =
        this._panelViewButtonNodeIDForActionID(action.id, button.id, forUrlbar);
      buttonNode.classList.add("subviewbutton", "subviewbutton-iconic");
      buttonNode.setAttribute("label", button.title);
      if (button.shortcut) {
        buttonNode.setAttribute("shortcut", button.shortcut);
      }
      if (button.disabled) {
        buttonNode.setAttribute("disabled", "true");
      }
      buttonNode.addEventListener("command", event => {
        button.onCommand(event, buttonNode);
      });
      bodyNode.appendChild(buttonNode);
    }
    return panelViewNode;
  },

  _toggleActivatedActionPanelForAction(action) {
    let panelNode = this.activatedActionPanelNode;
    if (panelNode) {
      panelNode.hidePopup();
      return null;
    }

    // Before creating the panel, get the anchor node for it because it'll throw
    // if there isn't one (which shouldn't happen, but still).
    let anchorNode = this.panelAnchorNodeForAction(action);

    panelNode = document.createElement("panel");
    panelNode.id = this._activatedActionPanelID;
    panelNode.classList.add("cui-widget-panel");
    panelNode.setAttribute("actionID", action.id);
    panelNode.setAttribute("role", "group");
    panelNode.setAttribute("type", "arrow");
    panelNode.setAttribute("flip", "slide");
    panelNode.setAttribute("noautofocus", "true");
    panelNode.setAttribute("tabspecific", "true");
    panelNode.setAttribute("photon", "true");

    // For tests.
    if (this._disableActivatedActionPanelAnimation) {
      panelNode.setAttribute("animate", "false");
    }

    let panelViewNode = null;
    let iframeNode = null;

    if (action.subview) {
      let multiViewNode = document.createElement("photonpanelmultiview");
      panelViewNode = this._makePanelViewNodeForAction(action, true);
      multiViewNode.appendChild(panelViewNode);
      panelNode.appendChild(multiViewNode);
    } else if (action.wantsIframe) {
      iframeNode = document.createElement("iframe");
      iframeNode.setAttribute("type", "content");
      panelNode.appendChild(iframeNode);
    }

    let popupSet = document.getElementById("mainPopupSet");
    popupSet.appendChild(panelNode);
    panelNode.addEventListener("popuphidden", () => {
      if (iframeNode) {
        action.onIframeHidden(iframeNode, panelNode);
      }
      panelNode.remove();
    }, { once: true });

    panelNode.addEventListener("popuphiding", () => {
      if (iframeNode) {
        action.onIframeHiding(iframeNode, panelNode);
      }
      anchorNode.removeAttribute("open");
    }, { once: true });

    if (panelViewNode) {
      action.subview.onPlaced(panelViewNode);
      action.subview.onShowing(panelViewNode);
    }

    // Hide the main page action panel before showing the activated-action
    // panel.
    this.panelNode.hidePopup();
    panelNode.openPopup(anchorNode, "bottomcenter topright");
    anchorNode.setAttribute("open", "true");

    if (iframeNode) {
      action.onIframeShown(iframeNode, panelNode);
    }

    return panelNode;
  },

  /**
   * Returns the node in the urlbar to which popups for the given action should
   * be anchored.  If the action is null, a sensible anchor is returned.
   *
   * @param  action (PageActions.Action, optional)
   *         The action you want to anchor.
   * @return (DOM node, nonnull) The node to which the action should be
   *         anchored.
   */
  panelAnchorNodeForAction(action) {
    // Try each of the following nodes in order, using the first that's visible.
    let potentialAnchorNodeIDs = [
      action && action.anchorIDOverride,
      action && this._urlbarButtonNodeIDForActionID(action.id),
      this.mainButtonNode.id,
      "identity-icon",
    ];
    let dwu = window.QueryInterface(Ci.nsIInterfaceRequestor)
                    .getInterface(Ci.nsIDOMWindowUtils);
    for (let id of potentialAnchorNodeIDs) {
      if (id) {
        let node = document.getElementById(id);
        if (node && !node.hidden) {
          let bounds = dwu.getBoundsWithoutFlushing(node);
          if (bounds.height > 0 && bounds.width > 0) {
            return node;
          }
        }
      }
    }
    let id = action ? action.id : "<no action>";
    throw new Error(`PageActions: No anchor node for ${id}`);
  },

  get activatedActionPanelNode() {
    return document.getElementById(this._activatedActionPanelID);
  },

  get _activatedActionPanelID() {
    return "pageActionActivatedActionPanel";
  },

  /**
   * Adds or removes as necessary a DOM node for the given action in the urlbar.
   *
   * @param  action (PageActions.Action, required)
   *         The action to place.
   */
  placeActionInUrlbar(action) {
    let insertBeforeID = PageActions.nextActionIDInUrlbar(action);
    let id = this._urlbarButtonNodeIDForActionID(action.id);
    let node = document.getElementById(id);

    if (!action.shownInUrlbar) {
      if (node) {
        if (action.__urlbarNodeInMarkup) {
          node.hidden = true;
        } else {
          node.remove();
        }
      }
      return;
    }

    let newlyPlaced = false;
    if (action.__urlbarNodeInMarkup) {
      newlyPlaced = node && node.hidden;
      node.hidden = false;
    } else if (!node) {
      newlyPlaced = true;
      node = this._makeUrlbarButtonNode(action);
      node.id = id;
    }

    if (newlyPlaced) {
      let parentNode = this.mainButtonNode.parentNode;
      let insertBeforeNode = null;
      if (insertBeforeID) {
        let insertBeforeNodeID =
          this._urlbarButtonNodeIDForActionID(insertBeforeID);
        insertBeforeNode = document.getElementById(insertBeforeNodeID);
      }
      parentNode.insertBefore(node, insertBeforeNode);
      action.onPlacedInUrlbar(node);

      // urlbar buttons should always have tooltips, so if the node doesn't have
      // one, then as a last resort use the label of the corresponding panel
      // button.  Why not set tooltiptext to action.title when the node is
      // created?  Because the consumer may set a title dynamically.
      if (!node.hasAttribute("tooltiptext")) {
        let panelNodeID = this._panelButtonNodeIDForActionID(action.id);
        let panelNode = document.getElementById(panelNodeID);
        if (panelNode) {
          node.setAttribute("tooltiptext", panelNode.getAttribute("label"));
        }
      }
    }
  },

  _makeUrlbarButtonNode(action) {
    let buttonNode = document.createElement("image");
    buttonNode.classList.add("urlbar-icon", "urlbar-page-action");
    buttonNode.setAttribute("role", "button");
    if (action.tooltip) {
      buttonNode.setAttribute("tooltiptext", action.tooltip);
    }
    if (action.iconURL) {
      buttonNode.style.listStyleImage = `url('${action.iconURL}')`;
    }
    buttonNode.setAttribute("context", "pageActionPanelContextMenu");
    buttonNode.addEventListener("contextmenu", event => {
      BrowserPageActions.onContextMenu(event);
    });
    if (action.nodeAttributes) {
      for (let name in action.nodeAttributes) {
        buttonNode.setAttribute(name, action.nodeAttributes[name]);
      }
    }
    buttonNode.addEventListener("click", event => {
      this.doCommandForAction(action, event, buttonNode);
    });
    return buttonNode;
  },

  /**
   * Removes all the DOM nodes of the given action.
   *
   * @param  action (PageActions.Action, required)
   *         The action to remove.
   */
  removeAction(action) {
    this._removeActionFromPanel(action);
    this._removeActionFromUrlbar(action);
  },

  _removeActionFromPanel(action) {
    let id = this._panelButtonNodeIDForActionID(action.id);
    let node = document.getElementById(id);
    if (node) {
      node.remove();
    }
    if (action.subview) {
      let panelViewNodeID = this._panelViewNodeIDForActionID(action.id, false);
      let panelViewNode = document.getElementById(panelViewNodeID);
      if (panelViewNode) {
        panelViewNode.remove();
      }
    }
    // If there are now no more non-built-in actions, remove the separator
    // between the built-ins and non-built-ins.
    if (!PageActions.nonBuiltInActions.length) {
      let separator = document.getElementById(
        this._panelButtonNodeIDForActionID(
          PageActions.ACTION_ID_BUILT_IN_SEPARATOR
        )
      );
      if (separator) {
        separator.remove();
      }
    }
  },

  _removeActionFromUrlbar(action) {
    let id = this._urlbarButtonNodeIDForActionID(action.id);
    let node = document.getElementById(id);
    if (node) {
      node.remove();
    }
  },

  /**
   * Updates the DOM nodes of an action to reflect its changed iconURL.
   *
   * @param  action (PageActions.Action, required)
   *         The action to update.
   */
  updateActionIconURL(action) {
    let url = action.iconURL ? `url('${action.iconURL}')` : null;
    let nodeIDs = [
      this._panelButtonNodeIDForActionID(action.id),
      this._urlbarButtonNodeIDForActionID(action.id),
    ];
    for (let nodeID of nodeIDs) {
      let node = document.getElementById(nodeID);
      if (node) {
        if (url) {
          node.style.listStyleImage = url;
        } else {
          node.style.removeProperty("list-style-image");
        }
      }
    }
  },

  /**
   * Updates the DOM nodes of an action to reflect its changed title.
   *
   * @param  action (PageActions.Action, required)
   *         The action to update.
   */
  updateActionTitle(action) {
    let id = this._panelButtonNodeIDForActionID(action.id);
    let node = document.getElementById(id);
    if (node) {
      node.setAttribute("label", action.title);
    }
  },

  doCommandForAction(action, event, buttonNode) {
    if (event.type == "click" && event.button != 0) {
      return;
    }
    PageActions.logTelemetry("used", action, buttonNode);
    // If we're in the panel, open a subview inside the panel:
    // Note that we can't use this.panelNode.contains(buttonNode) here
    // because of XBL boundaries breaking ELement.contains.
    if (action.subview && buttonNode && buttonNode.closest("panel") == this.panelNode) {
      let panelViewNodeID = this._panelViewNodeIDForActionID(action.id, false);
      let panelViewNode = document.getElementById(panelViewNodeID);
      action.subview.onShowing(panelViewNode);
      this.multiViewNode.showSubView(panelViewNode, buttonNode);
      return;
    }
    // Otherwise, hide the main popup in case it was open:
    this.panelNode.hidePopup();

    // Toggle the activated action's panel if necessary
    if (action.subview || action.wantsIframe) {
      this._toggleActivatedActionPanelForAction(action);
      return;
    }

    // Otherwise, run the action.
    action.onCommand(event, buttonNode);
  },

  /**
   * Returns the action for a node.
   *
   * @param  node (DOM node, required)
   *         A button DOM node, either one that's shown in the page action panel
   *         or the urlbar.
   * @return (PageAction.Action) The node's related action, or null if none.
   */
  actionForNode(node) {
    if (!node) {
      return null;
    }
    let actionID = this._actionIDForNodeID(node.id);
    let action = PageActions.actionForID(actionID);
    if (!action) {
      // The given node may be an ancestor of a node corresponding to an action,
      // like how #star-button is contained in #star-button-box, the latter
      // being the bookmark action's node.  Look up the ancestor chain.
      for (let n = node.parentNode; n && !action; n = n.parentNode) {
        if (n.id == "page-action-buttons" || n.localName == "panelview") {
          // We reached the page-action-buttons or panelview container.
          // Stop looking; no acton was found.
          break;
        }
        actionID = this._actionIDForNodeID(n.id);
        action = PageActions.actionForID(actionID);
      }
    }
    return action;
  },

  // The ID of the given action's top-level button in the panel.
  _panelButtonNodeIDForActionID(actionID) {
    return `pageAction-panel-${actionID}`;
  },

  // The ID of the given action's button in the urlbar.
  _urlbarButtonNodeIDForActionID(actionID) {
    let action = PageActions.actionForID(actionID);
    if (action && action.urlbarIDOverride) {
      return action.urlbarIDOverride;
    }
    return `pageAction-urlbar-${actionID}`;
  },

  // The ID of the given action's panelview.
  _panelViewNodeIDForActionID(actionID, forUrlbar) {
    let placementID = forUrlbar ? "urlbar" : "panel";
    return `pageAction-${placementID}-${actionID}-subview`;
  },

  // The ID of the given button in the given action's panelview.
  _panelViewButtonNodeIDForActionID(actionID, buttonID, forUrlbar) {
    let placementID = forUrlbar ? "urlbar" : "panel";
    return `pageAction-${placementID}-${actionID}-${buttonID}`;
  },

  // The ID of the action corresponding to the given top-level button in the
  // panel or button in the urlbar.
  _actionIDForNodeID(nodeID) {
    if (!nodeID) {
      return null;
    }
    let match = nodeID.match(/^pageAction-(?:panel|urlbar)-(.+)$/);
    if (match) {
      return match[1];
    }
    // Check all the urlbar ID overrides.
    for (let action of PageActions.actions) {
      if (action.urlbarIDOverride && action.urlbarIDOverride == nodeID) {
        return action.id;
      }
    }
    return null;
  },

  /**
   * Call this when the main page action button in the urlbar is activated.
   *
   * @param  event (DOM event, required)
   *         The click or whatever event.
   */
  mainButtonClicked(event) {
    event.stopPropagation();
    if ((event.type == "mousedown" && event.button != 0) ||
        (event.type == "keypress" && event.charCode != KeyEvent.DOM_VK_SPACE &&
         event.keyCode != KeyEvent.DOM_VK_RETURN)) {
      return;
    }

    // If the activated-action panel is open and anchored to the main button,
    // close it.
    let panelNode = this.activatedActionPanelNode;
    if (panelNode && panelNode.anchorNode.id == this.mainButtonNode.id) {
      panelNode.hidePopup();
      return;
    }

    if (this.panelNode.state == "open") {
      this.panelNode.hidePopup();
    } else if (this.panelNode.state == "closed") {
      this.showPanel(event);
    }
  },

  /**
   * Show the page action panel
   *
   * @param  event (DOM event, optional)
   *         The event that triggers showing the panel. (such as a mouse click,
   *         if the user clicked something to open the panel)
   */
  showPanel(event = null) {
    for (let action of PageActions.actions) {
      let buttonNodeID = this._panelButtonNodeIDForActionID(action.id);
      let buttonNode = document.getElementById(buttonNodeID);
      action.onShowingInPanel(buttonNode);
    }

    this.panelNode.hidden = false;
    this.panelNode.addEventListener("popuphiding", () => {
      this.mainButtonNode.removeAttribute("open");
    }, {once: true});
    this.mainButtonNode.setAttribute("open", "true");
    this.panelNode.openPopup(this.mainButtonNode, {
      position: "bottomcenter topright",
      triggerEvent: event,
    });
  },

  /**
   * Call this on the contextmenu event.  Note that this is called before
   * onContextMenuShowing.
   *
   * @param  event (DOM event, required)
   *         The contextmenu event.
   */
  onContextMenu(event) {
    let node = event.originalTarget;
    this._contextAction = this.actionForNode(node);
    // Don't show the menu if there's no action where the user clicked!
    if (!this._contextAction) {
      event.preventDefault();
    }
  },

  /**
   * Call this on the context menu's popupshowing event.
   *
   * @param  event (DOM event, required)
   *         The popupshowing event.
   * @param  popup (DOM node, required)
   *         The context menu popup DOM node.
   */
  onContextMenuShowing(event, popup) {
    if (event.target != popup) {
      return;
    }
    // Right now there's only one item in the context menu, to toggle the
    // context action's shown-in-urlbar state.  Update it now.
    let toggleItem = popup.firstChild;
    let toggleItemLabel = null;
    if (this._contextAction) {
      toggleItem.disabled = false;
      if (this._contextAction.shownInUrlbar) {
        toggleItemLabel = toggleItem.getAttribute("remove-label");
      }
    }
    if (!toggleItemLabel) {
      toggleItemLabel = toggleItem.getAttribute("add-label");
    }
    toggleItem.label = toggleItemLabel;
  },

  /**
   * Call this from the context menu's toggle menu item.
   */
  toggleShownInUrlbarForContextAction() {
    if (!this._contextAction) {
      return;
    }
    let telemetryType = this._contextAction.shownInUrlbar ? "removed" : "added";
    PageActions.logTelemetry(telemetryType, this._contextAction);
    this._contextAction.shownInUrlbar = !this._contextAction.shownInUrlbar;
  },

  _contextAction: null,

  /**
   * A bunch of strings (labels for actions and the like) are defined in DTD,
   * but actions are created in JS.  So what we do is add a bunch of attributes
   * to the page action panel's definition in the markup, whose values hold
   * these DTD strings.  Then when each built-in action is set up, we get the
   * related strings from the panel node and set up the action's node with them.
   *
   * The convention is to set for example the "title" property in an action's JS
   * definition to the name of the attribute on the panel node that holds the
   * actual title string.  Then call this function, passing the action's related
   * DOM node and the name of the attribute that you are setting on the DOM
   * node -- "label" or "title" in this example (either will do).
   *
   * @param  node (DOM node, required)
   *         The node of an action you're setting up.
   * @param  attrName (string, required)
   *         The name of the attribute *on the node you're setting up*.
   */
  takeNodeAttributeFromPanel(node, attrName) {
    let panelAttrName = node.getAttribute(attrName);
    if (!panelAttrName && attrName == "title") {
      attrName = "label";
      panelAttrName = node.getAttribute(attrName);
    }
    if (panelAttrName) {
      let attrValue = this.panelNode.getAttribute(panelAttrName);
      if (attrValue) {
        node.setAttribute(attrName, attrValue);
      }
    }
  },
};

var BrowserPageActionFeedback = {
  /**
   * The feedback page action panel DOM node (DOM node)
   */
  get panelNode() {
    delete this.panelNode;
    return this.panelNode = document.getElementById("pageActionFeedback");
  },

  get feedbackAnimationBox() {
    delete this.feedbackAnimationBox;
    return this.feedbackAnimationBox = document.getElementById("pageActionFeedbackAnimatableBox");
  },

  get feedbackLabel() {
    delete this.feedbackLabel;
    return this.feedbackLabel = document.getElementById("pageActionFeedbackMessage");
  },

  show(action, event) {
    this.feedbackLabel.textContent = this.panelNode.getAttribute(action.id + "Feedback");
    this.panelNode.hidden = false;

    let anchor = BrowserPageActions.panelAnchorNodeForAction(action);
    this.panelNode.openPopup(anchor, {
      position: "bottomcenter topright",
      triggerEvent: event,
    });

    this.panelNode.addEventListener("popupshown", () => {
      this.feedbackAnimationBox.setAttribute("animate", "true");
    }, {once: true});
    this.panelNode.addEventListener("popuphidden", () => {
      this.feedbackAnimationBox.removeAttribute("animate");
    }, {once: true});

    // The timeout value used here allows the panel to stay open for
    // 1 second after the text transition (duration=120ms) has finished.
    setTimeout(() => {
      this.panelNode.hidePopup(true);
    }, Services.prefs.getIntPref("browser.pageActions.feedbackTimeoutMS", 1120));
  },
};

// built-in actions below //////////////////////////////////////////////////////

// bookmark
BrowserPageActions.bookmark = {
  onShowingInPanel(buttonNode) {
    // Update the button label via the bookmark observer.
    BookmarkingUI.updateBookmarkPageMenuItem();
  },

  onCommand(event, buttonNode) {
    BrowserPageActions.panelNode.hidePopup();
    BookmarkingUI.onStarCommand(event);
  },
};

// copy URL
BrowserPageActions.copyURL = {
  onPlacedInPanel(buttonNode) {
    BrowserPageActions.takeNodeAttributeFromPanel(buttonNode, "title");
  },

  onCommand(event, buttonNode) {
    BrowserPageActions.panelNode.hidePopup();
    Cc["@mozilla.org/widget/clipboardhelper;1"]
      .getService(Ci.nsIClipboardHelper)
      .copyString(gURLBar.makeURIReadable(gBrowser.selectedBrowser.currentURI).displaySpec);
    let action = PageActions.actionForID("copyURL");
    BrowserPageActionFeedback.show(action, event);
  },
};

// email link
BrowserPageActions.emailLink = {
  onPlacedInPanel(buttonNode) {
    BrowserPageActions.takeNodeAttributeFromPanel(buttonNode, "title");
  },

  onCommand(event, buttonNode) {
    BrowserPageActions.panelNode.hidePopup();
    MailIntegration.sendLinkForBrowser(gBrowser.selectedBrowser);
  },
};

// send to device
BrowserPageActions.sendToDevice = {
  onPlacedInPanel(buttonNode) {
    BrowserPageActions.takeNodeAttributeFromPanel(buttonNode, "title");
  },

  onSubviewPlaced(panelViewNode) {
    let bodyNode = panelViewNode.firstChild;
    for (let node of bodyNode.childNodes) {
      BrowserPageActions.takeNodeAttributeFromPanel(node, "title");
      BrowserPageActions.takeNodeAttributeFromPanel(node, "shortcut");
    }
  },

  onShowingInPanel(buttonNode) {
    let browser = gBrowser.selectedBrowser;
    let url = browser.currentURI.spec;
    if (gSync.isSendableURI(url)) {
      buttonNode.removeAttribute("disabled");
    } else {
      buttonNode.setAttribute("disabled", "true");
    }
  },

  onShowingSubview(panelViewNode) {
    let browser = gBrowser.selectedBrowser;
    let url = browser.currentURI.spec;
    let title = browser.contentTitle;

    let bodyNode = panelViewNode.firstChild;
    let panelNode = panelViewNode.closest("panel");

    // This is on top because it also clears the device list between state
    // changes.
    gSync.populateSendTabToDevicesMenu(bodyNode, url, title, (clientId, name, clientType) => {
      if (!name) {
        return document.createElement("toolbarseparator");
      }
      let item = document.createElement("toolbarbutton");
      item.classList.add("pageAction-sendToDevice-device", "subviewbutton");
      if (clientId) {
        item.classList.add("subviewbutton-iconic");
      }
      item.setAttribute("tooltiptext", name);
      item.addEventListener("command", event => {
        if (panelNode) {
          panelNode.hidePopup();
        }
        // There are items in the subview that don't represent devices: "Sign
        // in", "Learn about Sync", etc.  Device items will be .sendtab-target.
        if (event.target.classList.contains("sendtab-target")) {
          let action = PageActions.actionForID("sendToDevice");
          BrowserPageActionFeedback.show(action, event);
        }
      });
      return item;
    });

    bodyNode.removeAttribute("state");
    // In the first ~10 sec after startup, Sync may not be loaded and the list
    // of devices will be empty.
    if (gSync.syncConfiguredAndLoading) {
      bodyNode.setAttribute("state", "notready");
      // Force a background Sync
      Services.tm.dispatchToMainThread(async () => {
        await Weave.Service.sync([]);  // [] = clients engine only
        // There's no way Sync is still syncing at this point, but we check
        // anyway to avoid infinite looping.
        if (!window.closed && !gSync.syncConfiguredAndLoading) {
          this.onShowingSubview(panelViewNode);
        }
      });
    }
  },
};
