/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

//------------------------------------------------------------------------------
// Requirements
//------------------------------------------------------------------------------

var rule = require("../lib/rules/avoid-nsISupportsString-preferences");

//------------------------------------------------------------------------------
// Tests
//------------------------------------------------------------------------------

function invalidCode(code, accessType = "get") {
  let message = "use " + accessType + "StringPref instead of " +
                accessType + "ComplexValue with nsISupportsString";
  return {code: code, errors: [{message: message, type: "CallExpression"}]};
}

exports.runTest = function(ruleTester) {
  ruleTester.run("no-useless-removeEventListener", rule, {
    valid: [
      "branch.getStringPref('name');",
      "branch.getComplexValue('name', Ci.nsIPrefLocalizedString);",
      "branch.setStringPref('name', 'blah');",
      "branch.setComplexValue('name', Ci.nsIPrefLocalizedString, pref);"
    ],
    invalid: [
      invalidCode("branch.getComplexValue('name', Ci.nsISupportsString);"),
      invalidCode("branch.getComplexValue('name', nsISupportsString);"),
      invalidCode("branch.getComplexValue('name', Ci.nsISupportsString).data;"),
      invalidCode("branch.setComplexValue('name', Ci.nsISupportsString, str);",
                  "set"),
      invalidCode("branch.setComplexValue('name', nsISupportsString, str);",
                  "set")
    ]
  });
};
