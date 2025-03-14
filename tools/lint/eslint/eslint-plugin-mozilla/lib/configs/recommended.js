"use strict";

module.exports = {
  // When adding items to this file please check for effects on sub-directories.
  "plugins": [
    "mozilla"
  ],
  "env": {
    "browser": true,
    "es6": true
  },
  "parserOptions": {
    "ecmaVersion": 8
  },
  // When adding items to this file please check for effects on all of toolkit
  // and browser
  "rules": {
    "mozilla/avoid-removeChild": "error",
    "mozilla/avoid-nsISupportsString-preferences": "error",
    "mozilla/import-globals": "error",
    "mozilla/no-import-into-var-and-global": "error",
    "mozilla/no-useless-parameters": "error",
    "mozilla/no-useless-removeEventListener": "error",
    "mozilla/use-default-preference-values": "error",
    "mozilla/use-ownerGlobal": "error",

    // Braces only needed for multi-line arrow function blocks
    // "arrow-body-style": ["error", "as-needed"],

    // Require spacing around =>
    "arrow-spacing": "error",

    // Always require spacing around a single line block
    "block-spacing": "error",

    // No newline before open brace for a block
    "brace-style": ["error", "1tbs", { "allowSingleLine": true }],

    // No space before always a space after a comma
    "comma-spacing": ["error", {"before": false, "after": true}],

    // Commas at the end of the line not the start
    // "comma-style": "error",

    // Warn about cyclomatic complexity in functions.
    // XXX Get this down to 20?
    "complexity": ["error", {"max": 35}],

    // Don't require spaces around computed properties
    "computed-property-spacing": ["error", "never"],

    // Functions must always return something or nothing
    "consistent-return": "error",

    // Require braces around blocks that start a new line
    // Note that this rule is likely to be overridden on a per-directory basis
    // very frequently.
    // "curly": ["error", "multi-line"],

    // Always require a trailing EOL
    "eol-last": "error",

    // No spaces between function name and parentheses
    "func-call-spacing": "error",

    // Require function* name()
    // "generator-star-spacing": ["error", {"before": false, "after": true}],

    // Two space indent
    // "indent": ["error", 2, { "SwitchCase": 1 }],

    // Space after colon not before in property declarations
    "key-spacing": ["error", {
      "beforeColon": false,
      "afterColon": true,
      "mode": "minimum"
    }],

    // Require spaces before and after keywords
    "keyword-spacing": "error",

    // Unix linebreaks
    "linebreak-style": ["error", "unix"],

    // Don't enforce the maximum depth that blocks can be nested. The complexity
    // rule is a better rule to check this.
    "max-depth": "off",

    // Always require parenthesis for new calls
    // "new-parens": "error",

    // Use [] instead of Array()
    // "no-array-constructor": "error",

    // Disallow assignment operators in conditional statements
    "no-cond-assign": "error",

    // Disallow the use of debugger
    "no-debugger": "error",

    // Disallow deleting variables
    "no-delete-var": "error",

    // No duplicate arguments in function declarations
    "no-dupe-args": "error",

    // No duplicate keys in object declarations
    "no-dupe-keys": "error",

    // No duplicate cases in switch statements
    "no-duplicate-case": "error",

    // Disallow unnecessary calls to .bind()
    "no-extra-bind": "error",

    // No labels
    "no-labels": "error",

    // Disallow unnecessary nested blocks
    "no-lone-blocks": "error",

    // If an if block ends with a return no need for an else block
    "no-else-return": "error",

    // No empty statements
    "no-empty": ["error", {"allowEmptyCatch": true}],

    // No empty character classes in regex
    "no-empty-character-class": "error",

    // Disallow empty destructuring
    "no-empty-pattern": "error",

    // No assiging to exception variable
    "no-ex-assign": "error",

    // No using !! where casting to boolean is already happening
    "no-extra-boolean-cast": "error",

    // No double semicolon
    "no-extra-semi": "error",

    // No overwriting defined functions
    "no-func-assign": "error",

    // No invalid regular expresions
    "no-invalid-regexp": "error",

    // No odd whitespace characters
    "no-irregular-whitespace": "error",

    // Disallow the use of the __iterator__ property
    "no-iterator": "error",

    // No single if block inside an else block
    "no-lonely-if": "error",

    // No mixing spaces and tabs in indent
    "no-mixed-spaces-and-tabs": ["error", "smart-tabs"],

    // No unnecessary spacing
    "no-multi-spaces": ["error", { exceptions: {
      "AssignmentExpression": true,
      "VariableDeclarator": true,
      "ArrayExpression": true,
      "ObjectExpression": true
    } }],

    // No reassigning native JS objects
    "no-native-reassign": "error",

    // Nested ternary statements are confusing
    "no-nested-ternary": "error",

    // Use {} instead of new Object()
    "no-new-object": "error",

    // No Math() or JSON()
    "no-obj-calls": "error",

    // No octal literals
    "no-octal": "error",

    // No redeclaring variables
    "no-redeclare": "error",

    // Disallow multiple spaces in regular expressions
    "no-regex-spaces": "error",

    // Disallow assignments where both sides are exactly the same
    "no-self-assign": "error",

    // No unnecessary comparisons
    "no-self-compare": "error",

    // No declaring variables from an outer scope
    // "no-shadow": "error",

    // No declaring variables that hide things like arguments
    "no-shadow-restricted-names": "error",

    // Disallow sparse arrays
    "no-sparse-arrays": "error",

    // No trailing whitespace
    "no-trailing-spaces": "error",

    // No using undeclared variables
    "no-undef": "error",

    // Error on newline where a semicolon is needed
    "no-unexpected-multiline": "error",

    // No unreachable statements
    "no-unreachable": "error",

    // Disallow control flow statements in finally blocks
    "no-unsafe-finally": "error",

    // No (!foo in bar) or (!object instanceof Class)
    "no-unsafe-negation": "error",

    // No declaring variables that are never used
    "no-unused-vars": ["error", {
      "vars": "local",
      "varsIgnorePattern": "^Cc|Ci|Cu|Cr|EXPORTED_SYMBOLS",
      "args": "none"
    }],

    // No using variables before defined
    // "no-use-before-define": ["error", "nofunc"],

    // Disallow unnecessary .call() and .apply()
    "no-useless-call": "error",

    // Disallow redundant return statements
    "no-useless-return": "error",

    // No using with
    "no-with": "error",

    // Require object-literal shorthand with ES6 method syntax
    "object-shorthand": ["error", "always", { "avoidQuotes": true }],

    // Require double-quotes everywhere, except where quotes are escaped
    // or template literals are used.
    "quotes": ["error", "double", {
      "avoidEscape": true,
      "allowTemplateLiterals": true
    }],

    // No spacing inside rest or spread expressions
    "rest-spread-spacing": "error",

    // Always require semicolon at end of statement
    // "semi": ["error", "always"],

    // Require space before blocks
    "space-before-blocks": "error",

    // Never use spaces before function parentheses
    "space-before-function-paren": ["error", "never"],

    // No space padding in parentheses
    // "space-in-parens": ["error", "never"],

    // Require spaces around operators
    "space-infix-ops": ["error", { "int32Hint": true }],

    // ++ and -- should not need spacing
    "space-unary-ops": ["error", {
      "words": true,
      "nonwords": false,
      "overrides": {
        "typeof": false // We tend to use typeof as a function call
      }
    }],

    // Requires or disallows a whitespace (space or tab) beginning a comment
    "spaced-comment": "error",

    // No comparisons to NaN
    "use-isnan": "error",

    // Only check typeof against valid results
    "valid-typeof": "error",

    // Don't concatenate string literals together (unless they span multiple
    // lines)
    "no-useless-concat": "error"
  },
  "globals": {
    "BroadcastChannel": false,
    // Specific to Firefox (Chrome code only).
    "ChromeWindow": false,
    "ChromeWorker": false,
    "ChromeUtils": false,
    "Components": false,
    "CSSPrimitiveValue": false,
    "CSSValueList": false,
    "dump": true,
    // Specific to Firefox
    // eslint-disable-next-line max-len
    // https://developer.mozilla.org/docs/Web/JavaScript/Reference/Global_Objects/InternalError
    "InternalError": true,
    "KeyEvent": false,
    "openDialog": false,
    "MenuBoxObject": false,
    // Specific to Firefox (Chrome code only).
    "MozSelfSupport": false,
    "SimpleGestureEvent": false,
    "sizeToContent": false,
    "SharedArrayBuffer": false,
    // Note: StopIteration will likely be removed as part of removing legacy
    // generators, see bug 968038.
    "StopIteration": false,
    // Specific to Firefox
    // eslint-disable-next-line max-len
    // https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/uneval
    "uneval": false,
    "XULElement": false
  }
};
