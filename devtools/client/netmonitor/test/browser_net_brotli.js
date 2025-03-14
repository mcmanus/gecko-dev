/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

const BROTLI_URL = HTTPS_EXAMPLE_URL + "html_brotli-test-page.html";
const BROTLI_REQUESTS = 1;

/**
 * Test brotli encoded response is handled correctly on HTTPS.
 */

add_task(function* () {
  let { L10N } = require("devtools/client/netmonitor/src/utils/l10n");

  let { tab, monitor } = yield initNetMonitor(BROTLI_URL);
  info("Starting test... ");

  let { document, gStore, windowRequire } = monitor.panelWin;
  let Actions = windowRequire("devtools/client/netmonitor/src/actions/index");
  let {
    getDisplayedRequests,
    getSortedRequests,
  } = windowRequire("devtools/client/netmonitor/src/selectors/index");

  gStore.dispatch(Actions.batchEnable(false));

  let wait = waitForNetworkEvents(monitor, BROTLI_REQUESTS);
  yield ContentTask.spawn(tab.linkedBrowser, {}, function* () {
    content.wrappedJSObject.performRequests();
  });
  yield wait;

  verifyRequestItemTarget(
    document,
    getDisplayedRequests(gStore.getState()),
    getSortedRequests(gStore.getState()).get(0),
    "GET", HTTPS_CONTENT_TYPE_SJS + "?fmt=br", {
      status: 200,
      statusText: "Connected",
      type: "plain",
      fullMimeType: "text/plain",
      transferred: L10N.getFormatStrWithNumbers("networkMenu.sizeB", 10),
      size: L10N.getFormatStrWithNumbers("networkMenu.sizeB", 64),
      time: true
    });

  wait = waitForDOM(document, "#response-panel .editor-mount iframe");
  EventUtils.sendMouseEvent({ type: "click" },
    document.querySelector(".network-details-panel-toggle"));
  EventUtils.sendMouseEvent({ type: "click" },
    document.querySelector("#response-tab"));
  let [editorFrame] = yield wait;

  yield once(editorFrame, "DOMContentLoaded");
  yield waitForDOM(editorFrame.contentDocument, ".CodeMirror-code");
  yield testResponse("br");

  yield teardown(monitor);

  function* testResponse(type) {
    switch (type) {
      case "br": {
        let text = editorFrame.contentDocument
          .querySelector(".CodeMirror-line").textContent;

        is(text, "X".repeat(64),
          "The text shown in the source editor is incorrect for the brotli request.");
        break;
      }
    }
  }
});
