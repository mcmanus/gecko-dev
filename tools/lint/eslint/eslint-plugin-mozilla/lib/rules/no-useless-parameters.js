/**
 * @fileoverview Reject common XPCOM methods called with useless optional
 *               parameters, or non-existent parameters.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

"use strict";

// -----------------------------------------------------------------------------
// Rule Definition
// -----------------------------------------------------------------------------

var helpers = require("../helpers");

module.exports = function(context) {

  // ---------------------------------------------------------------------------
  // Public
  //  --------------------------------------------------------------------------

  return {
    "CallExpression": function(node) {
      let callee = node.callee;
      if (callee.type !== "MemberExpression" ||
          callee.property.type !== "Identifier") {
        return;
      }

      if ((callee.property.name === "addEventListener" ||
           callee.property.name === "removeEventListener") &&
          node.arguments.length === 3) {
        let arg = node.arguments[2];
        if (arg.type === "Literal" && arg.value === false) {
          context.report(node, callee.property.name +
                         "'s third parameter can be omitted when it's false.");
        }
      }

      if (callee.property.name == "clearUserPref" &&
          node.arguments.length > 1) {
        context.report(node, callee.property.name + " takes only 1 parameter.");
      }

      if (callee.property.name === "removeObserver" &&
          node.arguments.length === 3) {
        let arg = node.arguments[2];
        if (arg.type === "Literal" && (arg.value === false ||
                                       arg.value === true)) {
          context.report(node, "removeObserver only takes 2 parameters.");
        }
      }

      if (callee.property.name === "getComputedStyle" &&
          node.arguments.length === 2) {
        let arg = node.arguments[1];
        if (arg.type === "Literal" && !arg.value) {
          context.report(node,
                         "getComputedStyle's second parameter can be omitted.");
        }
      }

      if (callee.property.name === "newURI" &&
          node.arguments.length > 1) {
        let arg = node.arguments[node.arguments.length - 1];
        if (arg.type === "Literal" && !arg.value) {
          context.report(node, "newURI's last parameters are optional.");
        }
      }
    }
  };
};
