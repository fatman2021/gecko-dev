/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */

import PropTypes from "prop-types";
import React, { Component } from "react";
import { connect } from "../../utils/connect";
import actions from "../../actions";
import {
  getActiveSearch,
  getSelectedSource,
  getSelectedLocation,
  getSettledSourceTextContent,
  getFileSearchQuery,
  getFileSearchResults,
  getContext,
} from "../../selectors";

import { removeOverlay } from "../../utils/editor";

import { scrollList } from "../../utils/result-list";

import SearchInput from "../shared/SearchInput";
import "./SearchBar.css";

const { PluralForm } = require("devtools/shared/plural-form");
const { debounce } = require("devtools/shared/debounce");

function getSearchShortcut() {
  return L10N.getStr("sourceSearch.search.key2");
}

class SearchBar extends Component {
  constructor(props) {
    super(props);
    this.state = {
      query: props.query,
      selectedResultIndex: 0,
      count: 0,
      index: -1,
      inputFocused: false,
    };
  }

  static get propTypes() {
    return {
      closeFileSearch: PropTypes.func.isRequired,
      cx: PropTypes.object.isRequired,
      doSearch: PropTypes.func.isRequired,
      editor: PropTypes.object,
      modifiers: PropTypes.object.isRequired,
      query: PropTypes.string.isRequired,
      searchOn: PropTypes.bool.isRequired,
      searchResults: PropTypes.object.isRequired,
      selectedContentLoaded: PropTypes.bool.isRequired,
      selectedSource: PropTypes.object.isRequired,
      setActiveSearch: PropTypes.func.isRequired,
      traverseResults: PropTypes.func.isRequired,
    };
  }

  componentWillUnmount() {
    const { shortcuts } = this.context;

    shortcuts.off(getSearchShortcut(), this.toggleSearch);
    shortcuts.off("Escape", this.onEscape);

    this.doSearch.cancel();
  }

  componentDidMount() {
    // overwrite this.doSearch with debounced version to
    // reduce frequency of queries
    this.doSearch = debounce(this.doSearch, 100);
    const { shortcuts } = this.context;

    shortcuts.on(getSearchShortcut(), this.toggleSearch);
    shortcuts.on("Escape", this.onEscape);
  }

  componentDidUpdate(prevProps, prevState) {
    if (this.refs.resultList && this.refs.resultList.refs) {
      scrollList(this.refs.resultList.refs, this.state.selectedResultIndex);
    }
  }

  onEscape = e => {
    this.closeSearch(e);
  };

  clearSearch = () => {
    const { editor: ed, query } = this.props;
    if (ed) {
      const ctx = { ed, cm: ed.codeMirror };
      removeOverlay(ctx, query);
    }
  };

  closeSearch = e => {
    const { cx, closeFileSearch, editor, searchOn, query } = this.props;
    this.clearSearch();
    if (editor && searchOn) {
      closeFileSearch(cx, editor);
      e.stopPropagation();
      e.preventDefault();
    }
    this.setState({ query, inputFocused: false });
  };

  toggleSearch = e => {
    e.stopPropagation();
    e.preventDefault();
    const { editor, searchOn, setActiveSearch } = this.props;

    // Set inputFocused to false, so that search query is highlighted whenever search shortcut is used, even if the input already has focus.
    this.setState({ inputFocused: false });

    if (!searchOn) {
      setActiveSearch("file");
    }

    if (this.props.searchOn && editor) {
      const query = editor.codeMirror.getSelection() || this.state.query;

      if (query !== "") {
        this.setState({ query, inputFocused: true });
        this.doSearch(query);
      } else {
        this.setState({ query: "", inputFocused: true });
      }
    }
  };

  doSearch = query => {
    const { cx, selectedSource, selectedContentLoaded } = this.props;
    if (!selectedSource || !selectedContentLoaded) {
      return;
    }

    this.props.doSearch(cx, query, this.props.editor);
  };

  traverseResults = (e, rev) => {
    e.stopPropagation();
    e.preventDefault();
    const { editor } = this.props;

    if (!editor) {
      return;
    }
    this.props.traverseResults(this.props.cx, rev, editor);
  };

  // Handlers

  onChange = e => {
    this.setState({ query: e.target.value });

    return this.doSearch(e.target.value);
  };

  onFocus = e => {
    this.setState({ inputFocused: true });
  };

  onBlur = e => {
    this.setState({ inputFocused: false });
  };

  onKeyDown = e => {
    if (e.key !== "Enter" && e.key !== "F3") {
      return;
    }

    this.traverseResults(e, e.shiftKey);
    e.preventDefault();
    this.doSearch(e.target.value);
  };

  onHistoryScroll = query => {
    this.setState({ query });
    this.doSearch(query);
  };

  // Renderers
  buildSummaryMsg() {
    const {
      searchResults: { matchIndex, count, index },
      query,
    } = this.props;

    if (query.trim() == "") {
      return "";
    }

    if (count == 0) {
      return L10N.getStr("editor.noResultsFound");
    }

    if (index == -1) {
      const resultsSummaryString = L10N.getStr("sourceSearch.resultsSummary1");
      return PluralForm.get(count, resultsSummaryString).replace("#1", count);
    }

    const searchResultsString = L10N.getStr("editor.searchResults1");
    return PluralForm.get(count, searchResultsString)
      .replace("#1", count)
      .replace("%d", matchIndex + 1);
  }

  shouldShowErrorEmoji() {
    const {
      query,
      searchResults: { count },
    } = this.props;
    return !!query && !count;
  }

  render() {
    const {
      searchResults: { count },
      searchOn,
    } = this.props;

    if (!searchOn) {
      return <div />;
    }

    return (
      <div className="search-bar">
        <SearchInput
          query={this.state.query}
          count={count}
          placeholder={L10N.getStr("sourceSearch.search.placeholder2")}
          summaryMsg={this.buildSummaryMsg()}
          isLoading={false}
          onChange={this.onChange}
          onFocus={this.onFocus}
          onBlur={this.onBlur}
          showErrorEmoji={this.shouldShowErrorEmoji()}
          onKeyDown={this.onKeyDown}
          onHistoryScroll={this.onHistoryScroll}
          handleNext={e => this.traverseResults(e, false)}
          handlePrev={e => this.traverseResults(e, true)}
          shouldFocus={this.state.inputFocused}
          showClose={true}
          handleClose={this.closeSearch}
          showSearchModifiers={true}
          searchKey="file-search"
          onToggleSearchModifier={() => this.doSearch(this.state.query)}
        />
      </div>
    );
  }
}

SearchBar.contextTypes = {
  shortcuts: PropTypes.object,
};

const mapStateToProps = (state, p) => {
  const selectedSource = getSelectedSource(state);
  const selectedLocation = getSelectedLocation(state);

  return {
    cx: getContext(state),
    searchOn: getActiveSearch(state) === "file",
    selectedSource,
    selectedContentLoaded: selectedLocation
      ? !!getSettledSourceTextContent(state, selectedLocation)
      : false,
    query: getFileSearchQuery(state),
    searchResults: getFileSearchResults(state),
  };
};

export default connect(mapStateToProps, {
  setFileSearchQuery: actions.setFileSearchQuery,
  setActiveSearch: actions.setActiveSearch,
  closeFileSearch: actions.closeFileSearch,
  doSearch: actions.doSearch,
  traverseResults: actions.traverseResults,
})(SearchBar);
