/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! The context within which CSS code is parsed.

#![deny(missing_docs)]

use cssparser::{Parser, SourcePosition, UnicodeRange};
use error_reporting::ParseErrorReporter;
#[cfg(feature = "gecko")]
use gecko_bindings::structs::URLExtraData;
#[cfg(feature = "gecko")]
use gecko_bindings::sugar::refptr::RefPtr;
use servo_url::ServoUrl;
use style_traits::OneOrMoreCommaSeparated;
use stylesheets::Origin;

/// Extra data that the style backend may need to parse stylesheets.
#[cfg(not(feature = "gecko"))]
pub struct ParserContextExtraData;

/// Extra data that the style backend may need to parse stylesheets.
#[cfg(feature = "gecko")]
pub struct ParserContextExtraData {
    /// The URL extra data.
    pub data: Option<RefPtr<URLExtraData>>,
}

#[cfg(not(feature = "gecko"))]
impl Default for ParserContextExtraData {
    fn default() -> Self {
        ParserContextExtraData
    }
}

#[cfg(feature = "gecko")]
impl Default for ParserContextExtraData {
    fn default() -> Self {
        ParserContextExtraData { data: None }
    }
}

#[cfg(feature = "gecko")]
impl ParserContextExtraData {
    /// Construct from a GeckoParserExtraData
    ///
    /// GeckoParserExtraData must live longer than this call
    pub unsafe fn new(data: *mut URLExtraData) -> Self {
        ParserContextExtraData {
            data: Some(RefPtr::new(data)),
        }
    }
}
/// The data that the parser needs from outside in order to parse a stylesheet.
pub struct ParserContext<'a> {
    /// The `Origin` of the stylesheet, whether it's a user, author or
    /// user-agent stylesheet.
    pub stylesheet_origin: Origin,
    /// The base url we're parsing this stylesheet as.
    pub base_url: &'a ServoUrl,
    /// An error reporter to report syntax errors.
    pub error_reporter: &'a ParseErrorReporter,
    /// Implementation-dependent extra data.
    pub extra_data: ParserContextExtraData,
}

impl<'a> ParserContext<'a> {
    /// Create a `ParserContext` with extra data.
    pub fn new_with_extra_data(stylesheet_origin: Origin,
                               base_url: &'a ServoUrl,
                               error_reporter: &'a ParseErrorReporter,
                               extra_data: ParserContextExtraData)
                               -> ParserContext<'a> {
        ParserContext {
            stylesheet_origin: stylesheet_origin,
            base_url: base_url,
            error_reporter: error_reporter,
            extra_data: extra_data,
        }
    }

    /// Create a parser context with the default extra data.
    pub fn new(stylesheet_origin: Origin,
               base_url: &'a ServoUrl,
               error_reporter: &'a ParseErrorReporter)
               -> ParserContext<'a> {
        let extra_data = ParserContextExtraData::default();
        Self::new_with_extra_data(stylesheet_origin, base_url, error_reporter, extra_data)
    }

    /// Create a parser context for on-the-fly parsing in CSSOM
    pub fn new_for_cssom(base_url: &'a ServoUrl,
                         error_reporter: &'a ParseErrorReporter)
                         -> ParserContext<'a> {
        Self::new(Origin::User, base_url, error_reporter)
    }
}

/// Defaults to a no-op.
/// Set a `RUST_LOG=style::errors` environment variable
/// to log CSS parse errors to stderr.
pub fn log_css_error(input: &mut Parser,
                     position: SourcePosition,
                     message: &str,
                     parsercontext: &ParserContext) {
    let servo_url = parsercontext.base_url;
    parsercontext.error_reporter.report_error(input, position, message, servo_url);
}

// XXXManishearth Replace all specified value parse impls with impls of this
// trait. This will make it easy to write more generic values in the future.
/// A trait to abstract parsing of a specified value given a `ParserContext` and
/// CSS input.
pub trait Parse : Sized {
    /// Parse a value of this type.
    ///
    /// Returns an error on failure.
    fn parse(context: &ParserContext, input: &mut Parser) -> Result<Self, ()>;
}

impl<T> Parse for Vec<T> where T: Parse + OneOrMoreCommaSeparated {
    fn parse(context: &ParserContext, input: &mut Parser) -> Result<Self, ()> {
        input.parse_comma_separated(|input| T::parse(context, input))
    }
}

/// Parse a non-empty space-separated or comma-separated list of objects parsed by parse_one
pub fn parse_space_or_comma_separated<F, T>(input: &mut Parser, mut parse_one: F)
        -> Result<Vec<T>, ()>
        where F: FnMut(&mut Parser) -> Result<T, ()> {
    let first = parse_one(input)?;
    let mut vec = vec![first];
    loop {
        let _ = input.try(|i| i.expect_comma());
        if let Ok(val) = input.try(|i| parse_one(i)) {
            vec.push(val)
        } else {
            break
        }
    }
    Ok(vec)
}
impl Parse for UnicodeRange {
    fn parse(_context: &ParserContext, input: &mut Parser) -> Result<Self, ()> {
        UnicodeRange::parse(input)
    }
}
