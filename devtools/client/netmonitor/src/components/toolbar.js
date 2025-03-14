/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const {
  createClass,
  createFactory,
  DOM,
  PropTypes,
} = require("devtools/client/shared/vendor/react");
const { connect } = require("devtools/client/shared/vendor/react-redux");
const { PluralForm } = require("devtools/shared/plural-form");
const Actions = require("../actions/index");
const { FILTER_SEARCH_DELAY } = require("../constants");
const {
  getDisplayedRequestsSummary,
  getRequestFilterTypes,
  isNetworkDetailsToggleButtonDisabled,
} = require("../selectors/index");
const {
  getFormattedSize,
  getFormattedTime
} = require("../utils/format-utils");
const { L10N } = require("../utils/l10n");

// Components
const SearchBox = createFactory(require("devtools/client/shared/components/search-box"));

const { button, div, span } = DOM;

const COLLPASE_DETAILS_PANE = L10N.getStr("collapseDetailsPane");
const EXPAND_DETAILS_PANE = L10N.getStr("expandDetailsPane");
const SEARCH_KEY_SHORTCUT = L10N.getStr("netmonitor.toolbar.filterFreetext.key");
const SEARCH_PLACE_HOLDER = L10N.getStr("netmonitor.toolbar.filterFreetext.label");
const TOOLBAR_CLEAR = L10N.getStr("netmonitor.toolbar.clear");

/*
 * Network monitor toolbar component
 * Toolbar contains a set of useful tools to control network requests
 */
const Toolbar = createClass({
  displayName: "Toolbar",

  propTypes: {
    clearRequests: PropTypes.func.isRequired,
    openStatistics: PropTypes.func.isRequired,
    requestFilterTypes: PropTypes.array.isRequired,
    setRequestFilterText: PropTypes.func.isRequired,
    networkDetailsToggleDisabled: PropTypes.bool.isRequired,
    networkDetailsOpen: PropTypes.bool.isRequired,
    summary: PropTypes.object.isRequired,
    toggleNetworkDetails: PropTypes.func.isRequired,
    toggleRequestFilterType: PropTypes.func.isRequired,
  },

  toggleRequestFilterType(evt) {
    if (evt.type === "keydown" && (evt.key !== "" || evt.key !== "Enter")) {
      return;
    }
    this.props.toggleRequestFilterType(evt.target.dataset.key);
  },

  render() {
    let {
      clearRequests,
      openStatistics,
      requestFilterTypes,
      setRequestFilterText,
      networkDetailsToggleDisabled,
      networkDetailsOpen,
      summary,
      toggleNetworkDetails,
    } = this.props;

    let toggleButtonClassName = [
      "network-details-panel-toggle",
      "devtools-button",
    ];
    if (!networkDetailsOpen) {
      toggleButtonClassName.push("pane-collapsed");
    }

    let { count, contentSize, transferredSize, millis } = summary;
    let text = (count === 0) ? L10N.getStr("networkMenu.empty") :
      PluralForm.get(count, L10N.getStr("networkMenu.summary3"))
      .replace("#1", count)
      .replace("#2", getFormattedSize(contentSize))
      .replace("#3", getFormattedSize(transferredSize))
      .replace("#4", getFormattedTime(millis));

    let buttons = requestFilterTypes.map(([type, checked]) => {
      let classList = ["devtools-button", `requests-list-filter-${type}-button`];
      checked && classList.push("checked");

      return (
        button({
          className: classList.join(" "),
          key: type,
          onClick: this.toggleRequestFilterType,
          onKeyDown: this.toggleRequestFilterType,
          "aria-pressed": checked,
          "data-key": type,
        },
          L10N.getStr(`netmonitor.toolbar.filter.${type}`)
        )
      );
    });

    return (
      span({ className: "devtools-toolbar devtools-toolbar-container" },
        span({ className: "devtools-toolbar-group" },
          button({
            className: "devtools-button devtools-clear-icon requests-list-clear-button",
            title: TOOLBAR_CLEAR,
            onClick: clearRequests,
          }),
          div({ className: "requests-list-filter-buttons" }, buttons),
        ),
        span({ className: "devtools-toolbar-group" },
          button({
            className: "devtools-button requests-list-network-summary-button",
            title: count ? text : L10N.getStr("netmonitor.toolbar.perf"),
            onClick: openStatistics,
          },
            span({ className: "summary-info-icon" }),
            span({ className: "summary-info-text" }, text),
          ),
          SearchBox({
            delay: FILTER_SEARCH_DELAY,
            keyShortcut: SEARCH_KEY_SHORTCUT,
            placeholder: SEARCH_PLACE_HOLDER,
            type: "filter",
            onChange: setRequestFilterText,
          }),
          button({
            className: toggleButtonClassName.join(" "),
            title: networkDetailsOpen ? COLLPASE_DETAILS_PANE : EXPAND_DETAILS_PANE,
            disabled: networkDetailsToggleDisabled,
            tabIndex: "0",
            onClick: toggleNetworkDetails,
          }),
        )
      )
    );
  }
});

module.exports = connect(
  (state) => ({
    networkDetailsToggleDisabled: isNetworkDetailsToggleButtonDisabled(state),
    networkDetailsOpen: state.ui.networkDetailsOpen,
    requestFilterTypes: getRequestFilterTypes(state),
    summary: getDisplayedRequestsSummary(state),
  }),
  (dispatch) => ({
    clearRequests: () => dispatch(Actions.clearRequests()),
    openStatistics: () => dispatch(Actions.openStatistics(true)),
    setRequestFilterText: (text) => dispatch(Actions.setRequestFilterText(text)),
    toggleRequestFilterType: (type) => dispatch(Actions.toggleRequestFilterType(type)),
    toggleNetworkDetails: () => dispatch(Actions.toggleNetworkDetails()),
  }),
)(Toolbar);
