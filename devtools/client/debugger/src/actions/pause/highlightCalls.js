/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */

import {
  getSymbols,
  getSelectedFrame,
  getCurrentThread,
} from "../../selectors";

// a is an ast location with start and end positions (line and column).
// b is a single position (line and column).
// This function tests to see if the b position
// falls within the range given in a.
function inHouseContainsPosition(a, b) {
  const bColumn = b.column || 0;
  const startsBefore =
    a.start.line < b.line ||
    (a.start.line === b.line && a.start.column <= bColumn);
  const endsAfter =
    a.end.line > b.line || (a.end.line === b.line && a.end.column >= bColumn);

  return startsBefore && endsAfter;
}

export function highlightCalls(cx) {
  return async function({ dispatch, getState, parserWorker }) {
    if (!cx) {
      return null;
    }

    const frame = await getSelectedFrame(
      getState(),
      getCurrentThread(getState())
    );

    if (!frame) {
      return null;
    }

    const { thread } = cx;

    const originalAstScopes = await parserWorker.getScopes(frame.location);
    if (!originalAstScopes) {
      return null;
    }

    if (!frame.location.source) {
      return null;
    }

    const symbols = getSymbols(getState(), frame.location.source);

    if (!symbols) {
      return null;
    }

    if (!symbols.callExpressions) {
      return null;
    }

    const localAstScope = originalAstScopes[0];
    const allFunctionCalls = symbols.callExpressions;

    const highlightedCalls = allFunctionCalls.filter(function(call) {
      const containsStart = inHouseContainsPosition(
        localAstScope,
        call.location.start
      );
      const containsEnd = inHouseContainsPosition(
        localAstScope,
        call.location.end
      );
      return containsStart && containsEnd;
    });

    return dispatch({
      type: "HIGHLIGHT_CALLS",
      thread,
      highlightedCalls,
    });
  };
}

export function unhighlightCalls(cx) {
  return async function({ dispatch, getState }) {
    const { thread } = cx;
    return dispatch({
      type: "UNHIGHLIGHT_CALLS",
      thread,
    });
  };
}
