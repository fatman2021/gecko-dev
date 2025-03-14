/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */

import React from "react";
import { Provider } from "react-redux";
import configureStore from "redux-mock-store";

import { shallow } from "enzyme";
import SearchBar from "../SearchBar";
import "../../../workers/search";
import "../../../utils/editor";

const SearchBarComponent = SearchBar.WrappedComponent;

jest.mock("../../../workers/search", () => ({
  WorkerDispatcher() {
    return {
      getMatches: () => Promise.resolve(["result"]),
      clear() {},
    };
  },
}));

jest.mock("../../../utils/editor", () => ({
  find: () => ({ ch: "1", line: "1" }),
}));

function generateDefaults() {
  return {
    query: "",
    searchOn: true,
    symbolSearchOn: true,
    editor: {},
    searchResults: { count: 0 },
    selectedSymbolType: "functions",
    selectedSource: {
      text: " text text query text",
    },
    selectedContentLoaded: true,
    setFileSearchQuery: msg => msg,
    symbolSearchResults: [],
    modifiers: {
      get: jest.fn(),
      toJS: () => ({
        caseSensitive: true,
        wholeWord: false,
        regexMatch: false,
      }),
    },
    selectedResultIndex: 0,
    updateSearchResults: jest.fn(),
    doSearch: jest.fn(),
    closeFileSearch: jest.fn(),
    cx: {},
    setActiveSearch: jest.fn(),
    toggleFileSearchModifier: jest.fn(),
    traverseResults: jest.fn(),
  };
}

function render(overrides = {}) {
  const defaults = generateDefaults();
  const mockStore = configureStore([]);
  const store = mockStore({
    ui: { mutableSearchOptions: { "foo-search": {} } },
  });
  const props = { ...defaults, ...overrides };
  const component = shallow(
    <Provider store={store}>
      <SearchBarComponent {...props} />
    </Provider>,
    {
      disableLifecycleMethods: true,
    }
  ).dive();
  return { component, props };
}

describe("SearchBar", () => {
  it("should render", () => {
    const { component } = render();
    expect(component).toMatchSnapshot();
  });
});

describe("doSearch", () => {
  it("should complete a search", async () => {
    const { component, props } = render();
    component
      .find("Connect(SearchInput)")
      .simulate("change", { target: { value: "query" } });

    const doSearchArgs = props.doSearch.mock.calls[0][1];
    expect(doSearchArgs).toMatchSnapshot();
  });
});

describe("showErrorEmoji", () => {
  it("true if query + no results", () => {
    const { component } = render({
      query: "test",
      searchResults: {
        count: 0,
      },
    });
    expect(component).toMatchSnapshot();
  });

  it("false if no query + no results", () => {
    const { component } = render({
      query: "",
      searchResults: {
        count: 0,
      },
    });
    expect(component).toMatchSnapshot();
  });

  it("false if query + results", () => {
    const { component } = render({
      query: "test",
      searchResults: {
        count: 10,
      },
    });
    expect(component).toMatchSnapshot();
  });
});
