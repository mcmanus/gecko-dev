"use strict";

const PATH = "/browser/browser/components/sessionstore/test/";

/**
 * Remove all cookies to start off a clean slate.
 */
add_task(function* test_setup() {
  requestLongerTimeout(3);
  Services.cookies.removeAll();
});

/**
 * Test multiple scenarios with different Set-Cookie header domain= params.
 */
add_task(function* test_run() {
  // Set-Cookie: foobar=random()
  // The domain of the cookie should be the request domain (www.example.com).
  // We should collect data only for the request domain, no parent or subdomains.
  yield testCookieCollection({
    host: "http://www.example.com",
    cookieHost: "www.example.com",
    cookieURIs: ["http://www.example.com" + PATH],
    noCookieURIs: ["http://example.com/" + PATH]
  });

  // Set-Cookie: foobar=random()
  // The domain of the cookie should be the request domain (example.com).
  // We should collect data only for the request domain, no parent or subdomains.
  yield testCookieCollection({
    host: "http://example.com",
    cookieHost: "example.com",
    cookieURIs: ["http://example.com" + PATH],
    noCookieURIs: ["http://www.example.com/" + PATH]
  });

  // Set-Cookie: foobar=random(); Domain=example.com
  // The domain of the cookie should be the given one (.example.com).
  // We should collect data for the given domain and its subdomains.
  yield testCookieCollection({
    host: "http://example.com",
    domain: "example.com",
    cookieHost: ".example.com",
    cookieURIs: ["http://example.com" + PATH, "http://www.example.com/" + PATH],
    noCookieURIs: ["about:robots"]
  });

  // Set-Cookie: foobar=random(); Domain=.example.com
  // The domain of the cookie should be the given one (.example.com).
  // We should collect data for the given domain and its subdomains.
  yield testCookieCollection({
    host: "http://example.com",
    domain: ".example.com",
    cookieHost: ".example.com",
    cookieURIs: ["http://example.com" + PATH, "http://www.example.com/" + PATH],
    noCookieURIs: ["about:robots"]
  });

  // Set-Cookie: foobar=random(); Domain=www.example.com
  // The domain of the cookie should be the given one (.www.example.com).
  // We should collect data for the given domain and its subdomains.
  yield testCookieCollection({
    host: "http://www.example.com",
    domain: "www.example.com",
    cookieHost: ".www.example.com",
    cookieURIs: ["http://www.example.com/" + PATH],
    noCookieURIs: ["http://example.com"]
  });

  // Set-Cookie: foobar=random(); Domain=.www.example.com
  // The domain of the cookie should be the given one (.www.example.com).
  // We should collect data for the given domain and its subdomains.
  yield testCookieCollection({
    host: "http://www.example.com",
    domain: ".www.example.com",
    cookieHost: ".www.example.com",
    cookieURIs: ["http://www.example.com/" + PATH],
    noCookieURIs: ["http://example.com"]
  });
});

/**
 * Test multiple scenarios with different privacy levels.
 */
add_task(function* test_run_privacy_level() {
  registerCleanupFunction(() => {
    Services.prefs.clearUserPref("browser.sessionstore.privacy_level");
  });

  // With the default privacy level we collect all cookies.
  yield testCookieCollection({
    host: "http://example.com",
    domain: ".example.com",
    cookieHost: ".example.com",
    cookieURIs: ["https://example.com/" + PATH, "http://example.com/" + PATH],
    noCookieURIs: ["about:robots"]
  });

  // With the default privacy level we collect all cookies.
  yield testCookieCollection({
    host: "https://example.com",
    domain: ".example.com",
    cookieHost: ".example.com",
    cookieURIs: ["https://example.com/" + PATH, "http://example.com/" + PATH],
    noCookieURIs: ["about:robots"]
  });

  // With the default privacy level we collect all cookies.
  yield testCookieCollection({
    isSecure: true,
    host: "https://example.com",
    domain: ".example.com",
    cookieHost: ".example.com",
    cookieURIs: ["https://example.com/" + PATH, "http://example.com/" + PATH],
    noCookieURIs: ["about:robots"]
  });

  // Set level=encrypted, don't store any secure cookies.
  Services.prefs.setIntPref("browser.sessionstore.privacy_level", 1);

  // With level=encrypted, non-secure cookies will be stored.
  yield testCookieCollection({
    host: "http://example.com",
    domain: ".example.com",
    cookieHost: ".example.com",
    cookieURIs: ["https://example.com/" + PATH, "http://example.com/" + PATH],
    noCookieURIs: ["about:robots"]
  });

  // With level=encrypted, non-secure cookies will be stored,
  // even if sent by an HTTPS site.
  yield testCookieCollection({
    host: "https://example.com",
    domain: ".example.com",
    cookieHost: ".example.com",
    cookieURIs: ["https://example.com/" + PATH, "http://example.com/" + PATH],
    noCookieURIs: ["about:robots"]
  });

  // With level=encrypted, secure cookies will NOT be stored.
  yield testCookieCollection({
    isSecure: true,
    host: "https://example.com",
    domain: ".example.com",
    cookieHost: ".example.com",
    noCookieURIs: ["https://example.com/" + PATH, "http://example.com/" + PATH]
  });

  // Set level=full, don't store any cookies.
  Services.prefs.setIntPref("browser.sessionstore.privacy_level", 2);

  // With level=full we must not store any cookies.
  yield testCookieCollection({
    host: "http://example.com",
    domain: ".example.com",
    cookieHost: ".example.com",
    noCookieURIs: ["https://example.com/" + PATH, "http://example.com/" + PATH]
  });

  // With level=full we must not store any cookies.
  yield testCookieCollection({
    host: "https://example.com",
    domain: ".example.com",
    cookieHost: ".example.com",
    noCookieURIs: ["https://example.com/" + PATH, "http://example.com/" + PATH]
  });

  // With level=full we must not store any cookies.
  yield testCookieCollection({
    isSecure: true,
    host: "https://example.com",
    domain: ".example.com",
    cookieHost: ".example.com",
    noCookieURIs: ["https://example.com/" + PATH, "http://example.com/" + PATH]
  });

  Services.prefs.clearUserPref("browser.sessionstore.privacy_level");
});

/**
 * Generic test function to check sessionstore's cookie collection module with
 * different cookie domains given in the Set-Cookie header. See above for some
 * usage examples.
 */
var testCookieCollection = async function(params) {
  let tab = gBrowser.addTab("about:blank");
  let browser = tab.linkedBrowser;

  let urlParams = new URLSearchParams();
  let value = Math.random();
  urlParams.append("value", value);

  if (params.domain) {
    urlParams.append("domain", params.domain);
  }

  if (params.isSecure) {
    urlParams.append("secure", "1");
  }

  // Construct request URI.
  let requestUri = `${params.host}${PATH}browser_cookies.sjs?${urlParams}`;

  // Wait for the browser to load and the cookie to be set.
  // These two events can probably happen in no particular order,
  // so let's wait for them in parallel.
  await Promise.all([
    waitForNewCookie(),
    replaceCurrentURI(browser, requestUri)
  ]);

  // Check all URIs for which the cookie should be collected.
  for (let uri of params.cookieURIs || []) {
    await replaceCurrentURI(browser, uri);

    // Check the cookie.
    let cookie = getCookie();
    is(cookie.host, params.cookieHost, "cookie host is correct");
    is(cookie.path, PATH, "cookie path is correct");
    is(cookie.name, "foobar", "cookie name is correct");
    is(cookie.value, value, "cookie value is correct");
  }

  // Check all URIs for which the cookie should NOT be collected.
  for (let uri of params.noCookieURIs || []) {
    await replaceCurrentURI(browser, uri);

    // Cookie should be ignored.
    ok(!getCookie(), "no cookie collected");
  }

  // Clean up.
  gBrowser.removeTab(tab);
  Services.cookies.removeAll();
};

/**
 * Replace the current URI of the given browser by loading a new URI. The
 * browser's session history will be completely replaced. This function ensures
 * that the parent process has the lastest shistory data before resolving.
 */
var replaceCurrentURI = async function(browser, uri) {
  // Replace the tab's current URI with the parent domain.
  let flags = Ci.nsIWebNavigation.LOAD_FLAGS_REPLACE_HISTORY;
  browser.loadURIWithFlags(uri, flags);
  await promiseBrowserLoaded(browser);

  // Ensure the tab's session history is up-to-date.
  await TabStateFlusher.flush(browser);
};

/**
 * Waits for a new "*example.com" cookie to be added.
 */
function waitForNewCookie() {
  return new Promise(resolve => {
    Services.obs.addObserver(function observer(subj, topic, data) {
      let cookie = subj.QueryInterface(Ci.nsICookie2);
      if (data == "added" && cookie.host.endsWith("example.com")) {
        Services.obs.removeObserver(observer, topic);
        resolve();
      }
    }, "cookie-changed", false);
  });
}

/**
 * Retrieves the first cookie in the first window from the current sessionstore
 * state.
 */
function getCookie() {
  let state = JSON.parse(ss.getWindowState(window));
  let cookies = state.windows[0].cookies || [];
  return cookies[0] || null;
}
