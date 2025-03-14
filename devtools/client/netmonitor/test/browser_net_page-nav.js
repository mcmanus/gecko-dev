/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

/**
 * Tests if page navigation ("close", "navigate", etc.) triggers an appropriate
 * action in the network monitor.
 */

add_task(function* () {
  let { tab, monitor } = yield initNetMonitor(SIMPLE_URL);
  info("Starting test... ");

  let { windowRequire } = monitor.panelWin;
  let { EVENTS } = windowRequire("devtools/client/netmonitor/src/constants");

  yield testNavigate();
  yield testNavigateBack();
  yield testClose();

  function* testNavigate() {
    info("Navigating forward...");

    let onWillNav = monitor.panelWin.once(EVENTS.TARGET_WILL_NAVIGATE);
    let onDidNav = monitor.panelWin.once(EVENTS.TARGET_DID_NAVIGATE);

    tab.linkedBrowser.loadURI(NAVIGATE_URL);
    yield onWillNav;

    is(tab.linkedBrowser.currentURI.spec, SIMPLE_URL,
      "Target started navigating to the correct location.");

    yield onDidNav;
    is(tab.linkedBrowser.currentURI.spec, NAVIGATE_URL,
      "Target finished navigating to the correct location.");
  }

  function* testNavigateBack() {
    info("Navigating backward...");

    let onWillNav = monitor.panelWin.once(EVENTS.TARGET_WILL_NAVIGATE);
    let onDidNav = monitor.panelWin.once(EVENTS.TARGET_DID_NAVIGATE);

    tab.linkedBrowser.loadURI(SIMPLE_URL);
    yield onWillNav;

    is(tab.linkedBrowser.currentURI.spec, NAVIGATE_URL,
      "Target started navigating back to the previous location.");

    yield onDidNav;
    is(tab.linkedBrowser.currentURI.spec, SIMPLE_URL,
      "Target finished navigating back to the previous location.");
  }

  function* testClose() {
    info("Closing...");

    let onDestroyed = monitor.once("destroyed");
    removeTab(tab);
    yield onDestroyed;

    ok(!monitor.panelWin.NetMonitorController.client,
      "There shouldn't be a client available after destruction.");
    ok(!monitor.panelWin.NetMonitorController.tabClient,
      "There shouldn't be a tabClient available after destruction.");
    ok(!monitor.panelWin.NetMonitorController.webConsoleClient,
      "There shouldn't be a webConsoleClient available after destruction.");
  }
});
