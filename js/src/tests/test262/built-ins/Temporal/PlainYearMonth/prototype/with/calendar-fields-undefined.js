// |reftest| skip -- Temporal is not supported
// Copyright (C) 2022 Igalia, S.L. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-temporal.plainyearmonth.prototype.with
description: >
  When calendar.fields is undefined, with() doesn't perform an
  observable array iteration
features: [Temporal]
---*/

const calendar = new Temporal.Calendar("iso8601");
calendar.fields = undefined;

const instance = new Temporal.PlainYearMonth(1981, 12, calendar);

// Detect observable array iteration:
const oldIterator = Array.prototype[Symbol.iterator];
Array.prototype[Symbol.iterator] = function () {
  throw new Test262Error(`array shouldn't be iterated: ${new Error().stack}`);
};

instance.with({ year: 2022 });

Array.prototype[Symbol.iterator] = oldIterator;

reportCompare(0, 0);
