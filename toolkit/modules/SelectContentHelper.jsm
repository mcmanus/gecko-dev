/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const Cc = Components.classes;
const Ci = Components.interfaces;
const Cu = Components.utils;

Cu.import("resource://gre/modules/XPCOMUtils.jsm");

XPCOMUtils.defineLazyModuleGetter(this, "BrowserUtils",
                                  "resource://gre/modules/BrowserUtils.jsm");
XPCOMUtils.defineLazyServiceGetter(this, "DOMUtils",
                                   "@mozilla.org/inspector/dom-utils;1", "inIDOMUtils");
XPCOMUtils.defineLazyModuleGetter(this, "DeferredTask",
                                  "resource://gre/modules/DeferredTask.jsm");

const kStateActive = 0x00000001; // NS_EVENT_STATE_ACTIVE
const kStateHover = 0x00000004; // NS_EVENT_STATE_HOVER

// A process global state for whether or not content thinks
// that a <select> dropdown is open or not. This is managed
// entirely within this module, and is read-only accessible
// via SelectContentHelper.open.
var gOpen = false;

this.EXPORTED_SYMBOLS = [
  "SelectContentHelper"
];

this.SelectContentHelper = function(aElement, aOptions, aGlobal) {
  this.element = aElement;
  this.initialSelection = aElement[aElement.selectedIndex] || null;
  this.global = aGlobal;
  this.closedWithEnter = false;
  this.isOpenedViaTouch = aOptions.isOpenedViaTouch;
  this._selectBackgroundColor = null;
  this._selectColor = null;
  this._uaBackgroundColor = null;
  this._uaColor = null;
  this._uaSelectBackgroundColor = null;
  this._uaSelectColor = null;
  this.init();
  this.showDropDown();
  this._updateTimer = new DeferredTask(this._update.bind(this), 0);
}

Object.defineProperty(SelectContentHelper, "open", {
  get() {
    return gOpen;
  },
});

this.SelectContentHelper.prototype = {
  init() {
    this.global.addMessageListener("Forms:SelectDropDownItem", this);
    this.global.addMessageListener("Forms:DismissedDropDown", this);
    this.global.addMessageListener("Forms:MouseOver", this);
    this.global.addMessageListener("Forms:MouseOut", this);
    this.global.addMessageListener("Forms:MouseUp", this);
    this.global.addEventListener("pagehide", this, { mozSystemGroup: true });
    this.global.addEventListener("mozhidedropdown", this, { mozSystemGroup: true });
    this.element.addEventListener("blur", this, { mozSystemGroup: true });
    let MutationObserver = this.element.ownerGlobal.MutationObserver;
    this.mut = new MutationObserver(mutations => {
      // Something changed the <select> while it was open, so
      // we'll poke a DeferredTask to update the parent sometime
      // in the very near future.
      this._updateTimer.arm();
    });
    this.mut.observe(this.element, {childList: true, subtree: true, attributes: true});
  },

  uninit() {
    this.element.openInParentProcess = false;
    this.global.removeMessageListener("Forms:SelectDropDownItem", this);
    this.global.removeMessageListener("Forms:DismissedDropDown", this);
    this.global.removeMessageListener("Forms:MouseOver", this);
    this.global.removeMessageListener("Forms:MouseOut", this);
    this.global.removeMessageListener("Forms:MouseUp", this);
    this.global.removeEventListener("pagehide", this, { mozSystemGroup: true });
    this.global.removeEventListener("mozhidedropdown", this, { mozSystemGroup: true });
    this.element.removeEventListener("blur", this, { mozSystemGroup: true });
    this.element = null;
    this.global = null;
    this.mut.disconnect();
    this._updateTimer.disarm();
    this._updateTimer = null;
    gOpen = false;
  },

  showDropDown() {
    this.element.openInParentProcess = true;
    let rect = this._getBoundingContentRect();
    DOMUtils.addPseudoClassLock(this.element, ":focus");
    let computedStyles = getComputedStyles(this.element);
    this._selectBackgroundColor = computedStyles.backgroundColor;
    this._selectColor = computedStyles.color;
    DOMUtils.clearPseudoClassLocks(this.element);
    this.global.sendAsyncMessage("Forms:ShowDropDown", {
      direction: computedStyles.direction,
      isOpenedViaTouch: this.isOpenedViaTouch,
      options: this._buildOptionList(),
      rect,
      selectedIndex: this.element.selectedIndex,
      selectBackgroundColor: this._selectBackgroundColor,
      selectColor: this._selectColor,
      uaBackgroundColor: this.uaBackgroundColor,
      uaColor: this.uaColor,
      uaSelectBackgroundColor: this.uaSelectBackgroundColor,
      uaSelectColor: this.uaSelectColor
    });
    gOpen = true;
  },

  _getBoundingContentRect() {
    return BrowserUtils.getElementBoundingScreenRect(this.element);
  },

  _buildOptionList() {
    DOMUtils.addPseudoClassLock(this.element, ":focus");
    let result = buildOptionListForChildren(this.element);
    DOMUtils.clearPseudoClassLocks(this.element);
    return result;
  },

  _update() {
    // The <select> was updated while the dropdown was open.
    // Let's send up a new list of options.
    // Technically we might not need to set this pseudo-class
    // during _update() since the element should organically
    // have :focus, though it is here for belt-and-suspenders.
    DOMUtils.addPseudoClassLock(this.element, ":focus");
    let computedStyles = getComputedStyles(this.element);
    this._selectBackgroundColor = computedStyles.backgroundColor;
    this._selectColor = computedStyles.color;
    DOMUtils.clearPseudoClassLocks(this.element);
    this.global.sendAsyncMessage("Forms:UpdateDropDown", {
      options: this._buildOptionList(),
      selectedIndex: this.element.selectedIndex,
      selectBackgroundColor: this._selectBackgroundColor,
      selectColor: this._selectColor,
      uaBackgroundColor: this.uaBackgroundColor,
      uaColor: this.uaColor,
      uaSelectBackgroundColor: this.uaSelectBackgroundColor,
      uaSelectColor: this.uaSelectColor
    });
  },

  // Determine user agent background-color and color.
  // This is used to skip applying the custom color if it matches
  // the user agent values.
  _calculateUAColors() {
    let dummyOption = this.element.ownerDocument.createElement("option");
    dummyOption.style.setProperty("color", "-moz-comboboxtext", "important");
    dummyOption.style.setProperty("background-color", "-moz-combobox", "important");
    let optionCS = this.element.ownerGlobal.getComputedStyle(dummyOption);
    this._uaBackgroundColor = optionCS.backgroundColor;
    this._uaColor = optionCS.color;
    let dummySelect = this.element.ownerDocument.createElement("select");
    dummySelect.style.setProperty("color", "-moz-fieldtext", "important");
    dummySelect.style.setProperty("background-color", "-moz-field", "important");
    let selectCS = this.element.ownerGlobal.getComputedStyle(dummySelect);
    this._uaSelectBackgroundColor = selectCS.backgroundColor;
    this._uaSelectColor = selectCS.color;
  },

  get uaBackgroundColor() {
    if (!this._uaBackgroundColor) {
      this._calculateUAColors();
    }
    return this._uaBackgroundColor;
  },

  get uaColor() {
    if (!this._uaColor) {
      this._calculateUAColors();
    }
    return this._uaColor;
  },

  get uaSelectBackgroundColor() {
    if (!this._selectBackgroundColor) {
      this._calculateUAColors();
    }
    return this._uaSelectBackgroundColor;
  },

  get uaSelectColor() {
    if (!this._selectBackgroundColor) {
      this._calculateUAColors();
    }
    return this._uaSelectColor;
  },

  dispatchMouseEvent(win, target, eventName) {
    let mouseEvent = new win.MouseEvent(eventName, {
      view: win,
      bubbles: true,
      cancelable: true,
    });
    target.dispatchEvent(mouseEvent);
  },

  receiveMessage(message) {
    switch (message.name) {
      case "Forms:SelectDropDownItem":
        this.element.selectedIndex = message.data.value;
        this.closedWithEnter = message.data.closedWithEnter;
        break;

      case "Forms:DismissedDropDown":
        let selectedOption = this.element.item(this.element.selectedIndex);
        if (this.initialSelection != selectedOption) {
          let win = this.element.ownerGlobal;
          // For ordering of events, we're using non-e10s as our guide here,
          // since the spec isn't exactly clear. In non-e10s, we fire:
          // mousedown, mouseup, input, change, click if the user clicks
          // on an element in the dropdown. If the user uses the keyboard
          // to select an element in the dropdown, we only fire input and
          // change events.
          if (!this.closedWithEnter) {
            this.dispatchMouseEvent(win, selectedOption, "mousedown");
            this.dispatchMouseEvent(win, selectedOption, "mouseup");
            DOMUtils.removeContentState(this.element, kStateActive);
          }

          let inputEvent = new win.UIEvent("input", {
            bubbles: true,
          });
          this.element.dispatchEvent(inputEvent);

          let changeEvent = new win.Event("change", {
            bubbles: true,
          });
          this.element.dispatchEvent(changeEvent);

          if (!this.closedWithEnter) {
            this.dispatchMouseEvent(win, selectedOption, "click");
          }
        }

        this.uninit();
        break;

      case "Forms:MouseOver":
        DOMUtils.setContentState(this.element, kStateHover);
        break;

      case "Forms:MouseOut":
        DOMUtils.removeContentState(this.element, kStateHover);
        break;

      case "Forms:MouseUp":
        let win = this.element.ownerGlobal;
        if (message.data.onAnchor) {
          this.dispatchMouseEvent(win, this.element, "mouseup");
        }
        DOMUtils.removeContentState(this.element, kStateActive);
        if (message.data.onAnchor) {
          this.dispatchMouseEvent(win, this.element, "click");
        }
        break;
    }
  },

  handleEvent(event) {
    switch (event.type) {
      case "pagehide":
        if (this.element.ownerDocument === event.target) {
          this.global.sendAsyncMessage("Forms:HideDropDown", {});
          this.uninit();
        }
        break;
      case "blur":
      case "mozhidedropdown":
        if (this.element === event.target) {
          this.global.sendAsyncMessage("Forms:HideDropDown", {});
          this.uninit();
        }
        break;
    }
  }

}

function getComputedStyles(element) {
  return element.ownerGlobal.getComputedStyle(element);
}

function buildOptionListForChildren(node) {
  let result = [];

  for (let child of node.children) {
    let tagName = child.tagName.toUpperCase();

    if (tagName == "OPTION" || tagName == "OPTGROUP") {
      if (child.hidden) {
        continue;
      }

      let textContent =
        tagName == "OPTGROUP" ? child.getAttribute("label")
                              : child.text;
      if (textContent == null) {
        textContent = "";
      }

      // Selected options have the :checked pseudo-class, which
      // we want to disable before calculating the computed
      // styles since the user agent styles alter the styling
      // based on :checked.
      DOMUtils.addPseudoClassLock(child, ":checked", false);
      let cs = getComputedStyles(child);

      let info = {
        index: child.index,
        tagName,
        textContent,
        disabled: child.disabled,
        display: cs.display,
        // We need to do this for every option element as each one can have
        // an individual style set for direction
        textDirection: cs.direction,
        tooltip: child.title,
        backgroundColor: cs.backgroundColor,
        color: cs.color,
        children: tagName == "OPTGROUP" ? buildOptionListForChildren(child) : []
      };

      // We must wait until all computedStyles have been
      // read before we clear the locks.
      DOMUtils.clearPseudoClassLocks(child);

      result.push(info);
    }
  }
  return result;
}
