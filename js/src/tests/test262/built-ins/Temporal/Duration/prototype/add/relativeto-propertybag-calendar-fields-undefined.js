// |reftest| skip -- Temporal is not supported
// Copyright (C) 2022 Igalia, S.L. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-temporal.duration.prototype.add
description: >
  When calendar.fields is undefined, add() doesn't perform an
  observable array iteration to convert the property bag to relativeTo
features: [Temporal]
---*/

const calendar = new Temporal.Calendar("iso8601");
calendar.fields = undefined;

const instance = new Temporal.Duration(1, 0, 0, 1);

// Detect observable array iteration:
const oldIterator = Array.prototype[Symbol.iterator];
Array.prototype[Symbol.iterator] = function () {
  throw new Test262Error(`array shouldn't be iterated: ${new Error().stack}`);
};

const relativeTo = { year: 1981, month: 12, day: 15, calendar };

instance.add(new Temporal.Duration(0, 0, 0, 0, -24), { relativeTo });

Array.prototype[Symbol.iterator] = oldIterator;

reportCompare(0, 0);
